#include "implementation_ui.hpp"

#include "icelib/hdl.hpp"
#include "icelib/interchange.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "icons.hpp"
#include "layout_canvas.hpp"
#include "layout_render.hpp"
#include "text_document_editor.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSvgItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace {

QString stageTitle(ice::ImplementationStage stage) {
    switch (stage) {
    case ice::ImplementationStage::created:
        return QStringLiteral("HDL");
    case ice::ImplementationStage::elaboration:
        return QStringLiteral("Elaboration");
    case ice::ImplementationStage::synthesis:
        return QStringLiteral("Synthesis");
    case ice::ImplementationStage::floorplan:
        return QStringLiteral("Floorplan");
    case ice::ImplementationStage::placement:
        return QStringLiteral("Placement");
    case ice::ImplementationStage::cts:
        return QStringLiteral("CTS");
    case ice::ImplementationStage::routing:
        return QStringLiteral("Routing");
    case ice::ImplementationStage::final:
        return QStringLiteral("Final");
    }
    return QStringLiteral("Implementation");
}

QString statusText(ice::ImplementationStageStatus status) {
    switch (status) {
    case ice::ImplementationStageStatus::pending:
        return QStringLiteral("Pending");
    case ice::ImplementationStageStatus::running:
        return QStringLiteral("Running");
    case ice::ImplementationStageStatus::succeeded:
        return QStringLiteral("Succeeded");
    case ice::ImplementationStageStatus::failed:
        return QStringLiteral("Failed");
    case ice::ImplementationStageStatus::skipped:
        return QStringLiteral("Skipped");
    case ice::ImplementationStageStatus::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Pending");
}

QString statusColor(ice::ImplementationStageStatus status) {
    switch (status) {
    case ice::ImplementationStageStatus::running:
        return QStringLiteral("#2563eb");
    case ice::ImplementationStageStatus::succeeded:
        return QStringLiteral("#15803d");
    case ice::ImplementationStageStatus::failed:
        return QStringLiteral("#b91c1c");
    case ice::ImplementationStageStatus::cancelled:
        return QStringLiteral("#a16207");
    case ice::ImplementationStageStatus::skipped:
        return QStringLiteral("#6b7280");
    case ice::ImplementationStageStatus::pending:
        return QStringLiteral("#4b5563");
    }
    return QStringLiteral("#4b5563");
}

QString runStatusText(ice::ImplementationStatus status) {
    switch (status) {
    case ice::ImplementationStatus::pending:
        return QStringLiteral("Pending");
    case ice::ImplementationStatus::running:
        return QStringLiteral("Running");
    case ice::ImplementationStatus::succeeded:
        return QStringLiteral("Succeeded");
    case ice::ImplementationStatus::failed:
        return QStringLiteral("Failed");
    case ice::ImplementationStatus::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Pending");
}

QString runStatusColor(ice::ImplementationStatus status) {
    switch (status) {
    case ice::ImplementationStatus::running:
        return QStringLiteral("#2563eb");
    case ice::ImplementationStatus::succeeded:
        return QStringLiteral("#15803d");
    case ice::ImplementationStatus::failed:
        return QStringLiteral("#b91c1c");
    case ice::ImplementationStatus::cancelled:
        return QStringLiteral("#a16207");
    case ice::ImplementationStatus::pending:
        return QStringLiteral("#4b5563");
    }
    return QStringLiteral("#4b5563");
}

namespace {

QString stageSummaryText(const ice::ImplementationRun *run, ice::ImplementationStage stage) {
    if (!run)
        return {};
    const auto metric = [&](const char *name) -> std::optional<double> {
        const auto found = run->metrics.find(name);
        return found == run->metrics.end() ? std::nullopt : std::optional<double>(found->second);
    };
    QStringList parts;
    if (const auto instances = metric("design__instance__count"))
        parts << QStringLiteral("%1 instances").arg(static_cast<qulonglong>(std::max(0.0, *instances)));
    if (stage == ice::ImplementationStage::floorplan) {
        if (const auto die = metric("design__die__area"))
            parts << QStringLiteral("die %1 µm²").arg(*die, 0, 'f', 0);
        if (const auto core = metric("design__core__area"))
            parts << QStringLiteral("core %1 µm²").arg(*core, 0, 'f', 0);
    } else if (stage == ice::ImplementationStage::placement || stage == ice::ImplementationStage::cts) {
        if (const auto wns = metric("timing__setup__ws")) {
            if (std::abs(*wns) > 1.0e20)
                parts << QStringLiteral("timing unconstrained");
            else
                parts << QStringLiteral("setup WNS %1 ns").arg(*wns * 1.0e9, 0, 'f', 3);
        }
    } else if (stage == ice::ImplementationStage::routing || stage == ice::ImplementationStage::final) {
        if (const auto drc = metric("route__drc_errors"))
            parts << QStringLiteral("%1 DRC").arg(static_cast<qulonglong>(std::max(0.0, *drc)));
        if (const auto wirelength = metric("route__wirelength"))
            parts << QStringLiteral("%1 µm wirelength").arg(*wirelength, 0, 'f', 0);
        if (const auto vias = metric("route__vias"))
            parts << QStringLiteral("%1 vias").arg(static_cast<qulonglong>(std::max(0.0, *vias)));
        if (const auto antenna = metric("antenna__violating__nets"))
            parts << QStringLiteral("%1 antenna nets").arg(static_cast<qulonglong>(std::max(0.0, *antenna)));
        if (stage == ice::ImplementationStage::final) {
            if (const auto wns = metric("timing__setup__ws"); wns && std::abs(*wns) <= 1.0e20)
                parts << QStringLiteral("setup WNS %1 ns").arg(*wns * 1.0e9, 0, 'f', 3);
            if (const auto power = metric("power__total"); power && *power >= 0.0)
                parts << QStringLiteral("power %1 mW").arg(*power * 1.0e3, 0, 'f', 2);
        }
    }
    if (parts.isEmpty())
        return {};
    return QStringLiteral("%1 succeeded · %2").arg(stageTitle(stage), parts.join(QStringLiteral(" · ")));
}

} // namespace

std::string stageDefPath(ice::ImplementationStage stage) {
    switch (stage) {
    case ice::ImplementationStage::floorplan:
        return "def/2_floorplan.def";
    case ice::ImplementationStage::placement:
        return "def/3_place.def";
    case ice::ImplementationStage::cts:
        return "def/4_cts.def";
    case ice::ImplementationStage::routing:
        return "def/5_route.def";
    case ice::ImplementationStage::final:
        return "def/6_final.def";
    default:
        return {};
    }
}

std::string stageSdcPath(ice::ImplementationStage stage) {
    switch (stage) {
    case ice::ImplementationStage::floorplan:
        return "final/2_floorplan.sdc";
    case ice::ImplementationStage::placement:
        return "final/3_place.sdc";
    case ice::ImplementationStage::cts:
        return "final/4_cts.sdc";
    case ice::ImplementationStage::routing:
        return "final/5_route.sdc";
    case ice::ImplementationStage::final:
        return "final/6_final.sdc";
    default:
        return {};
    }
}

std::string stageOdbPath(ice::ImplementationStage stage) {
    switch (stage) {
    case ice::ImplementationStage::floorplan:
        return "db/2_floorplan.odb";
    case ice::ImplementationStage::placement:
        return "db/3_place.odb";
    case ice::ImplementationStage::cts:
        return "db/4_cts.odb";
    case ice::ImplementationStage::routing:
        return "db/5_route.odb";
    case ice::ImplementationStage::final:
        return "db/6_final.odb";
    default:
        return {};
    }
}

constexpr double kMicronToProjectUnit = 1000.0;

QString lowerName(QString name) { return name.toLower(); }

ice::ObjectId previewLayerId(const ice::Project *project, const QString &name, int number) {
    if (!project)
        return 0;
    for (const auto &[id, layer] : project->layers) {
        if (number > 0 && layer.number == number)
            return id;
        if (!name.isEmpty() && lowerName(QString::fromStdString(layer.name)) == lowerName(name))
            return id;
    }
    return 0;
}

ice::LayoutPreviewLayer parsePreviewLayer(const QJsonObject &object, const ice::Project *project) {
    ice::LayoutPreviewLayer layer;
    const auto name = object.value(QStringLiteral("layer")).toString();
    const auto number = object.value(QStringLiteral("layer_number")).toInt();
    layer.number = number;
    layer.datatype = object.value(QStringLiteral("layer_datatype")).toInt();
    layer.name = name;
    layer.id = previewLayerId(project, name, number);
    return layer;
}

ice::Rect parsePreviewRect(const QJsonObject &object) {
    ice::Rect rect;
    rect.x = object.value(QStringLiteral("x")).toDouble() * kMicronToProjectUnit;
    rect.y = object.value(QStringLiteral("y")).toDouble() * kMicronToProjectUnit;
    rect.width = object.value(QStringLiteral("width")).toDouble() * kMicronToProjectUnit;
    rect.height = object.value(QStringLiteral("height")).toDouble() * kMicronToProjectUnit;
    return rect;
}

std::optional<ice::LayoutPreviewHeatMap> parsePreviewHeatMap(const QJsonValue &value) {
    if (!value.isObject())
        return std::nullopt;
    const auto object = value.toObject();
    ice::LayoutPreviewHeatMap heatmap;
    const auto bounds = object.value(QStringLiteral("bounds")).toObject();
    const auto rect = parsePreviewRect(bounds);
    heatmap.bounds = QRectF(rect.x, rect.y, rect.width, rect.height);
    heatmap.columns = object.value(QStringLiteral("columns")).toInt();
    heatmap.rows = object.value(QStringLiteral("rows")).toInt();
    heatmap.minimum = object.value(QStringLiteral("minimum")).toDouble();
    heatmap.maximum = object.value(QStringLiteral("maximum")).toDouble();
    const auto values = object.value(QStringLiteral("values")).toArray();
    heatmap.values.reserve(static_cast<std::size_t>(values.size()));
    for (const auto &entry : values)
        heatmap.values.push_back(entry.toDouble());
    if (heatmap.columns <= 0 || heatmap.rows <= 0 ||
        heatmap.values.size() != static_cast<std::size_t>(heatmap.columns) * static_cast<std::size_t>(heatmap.rows))
        return std::nullopt;
    return heatmap;
}

std::optional<ice::LayoutPreviewHeatMap> parseHeatMapCsv(const std::filesystem::path &path) {
    std::ifstream input(path);
    if (!input.is_open())
        return std::nullopt;
    struct Bin {
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        double value = 0.0;
    };
    std::vector<Bin> bins;
    std::set<double> xs;
    std::set<double> ys;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == 'x')
            continue;
        std::stringstream stream(line);
        std::string field;
        std::vector<double> values;
        while (std::getline(stream, field, ',')) {
            try {
                values.push_back(std::stod(field));
            } catch (const std::exception &) {
                values.clear();
                break;
            }
        }
        if (values.size() < 5)
            continue;
        bins.push_back({values[0], values[1], values[2], values[3], values[4]});
        xs.insert(values[0]);
        ys.insert(values[1]);
    }
    if (bins.empty() || xs.empty() || ys.empty())
        return std::nullopt;
    ice::LayoutPreviewHeatMap heatmap;
    heatmap.columns = static_cast<int>(xs.size());
    heatmap.rows = static_cast<int>(ys.size());
    const auto x0 = *xs.begin();
    const auto y0 = *ys.begin();
    double x1 = x0;
    double y1 = y0;
    for (const auto &bin : bins) {
        x1 = std::max(x1, bin.x1);
        y1 = std::max(y1, bin.y1);
    }
    heatmap.bounds = QRectF(x0 * kMicronToProjectUnit, y0 * kMicronToProjectUnit, (x1 - x0) * kMicronToProjectUnit,
                            (y1 - y0) * kMicronToProjectUnit);
    heatmap.values.assign(static_cast<std::size_t>(heatmap.columns) * heatmap.rows, 0.0);
    heatmap.minimum = std::numeric_limits<double>::max();
    heatmap.maximum = std::numeric_limits<double>::lowest();
    const std::vector<double> xValues(xs.begin(), xs.end());
    const std::vector<double> yValues(ys.begin(), ys.end());
    for (const auto &bin : bins) {
        const auto column =
            static_cast<int>(std::lower_bound(xValues.begin(), xValues.end(), bin.x0) - xValues.begin());
        const auto row = static_cast<int>(std::lower_bound(yValues.begin(), yValues.end(), bin.y0) - yValues.begin());
        if (column < 0 || column >= heatmap.columns || row < 0 || row >= heatmap.rows)
            continue;
        const auto index = static_cast<std::size_t>(row) * heatmap.columns + column;
        heatmap.values[index] = bin.value;
        heatmap.minimum = std::min(heatmap.minimum, bin.value);
        heatmap.maximum = std::max(heatmap.maximum, bin.value);
    }
    if (heatmap.maximum <= heatmap.minimum)
        return std::nullopt;
    return heatmap;
}

std::vector<ice::LayoutPreviewMarker> parseDrcMarkers(const std::filesystem::path &path) {
    std::vector<ice::LayoutPreviewMarker> markers;
    std::ifstream input(path);
    if (!input.is_open())
        return markers;
    const std::regex violationType(R"(^\s*violation type:\s*(.+)$)");
    const std::regex sources(R"(^\s*srcs:\s*(.+)$)");
    const std::regex comment(R"(^\s*comment:\s*(.+)$)");
    const std::regex bboxLayer(
        R"(^\s*bbox =\s*\(\s*([^,]+),\s*([^\)]+)\)\s*-\s*\(\s*([^,]+),\s*([^\)]+)\)\s*on Layer\s*(.+)$)");
    std::string line;
    std::string currentType;
    std::string currentSources;
    std::string currentComment;
    while (std::getline(input, line)) {
        std::smatch match;
        if (std::regex_match(line, match, violationType)) {
            currentType = match[1].str();
            currentSources.clear();
            currentComment.clear();
            continue;
        }
        if (std::regex_match(line, match, sources)) {
            currentSources = match[1].str();
            continue;
        }
        if (std::regex_match(line, match, comment)) {
            currentComment = match[1].str();
            continue;
        }
        if (!std::regex_match(line, match, bboxLayer))
            continue;
        try {
            const auto scale = kMicronToProjectUnit;
            const auto x0 = std::stod(match[1].str()) * scale;
            const auto y0 = std::stod(match[2].str()) * scale;
            const auto x1 = std::stod(match[3].str()) * scale;
            const auto y1 = std::stod(match[4].str()) * scale;
            ice::LayoutPreviewMarker marker;
            marker.type = QString::fromStdString(currentType);
            marker.layer = QString::fromStdString(match[5].str());
            marker.rectangle = QRectF(QPointF(x0, y0), QPointF(x1, y1)).normalized();
            marker.sources = QString::fromStdString(currentSources);
            marker.comment = QString::fromStdString(currentComment);
            markers.push_back(std::move(marker));
        } catch (const std::exception &) {
        }
    }
    return markers;
}

std::shared_ptr<ice::LayoutPreviewModel> parseOdbPreview(const QByteArray &json, const ice::Project *project,
                                                         QString &error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (document.isNull() || !document.isObject()) {
        error = QStringLiteral("OpenDB preview returned invalid JSON: %1").arg(parseError.errorString());
        return nullptr;
    }
    const auto root = document.object();
    auto model = std::make_shared<ice::LayoutPreviewModel>();
    model->truncated = root.value(QStringLiteral("truncated")).toBool();
    const auto bounds = parsePreviewRect(root.value(QStringLiteral("bounds")).toObject());
    model->bounds = QRectF(bounds.x, bounds.y, bounds.width, bounds.height);
    bool validBounds = model->bounds.isValid() && model->bounds.width() > 0.0 && model->bounds.height() > 0.0;

    for (const auto &entry : root.value(QStringLiteral("shapes")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewShape shape;
        shape.layer = parsePreviewLayer(object, project);
        const auto kind = object.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("text"))
            shape.kind = ice::LayoutPreviewShape::Kind::text;
        else if (kind == QStringLiteral("path"))
            shape.kind = ice::LayoutPreviewShape::Kind::path;
        else
            shape.kind = ice::LayoutPreviewShape::Kind::polygon;
        shape.width = object.value(QStringLiteral("width")).toDouble();
        shape.text = object.value(QStringLiteral("text")).toString();
        shape.net = object.value(QStringLiteral("net")).toString();
        shape.netId = object.value(QStringLiteral("net_id")).toInt();
        shape.special = object.value(QStringLiteral("special")).toBool();
        for (const auto &pointValue : object.value(QStringLiteral("points")).toArray()) {
            const auto point = pointValue.toArray();
            if (point.size() < 2)
                continue;
            const QPointF mapped(point[0].toDouble() * kMicronToProjectUnit,
                                 point[1].toDouble() * kMicronToProjectUnit);
            if (shape.kind == ice::LayoutPreviewShape::Kind::path)
                shape.centerline.push_back(mapped);
            else
                shape.polygon << mapped;
        }
        if (shape.kind == ice::LayoutPreviewShape::Kind::polygon && shape.polygon.size() >= 3) {
            model->shapes.push_back(std::move(shape));
            if (!validBounds)
                model->bounds = model->bounds.united(shape.polygon.boundingRect());
        } else if (shape.kind == ice::LayoutPreviewShape::Kind::path && shape.centerline.size() >= 2) {
            model->shapes.push_back(std::move(shape));
        }
    }

    for (const auto &entry : root.value(QStringLiteral("ports")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewPort port;
        port.layer = parsePreviewLayer(object, project);
        const auto rectangle = parsePreviewRect(object.value(QStringLiteral("rectangle")).toObject());
        port.rectangle = QRectF(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
        port.label = object.value(QStringLiteral("name")).toString();
        port.net = object.value(QStringLiteral("net")).toString();
        port.netId = object.value(QStringLiteral("net_id")).toInt();
        if (port.rectangle.isValid()) {
            model->ports.push_back(std::move(port));
            if (!validBounds)
                model->bounds = model->bounds.united(model->ports.back().rectangle);
        }
    }

    for (const auto &entry : root.value(QStringLiteral("instances")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewPlannedDevice device;
        const auto rectangle = parsePreviewRect(object.value(QStringLiteral("rectangle")).toObject());
        device.rectangle = QRectF(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
        device.name = object.value(QStringLiteral("name")).toString();
        device.id = object.value(QStringLiteral("id")).toInt();
        device.master = object.value(QStringLiteral("master")).toString();
        device.category = object.value(QStringLiteral("category")).toString();
        device.module = object.value(QStringLiteral("module")).toString();
        device.orientation = object.value(QStringLiteral("orientation")).toString();
        if (device.rectangle.isValid()) {
            model->plannedDevices.push_back(std::move(device));
            if (!validBounds)
                model->bounds = model->bounds.united(model->plannedDevices.back().rectangle);
        }
    }

    for (const auto &entry : root.value(QStringLiteral("nets")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewNet net;
        net.id = object.value(QStringLiteral("id")).toInt();
        net.name = object.value(QStringLiteral("name")).toString();
        net.type = object.value(QStringLiteral("type")).toString();
        net.special = object.value(QStringLiteral("special")).toBool();
        net.termCount = static_cast<quint64>(object.value(QStringLiteral("term_count")).toDouble());
        for (const auto &term : object.value(QStringLiteral("terms")).toArray())
            net.terms.push_back(term.toString());
        model->nets.push_back(std::move(net));
    }

    for (const auto &entry : root.value(QStringLiteral("modules")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewModule module;
        module.id = object.value(QStringLiteral("id")).toInt();
        module.name = object.value(QStringLiteral("name")).toString();
        module.parentId = object.value(QStringLiteral("parent_id")).toInt();
        module.instanceCount = static_cast<quint64>(object.value(QStringLiteral("instance_count")).toDouble());
        model->modules.push_back(std::move(module));
    }

    for (const auto &entry : root.value(QStringLiteral("guides")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewGuide guide;
        guide.net = object.value(QStringLiteral("net")).toString();
        guide.layer = object.value(QStringLiteral("layer")).toString();
        guide.congested = object.value(QStringLiteral("congested")).toBool();
        const auto rectangle = parsePreviewRect(object.value(QStringLiteral("rectangle")).toObject());
        guide.rectangle = QRectF(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
        if (guide.rectangle.isValid())
            model->guides.push_back(std::move(guide));
    }
    for (const auto &entry : root.value(QStringLiteral("rows")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewRow row;
        row.site = object.value(QStringLiteral("site")).toString();
        const auto rectangle = parsePreviewRect(object.value(QStringLiteral("rectangle")).toObject());
        row.rectangle = QRectF(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
        if (row.rectangle.isValid())
            model->rows.push_back(std::move(row));
    }
    for (const auto &entry : root.value(QStringLiteral("blockages")).toArray()) {
        const auto object = entry.toObject();
        ice::LayoutPreviewBlockage blockage;
        blockage.type = object.value(QStringLiteral("type")).toString();
        const auto rectangle = parsePreviewRect(object.value(QStringLiteral("rectangle")).toObject());
        blockage.rectangle = QRectF(rectangle.x, rectangle.y, rectangle.width, rectangle.height);
        if (blockage.rectangle.isValid())
            model->blockages.push_back(std::move(blockage));
    }

    model->placementDensity = parsePreviewHeatMap(root.value(QStringLiteral("placement_density")));
    model->routingCongestion = parsePreviewHeatMap(root.value(QStringLiteral("routing_congestion")));
    if (model->empty() && model->plannedDevices.empty() && model->ports.empty()) {
        error = QStringLiteral("OpenDB preview contains no geometry.");
        return nullptr;
    }
    return model;
}

} // namespace

// Read-only, zoomable canvas used by the physical phase tabs. It renders
// either an ODB-backed preview model or the legacy DEF import fallback.
class ImplementationPreviewCanvas : public QGraphicsView {
  public:
    struct LayerEntry {
        QString key;
        QString label;
        int number = 0;
        QColor color;
        bool visible = true;
    };

    enum class Tool { none, ruler, label };
    enum class SelectionKind { none, instance, port, shape, marker };

    struct Selection {
        SelectionKind kind = SelectionKind::none;
        int index = -1;
        QString name;
        QString master;
        QString category;
        QString module;
        QString net;
        QString layer;
        QString type;
        QString sources;
        QString comment;
        int netId = 0;
        bool special = false;
        QRectF rectangle;
        QPolygonF polygon;
        std::vector<QPointF> centerline;
        qreal width = 0.0;

        bool valid() const { return kind != SelectionKind::none; }
    };

    struct HeatMapOptions {
        bool logScale = false;
        bool reverse = false;
        bool showLegend = true;
        int alpha = 140;
        std::optional<double> minimum;
        std::optional<double> maximum;
    };

    explicit ImplementationPreviewCanvas(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setRenderHint(QPainter::Antialiasing, true);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setBackgroundBrush(palette().color(QPalette::Base));
    }

    void setPreview(const std::shared_ptr<ice::Project> &project, ice::ObjectId cell, const QString &message,
                    const std::shared_ptr<ice::LayoutPreviewModel> &model = {},
                    const ice::Project *styleProject = nullptr) {
        project_ = project;
        cell_ = cell;
        message_ = message;
        model_ = model;
        styleProject_ = styleProject ? styleProject : project.get();
        rebuild();
    }

    void setHeatMap(ice::LayoutHeatMapKind kind) {
        customHeatMap_.reset();
        if (heatMapKind_ == kind)
            return;
        heatMapKind_ = kind;
        // Overlay changes should not discard the user's zoom and pan.
        rebuild(false);
    }

    void setCustomHeatMap(std::optional<ice::LayoutPreviewHeatMap> heatmap) {
        customHeatMap_ = std::move(heatmap);
        heatMapKind_ = ice::LayoutHeatMapKind::none;
        rebuild(false);
    }
    bool hasCustomHeatMap() const { return customHeatMap_.has_value(); }

    std::vector<LayerEntry> layers() const { return layers_; }

    void setLayerVisible(const QString &key, bool visible) {
        if (key.isEmpty())
            return;
        if (visible)
            hiddenLayers_.erase(key);
        else
            hiddenLayers_.insert(key);
        rebuild(false);
    }

    void setInstanceCategoryVisible(const QString &category, bool visible) {
        if (category.isEmpty())
            return;
        if (visible)
            hiddenInstanceCategories_.erase(category);
        else
            hiddenInstanceCategories_.insert(category);
        rebuild(false);
    }

    void setNetTypeVisible(const QString &type, bool visible) {
        if (type.isEmpty())
            return;
        if (visible)
            hiddenNetTypes_.erase(type);
        else
            hiddenNetTypes_.insert(type);
        rebuild(false);
    }

    void setShowSpecialNets(bool visible) {
        if (showSpecialNets_ == visible)
            return;
        showSpecialNets_ = visible;
        rebuild(false);
    }

    void setShowPorts(bool visible) {
        if (showPorts_ == visible)
            return;
        showPorts_ = visible;
        rebuild(false);
    }

    void setShowGuides(bool visible) {
        if (showGuides_ == visible)
            return;
        showGuides_ = visible;
        rebuild(false);
    }
    void setShowRows(bool visible) {
        if (showRows_ == visible)
            return;
        showRows_ = visible;
        rebuild(false);
    }
    void setShowBlockages(bool visible) {
        if (showBlockages_ == visible)
            return;
        showBlockages_ = visible;
        rebuild(false);
    }

    void setHeatMapOptions(const HeatMapOptions &options) {
        heatMapOptions_ = options;
        rebuild(false);
    }
    HeatMapOptions heatMapOptions() const { return heatMapOptions_; }

    void setSelectionCallback(std::function<void(const Selection &)> callback) {
        selectionCallback_ = std::move(callback);
    }
    Selection selection() const { return selection_; }

    void clearSelection() {
        if (!selection_.valid())
            return;
        selection_ = {};
        if (selectionCallback_)
            selectionCallback_(selection_);
        rebuild(false);
    }

    void setFocusNet(const QString &net) {
        if (focusNet_ == net)
            return;
        focusNet_ = net;
        rebuild(false);
    }
    QString focusNet() const { return focusNet_; }
    void clearFocusNet() { setFocusNet({}); }

    void selectMarker(int index) {
        if (!activeModel_ || index < 0 || index >= static_cast<int>(activeModel_->markers.size()))
            return;
        selection_ = selectionForMarker(*activeModel_, index);
        if (selectionCallback_)
            selectionCallback_(selection_);
        rebuild(false);
        zoomToSelection();
    }

    bool selectByName(const QString &name) {
        if (!activeModel_ || name.isEmpty())
            return false;
        for (int index = 0; index < static_cast<int>(activeModel_->plannedDevices.size()); ++index) {
            const auto &device = activeModel_->plannedDevices[static_cast<std::size_t>(index)];
            if (device.name != name)
                continue;
            selection_ = {};
            selection_.kind = SelectionKind::instance;
            selection_.index = index;
            selection_.name = device.name;
            selection_.master = device.master;
            selection_.category = device.category.isEmpty() ? QStringLiteral("other") : device.category;
            selection_.module = device.module;
            selection_.rectangle = device.rectangle;
            if (selectionCallback_)
                selectionCallback_(selection_);
            rebuild(false);
            zoomToSelection();
            return true;
        }
        for (int index = 0; index < static_cast<int>(activeModel_->ports.size()); ++index) {
            const auto &port = activeModel_->ports[static_cast<std::size_t>(index)];
            if (port.label != name && port.net != name)
                continue;
            selection_ = {};
            selection_.kind = SelectionKind::port;
            selection_.index = index;
            selection_.name = port.label;
            selection_.net = port.net;
            selection_.netId = port.netId;
            selection_.layer = port.layer.name;
            selection_.rectangle = port.rectangle;
            if (selectionCallback_)
                selectionCallback_(selection_);
            rebuild(false);
            zoomToSelection();
            return true;
        }
        for (int index = 0; index < static_cast<int>(activeModel_->shapes.size()); ++index) {
            const auto &shape = activeModel_->shapes[static_cast<std::size_t>(index)];
            if (shape.net != name)
                continue;
            selection_ = {};
            selection_.kind = SelectionKind::shape;
            selection_.index = index;
            selection_.name = shape.net;
            selection_.net = shape.net;
            selection_.netId = shape.netId;
            selection_.special = shape.special;
            selection_.layer = shape.layer.name;
            selection_.polygon = shape.polygon;
            selection_.centerline = shape.centerline;
            selection_.width = shape.width;
            if (selectionCallback_)
                selectionCallback_(selection_);
            rebuild(false);
            zoomToSelection();
            return true;
        }
        return false;
    }

    std::vector<Selection> markers() const { return markerSelections_; }

    void setTool(Tool tool) { tool_ = tool; }
    Tool tool() const { return tool_; }

    void clearAnnotations() {
        rulers_.clear();
        labels_.clear();
        rebuild(false);
    }

    void setStatusCallback(std::function<void(const QPointF &, const QString &)> callback) {
        statusCallback_ = std::move(callback);
    }

    void fitView() {
        fitOnResize_ = true;
        fitToScene();
    }

    void zoomIn() {
        fitOnResize_ = false;
        scale(1.25, 1.25);
    }

    void zoomOut() {
        fitOnResize_ = false;
        scale(1.0 / 1.25, 1.0 / 1.25);
    }

    void zoomToSelection() {
        const auto rect = selectionSceneRect();
        if (!rect.isValid())
            return;
        fitOnResize_ = false;
        fitInView(rect.adjusted(-20.0, -20.0, 20.0, 20.0), Qt::KeepAspectRatio);
    }

    void saveImage(const QString &path) {
        if (path.isEmpty())
            return;
        grab().save(path);
    }

    std::map<QString, int> instanceCategories() const {
        std::map<QString, int> counts;
        if (!activeModel_)
            return counts;
        for (const auto &device : activeModel_->plannedDevices) {
            const auto category = device.category.isEmpty() ? QStringLiteral("other") : device.category;
            ++counts[category];
        }
        return counts;
    }

    std::map<QString, int> netTypes() const {
        std::map<QString, int> counts;
        if (!activeModel_)
            return counts;
        for (const auto &net : activeModel_->nets) {
            const auto type = net.type.isEmpty() ? QStringLiteral("signal") : net.type.toLower();
            ++counts[type];
        }
        return counts;
    }

    std::vector<ice::LayoutPreviewNet> nets() const {
        return activeModel_ ? activeModel_->nets : std::vector<ice::LayoutPreviewNet>{};
    }

    std::vector<ice::LayoutPreviewModule> modules() const {
        return activeModel_ ? activeModel_->modules : std::vector<ice::LayoutPreviewModule>{};
    }

    std::vector<ice::LayoutPreviewMarker> drcMarkers() const {
        return activeModel_ ? activeModel_->markers : std::vector<ice::LayoutPreviewMarker>{};
    }

    bool instanceCategoryVisible(const QString &category) const {
        return hiddenInstanceCategories_.find(category) == hiddenInstanceCategories_.end();
    }
    bool netTypeVisible(const QString &type) const { return hiddenNetTypes_.find(type) == hiddenNetTypes_.end(); }
    bool showSpecialNets() const { return showSpecialNets_; }
    bool showPorts() const { return showPorts_; }
    bool showGuides() const { return showGuides_; }
    bool showRows() const { return showRows_; }
    bool showBlockages() const { return showBlockages_; }
    bool hasGuides() const { return activeModel_ && !activeModel_->guides.empty(); }
    bool hasRows() const { return activeModel_ && !activeModel_->rows.empty(); }
    bool hasBlockages() const { return activeModel_ && !activeModel_->blockages.empty(); }

    std::optional<ice::LayoutPreviewHeatMap> currentHeatMap() const {
        if (customHeatMap_)
            return customHeatMap_;
        if (!activeModel_)
            return std::nullopt;
        if (heatMapKind_ == ice::LayoutHeatMapKind::placement_density && activeModel_->placementDensity)
            return *activeModel_->placementDensity;
        if (heatMapKind_ == ice::LayoutHeatMapKind::routing_congestion && activeModel_->routingCongestion)
            return *activeModel_->routingCongestion;
        return std::nullopt;
    }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (fitOnResize_)
            fitToScene();
    }

    void wheelEvent(QWheelEvent *event) override {
        const auto factor = event->angleDelta().y() > 0 ? 1.18 : (1.0 / 1.18);
        scale(factor, factor);
        fitOnResize_ = false;
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        fitOnResize_ = true;
        fitToScene();
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            QGraphicsView::mousePressEvent(event);
            return;
        }
        const auto point = viewToModel(event->pos());
        if (tool_ == Tool::ruler) {
            rulerBuilding_ = true;
            rulerStart_ = point;
            event->accept();
            return;
        }
        if (tool_ == Tool::label) {
            bool accepted = false;
            const auto text = QInputDialog::getText(this, QStringLiteral("Add label"), QStringLiteral("Label:"),
                                                    QLineEdit::Normal, {}, &accepted);
            if (accepted && !text.isEmpty()) {
                labels_.emplace_back(point, text);
                rebuild(false);
            }
            event->accept();
            return;
        }
        selection_ = hitTest(point);
        if (selectionCallback_)
            selectionCallback_(selection_);
        rebuild(false);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        const auto point = viewToModel(event->pos());
        if (statusCallback_)
            statusCallback_(point, tool_ == Tool::ruler ? QStringLiteral("Ruler: click and drag") : QString());
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (tool_ == Tool::ruler && rulerBuilding_ && event->button() == Qt::LeftButton) {
            const auto end = viewToModel(event->pos());
            if (QLineF(rulerStart_, end).length() > 0.5)
                rulers_.emplace_back(rulerStart_, end);
            rulerBuilding_ = false;
            rebuild(false);
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        const auto point = viewToModel(event->pos());
        selection_ = hitTest(point);
        if (selectionCallback_)
            selectionCallback_(selection_);
        rebuild(false);
        QMenu menu(this);
        auto *inspect = menu.addAction(QStringLiteral("Inspect"));
        inspect->setEnabled(selection_.valid());
        auto *focus = menu.addAction(QStringLiteral("Focus net"));
        focus->setEnabled(!selection_.net.isEmpty());
        auto *clearFocus = menu.addAction(QStringLiteral("Clear focused net"));
        clearFocus->setEnabled(!focusNet_.isEmpty());
        auto *zoom = menu.addAction(QStringLiteral("Zoom to selection"));
        zoom->setEnabled(selection_.valid());
        menu.addSeparator();
        auto *clear = menu.addAction(QStringLiteral("Clear selection"));
        clear->setEnabled(selection_.valid());
        const auto *chosen = menu.exec(event->globalPos());
        if (chosen == focus)
            setFocusNet(selection_.net);
        else if (chosen == clearFocus)
            clearFocusNet();
        else if (chosen == zoom)
            zoomToSelection();
        else if (chosen == clear) {
            selection_ = {};
            if (selectionCallback_)
                selectionCallback_(selection_);
            rebuild(false);
        }
        event->accept();
    }

    void leaveEvent(QEvent *event) override {
        if (statusCallback_)
            statusCallback_({}, {});
        QGraphicsView::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *event) override {
        QGraphicsView::paintEvent(event);
        if (!heatMapOptions_.showLegend)
            return;
        const auto heatmap = currentHeatMap();
        if (!heatmap)
            return;
        const auto minimum = heatMapOptions_.minimum.value_or(heatmap->minimum);
        const auto maximum = heatMapOptions_.maximum.value_or(heatmap->maximum);
        if (maximum <= minimum)
            return;
        QPainter painter(viewport());
        constexpr int width = 180;
        constexpr int height = 12;
        const int left = viewport()->width() - width - 18;
        const int top = 18;
        for (int x = 0; x < width; ++x) {
            const auto value = minimum + (maximum - minimum) * static_cast<double>(x) / (width - 1);
            painter.fillRect(left + x, top, 1, height, heatMapColor(value, minimum, maximum));
        }
        painter.setPen(QPen(QColor(120, 120, 120), 1));
        painter.drawRect(left, top, width, height);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(left, top + height + 14, QString::number(minimum, 'g', 4));
        painter.drawText(left + width - 55, top + height + 14, QString::number(maximum, 'g', 4));
    }

  private:
    static QPointF scenePoint(QPointF point) { return QPointF(point.x(), -point.y()); }
    static QRectF sceneRect(QRectF rectangle) {
        return QRectF(rectangle.left(), -(rectangle.top() + rectangle.height()), rectangle.width(), rectangle.height());
    }
    static QPolygonF scenePolygon(const QPolygonF &polygon) {
        QPolygonF result;
        result.reserve(polygon.size());
        for (const auto &point : polygon)
            result << scenePoint(point);
        return result;
    }

    QPointF viewToModel(QPoint position) const {
        const auto scene = mapToScene(position);
        return QPointF(scene.x(), -scene.y());
    }

    static qreal distanceToSegment(const QPointF &point, const QPointF &start, const QPointF &end) {
        const QPointF segment(end.x() - start.x(), end.y() - start.y());
        const qreal lengthSquared = segment.x() * segment.x() + segment.y() * segment.y();
        if (lengthSquared <= 0.0)
            return QLineF(point, start).length();
        const QPointF delta(point.x() - start.x(), point.y() - start.y());
        const qreal t = std::clamp((delta.x() * segment.x() + delta.y() * segment.y()) / lengthSquared, 0.0, 1.0);
        return QLineF(point, start + t * segment).length();
    }

    Selection selectionForMarker(const ice::LayoutPreviewModel &model, int index) const {
        Selection selection;
        if (index < 0 || index >= static_cast<int>(model.markers.size()))
            return selection;
        const auto &marker = model.markers[static_cast<std::size_t>(index)];
        selection.kind = SelectionKind::marker;
        selection.index = index;
        selection.name = marker.type;
        selection.type = marker.type;
        selection.layer = marker.layer;
        selection.sources = marker.sources;
        selection.comment = marker.comment;
        selection.rectangle = marker.rectangle;
        return selection;
    }

    Selection hitTest(const QPointF &point) const {
        Selection result;
        if (!activeModel_)
            return result;
        const auto layerVisible = [&](const ice::LayoutPreviewLayer &layer) {
            return hiddenLayers_.find(ice::layoutPreviewLayerKey(layer)) == hiddenLayers_.end();
        };
        const auto netVisible = [&](int net_id, bool special) {
            if (special && !showSpecialNets_)
                return false;
            for (const auto &net : activeModel_->nets) {
                if (net.id != net_id)
                    continue;
                return hiddenNetTypes_.find(net.type.toLower()) == hiddenNetTypes_.end();
            }
            return true;
        };

        for (int index = static_cast<int>(activeModel_->markers.size()) - 1; index >= 0; --index) {
            if (activeModel_->markers[static_cast<std::size_t>(index)].rectangle.contains(point))
                return selectionForMarker(*activeModel_, index);
        }

        qreal bestArea = std::numeric_limits<qreal>::max();
        for (int index = 0; index < static_cast<int>(activeModel_->ports.size()); ++index) {
            const auto &port = activeModel_->ports[static_cast<std::size_t>(index)];
            if (!layerVisible(port.layer) || !netVisible(port.netId, false) || !port.rectangle.contains(point))
                continue;
            const auto area = port.rectangle.width() * port.rectangle.height();
            if (area > bestArea)
                continue;
            bestArea = area;
            result.kind = SelectionKind::port;
            result.index = index;
            result.name = port.label;
            result.net = port.net;
            result.netId = port.netId;
            result.layer = port.layer.name;
            result.rectangle = port.rectangle;
        }

        bestArea = std::numeric_limits<qreal>::max();
        for (int index = 0; index < static_cast<int>(activeModel_->plannedDevices.size()); ++index) {
            const auto &device = activeModel_->plannedDevices[static_cast<std::size_t>(index)];
            const auto category = device.category.isEmpty() ? QStringLiteral("other") : device.category;
            if (hiddenInstanceCategories_.find(category) != hiddenInstanceCategories_.end() ||
                !device.rectangle.contains(point))
                continue;
            const auto area = device.rectangle.width() * device.rectangle.height();
            if (area > bestArea)
                continue;
            bestArea = area;
            result.kind = SelectionKind::instance;
            result.index = index;
            result.name = device.name;
            result.master = device.master;
            result.category = category;
            result.module = device.module;
            result.rectangle = device.rectangle;
        }

        qreal bestDistance = std::numeric_limits<qreal>::max();
        for (int index = 0; index < static_cast<int>(activeModel_->shapes.size()); ++index) {
            const auto &shape = activeModel_->shapes[static_cast<std::size_t>(index)];
            if (!layerVisible(shape.layer) || !netVisible(shape.netId, shape.special))
                continue;
            if (shape.kind == ice::LayoutPreviewShape::Kind::polygon) {
                if (shape.polygon.size() < 3 || !shape.polygon.boundingRect().contains(point) ||
                    !shape.polygon.containsPoint(point, Qt::OddEvenFill))
                    continue;
                result.kind = SelectionKind::shape;
                result.index = index;
                result.name = shape.net;
                result.net = shape.net;
                result.netId = shape.netId;
                result.special = shape.special;
                result.layer = shape.layer.name;
                result.polygon = shape.polygon;
                bestDistance = 0.0;
                break;
            }
            if (shape.kind == ice::LayoutPreviewShape::Kind::path) {
                qreal distance = std::numeric_limits<qreal>::max();
                for (std::size_t segment = 0; segment + 1 < shape.centerline.size(); ++segment)
                    distance = std::min(
                        distance, distanceToSegment(point, shape.centerline[segment], shape.centerline[segment + 1]));
                const auto threshold = std::max<qreal>(shape.width * 0.5, 1.0);
                if (distance > threshold || distance >= bestDistance)
                    continue;
                bestDistance = distance;
                result.kind = SelectionKind::shape;
                result.index = index;
                result.name = shape.net;
                result.net = shape.net;
                result.netId = shape.netId;
                result.special = shape.special;
                result.layer = shape.layer.name;
                result.centerline = shape.centerline;
                result.width = shape.width;
            }
        }
        return result;
    }

    QRectF selectionSceneRect() const {
        QRectF rectangle = selection_.rectangle;
        if (selection_.polygon.size() >= 3)
            rectangle = selection_.polygon.boundingRect();
        else if (!selection_.centerline.empty()) {
            rectangle = QRectF(selection_.centerline.front(), QSizeF(0.0, 0.0));
            for (const auto &point : selection_.centerline)
                rectangle = rectangle.united(QRectF(point, QSizeF(0.0, 0.0)));
            rectangle.adjust(-selection_.width, -selection_.width, selection_.width, selection_.width);
        }
        return sceneRect(rectangle);
    }

    QColor heatMapColor(double value, double minimum, double maximum) const {
        const auto span = maximum - minimum;
        auto normalized = span > 0.0 ? std::clamp((value - minimum) / span, 0.0, 1.0) : 0.0;
        if (heatMapOptions_.logScale)
            normalized =
                span > 0.0 ? std::clamp(std::log10(1.0 + value - minimum) / std::log10(1.0 + span), 0.0, 1.0) : 0.0;
        if (heatMapOptions_.reverse)
            normalized = 1.0 - normalized;
        return QColor::fromHsvF(0.66 * (1.0 - normalized), 0.78, 0.92,
                                std::clamp(heatMapOptions_.alpha, 0, 255) / 255.0);
    }

    void addHeatMap(const ice::LayoutPreviewHeatMap &heatmap) {
        if (heatmap.columns <= 0 || heatmap.rows <= 0 || heatmap.bounds.width() <= 0.0 ||
            heatmap.bounds.height() <= 0.0 || heatmap.maximum <= heatmap.minimum)
            return;
        const auto minimum = heatMapOptions_.minimum.value_or(heatmap.minimum);
        const auto maximum = heatMapOptions_.maximum.value_or(heatmap.maximum);
        if (maximum <= minimum)
            return;
        const auto bin_width = heatmap.bounds.width() / heatmap.columns;
        const auto bin_height = heatmap.bounds.height() / heatmap.rows;
        for (int row = 0; row < heatmap.rows; ++row) {
            for (int column = 0; column < heatmap.columns; ++column) {
                const auto index = static_cast<std::size_t>(row) * heatmap.columns + column;
                if (index >= heatmap.values.size())
                    continue;
                const auto value = heatmap.values[index];
                if (value < minimum)
                    continue;
                const auto x = heatmap.bounds.x() + column * bin_width;
                const auto y = heatmap.bounds.y() + row * bin_height;
                auto *item = scene()->addRect(QRectF(x, -(y + bin_height), bin_width, bin_height), Qt::NoPen,
                                              heatMapColor(value, minimum, maximum));
                item->setZValue(-1.0);
            }
        }
    }

    void addMarkers(const ice::LayoutPreviewModel &model) {
        for (int index = 0; index < static_cast<int>(model.markers.size()); ++index) {
            const auto &marker = model.markers[static_cast<std::size_t>(index)];
            QPen pen(QColor(220, 38, 38), 1.0);
            pen.setCosmetic(true);
            auto *item = scene()->addRect(sceneRect(marker.rectangle), pen, QColor(220, 38, 38, 40));
            item->setZValue(4.0);
        }
    }

    void addFocusNet(const ice::LayoutPreviewModel &model) {
        if (focusNet_.isEmpty())
            return;
        QPen pen(QColor(250, 204, 21), 1.8);
        pen.setCosmetic(true);
        for (const auto &shape : model.shapes) {
            if (shape.net != focusNet_)
                continue;
            if (shape.kind == ice::LayoutPreviewShape::Kind::polygon) {
                auto *item = scene()->addPolygon(scenePolygon(shape.polygon), pen, QColor(250, 204, 21, 35));
                item->setZValue(6.0);
            } else if (shape.kind == ice::LayoutPreviewShape::Kind::path && shape.centerline.size() >= 2) {
                QPolygonF polygon;
                for (const auto &point : shape.centerline)
                    polygon << scenePoint(point);
                auto *item = scene()->addPolygon(polygon, pen, QBrush(Qt::NoBrush));
                item->setZValue(6.0);
            }
        }
        for (const auto &port : model.ports) {
            if (port.net != focusNet_)
                continue;
            auto *item = scene()->addRect(sceneRect(port.rectangle), pen, QColor(250, 204, 21, 45));
            item->setZValue(6.0);
        }
    }

    void addSelectionOverlay() {
        if (!selection_.valid())
            return;
        QPen pen(QColor(37, 99, 235), 2.0);
        pen.setCosmetic(true);
        const auto fill = QColor(37, 99, 235, 40);
        if (selection_.kind == SelectionKind::shape && selection_.polygon.size() >= 3) {
            auto *item = scene()->addPolygon(scenePolygon(selection_.polygon), pen, fill);
            item->setZValue(8.0);
        } else if (selection_.kind == SelectionKind::shape && selection_.centerline.size() >= 2) {
            QPolygonF polygon;
            for (const auto &point : selection_.centerline)
                polygon << scenePoint(point);
            auto *item = scene()->addPolygon(polygon, pen, QBrush(Qt::NoBrush));
            item->setZValue(8.0);
        } else if (selection_.rectangle.isValid()) {
            auto *item = scene()->addRect(sceneRect(selection_.rectangle), pen, fill);
            item->setZValue(8.0);
        }
    }

    void addAnnotations() {
        QPen rulerPen(QColor(250, 204, 21), 1.2);
        rulerPen.setCosmetic(true);
        for (const auto &[start, end] : rulers_) {
            const auto sceneStart = scenePoint(start);
            const auto sceneEnd = scenePoint(end);
            scene()->addLine(QLineF(sceneStart, sceneEnd), rulerPen)->setZValue(9.0);
            auto *label =
                scene()->addText(QStringLiteral("%1 µm").arg(QLineF(start, end).length() / 1000.0, 0, 'f', 3));
            label->setDefaultTextColor(QColor(250, 204, 21));
            label->setPos((sceneStart + sceneEnd) * 0.5);
            label->setZValue(9.0);
        }
        for (const auto &[position, text] : labels_) {
            auto *item = scene()->addText(text);
            item->setDefaultTextColor(palette().color(QPalette::Text));
            item->setPos(scenePoint(position));
            item->setZValue(9.0);
        }
    }

    void rebuild(bool refit = true) {
        scene()->clear();
        layers_.clear();
        markerSelections_.clear();
        activeModel_ = nullptr;
        fallbackModel_.reset();
        if (refit)
            fitOnResize_ = true;
        const ice::LayoutPreviewModel *model = model_.get();
        if (!model && project_ && cell_ != 0) {
            const auto found = project_->layoutCells.find(cell_);
            if (found != project_->layoutCells.end())
                fallbackModel_ = ice::buildLayoutCellPreviewModel(*project_, found->second, true, true, true);
            if (fallbackModel_ && !fallbackModel_->empty())
                model = &*fallbackModel_;
        }
        activeModel_ = model;
        if (model && !model->empty()) {
            const auto styles = styleProject_ ? layerStylesFor(*styleProject_) : ice::LayoutRenderer::LayerStyleMap{};
            std::map<QString, LayerEntry> uniqueLayers;
            const auto collectLayer = [&](const ice::LayoutPreviewLayer &layer) {
                const auto key = ice::layoutPreviewLayerKey(layer);
                if (uniqueLayers.contains(key))
                    return;
                LayerEntry entry;
                entry.key = key;
                entry.number = layer.number;
                entry.label = !layer.name.isEmpty() ? layer.name
                                                    : (layer.number > 0 ? QStringLiteral("Layer %1").arg(layer.number)
                                                                        : QStringLiteral("Unnamed layer"));
                if (layer.id != 0) {
                    if (const auto style = styles.find(layer.id); style != styles.end())
                        entry.color = style->second.color;
                }
                if (!entry.color.isValid())
                    entry.color = layerStyleForNumber(layer.number).color;
                entry.visible = !hiddenLayers_.contains(key);
                uniqueLayers.emplace(key, std::move(entry));
            };
            for (const auto &shape : model->shapes)
                collectLayer(shape.layer);
            for (const auto &port : model->ports)
                collectLayer(port.layer);
            for (auto &[key, entry] : uniqueLayers)
                layers_.push_back(std::move(entry));
            std::sort(layers_.begin(), layers_.end(), [](const auto &left, const auto &right) {
                if (left.number != right.number)
                    return left.number < right.number;
                return left.label < right.label;
            });

            for (int index = 0; index < static_cast<int>(model->markers.size()); ++index)
                markerSelections_.push_back(selectionForMarker(*model, index));

            if (customHeatMap_)
                addHeatMap(*customHeatMap_);
            else if (heatMapKind_ == ice::LayoutHeatMapKind::placement_density && model->placementDensity)
                addHeatMap(*model->placementDensity);
            else if (heatMapKind_ == ice::LayoutHeatMapKind::routing_congestion && model->routingCongestion)
                addHeatMap(*model->routingCongestion);
            ice::LayoutRenderer::PaintOptions options;
            options.hiddenLayers = hiddenLayers_;
            options.hiddenInstanceCategories = hiddenInstanceCategories_;
            options.hiddenNetTypes = hiddenNetTypes_;
            options.showSpecialNets = showSpecialNets_;
            options.showPorts = showPorts_;
            options.showGuides = showGuides_;
            options.showRows = showRows_;
            options.showBlockages = showBlockages_;
            auto *item = createLayoutPreviewItem(model, styles, {}, options);
            scene()->addItem(item);
            addMarkers(*model);
            addFocusNet(*model);
            addSelectionOverlay();
            addAnnotations();
            scene()->setSceneRect(scene()->itemsBoundingRect());
            if (refit)
                fitToScene();
            return;
        }
        auto *text = scene()->addText(message_.isEmpty() ? QStringLiteral("No preview available.") : message_);
        auto font = text->font();
        font.setPointSize(font.pointSize() + 1);
        text->setFont(font);
        text->setDefaultTextColor(palette().color(QPalette::Text));
        scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-40, -40, 40, 40));
        if (refit)
            fitToScene();
    }

    void fitToScene() {
        if (!scene() || scene()->sceneRect().isEmpty())
            return;
        fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    }

    std::shared_ptr<ice::Project> project_;
    ice::ObjectId cell_ = 0;
    QString message_;
    std::shared_ptr<ice::LayoutPreviewModel> model_;
    const ice::Project *styleProject_ = nullptr;
    ice::LayoutHeatMapKind heatMapKind_ = ice::LayoutHeatMapKind::none;
    std::set<QString> hiddenLayers_;
    std::set<QString> hiddenInstanceCategories_ = {QStringLiteral("fill"), QStringLiteral("endcap"),
                                                   QStringLiteral("tap"), QStringLiteral("antenna"),
                                                   QStringLiteral("cover")};
    std::set<QString> hiddenNetTypes_;
    bool showSpecialNets_ = true;
    bool showPorts_ = true;
    bool showGuides_ = false;
    bool showRows_ = false;
    bool showBlockages_ = false;
    HeatMapOptions heatMapOptions_;
    std::optional<ice::LayoutPreviewHeatMap> customHeatMap_;
    std::optional<ice::LayoutPreviewModel> fallbackModel_;
    const ice::LayoutPreviewModel *activeModel_ = nullptr;
    Selection selection_;
    QString focusNet_;
    std::vector<Selection> markerSelections_;
    std::vector<std::pair<QPointF, QPointF>> rulers_;
    std::vector<std::pair<QPointF, QString>> labels_;
    Tool tool_ = Tool::none;
    bool rulerBuilding_ = false;
    QPointF rulerStart_;
    std::function<void(const Selection &)> selectionCallback_;
    std::function<void(const QPointF &, const QString &)> statusCallback_;
    std::vector<LayerEntry> layers_;
    bool fitOnResize_ = true;
};

std::optional<std::filesystem::path> regularFile(const std::filesystem::path &path) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status) || status)
        return std::nullopt;
    const auto size = std::filesystem::file_size(path, status);
    if (status || size == 0)
        return std::nullopt;
    return path;
}

std::optional<std::filesystem::path> runArtifact(const ice::ImplementationRun &run, const QString &root,
                                                 const std::string &suffix) {
    const auto resolve = [&](const std::string &artifact) -> std::optional<std::filesystem::path> {
        if (!artifact.ends_with(suffix))
            return std::nullopt;
        std::filesystem::path path(artifact);
        if (path.is_relative())
            path = std::filesystem::path(root.toStdString()) / path;
        return regularFile(path);
    };
    for (const auto &[_, state] : run.stages)
        for (const auto &artifact : state.artifacts)
            if (auto found = resolve(artifact))
                return found;
    for (const auto &artifact : run.artifacts)
        if (auto found = resolve(artifact))
            return found;
    return std::nullopt;
}

// Resolve a run graph from the recorded artifacts first, then from the run
// directory. When only the DOT file exists, optionally render it to SVG with
// Graphviz so the canvas can display it.
std::optional<std::filesystem::path> runGraph(const ice::ImplementationRun &run, const QString &root,
                                              const std::string &kind, bool renderDot) {
    const auto suffix = "/reports/" + kind;
    if (auto svg = runArtifact(run, root, suffix + ".svg"))
        return svg;
    const auto rootPath = std::filesystem::path(root.toStdString());
    std::vector<std::filesystem::path> runDirectories;
    if (!run.directory.empty())
        runDirectories.push_back(rootPath / run.directory);
    runDirectories.push_back(rootPath / "build" / "impl" / ice::implementationRunStem(run));
    for (const auto &directory : runDirectories)
        if (auto svg = regularFile(directory / "reports" / (kind + ".svg")))
            return svg;
    std::optional<std::filesystem::path> dot = runArtifact(run, root, suffix + ".dot");
    if (!dot) {
        for (const auto &directory : runDirectories) {
            if (auto candidate = regularFile(directory / "reports" / (kind + ".dot"))) {
                dot = candidate;
                break;
            }
        }
    }
    if (!dot || !renderDot)
        return std::nullopt;
    auto svgPath = *dot;
    svgPath.replace_extension(".svg");
    if (auto existing = regularFile(svgPath))
        return existing;
    const auto executable = QStandardPaths::findExecutable(QStringLiteral("dot"));
    if (executable.isEmpty())
        return std::nullopt;
    std::error_code sizeStatus;
    const auto dotBytes = std::filesystem::file_size(*dot, sizeStatus);
    const bool large = !sizeStatus && dotBytes > 1ULL * 1024ULL * 1024ULL;
    QStringList arguments;
    if (large)
        arguments << QStringLiteral("-Ksfdp");
    arguments << QStringLiteral("-Tsvg") << QString::fromStdString(dot->string()) << QStringLiteral("-o")
              << QString::fromStdString(svgPath.string());
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(large ? 300000 : 60000))
        return std::nullopt;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return std::nullopt;
    return regularFile(svgPath);
}

// Displays a Yosys-generated SVG graph with the same zoom/pan behavior as the
// DEF preview canvas.
class SvgPreviewCanvas : public QGraphicsView {
  public:
    explicit SvgPreviewCanvas(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setRenderHint(QPainter::Antialiasing, true);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setBackgroundBrush(palette().color(QPalette::Base));
    }

    ~SvgPreviewCanvas() override { scene()->clear(); }

    void setSvg(const std::filesystem::path &path, const QString &message) {
        svgPath_ = path;
        message_ = message;
        reload();
    }

  protected:
    void changeEvent(QEvent *event) override {
        QGraphicsView::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
            setBackgroundBrush(palette().color(QPalette::Base));
            reload();
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (fitOnResize_)
            fitToScene();
    }

    void wheelEvent(QWheelEvent *event) override {
        const auto factor = event->angleDelta().y() > 0 ? 1.18 : (1.0 / 1.18);
        scale(factor, factor);
        fitOnResize_ = false;
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        fitOnResize_ = true;
        fitToScene();
        QGraphicsView::mouseDoubleClickEvent(event);
    }

  private:
    QString themedSvg(QString source) const {
        const auto text = palette().color(QPalette::Text).name(QColor::HexRgb);
        const auto base = palette().color(QPalette::Base).name(QColor::HexRgb);
        source.replace(QStringLiteral("fill=\"white\""), QStringLiteral("fill=\"%1\"").arg(base));
        source.replace(QStringLiteral("fill=\"black\""), QStringLiteral("fill=\"%1\"").arg(text));
        source.replace(QStringLiteral("stroke=\"black\""), QStringLiteral("stroke=\"%1\"").arg(text));
        source.replace(QStringLiteral("stroke=\"white\""), QStringLiteral("stroke=\"%1\"").arg(base));
        // Graphviz leaves text without an explicit fill. Give each text
        // element the theme foreground so it stays readable on both palettes.
        const QRegularExpression textElement(QStringLiteral("<text\\b([^>]*)>"));
        QString result;
        result.reserve(source.size() + 64);
        int offset = 0;
        auto matches = textElement.globalMatch(source);
        while (matches.hasNext()) {
            const auto match = matches.next();
            result += source.mid(offset, match.capturedStart() - offset);
            const auto attributes = match.captured(1);
            if (attributes.contains(QStringLiteral("fill="))) {
                result += match.captured(0);
            } else {
                result +=
                    QStringLiteral("<text fill=\"") + text + QStringLiteral("\"") + attributes + QStringLiteral(">");
            }
            offset = match.capturedEnd();
        }
        result += source.mid(offset);
        return result;
    }

    std::filesystem::path themedCopy(const std::filesystem::path &source) {
        if (!themeDirectory_)
            themeDirectory_ = std::make_unique<QTemporaryDir>();
        if (!themeDirectory_ || !themeDirectory_->isValid())
            return source;
        QFile input(QString::fromStdString(source.string()));
        if (!input.open(QIODevice::ReadOnly))
            return source;
        const auto themed = themedSvg(QString::fromUtf8(input.readAll()));
        input.close();
        const auto target = themeDirectory_->filePath(QStringLiteral("graph.svg"));
        QFile output(target);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return source;
        output.write(themed.toUtf8());
        output.close();
        return std::filesystem::path(target.toStdString());
    }

    void reload() {
        scene()->clear();
        fitOnResize_ = true;
        std::error_code status;
        if (!svgPath_.empty() && std::filesystem::is_regular_file(svgPath_, status) && !status) {
            const auto displayPath = themedCopy(svgPath_);
            auto *item = new QGraphicsSvgItem(QString::fromStdString(displayPath.string()));
            if (item->renderer() && item->renderer()->isValid()) {
                item->setFlags(QGraphicsItem::ItemClipsToShape);
                scene()->addItem(item);
                scene()->setSceneRect(item->boundingRect());
                fitToScene();
                return;
            }
            delete item;
        }
        auto *text = scene()->addText(message_.isEmpty() ? QStringLiteral("No graph available.") : message_);
        auto font = text->font();
        font.setPointSize(font.pointSize() + 1);
        text->setFont(font);
        text->setDefaultTextColor(palette().color(QPalette::Text));
        scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-40, -40, 40, 40));
        fitToScene();
    }

    void fitToScene() {
        if (!scene() || scene()->sceneRect().isEmpty() || viewport()->size().isEmpty())
            return;
        const auto rect = scene()->sceneRect();
        const auto viewportSize = viewport()->size();
        const double sx = static_cast<double>(viewportSize.width()) / rect.width();
        const double sy = static_cast<double>(viewportSize.height()) / rect.height();
        double scaleFactor = std::min(sx, sy);
        // Extremely tall or wide graphs become unreadably small when fitted
        // entirely. Fit the narrow axis and let the user pan along the long one.
        if (rect.height() > rect.width() * 3.0)
            scaleFactor = sx;
        else if (rect.width() > rect.height() * 3.0)
            scaleFactor = sy;
        scaleFactor = std::clamp(scaleFactor, 0.02, 8.0);
        resetTransform();
        scale(scaleFactor, scaleFactor);
        centerOn(rect.left() + viewportSize.width() / (2.0 * scaleFactor),
                 rect.top() + viewportSize.height() / (2.0 * scaleFactor));
    }

    std::filesystem::path svgPath_;
    QString message_;
    std::unique_ptr<QTemporaryDir> themeDirectory_;
    bool fitOnResize_ = true;
};

ImplementationUiController::ImplementationUiController(QObject *parent) : parent_(parent) {}

ImplementationUiController::~ImplementationUiController() {
    for (auto *process : {process_, previewProcess_, heatMapProcess_}) {
        if (!process)
            continue;
        process->disconnect();
        process->kill();
        process->waitForFinished(1000);
    }
    process_ = nullptr;
    previewProcess_ = nullptr;
    heatMapProcess_ = nullptr;
}

void ImplementationUiController::setProject(const ice::Project *project, const QString &projectPath) {
    if (project_ != project || projectPath_ != projectPath)
        previewCache_.clear();
    project_ = project;
    projectPath_ = projectPath;
    if (activeRunId_ == 0 || !activeRun())
        activeRunId_ = 0;
    if (activeRunId_ == 0 && project_ && !project_->implementationRuns.empty())
        activeRunId_ = project_->implementationRuns.begin()->first;
    refresh();
}

QString ImplementationUiController::projectRoot() const {
    if (projectPath_.isEmpty())
        return {};
    const QFileInfo info(projectPath_);
    return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

bool ImplementationUiController::busy() const { return process_ && process_->state() != QProcess::NotRunning; }

const ice::ImplementationRun *ImplementationUiController::activeRun() const {
    if (!project_)
        return nullptr;
    const auto found = project_->implementationRuns.find(activeRunId_);
    return found == project_->implementationRuns.end() ? nullptr : &found->second;
}

void ImplementationUiController::setActiveRunId(ice::ObjectId id) {
    if (activeRunId_ == id)
        return;
    activeRunId_ = id;
    if (onChanged_)
        onChanged_();
}

void ImplementationUiController::addLogListener(
    std::function<void(ice::ImplementationStage, const QString &)> listener) {
    logListeners_.push_back(std::move(listener));
}

void ImplementationUiController::refresh() {
    if (onChanged_)
        onChanged_();
}

ImplementationUiController::StagePreview ImplementationUiController::stagePreview(ice::ImplementationStage stage) {
    StagePreview result;
    const auto *run = activeRun();
    if (!run) {
        result.message = QStringLiteral("Select an implementation run in the Runs panel.");
        return result;
    }
    if (stage == ice::ImplementationStage::created) {
        result.message = QStringLiteral("RTL has no physical preview.");
        return result;
    }
    if (stage == ice::ImplementationStage::synthesis) {
        result.message = QStringLiteral("Synthesis has no physical geometry yet.\nRun Floorplan to create a DEF "
                                        "checkpoint.");
        if (const auto cells = run->metrics.find("synth.cells"); cells != run->metrics.end()) {
            result.message = QStringLiteral("Synthesized %1 cells.\nRun Floorplan to create a DEF checkpoint.")
                                 .arg(QString::number(cells->second, 'g', 10));
        }
        return result;
    }
    const auto odbRelative = stageOdbPath(stage);
    const auto defRelative = stageDefPath(stage);
    if (odbRelative.empty() && defRelative.empty()) {
        result.message = QStringLiteral("This stage has no physical checkpoint.");
        return result;
    }
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = odbRelative.empty() ? std::filesystem::path{} : runRoot / odbRelative;
    const auto defPath = defRelative.empty() ? std::filesystem::path{} : runRoot / defRelative;
    std::error_code status;
    const bool odbExists = !odbPath.empty() && std::filesystem::is_regular_file(odbPath, status) && !status;
    const bool defExists = !defPath.empty() && std::filesystem::is_regular_file(defPath, status) && !status;
    const auto previewPath = odbExists ? odbPath : defPath;
    const auto stamp =
        (odbExists || defExists) ? std::filesystem::last_write_time(previewPath, status).time_since_epoch().count() : 0;
    const auto size = (odbExists || defExists) ? std::filesystem::file_size(previewPath, status) : 0;
    const auto drcPath = runRoot / "reports" / "route_drc.rpt";
    const auto drcStamp = std::filesystem::is_regular_file(drcPath, status) && !status
                              ? std::filesystem::last_write_time(drcPath, status).time_since_epoch().count()
                              : 0;
    const auto key = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
                         .arg(static_cast<qulonglong>(run->id))
                         .arg(static_cast<int>(stage))
                         .arg(odbExists ? 1 : 0)
                         .arg(defExists ? 1 : 0)
                         .arg(static_cast<qlonglong>(stamp))
                         .arg(static_cast<qulonglong>(size))
                         .arg(static_cast<qlonglong>(drcStamp));
    if (const auto cached = previewCache_.find(static_cast<int>(stage));
        cached != previewCache_.end() && cached->second.key == key) {
        return {cached->second.project, cached->second.cell, cached->second.model, cached->second.message};
    }
    auto cache = [&](StagePreview preview) {
        previewCache_[static_cast<int>(stage)] = {key, preview.project, preview.cell, preview.model, preview.message};
        return preview;
    };
    if (!odbExists && !defExists) {
        result.message = QStringLiteral("No %1 ODB checkpoint yet.").arg(stageTitle(stage));
        return cache(std::move(result));
    }

    QString odbError;
    if (odbExists) {
        const auto helper = helperExecutable();
        if (!helper.isEmpty()) {
            QProcess process;
            process.setProcessChannelMode(QProcess::SeparateChannels);
            process.start(helper, {QStringLiteral("odb-preview"), QString::fromStdString(odbPath.string()),
                                   QStringLiteral("--heatmap-bins"), QStringLiteral("32")});
            if (process.waitForStarted(5000) && process.waitForFinished(120000) &&
                process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) {
                result.model = parseOdbPreview(process.readAllStandardOutput(), project_, odbError);
                if (result.model) {
                    if (stage == ice::ImplementationStage::routing || stage == ice::ImplementationStage::final)
                        result.model->markers = parseDrcMarkers(runRoot / "reports" / "route_drc.rpt");
                    result.message.clear();
                    return cache(std::move(result));
                }
            } else {
                process.kill();
                process.waitForFinished();
                odbError = QString::fromStdString(process.readAllStandardError().toStdString());
                if (odbError.isEmpty())
                    odbError = QStringLiteral("ice-openroad odb-preview did not finish.");
            }
        }
    }
    if (!defExists) {
        result.message = odbError.isEmpty()
                             ? QStringLiteral("Unable to read the %1 ODB checkpoint.").arg(stageTitle(stage))
                             : odbError;
        return cache(std::move(result));
    }
    const auto path = defPath;
    const auto pdks = ice::findPdks(run->config.pdk);
    if (pdks.empty()) {
        result.message =
            QStringLiteral("PDK package '%1' is not installed.").arg(QString::fromStdString(run->config.pdk));
        return cache(std::move(result));
    }
    try {
        const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
        if (!package.pdk) {
            result.message =
                QStringLiteral("Package '%1' does not declare a PDK.").arg(QString::fromStdString(package.name));
            return cache(std::move(result));
        }
        std::vector<ice::Diagnostic> diagnostics;
        const auto platform = ice::resolveImplementationPlatform(package, run->config.stack, diagnostics);
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.severity == ice::Diagnostic::Severity::error) {
                result.message = QString::fromStdString(diagnostic.message);
                return cache(std::move(result));
            }
        }
        ice::DefImportOptions options;
        options.techLef = platform.techLef;
        options.cellLef = platform.cellLef;
        options.pdk = package.pdk->id.empty() ? package.name : package.pdk->id;
        options.stack = platform.stack.empty() ? run->config.stack : platform.stack;
        // Layer definitions are keyed by lower-case names.
        for (const auto &layer : package.pdk->layers) {
            std::string name = layer.name;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            options.layers[name] = {layer.number, layer.datatype};
        }
        options.cellName = run->name + " preview";
        result.project = std::make_shared<ice::Project>();
        result.project->identity = run->name + " preview";
        result.project->name = run->name + " preview";
        result.cell = ice::LayoutInterchange::importDefIntoProject(*result.project, path, options);
        if (result.cell == 0) {
            result.project.reset();
            result.message = QStringLiteral("Unable to import the %1 DEF checkpoint.").arg(stageTitle(stage));
        }
        return cache(std::move(result));
    } catch (const std::exception &error) {
        result.project.reset();
        result.cell = 0;
        result.message = QString::fromStdString(error.what());
        return cache(std::move(result));
    }
}

void ImplementationUiController::requestStagePreview(ice::ImplementationStage stage, StagePreviewCallback callback) {
    if (!callback)
        return;
    const auto *run = activeRun();
    if (!run) {
        StagePreview result;
        result.message = QStringLiteral("Select an implementation run in the Runs panel.");
        callback(result);
        return;
    }
    const auto odbRelative = stageOdbPath(stage);
    const auto defRelative = stageDefPath(stage);
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = odbRelative.empty() ? std::filesystem::path{} : runRoot / odbRelative;
    const auto defPath = defRelative.empty() ? std::filesystem::path{} : runRoot / defRelative;
    std::error_code status;
    const bool odbExists = !odbPath.empty() && std::filesystem::is_regular_file(odbPath, status) && !status;
    const bool defExists = !defPath.empty() && std::filesystem::is_regular_file(defPath, status) && !status;
    const auto previewPath = odbExists ? odbPath : defPath;
    const auto stamp =
        (odbExists || defExists) ? std::filesystem::last_write_time(previewPath, status).time_since_epoch().count() : 0;
    const auto size = (odbExists || defExists) ? std::filesystem::file_size(previewPath, status) : 0;
    const auto drcPath = runRoot / "reports" / "route_drc.rpt";
    const auto drcStamp = std::filesystem::is_regular_file(drcPath, status) && !status
                              ? std::filesystem::last_write_time(drcPath, status).time_since_epoch().count()
                              : 0;
    const auto key = QStringLiteral("%1:%2:%3:%4:%5:%6:%7")
                         .arg(static_cast<qulonglong>(run->id))
                         .arg(static_cast<int>(stage))
                         .arg(odbExists ? 1 : 0)
                         .arg(defExists ? 1 : 0)
                         .arg(static_cast<qlonglong>(stamp))
                         .arg(static_cast<qulonglong>(size))
                         .arg(static_cast<qlonglong>(drcStamp));
    if (const auto cached = previewCache_.find(static_cast<int>(stage));
        cached != previewCache_.end() && cached->second.key == key) {
        callback({cached->second.project, cached->second.cell, cached->second.model, cached->second.message});
        return;
    }
    const auto helper = helperExecutable();
    if (!odbExists || helper.isEmpty()) {
        callback(stagePreview(stage));
        return;
    }

    if (previewProcess_) {
        previewProcess_->disconnect();
        previewProcess_->kill();
        previewProcess_->deleteLater();
        previewProcess_ = nullptr;
    }
    auto *process = new QProcess(parent_);
    previewProcess_ = process;
    process->setProperty("iceHandled", false);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(process, &QProcess::errorOccurred, parent_,
                     [this, process, callback](QProcess::ProcessError error) {
                         if (error != QProcess::FailedToStart || process->property("iceHandled").toBool())
                             return;
                         process->setProperty("iceHandled", true);
                         StagePreview result;
                         result.message = QStringLiteral("Unable to start the ODB preview process.");
                         previewProcess_ = nullptr;
                         process->deleteLater();
                         callback(result);
                     });
    QObject::connect(
        process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), parent_,
        [this, process, stage, key, runRoot, odbPath, drcPath, callback, defExists](int exitCode,
                                                                                    QProcess::ExitStatus) {
            if (process->property("iceHandled").toBool())
                return;
            process->setProperty("iceHandled", true);
            auto result = StagePreview{};
            QString parseError;
            if (process->exitStatus() == QProcess::NormalExit && exitCode == 0) {
                result.model = parseOdbPreview(process->readAllStandardOutput(), project_, parseError);
                if (result.model) {
                    if (stage == ice::ImplementationStage::routing || stage == ice::ImplementationStage::final)
                        result.model->markers = parseDrcMarkers(drcPath);
                    previewCache_[static_cast<int>(stage)] = {key, result.project, result.cell, result.model,
                                                              result.message};
                    previewProcess_ = nullptr;
                    process->deleteLater();
                    callback(result);
                    return;
                }
            }
            parseError = QString::fromStdString(process->readAllStandardError().toStdString()).trimmed();
            if (parseError.isEmpty())
                parseError = QStringLiteral("Unable to load the ODB preview.");
            result.message = parseError;
            previewCache_[static_cast<int>(stage)] = {key, result.project, result.cell, result.model, result.message};
            previewProcess_ = nullptr;
            process->deleteLater();
            callback(result);
            (void)defExists;
        });
    process->start(helper, {QStringLiteral("odb-preview"), QString::fromStdString(odbPath.string()),
                            QStringLiteral("--heatmap-bins"), QStringLiteral("32")});
}

QString ImplementationUiController::exportHeatMap(ice::ImplementationStage stage, const QString &kind, QString &error) {
    const auto *run = activeRun();
    if (!run) {
        error = QStringLiteral("Select an implementation run in the Runs panel.");
        return {};
    }
    const auto odbRelative = stageOdbPath(stage);
    const auto sdcRelative = stageSdcPath(stage);
    if (odbRelative.empty()) {
        error = QStringLiteral("This stage has no physical checkpoint.");
        return {};
    }
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = runRoot / odbRelative;
    const auto sdcPath = runRoot / sdcRelative;
    std::error_code status;
    if (!std::filesystem::is_regular_file(odbPath, status) || status) {
        error = QStringLiteral("No %1 ODB checkpoint yet.").arg(stageTitle(stage));
        return {};
    }
    const auto helper = helperExecutable();
    if (helper.isEmpty()) {
        error = QStringLiteral("The ice-openroad helper is not available.");
        return {};
    }
    const auto pdks = ice::findPdks(run->config.pdk);
    if (pdks.empty()) {
        error = QStringLiteral("PDK package '%1' is not installed.").arg(QString::fromStdString(run->config.pdk));
        return {};
    }
    QStringList arguments = {QStringLiteral("heatmap"), QStringLiteral("--input-db"),
                             QString::fromStdString(odbPath.string()), QStringLiteral("--kind"), kind};
    if (std::filesystem::is_regular_file(sdcPath, status) && !status)
        arguments << QStringLiteral("--sdc") << QString::fromStdString(sdcPath.string());
    try {
        const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
        if (!package.pdk) {
            error = QStringLiteral("Package '%1' does not declare a PDK.").arg(QString::fromStdString(package.name));
            return {};
        }
        std::vector<ice::Diagnostic> diagnostics;
        const auto platform = ice::resolveImplementationPlatform(package, run->config.stack, diagnostics);
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.severity == ice::Diagnostic::Severity::error) {
                error = QString::fromStdString(diagnostic.message);
                return {};
            }
        }
        std::vector<std::filesystem::path> liberty;
        if (const auto found = platform.liberty.find(run->config.corner); found != platform.liberty.end())
            liberty = found->second;
        else if (!platform.liberty.empty())
            liberty = platform.liberty.begin()->second;
        for (const auto &path : liberty)
            arguments << QStringLiteral("--liberty") << QString::fromStdString(path.string());
    } catch (const std::exception &exception) {
        error = QString::fromStdString(exception.what());
        return {};
    }
    const auto output = runRoot / "reports" / (kind.toStdString() + "-heatmap.csv");
    arguments << QStringLiteral("--output") << QString::fromStdString(output.string()) << QStringLiteral("--log")
              << QString::fromStdString((runRoot / "logs" / "heatmap.log").string()) << QStringLiteral("--threads")
              << QString::number(std::max(1, run->config.threads));

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(helper, arguments);
    const bool started = process.waitForStarted(5000);
    const bool finished = started && process.waitForFinished(300000);
    const auto standardError = QString::fromStdString(process.readAllStandardError().toStdString()).trimmed();
    const auto standardOutput = QString::fromStdString(process.readAllStandardOutput().toStdString()).trimmed();
    QApplication::restoreOverrideCursor();
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (!finished)
            process.kill();
        error = standardError;
        if (error.isEmpty())
            error = standardOutput.isEmpty() ? QStringLiteral("OpenROAD heat-map export failed.") : standardOutput;
        return {};
    }
    if (!std::filesystem::is_regular_file(output, status) || status) {
        error = standardError.isEmpty() ? QStringLiteral("OpenROAD did not write the heat-map CSV.") : standardError;
        return {};
    }
    return QString::fromStdString(output.string());
}

void ImplementationUiController::requestExportHeatMap(ice::ImplementationStage stage, const QString &kind,
                                                      HeatMapCallback callback) {
    if (!callback)
        return;
    const auto *run = activeRun();
    if (!run) {
        callback({}, QStringLiteral("Select an implementation run in the Runs panel."));
        return;
    }
    const auto odbRelative = stageOdbPath(stage);
    const auto sdcRelative = stageSdcPath(stage);
    if (odbRelative.empty()) {
        callback({}, QStringLiteral("This stage has no physical checkpoint."));
        return;
    }
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = runRoot / odbRelative;
    const auto sdcPath = runRoot / sdcRelative;
    std::error_code status;
    if (!std::filesystem::is_regular_file(odbPath, status) || status) {
        callback({}, QStringLiteral("No %1 ODB checkpoint yet.").arg(stageTitle(stage)));
        return;
    }
    const auto helper = helperExecutable();
    if (helper.isEmpty()) {
        callback({}, QStringLiteral("The ice-openroad helper is not available."));
        return;
    }
    QStringList arguments = {QStringLiteral("heatmap"), QStringLiteral("--input-db"),
                             QString::fromStdString(odbPath.string()), QStringLiteral("--kind"), kind};
    if (std::filesystem::is_regular_file(sdcPath, status) && !status)
        arguments << QStringLiteral("--sdc") << QString::fromStdString(sdcPath.string());
    const auto pdks = ice::findPdks(run->config.pdk);
    if (pdks.empty()) {
        callback({}, QStringLiteral("PDK package '%1' is not installed.").arg(QString::fromStdString(run->config.pdk)));
        return;
    }
    try {
        const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
        if (!package.pdk) {
            callback({},
                     QStringLiteral("Package '%1' does not declare a PDK.").arg(QString::fromStdString(package.name)));
            return;
        }
        std::vector<ice::Diagnostic> diagnostics;
        const auto platform = ice::resolveImplementationPlatform(package, run->config.stack, diagnostics);
        std::vector<std::filesystem::path> liberty;
        if (const auto found = platform.liberty.find(run->config.corner); found != platform.liberty.end())
            liberty = found->second;
        else if (!platform.liberty.empty())
            liberty = platform.liberty.begin()->second;
        for (const auto &path : liberty)
            arguments << QStringLiteral("--liberty") << QString::fromStdString(path.string());
    } catch (const std::exception &exception) {
        callback({}, QString::fromStdString(exception.what()));
        return;
    }
    const auto output = runRoot / "reports" / (kind.toStdString() + "-heatmap.csv");
    arguments << QStringLiteral("--output") << QString::fromStdString(output.string()) << QStringLiteral("--log")
              << QString::fromStdString((runRoot / "logs" / "heatmap.log").string()) << QStringLiteral("--threads")
              << QString::number(std::max(1, run->config.threads));

    if (heatMapProcess_) {
        heatMapProcess_->disconnect();
        heatMapProcess_->kill();
        heatMapProcess_->deleteLater();
        heatMapProcess_ = nullptr;
    }
    auto *process = new QProcess(parent_);
    heatMapProcess_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), parent_,
                     [this, process, output, callback](int exitCode, QProcess::ExitStatus) {
                         const auto standardError =
                             QString::fromStdString(process->readAllStandardError().toStdString()).trimmed();
                         const auto standardOutput =
                             QString::fromStdString(process->readAllStandardOutput().toStdString()).trimmed();
                         heatMapProcess_ = nullptr;
                         process->deleteLater();
                         std::error_code status;
                         if (process->exitStatus() != QProcess::NormalExit || exitCode != 0 ||
                             !std::filesystem::is_regular_file(output, status) || status) {
                             auto error = standardError;
                             if (error.isEmpty())
                                 error = standardOutput.isEmpty() ? QStringLiteral("OpenROAD heat-map export failed.")
                                                                  : standardOutput;
                             callback({}, error);
                             return;
                         }
                         callback(QString::fromStdString(output.string()), {});
                     });
    process->start(helper, arguments);
}

bool ImplementationUiController::openInOpenRoadGui(ice::ImplementationStage stage, QString &error) {
    const auto *run = activeRun();
    if (!run || projectPath_.isEmpty()) {
        error = QStringLiteral("Select an implementation run first.");
        return false;
    }
    const auto odbRelative = stageOdbPath(stage);
    if (odbRelative.empty()) {
        error = QStringLiteral("This stage has no ODB checkpoint.");
        return false;
    }
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = runRoot / odbRelative;
    std::error_code status;
    if (!std::filesystem::is_regular_file(odbPath, status) || status) {
        error = QStringLiteral("No %1 ODB checkpoint yet.").arg(stageTitle(stage));
        return false;
    }
    auto toolchain = ice::discoverImplementationToolchain();
    auto openroad = toolchain.openroad.path;
    if (openroad.empty())
        openroad = QStandardPaths::findExecutable(QStringLiteral("openroad")).toStdString();
    if (openroad.empty()) {
        error = QStringLiteral("OpenROAD was not found on PATH.");
        return false;
    }

    const auto tclQuote = [](const std::filesystem::path &path) {
        std::string value = path.generic_string();
        std::string result = "\"";
        for (const char character : value) {
            if (character == '\\' || character == '"' || character == '$' || character == '[' || character == ']')
                result.push_back('\\');
            result.push_back(character);
        }
        result.push_back('"');
        return result;
    };

    QString script;
    std::vector<std::filesystem::path> liberty;
    const auto pdks = ice::findPdks(run->config.pdk);
    if (!pdks.empty()) {
        try {
            const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
            std::vector<ice::Diagnostic> diagnostics;
            const auto platform = ice::resolveImplementationPlatform(package, run->config.stack, diagnostics);
            if (const auto found = platform.liberty.find(run->config.corner); found != platform.liberty.end())
                liberty = found->second;
            else if (!platform.liberty.empty())
                liberty = platform.liberty.begin()->second;
        } catch (const std::exception &) {
        }
    }
    for (const auto &path : liberty)
        script += QStringLiteral("read_liberty %1\n").arg(QString::fromStdString(tclQuote(path)));
    script += QStringLiteral("read_db %1\n").arg(QString::fromStdString(tclQuote(odbPath)));
    const auto sdcRelative = stageSdcPath(stage);
    if (!sdcRelative.empty()) {
        const auto sdcPath = runRoot / sdcRelative;
        if (std::filesystem::is_regular_file(sdcPath, status) && !status)
            script += QStringLiteral("read_sdc %1\n").arg(QString::fromStdString(tclQuote(sdcPath)));
    }

    const auto scriptPath = runRoot / "logs" / "openroad-gui.tcl";
    std::filesystem::create_directories(scriptPath.parent_path(), status);
    std::ofstream output(scriptPath, std::ios::trunc);
    if (!output) {
        error = QStringLiteral("Unable to write %1.").arg(QString::fromStdString(scriptPath.string()));
        return false;
    }
    output << script.toStdString();
    output.close();

    QStringList arguments = {QStringLiteral("-gui"), QStringLiteral("-no_init"), QStringLiteral("-no_splash"),
                             QString::fromStdString(scriptPath.string())};
    if (!QProcess::startDetached(QString::fromStdString(openroad.string()), arguments,
                                 QString::fromStdString(runRoot.string()))) {
        error = QStringLiteral("Unable to launch OpenROAD GUI.");
        return false;
    }
    return true;
}

QString ImplementationUiController::timingReport(ice::ImplementationStage stage, QString &error) {
    const auto *run = activeRun();
    if (!run || projectPath_.isEmpty()) {
        error = QStringLiteral("Select an implementation run first.");
        return {};
    }
    const auto odbRelative = stageOdbPath(stage);
    if (odbRelative.empty()) {
        error = QStringLiteral("This stage has no ODB checkpoint.");
        return {};
    }
    const auto runRoot = std::filesystem::path(projectRoot().toStdString()) / run->directory;
    const auto odbPath = runRoot / odbRelative;
    std::error_code status;
    if (!std::filesystem::is_regular_file(odbPath, status) || status) {
        error = QStringLiteral("No %1 ODB checkpoint yet.").arg(stageTitle(stage));
        return {};
    }
    auto openroad = ice::discoverImplementationToolchain().openroad.path;
    if (openroad.empty())
        openroad = QStandardPaths::findExecutable(QStringLiteral("openroad")).toStdString();
    if (openroad.empty()) {
        error = QStringLiteral("OpenROAD was not found on PATH.");
        return {};
    }
    const auto tclQuote = [](const std::filesystem::path &path) {
        std::string value = path.generic_string();
        std::string result = "\"";
        for (const char character : value) {
            if (character == '\\' || character == '"' || character == '$' || character == '[' || character == ']')
                result.push_back('\\');
            result.push_back(character);
        }
        result.push_back('"');
        return result;
    };
    QString script;
    std::vector<std::filesystem::path> liberty;
    const auto pdks = ice::findPdks(run->config.pdk);
    if (!pdks.empty()) {
        try {
            const auto package = ice::PackageManager::readManifest(pdks.front().packageRoot);
            std::vector<ice::Diagnostic> diagnostics;
            const auto platform = ice::resolveImplementationPlatform(package, run->config.stack, diagnostics);
            if (const auto found = platform.liberty.find(run->config.corner); found != platform.liberty.end())
                liberty = found->second;
            else if (!platform.liberty.empty())
                liberty = platform.liberty.begin()->second;
        } catch (const std::exception &) {
        }
    }
    for (const auto &path : liberty)
        script += QStringLiteral("read_liberty %1\n").arg(QString::fromStdString(tclQuote(path)));
    script += QStringLiteral("read_db %1\n").arg(QString::fromStdString(tclQuote(odbPath)));
    const auto sdcRelative = stageSdcPath(stage);
    if (!sdcRelative.empty()) {
        const auto sdcPath = runRoot / sdcRelative;
        if (std::filesystem::is_regular_file(sdcPath, status) && !status)
            script += QStringLiteral("read_sdc %1\n").arg(QString::fromStdString(tclQuote(sdcPath)));
    }
    script += QStringLiteral("report_checks -path_delay max -format full_clock_expanded -group_count 5\n");
    script += QStringLiteral("report_checks -path_delay min -format full_clock_expanded -group_count 5\n");
    script += QStringLiteral("report_power\n");

    const auto scriptPath = runRoot / "logs" / "timing-report.tcl";
    std::filesystem::create_directories(scriptPath.parent_path(), status);
    std::ofstream outputFile(scriptPath, std::ios::trunc);
    if (!outputFile) {
        error = QStringLiteral("Unable to write %1.").arg(QString::fromStdString(scriptPath.string()));
        return {};
    }
    outputFile << script.toStdString();
    outputFile.close();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(runRoot.string()));
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QString::fromStdString(openroad.string()),
                  {QStringLiteral("-no_init"), QStringLiteral("-no_splash"), QStringLiteral("-exit"),
                   QString::fromStdString(scriptPath.string())});
    const bool started = process.waitForStarted(5000);
    const bool finished = started && process.waitForFinished(180000);
    QApplication::restoreOverrideCursor();
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (!finished)
            process.kill();
        error = QString::fromStdString(process.readAllStandardOutput().toStdString()).trimmed();
        if (error.isEmpty())
            error = QStringLiteral("OpenROAD timing report failed.");
        return {};
    }
    const auto output = QString::fromStdString(process.readAllStandardOutput().toStdString());
    std::filesystem::remove(scriptPath, status);
    return output;
}

bool ImplementationUiController::importLayout(ice::ObjectId runId, QString &error) {
    if (!project_ || projectPath_.isEmpty()) {
        error = QStringLiteral("No project is open.");
        return false;
    }
    const auto found = project_->implementationRuns.find(runId);
    if (found == project_->implementationRuns.end()) {
        error = QStringLiteral("Select an implementation run first.");
        return false;
    }
    const auto cli = cliExecutable();
    if (cli.isEmpty()) {
        error = QStringLiteral("ice-cli was not found.");
        return false;
    }
    if (beforeRun_ && !beforeRun_()) {
        error = QStringLiteral("The project could not be saved before importing.");
        return false;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(cli,
                  {QStringLiteral("implementation-import"), projectPath_, QString::fromStdString(found->second.name)});
    if (!process.waitForStarted(5000) || !process.waitForFinished(120000)) {
        process.kill();
        error = QStringLiteral("Implementation import did not finish.");
        return false;
    }
    const auto output = QString::fromStdString(process.readAllStandardOutput().toStdString()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = output.isEmpty() ? QStringLiteral("Implementation import failed.") : output;
        return false;
    }
    if (onProjectChanged_)
        onProjectChanged_();
    return true;
}

QString ImplementationUiController::cliExecutable() const {
    const auto configured = qEnvironmentVariable("ICE_CLI");
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable())
        return configured;
    const auto sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/ice-cli");
    if (QFileInfo(sibling).isExecutable())
        return sibling;
    return QStandardPaths::findExecutable(QStringLiteral("ice-cli"));
}

QString ImplementationUiController::helperExecutable() const {
    const auto configured = qEnvironmentVariable("ICE_OPENROAD_HELPER");
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable())
        return configured;
    const auto sibling = QCoreApplication::applicationDirPath() + QStringLiteral("/ice-openroad");
    if (QFileInfo(sibling).isExecutable())
        return sibling;
    const auto fromIcelib = ice::findOpenRoadHelper();
    if (fromIcelib)
        return QString::fromStdString(fromIcelib->string());
    return QStandardPaths::findExecutable(QStringLiteral("ice-openroad"));
}

void ImplementationUiController::run(ice::ImplementationStage from, ice::ImplementationStage to) {
    if (busy()) {
        appendProcessText(QStringLiteral("[%1] An implementation run is already active.\n")
                              .arg(QString::fromUtf8(ice::implementationStageName(runningStage_))));
        return;
    }
    const auto *run = activeRun();
    if (!run || projectPath_.isEmpty()) {
        appendProcessText(QStringLiteral("[created] No implementation run is selected.\n"));
        return;
    }
    const auto cli = cliExecutable();
    if (cli.isEmpty()) {
        appendProcessText(
            QStringLiteral("[created] ice-cli was not found. Set ICE_CLI or install the ICE command-line tools.\n"));
        return;
    }
    const QStringList arguments = {QStringLiteral("implementation-run"),
                                   projectPath_,
                                   QString::fromStdString(run->name),
                                   QStringLiteral("--from"),
                                   QString::fromUtf8(ice::implementationStageName(from)),
                                   QStringLiteral("--to"),
                                   QString::fromUtf8(ice::implementationStageName(to)),
                                   QStringLiteral("--verbose")};
    appendProcessText(
        QStringLiteral("[%1] Starting %2 → %3.\n")
            .arg(QString::fromUtf8(ice::implementationStageName(from)), stageTitle(from), stageTitle(to)));
    startCli(cli, arguments, from);
}

void ImplementationUiController::exportMapped(ice::ObjectId runId, const QString &destination) {
    if (busy()) {
        appendProcessText(QStringLiteral("[synthesis] Another implementation process is already active.\n"));
        return;
    }
    if (!project_ || projectPath_.isEmpty())
        return;
    const auto found = project_->implementationRuns.find(runId);
    if (found == project_->implementationRuns.end())
        return;
    const auto synthesis = found->second.stages.find(ice::ImplementationStage::synthesis);
    if (synthesis == found->second.stages.end() ||
        synthesis->second.status != ice::ImplementationStageStatus::succeeded) {
        appendProcessText(
            QStringLiteral("[synthesis] Synthesis must complete before exporting a technology-mapped netlist.\n"));
        return;
    }
    const auto cli = cliExecutable();
    if (cli.isEmpty()) {
        appendProcessText(
            QStringLiteral("[synthesis] ice-cli was not found. Set ICE_CLI or install the ICE command-line tools.\n"));
        return;
    }
    QStringList arguments = {QStringLiteral("implementation-map"), projectPath_,
                             QString::fromStdString(found->second.name)};
    if (!destination.isEmpty())
        arguments << QStringLiteral("--output") << destination;
    arguments << QStringLiteral("--verbose");
    if (destination.isEmpty())
        appendProcessText(QStringLiteral("[synthesis] Mapping %1.\n").arg(QString::fromStdString(found->second.name)));
    else
        appendProcessText(QStringLiteral("[synthesis] Mapping %1 → %2.\n")
                              .arg(QString::fromStdString(found->second.name), destination));
    startCli(cli, arguments, ice::ImplementationStage::synthesis);
}

void ImplementationUiController::startCli(const QString &executable, const QStringList &arguments,
                                          ice::ImplementationStage stage) {
    if (beforeRun_ && !beforeRun_()) {
        appendProcessText(QStringLiteral("[created] Implementation command cancelled before starting.\n"));
        return;
    }
    runningStage_ = stage;
    processBuffer_.clear();
    process_ = new QProcess(parent_);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setWorkingDirectory(projectRoot());
    QObject::connect(process_, &QProcess::readyRead, parent_, [this] {
        if (process_)
            appendProcessText(QString::fromUtf8(process_->readAllStandardOutput()));
    });
    QObject::connect(process_, &QProcess::errorOccurred, parent_, [this, stage](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            appendProcessText(QStringLiteral("[%1] Failed to start ice-cli.\n")
                                  .arg(QString::fromUtf8(ice::implementationStageName(stage))));
    });
    QObject::connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), parent_,
                     [this](int exitCode, QProcess::ExitStatus status) { finishRun(exitCode, status); });
    process_->start(executable, arguments);
    if (onChanged_)
        onChanged_();
}

void ImplementationUiController::cancel() {
    const auto *run = activeRun();
    if (!run)
        return;
    std::string error;
    if (ice::cancelImplementationRun(*run, projectRoot().toStdString(), error))
        appendProcessText(QStringLiteral("[%1] Cancellation requested.\n")
                              .arg(QString::fromUtf8(ice::implementationStageName(runningStage_))));
    else
        appendProcessText(
            QStringLiteral("[%1] %2\n")
                .arg(QString::fromUtf8(ice::implementationStageName(runningStage_)), QString::fromStdString(error)));
}

void ImplementationUiController::appendProcessText(const QString &text) {
    processBuffer_ += text;
    QStringList lines = processBuffer_.split(QLatin1Char('\n'));
    processBuffer_ = lines.takeLast();
    for (auto &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        bool stageChanged = false;
        if (line.startsWith(QLatin1Char('['))) {
            const auto close = line.indexOf(QLatin1Char(']'));
            if (close > 1) {
                const auto prefix = line.mid(1, close - 1).toStdString();
                if (const auto parsed = ice::parseImplementationStage(prefix); parsed && *parsed != runningStage_) {
                    runningStage_ = *parsed;
                    stageChanged = true;
                }
            }
        }
        for (auto &listener : logListeners_)
            listener(runningStage_, line + QLatin1Char('\n'));
        if (stageChanged && onChanged_)
            onChanged_();
    }
}

void ImplementationUiController::finishRun(int exitCode, int exitStatus) {
    if (!processBuffer_.isEmpty()) {
        appendProcessText(processBuffer_ + QLatin1Char('\n'));
        processBuffer_.clear();
    }
    appendProcessText(QStringLiteral("[%1] ice-cli finished with exit code %2%3.\n")
                          .arg(QString::fromUtf8(ice::implementationStageName(runningStage_)),
                               QString::number(exitCode),
                               exitStatus == QProcess::CrashExit ? QStringLiteral(" (crashed)") : QString()));
    if (process_) {
        process_->deleteLater();
        process_ = nullptr;
    }
    runningStage_ = ice::ImplementationStage::created;
    if (onProjectChanged_)
        onProjectChanged_();
    if (onRunFinished_)
        onRunFinished_(exitCode);
    if (onChanged_)
        onChanged_();
}

ImplementationPage::ImplementationPage(ice::ImplementationStage stage, ImplementationUiController *controller,
                                       QWidget *parent, bool embedded)
    : QWidget(parent), stage_(stage), controller_(controller), embedded_(embedded) {
    buildUi();
    refresh();
}

void ImplementationPage::buildUi() {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *content = new QWidget;
    auto *contentLayout = new QHBoxLayout(content);
    contentLayout->setSpacing(6);

    leftTabs_ = new QTabWidget;
    leftTabs_->setMinimumWidth(180);
    if (hdlMode() || synthesisMode()) {
        auto *explorerPage = new QWidget;
        auto *explorerLayout = new QVBoxLayout(explorerPage);
        explorerLayout->setContentsMargins(4, 4, 4, 4);
        explorerLayout->setSpacing(4);
        auto *searchRow = new QHBoxLayout;
        hdlSearch_ = new QLineEdit;
        hdlSearch_->setPlaceholderText(QStringLiteral("Search HDL files…"));
        hdlSearch_->setClearButtonEnabled(true);
        hdlSearch_->setAccessibleName(QStringLiteral("Search HDL files"));
        searchRow->addWidget(hdlSearch_, 1);
        if (hdlMode()) {
            newHdlButton_ = new QPushButton;
            newHdlButton_->setFixedWidth(28);
            setLucideIcon(newHdlButton_, QStringLiteral("plus"), 16);
            newHdlButton_->setToolTip(QStringLiteral("New HDL block"));
            newHdlButton_->setAccessibleName(QStringLiteral("New HDL block"));
            searchRow->addWidget(newHdlButton_);
            deleteHdlButton_ = new QPushButton;
            deleteHdlButton_->setFixedWidth(28);
            setLucideIcon(deleteHdlButton_, QStringLiteral("trash"), 16);
            deleteHdlButton_->setToolTip(QStringLiteral("Delete selected HDL file"));
            deleteHdlButton_->setAccessibleName(QStringLiteral("Delete selected HDL file"));
            deleteHdlButton_->setEnabled(false);
            searchRow->addWidget(deleteHdlButton_);
        } else if (synthesisMode()) {
            elaborateButton_ = new QPushButton(QStringLiteral("Elaborate design"));
            elaborateButton_->setToolTip(QStringLiteral("Create an HDL run and elaborate the selected design"));
            searchRow->addWidget(elaborateButton_);
        }
        explorerLayout->addLayout(searchRow);
        hdlFileList_ = new QListWidget;
        hdlFileList_->setAlternatingRowColors(true);
        if (synthesisMode()) {
            auto *splitter = new QSplitter(Qt::Vertical);
            splitter->addWidget(hdlFileList_);
            runsTabs_ = new QTabWidget;
            runsTabs_->setMinimumHeight(170);
            auto *runsPage = new QWidget;
            auto *runsLayout = new QVBoxLayout(runsPage);
            runsLayout->setContentsMargins(4, 4, 4, 4);
            runsLayout->setSpacing(4);
            runSearch_ = new QLineEdit;
            runSearch_->setPlaceholderText(QStringLiteral("Search runs…"));
            runSearch_->setClearButtonEnabled(true);
            runsLayout->addWidget(runSearch_);
            runsTable_ = new QTableWidget(0, 2);
            runsTable_->setHorizontalHeaderLabels({QStringLiteral("Run"), QStringLiteral("Stage")});
            runsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
            runsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            runsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
            runsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
            runsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
            runsLayout->addWidget(runsTable_, 1);
            synthesizeButton_ = new QPushButton(QStringLiteral("Synthesize"));
            showGraphButton_ = new QPushButton(QStringLiteral("Show graph"));
            exportVerilogButton_ = new QPushButton(QStringLiteral("Export .v"));
            removeRunButton_ = new QPushButton(QStringLiteral("Remove run"));
            removeRunButton_->setToolTip(QStringLiteral("Delete the selected run and its build directory"));
            auto *runActions = new QGridLayout;
            runActions->setSpacing(4);
            runActions->addWidget(synthesizeButton_, 0, 0, 1, 2);
            runActions->addWidget(showGraphButton_, 1, 0);
            runActions->addWidget(exportVerilogButton_, 1, 1);
            runActions->addWidget(removeRunButton_, 2, 0, 1, 2);
            runsLayout->addLayout(runActions);
            runsTabs_->addTab(runsPage, QStringLiteral("Runs"));
            splitter->addWidget(runsTabs_);
            splitter->setStretchFactor(0, 1);
            splitter->setStretchFactor(1, 0);
            explorerLayout->addWidget(splitter, 1);
        } else {
            explorerLayout->addWidget(hdlFileList_, 1);
        }
        leftTabs_->addTab(explorerPage, QStringLiteral("Explorer"));
    } else {
        if (floorplanMode()) {
            netlistList_ = new QListWidget;
            netlistList_->setAlternatingRowColors(true);
            netlistList_->setToolTip(QStringLiteral("Technology-mapped netlists exported from Synthesis runs"));
        }
        runTree_ = new QTreeWidget;
        runTree_->setHeaderLabels({QStringLiteral("Run / Stage"), QStringLiteral("Status")});
        runTree_->setRootIsDecorated(true);
        runTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        runTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        QWidget *runsPage = nullptr;
        if (physicalStageMode()) {
            runsPage = new QWidget;
            auto *runsLayout = new QVBoxLayout(runsPage);
            runsLayout->setContentsMargins(4, 4, 4, 4);
            runsLayout->setSpacing(4);
            if (embedded_) {
                auto *runRow = new QHBoxLayout;
                runRow->setSpacing(3);
                runCombo_ = new QComboBox;
                runCombo_->setMinimumContentsLength(8);
                runCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                runCombo_->setToolTip(QStringLiteral("Active implementation run"));
                runCombo_->setAccessibleName(QStringLiteral("Active implementation run"));
                runRow->addWidget(runCombo_, 1);

                const auto addIconButton = [&](const QString &icon, const QString &tooltip) {
                    auto *button = new QPushButton;
                    button->setFlat(true);
                    button->setFixedSize(22, 22);
                    button->setToolTip(tooltip);
                    button->setAccessibleName(tooltip);
                    setLucideIcon(button, icon, 12);
                    runRow->addWidget(button);
                    return button;
                };
                runStageButton_ = addIconButton(QStringLiteral("play"), QStringLiteral("Run selected stage"));
                runFromHereButton_ =
                    addIconButton(QStringLiteral("arrow-down"), QStringLiteral("Run from this stage through final"));
                runAllButton_ =
                    addIconButton(QStringLiteral("grid-3x3"), QStringLiteral("Run floorplan through final"));
                cancelStageButton_ = addIconButton(QStringLiteral("x"), QStringLiteral("Cancel the active process"));
                cancelStageButton_->setEnabled(false);
                runsLayout->addLayout(runRow);

                stageHintLabel_ = new QLabel;
                stageHintLabel_->setObjectName(QStringLiteral("physicalStageHint"));
                stageHintLabel_->setWordWrap(true);
                stageHintLabel_->setContentsMargins(1, 0, 1, 2);
                auto hintFont = stageHintLabel_->font();
                hintFont.setPointSizeF(std::max(7.0, hintFont.pointSizeF() - 1.0));
                stageHintLabel_->setFont(hintFont);
                runsLayout->addWidget(stageHintLabel_);
                auto *openGuiButton = new QPushButton(QStringLiteral("Open in OpenROAD GUI"));
                openGuiButton->setToolTip(QStringLiteral("Open the active stage ODB in the standalone OpenROAD GUI"));
                runsLayout->addWidget(openGuiButton);
                QObject::connect(openGuiButton, &QPushButton::clicked, this, [this] {
                    QString error;
                    if (!controller_->openInOpenRoadGui(stage_, error))
                        QMessageBox::warning(this, QStringLiteral("OpenROAD GUI"), error);
                });
                if (stage_ == ice::ImplementationStage::final) {
                    auto *importButton = new QPushButton(QStringLiteral("Import final layout"));
                    importButton->setToolTip(QStringLiteral("Import the final DEF as a layout cell"));
                    runsLayout->addWidget(importButton);
                    QObject::connect(importButton, &QPushButton::clicked, this, [this] {
                        const auto id = controller_->activeRunId();
                        if (id == 0)
                            return;
                        QString error;
                        if (!controller_->importLayout(id, error))
                            QMessageBox::warning(this, QStringLiteral("Import layout"), error);
                        else
                            QMessageBox::information(this, QStringLiteral("Import layout"),
                                                     QStringLiteral("The final layout was imported."));
                    });
                }
            }
            runsLayout->addWidget(runTree_, 1);
            if (!embedded_) {
                auto *runActions = new QHBoxLayout;
                runActions->setSpacing(4);
                const auto runLabel =
                    stage_ == ice::ImplementationStage::cts ? QStringLiteral("CTS") : stageTitle(stage_).toLower();
                runStageButton_ = new QPushButton(QStringLiteral("Run %1").arg(runLabel));
                cancelStageButton_ = new QPushButton(QStringLiteral("Cancel"));
                cancelStageButton_->setEnabled(false);
                runActions->addWidget(runStageButton_, 1);
                runActions->addWidget(cancelStageButton_);
                runsLayout->addLayout(runActions);
            }
            leftTabs_->addTab(runsPage, QStringLiteral("Runs"));

            auto *displayPage = new QWidget;
            auto *displayLayout = new QVBoxLayout(displayPage);
            displayLayout->setContentsMargins(4, 4, 4, 4);
            displayLayout->setSpacing(4);
            auto *displayHint = new QLabel(QStringLiteral("Toggle layer visibility"));
            displayHint->setContentsMargins(1, 0, 1, 0);
            displayLayout->addWidget(displayHint);
            displayTree_ = new QTreeWidget;
            displayTree_->setHeaderHidden(true);
            displayTree_->setAlternatingRowColors(true);
            displayTree_->setToolTip(QStringLiteral("Visibility filters for the active stage preview"));
            displayLayout->addWidget(displayTree_, 1);
            leftTabs_->addTab(displayPage, QStringLiteral("Display"));
        } else {
            leftTabs_->addTab(runTree_, QStringLiteral("Runs"));
        }
        if (netlistList_)
            leftTabs_->addTab(netlistList_, QStringLiteral("Netlists"));
        artifactsList_ = new QListWidget;
        leftTabs_->addTab(artifactsList_, QStringLiteral("Artifacts"));
        if (runsPage && embedded_)
            leftTabs_->setCurrentWidget(runsPage);
        else if (netlistList_)
            leftTabs_->setCurrentWidget(netlistList_);
    }
    contentLayout->addWidget(leftTabs_, 0);

    middleTabs_ = new QTabWidget;
    middleTabs_->setDocumentMode(true);
    if (hdlMode()) {
        middleTabs_->setTabsClosable(true);
        QObject::connect(middleTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (!middleTabs_ || index < 0)
                return;
            const auto id = static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong());
            if (id == 0)
                return;
            if (activeHdlCellId_ != id) {
                flushHdlEdit();
                activeHdlCellId_ = id;
                hdlEditor_ = hdlEditorFor(id);
                refreshProperties();
            }
        });
        QObject::connect(middleTabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
            if (!middleTabs_ || index < 0)
                return;
            const auto id = static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong());
            if (id != 0)
                closeHdlFile(id);
        });
    } else if (synthesisMode()) {
        middleTabs_->setTabsClosable(true);
        QObject::connect(middleTabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
            if (!middleTabs_ || index < 0 || index >= middleTabs_->count())
                return;
            auto *widget = middleTabs_->widget(index);
            if (widget)
                closeArtifactTab(widget);
        });
    } else {
        middleTabs_->setTabsClosable(true);
        QObject::connect(middleTabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
            if (!middleTabs_ || index < 0 || index >= middleTabs_->count())
                return;
            auto *widget = middleTabs_->widget(index);
            if (widget)
                closeArtifactTab(widget);
        });
        auto *previewPage = new QWidget;
        auto *previewLayout = new QVBoxLayout(previewPage);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(2);
        auto *overlayRow = new QHBoxLayout;
        overlayRow->setContentsMargins(6, 4, 6, 0);
        overlayRow->addWidget(new QLabel(QStringLiteral("Heat map")));
        heatMapCombo_ = new QComboBox;
        heatMapCombo_->addItem(QStringLiteral("None"), static_cast<int>(ice::LayoutHeatMapKind::none));
        heatMapCombo_->addItem(QStringLiteral("Placement density"),
                               static_cast<int>(ice::LayoutHeatMapKind::placement_density));
        heatMapCombo_->addItem(QStringLiteral("Routing congestion"),
                               static_cast<int>(ice::LayoutHeatMapKind::routing_congestion));
        heatMapCombo_->addItem(QStringLiteral("Power density (export only)"), 100);
        heatMapCombo_->addItem(QStringLiteral("Pin density (export only)"), 101);
        heatMapCombo_->addItem(QStringLiteral("RUDY congestion (export only)"), 102);
        heatMapCombo_->addItem(QStringLiteral("IR drop (export only)"), 103);
        heatMapCombo_->addItem(QStringLiteral("Loaded CSV"), 200);
        overlayRow->addWidget(heatMapCombo_);
        auto *loadHeatMapButton = new QPushButton(QStringLiteral("Load CSV"));
        loadHeatMapButton->setToolTip(QStringLiteral("Load an OpenROAD heat-map CSV and display it"));
        overlayRow->addWidget(loadHeatMapButton);
        auto *exportHeatMapButton = new QPushButton(QStringLiteral("Export CSV"));
        exportHeatMapButton->setToolTip(
            QStringLiteral("Write the selected OpenROAD heat map under the run reports directory."));
        overlayRow->addWidget(exportHeatMapButton);
        auto *heatMapSettingsButton = new QPushButton(QStringLiteral("Settings"));
        heatMapSettingsButton->setToolTip(QStringLiteral("Configure heat-map scale, range, and legend"));
        overlayRow->addWidget(heatMapSettingsButton);
        overlayRow->addStretch(1);
        previewLayout->addLayout(overlayRow);
        previewStatusLabel_ = new QLabel;
        previewStatusLabel_->setWordWrap(true);
        previewStatusLabel_->setContentsMargins(6, 0, 6, 2);
        previewStatusLabel_->setStyleSheet(QStringLiteral("color: #a16207;"));
        previewStatusLabel_->setVisible(false);
        previewLayout->addWidget(previewStatusLabel_);
        QObject::connect(loadHeatMapButton, &QPushButton::clicked, this, [this] {
            if (!previewCanvas_ || !heatMapCombo_)
                return;
            const auto path =
                QFileDialog::getOpenFileName(this, QStringLiteral("Load heat map"), controller_->projectRoot(),
                                             QStringLiteral("CSV files (*.csv);;All files (*)"));
            if (path.isEmpty())
                return;
            auto heatmap = parseHeatMapCsv(path.toStdString());
            if (!heatmap) {
                QMessageBox::warning(this, QStringLiteral("Load heat map"),
                                     QStringLiteral("Unable to parse a heat-map CSV with x0,y0,x1,y1,value columns."));
                return;
            }
            previewCanvas_->setCustomHeatMap(std::move(heatmap));
            heatMapCombo_->setCurrentIndex(heatMapCombo_->findData(200));
        });
        QObject::connect(exportHeatMapButton, &QPushButton::clicked, this, [this] {
            if (!heatMapCombo_)
                return;
            const auto data = heatMapCombo_->currentData().toInt();
            QString kind;
            if (data == static_cast<int>(ice::LayoutHeatMapKind::placement_density))
                kind = QStringLiteral("placement");
            else if (data == static_cast<int>(ice::LayoutHeatMapKind::routing_congestion))
                kind = QStringLiteral("routing");
            else if (data == 100)
                kind = QStringLiteral("power");
            else if (data == 101)
                kind = QStringLiteral("pin");
            else if (data == 102)
                kind = QStringLiteral("rudy");
            else if (data == 103)
                kind = QStringLiteral("irdrop");
            if (kind.isEmpty()) {
                QMessageBox::information(this, QStringLiteral("Export heat map"),
                                         QStringLiteral("Select a heat map to export."));
                return;
            }
            if (previewStatusLabel_) {
                previewStatusLabel_->setText(QStringLiteral("Exporting heat map…"));
                previewStatusLabel_->setStyleSheet(
                    QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
                previewStatusLabel_->setVisible(true);
            }
            QPointer<ImplementationPage> self(this);
            controller_->requestExportHeatMap(stage_, kind, [self](const QString &path, const QString &error) {
                if (!self)
                    return;
                if (self->previewStatusLabel_)
                    self->previewStatusLabel_->setVisible(false);
                if (path.isEmpty()) {
                    QMessageBox::warning(self, QStringLiteral("Export heat map"), error);
                    return;
                }
                self->refreshArtifacts();
                QMessageBox::information(self, QStringLiteral("Export heat map"),
                                         QStringLiteral("Wrote %1.").arg(path));
            });
        });
        QObject::connect(heatMapSettingsButton, &QPushButton::clicked, this, [this] {
            if (!previewCanvas_)
                return;
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Heat-map settings"));
            auto *layout = new QVBoxLayout(&dialog);
            auto *form = new QFormLayout;
            auto *logScale = new QCheckBox(QStringLiteral("Log scale"));
            auto *reverse = new QCheckBox(QStringLiteral("Reverse scale"));
            auto *legend = new QCheckBox(QStringLiteral("Show legend"));
            auto *automaticRange = new QCheckBox(QStringLiteral("Automatic range"));
            auto *minimum = new QDoubleSpinBox;
            auto *maximum = new QDoubleSpinBox;
            auto *alpha = new QSpinBox;
            minimum->setRange(-1.0e9, 1.0e9);
            maximum->setRange(-1.0e9, 1.0e9);
            minimum->setDecimals(4);
            maximum->setDecimals(4);
            alpha->setRange(0, 255);
            const auto options = previewCanvas_->heatMapOptions();
            logScale->setChecked(options.logScale);
            reverse->setChecked(options.reverse);
            legend->setChecked(options.showLegend);
            automaticRange->setChecked(!options.minimum.has_value() && !options.maximum.has_value());
            if (const auto heatmap = previewCanvas_->currentHeatMap()) {
                minimum->setValue(options.minimum.value_or(heatmap->minimum));
                maximum->setValue(options.maximum.value_or(heatmap->maximum));
            }
            alpha->setValue(options.alpha);
            minimum->setEnabled(!automaticRange->isChecked());
            maximum->setEnabled(!automaticRange->isChecked());
            QObject::connect(automaticRange, &QCheckBox::toggled, minimum, &QWidget::setDisabled);
            QObject::connect(automaticRange, &QCheckBox::toggled, maximum, &QWidget::setDisabled);
            form->addRow(logScale);
            form->addRow(reverse);
            form->addRow(legend);
            form->addRow(automaticRange);
            form->addRow(QStringLiteral("Minimum"), minimum);
            form->addRow(QStringLiteral("Maximum"), maximum);
            form->addRow(QStringLiteral("Alpha"), alpha);
            layout->addLayout(form);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            if (dialog.exec() != QDialog::Accepted)
                return;
            ImplementationPreviewCanvas::HeatMapOptions updated;
            updated.logScale = logScale->isChecked();
            updated.reverse = reverse->isChecked();
            updated.showLegend = legend->isChecked();
            updated.alpha = alpha->value();
            if (!automaticRange->isChecked()) {
                updated.minimum = minimum->value();
                updated.maximum = maximum->value();
            }
            previewCanvas_->setHeatMapOptions(updated);
        });
        previewCanvas_ = new ImplementationPreviewCanvas;
        previewLayout->addWidget(previewCanvas_, 1);
        auto *toolRow = new QHBoxLayout;
        toolRow->setContentsMargins(6, 4, 6, 0);
        const auto addToolButton = [&](const QString &text, const QString &tooltip) {
            auto *button = new QPushButton(text);
            button->setFlat(true);
            button->setToolTip(tooltip);
            toolRow->addWidget(button);
            return button;
        };
        auto *fitButton = addToolButton(QStringLiteral("Fit"), QStringLiteral("Fit the layout"));
        auto *zoomInButton = addToolButton(QStringLiteral("+"), QStringLiteral("Zoom in"));
        auto *zoomOutButton = addToolButton(QStringLiteral("−"), QStringLiteral("Zoom out"));
        auto *rulerButton = addToolButton(QStringLiteral("Ruler"), QStringLiteral("Draw a ruler"));
        rulerButton->setCheckable(true);
        auto *labelButton = addToolButton(QStringLiteral("Label"), QStringLiteral("Place a text label"));
        labelButton->setCheckable(true);
        auto *clearAnnotationsButton =
            addToolButton(QStringLiteral("Clear"), QStringLiteral("Clear rulers and labels"));
        auto *saveImageButton = addToolButton(QStringLiteral("Save image"), QStringLiteral("Save the visible preview"));
        auto *findButton = addToolButton(QStringLiteral("Find"), QStringLiteral("Find an instance, port, or net"));
        toolRow->addStretch(1);
        previewLayout->insertLayout(0, toolRow);
        previewCoordinateLabel_ = new QLabel;
        previewCoordinateLabel_->setContentsMargins(6, 0, 6, 2);
        previewCoordinateLabel_->setStyleSheet(
            QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
        previewLayout->addWidget(previewCoordinateLabel_);
        QObject::connect(fitButton, &QPushButton::clicked, this, [this] {
            if (previewCanvas_)
                previewCanvas_->fitView();
        });
        QObject::connect(zoomInButton, &QPushButton::clicked, this, [this] {
            if (previewCanvas_)
                previewCanvas_->zoomIn();
        });
        QObject::connect(zoomOutButton, &QPushButton::clicked, this, [this] {
            if (previewCanvas_)
                previewCanvas_->zoomOut();
        });
        QObject::connect(rulerButton, &QPushButton::toggled, this, [this, labelButton](bool checked) {
            if (!previewCanvas_)
                return;
            if (checked)
                labelButton->setChecked(false);
            previewCanvas_->setTool(checked ? ImplementationPreviewCanvas::Tool::ruler
                                            : ImplementationPreviewCanvas::Tool::none);
        });
        QObject::connect(labelButton, &QPushButton::toggled, this, [this, rulerButton](bool checked) {
            if (!previewCanvas_)
                return;
            if (checked)
                rulerButton->setChecked(false);
            previewCanvas_->setTool(checked ? ImplementationPreviewCanvas::Tool::label
                                            : ImplementationPreviewCanvas::Tool::none);
        });
        QObject::connect(clearAnnotationsButton, &QPushButton::clicked, this, [this] {
            if (previewCanvas_)
                previewCanvas_->clearAnnotations();
        });
        QObject::connect(saveImageButton, &QPushButton::clicked, this, [this] {
            if (!previewCanvas_)
                return;
            const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Save preview"),
                                                           QStringLiteral("physical-preview.png"),
                                                           QStringLiteral("PNG image (*.png);;All files (*)"));
            if (!path.isEmpty())
                previewCanvas_->saveImage(path);
        });
        QObject::connect(findButton, &QPushButton::clicked, this, [this] {
            if (!previewCanvas_)
                return;
            bool accepted = false;
            const auto name =
                QInputDialog::getText(this, QStringLiteral("Find object"),
                                      QStringLiteral("Instance, port, or net name:"), QLineEdit::Normal, {}, &accepted);
            if (!accepted || name.isEmpty())
                return;
            if (!previewCanvas_->selectByName(name))
                QMessageBox::information(this, QStringLiteral("Find object"),
                                         QStringLiteral("No object named %1 was found.").arg(name));
        });
        previewCanvas_->setStatusCallback([this](const QPointF &point, const QString &hint) {
            if (!previewCoordinateLabel_)
                return;
            if (point.isNull()) {
                previewCoordinateLabel_->clear();
                return;
            }
            QString text = QStringLiteral("x %1 µm   y %2 µm")
                               .arg(point.x() / 1000.0, 0, 'f', 3)
                               .arg(point.y() / 1000.0, 0, 'f', 3);
            if (!hint.isEmpty())
                text += QStringLiteral("   ·   ") + hint;
            previewCoordinateLabel_->setText(text);
        });
        previewCanvas_->setSelectionCallback(
            [this](const ImplementationPreviewCanvas::Selection &) { refreshInspector(); });
        QObject::connect(heatMapCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (!previewCanvas_ || !heatMapCombo_)
                return;
            const auto data = heatMapCombo_->currentData().toInt();
            if (data == 200) {
                if (!previewCanvas_->hasCustomHeatMap())
                    previewCanvas_->setHeatMap(ice::LayoutHeatMapKind::none);
                return;
            }
            if (data >= 100) {
                previewCanvas_->setHeatMap(ice::LayoutHeatMapKind::none);
                return;
            }
            previewCanvas_->setHeatMap(static_cast<ice::LayoutHeatMapKind>(data));
        });
        const auto previewIndex = middleTabs_->addTab(previewPage, QStringLiteral("Preview"));
        middleTabs_->tabBar()->setTabButton(previewIndex, QTabBar::RightSide, nullptr);
    }
    contentLayout->addWidget(middleTabs_, 1);

    rightTabs_ = new QTabWidget;
    rightTabs_->setMinimumWidth(240);
    if (hdlMode()) {
        auto *outlinePage = new QWidget;
        auto *outlineLayout = new QVBoxLayout(outlinePage);
        outlineLayout->setContentsMargins(4, 4, 4, 4);
        outlineLayout->setSpacing(4);
        outlineTree_ = new QTreeWidget;
        outlineTree_->setHeaderLabels({QStringLiteral("Outline"), QStringLiteral("Line")});
        outlineTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        outlineTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        outlineLayout->addWidget(outlineTree_, 1);
        rightTabs_->addTab(outlinePage, QStringLiteral("Outline"));
        auto *ipPage = new QWidget;
        auto *ipLayout = new QVBoxLayout(ipPage);
        ipLayout->setContentsMargins(4, 4, 4, 4);
        ipLayout->setSpacing(4);
        ipLibrariesTree_ = new QTreeWidget;
        ipLibrariesTree_->setHeaderLabels({QStringLiteral("IP / Library"), QStringLiteral("Details")});
        ipLibrariesTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        ipLibrariesTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        ipLayout->addWidget(ipLibrariesTree_, 1);
        rightTabs_->addTab(ipPage, QStringLiteral("IP/Libraries"));
        QObject::connect(outlineTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
            if (!item)
                return;
            const auto line = item->data(0, Qt::UserRole).toInt();
            if (line <= 0)
                return;
            if (auto *editor = hdlEditorFor(activeHdlCellId_))
                editor->goToLine(line);
        });
        QObject::connect(outlineTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
            if (!item)
                return;
            const auto line = item->data(0, Qt::UserRole).toInt();
            if (line <= 0)
                return;
            if (auto *editor = hdlEditorFor(activeHdlCellId_))
                editor->goToLine(line);
        });
        QObject::connect(ipLibrariesTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
            if (!item)
                return;
            const auto id = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
            if (id != 0)
                openHdlFile(id);
        });
        QObject::connect(ipLibrariesTree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
            if (!item)
                return;
            const auto id = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
            if (id != 0)
                openHdlFile(id);
        });
    } else if (synthesisMode()) {
        auto *artifactsPage = new QWidget;
        auto *artifactsLayout = new QVBoxLayout(artifactsPage);
        artifactsLayout->setContentsMargins(4, 4, 4, 4);
        artifactsLayout->setSpacing(4);
        artifactsList_ = new QListWidget;
        artifactsList_->setAlternatingRowColors(true);
        artifactsLayout->addWidget(artifactsList_, 1);
        rightTabs_->addTab(artifactsPage, QStringLiteral("Artifacts"));
    } else {
        propertiesTree_ = new QTreeWidget;
        propertiesTree_->setHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
        propertiesTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        propertiesTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        rightTabs_->addTab(propertiesTree_, QStringLiteral("Properties"));
        metricsTable_ = new QTableWidget(0, 2);
        metricsTable_->setHorizontalHeaderLabels({QStringLiteral("Metric"), QStringLiteral("Value")});
        metricsTable_->horizontalHeader()->setStretchLastSection(false);
        metricsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        metricsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        metricsTable_->verticalHeader()->setVisible(false);
        metricsTable_->setAlternatingRowColors(true);
        metricsTable_->setWordWrap(false);
        metricsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        rightTabs_->addTab(metricsTable_, QStringLiteral("Metrics"));
        inspectorTree_ = new QTreeWidget;
        inspectorTree_->setHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
        inspectorTree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        inspectorTree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        rightTabs_->addTab(inspectorTree_, QStringLiteral("Inspector"));
        drcTable_ = new QTableWidget(0, 3);
        drcTable_->setHorizontalHeaderLabels(
            {QStringLiteral("Type"), QStringLiteral("Layer"), QStringLiteral("Sources")});
        drcTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        drcTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        drcTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        drcTable_->verticalHeader()->setVisible(false);
        drcTable_->setAlternatingRowColors(true);
        drcTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        rightTabs_->addTab(drcTable_, QStringLiteral("DRC"));
        timingTable_ = new QTableWidget(0, 2);
        timingTable_->setHorizontalHeaderLabels({QStringLiteral("Metric"), QStringLiteral("Value")});
        timingTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        timingTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        timingTable_->verticalHeader()->setVisible(false);
        timingTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        auto *timingPage = new QWidget;
        auto *timingLayout = new QVBoxLayout(timingPage);
        timingLayout->setContentsMargins(4, 4, 4, 4);
        auto *timingButton = new QPushButton(QStringLiteral("Generate timing report"));
        timingButton->setToolTip(QStringLiteral("Run OpenROAD STA and show the worst setup/hold paths"));
        timingLayout->addWidget(timingButton);
        timingLayout->addWidget(timingTable_, 1);
        timingReportView_ = new QPlainTextEdit;
        timingReportView_->setReadOnly(true);
        timingReportView_->setPlaceholderText(QStringLiteral("Generate a report to inspect worst paths."));
        timingLayout->addWidget(timingReportView_, 1);
        rightTabs_->addTab(timingPage, QStringLiteral("Timing"));
        QObject::connect(timingButton, &QPushButton::clicked, this, [this] {
            QString error;
            const auto report = controller_->timingReport(stage_, error);
            if (timingReportView_)
                timingReportView_->setPlainText(report.isEmpty() ? error : report);
            if (!error.isEmpty())
                QMessageBox::warning(this, QStringLiteral("Timing report"), error);
        });
        clockTree_ = new QTreeWidget;
        clockTree_->setHeaderLabels({QStringLiteral("Clock net"), QStringLiteral("Pins")});
        clockTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        clockTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        rightTabs_->addTab(clockTree_, QStringLiteral("Clock"));
        hierarchyTree_ = new QTreeWidget;
        hierarchyTree_->setHeaderLabels({QStringLiteral("Module"), QStringLiteral("Instances")});
        hierarchyTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        hierarchyTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        rightTabs_->addTab(hierarchyTree_, QStringLiteral("Hierarchy"));
        if (physicalStageMode()) {
            auto *formPage = new QWidget;
            auto *outer = new QVBoxLayout(formPage);
            outer->setContentsMargins(6, 6, 6, 6);
            outer->setSpacing(6);
            auto *form = new QFormLayout;
            if (floorplanMode()) {
                utilizationSpin_ = new QDoubleSpinBox;
                utilizationSpin_->setRange(0.01, 0.99);
                utilizationSpin_->setSingleStep(0.05);
                utilizationSpin_->setDecimals(2);
                form->addRow(QStringLiteral("Utilization"), utilizationSpin_);
                aspectRatioSpin_ = new QDoubleSpinBox;
                aspectRatioSpin_->setRange(0.1, 10.0);
                aspectRatioSpin_->setSingleStep(0.1);
                aspectRatioSpin_->setDecimals(2);
                form->addRow(QStringLiteral("Aspect ratio"), aspectRatioSpin_);
                dieAreaCheck_ = new QCheckBox(QStringLiteral("Explicit die area"));
                form->addRow(QString(), dieAreaCheck_);
                dieWidthSpin_ = new QDoubleSpinBox;
                dieWidthSpin_->setRange(0.001, 100000.0);
                dieWidthSpin_->setDecimals(3);
                dieWidthSpin_->setValue(100.0);
                dieWidthSpin_->setSuffix(QStringLiteral(" µm"));
                dieWidthSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Die width"), dieWidthSpin_);
                dieHeightSpin_ = new QDoubleSpinBox;
                dieHeightSpin_->setRange(0.001, 100000.0);
                dieHeightSpin_->setDecimals(3);
                dieHeightSpin_->setValue(100.0);
                dieHeightSpin_->setSuffix(QStringLiteral(" µm"));
                dieHeightSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Die height"), dieHeightSpin_);
                dieXSpin_ = new QDoubleSpinBox;
                dieXSpin_->setRange(-100000.0, 100000.0);
                dieXSpin_->setDecimals(3);
                dieXSpin_->setSuffix(QStringLiteral(" µm"));
                dieXSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Die X"), dieXSpin_);
                dieYSpin_ = new QDoubleSpinBox;
                dieYSpin_->setRange(-100000.0, 100000.0);
                dieYSpin_->setDecimals(3);
                dieYSpin_->setSuffix(QStringLiteral(" µm"));
                dieYSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Die Y"), dieYSpin_);
                QObject::connect(dieAreaCheck_, &QCheckBox::toggled, this, [this](bool checked) {
                    for (auto *spin : {dieWidthSpin_, dieHeightSpin_, dieXSpin_, dieYSpin_})
                        if (spin)
                            spin->setEnabled(checked);
                });
                coreAreaCheck_ = new QCheckBox(QStringLiteral("Explicit core area"));
                form->addRow(QString(), coreAreaCheck_);
                coreWidthSpin_ = new QDoubleSpinBox;
                coreWidthSpin_->setRange(0.001, 100000.0);
                coreWidthSpin_->setDecimals(3);
                coreWidthSpin_->setSuffix(QStringLiteral(" µm"));
                coreWidthSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Core width"), coreWidthSpin_);
                coreHeightSpin_ = new QDoubleSpinBox;
                coreHeightSpin_->setRange(0.001, 100000.0);
                coreHeightSpin_->setDecimals(3);
                coreHeightSpin_->setSuffix(QStringLiteral(" µm"));
                coreHeightSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Core height"), coreHeightSpin_);
                coreXSpin_ = new QDoubleSpinBox;
                coreXSpin_->setRange(-100000.0, 100000.0);
                coreXSpin_->setDecimals(3);
                coreXSpin_->setSuffix(QStringLiteral(" µm"));
                coreXSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Core X"), coreXSpin_);
                coreYSpin_ = new QDoubleSpinBox;
                coreYSpin_->setRange(-100000.0, 100000.0);
                coreYSpin_->setDecimals(3);
                coreYSpin_->setSuffix(QStringLiteral(" µm"));
                coreYSpin_->setEnabled(false);
                form->addRow(QStringLiteral("Core Y"), coreYSpin_);
                QObject::connect(coreAreaCheck_, &QCheckBox::toggled, this, [this](bool checked) {
                    for (auto *spin : {coreWidthSpin_, coreHeightSpin_, coreXSpin_, coreYSpin_})
                        if (spin)
                            spin->setEnabled(checked);
                });
                siteEdit_ = new QLineEdit;
                siteEdit_->setPlaceholderText(QStringLiteral("Platform default"));
                form->addRow(QStringLiteral("Placement site"), siteEdit_);
                ioHorizontalEdit_ = new QLineEdit;
                ioHorizontalEdit_->setPlaceholderText(QStringLiteral("Platform default, e.g. Metal3"));
                form->addRow(QStringLiteral("IO horizontal"), ioHorizontalEdit_);
                ioVerticalEdit_ = new QLineEdit;
                ioVerticalEdit_->setPlaceholderText(QStringLiteral("Platform default, e.g. Metal2"));
                form->addRow(QStringLiteral("IO vertical"), ioVerticalEdit_);
            } else if (stage_ == ice::ImplementationStage::placement) {
                placementDensitySpin_ = new QDoubleSpinBox;
                placementDensitySpin_->setRange(0.0, 1.0);
                placementDensitySpin_->setSingleStep(0.05);
                placementDensitySpin_->setDecimals(2);
                placementDensitySpin_->setSpecialValueText(QStringLiteral("Platform default"));
                form->addRow(QStringLiteral("Density"), placementDensitySpin_);
                timingDrivenCheck_ = new QCheckBox(QStringLiteral("Timing driven"));
                form->addRow(QString(), timingDrivenCheck_);
                routabilityDrivenCheck_ = new QCheckBox(QStringLiteral("Routability driven"));
                form->addRow(QString(), routabilityDrivenCheck_);
            } else if (stage_ == ice::ImplementationStage::cts) {
                ctsEnabledCheck_ = new QCheckBox(QStringLiteral("Run clock-tree synthesis"));
                form->addRow(QString(), ctsEnabledCheck_);
                ctsRootBufferEdit_ = new QLineEdit;
                ctsRootBufferEdit_->setPlaceholderText(QStringLiteral("Platform default"));
                form->addRow(QStringLiteral("Root buffer"), ctsRootBufferEdit_);
                ctsBuffersEdit_ = new QLineEdit;
                ctsBuffersEdit_->setPlaceholderText(QStringLiteral("Comma-separated, e.g. BUF_X1, BUF_X2"));
                form->addRow(QStringLiteral("Clock buffers"), ctsBuffersEdit_);
            } else if (stage_ == ice::ImplementationStage::routing) {
                routingMinLayerEdit_ = new QLineEdit;
                routingMinLayerEdit_->setPlaceholderText(QStringLiteral("Platform default"));
                form->addRow(QStringLiteral("Routing min layer"), routingMinLayerEdit_);
                routingMaxLayerEdit_ = new QLineEdit;
                routingMaxLayerEdit_->setPlaceholderText(QStringLiteral("Platform default"));
                form->addRow(QStringLiteral("Routing max layer"), routingMaxLayerEdit_);
                antennaRepairCheck_ = new QCheckBox(QStringLiteral("Antenna repair"));
                form->addRow(QString(), antennaRepairCheck_);
            }
            threadsSpin_ = new QSpinBox;
            threadsSpin_->setRange(1, 64);
            form->addRow(QStringLiteral("Threads"), threadsSpin_);
            timeoutSpin_ = new QSpinBox;
            timeoutSpin_->setRange(1, 86400);
            timeoutSpin_->setSuffix(QStringLiteral(" s"));
            form->addRow(QStringLiteral("Timeout"), timeoutSpin_);
            outer->addLayout(form);
            applyStageButton_ = new QPushButton(QStringLiteral("Apply %1 settings").arg(stageTitle(stage_)));
            outer->addWidget(applyStageButton_);
            outer->addStretch(1);
            const auto settingsIndex = rightTabs_->addTab(formPage, stageTitle(stage_));
            rightTabs_->tabBar()->moveTab(settingsIndex, 0);
            rightTabs_->setCurrentIndex(0);
        }
    }
    contentLayout->addWidget(rightTabs_, 0);

    layout->addWidget(content, 1);

    if (hdlMode()) {
        hdlEditTimer_ = new QTimer(this);
        hdlEditTimer_->setSingleShot(true);
        hdlEditTimer_->setInterval(400);
        QObject::connect(hdlEditTimer_, &QTimer::timeout, this, [this] {
            if (hdlEditCellId_ == 0 || loadingHdlEditor_)
                return;
            if (auto *editor = hdlEditorFor(hdlEditCellId_))
                controller_->editHdlText(hdlEditCellId_, editor->text().toStdString());
            refreshOutline();
        });
        QObject::connect(newHdlButton_, &QPushButton::clicked, this, [this] {
            if (createHdlBlockCallback_)
                createHdlBlockCallback_();
        });
        QObject::connect(deleteHdlButton_, &QPushButton::clicked, this, [this] {
            auto *item = hdlFileList_ ? hdlFileList_->currentItem() : nullptr;
            if (!item || item->isHidden())
                return;
            const auto id = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
            if (id != 0 && deleteHdlBlockCallback_)
                deleteHdlBlockCallback_(id);
        });
        QObject::connect(hdlSearch_, &QLineEdit::textChanged, this,
                         [this](const QString &text) { applyHdlFilter(text); });
        QObject::connect(hdlFileList_, &QListWidget::currentRowChanged, this, [this](int row) {
            deleteHdlButton_->setEnabled(row >= 0);
            if (row < 0 || !hdlFileList_)
                return;
            auto *item = hdlFileList_->item(row);
            if (!item)
                return;
            const auto id = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
            const auto *project = controller_->project();
            if (!project)
                return;
            const auto cell = project->schematicCells.find(id);
            if (cell == project->schematicCells.end() || !cell->second.systemVerilogSchematic)
                return;
            flushHdlEdit();
            activeHdlCellId_ = id;
            openHdlFile(id);
            refreshProperties();
        });
    } else if (synthesisMode()) {
        QObject::connect(hdlSearch_, &QLineEdit::textChanged, this,
                         [this](const QString &text) { applyHdlFilter(text); });
        QObject::connect(hdlFileList_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row < 0 || !hdlFileList_)
                return;
            auto *item = hdlFileList_->item(row);
            if (!item)
                return;
            activeHdlCellId_ = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
        });
        QObject::connect(elaborateButton_, &QPushButton::clicked, this, [this] {
            if (activeHdlCellId_ == 0) {
                QMessageBox::information(this, QStringLiteral("Elaborate Design"),
                                         QStringLiteral("Select an HDL file in the Explorer first."));
                return;
            }
            const auto pdk = promptPdk(QStringLiteral("Elaborate Design"), {});
            if (!pdk || !elaborateDesignCallback_)
                return;
            elaborateDesignCallback_(activeHdlCellId_, *pdk);
        });
        QObject::connect(runSearch_, &QLineEdit::textChanged, this,
                         [this](const QString &text) { applyRunFilter(text); });
        QObject::connect(runsTable_, &QTableWidget::itemSelectionChanged, this, [this] {
            const auto id = selectedRunId();
            if (id != 0) {
                controller_->setActiveRunId(id);
                refreshArtifacts();
            }
            updateRunActions();
        });
        QObject::connect(artifactsList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item)
                return;
            const auto path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                openArtifact(path);
        });
        QObject::connect(runsTable_, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
            const auto id = selectedRunId();
            if (id != 0)
                showRunGraph(id);
        });
        QObject::connect(synthesizeButton_, &QPushButton::clicked, this, [this] {
            const auto id = actionRunId();
            const auto *run = id != 0 && controller_->project()
                                  ? (controller_->project()->implementationRuns.contains(id)
                                         ? &controller_->project()->implementationRuns.at(id)
                                         : nullptr)
                                  : nullptr;
            if (!run || !synthesizeRunCallback_)
                return;
            const auto pdk = promptPdk(QStringLiteral("Synthesize Design"), run->config.pdk);
            if (!pdk)
                return;
            synthesizeRunCallback_(id, *pdk);
        });
        QObject::connect(showGraphButton_, &QPushButton::clicked, this, [this] {
            const auto id = actionRunId();
            if (id != 0)
                showRunGraph(id);
        });
        QObject::connect(exportVerilogButton_, &QPushButton::clicked, this, [this] {
            const auto id = actionRunId();
            if (id != 0 && exportVerilogCallback_)
                exportVerilogCallback_(id);
        });
        QObject::connect(removeRunButton_, &QPushButton::clicked, this, [this] {
            const auto id = actionRunId();
            if (id != 0 && removeRunCallback_)
                removeRunCallback_(id);
        });
    } else {
        QObject::connect(runTree_, &QTreeWidget::currentItemChanged, this,
                         [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                             if (!item)
                                 return;
                             const auto value = item->data(0, Qt::UserRole);
                             const auto stageValue = item->data(0, Qt::UserRole + 1);
                             if (!value.isValid())
                                 return;
                             controller_->setActiveRunId(static_cast<ice::ObjectId>(value.toULongLong()));
                             if (stageValue.isValid() && stageActivatedCallback_)
                                 stageActivatedCallback_(static_cast<ice::ImplementationStage>(stageValue.toInt()));
                         });
        if (netlistList_) {
            QObject::connect(netlistList_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
                if (!item)
                    return;
                const auto id = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
                if (id != 0)
                    controller_->setActiveRunId(id);
            });
            QObject::connect(netlistList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
                if (!item)
                    return;
                const auto id = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
                if (id != 0)
                    controller_->setActiveRunId(id);
            });
        }
        QObject::connect(artifactsList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item)
                return;
            const auto path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                openArtifact(path);
        });
        if (displayTree_) {
            QObject::connect(displayTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
                if (!previewCanvas_ || !item)
                    return;
                const auto group = item->data(0, Qt::UserRole).toString();
                const auto key = item->data(0, Qt::UserRole + 1).toString();
                const auto visible = item->checkState(0) == Qt::Checked;
                if (group == QStringLiteral("layer"))
                    previewCanvas_->setLayerVisible(key, visible);
                else if (group == QStringLiteral("instance"))
                    previewCanvas_->setInstanceCategoryVisible(key, visible);
                else if (group == QStringLiteral("net"))
                    previewCanvas_->setNetTypeVisible(key, visible);
                else if (group == QStringLiteral("option") && key == QStringLiteral("special"))
                    previewCanvas_->setShowSpecialNets(visible);
                else if (group == QStringLiteral("option") && key == QStringLiteral("ports"))
                    previewCanvas_->setShowPorts(visible);
                else if (group == QStringLiteral("option") && key == QStringLiteral("guides"))
                    previewCanvas_->setShowGuides(visible);
                else if (group == QStringLiteral("option") && key == QStringLiteral("rows"))
                    previewCanvas_->setShowRows(visible);
                else if (group == QStringLiteral("option") && key == QStringLiteral("blockages"))
                    previewCanvas_->setShowBlockages(visible);
            });
        }
        if (drcTable_) {
            QObject::connect(drcTable_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
                if (previewCanvas_ && row >= 0)
                    previewCanvas_->selectMarker(row);
            });
        }
        if (runStageButton_) {
            QObject::connect(runStageButton_, &QPushButton::clicked, this, [this] {
                if (!controller_->activeRun() || controller_->busy())
                    return;
                controller_->run(stage_, stage_);
            });
        }
        if (runFromHereButton_) {
            QObject::connect(runFromHereButton_, &QPushButton::clicked, this, [this] {
                if (!controller_->activeRun() || controller_->busy())
                    return;
                controller_->run(stage_, ice::ImplementationStage::final);
            });
        }
        if (runAllButton_) {
            QObject::connect(runAllButton_, &QPushButton::clicked, this, [this] {
                if (!controller_->activeRun() || controller_->busy())
                    return;
                controller_->run(ice::ImplementationStage::floorplan, ice::ImplementationStage::final);
            });
        }
        if (runCombo_) {
            QObject::connect(runCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
                if (!runCombo_)
                    return;
                const auto id = static_cast<ice::ObjectId>(runCombo_->currentData().toULongLong());
                if (id != 0)
                    controller_->setActiveRunId(id);
            });
        }
        if (cancelStageButton_) {
            QObject::connect(cancelStageButton_, &QPushButton::clicked, this, [this] { controller_->cancel(); });
        }
        if (applyStageButton_) {
            QObject::connect(applyStageButton_, &QPushButton::clicked, this, [this] { applyStageForm(); });
        }
    }
}

void ImplementationPage::refreshRunTree() {
    refreshRunCombo();
    QSignalBlocker blocker(runTree_);
    runTree_->clear();
    const auto *project = controller_->project();
    if (!project)
        return;
    const auto isPhysicalStage = [](ice::ImplementationStage stage) {
        return stage == ice::ImplementationStage::floorplan || stage == ice::ImplementationStage::placement ||
               stage == ice::ImplementationStage::cts || stage == ice::ImplementationStage::routing ||
               stage == ice::ImplementationStage::final;
    };
    QTreeWidgetItem *activeItem = nullptr;
    for (const auto &[id, run] : project->implementationRuns) {
        auto *item = new QTreeWidgetItem(runTree_);
        item->setText(0, QString::fromStdString(run.name));
        item->setData(0, Qt::UserRole, static_cast<qulonglong>(id));
        item->setText(1, runStatusText(run.status));
        item->setForeground(1, QBrush(QColor(runStatusColor(run.status))));
        item->setToolTip(0, QStringLiteral("%1 · %2 / %3\n%4")
                                .arg(QString::fromStdString(run.config.top), QString::fromStdString(run.config.pdk),
                                     QString::fromStdString(run.config.stack), QString::fromStdString(run.directory)));
        for (const auto stage : ice::implementationStages()) {
            if (physicalStageMode() && !isPhysicalStage(stage))
                continue;
            const auto state = run.stages.find(stage);
            if (state == run.stages.end())
                continue;
            auto *stageItem = new QTreeWidgetItem(item);
            stageItem->setText(0, stageTitle(stage));
            stageItem->setData(0, Qt::UserRole, static_cast<qulonglong>(id));
            stageItem->setData(0, Qt::UserRole + 1, static_cast<int>(stage));
            stageItem->setText(1, statusText(state->second.status));
            stageItem->setForeground(1, QBrush(QColor(statusColor(state->second.status))));
            if (!state->second.error.empty())
                stageItem->setToolTip(0, QString::fromStdString(state->second.error));
        }
        item->setExpanded(id == controller_->activeRunId());
        if (id == controller_->activeRunId())
            activeItem = item;
    }
    if (activeItem)
        runTree_->setCurrentItem(activeItem);
}

void ImplementationPage::refreshRunCombo() {
    if (!runCombo_)
        return;
    QSignalBlocker blocker(runCombo_);
    runCombo_->clear();
    const auto *project = controller_->project();
    if (!project) {
        runCombo_->addItem(QStringLiteral("No project"));
        runCombo_->setEnabled(false);
        return;
    }
    int activeIndex = -1;
    for (const auto &[id, run] : project->implementationRuns) {
        runCombo_->addItem(QString::fromStdString(run.name) + QStringLiteral("  ·  ") + runStatusText(run.status),
                           static_cast<qulonglong>(id));
        if (id == controller_->activeRunId())
            activeIndex = runCombo_->count() - 1;
    }
    if (runCombo_->count() == 0) {
        runCombo_->addItem(QStringLiteral("No implementation runs"));
        runCombo_->setEnabled(false);
        return;
    }
    runCombo_->setEnabled(true);
    runCombo_->setCurrentIndex(activeIndex >= 0 ? activeIndex : 0);
}

void ImplementationPage::refreshNetlistExplorer() {
    if (!netlistList_)
        return;
    QSignalBlocker blocker(netlistList_);
    netlistList_->clear();
    const auto *project = controller_->project();
    if (!project)
        return;
    const auto root = std::filesystem::path(controller_->projectRoot().toStdString());
    struct Entry {
        std::string display;
        std::string path;
        ice::ObjectId run = 0;
    };
    std::vector<Entry> entries;
    std::set<std::string> seen;
    const auto addNetlist = [&](const std::filesystem::path &path, ice::ObjectId runId) {
        std::error_code status;
        if (path.empty() || !std::filesystem::is_regular_file(path, status) || status)
            return;
        const auto normalized = path.lexically_normal().generic_string();
        if (!seen.insert(normalized).second)
            return;
        const auto relative = path.lexically_relative(root);
        const auto display =
            !relative.empty() && !relative.generic_string().starts_with("..") ? relative.generic_string() : normalized;
        entries.push_back({display, normalized, runId});
    };
    for (const auto &[id, run] : project->implementationRuns) {
        if (!run.directory.empty())
            addNetlist(root / run.directory / ice::implementationMappedNetlistRelativePath(), id);
        // Exported copies are recorded on the run as well.
        for (const auto &artifact : run.artifacts) {
            if (!artifact.ends_with(".v") || artifact.ends_with("/synth/1_synth.v") ||
                artifact.find("/final/") != std::string::npos)
                continue;
            std::filesystem::path path(artifact);
            if (path.is_relative())
                path = root / path;
            addNetlist(path, id);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right) { return left.display < right.display; });
    for (const auto &entry : entries) {
        auto *item = new QListWidgetItem(QString::fromStdString(entry.display));
        item->setData(Qt::UserRole, static_cast<qulonglong>(entry.run));
        item->setToolTip(QString::fromStdString(entry.path));
        netlistList_->addItem(item);
        if (entry.run == controller_->activeRunId())
            netlistList_->setCurrentItem(item);
    }
    if (entries.empty()) {
        auto *item = new QListWidgetItem(QStringLiteral("No technology-mapped netlists yet."));
        item->setFlags(Qt::NoItemFlags);
        item->setToolTip(QStringLiteral("Run Synthesize, then Export .v in the Synthesis tab."));
        netlistList_->addItem(item);
    }
}

void ImplementationPage::refreshStageForm() {
    if (!physicalStageMode())
        return;
    const auto *run = controller_->activeRun();
    const auto *project = controller_->project();
    if (!run) {
        if (applyStageButton_)
            applyStageButton_->setEnabled(false);
        return;
    }
    const auto joinList = [](const std::vector<std::string> &values) {
        QStringList text;
        for (const auto &value : values)
            text.push_back(QString::fromStdString(value));
        return text.join(QStringLiteral(", "));
    };
    if (floorplanMode()) {
        const auto toMicrons = [&](ice::Coordinate value) -> double {
            if (!project || project->units.nanometers_per_unit <= 0 || project->units.denominator <= 0)
                return static_cast<double>(value) / 1000.0;
            const auto nanometers = static_cast<double>(value) *
                                    static_cast<double>(project->units.nanometers_per_unit) /
                                    static_cast<double>(project->units.denominator);
            return nanometers / 1000.0;
        };
        utilizationSpin_->setValue(run->config.utilization);
        aspectRatioSpin_->setValue(run->config.aspectRatio);
        dieAreaCheck_->setChecked(run->config.dieArea.has_value());
        if (run->config.dieArea) {
            dieWidthSpin_->setValue(toMicrons(run->config.dieArea->width));
            dieHeightSpin_->setValue(toMicrons(run->config.dieArea->height));
            dieXSpin_->setValue(toMicrons(run->config.dieArea->x));
            dieYSpin_->setValue(toMicrons(run->config.dieArea->y));
        }
        coreAreaCheck_->setChecked(run->config.coreArea.has_value());
        if (run->config.coreArea) {
            coreWidthSpin_->setValue(toMicrons(run->config.coreArea->width));
            coreHeightSpin_->setValue(toMicrons(run->config.coreArea->height));
            coreXSpin_->setValue(toMicrons(run->config.coreArea->x));
            coreYSpin_->setValue(toMicrons(run->config.coreArea->y));
        }
        siteEdit_->setText(QString::fromStdString(run->config.site));
        ioHorizontalEdit_->setText(joinList(run->config.ioHorizontalLayers));
        ioVerticalEdit_->setText(joinList(run->config.ioVerticalLayers));
    } else if (stage_ == ice::ImplementationStage::placement) {
        placementDensitySpin_->setValue(run->config.placementDensity);
        timingDrivenCheck_->setChecked(run->config.timingDriven);
        routabilityDrivenCheck_->setChecked(run->config.routabilityDriven);
    } else if (stage_ == ice::ImplementationStage::cts) {
        ctsEnabledCheck_->setChecked(run->config.ctsEnabled);
        ctsRootBufferEdit_->setText(QString::fromStdString(run->config.ctsRootBuffer));
        ctsBuffersEdit_->setText(joinList(run->config.ctsBuffers));
    } else if (stage_ == ice::ImplementationStage::routing) {
        routingMinLayerEdit_->setText(QString::fromStdString(run->config.routingMinLayer));
        routingMaxLayerEdit_->setText(QString::fromStdString(run->config.routingMaxLayer));
        antennaRepairCheck_->setChecked(run->config.antennaRepair);
    }
    threadsSpin_->setValue(run->config.threads);
    timeoutSpin_->setValue(run->config.timeoutSeconds);
}

void ImplementationPage::updateStageActions() {
    if (!physicalStageMode())
        return;
    const auto *run = controller_->activeRun();
    const auto busy = controller_->busy();
    const auto status = [&](ice::ImplementationStage stage) {
        if (!run)
            return ice::ImplementationStageStatus::pending;
        const auto found = run->stages.find(stage);
        return found == run->stages.end() ? ice::ImplementationStageStatus::pending : found->second.status;
    };
    bool ready = false;
    QString hint = QStringLiteral("Select an implementation run in the Runs panel.");
    if (run) {
        if (stage_ == ice::ImplementationStage::floorplan) {
            bool mapped = false;
            if (!run->directory.empty() && !controller_->projectRoot().isEmpty()) {
                std::error_code fileStatus;
                mapped = std::filesystem::is_regular_file(
                             std::filesystem::path(controller_->projectRoot().toStdString()) / run->directory /
                                 ice::implementationMappedNetlistRelativePath(),
                             fileStatus) &&
                         !fileStatus;
            }
            const bool synthesized =
                status(ice::ImplementationStage::synthesis) == ice::ImplementationStageStatus::succeeded;
            ready = mapped && synthesized;
            if (!synthesized)
                hint = QStringLiteral("Run synthesis first.");
            else if (!mapped)
                hint = QStringLiteral("Export a technology-mapped netlist from the Synthesis tab first.");
            else
                hint = QStringLiteral("Run the floorplan stage for the active run.");
        } else if (stage_ == ice::ImplementationStage::placement) {
            ready = status(ice::ImplementationStage::floorplan) == ice::ImplementationStageStatus::succeeded;
            hint = ready ? QStringLiteral("Run the placement stage for the active run.")
                         : QStringLiteral("Run floorplan first.");
        } else if (stage_ == ice::ImplementationStage::cts) {
            if (!run->config.ctsEnabled) {
                hint = QStringLiteral("CTS is disabled in the stage settings.");
            } else {
                ready = status(ice::ImplementationStage::placement) == ice::ImplementationStageStatus::succeeded;
                hint = ready ? QStringLiteral("Run the CTS stage for the active run.")
                             : QStringLiteral("Run placement first.");
            }
        } else if (stage_ == ice::ImplementationStage::routing) {
            const auto prerequisite =
                run->config.ctsEnabled ? ice::ImplementationStage::cts : ice::ImplementationStage::placement;
            ready = status(prerequisite) == ice::ImplementationStageStatus::succeeded;
            hint = ready ? QStringLiteral("Run the routing stage for the active run.")
                         : (run->config.ctsEnabled ? QStringLiteral("Run CTS first.")
                                                   : QStringLiteral("Run placement first."));
        } else if (stage_ == ice::ImplementationStage::final) {
            ready = status(ice::ImplementationStage::routing) == ice::ImplementationStageStatus::succeeded;
            hint = ready ? QStringLiteral("Run the final extraction and sign-off stage for the active run.")
                         : QStringLiteral("Run routing first.");
        }
    }
    stageReady_ = run && ready;
    stageHint_ = hint;
    const bool flowReady =
        run && (status(ice::ImplementationStage::floorplan) == ice::ImplementationStageStatus::succeeded ||
                (stage_ == ice::ImplementationStage::floorplan && ready));
    if (runStageButton_) {
        runStageButton_->setEnabled(run && !busy && ready);
        runStageButton_->setToolTip(hint);
    }
    if (runFromHereButton_) {
        runFromHereButton_->setEnabled(run && !busy && ready);
        runFromHereButton_->setToolTip(QStringLiteral("Run %1 through final.").arg(stageTitle(stage_)));
    }
    if (runAllButton_) {
        runAllButton_->setEnabled(!busy && flowReady);
        runAllButton_->setToolTip(QStringLiteral("Run floorplan through final."));
    }
    if (cancelStageButton_)
        cancelStageButton_->setEnabled(busy);
    if (applyStageButton_)
        applyStageButton_->setEnabled(run && !busy);
    if (stageHintLabel_) {
        if (busy) {
            stageHintLabel_->setText(QStringLiteral("Running %1…").arg(stageTitle(controller_->runningStage())));
            stageHintLabel_->setStyleSheet(QStringLiteral("color: #2563eb;"));
        } else if (!run) {
            stageHintLabel_->setText(hint);
            stageHintLabel_->setStyleSheet({});
        } else {
            const auto state = run->stages.find(stage_);
            const auto statusValue =
                state == run->stages.end() ? ice::ImplementationStageStatus::pending : state->second.status;
            if (statusValue == ice::ImplementationStageStatus::failed) {
                const auto message = state != run->stages.end() && !state->second.error.empty()
                                         ? QStringLiteral("%1 failed: %2")
                                               .arg(stageTitle(stage_), QString::fromStdString(state->second.error))
                                         : QStringLiteral("%1 failed. Check the Activity log.").arg(stageTitle(stage_));
                stageHintLabel_->setText(message);
                stageHintLabel_->setStyleSheet(QStringLiteral("color: #b91c1c;"));
            } else if (statusValue == ice::ImplementationStageStatus::succeeded) {
                const auto summary = stageSummaryText(run, stage_);
                stageHintLabel_->setText(summary.isEmpty() ? QStringLiteral("%1 succeeded.").arg(stageTitle(stage_))
                                                           : summary);
                stageHintLabel_->setStyleSheet(QStringLiteral("color: #15803d;"));
            } else {
                stageHintLabel_->setText(hint);
                stageHintLabel_->setStyleSheet(ready ? QStringLiteral("color: #15803d;")
                                                     : QStringLiteral("color: #a16207;"));
            }
        }
    }
}

void ImplementationPage::applyStageForm() {
    if (!physicalStageMode() || !applyConfigCallback_ || !controller_ || controller_->busy())
        return;
    const auto *run = controller_->activeRun();
    const auto *project = controller_->project();
    if (!run || !project)
        return;
    auto config = run->config;
    const auto splitList = [](const QString &text) {
        std::vector<std::string> values;
        for (const auto &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const auto trimmed = part.trimmed();
            if (!trimmed.isEmpty())
                values.push_back(trimmed.toStdString());
        }
        return values;
    };
    if (floorplanMode()) {
        config.utilization = utilizationSpin_->value();
        config.aspectRatio = aspectRatioSpin_->value();
        if (dieAreaCheck_->isChecked()) {
            const auto toUnits = [&](double microns) -> ice::Coordinate {
                const auto nanometers = microns * 1000.0;
                const auto &units = project->units;
                const auto scale = units.nanometers_per_unit > 0
                                       ? static_cast<double>(units.denominator) / units.nanometers_per_unit
                                       : 1.0;
                return static_cast<ice::Coordinate>(std::llround(nanometers * scale));
            };
            ice::Rect die;
            die.x = toUnits(dieXSpin_->value());
            die.y = toUnits(dieYSpin_->value());
            die.width = toUnits(dieWidthSpin_->value());
            die.height = toUnits(dieHeightSpin_->value());
            config.dieArea = die;
        } else {
            config.dieArea.reset();
        }
        if (coreAreaCheck_->isChecked()) {
            const auto toUnits = [&](double microns) -> ice::Coordinate {
                const auto nanometers = microns * 1000.0;
                const auto &units = project->units;
                const auto scale = units.nanometers_per_unit > 0
                                       ? static_cast<double>(units.denominator) / units.nanometers_per_unit
                                       : 1.0;
                return static_cast<ice::Coordinate>(std::llround(nanometers * scale));
            };
            ice::Rect core;
            core.x = toUnits(coreXSpin_->value());
            core.y = toUnits(coreYSpin_->value());
            core.width = toUnits(coreWidthSpin_->value());
            core.height = toUnits(coreHeightSpin_->value());
            config.coreArea = core;
        } else {
            config.coreArea.reset();
        }
        config.site = siteEdit_->text().trimmed().toStdString();
        config.ioHorizontalLayers = splitList(ioHorizontalEdit_->text());
        config.ioVerticalLayers = splitList(ioVerticalEdit_->text());
    } else if (stage_ == ice::ImplementationStage::placement) {
        config.placementDensity = placementDensitySpin_->value();
        config.timingDriven = timingDrivenCheck_->isChecked();
        config.routabilityDriven = routabilityDrivenCheck_->isChecked();
    } else if (stage_ == ice::ImplementationStage::cts) {
        config.ctsEnabled = ctsEnabledCheck_->isChecked();
        config.ctsRootBuffer = ctsRootBufferEdit_->text().trimmed().toStdString();
        config.ctsBuffers = splitList(ctsBuffersEdit_->text());
    } else if (stage_ == ice::ImplementationStage::routing) {
        config.routingMinLayer = routingMinLayerEdit_->text().trimmed().toStdString();
        config.routingMaxLayer = routingMaxLayerEdit_->text().trimmed().toStdString();
        config.antennaRepair = antennaRepairCheck_->isChecked();
    }
    config.threads = threadsSpin_->value();
    config.timeoutSeconds = timeoutSpin_->value();
    applyConfigCallback_(run->id, config);
}

void ImplementationPage::refreshSynthesisExplorer() {
    if (!hdlFileList_)
        return;
    QSignalBlocker blocker(hdlFileList_);
    hdlFileList_->clear();
    const auto *project = controller_->project();
    if (!project)
        return;
    struct Entry {
        ice::ObjectId id = 0;
        std::string name;
    };
    std::vector<Entry> entries;
    for (const auto &[id, cell] : project->schematicCells) {
        if (cell.systemVerilogSchematic)
            entries.push_back({id, cell.name});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right) { return left.name < right.name; });
    if (activeHdlCellId_ == 0 || !project->schematicCells.contains(activeHdlCellId_) ||
        !project->schematicCells.at(activeHdlCellId_).systemVerilogSchematic)
        activeHdlCellId_ = entries.empty() ? 0 : entries.front().id;
    int activeRow = -1;
    for (const auto &entry : entries) {
        auto *item = new QListWidgetItem(QString::fromStdString(entry.name) + QStringLiteral(".sv"));
        item->setData(Qt::UserRole, static_cast<qulonglong>(entry.id));
        item->setToolTip(QStringLiteral("%1.sv · SystemVerilog").arg(QString::fromStdString(entry.name)));
        hdlFileList_->addItem(item);
        if (entry.id == activeHdlCellId_)
            activeRow = hdlFileList_->count() - 1;
    }
    if (activeRow >= 0)
        hdlFileList_->setCurrentRow(activeRow);
    applyHdlFilter(hdlSearch_ ? hdlSearch_->text() : QString());
}

void ImplementationPage::openArtifact(const QString &artifactPath) {
    if ((!synthesisMode() && !physicalStageMode()) || !middleTabs_)
        return;
    std::filesystem::path path(artifactPath.toStdString());
    if (path.is_relative())
        path = std::filesystem::path(controller_->projectRoot().toStdString()) / path;
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status) || status) {
        QMessageBox::information(
            this, QStringLiteral("Open Artifact"),
            QStringLiteral("The file does not exist:\n%1").arg(QString::fromStdString(path.string())));
        return;
    }
    const auto size = std::filesystem::file_size(path, status);
    const auto extension = QString::fromStdString(path.extension().string()).toLower();
    if (!status && (size > 16ULL * 1024ULL * 1024ULL || extension == QStringLiteral(".odb"))) {
        QMessageBox::information(this, QStringLiteral("Open Artifact"),
                                 QStringLiteral("%1 is too large for the built-in viewer and will open externally.")
                                     .arg(QString::fromStdString(path.filename().string())));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path.string())));
        return;
    }
    const auto key = path.lexically_normal().generic_string();
    if (const auto existing = artifactTabs_.find(key); existing != artifactTabs_.end()) {
        if (path.extension() == ".svg") {
            if (auto *canvas = dynamic_cast<SvgPreviewCanvas *>(existing->second))
                canvas->setSvg(path, QStringLiteral("The graph could not be rendered."));
        } else if (auto *editor = dynamic_cast<TextDocumentEditor *>(existing->second)) {
            std::ifstream input(path);
            if (input.is_open()) {
                std::ostringstream contents;
                contents << input.rdbuf();
                editor->setText(QString::fromStdString(contents.str()));
            }
        }
        middleTabs_->setCurrentWidget(existing->second);
        return;
    }
    if (middlePlaceholder_) {
        const auto index = middleTabs_->indexOf(middlePlaceholder_);
        if (index >= 0)
            middleTabs_->removeTab(index);
        delete middlePlaceholder_;
        middlePlaceholder_ = nullptr;
    }
    QWidget *page = nullptr;
    if (path.extension() == ".svg") {
        auto *canvas = new SvgPreviewCanvas(middleTabs_);
        canvas->setSvg(path, QStringLiteral("The graph could not be rendered."));
        page = canvas;
    } else {
        auto *editor = new TextDocumentEditor(middleTabs_);
        std::ifstream input(path);
        if (!input.is_open()) {
            delete editor;
            ensureMiddlePlaceholder();
            QMessageBox::information(this, QStringLiteral("Open Artifact"),
                                     QStringLiteral("Unable to read:\n%1").arg(QString::fromStdString(path.string())));
            return;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        editor->setText(QString::fromStdString(contents.str()));
        if (extension == QStringLiteral(".v") || extension == QStringLiteral(".sv") ||
            extension == QStringLiteral(".vh"))
            editor->setLanguage(TextDocumentLanguage::SystemVerilog);
        else if (extension == QStringLiteral(".sp") || extension == QStringLiteral(".cir") ||
                 extension == QStringLiteral(".spice"))
            editor->setLanguage(TextDocumentLanguage::Spice);
        editor->setReadOnly(true);
        page = editor;
    }
    const auto index = middleTabs_->addTab(page, QString::fromStdString(path.filename().string()));
    middleTabs_->tabBar()->setTabData(index, 0);
    middleTabs_->setTabToolTip(index, QString::fromStdString(path.string()));
    middleTabs_->tabBar()->setVisible(true);
    artifactTabs_[key] = page;
    middleTabs_->setCurrentWidget(page);
}

void ImplementationPage::closeArtifactTab(QWidget *widget) {
    if (!middleTabs_ || !widget)
        return;
    for (auto iterator = artifactTabs_.begin(); iterator != artifactTabs_.end(); ++iterator) {
        if (iterator->second != widget)
            continue;
        const auto index = middleTabs_->indexOf(widget);
        if (index >= 0)
            middleTabs_->removeTab(index);
        delete widget;
        artifactTabs_.erase(iterator);
        ensureMiddlePlaceholder();
        return;
    }
}

void ImplementationPage::ensureMiddlePlaceholder() {
    if (!synthesisMode() || !middleTabs_ || middleTabs_->count() > 0)
        return;
    auto *page = new QWidget(middleTabs_);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *label = new QLabel(QStringLiteral("Double-click an artifact to open its graph or netlist."), page);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    layout->addStretch(1);
    layout->addWidget(label);
    layout->addStretch(1);
    middlePlaceholder_ = page;
    const auto index = middleTabs_->addTab(page, QStringLiteral("Start"));
    middleTabs_->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
    middleTabs_->tabBar()->setVisible(false);
}

QString ImplementationPage::runStageLabel(const ice::ImplementationRun &run) const {
    const auto status = [&](ice::ImplementationStage stage) {
        const auto found = run.stages.find(stage);
        return found == run.stages.end() ? ice::ImplementationStageStatus::pending : found->second.status;
    };
    if (run.status == ice::ImplementationStatus::failed)
        return QStringLiteral("failed");
    if (status(ice::ImplementationStage::synthesis) == ice::ImplementationStageStatus::running)
        return QStringLiteral("synthesizing");
    if (status(ice::ImplementationStage::synthesis) == ice::ImplementationStageStatus::succeeded)
        return QStringLiteral("synthesized");
    if (status(ice::ImplementationStage::elaboration) == ice::ImplementationStageStatus::running)
        return QStringLiteral("elaborating");
    if (status(ice::ImplementationStage::elaboration) == ice::ImplementationStageStatus::succeeded)
        return QStringLiteral("elaborated");
    if (run.status == ice::ImplementationStatus::cancelled)
        return QStringLiteral("cancelled");
    return QStringLiteral("created");
}

std::optional<std::string> ImplementationPage::promptPdk(const QString &title, const std::string &current) {
    const auto pdks = ice::discoverPdks();
    if (pdks.empty()) {
        QMessageBox::warning(this, title, QStringLiteral("No PDK packages are installed."));
        return std::nullopt;
    }
    QStringList labels;
    std::vector<std::string> ids;
    int currentIndex = 0;
    for (const auto &pdk : pdks) {
        QString label = QString::fromStdString(pdk.id);
        if (!pdk.packageName.empty())
            label += QStringLiteral(" (%1)").arg(QString::fromStdString(pdk.packageName));
        if (!pdk.implementationReady) {
            QString reason = QStringLiteral("no implementation data");
            for (const auto &missing : pdk.implementationMissing) {
                if (missing.find("OPENROAD_FLOW_SCRIPTS") != std::string::npos) {
                    reason = QStringLiteral("needs OpenROAD-flow-scripts");
                    break;
                }
            }
            if (reason == QStringLiteral("no implementation data") && !pdk.implementationMissing.empty())
                reason = QString::fromStdString(pdk.implementationMissing.front());
            label += QStringLiteral(" — ") + reason;
        }
        labels.push_back(label);
        ids.push_back(pdk.id);
        if (pdk.id == current)
            currentIndex = static_cast<int>(ids.size()) - 1;
    }
    bool accepted = false;
    const auto selected =
        QInputDialog::getItem(this, title, QStringLiteral("PDK:"), labels, currentIndex, false, &accepted);
    if (!accepted)
        return std::nullopt;
    const auto index = labels.indexOf(selected);
    if (index < 0)
        return std::nullopt;
    return ids.at(static_cast<std::size_t>(index));
}

void ImplementationPage::refreshRunsTable() {
    if (!runsTable_)
        return;
    QSignalBlocker blocker(runsTable_);
    const auto previous = selectedRunId();
    runsTable_->setRowCount(0);
    const auto *project = controller_->project();
    if (!project) {
        updateRunActions();
        return;
    }
    std::vector<std::pair<std::string, ice::ObjectId>> runs;
    for (const auto &[id, run] : project->implementationRuns)
        runs.emplace_back(run.name, id);
    std::sort(runs.begin(), runs.end(), [](const auto &left, const auto &right) { return left.first < right.first; });
    const auto active = controller_->activeRunId();
    const auto selected = active != 0 && project->implementationRuns.contains(active) ? active : previous;
    int selectedRow = -1;
    for (const auto &[name, id] : runs) {
        const auto row = runsTable_->rowCount();
        runsTable_->insertRow(row);
        auto *runItem = new QTableWidgetItem(QString::fromStdString(name));
        runItem->setData(Qt::UserRole, static_cast<qulonglong>(id));
        runsTable_->setItem(row, 0, runItem);
        const auto &run = project->implementationRuns.at(id);
        runsTable_->setItem(row, 1, new QTableWidgetItem(runStageLabel(run)));
        if (id == selected)
            selectedRow = row;
    }
    if (selectedRow >= 0)
        runsTable_->setCurrentCell(selectedRow, 0);
    applyRunFilter(runSearch_ ? runSearch_->text() : QString());
    updateRunActions();
}

void ImplementationPage::applyRunFilter(const QString &text) {
    if (!runsTable_)
        return;
    const auto needle = text.trimmed();
    for (int row = 0; row < runsTable_->rowCount(); ++row) {
        const auto *runItem = runsTable_->item(row, 0);
        const auto *stageItem = runsTable_->item(row, 1);
        const bool match = needle.isEmpty() || (runItem && runItem->text().contains(needle, Qt::CaseInsensitive)) ||
                           (stageItem && stageItem->text().contains(needle, Qt::CaseInsensitive));
        runsTable_->setRowHidden(row, !match);
    }
    if (const auto *current = runsTable_->currentItem(); current && runsTable_->isRowHidden(current->row())) {
        runsTable_->clearSelection();
        runsTable_->setCurrentCell(-1, -1);
    }
    updateRunActions();
}

ice::ObjectId ImplementationPage::selectedRunId() const {
    if (!runsTable_)
        return 0;
    const auto row = runsTable_->currentRow();
    if (row < 0 || runsTable_->isRowHidden(row))
        return 0;
    const auto *item = runsTable_->item(row, 0);
    return item ? static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong()) : 0;
}

ice::ObjectId ImplementationPage::actionRunId() const {
    const auto selected = selectedRunId();
    return selected != 0 ? selected : controller_->activeRunId();
}

void ImplementationPage::updateRunActions() {
    if (!synthesizeButton_ || !showGraphButton_ || !exportVerilogButton_)
        return;
    const auto id = actionRunId();
    const auto *project = controller_->project();
    const auto *run =
        project && id != 0 && project->implementationRuns.contains(id) ? &project->implementationRuns.at(id) : nullptr;
    if (!run) {
        synthesizeButton_->setEnabled(false);
        showGraphButton_->setEnabled(false);
        exportVerilogButton_->setEnabled(false);
        if (removeRunButton_)
            removeRunButton_->setEnabled(false);
        return;
    }
    const auto stageStatus = [&](ice::ImplementationStage stage) {
        const auto found = run->stages.find(stage);
        return found == run->stages.end() ? ice::ImplementationStageStatus::pending : found->second.status;
    };
    const bool busy = controller_->busy();
    const bool elaborated =
        stageStatus(ice::ImplementationStage::elaboration) == ice::ImplementationStageStatus::succeeded;
    const bool synthesized =
        stageStatus(ice::ImplementationStage::synthesis) == ice::ImplementationStageStatus::succeeded;
    synthesizeButton_->setEnabled(elaborated && !busy);
    showGraphButton_->setEnabled((elaborated || synthesized) && !busy);
    exportVerilogButton_->setEnabled(synthesized && !busy);
    if (removeRunButton_)
        removeRunButton_->setEnabled(!busy && run->status != ice::ImplementationStatus::running);
}

void ImplementationPage::showRunGraph(ice::ObjectId runId) {
    const auto *project = controller_->project();
    if (!project || !synthesisMode() || !middleTabs_)
        return;
    const auto run = project->implementationRuns.find(runId);
    if (run == project->implementationRuns.end())
        return;
    const auto root = controller_->projectRoot();
    const auto elaborationPath = runGraph(run->second, root, "elaboration", true);
    const auto synthesisPath = runGraph(run->second, root, "synthesis", true);
    if (!elaborationPath && !synthesisPath) {
        const auto runDirectory = std::filesystem::path(root.toStdString()) / run->second.directory;
        QMessageBox::information(
            this, QStringLiteral("Run Graph"),
            QStringLiteral("No elaboration or synthesis graph is available for this run yet.\n\nRun directory:\n%1")
                .arg(QString::fromStdString(runDirectory.string())));
        return;
    }
    if (elaborationPath)
        openArtifact(QString::fromStdString(elaborationPath->string()));
    if (synthesisPath)
        openArtifact(QString::fromStdString(synthesisPath->string()));
}

void ImplementationPage::refreshExplorer() {
    if (!hdlFileList_)
        return;
    QSignalBlocker blocker(hdlFileList_);
    QSignalBlocker tabBlocker(middleTabs_);
    hdlFileList_->clear();
    const auto *project = controller_->project();
    const auto projectKey =
        project ? controller_->projectPath() + QLatin1Char(':') + QString::fromStdString(project->identity) : QString();
    if (projectKey != hdlProjectKey_) {
        hdlProjectKey_ = projectKey;
        hdlTabsInitialized_ = false;
        hdlEditCellId_ = 0;
        if (hdlEditTimer_)
            hdlEditTimer_->stop();
        for (auto &[id, editor] : hdlEditors_) {
            for (int index = 0; index < middleTabs_->count(); ++index) {
                if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == id) {
                    middleTabs_->removeTab(index);
                    break;
                }
            }
            delete editor;
        }
        hdlEditors_.clear();
        hdlEditor_ = nullptr;
        activeHdlCellId_ = 0;
    }
    if (!project)
        return;

    struct Entry {
        ice::ObjectId id = 0;
        std::string name;
    };
    std::vector<Entry> entries;
    for (const auto &[id, cell] : project->schematicCells) {
        if (cell.systemVerilogSchematic)
            entries.push_back({id, cell.name});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &left, const Entry &right) { return left.name < right.name; });

    for (auto it = hdlEditors_.begin(); it != hdlEditors_.end();) {
        const auto cell = project->schematicCells.find(it->first);
        if (cell == project->schematicCells.end() || !cell->second.systemVerilogSchematic) {
            const auto id = it->first;
            const auto *editor = it->second;
            ++it;
            for (int index = 0; index < middleTabs_->count(); ++index) {
                if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == id) {
                    middleTabs_->removeTab(index);
                    break;
                }
            }
            hdlEditors_.erase(id);
            delete editor;
            continue;
        }
        if ((hdlEditCellId_ != it->first || !hdlEditTimer_ || !hdlEditTimer_->isActive())) {
            const auto text = QString::fromStdString(cell->second.systemVerilogText);
            if (it->second->text() != text) {
                loadingHdlEditor_ = true;
                it->second->setText(text);
                loadingHdlEditor_ = false;
            }
        }
        for (int index = 0; index < middleTabs_->count(); ++index) {
            if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == it->first) {
                middleTabs_->setTabText(index, QString::fromStdString(cell->second.name) + QStringLiteral(".sv"));
                break;
            }
        }
        ++it;
    }

    if (activeHdlCellId_ == 0 || !project->schematicCells.contains(activeHdlCellId_) ||
        !project->schematicCells.at(activeHdlCellId_).systemVerilogSchematic) {
        activeHdlCellId_ = 0;
        if (!hdlEditors_.empty()) {
            const auto current = middleTabs_->currentIndex();
            if (current >= 0) {
                const auto currentId =
                    static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(current).toULongLong());
                if (hdlEditors_.contains(currentId))
                    activeHdlCellId_ = currentId;
            }
            if (activeHdlCellId_ == 0)
                activeHdlCellId_ = hdlEditors_.begin()->first;
        } else if (!entries.empty()) {
            activeHdlCellId_ = entries.front().id;
        }
    }
    hdlEditor_ = hdlEditorFor(activeHdlCellId_);
    for (int index = 0; index < middleTabs_->count(); ++index) {
        if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == activeHdlCellId_) {
            middleTabs_->setCurrentIndex(index);
            break;
        }
    }

    int activeRow = -1;
    for (const auto &entry : entries) {
        auto *item = new QListWidgetItem(QString::fromStdString(entry.name) + QStringLiteral(".sv"));
        item->setData(Qt::UserRole, static_cast<qulonglong>(entry.id));
        item->setToolTip(QStringLiteral("%1.sv · SystemVerilog").arg(QString::fromStdString(entry.name)));
        hdlFileList_->addItem(item);
        if (entry.id == activeHdlCellId_)
            activeRow = hdlFileList_->count() - 1;
    }
    if (activeRow >= 0)
        hdlFileList_->setCurrentRow(activeRow);
    else if (activeHdlCellId_ == 0)
        hdlFileList_->clearSelection();
    applyHdlFilter(hdlSearch_ ? hdlSearch_->text() : QString());

    if (!hdlTabsInitialized_ && !entries.empty()) {
        hdlTabsInitialized_ = true;
        if (activeHdlCellId_ != 0)
            openHdlFile(activeHdlCellId_);
    }
}

void ImplementationPage::applyHdlFilter(const QString &text) {
    if (!hdlFileList_)
        return;
    const auto needle = text.trimmed();
    for (int row = 0; row < hdlFileList_->count(); ++row) {
        auto *item = hdlFileList_->item(row);
        item->setHidden(!needle.isEmpty() && !item->text().contains(needle, Qt::CaseInsensitive));
    }
    auto *current = hdlFileList_->currentItem();
    if (current && current->isHidden()) {
        hdlFileList_->clearSelection();
        hdlFileList_->setCurrentRow(-1);
        current = nullptr;
    }
    if (deleteHdlButton_)
        deleteHdlButton_->setEnabled(current != nullptr && !current->isHidden());
}

void ImplementationPage::openHdlFile(ice::ObjectId id) {
    const auto *project = controller_->project();
    if (!project)
        return;
    const auto cell = project->schematicCells.find(id);
    if (cell == project->schematicCells.end() || !cell->second.systemVerilogSchematic)
        return;
    if (const auto existing = hdlEditors_.find(id); existing != hdlEditors_.end()) {
        for (int index = 0; index < middleTabs_->count(); ++index) {
            if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == id) {
                middleTabs_->setCurrentIndex(index);
                break;
            }
        }
        hdlEditor_ = existing->second;
        activeHdlCellId_ = id;
        return;
    }
    auto *editor = new TextDocumentEditor(middleTabs_);
    editor->setLanguage(TextDocumentLanguage::SystemVerilog);
    loadingHdlEditor_ = true;
    editor->setText(QString::fromStdString(cell->second.systemVerilogText));
    loadingHdlEditor_ = false;
    editor->setOnTextEdited([this, id](const QString &) {
        if (!loadingHdlEditor_ && hdlEditTimer_) {
            hdlEditCellId_ = id;
            hdlEditTimer_->start();
        }
    });
    const auto index = middleTabs_->addTab(editor, QString::fromStdString(cell->second.name) + QStringLiteral(".sv"));
    middleTabs_->tabBar()->setTabData(index, static_cast<qulonglong>(id));
    hdlEditors_[id] = editor;
    hdlEditor_ = editor;
    activeHdlCellId_ = id;
    middleTabs_->setCurrentIndex(index);
    QSignalBlocker blocker(hdlFileList_);
    for (int row = 0; row < hdlFileList_->count(); ++row) {
        if (static_cast<ice::ObjectId>(hdlFileList_->item(row)->data(Qt::UserRole).toULongLong()) == id) {
            hdlFileList_->setCurrentRow(row);
            break;
        }
    }
}

void ImplementationPage::closeHdlFile(ice::ObjectId id) {
    flushHdlEdit();
    QSignalBlocker tabBlocker(middleTabs_);
    const auto editor = hdlEditors_.find(id);
    if (editor == hdlEditors_.end())
        return;
    for (int index = 0; index < middleTabs_->count(); ++index) {
        if (static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(index).toULongLong()) == id) {
            middleTabs_->removeTab(index);
            break;
        }
    }
    delete editor->second;
    hdlEditors_.erase(editor);
    if (activeHdlCellId_ == id) {
        hdlEditor_ = nullptr;
        activeHdlCellId_ = 0;
        if (middleTabs_->count() > 0) {
            const auto current = middleTabs_->currentIndex();
            if (current >= 0) {
                activeHdlCellId_ = static_cast<ice::ObjectId>(middleTabs_->tabBar()->tabData(current).toULongLong());
                hdlEditor_ = hdlEditorFor(activeHdlCellId_);
            }
        }
    }
    QSignalBlocker blocker(hdlFileList_);
    if (activeHdlCellId_ == 0) {
        hdlFileList_->clearSelection();
    } else {
        for (int row = 0; row < hdlFileList_->count(); ++row) {
            if (static_cast<ice::ObjectId>(hdlFileList_->item(row)->data(Qt::UserRole).toULongLong()) ==
                activeHdlCellId_) {
                hdlFileList_->setCurrentRow(row);
                break;
            }
        }
    }
    refreshProperties();
}

void ImplementationPage::flushHdlEdit() {
    if (hdlEditTimer_ && hdlEditTimer_->isActive())
        hdlEditTimer_->stop();
    if (hdlEditCellId_ == 0)
        return;
    if (auto *editor = hdlEditorFor(hdlEditCellId_))
        controller_->editHdlText(hdlEditCellId_, editor->text().toStdString());
    hdlEditCellId_ = 0;
}

TextDocumentEditor *ImplementationPage::hdlEditorFor(ice::ObjectId id) const {
    const auto found = hdlEditors_.find(id);
    return found == hdlEditors_.end() ? nullptr : found->second;
}

void ImplementationPage::selectHdlCell(ice::ObjectId id) {
    flushHdlEdit();
    activeHdlCellId_ = id;
    openHdlFile(id);
    refreshProperties();
}

void ImplementationPage::refreshOutline() {
    if (!outlineTree_)
        return;
    QSignalBlocker blocker(outlineTree_);
    outlineTree_->clear();
    const auto addItem = [](QTreeWidgetItem *parent, const QString &text, int line, const QString &tooltip = {}) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, text);
        if (line > 0)
            item->setText(1, QString::number(line));
        item->setData(0, Qt::UserRole, line);
        if (!tooltip.isEmpty())
            item->setToolTip(0, tooltip);
        return item;
    };
    const auto addTopItem = [&](const QString &text, int line, const QString &tooltip = {}) {
        auto *item = new QTreeWidgetItem(outlineTree_);
        item->setText(0, text);
        if (line > 0)
            item->setText(1, QString::number(line));
        item->setData(0, Qt::UserRole, line);
        if (!tooltip.isEmpty())
            item->setToolTip(0, tooltip);
        return item;
    };
    const auto addGroup = [&](const QString &title) {
        auto *item = new QTreeWidgetItem(outlineTree_);
        item->setText(0, title);
        item->setFirstColumnSpanned(true);
        auto font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        return item;
    };

    const auto *project = controller_->project();
    if (!project || activeHdlCellId_ == 0) {
        addTopItem(QStringLiteral("No HDL file open"), 0);
        return;
    }
    const auto cell = project->schematicCells.find(activeHdlCellId_);
    if (cell == project->schematicCells.end() || !cell->second.systemVerilogSchematic) {
        addTopItem(QStringLiteral("No HDL file open"), 0);
        return;
    }
    QString source = QString::fromStdString(cell->second.systemVerilogText);
    if (const auto *editor = hdlEditorFor(activeHdlCellId_))
        source = editor->text();
    const auto lines = source.split(QLatin1Char('\n'));
    const auto parsed = ice::parseSystemVerilogModule(source.toStdString(), cell->second.name);

    QTreeWidgetItem *parsedModuleItem = nullptr;
    const QRegularExpression moduleExpression(QStringLiteral(R"(^\s*module\s+([A-Za-z_]\w*))"));
    for (int index = 0; index < lines.size(); ++index) {
        const auto match = moduleExpression.match(lines.at(index));
        if (!match.hasMatch())
            continue;
        auto *moduleItem = addTopItem(QStringLiteral("module %1").arg(match.captured(1)), index + 1);
        if (parsed.ok && match.captured(1).toStdString() == parsed.module.name)
            parsedModuleItem = moduleItem;
    }
    if (parsed.ok) {
        if (!parsedModuleItem)
            parsedModuleItem =
                addTopItem(QStringLiteral("module %1").arg(QString::fromStdString(parsed.module.name)), 0);
        for (const auto &port : parsed.module.ports) {
            QString direction = port.direction == ice::HdlPortDirection::input    ? QStringLiteral("input")
                                : port.direction == ice::HdlPortDirection::output ? QStringLiteral("output")
                                                                                  : QStringLiteral("inout");
            QString text = direction;
            if (port.real)
                text += QStringLiteral(" real");
            else if (port.width > 1)
                text += QStringLiteral(" [%1:0]").arg(port.width - 1);
            text += QStringLiteral(" %1").arg(QString::fromStdString(port.name));
            addItem(parsedModuleItem, text, 0);
        }
    }

    const QRegularExpression parameterExpression(
        QStringLiteral(R"(^\s*(?:parameter|localparam)\s+(?:[A-Za-z_]\w*\s+)?([A-Za-z_]\w*))"));
    const QRegularExpression alwaysExpression(QStringLiteral(R"(^\s*(always_ff|always_comb|always_latch|always)\b)"));
    const QRegularExpression assignExpression(QStringLiteral(R"(^\s*assign\s+(.+?)\s*=)"));
    const QRegularExpression functionExpression(QStringLiteral(R"(^\s*(function|task)\b)"));
    QTreeWidgetItem *parameters = nullptr;
    QTreeWidgetItem *alwaysBlocks = nullptr;
    QTreeWidgetItem *assignments = nullptr;
    QTreeWidgetItem *functions = nullptr;
    for (int index = 0; index < lines.size(); ++index) {
        const auto &line = lines.at(index);
        if (const auto match = parameterExpression.match(line); match.hasMatch()) {
            if (!parameters)
                parameters = addGroup(QStringLiteral("Parameters"));
            addItem(parameters, match.captured(1), index + 1);
        }
        if (const auto match = alwaysExpression.match(line); match.hasMatch()) {
            if (!alwaysBlocks)
                alwaysBlocks = addGroup(QStringLiteral("Always blocks"));
            addItem(alwaysBlocks, match.captured(1), index + 1);
        }
        if (const auto match = assignExpression.match(line); match.hasMatch()) {
            if (!assignments)
                assignments = addGroup(QStringLiteral("Assignments"));
            addItem(assignments, QStringLiteral("assign %1").arg(match.captured(1).trimmed()), index + 1);
        }
        if (const auto match = functionExpression.match(line); match.hasMatch()) {
            if (!functions)
                functions = addGroup(QStringLiteral("Functions / Tasks"));
            addItem(functions, match.captured(1), index + 1);
        }
    }
    outlineTree_->expandAll();
}

void ImplementationPage::refreshIpLibraries() {
    if (!ipLibrariesTree_)
        return;
    QSignalBlocker blocker(ipLibrariesTree_);
    ipLibrariesTree_->clear();
    const auto addGroup = [&](const QString &title) {
        auto *item = new QTreeWidgetItem(ipLibrariesTree_);
        item->setText(0, title);
        item->setFirstColumnSpanned(true);
        auto font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        return item;
    };
    const auto addValue = [](QTreeWidgetItem *parent, const QString &name, const QString &details, ice::ObjectId id = 0,
                             const QString &tooltip = {}) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, name);
        item->setText(1, details);
        if (id != 0)
            item->setData(0, Qt::UserRole, static_cast<qulonglong>(id));
        if (!tooltip.isEmpty())
            item->setToolTip(0, tooltip);
        return item;
    };

    const auto *project = controller_->project();
    auto *dependencies = addGroup(QStringLiteral("Project Packages"));
    if (!project || project->packageDependencies.empty())
        addValue(dependencies, QStringLiteral("(none)"), QString());
    else
        for (const auto &dependency : project->packageDependencies)
            addValue(dependencies, QString::fromStdString(dependency), QString());

    auto *pdks = addGroup(QStringLiteral("Installed PDK Packages"));
    const auto installed = ice::discoverPdks();
    if (installed.empty()) {
        addValue(pdks, QStringLiteral("(none)"), QString());
    } else {
        for (const auto &pdk : installed) {
            const auto details = pdk.packageVersion.empty() ? QString() : QString::fromStdString(pdk.packageVersion);
            addValue(pdks, QString::fromStdString(pdk.id), details, 0,
                     QString::fromStdString(pdk.packageRoot.generic_string()));
        }
    }

    auto *modules = addGroup(QStringLiteral("HDL Modules"));
    if (!project) {
        addValue(modules, QStringLiteral("(none)"), QString());
    } else {
        std::vector<std::pair<std::string, ice::ObjectId>> cells;
        for (const auto &[id, cell] : project->schematicCells) {
            if (cell.systemVerilogSchematic)
                cells.emplace_back(cell.name, id);
        }
        std::sort(cells.begin(), cells.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });
        if (cells.empty())
            addValue(modules, QStringLiteral("(none)"), QString());
        for (const auto &[name, id] : cells) {
            const auto &cell = project->schematicCells.at(id);
            const auto parsed = ice::parseSystemVerilogModule(cell);
            const auto details =
                parsed.ok ? QString::fromStdString(parsed.module.name) : QStringLiteral("SystemVerilog");
            addValue(modules, QString::fromStdString(name) + QStringLiteral(".sv"), details, id,
                     QStringLiteral("Double-click to open"));
        }
    }
    ipLibrariesTree_->expandAll();
}

void ImplementationPage::refreshProperties() {
    if (hdlMode()) {
        refreshOutline();
        refreshIpLibraries();
        return;
    }
    QSignalBlocker blocker(propertiesTree_);
    propertiesTree_->clear();
    const auto section = [&](const QString &title) {
        auto *item = new QTreeWidgetItem(propertiesTree_);
        item->setText(0, title);
        item->setFirstColumnSpanned(true);
        auto font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        return item;
    };
    const auto value = [](QTreeWidgetItem *parent, const QString &key, const QString &text) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, key);
        item->setText(1, text);
        item->setToolTip(1, text);
    };

    const auto *run = controller_->activeRun();
    if (!run)
        return;
    auto *runItem = section(QStringLiteral("Run"));
    value(runItem, QStringLiteral("Name"), QString::fromStdString(run->name));
    value(runItem, QStringLiteral("Top"), QString::fromStdString(run->config.top));
    value(runItem, QStringLiteral("PDK"), QString::fromStdString(run->config.pdk));
    value(runItem, QStringLiteral("Stack"), QString::fromStdString(run->config.stack));
    value(runItem, QStringLiteral("Status"), runStatusText(run->status));
    value(runItem, QStringLiteral("Current stage"), stageTitle(run->currentStage));
    value(runItem, QStringLiteral("Directory"), QString::fromStdString(run->directory));
    value(runItem, QStringLiteral("Config hash"), QString::fromStdString(run->configHash));
    if (run->resultLayout != 0)
        value(runItem, QStringLiteral("Layout"), QString::number(run->resultLayout));

    const auto state = run->stages.find(stage_);
    if (state != run->stages.end()) {
        auto *stageItem = section(stageTitle(stage_));
        value(stageItem, QStringLiteral("Status"), statusText(state->second.status));
        value(stageItem, QStringLiteral("Started"), QString::fromStdString(state->second.startedAt));
        value(stageItem, QStringLiteral("Finished"), QString::fromStdString(state->second.finishedAt));
        value(stageItem, QStringLiteral("Artifacts"), QString::number(state->second.artifacts.size()));
        if (!state->second.error.empty())
            value(stageItem, QStringLiteral("Error"), QString::fromStdString(state->second.error));
    }

    if (!run->toolVersions.empty()) {
        auto *toolsItem = section(QStringLiteral("Tools"));
        for (const auto &[name, version] : run->toolVersions)
            value(toolsItem, QString::fromStdString(name), QString::fromStdString(version));
    }

    auto *settingsItem = section(QStringLiteral("Settings"));
    value(settingsItem, QStringLiteral("Corner"), QString::fromStdString(run->config.corner));
    value(settingsItem, QStringLiteral("Threads"), QString::number(run->config.threads));
    value(settingsItem, QStringLiteral("Timeout (s)"), QString::number(run->config.timeoutSeconds));
    value(settingsItem, QStringLiteral("Utilization"), QString::number(run->config.utilization, 'g', 6));
    value(settingsItem, QStringLiteral("Aspect ratio"), QString::number(run->config.aspectRatio, 'g', 6));
    value(settingsItem, QStringLiteral("CTS"),
          run->config.ctsEnabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled"));
    if (!run->config.routingMinLayer.empty())
        value(settingsItem, QStringLiteral("Routing min"), QString::fromStdString(run->config.routingMinLayer));
    if (!run->config.routingMaxLayer.empty())
        value(settingsItem, QStringLiteral("Routing max"), QString::fromStdString(run->config.routingMaxLayer));
    propertiesTree_->expandAll();
}

void ImplementationPage::refresh() {
    if (hdlMode()) {
        refreshExplorer();
        refreshProperties();
        return;
    }
    if (synthesisMode()) {
        ensureMiddlePlaceholder();
        refreshSynthesisExplorer();
        refreshRunsTable();
        refreshArtifacts();
        return;
    }
    refreshNetlistExplorer();
    refreshRunTree();
    refreshStageForm();
    updateStageActions();
    const auto *run = controller_->activeRun();
    if (!run) {
        metricsTable_->setRowCount(0);
        artifactsList_->clear();
        if (previewCanvas_)
            previewCanvas_->setPreview({}, 0, QStringLiteral("Select an implementation run in the Runs panel."), {},
                                       controller_->project());
        refreshProperties();
        return;
    }
    refreshMetrics();
    refreshArtifacts();
    refreshProperties();
    if (isVisible())
        refreshPreview();
}

void ImplementationPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (previewCanvas_)
        refreshPreview();
}

void ImplementationPage::refreshMetrics() {
    metricsTable_->setRowCount(0);
    const auto *run = controller_->activeRun();
    if (!run)
        return;
    std::map<QString, std::vector<std::pair<QString, double>>> groups;
    for (const auto &[name, value] : run->metrics) {
        const auto full = QString::fromStdString(name);
        auto group = full.section(QStringLiteral("__"), 0, 0);
        if (group.isEmpty())
            group = QStringLiteral("Other");
        groups[group].push_back({full, value});
    }
    int row = 0;
    const auto addSection = [&](const QString &name) {
        metricsTable_->insertRow(row);
        auto *item = new QTableWidgetItem(name);
        auto font = item->font();
        font.setBold(true);
        item->setFont(font);
        item->setFlags(Qt::NoItemFlags);
        metricsTable_->setItem(row, 0, item);
        metricsTable_->setSpan(row, 0, 1, 2);
        ++row;
    };
    const auto formatValue = [](const QString &name, double value) -> QString {
        if (name.startsWith(QStringLiteral("timing__")) && std::abs(value) <= 1.0e20)
            return QStringLiteral("%1 ns").arg(value * 1.0e9, 0, 'f', 4);
        if (name.startsWith(QStringLiteral("power__")))
            return QStringLiteral("%1 mW").arg(value * 1.0e3, 0, 'f', 4);
        if (name.endsWith(QStringLiteral("__area")) || name.endsWith(QStringLiteral("__area__stdcell")))
            return QStringLiteral("%1 µm²").arg(value, 0, 'f', 2);
        if (name.endsWith(QStringLiteral("__wirelength")))
            return QStringLiteral("%1 µm").arg(value, 0, 'f', 2);
        return QString::number(value, 'g', 12);
    };
    for (auto &[group, metrics] : groups) {
        std::sort(metrics.begin(), metrics.end(),
                  [](const auto &left, const auto &right) { return left.first < right.first; });
        addSection(group);
        for (const auto &[name, value] : metrics) {
            metricsTable_->insertRow(row);
            metricsTable_->setItem(row, 0, new QTableWidgetItem(name));
            auto *valueItem = new QTableWidgetItem(formatValue(name, value));
            valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            metricsTable_->setItem(row, 1, valueItem);
            ++row;
        }
    }
}

void ImplementationPage::refreshArtifacts() {
    artifactsList_->clear();
    const auto *run = controller_->activeRun();
    if (!run)
        return;
    std::vector<std::string> artifacts;
    const auto append = [&](const std::string &artifact) {
        if (std::find(artifacts.begin(), artifacts.end(), artifact) == artifacts.end())
            artifacts.push_back(artifact);
    };
    const auto appendStage = [&](ice::ImplementationStage stage) {
        const auto state = run->stages.find(stage);
        if (state == run->stages.end())
            return;
        for (const auto &artifact : state->second.artifacts)
            append(artifact);
    };
    const auto isSynthesisArtifact = [](const std::string &artifact) {
        return artifact.ends_with("/reports/elaboration.svg") || artifact == "reports/elaboration.svg" ||
               artifact.ends_with("/reports/elaboration.dot") || artifact == "reports/elaboration.dot" ||
               artifact.ends_with("/reports/synthesis.svg") || artifact == "reports/synthesis.svg" ||
               artifact.ends_with("/reports/synthesis.dot") || artifact == "reports/synthesis.dot" ||
               artifact.ends_with("/synth/2_tech_map.v") || artifact == "synth/2_tech_map.v";
    };
    if (synthesisMode()) {
        appendStage(ice::ImplementationStage::elaboration);
        appendStage(ice::ImplementationStage::synthesis);
        for (const auto &artifact : run->artifacts) {
            if (isSynthesisArtifact(artifact))
                append(artifact);
        }
        artifacts.erase(std::remove_if(artifacts.begin(), artifacts.end(),
                                       [&](const auto &artifact) { return !isSynthesisArtifact(artifact); }),
                        artifacts.end());
    } else {
        appendStage(stage_);
        const auto runRoot = std::filesystem::path(controller_->projectRoot().toStdString()) / run->directory;
        for (const auto *heatmap :
             {"reports/placement-heatmap.csv", "reports/routing-heatmap.csv", "reports/power-heatmap.csv",
              "reports/pin-heatmap.csv", "reports/rudy-heatmap.csv", "reports/irdrop-heatmap.csv"}) {
            std::error_code status;
            if (std::filesystem::is_regular_file(runRoot / heatmap, status) && !status)
                append(heatmap);
        }
    }
    const auto pathFor = [](const std::string &artifact) -> QString { return QString::fromStdString(artifact); };
    const auto labelFor = [&](const std::string &artifact) -> QString {
        if (synthesisMode()) {
            if (artifact.ends_with("/reports/elaboration.svg") || artifact == "reports/elaboration.svg")
                return QStringLiteral("Elaboration graph (elaboration.svg)");
            if (artifact.ends_with("/reports/elaboration.dot") || artifact == "reports/elaboration.dot")
                return QStringLiteral("Elaboration graph source (elaboration.dot)");
            if (artifact.ends_with("/reports/synthesis.svg") || artifact == "reports/synthesis.svg")
                return QStringLiteral("Synthesis graph (synthesis.svg)");
            if (artifact.ends_with("/reports/synthesis.dot") || artifact == "reports/synthesis.dot")
                return QStringLiteral("Synthesis graph source (synthesis.dot)");
            if (artifact.ends_with("/synth/2_tech_map.v") || artifact == "synth/2_tech_map.v")
                return QStringLiteral("Technology-mapped netlist (2_tech_map.v)");
        }
        return QString::fromStdString(artifact);
    };
    for (const auto &artifact : artifacts) {
        auto *item = new QListWidgetItem(labelFor(artifact));
        item->setToolTip(pathFor(artifact));
        item->setData(Qt::UserRole, pathFor(artifact));
        artifactsList_->addItem(item);
    }
}

void ImplementationPage::refreshDisplayControls() {
    if (!displayTree_)
        return;
    QSignalBlocker blocker(displayTree_);
    displayTree_->clear();
    if (!previewCanvas_)
        return;
    const auto addGroup = [&](const QString &name) {
        auto *group = new QTreeWidgetItem(displayTree_);
        group->setText(0, name);
        auto font = group->font(0);
        font.setBold(true);
        group->setFont(0, font);
        group->setFirstColumnSpanned(true);
        group->setExpanded(true);
        return group;
    };
    const auto addLeaf = [&](QTreeWidgetItem *parent, const QString &label, bool visible, const QString &group,
                             const QString &key, const QIcon &icon = {}) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, group);
        item->setData(0, Qt::UserRole + 1, key);
        if (!icon.isNull())
            item->setIcon(0, icon);
        return item;
    };

    const auto layers = previewCanvas_->layers();
    if (!layers.empty()) {
        auto *group = addGroup(QStringLiteral("Layers"));
        for (const auto &layer : layers)
            addLeaf(group, layer.label, layer.visible, QStringLiteral("layer"), layer.key,
                    QIcon(layerPreviewIcon({layer.color, Qt::SolidPattern})));
    }

    const auto categories = previewCanvas_->instanceCategories();
    if (!categories.empty()) {
        auto *group = addGroup(QStringLiteral("Instances"));
        for (const auto &[category, count] : categories) {
            const auto label = category.isEmpty() ? QStringLiteral("other") : category;
            addLeaf(group, QStringLiteral("%1 (%2)").arg(label).arg(count),
                    previewCanvas_->instanceCategoryVisible(label), QStringLiteral("instance"), label);
        }
    }

    const auto netTypes = previewCanvas_->netTypes();
    if (!netTypes.empty()) {
        auto *group = addGroup(QStringLiteral("Nets"));
        for (const auto &[type, count] : netTypes) {
            const auto label = type.isEmpty() ? QStringLiteral("signal") : type;
            addLeaf(group, QStringLiteral("%1 (%2)").arg(label).arg(count), previewCanvas_->netTypeVisible(label),
                    QStringLiteral("net"), label);
        }
    }

    auto *options = addGroup(QStringLiteral("Options"));
    addLeaf(options, QStringLiteral("Special nets"), previewCanvas_->showSpecialNets(), QStringLiteral("option"),
            QStringLiteral("special"));
    addLeaf(options, QStringLiteral("Ports"), previewCanvas_->showPorts(), QStringLiteral("option"),
            QStringLiteral("ports"));
    if (previewCanvas_->hasGuides())
        addLeaf(options, QStringLiteral("Route guides"), previewCanvas_->showGuides(), QStringLiteral("option"),
                QStringLiteral("guides"));
    if (previewCanvas_->hasRows())
        addLeaf(options, QStringLiteral("Rows"), previewCanvas_->showRows(), QStringLiteral("option"),
                QStringLiteral("rows"));
    if (previewCanvas_->hasBlockages())
        addLeaf(options, QStringLiteral("Blockages"), previewCanvas_->showBlockages(), QStringLiteral("option"),
                QStringLiteral("blockages"));
    displayTree_->expandAll();
    if (displayTree_->topLevelItemCount() == 0) {
        auto *item = new QTreeWidgetItem(displayTree_);
        item->setText(0, QStringLiteral("No display data in this preview."));
        item->setFlags(Qt::NoItemFlags);
    }
}

void ImplementationPage::refreshInspector() {
    if (!inspectorTree_)
        return;
    QSignalBlocker blocker(inspectorTree_);
    inspectorTree_->clear();
    if (!previewCanvas_) {
        return;
    }
    const auto selection = previewCanvas_->selection();
    if (!selection.valid()) {
        auto *item = new QTreeWidgetItem(inspectorTree_);
        item->setText(0, QStringLiteral("Select an object in the canvas."));
        item->setFirstColumnSpanned(true);
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    const auto add = [&](const QString &name, const QString &value) {
        auto *item = new QTreeWidgetItem(inspectorTree_);
        item->setText(0, name);
        item->setText(1, value);
        item->setToolTip(1, value);
    };
    const auto kind =
        selection.kind == ImplementationPreviewCanvas::SelectionKind::instance ? QStringLiteral("Instance")
        : selection.kind == ImplementationPreviewCanvas::SelectionKind::port   ? QStringLiteral("Port")
        : selection.kind == ImplementationPreviewCanvas::SelectionKind::shape  ? QStringLiteral("Shape")
                                                                               : QStringLiteral("DRC marker");
    add(QStringLiteral("Kind"), kind);
    if (!selection.name.isEmpty())
        add(QStringLiteral("Name"), selection.name);
    if (!selection.master.isEmpty())
        add(QStringLiteral("Master"), selection.master);
    if (!selection.category.isEmpty())
        add(QStringLiteral("Category"), selection.category);
    if (!selection.module.isEmpty())
        add(QStringLiteral("Module"), selection.module);
    if (!selection.net.isEmpty())
        add(QStringLiteral("Net"), selection.net);
    if (!selection.layer.isEmpty())
        add(QStringLiteral("Layer"), selection.layer);
    if (!selection.type.isEmpty())
        add(QStringLiteral("Type"), selection.type);
    if (!selection.sources.isEmpty())
        add(QStringLiteral("Sources"), selection.sources);
    if (!selection.comment.isEmpty())
        add(QStringLiteral("Comment"), selection.comment);
    if (selection.special)
        add(QStringLiteral("Special net"), QStringLiteral("yes"));
    if (selection.rectangle.isValid())
        add(QStringLiteral("Bounds"), QStringLiteral("%1 × %2 µm")
                                          .arg(selection.rectangle.width() / 1000.0, 0, 'f', 3)
                                          .arg(selection.rectangle.height() / 1000.0, 0, 'f', 3));
    inspectorTree_->expandAll();
}

void ImplementationPage::refreshDrc() {
    if (!drcTable_)
        return;
    drcTable_->setRowCount(0);
    if (!previewCanvas_)
        return;
    const auto markers = previewCanvas_->drcMarkers();
    for (int row = 0; row < static_cast<int>(markers.size()); ++row) {
        const auto &marker = markers[static_cast<std::size_t>(row)];
        drcTable_->insertRow(row);
        drcTable_->setItem(row, 0, new QTableWidgetItem(marker.type));
        drcTable_->setItem(row, 1, new QTableWidgetItem(marker.layer));
        auto *sources = new QTableWidgetItem(marker.sources);
        sources->setToolTip(marker.comment.isEmpty() ? marker.sources : marker.comment);
        drcTable_->setItem(row, 2, sources);
    }
    if (markers.empty()) {
        drcTable_->insertRow(0);
        auto *item = new QTableWidgetItem(QStringLiteral("No DRC markers."));
        item->setFlags(Qt::NoItemFlags);
        drcTable_->setItem(0, 0, item);
        drcTable_->setSpan(0, 0, 1, 3);
    }
}

void ImplementationPage::refreshTiming() {
    if (!timingTable_)
        return;
    timingTable_->setRowCount(0);
    const auto *run = controller_->activeRun();
    if (!run)
        return;
    int row = 0;
    const auto add = [&](const QString &name, const QString &value) {
        timingTable_->insertRow(row);
        timingTable_->setItem(row, 0, new QTableWidgetItem(name));
        auto *item = new QTableWidgetItem(value);
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        timingTable_->setItem(row, 1, item);
        ++row;
    };
    for (const auto &[name, value] : run->metrics) {
        if (name.starts_with("timing__")) {
            if (std::abs(value) > 1.0e20)
                add(QString::fromStdString(name), QStringLiteral("unconstrained"));
            else
                add(QString::fromStdString(name), QStringLiteral("%1 ns").arg(value * 1.0e9, 0, 'f', 4));
        } else if (name.starts_with("power__")) {
            add(QString::fromStdString(name), QStringLiteral("%1 mW").arg(value * 1.0e3, 0, 'f', 4));
        } else if (name == "route__drc_errors") {
            add(QString::fromStdString(name), QString::number(value, 'g', 8));
        } else if (name.starts_with("antenna__")) {
            add(QString::fromStdString(name), QString::number(value, 'g', 8));
        }
    }
    if (row == 0) {
        timingTable_->insertRow(0);
        auto *item = new QTableWidgetItem(QStringLiteral("No timing metrics for this run yet."));
        item->setFlags(Qt::NoItemFlags);
        timingTable_->setItem(0, 0, item);
        timingTable_->setSpan(0, 0, 1, 2);
    }
}

void ImplementationPage::refreshClock() {
    if (!clockTree_)
        return;
    QSignalBlocker blocker(clockTree_);
    clockTree_->clear();
    if (!previewCanvas_)
        return;
    int clocks = 0;
    for (const auto &net : previewCanvas_->nets()) {
        if (net.type.toLower() != QStringLiteral("clock"))
            continue;
        auto *item = new QTreeWidgetItem(clockTree_);
        item->setText(0, net.name);
        item->setText(1, QString::number(net.termCount));
        item->setToolTip(0, net.special ? QStringLiteral("Special clock net") : QStringLiteral("Clock net"));
        for (const auto &term : net.terms) {
            auto *child = new QTreeWidgetItem(item);
            child->setText(0, term);
        }
        item->setExpanded(item->childCount() <= 32);
        ++clocks;
    }
    if (clocks == 0) {
        auto *item = new QTreeWidgetItem(clockTree_);
        item->setText(0, QStringLiteral("No clock nets in the current preview."));
        item->setFlags(Qt::NoItemFlags);
    }
}

void ImplementationPage::refreshHierarchy() {
    if (!hierarchyTree_)
        return;
    QSignalBlocker blocker(hierarchyTree_);
    hierarchyTree_->clear();
    if (!previewCanvas_)
        return;
    const auto modules = previewCanvas_->modules();
    if (modules.empty()) {
        auto *item = new QTreeWidgetItem(hierarchyTree_);
        item->setText(0, QStringLiteral("No module hierarchy in this preview."));
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    std::map<int, QTreeWidgetItem *> items;
    for (const auto &module : modules) {
        auto *parent = items.find(module.parentId) == items.end() ? nullptr : items.at(module.parentId);
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(hierarchyTree_);
        item->setText(0, module.name);
        item->setText(1, QString::number(module.instanceCount));
        items[module.id] = item;
    }
    hierarchyTree_->expandAll();
}

void ImplementationPage::refreshPreview() {
    if (!previewCanvas_)
        return;
    if (previewStatusLabel_)
        previewStatusLabel_->setVisible(false);
    if (physicalStageMode()) {
        const auto *run = controller_->activeRun();
        if (run) {
            const auto stage = run->stages.find(stage_);
            if (stage != run->stages.end() && stage->second.status != ice::ImplementationStageStatus::succeeded &&
                stage->second.status != ice::ImplementationStageStatus::running) {
                const auto title = stageTitle(stage_);
                QString message;
                if (stage->second.status == ice::ImplementationStageStatus::failed)
                    message = QStringLiteral("%1 failed. Check the stage log, adjust the settings, then run it again.")
                                  .arg(title);
                else if (stage->second.status == ice::ImplementationStageStatus::skipped)
                    message = QStringLiteral("%1 is skipped for this run.").arg(title);
                else
                    message = QStringLiteral("%1 settings changed or the stage has not run yet.\n"
                                             "Run the stage to generate the DEF preview.")
                                  .arg(title);
                previewCanvas_->setPreview({}, 0, message, {}, controller_->project());
                refreshDisplayControls();
                refreshInspector();
                refreshDrc();
                refreshTiming();
                refreshClock();
                refreshHierarchy();
                return;
            }
        }
    }
    if (previewStatusLabel_) {
        previewStatusLabel_->setText(QStringLiteral("Loading preview…"));
        previewStatusLabel_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
        previewStatusLabel_->setVisible(true);
    }
    QPointer<ImplementationPage> self(this);
    const auto runId = controller_->activeRunId();
    controller_->requestStagePreview(
        stage_, [self, runId, stage = stage_](const ImplementationUiController::StagePreview &preview) {
            if (!self || self->controller_->activeRunId() != runId)
                return;
            self->previewCanvas_->setPreview(preview.project, preview.cell, preview.message, preview.model,
                                             preview.project ? preview.project.get() : self->controller_->project());
            if (self->previewStatusLabel_) {
                if (preview.model && preview.model->truncated) {
                    self->previewStatusLabel_->setText(
                        QStringLiteral("Preview truncated by the OpenDB reader; some routed geometry is not shown."));
                    self->previewStatusLabel_->setStyleSheet(QStringLiteral("color: #a16207;"));
                    self->previewStatusLabel_->setVisible(true);
                } else {
                    self->previewStatusLabel_->setVisible(false);
                }
            }
            self->refreshDisplayControls();
            self->refreshInspector();
            self->refreshDrc();
            self->refreshTiming();
            self->refreshClock();
            self->refreshHierarchy();
            (void)stage;
        });
}

PhysicalDesignPage::PhysicalDesignPage(ImplementationUiController *controller, QWidget *parent)
    : QWidget(parent), controller_(controller) {
    stages_ = {ice::ImplementationStage::floorplan, ice::ImplementationStage::placement, ice::ImplementationStage::cts,
               ice::ImplementationStage::routing, ice::ImplementationStage::final};
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stageBar_ = new QTabBar;
    stageBar_->setDocumentMode(true);
    stageBar_->setExpanding(false);
    stageBar_->setDrawBase(false);
    for (const auto stage : stages_)
        stageBar_->addTab(stageTitle(stage));
    layout->addWidget(stageBar_);
    stageStack_ = new QStackedWidget;
    for (const auto stage : stages_) {
        auto *page = new ImplementationPage(stage, controller, stageStack_, true);
        page->setStageActivatedCallback([this](ice::ImplementationStage activated) { setCurrentStage(activated); });
        pages_.push_back(page);
        stageStack_->addWidget(page);
    }
    layout->addWidget(stageStack_, 1);
    QObject::connect(stageBar_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && stageStack_)
            stageStack_->setCurrentIndex(index);
    });
    stageStack_->setCurrentIndex(0);
    stageTimer_ = new QTimer(this);
    stageTimer_->setInterval(750);
    QObject::connect(stageTimer_, &QTimer::timeout, this, [this] { refreshStageBar(); });
    stageTimer_->start();
    refreshStageBar();
}

void PhysicalDesignPage::refreshStageBar() {
    if (!stageBar_ || !controller_)
        return;
    const auto *run = controller_->activeRun();
    for (int index = 0; index < stageBar_->count(); ++index) {
        const auto stage = stageAt(index);
        auto label = stageTitle(stage);
        QColor color = palette().color(QPalette::Text);
        if (run) {
            const auto found = run->stages.find(stage);
            if (found != run->stages.end()) {
                switch (found->second.status) {
                case ice::ImplementationStageStatus::running:
                    label = QStringLiteral("▶ %1").arg(label);
                    color = QColor(37, 99, 235);
                    break;
                case ice::ImplementationStageStatus::succeeded:
                    label = QStringLiteral("✓ %1").arg(label);
                    color = QColor(21, 128, 61);
                    break;
                case ice::ImplementationStageStatus::failed:
                    label = QStringLiteral("✗ %1").arg(label);
                    color = QColor(185, 28, 28);
                    break;
                case ice::ImplementationStageStatus::cancelled:
                    label = QStringLiteral("■ %1").arg(label);
                    color = QColor(161, 98, 7);
                    break;
                case ice::ImplementationStageStatus::skipped:
                    label = QStringLiteral("– %1").arg(label);
                    color = QColor(107, 114, 128);
                    break;
                case ice::ImplementationStageStatus::pending:
                    break;
                }
            }
        }
        stageBar_->setTabText(index, label);
        stageBar_->setTabTextColor(index, color);
    }
}

void PhysicalDesignPage::setCurrentStage(ice::ImplementationStage stage) {
    const auto index = stageIndex(stage);
    if (index < 0 || !stageStack_)
        return;
    stageStack_->setCurrentIndex(index);
    if (stageBar_) {
        QSignalBlocker blocker(stageBar_);
        stageBar_->setCurrentIndex(index);
    }
}

ice::ImplementationStage PhysicalDesignPage::currentStage() const {
    return stageAt(stageStack_ ? stageStack_->currentIndex() : 0);
}

std::vector<ImplementationPage *> PhysicalDesignPage::pages() const { return pages_; }

ImplementationPage *PhysicalDesignPage::pageForStage(ice::ImplementationStage stage) const {
    const auto index = stageIndex(stage);
    return index >= 0 && index < static_cast<int>(pages_.size()) ? pages_[index] : nullptr;
}

void PhysicalDesignPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (auto *page = pageForStage(currentStage()))
        page->refresh();
}

int PhysicalDesignPage::stageIndex(ice::ImplementationStage stage) const {
    for (int index = 0; index < static_cast<int>(stages_.size()); ++index) {
        if (stages_[index] == stage)
            return index;
    }
    return -1;
}

ice::ImplementationStage PhysicalDesignPage::stageAt(int index) const {
    if (index < 0 || index >= static_cast<int>(stages_.size()))
        return ice::ImplementationStage::floorplan;
    return stages_[index];
}
