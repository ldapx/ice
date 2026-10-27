#include "procedural_layout.hpp"

#include "icelib/operations.hpp"
#include "icons.hpp"
#include "layout_devices.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ice {
namespace {

std::filesystem::path proceduralRunnerScriptPath() {
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "assets" / "layout" / "procedural_runner.py",
        std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "layout" / "procedural_runner.py",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "layout" / "procedural_runner.py",
#endif
        std::filesystem::path("assets") / "layout" / "procedural_runner.py",
        std::filesystem::path("/usr/share/ice/assets/layout/procedural_runner.py"),
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_regular_file(candidate, status) && !status)
            return candidate;
    }
    return {};
}

struct ParsedShape {
    std::string layer;
    std::optional<Rect> rectangle;
    std::vector<LayoutPoint> polygon;
};

struct ParsedPort {
    std::string name;
    std::string layer;
    Rect rectangle;
};

struct ParsedInstance {
    std::string cell;
    std::string name;
    Coordinate x = 0;
    Coordinate y = 0;
    Orientation orientation = Orientation::r0;
};

struct ParsedLayout {
    std::map<std::string, std::optional<int>> layers;
    std::vector<ParsedShape> shapes;
    std::vector<ParsedPort> ports;
    std::vector<ParsedInstance> instances;
};

bool parseInteger(const QJsonValue &value, const QString &label, Coordinate &result, QString *error) {
    if (!value.isDouble()) {
        if (error)
            *error = QStringLiteral("%1 must be a number").arg(label);
        return false;
    }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < static_cast<double>(std::numeric_limits<Coordinate>::lowest()) ||
        number > static_cast<double>(std::numeric_limits<Coordinate>::max())) {
        if (error)
            *error = QStringLiteral("%1 is outside the supported coordinate range").arg(label);
        return false;
    }
    result = static_cast<Coordinate>(std::llround(number));
    return true;
}

bool parseRect(const QJsonObject &object, const QString &label, Rect &rect, QString *error) {
    if (!parseInteger(object.value("x"), label + QStringLiteral(" x"), rect.x, error) ||
        !parseInteger(object.value("y"), label + QStringLiteral(" y"), rect.y, error) ||
        !parseInteger(object.value("width"), label + QStringLiteral(" width"), rect.width, error) ||
        !parseInteger(object.value("height"), label + QStringLiteral(" height"), rect.height, error))
        return false;
    if (!rect.valid()) {
        if (error)
            *error = QStringLiteral("%1 must have positive width and height").arg(label);
        return false;
    }
    return true;
}

bool parseOrientation(const QString &value, Orientation &orientation, QString *error) {
    static const std::map<QString, Orientation> values = {
        {QStringLiteral("r0"), Orientation::r0},     {QStringLiteral("r90"), Orientation::r90},
        {QStringLiteral("r180"), Orientation::r180}, {QStringLiteral("r270"), Orientation::r270},
        {QStringLiteral("mx"), Orientation::mx},     {QStringLiteral("my"), Orientation::my},
        {QStringLiteral("m45"), Orientation::m45},   {QStringLiteral("m135"), Orientation::m135}};
    const auto it = values.find(value);
    if (it == values.end()) {
        if (error)
            *error = QStringLiteral("Unsupported orientation: %1").arg(value);
        return false;
    }
    orientation = it->second;
    return true;
}

bool parseGeneratedLayout(const QJsonObject &root, ParsedLayout &layout, QString *error) {
    const auto layers = root.value("layers");
    if (!layers.isArray()) {
        if (error)
            *error = QStringLiteral("The procedural layout output has no layers array.");
        return false;
    }
    for (const auto &entry : layers.toArray()) {
        if (!entry.isObject()) {
            if (error)
                *error = QStringLiteral("A procedural layout layer entry is not an object.");
            return false;
        }
        const auto object = entry.toObject();
        const auto name = object.value("name").toString().trimmed();
        if (name.isEmpty()) {
            if (error)
                *error = QStringLiteral("A procedural layout layer has no name.");
            return false;
        }
        std::optional<int> number;
        const auto numberValue = object.value("number");
        if (!numberValue.isUndefined() && !numberValue.isNull()) {
            if (!numberValue.isDouble()) {
                if (error)
                    *error = QStringLiteral("Layer %1 number must be a number.").arg(name);
                return false;
            }
            const auto raw = numberValue.toDouble();
            if (!std::isfinite(raw) || raw < 0 || raw > static_cast<double>(std::numeric_limits<int>::max())) {
                if (error)
                    *error = QStringLiteral("Layer %1 number is invalid.").arg(name);
                return false;
            }
            number = static_cast<int>(std::llround(raw));
        }
        layout.layers[name.toStdString()] = number;
    }

    const auto shapes = root.value("shapes");
    if (!shapes.isArray()) {
        if (error)
            *error = QStringLiteral("The procedural layout output has no shapes array.");
        return false;
    }
    for (const auto &entry : shapes.toArray()) {
        if (!entry.isObject()) {
            if (error)
                *error = QStringLiteral("A procedural layout shape is not an object.");
            return false;
        }
        const auto object = entry.toObject();
        ParsedShape shape;
        shape.layer = object.value("layer").toString().trimmed().toStdString();
        if (shape.layer.empty()) {
            if (error)
                *error = QStringLiteral("A procedural layout shape has no layer.");
            return false;
        }
        if (object.contains("rectangle")) {
            Rect rectangle;
            if (!parseRect(object.value("rectangle").toObject(), QStringLiteral("Shape rectangle"), rectangle, error))
                return false;
            shape.rectangle = rectangle;
        } else if (object.contains("polygon")) {
            const auto points = object.value("polygon");
            if (!points.isArray() || points.toArray().size() < 3) {
                if (error)
                    *error = QStringLiteral("A procedural layout polygon needs at least three points.");
                return false;
            }
            for (const auto &pointValue : points.toArray()) {
                if (!pointValue.isObject()) {
                    if (error)
                        *error = QStringLiteral("A procedural layout polygon point is not an object.");
                    return false;
                }
                const auto point = pointValue.toObject();
                LayoutPoint layoutPoint;
                if (!parseInteger(point.value("x"), QStringLiteral("Polygon point x"), layoutPoint.x, error) ||
                    !parseInteger(point.value("y"), QStringLiteral("Polygon point y"), layoutPoint.y, error))
                    return false;
                shape.polygon.push_back(layoutPoint);
            }
        } else {
            if (error)
                *error = QStringLiteral("A procedural layout shape has no rectangle or polygon.");
            return false;
        }
        layout.shapes.push_back(std::move(shape));
    }

    const auto ports = root.value("ports");
    if (!ports.isUndefined() && !ports.isNull()) {
        if (!ports.isArray()) {
            if (error)
                *error = QStringLiteral("The procedural layout ports entry is not an array.");
            return false;
        }
        for (const auto &entry : ports.toArray()) {
            if (!entry.isObject()) {
                if (error)
                    *error = QStringLiteral("A procedural layout port is not an object.");
                return false;
            }
            const auto object = entry.toObject();
            ParsedPort port;
            port.name = object.value("name").toString().trimmed().toStdString();
            port.layer = object.value("layer").toString().trimmed().toStdString();
            if (port.name.empty() || port.layer.empty()) {
                if (error)
                    *error = QStringLiteral("A procedural layout port needs a name and a layer.");
                return false;
            }
            if (!parseRect(object.value("rectangle").toObject(),
                           QStringLiteral("Port %1 rectangle").arg(QString::fromStdString(port.name)), port.rectangle,
                           error))
                return false;
            layout.ports.push_back(std::move(port));
        }
    }

    const auto instances = root.value("instances");
    if (!instances.isUndefined() && !instances.isNull()) {
        if (!instances.isArray()) {
            if (error)
                *error = QStringLiteral("The procedural layout instances entry is not an array.");
            return false;
        }
        for (const auto &entry : instances.toArray()) {
            if (!entry.isObject()) {
                if (error)
                    *error = QStringLiteral("A procedural layout instance is not an object.");
                return false;
            }
            const auto object = entry.toObject();
            ParsedInstance instance;
            instance.cell = object.value("cell").toString().trimmed().toStdString();
            instance.name = object.value("name").toString().trimmed().toStdString();
            if (instance.cell.empty() || instance.name.empty()) {
                if (error)
                    *error = QStringLiteral("A procedural layout instance needs a cell name and an instance name.");
                return false;
            }
            if (!parseInteger(object.value("x"),
                              QStringLiteral("Instance %1 x").arg(QString::fromStdString(instance.name)), instance.x,
                              error) ||
                !parseInteger(object.value("y"),
                              QStringLiteral("Instance %1 y").arg(QString::fromStdString(instance.name)), instance.y,
                              error) ||
                !parseOrientation(object.value("orientation").toString(QStringLiteral("r0")), instance.orientation,
                                  error))
                return false;
            layout.instances.push_back(std::move(instance));
        }
    }
    return true;
}

int inferredLayerNumber(const std::string &name, const Project &project) {
    int number = 0;
    int multiplier = 1;
    bool hasDigits = false;
    for (auto it = name.rbegin(); it != name.rend() && std::isdigit(static_cast<unsigned char>(*it)); ++it) {
        number += (*it - '0') * multiplier;
        multiplier *= 10;
        hasDigits = true;
    }
    if (hasDigits && number > 0)
        return number;
    int highest = 0;
    for (const auto &[_, layer] : project.layers)
        highest = std::max(highest, layer.number);
    return highest + 1;
}

bool applyGeneratedLayout(Project &project, ObjectId proceduralLayoutId, const ProceduralLayout &procedural,
                          const ParsedLayout &generated, QString *error) {
    auto cellIt = project.layoutCells.find(procedural.generatedCell);
    if (cellIt == project.layoutCells.end()) {
        if (error)
            *error = QStringLiteral("The generated layout cell is missing from the project.");
        return false;
    }

    std::map<std::string, ObjectId> cellsByName;
    for (const auto &[id, cell] : project.layoutCells)
        cellsByName[cell.name] = id;
    for (const auto &instance : generated.instances) {
        const auto cell = cellsByName.find(instance.cell);
        if (cell == cellsByName.end()) {
            if (error)
                *error = QStringLiteral("Instance '%1' references an unknown layout cell '%2'.")
                             .arg(QString::fromStdString(instance.name), QString::fromStdString(instance.cell));
            return false;
        }
        if (cell->second == procedural.generatedCell) {
            if (error)
                *error = QStringLiteral("Instance '%1' would make the generated layout contain itself.")
                             .arg(QString::fromStdString(instance.name));
            return false;
        }
    }

    auto backup = project;
    try {
        std::map<std::string, ObjectId> layerIds;
        for (const auto &[name, number] : generated.layers) {
            const auto existing = std::find_if(project.layers.begin(), project.layers.end(),
                                               [&](const auto &entry) { return entry.second.name == name; });
            if (existing != project.layers.end()) {
                layerIds[name] = existing->first;
                continue;
            }
            const auto layerNumber = number ? *number : inferredLayerNumber(name, project);
            layerIds[name] = addLayer(project, name, layerNumber);
        }
        const auto layerIdFor = [&](const std::string &name) {
            const auto existing = std::find_if(project.layers.begin(), project.layers.end(),
                                               [&](const auto &entry) { return entry.second.name == name; });
            if (existing != project.layers.end())
                return existing->first;
            const auto generatedLayer = layerIds.find(name);
            if (generatedLayer != layerIds.end())
                return generatedLayer->second;
            return addLayer(project, name, inferredLayerNumber(name, project));
        };

        auto &cell = project.layoutCells.at(procedural.generatedCell);
        cell.shapes.clear();
        cell.ports.clear();
        cell.instances.clear();
        cell.plannedDevices.clear();
        cell.plannedConnections.clear();
        cell.generatedBy = proceduralLayoutId;

        for (const auto &shape : generated.shapes) {
            const auto id = project.nextId();
            if (shape.rectangle) {
                cell.shapes.emplace(id, Shape{id, layerIdFor(shape.layer), std::nullopt, *shape.rectangle, {}});
                continue;
            }
            auto minX = std::numeric_limits<Coordinate>::max();
            auto minY = std::numeric_limits<Coordinate>::max();
            auto maxX = std::numeric_limits<Coordinate>::lowest();
            auto maxY = std::numeric_limits<Coordinate>::lowest();
            for (const auto &point : shape.polygon) {
                minX = std::min(minX, point.x);
                minY = std::min(minY, point.y);
                maxX = std::max(maxX, point.x);
                maxY = std::max(maxY, point.y);
            }
            cell.shapes.emplace(id, Shape{id, layerIdFor(shape.layer), std::nullopt,
                                          Rect{minX, minY, maxX - minX, maxY - minY}, shape.polygon});
        }
        for (const auto &port : generated.ports) {
            const auto id = project.nextId();
            cell.ports.emplace(id, Port{id, port.name, layerIdFor(port.layer), port.rectangle});
        }
        for (const auto &instance : generated.instances) {
            const auto id = project.nextId();
            cell.instances.emplace(id, LayoutInstance{id, cellsByName.at(instance.cell), instance.name, instance.x,
                                                      instance.y, instance.orientation});
        }
    } catch (const std::exception &exception) {
        project = std::move(backup);
        if (error)
            *error =
                QStringLiteral("Unable to apply the generated layout: %1").arg(QString::fromUtf8(exception.what()));
        return false;
    }
    return true;
}

} // namespace

std::string defaultProceduralLayoutScript() {
    return R"(# Procedural layout script.
# ICE runs this script with a `layout` builder object injected into the module.
# Coordinates are integer database units (one nanometer by default).
#
# layer = layout.layer("Metal1", 1)
# layer.rectangle(x, y, width, height)
# layer.polygon([(x, y), (x, y), (x, y)])
# layout.port("P1", "Metal1", x, y, width, height)
# layout.instance("other_layout_cell", x, y, name="I1", orientation="r0")

metal1 = layout.layer("Metal1", 1)
metal2 = layout.layer("Metal2", 2)

metal1.rectangle(0, 0, 4000, 600)
metal1.rectangle(0, 2400, 4000, 600)
metal2.rectangle(1000, 1500, 2000, 600)
)";
}

bool regenerateProceduralLayout(Project &project, ObjectId proceduralLayoutId, QString *error) {
    if (error)
        error->clear();
    const auto proceduralIt = project.proceduralLayouts.find(proceduralLayoutId);
    if (proceduralIt == project.proceduralLayouts.end()) {
        if (error)
            *error = QStringLiteral("The procedural layout does not exist.");
        return false;
    }
    const auto runner = proceduralRunnerScriptPath();
    if (runner.empty()) {
        if (error)
            *error = QStringLiteral("The procedural layout runner script was not found.");
        return false;
    }
    const auto python = pythonExecutable();
    if (python.isEmpty()) {
        if (error)
            *error = QStringLiteral("Python 3 was not found. Set ICE_PYTHON to the interpreter path.");
        return false;
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        if (error)
            *error = QStringLiteral("Unable to create a temporary directory for procedural layout generation.");
        return false;
    }
    const auto scriptPath = temporary.filePath(QStringLiteral("layout.py"));
    const auto outputPath = temporary.filePath(QStringLiteral("layout.json"));
    {
        QFile scriptFile(scriptPath);
        if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Unable to write the procedural layout script.");
            return false;
        }
        scriptFile.write(proceduralIt->second.script.data(), static_cast<qint64>(proceduralIt->second.script.size()));
    }

    QProcess process;
    process.setProgram(python);
    process.setArguments({QString::fromStdString(runner.string()), QStringLiteral("--script"), scriptPath,
                          QStringLiteral("--output"), outputPath});
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Unable to start Python: %1").arg(process.errorString());
        return false;
    }
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("The procedural layout script timed out.");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        auto details = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (details.isEmpty())
            details = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        if (details.size() > 4000)
            details = details.right(4000);
        if (error)
            *error =
                details.isEmpty()
                    ? QStringLiteral("The procedural layout script failed with exit code %1.").arg(process.exitCode())
                    : QStringLiteral("The procedural layout script failed:\n%1").arg(details);
        return false;
    }

    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("The procedural layout script did not produce an output file.");
        return false;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(outputFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Unable to parse the procedural layout output: %1").arg(parseError.errorString());
        return false;
    }
    ParsedLayout generated;
    if (!parseGeneratedLayout(document.object(), generated, error))
        return false;
    return applyGeneratedLayout(project, proceduralLayoutId, proceduralIt->second, generated, error);
}

} // namespace ice

ProceduralLayoutEditor::ProceduralLayoutEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);

    auto *titleRow = new QHBoxLayout;
    titleRow->addWidget(heading(QStringLiteral("Python script")), 1);
    regenerateButton_ = new QPushButton(QStringLiteral("Regenerate layout"));
    setLucideIcon(regenerateButton_, QStringLiteral("refresh-cw"), 16);
    titleRow->addWidget(regenerateButton_);
    layout->addLayout(titleRow);

    auto *hint = new QLabel(
        QStringLiteral("This procedural layout is generated from the Python script below. Right-click the "
                       "procedural layout in Hierarchy and choose Regenerate layout (or use the button above) "
                       "to rebuild its static layout child."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    layout->addWidget(hint);

    editor_ = new QPlainTextEdit;
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(std::max(font.pointSizeF(), 10.0));
    editor_->setFont(font);
    editor_->setTabStopDistance(editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    editor_->setAccessibleName(QStringLiteral("Procedural layout Python script"));
    editor_->setPlaceholderText(QStringLiteral("Write Python that builds the layout through the `layout` object."));
    layout->addWidget(editor_, 1);

    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (onScriptEdited_)
            onScriptEdited_(editor_->toPlainText());
    });
    connect(regenerateButton_, &QPushButton::clicked, this, [this] {
        if (onRegenerate_)
            onRegenerate_();
    });
}

void ProceduralLayoutEditor::setScript(const QString &script) {
    const QSignalBlocker blocker(editor_);
    editor_->setPlainText(script);
}

QString ProceduralLayoutEditor::script() const { return editor_->toPlainText(); }

void ProceduralLayoutEditor::setOnScriptEdited(std::function<void(const QString &)> callback) {
    onScriptEdited_ = std::move(callback);
}

void ProceduralLayoutEditor::setOnRegenerate(std::function<void()> callback) { onRegenerate_ = std::move(callback); }

void ProceduralLayoutEditor::showMessage(const QString &message, bool error) {
    status_->setText(message);
    status_->setStyleSheet(error ? QStringLiteral("color: #d9534f; font-weight: bold;")
                                 : QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
}
