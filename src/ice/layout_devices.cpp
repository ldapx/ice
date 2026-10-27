#include "layout_devices.hpp"

#include "icelib/operations.hpp"
#include "package_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPaintEvent>
#include <QPainter>
#include <QProcess>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

void addPdkLayers(ice::Project &project, const ice::PackagePdk &pdk) {
    for (const auto &layer : pdk.layers) {
        const auto exists = std::any_of(project.layers.begin(), project.layers.end(),
                                        [&](const auto &entry) { return entry.second.name == layer.name; });
        if (!exists)
            ice::addLayer(project, layer.name, layer.number);
    }
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
        const auto package = std::find_if(packages.begin(), packages.end(),
                                          [&](const auto &entry) { return entry.spec.name == pdkName; });
        if (package != packages.end() && package->spec.pdk)
            addPdkLayers(project, *package->spec.pdk);
    }
}

std::filesystem::path layoutDeviceLibraryPath() {
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "assets" / "layout" / "ihp.sg13g2.json",
        std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "layout" / "ihp.sg13g2.json",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "ihp.sg13g2.json",
#endif
        std::filesystem::path("assets") / "layout" / "ihp.sg13g2.json",
        std::filesystem::path("/usr/share/ice/assets/layout/ihp.sg13g2.json"),
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
    }
    return {};
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
    const auto name = cellName.starts_with("IHP ") ? cellName.substr(4) : cellName;
    const auto it = std::find_if(library.cells.begin(), library.cells.end(),
                                 [&](const auto &candidate) { return candidate.name == name; });
    return it == library.cells.end() ? nullptr : &*it;
}

const ice::LayoutDeviceCell *findLayoutDeviceForCellName(const ice::LayoutDeviceLibrary &library,
                                                         const std::string &cellName);

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
    const auto name = cellName.starts_with("IHP ") ? cellName.substr(4) : cellName;
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

std::string uniqueDeviceCellName(const ice::Project &project, const std::string &deviceName) {
    const auto base = "IHP " + deviceName;
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
        clone.shapes.emplace(id, ice::Shape{id, shape.layer, shape.net, shape.rectangle, shape.polygon});
    }
    clone.ports.clear();
    for (const auto &[_, port] : source.ports) {
        const auto id = nextId++;
        clone.ports.emplace(id, ice::Port{id, port.name, port.layer, port.rectangle});
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

std::filesystem::path layoutPcellScriptPath() {
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "assets" / "layout" / "ihp_pcell.py",
        std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "layout" / "ihp_pcell.py",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "ihp_pcell.py",
#endif
        std::filesystem::path("assets") / "layout" / "ihp_pcell.py",
        std::filesystem::path("/usr/share/ice/assets/layout/ihp_pcell.py"),
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
    }
    return {};
}

std::filesystem::path ihpSourceRoot() {
    const auto valid = [](const std::filesystem::path &path) {
        std::error_code status;
        return std::filesystem::is_directory(path / "cni", status) && !status &&
               std::filesystem::is_directory(path / "ihp" / "refs" / "ihp_pycell", status) && !status;
    };
    std::vector<std::filesystem::path> candidates;
    const auto configured = qEnvironmentVariable("ICE_IHP_ROOT");
    if (!configured.isEmpty())
        candidates.emplace_back(configured.toStdString());
#ifdef ICE_SOURCE_DIR
    candidates.push_back(std::filesystem::path(ICE_SOURCE_DIR).parent_path() / "IHP");
#endif
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    candidates.push_back(std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / ".." / "IHP");
    candidates.push_back(std::filesystem::current_path() / "IHP");
    candidates.push_back(std::filesystem::current_path().parent_path() / "IHP");
    candidates.push_back(std::filesystem::path(QDir::homePath().toStdString()) / "repositories" / "IHP");
    for (const auto &candidate : candidates) {
        std::error_code status;
        const auto normalized = std::filesystem::weakly_canonical(candidate, status);
        if (!status && valid(normalized))
            return normalized;
    }
    return {};
}

QString pythonExecutable() {
    const auto configured = qEnvironmentVariable("ICE_PYTHON");
    if (!configured.isEmpty())
        return configured;
    for (const auto &candidate : {QStringLiteral("python3"), QStringLiteral("python")}) {
        const auto path = QStandardPaths::findExecutable(candidate);
        if (!path.isEmpty())
            return path;
    }
    return {};
}

void applyLayoutDeviceShapes(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                             const std::vector<ice::LayoutDeviceShape> &sourceShapes) {
    cell.shapes.clear();
    cell.ports.clear();
    cell.instances.clear();
    std::map<std::pair<int, int>, std::string> layerNames;
    for (const auto &layer : library.layers)
        layerNames[{layer.number, layer.datatype}] = layer.name;
    std::map<std::pair<int, int>, ice::ObjectId> layerIds;
    for (const auto &sourceShape : sourceShapes) {
        if (sourceShape.polygon.size() < 3)
            continue;
        const auto key = std::make_pair(sourceShape.layer, sourceShape.datatype);
        auto layerIt = layerIds.find(key);
        if (layerIt == layerIds.end()) {
            const auto nameIt = layerNames.find(key);
            std::string layerName;
            if (nameIt != layerNames.end() && !nameIt->second.empty())
                layerName = nameIt->second;
            else if (sourceShape.datatype == 0)
                layerName = "Layer " + std::to_string(sourceShape.layer);
            else
                layerName =
                    "Layer " + std::to_string(sourceShape.layer) + " (dt " + std::to_string(sourceShape.datatype) + ")";
            ice::ObjectId layerId = 0;
            for (const auto &[id, layer] : project.layers) {
                if (layer.number == sourceShape.layer && layer.name == layerName) {
                    layerId = id;
                    break;
                }
            }
            if (layerId == 0)
                layerId = ice::addLayer(project, layerName, sourceShape.layer);
            layerIt = layerIds.emplace(key, layerId).first;
        }
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
        cell.shapes.emplace(shapeId, ice::Shape{shapeId, layerIt->second, std::nullopt,
                                                ice::Rect{minX, minY, maxX - minX, maxY - minY}, std::move(polygon)});
    }
}

std::optional<ice::LayoutDeviceCell> makeLayoutCellPreview(const ice::Project &project, const ice::LayoutCell &cell,
                                                           const ice::LayoutDeviceLibrary &library) {
    std::map<std::pair<int, std::string>, int> datatypes;
    for (const auto &layer : library.layers)
        datatypes[{layer.number, layer.name}] = layer.datatype;

    ice::LayoutDeviceCell preview;
    preview.name = cell.name;
    preview.category = "Device";
    ice::Coordinate minX = std::numeric_limits<ice::Coordinate>::max();
    ice::Coordinate minY = std::numeric_limits<ice::Coordinate>::max();
    ice::Coordinate maxX = std::numeric_limits<ice::Coordinate>::lowest();
    ice::Coordinate maxY = std::numeric_limits<ice::Coordinate>::lowest();
    for (const auto &[_, shape] : cell.shapes) {
        const auto layerIt = project.layers.find(shape.layer);
        if (layerIt == project.layers.end())
            continue;
        ice::LayoutDeviceShape previewShape;
        previewShape.layer = layerIt->second.number;
        const auto datatype = datatypes.find({layerIt->second.number, layerIt->second.name});
        previewShape.datatype = datatype == datatypes.end() ? 0 : datatype->second;
        if (shape.polygon.size() >= 3) {
            previewShape.polygon.reserve(shape.polygon.size());
            for (const auto &point : shape.polygon)
                previewShape.polygon.push_back({point.x, point.y});
        } else {
            const auto &rect = shape.rectangle;
            previewShape.polygon = {{rect.x, rect.y},
                                    {rect.x + rect.width, rect.y},
                                    {rect.x + rect.width, rect.y + rect.height},
                                    {rect.x, rect.y + rect.height}};
        }
        for (const auto &point : previewShape.polygon) {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }
        preview.shapes.push_back(std::move(previewShape));
    }
    if (preview.shapes.empty() || maxX <= minX || maxY <= minY)
        return std::nullopt;
    preview.bounds = ice::Rect{minX, minY, maxX - minX, maxY - minY};
    return preview;
}

std::optional<ice::LayoutDeviceGeometry> generateLayoutDeviceGeometry(const ice::LayoutDeviceCell &device,
                                                                      const std::map<std::string, std::string> &values,
                                                                      QString *error) {
    if (error)
        error->clear();
    if (!device.generator) {
        if (error)
            *error = QStringLiteral("This device does not have a PCell generator.");
        return std::nullopt;
    }
    const auto script = layoutPcellScriptPath();
    if (script.empty()) {
        if (error)
            *error = QStringLiteral("The IHP PCell generator script was not found.");
        return std::nullopt;
    }
    const auto ihpRoot = ihpSourceRoot();
    if (ihpRoot.empty()) {
        if (error)
            *error = QStringLiteral("The IHP source tree was not found. Set ICE_IHP_ROOT to its location.");
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
                          QStringLiteral("--ihp-root"),
                          QString::fromStdString(ihpRoot.string()),
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
    process.setWorkingDirectory(QString::fromStdString(ihpRoot.string()));
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Unable to start the IHP PCell generator: %1").arg(process.errorString());
        return std::nullopt;
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("The IHP PCell generator timed out.");
        return std::nullopt;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            auto message = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            if (message.isEmpty())
                message = QStringLiteral("The IHP PCell generator exited with code %1.").arg(process.exitCode());
            if (message.size() > 4000)
                message = message.right(4000);
            *error = message;
        }
        return std::nullopt;
    }
    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("The IHP PCell generator did not produce geometry.");
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
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Layout device preview"));
}

void LayoutDevicePreview::setCell(const ice::LayoutDeviceCell *cell) {
    cell_ = cell;
    update();
}

void LayoutDevicePreview::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), ice::uiColor(ice::UiColor::Background2));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ice::uiColor(ice::UiColor::Border1), 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    if (!cell_ || cell_->shapes.empty() || !cell_->bounds.valid()) {
        painter.setPen(ice::uiColor(ice::UiColor::Text2));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No geometry"));
        return;
    }

    const auto &bounds = cell_->bounds;
    const qreal maxY = static_cast<qreal>(bounds.y + bounds.height);
    const QRectF area = rect().adjusted(10, 10, -10, -10);
    const qreal width = static_cast<qreal>(bounds.width);
    const qreal height = static_cast<qreal>(bounds.height);
    if (width <= 0.0 || height <= 0.0 || area.width() <= 0.0 || area.height() <= 0.0)
        return;
    const qreal scale = std::min(area.width() / width, area.height() / height);
    const qreal drawWidth = width * scale;
    const qreal drawHeight = height * scale;
    const qreal offsetX = area.left() + (area.width() - drawWidth) / 2.0;
    const qreal offsetY = area.top() + (area.height() - drawHeight) / 2.0;
    const auto &colors = layerPalette();
    painter.setClipRect(area);
    for (const auto &shape : cell_->shapes) {
        QPolygonF polygon;
        polygon.reserve(static_cast<int>(shape.polygon.size()));
        for (const auto &point : shape.polygon) {
            polygon << QPointF(offsetX + (static_cast<qreal>(point.x) - bounds.x) * scale,
                               offsetY + (maxY - static_cast<qreal>(point.y)) * scale);
        }
        auto color = colors[(static_cast<std::size_t>(shape.layer) * 31 + shape.datatype) % colors.size()];
        painter.setPen(QPen(color, 0.6));
        color.setAlpha(70);
        painter.setBrush(color);
        painter.drawPolygon(polygon);
    }
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

    const auto libraryPath = layoutDeviceLibraryPath();
    try {
        if (libraryPath.empty())
            throw ice::ProjectError("layout.library", "Layout device library is not available");
        library_ = ice::LayoutDeviceLibrary::load(libraryPath);
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

void LayoutDevicesPanel::focusDeviceList() {
    list_->setFocus();
}

void LayoutDevicesPanel::clearDeviceSelection() {
    QSignalBlocker blocker(list_);
    list_->setCurrentRow(-1);
    list_->clearSelection();
    notifySelection();
}

void LayoutDevicesPanel::setPdk(const std::string &pdk) {
    if (activePdk_ && *activePdk_ == pdk)
        return;
    activePdk_ = pdk;
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
    if (activePdk_ && *activePdk_ == library_.pdk) {
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
    if (!activePdk_ || *activePdk_ != library_.pdk) {
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
