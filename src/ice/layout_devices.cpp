#include "layout_devices.hpp"

#include "icelib/operations.hpp"
#include "package_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPaintEvent>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>
#include <utility>

namespace {
// Defined below with the device-layer helpers. Declared here so PDK layer
// activation can apply the same purpose/marker visibility rules.
bool layerHiddenByDefault(const std::string &name, int datatype);

void addLayoutDeviceLibraryLayers(ice::Project &project, const std::string &pdk) {
    const auto path = layoutDeviceLibraryPath(pdk);
    if (path.empty())
        return;
    try {
        const auto library = ice::LayoutDeviceLibrary::load(path);
        for (const auto &layer : library.layers) {
            const auto exists = std::find_if(project.layers.begin(), project.layers.end(), [&](const auto &entry) {
                const auto &existing = entry.second;
                return existing.number == layer.number && existing.datatype == layer.datatype &&
                       existing.name == layer.name && existing.layout == 0 &&
                       (existing.pdk.empty() || existing.pdk == pdk);
            });
            if (exists == project.layers.end()) {
                const auto id = ice::addLayer(project, layer.name, layer.number, 0, pdk, layer.datatype);
                if (layerHiddenByDefault(layer.name, layer.datatype))
                    project.layers.at(id).visible = false;
            } else if (exists->second.pdk.empty()) {
                exists->second.pdk = pdk;
            }
        }
    } catch (const ice::ProjectError &) {
        // A package without a loadable device library can still contribute its
        // declared PDK layers.
    }
}
} // namespace

void addPdkLayers(ice::Project &project, const ice::PackagePdk &pdk, const std::string &layoutPdk) {
    const auto technology = layoutPdk.empty() ? pdk.id : layoutPdk;
    for (const auto &layer : pdk.layers) {
        const auto exists = std::find_if(project.layers.begin(), project.layers.end(), [&](const auto &entry) {
            const auto &existing = entry.second;
            return existing.name == layer.name && existing.layout == 0 &&
                   (existing.pdk.empty() || existing.pdk == technology);
        });
        if (exists == project.layers.end())
            ice::addLayer(project, layer.name, layer.number, 0, technology);
    }
    // The device library is the complete layer map for the technology. Adding
    // it here means creating a PDK layout exposes every layer up front and
    // placing devices only reuses existing entries.
    addLayoutDeviceLibraryLayers(project, technology);
}

void ensureLayoutPdkLayers(ice::Project &project, const std::filesystem::path &installDirectory) {
    std::vector<std::string> pdkNames;
    for (const auto &[_, cell] : project.layoutCells) {
        if (cell.pdk.empty() || cell.pdk == ice::genericPdkId)
            continue;
        if (std::find(pdkNames.begin(), pdkNames.end(), cell.pdk) == pdkNames.end())
            pdkNames.push_back(cell.pdk);
    }
    if (pdkNames.empty())
        return;
    const auto packages = installedPackages(installDirectory);
    for (const auto &pdkName : pdkNames) {
        const auto package = std::find_if(packages.begin(), packages.end(), [&](const auto &entry) {
            return ice::packageMatchesPdk(entry.spec, pdkName);
        });
        if (package == packages.end() || !package->spec.pdk)
            continue;
        addPdkLayers(project, *package->spec.pdk, pdkName);
    }
}

namespace {

std::filesystem::path pdkPackageRoot(const std::string &pdk) {
    if (pdk.empty() || pdk == ice::genericPdkId)
        return {};
    const auto packages = installedPackages(configuredPackageInstallDirectory());
    for (const auto &package : packages) {
        if (ice::packageMatchesPdk(package.spec, pdk))
            return package.spec.root;
    }
    return {};
}

bool regularFile(const std::filesystem::path &path) {
    std::error_code status;
    return std::filesystem::is_regular_file(path, status) && !status;
}

} // namespace

QString icepyPythonPath() {
    QStringList paths;
    const auto appDir = QCoreApplication::applicationDirPath();
    paths << appDir;
    paths << appDir + QStringLiteral("/python");
    // Installed layout: <prefix>/bin/ice, <prefix>/lib/icepy.so and
    // <prefix>/share/ice/python/icepy_pcells.py.
    paths << appDir + QStringLiteral("/../lib");
    paths << appDir + QStringLiteral("/../share/ice/python");
#ifdef ICE_SOURCE_DIR
    paths << QString::fromStdString((std::filesystem::path(ICE_SOURCE_DIR) / "python").string());
    paths << QString::fromStdString((std::filesystem::path(ICE_SOURCE_DIR) / "build").string());
#endif
    const auto inherited = qEnvironmentVariable("PYTHONPATH");
    if (!inherited.isEmpty())
        paths << inherited;
    return paths.join(QDir::listSeparator());
}

std::filesystem::path layoutDeviceLibraryPath(const std::string &pdk) {
    // Installed PDK packages own their device libraries. Keeping the library
    // inside the package lets the package version pin the PCell definitions,
    // defaults, and geometry together with the simulation models.
    if (!pdk.empty() && pdk != ice::genericPdkId) {
        const auto packageRoot = pdkPackageRoot(pdk);
        if (!packageRoot.empty()) {
            const std::vector<std::filesystem::path> packageCandidates = {
                packageRoot / "layout" / "devices.json",
                packageRoot / "layout" / "library.json",
                packageRoot / "layout" / (pdk + ".json"),
            };
            for (const auto &candidate : packageCandidates) {
                if (regularFile(candidate))
                    return candidate;
            }
        }
    }

    // Source-tree and system fallbacks keep the bundled IHP library available
    // for old projects and for running ICE without an installed package.
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "assets" / "layout" / "ihp.sg13g2.json",
        std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "layout" / "ihp.sg13g2.json",
        std::filesystem::path(appDir) / ".." / "share" / "ice" / "assets" / "layout" / "ihp.sg13g2.json",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "ihp.sg13g2.json",
#endif
        std::filesystem::path("assets") / "layout" / "ihp.sg13g2.json",
        std::filesystem::path("/usr/share/ice/assets/layout/ihp.sg13g2.json"),
    };
    for (const auto &candidate : candidates) {
        if (regularFile(candidate))
            return candidate;
    }
    return {};
}

std::string layoutDeviceCellPrefix(const ice::LayoutDeviceLibrary &library) {
    if (!library.cellPrefix.empty())
        return library.cellPrefix;
    // The original bundled IHP library predates the cell_prefix field.
    if (library.pdk == "ihp.sg13g2" || library.pdk == "ihp-sg13g2")
        return "IHP ";
    return {};
}

std::string stripLayoutDevicePrefix(const ice::LayoutDeviceLibrary &library, const std::string &cellName) {
    const auto prefix = layoutDeviceCellPrefix(library);
    if (!prefix.empty() && cellName.starts_with(prefix))
        return cellName.substr(prefix.size());
    return cellName;
}

bool layoutPdkIdsMatch(const std::string &left, const std::string &right) {
    const auto normalize = [](std::string value) {
        for (auto &character : value)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        std::replace(value.begin(), value.end(), '.', '-');
        std::replace(value.begin(), value.end(), '_', '-');
        return value;
    };
    return normalize(left) == normalize(right);
}

std::map<std::string, std::string> parameterValues(const std::vector<ice::LayoutDeviceParameter> &parameters) {
    std::map<std::string, std::string> values;
    for (const auto &parameter : parameters)
        values[parameter.name] = parameter.value;
    return values;
}

void syncLayoutDeviceParameters(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceCell &device) {
    std::map<std::string, ice::Parameter> existing;
    for (const auto &parameter : cell.parameters)
        existing[parameter.name] = parameter;
    std::vector<ice::Parameter> updated;
    updated.reserve(device.parameters.size());
    auto nextId = project.nextId();
    for (const auto &parameter : device.parameters) {
        const auto current = existing.find(parameter.name);
        if (current != existing.end() && current->second.id != 0)
            updated.push_back(current->second);
        else
            updated.push_back({nextId++, parameter.name, parameter.value});
    }
    cell.parameters = std::move(updated);
}

const ice::LayoutDeviceCell *findLayoutDevice(const ice::LayoutDeviceLibrary &library, const std::string &cellName) {
    const auto name = stripLayoutDevicePrefix(library, cellName);
    const auto it = std::find_if(library.cells.begin(), library.cells.end(),
                                 [&](const auto &candidate) { return candidate.name == name; });
    return it == library.cells.end() ? nullptr : &*it;
}

const ice::LayoutDeviceCell *findLayoutDeviceForCell(const ice::LayoutDeviceLibrary &library,
                                                     const ice::LayoutCell &cell) {
    if (!cell.device.empty()) {
        const auto it = std::find_if(library.cells.begin(), library.cells.end(),
                                     [&](const auto &candidate) { return candidate.name == cell.device; });
        if (it != library.cells.end())
            return &*it;
    }
    return findLayoutDeviceForCellName(library, cell.name);
}

const ice::LayoutDeviceCell *findLayoutDeviceForCellName(const ice::LayoutDeviceLibrary &library,
                                                         const std::string &cellName) {
    const auto name = stripLayoutDevicePrefix(library, cellName);
    const auto exact = std::find_if(library.cells.begin(), library.cells.end(),
                                    [&](const auto &candidate) { return candidate.name == name; });
    if (exact != library.cells.end())
        return &*exact;
    const ice::LayoutDeviceCell *best = nullptr;
    std::size_t bestLength = 0;
    for (const auto &candidate : library.cells) {
        if (candidate.name.size() <= bestLength || !name.starts_with(candidate.name + "_"))
            continue;
        best = &candidate;
        bestLength = candidate.name.size();
    }
    return best;
}

std::string uniqueDeviceCellName(const ice::Project &project, const std::string &deviceName,
                                 const std::string &prefix) {
    const auto base = prefix + deviceName;
    for (int index = 1;; ++index) {
        const auto candidate = base + "_" + std::to_string(index);
        const bool exists = std::any_of(project.layoutCells.begin(), project.layoutCells.end(),
                                        [&](const auto &entry) { return entry.second.name == candidate; });
        if (!exists)
            return candidate;
    }
}

ice::ObjectId cloneLibraryDeviceCell(ice::Project &project, const ice::LayoutCell &source, const std::string &name) {
    ice::LayoutCell clone = source;
    auto nextId = project.nextId();
    clone.id = nextId++;
    clone.name = name;
    clone.parameters.clear();
    for (const auto &parameter : source.parameters)
        clone.parameters.push_back({nextId++, parameter.name, parameter.value});
    clone.shapes.clear();
    for (const auto &[_, shape] : source.shapes) {
        const auto id = nextId++;
        auto cloned = shape;
        cloned.id = id;
        clone.shapes.emplace(id, std::move(cloned));
    }
    clone.ports.clear();
    for (const auto &[_, port] : source.ports) {
        const auto id = nextId++;
        auto clonedPort = port;
        clonedPort.id = id;
        clone.ports.emplace(id, std::move(clonedPort));
    }
    clone.instances.clear();
    for (const auto &[_, instance] : source.instances) {
        const auto id = nextId++;
        clone.instances.emplace(
            id, ice::LayoutInstance{id, instance.cell, instance.name, instance.x, instance.y, instance.orientation});
    }
    const auto cloneId = clone.id;
    project.layoutCells.emplace(cloneId, std::move(clone));
    return cloneId;
}

QString pythonExecutable() {
    auto configured = qEnvironmentVariable("ICE_PYTHON");
    if (!configured.isEmpty())
        return configured;
    for (const auto &candidate : {QStringLiteral("python3"), QStringLiteral("python")}) {
        const auto path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty())
            return path;
    }
    return {};
}

namespace {
bool layerHiddenByDefault(const std::string &name, int datatype) {
    // Non-zero datatypes are purposes such as pin, nofill, noqrc, or mask.
    // They are part of the device geometry but should not clutter the canvas
    // unless the user explicitly enables them.
    if (datatype != 0)
        return true;
    const auto dot = name.find('.');
    const auto base = name.substr(0, dot);
    static const std::set<std::string> markers = {
        "HeatTrans", "HeatRes", "EdgeSeal", "EXTBlock", "NoRCX", "Recog",
    };
    return markers.contains(base);
}

ice::ObjectId layoutDeviceLayerId(ice::Project &project, std::map<std::pair<int, int>, std::string> &layerNames,
                                  std::map<std::pair<int, int>, ice::ObjectId> &layerIds, int number, int datatype) {
    const auto key = std::make_pair(number, datatype);
    if (const auto existing = layerIds.find(key); existing != layerIds.end())
        return existing->second;
    const auto nameIt = layerNames.find(key);
    std::string layerName;
    if (nameIt != layerNames.end() && !nameIt->second.empty())
        layerName = nameIt->second;
    else if (datatype == 0)
        layerName = "Layer " + std::to_string(number);
    else
        layerName = "Layer " + std::to_string(number) + " (dt " + std::to_string(datatype) + ")";
    ice::ObjectId layerId = 0;
    for (const auto &[id, layer] : project.layers) {
        if (layer.number == number && layer.datatype == datatype && layer.name == layerName) {
            layerId = id;
            break;
        }
    }
    if (layerId == 0) {
        layerId = ice::addLayer(project, layerName, number, 0, {}, datatype);
        if (layerHiddenByDefault(layerName, datatype)) {
            if (const auto layer = project.layers.find(layerId); layer != project.layers.end())
                layer->second.visible = false;
        }
    }
    layerIds.emplace(key, layerId);
    return layerId;
}
} // namespace

void applyLayoutDeviceShapes(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                             const std::vector<ice::LayoutDeviceShape> &sourceShapes,
                             const std::vector<ice::LayoutDevicePort> &sourcePorts) {
    std::map<std::string, ice::Port> existingPorts;
    for (const auto &[_, port] : cell.ports)
        existingPorts.emplace(port.name, port);
    cell.shapes.clear();
    cell.ports.clear();
    cell.instances.clear();
    std::map<std::pair<int, int>, std::string> layerNames;
    for (const auto &layer : library.layers)
        layerNames[{layer.number, layer.datatype}] = layer.name;
    std::map<std::pair<int, int>, ice::ObjectId> layerIds;
    const auto layerIdFor = [&](int number, int datatype) {
        return layoutDeviceLayerId(project, layerNames, layerIds, number, datatype);
    };
    for (const auto &sourceShape : sourceShapes) {
        if (sourceShape.polygon.size() < 3)
            continue;
        const auto layerId = layerIdFor(sourceShape.layer, sourceShape.datatype);
        ice::Coordinate minX = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate minY = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate maxX = std::numeric_limits<ice::Coordinate>::lowest();
        ice::Coordinate maxY = std::numeric_limits<ice::Coordinate>::lowest();
        for (const auto &point : sourceShape.polygon) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        std::vector<ice::LayoutPoint> polygon;
        polygon.reserve(sourceShape.polygon.size());
        for (const auto &point : sourceShape.polygon)
            polygon.push_back({point.x, point.y});
        const auto shapeId = project.nextId();
        cell.shapes.emplace(shapeId, ice::Shape{shapeId, layerId, std::nullopt,
                                                ice::Rect{minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
    }
    for (const auto &sourcePort : sourcePorts) {
        const auto existing = existingPorts.find(sourcePort.name);
        const auto id = existing == existingPorts.end() ? project.nextId() : existing->second.id;
        ice::Port port{id, sourcePort.name, layerIdFor(sourcePort.layer, sourcePort.datatype), sourcePort.rectangle};
        port.direction = sourcePort.direction;
        port.side = sourcePort.side;
        port.label = sourcePort.label.empty() ? sourcePort.name : sourcePort.label;
        if (existing != existingPorts.end()) {
            port.net = existing->second.net;
            port.visible = existing->second.visible;
            port.locked = existing->second.locked;
            port.sourceInstance = existing->second.sourceInstance;
            port.sourcePort = existing->second.sourcePort;
            existingPorts.erase(existing);
        }
        cell.ports.emplace(id, std::move(port));
    }
}

void applyLayoutDevicePorts(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                            const std::vector<ice::LayoutDevicePort> &sourcePorts) {
    if (sourcePorts.empty())
        return;
    std::map<std::string, ice::Port> existingPorts;
    for (const auto &[_, port] : cell.ports)
        existingPorts.emplace(port.name, port);
    cell.ports.clear();
    std::map<std::pair<int, int>, std::string> layerNames;
    for (const auto &layer : library.layers)
        layerNames[{layer.number, layer.datatype}] = layer.name;
    std::map<std::pair<int, int>, ice::ObjectId> layerIds;
    for (const auto &sourcePort : sourcePorts) {
        const auto existing = existingPorts.find(sourcePort.name);
        const auto id = existing == existingPorts.end() ? project.nextId() : existing->second.id;
        ice::Port port{id, sourcePort.name,
                       layoutDeviceLayerId(project, layerNames, layerIds, sourcePort.layer, sourcePort.datatype),
                       sourcePort.rectangle};
        port.direction = sourcePort.direction;
        port.side = sourcePort.side;
        port.label = sourcePort.label.empty() ? sourcePort.name : sourcePort.label;
        if (existing != existingPorts.end()) {
            port.net = existing->second.net;
            port.visible = existing->second.visible;
            port.locked = existing->second.locked;
            port.sourceInstance = existing->second.sourceInstance;
            port.sourcePort = existing->second.sourcePort;
            existingPorts.erase(existing);
        }
        cell.ports.emplace(id, std::move(port));
    }
}

std::optional<ice::LayoutDeviceGeometry> generateLayoutDeviceGeometry(const ice::LayoutDeviceCell &device,
                                                                      const std::map<std::string, std::string> &values,
                                                                      QString *error, const std::string &pdk) {
    if (error)
        error->clear();
    if (!device.generator) {
        if (error)
            *error = QStringLiteral("This device does not have a PCell generator.");
        return std::nullopt;
    }
    const auto packageRoot = pdkPackageRoot(pdk);
    if (packageRoot.empty()) {
        if (error)
            *error = QStringLiteral("The PDK package '%1' is not installed.").arg(QString::fromStdString(pdk));
        return std::nullopt;
    }
    const auto script = packageRoot / "layout" / "device_generator.py";
    if (!regularFile(script)) {
        if (error)
            *error = QStringLiteral("The PDK package '%1' does not provide layout/device_generator.py.")
                         .arg(QString::fromStdString(pdk));
        return std::nullopt;
    }
    const auto python = pythonExecutable();
    if (python.isEmpty()) {
        if (error)
            *error = QStringLiteral("Python 3 was not found. Set ICE_PYTHON to the interpreter path.");
        return std::nullopt;
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        if (error)
            *error = QStringLiteral("Unable to create a temporary directory for PCell generation.");
        return std::nullopt;
    }
    QJsonObject parameterObject;
    for (const auto &[name, value] : values)
        parameterObject.insert(QString::fromStdString(name), QString::fromStdString(value));
    const auto paramsPath = temporary.filePath(QStringLiteral("params.json"));
    const auto outputPath = temporary.filePath(QStringLiteral("geometry.json"));
    {
        QFile paramsFile(paramsPath);
        if (!paramsFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Unable to write the PCell parameter file.");
            return std::nullopt;
        }
        paramsFile.write(QJsonDocument(parameterObject).toJson(QJsonDocument::Compact));
    }

    QStringList arguments{QString::fromStdString(script.string()),
                          QStringLiteral("--pdk-root"),
                          QString::fromStdString(packageRoot.string()),
                          QStringLiteral("--module"),
                          QString::fromStdString(device.generator->module),
                          QStringLiteral("--class"),
                          QString::fromStdString(device.generator->class_name),
                          QStringLiteral("--params-file"),
                          paramsPath,
                          QStringLiteral("--output"),
                          outputPath};
    QProcess process;
    process.setProgram(python);
    process.setArguments(arguments);
    process.setWorkingDirectory(QString::fromStdString(packageRoot.string()));
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONPATH"), icepyPythonPath());
    process.setProcessEnvironment(environment);
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Unable to start the PCell generator: %1").arg(process.errorString());
        return std::nullopt;
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("The PCell generator timed out.");
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            auto message = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (message.isEmpty())
                message = QStringLiteral("The PCell generator exited with code %1.").arg(process.exitCode());
            if (message.size() > 4000)
                message = message.right(4000);
            *error = std::move(message);
        }
        return std::nullopt;
    }
    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("The PCell generator did not produce geometry.");
        return std::nullopt;
    }
    try {
        return ice::parseLayoutDeviceGeometry(outputFile.readAll().toStdString());
    } catch (const ice::ProjectError &exception) {
        if (error)
            *error = QString::fromStdString(exception.what());
        return std::nullopt;
    }
}

LayoutDevicePreview::LayoutDevicePreview(QWidget *parent) : QWidget(parent) {
    setMinimumSize(0, 0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Layout device preview"));
}

void LayoutDevicePreview::setCell(const ice::LayoutDeviceCell *cell) {
    model_ = cell ? ice::buildLayoutDevicePreviewModel(*cell) : ice::LayoutPreviewModel{};
    update();
}

void LayoutDevicePreview::setModel(const ice::LayoutPreviewModel &model) {
    model_ = model;
    update();
}

void LayoutDevicePreview::setStyles(const ice::LayoutRenderer::LayerStyleMap &styles,
                                    const ice::LayoutRenderer::DeviceStyleMap &deviceStyles) {
    styles_ = styles;
    deviceStyles_ = deviceStyles;
    update();
}

void LayoutDevicePreview::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), ice::uiColor(ice::UiColor::Background2));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ice::uiColor(ice::UiColor::Border1), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    if (model_.empty() || !model_.bounds.isValid()) {
        painter.setPen(ice::uiColor(ice::UiColor::Text2));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No geometry"));
        return;
    }

    ice::LayoutRenderer::PaintOptions options;
    options.shapeAlpha = 70;
    options.portAlpha = 30;
    options.outlineWidth = 0.6;
    ice::LayoutRenderer::paintFitted(painter, model_, styles_, deviceStyles_, rect().adjusted(10, 10, -10, -10),
                                     options);
}

namespace {
std::string lowerDeviceText(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}
} // namespace

LayoutDevicePlacementDialog::LayoutDevicePlacementDialog(const ice::LayoutDeviceLibrary &library,
                                                         const std::string &suggestedName,
                                                         const std::vector<std::string> &schematicPins,
                                                         const std::map<std::string, std::string> &initialParameters,
                                                         QWidget *parent)
    : QDialog(parent), library_(library), suggestedName_(suggestedName), schematicPins_(schematicPins),
      initialParameters_(initialParameters) {
    setWindowTitle(QStringLiteral("Place Layout Device"));
    setModal(true);
    auto *root = new QVBoxLayout(this);
    hint_ = new QLabel(
        QStringLiteral("Choose the layout PCell that represents %1. The parameter values apply to the new instance.")
            .arg(QString::fromStdString(suggestedName.empty() ? std::string("this device") : suggestedName)),
        this);
    hint_->setWordWrap(true);
    root->addWidget(hint_);

    auto *filters = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Search layout devices…"));
    search_->setClearButtonEnabled(true);
    category_ = new QComboBox(this);
    filters->addWidget(search_, 1);
    filters->addWidget(category_, 0);
    root->addLayout(filters);

    auto *content = new QHBoxLayout;
    auto *listColumn = new QVBoxLayout;
    list_ = new QListWidget(this);
    list_->setAccessibleName(QStringLiteral("Layout devices for planned device"));
    listColumn->addWidget(list_, 1);
    preview_ = new LayoutDevicePreview(this);
    preview_->setMinimumSize(260, 260);
    content->addLayout(listColumn, 1);
    content->addWidget(preview_, 1);
    root->addLayout(content, 1);

    parameters_ = new QTableWidget(0, 2, this);
    parameters_->setHorizontalHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Value")});
    parameters_->horizontalHeader()->setStretchLastSection(true);
    parameters_->verticalHeader()->setVisible(false);
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(parameters_, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (selectedDevice())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(category_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { rebuildList(); });
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { updateSelection(); });

    category_->addItem(QStringLiteral("All categories"), QString());
    std::set<std::string> categories;
    for (const auto &device : library_.cells)
        if (!device.category.empty())
            categories.insert(device.category);
    for (const auto &category : categories)
        category_->addItem(QString::fromStdString(category), QString::fromStdString(category));

    if (library_.cells.empty()) {
        hint_->setText(QStringLiteral("No layout device library is installed for this PDK. "
                                      "Use Draw Layout Manually instead."));
        search_->setEnabled(false);
        category_->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    }
    rebuildList();
    resize(820, 680);
}

int LayoutDevicePlacementDialog::scoreDevice(const ice::LayoutDeviceCell &device) const {
    int score = 0;
    if (device.ports.empty())
        score -= 1000;
    const auto lowerName = lowerDeviceText(device.name);
    const auto suggested = lowerDeviceText(suggestedName_);

    if (const auto model = initialParameters_.find("model"); model != initialParameters_.end()) {
        for (const auto &parameter : device.parameters) {
            if (parameter.name == "model" && parameter.value == model->second) {
                score += 1000;
                break;
            }
        }
    }
    if (!suggested.empty()) {
        if (suggested == lowerName)
            score += 200;
        else if (suggested.find(lowerName) != std::string::npos || lowerName.find(suggested) != std::string::npos)
            score += 80;
        if (suggested.find("resistor") != std::string::npos &&
            (lowerName.find("res") != std::string::npos || lowerName.starts_with("rhigh") ||
             lowerName.starts_with("rppd") || lowerName.starts_with("rsil")))
            score += 40;
        if (suggested.find("capacitor") != std::string::npos &&
            (lowerName.find("cap") != std::string::npos || lowerName.find("cmim") != std::string::npos))
            score += 40;
        if (suggested.find("inductor") != std::string::npos && lowerName.find("ind") != std::string::npos)
            score += 40;
        if (suggested.find("diode") != std::string::npos &&
            (lowerName.find("diode") != std::string::npos || lowerName.find("dantenna") != std::string::npos ||
             lowerName.find("dpantenna") != std::string::npos))
            score += 40;
    }
    const auto voltage =
        suggested.find("6v") != std::string::npos
            ? std::string("6")
            : (suggested.find("10v") != std::string::npos
                   ? std::string("10")
                   : (suggested.find("3.3v") != std::string::npos ? std::string("3.3") : std::string{}));
    if (!voltage.empty()) {
        for (const auto &parameter : device.parameters) {
            if (lowerDeviceText(parameter.name) != "volt")
                continue;
            const auto value = lowerDeviceText(parameter.value);
            if (value.find(voltage) != std::string::npos) {
                score += 60;
                break;
            }
        }
    }

    for (const auto &pin : schematicPins_) {
        const auto wanted = lowerDeviceText(pin);
        for (const auto &port : device.ports) {
            const auto portName = lowerDeviceText(port.name);
            const auto portLabel = lowerDeviceText(port.label);
            if (portName == wanted || portLabel == wanted) {
                score += 10;
                break;
            }
            if ((wanted == "1" && (portName == "plus" || portName == "a" || portName == "p" || portName == "1")) ||
                (wanted == "2" &&
                 (portName == "minus" || portName == "b" || portName == "n" || portName == "2" || portName == "k"))) {
                score += 8;
                break;
            }
        }
    }
    return score;
}

std::string LayoutDevicePlacementDialog::initialValue(const std::string &parameterName) const {
    const auto find = [&](const std::string &name) -> std::string {
        const auto wanted = lowerDeviceText(name);
        for (const auto &[candidate, value] : initialParameters_) {
            if (lowerDeviceText(candidate) == wanted)
                return value;
        }
        return {};
    };
    auto value = find(parameterName);
    if (!value.empty())
        return value;
    const auto parameter = lowerDeviceText(parameterName);
    if (parameter == "r")
        return find("resistance");
    if (parameter == "c")
        return find("capacitance");
    if (parameter == "nf")
        return find("m");
    return {};
}

void LayoutDevicePlacementDialog::rebuildList() {
    if (!list_)
        return;
    const auto query = search_ ? search_->text().trimmed().toLower() : QString{};
    const auto category = category_ ? category_->currentData().toString() : QString{};
    const auto previous = selectedDevice() ? selectedDevice()->name : std::string{};
    std::vector<int> indices;
    for (int index = 0; index < static_cast<int>(library_.cells.size()); ++index) {
        const auto &device = library_.cells[static_cast<std::size_t>(index)];
        if (!category.isEmpty() && QString::fromStdString(device.category) != category)
            continue;
        if (!query.isEmpty()) {
            const auto haystack = QString::fromStdString(device.name + " " + device.category).toLower();
            if (!haystack.contains(query))
                continue;
        }
        indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end(), [&](int left, int right) {
        const auto &a = library_.cells[static_cast<std::size_t>(left)];
        const auto &b = library_.cells[static_cast<std::size_t>(right)];
        const auto leftScore = scoreDevice(a);
        const auto rightScore = scoreDevice(b);
        if (leftScore != rightScore)
            return leftScore > rightScore;
        return a.name < b.name;
    });

    QSignalBlocker blocker(list_);
    list_->clear();
    int selectedRow = indices.empty() ? -1 : 0;
    for (int row = 0; row < static_cast<int>(indices.size()); ++row) {
        const auto index = indices[static_cast<std::size_t>(row)];
        const auto &device = library_.cells[static_cast<std::size_t>(index)];
        auto *item = new QListWidgetItem(QString::fromStdString(device.name));
        item->setData(Qt::UserRole, index);
        item->setToolTip(QString::fromStdString(device.category));
        list_->addItem(item);
        if (!previous.empty() && device.name == previous)
            selectedRow = row;
    }
    blocker.unblock();
    if (selectedRow >= 0)
        list_->setCurrentRow(selectedRow);
    updateSelection();
}

void LayoutDevicePlacementDialog::updateSelection() {
    if (!preview_ || !parameters_)
        return;
    const auto *device = selectedDevice();
    preview_->setCell(device);
    parameters_->setRowCount(0);
    if (auto *buttons = findChild<QDialogButtonBox *>())
        buttons->button(QDialogButtonBox::Ok)->setEnabled(device != nullptr);
    if (!device)
        return;
    for (const auto &parameter : device->parameters) {
        const auto row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *name = new QTableWidgetItem(QString::fromStdString(parameter.name));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        auto *value = new QTableWidgetItem(QString::fromStdString(
            initialValue(parameter.name).empty() ? parameter.value : initialValue(parameter.name)));
        parameters_->setItem(row, 0, name);
        parameters_->setItem(row, 1, value);
    }
    parameters_->resizeColumnToContents(0);
}

const ice::LayoutDeviceCell *LayoutDevicePlacementDialog::selectedDevice() const {
    if (!list_)
        return nullptr;
    const auto *item = list_->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid())
        return nullptr;
    const auto index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(library_.cells.size()))
        return nullptr;
    return &library_.cells[static_cast<std::size_t>(index)];
}

std::map<std::string, std::string> LayoutDevicePlacementDialog::parameterValues() const {
    std::map<std::string, std::string> values;
    if (!parameters_)
        return values;
    for (int row = 0; row < parameters_->rowCount(); ++row) {
        const auto *name = parameters_->item(row, 0);
        const auto *value = parameters_->item(row, 1);
        if (name && value)
            values[name->text().toStdString()] = value->text().toStdString();
    }
    return values;
}

LayoutDevicesPanel::LayoutDevicesPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search devices…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Search layout devices"));
    category_ = new QComboBox;
    category_->setAccessibleName(QStringLiteral("Layout device category"));
    category_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    category_->setMinimumContentsLength(12);
    layout->addWidget(search_);
    layout->addWidget(category_);
    list_ = new QListWidget;
    list_->setAccessibleName(QStringLiteral("Available layout devices"));
    layout->addWidget(list_, 1);

    libraryPath_ = layoutDeviceLibraryPath();
    try {
        if (libraryPath_.empty())
            throw ice::ProjectError("layout.library", "Layout device library is not available");
        library_ = ice::LayoutDeviceLibrary::load(libraryPath_);
    } catch (const ice::ProjectError &error) {
        loadError_ = QString::fromStdString(error.what());
    }
    for (const auto &cell : library_.cells) {
        defaultParameters_[cell.name] = cell.parameters;
        defaultGeometry_[cell.name] = ice::LayoutDeviceGeometry{cell.bounds, cell.shapes};
    }
    rebuildCategories();
    rebuildList();

    connect(search_, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(category_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { rebuildList(); });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *) {
        notifySelection();
        if (current && current->data(Qt::UserRole).isValid() && onDeviceActivated_)
            onDeviceActivated_();
    });
}

void LayoutDevicesPanel::setOnDeviceSelected(DeviceSelectedCallback callback) {
    onDeviceSelected_ = std::move(callback);
}

void LayoutDevicesPanel::setOnDeviceActivated(std::function<void()> callback) {
    onDeviceActivated_ = std::move(callback);
}

void LayoutDevicesPanel::focusDeviceList() { list_->setFocus(); }

void LayoutDevicesPanel::clearDeviceSelection() {
    QSignalBlocker blocker(list_);
    list_->setCurrentRow(-1);
    list_->clearSelection();
    notifySelection();
}

void LayoutDevicesPanel::setPdk(const std::string &pdk) {
    if (activePdk_ && layoutPdkIdsMatch(*activePdk_, pdk))
        return;
    activePdk_ = pdk;

    if (!pdk.empty() && pdk != ice::genericPdkId) {
        const auto path = layoutDeviceLibraryPath(pdk);
        if (!path.empty() && path != libraryPath_) {
            try {
                auto library = ice::LayoutDeviceLibrary::load(path);
                library_ = std::move(library);
                libraryPath_ = path;
                loadError_.clear();
                defaultParameters_.clear();
                defaultGeometry_.clear();
                for (const auto &cell : library_.cells) {
                    defaultParameters_[cell.name] = cell.parameters;
                    defaultGeometry_[cell.name] = ice::LayoutDeviceGeometry{cell.bounds, cell.shapes};
                }
            } catch (const ice::ProjectError &error) {
                loadError_ = QString::fromStdString(error.what());
            }
        } else if (path.empty()) {
            loadError_ = QStringLiteral("No layout device library is installed for this PDK.");
        }
    }

    clearDeviceSelection();
    rebuildCategories();
    rebuildList();
}

const ice::LayoutDeviceLibrary &LayoutDevicesPanel::library() const { return library_; }

const ice::LayoutDeviceCell *LayoutDevicesPanel::selectedDevice() const {
    const auto *item = list_->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid())
        return nullptr;
    const auto index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(library_.cells.size()))
        return nullptr;
    return &library_.cells[index];
}

bool LayoutDevicesPanel::setParameterValue(const std::string &deviceName, const std::string &parameterName,
                                           const std::string &value) {
    for (auto &cell : library_.cells) {
        if (cell.name != deviceName)
            continue;
        for (auto &parameter : cell.parameters) {
            if (parameter.name != parameterName)
                continue;
            parameter.value = value;
            return true;
        }
        return false;
    }
    return false;
}

bool LayoutDevicesPanel::setDeviceGeometry(const std::string &deviceName, const ice::LayoutDeviceGeometry &geometry) {
    for (auto &cell : library_.cells) {
        if (cell.name != deviceName)
            continue;
        cell.bounds = geometry.bounds;
        cell.shapes = geometry.shapes;
        return true;
    }
    return false;
}

void LayoutDevicesPanel::resetDevices() {
    for (auto &cell : library_.cells) {
        if (const auto defaults = defaultParameters_.find(cell.name); defaults != defaultParameters_.end())
            cell.parameters = defaults->second;
        if (const auto geometry = defaultGeometry_.find(cell.name); geometry != defaultGeometry_.end()) {
            cell.bounds = geometry->second.bounds;
            cell.shapes = geometry->second.shapes;
        }
    }
}

void LayoutDevicesPanel::notifySelection() {
    if (onDeviceSelected_)
        onDeviceSelected_(selectedDevice());
}

void LayoutDevicesPanel::rebuildCategories() {
    const auto current = category_->currentData().toString();
    QSignalBlocker blocker(category_);
    category_->clear();
    category_->addItem(QStringLiteral("All categories"), QString());
    QStringList categories;
    if (activePdk_ && layoutPdkIdsMatch(*activePdk_, library_.pdk)) {
        for (const auto &cell : library_.cells) {
            const auto category = QString::fromStdString(cell.category);
            if (!category.isEmpty() && !categories.contains(category))
                categories << category;
        }
    }
    for (const auto &category : categories)
        category_->addItem(category, category);
    const auto index = category_->findData(current);
    category_->setCurrentIndex(index >= 0 ? index : 0);
}

void LayoutDevicesPanel::rebuildList(const QString &keepSelected) {
    QString selectedName = keepSelected;
    if (selectedName.isEmpty()) {
        if (const auto *item = list_->currentItem(); item && item->data(Qt::UserRole).isValid()) {
            const auto index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(library_.cells.size()))
                selectedName = QString::fromStdString(library_.cells[index].name);
        }
    }
    const auto needle = search_->text().trimmed();
    const auto category = category_->currentData().toString();
    QSignalBlocker blocker(list_);
    list_->clear();
    int selectedRow = -1;
    int row = 0;
    if (!activePdk_ || !layoutPdkIdsMatch(*activePdk_, library_.pdk)) {
        notifySelection();
        return;
    }
    for (std::size_t index = 0; index < library_.cells.size(); ++index) {
        const auto &cell = library_.cells[index];
        const auto cellCategory = QString::fromStdString(cell.category);
        if (!category.isEmpty() && cellCategory != category)
            continue;
        const auto cellName = QString::fromStdString(cell.name);
        const auto source = QString::fromStdString(cell.source);
        if (!needle.isEmpty() && !cellName.contains(needle, Qt::CaseInsensitive) &&
            !cellCategory.contains(needle, Qt::CaseInsensitive) && !source.contains(needle, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(cellName, list_);
        item->setData(Qt::UserRole, static_cast<int>(index));
        item->setToolTip(QStringLiteral("%1 · %2 shapes · %3").arg(cellCategory).arg(cell.shapes.size()).arg(source));
        if (cellName == selectedName)
            selectedRow = row;
        ++row;
    }
    if (list_->count() == 0) {
        auto *item =
            new QListWidgetItem(loadError_.isEmpty() ? QStringLiteral("No matching devices") : loadError_, list_);
        item->setFlags(Qt::NoItemFlags);
        notifySelection();
        return;
    }
    list_->setCurrentRow(selectedRow);
    list_->clearSelection();
    if (selectedRow >= 0)
        list_->setCurrentRow(selectedRow);
    notifySelection();
}
