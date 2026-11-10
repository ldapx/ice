#include "implementation_ui.hpp"

#include "icelib/hdl.hpp"
#include "icelib/interchange.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "layout_canvas.hpp"
#include "layout_render.hpp"
#include "text_document_editor.hpp"
#include "ui_helpers.hpp"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
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
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
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
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
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
        if (device.rectangle.isValid()) {
            model->plannedDevices.push_back(std::move(device));
            if (!validBounds)
                model->bounds = model->bounds.united(model->plannedDevices.back().rectangle);
        }
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
        if (heatMapKind_ == kind)
            return;
        heatMapKind_ = kind;
        rebuild();
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

  private:
    static QColor heatMapColor(double value, double minimum, double maximum) {
        const auto span = maximum - minimum;
        const auto normalized = span > 0.0 ? std::clamp((value - minimum) / span, 0.0, 1.0) : 0.0;
        return QColor::fromHsvF(0.66 * (1.0 - normalized), 0.78, 0.92, 0.42);
    }

    void addHeatMap(const ice::LayoutPreviewHeatMap &heatmap) {
        if (heatmap.columns <= 0 || heatmap.rows <= 0 || heatmap.bounds.width() <= 0.0 ||
            heatmap.bounds.height() <= 0.0 || heatmap.maximum <= heatmap.minimum)
            return;
        const auto bin_width = heatmap.bounds.width() / heatmap.columns;
        const auto bin_height = heatmap.bounds.height() / heatmap.rows;
        for (int row = 0; row < heatmap.rows; ++row) {
            for (int column = 0; column < heatmap.columns; ++column) {
                const auto index = static_cast<std::size_t>(row) * heatmap.columns + column;
                if (index >= heatmap.values.size())
                    continue;
                const auto value = heatmap.values[index];
                if (value <= heatmap.minimum && heatmap.maximum > heatmap.minimum)
                    continue;
                const auto x = heatmap.bounds.x() + column * bin_width;
                const auto y = heatmap.bounds.y() + row * bin_height;
                auto *item = scene()->addRect(QRectF(x, -(y + bin_height), bin_width, bin_height), Qt::NoPen,
                                              heatMapColor(value, heatmap.minimum, heatmap.maximum));
                item->setZValue(-1.0);
            }
        }
    }

    void rebuild() {
        scene()->clear();
        fitOnResize_ = true;
        std::optional<ice::LayoutPreviewModel> fallback;
        const ice::LayoutPreviewModel *model = model_.get();
        if (!model && project_ && cell_ != 0) {
            const auto found = project_->layoutCells.find(cell_);
            if (found != project_->layoutCells.end())
                fallback = ice::buildLayoutCellPreviewModel(*project_, found->second, true, true, true);
            if (fallback && !fallback->empty())
                model = &*fallback;
        }
        if (model && !model->empty()) {
            const auto styles = styleProject_ ? layerStylesFor(*styleProject_) : ice::LayoutRenderer::LayerStyleMap{};
            if (heatMapKind_ == ice::LayoutHeatMapKind::placement_density && model->placementDensity)
                addHeatMap(*model->placementDensity);
            else if (heatMapKind_ == ice::LayoutHeatMapKind::routing_congestion && model->routingCongestion)
                addHeatMap(*model->routingCongestion);
            auto *item = createLayoutPreviewItem(*model, styles, {});
            scene()->addItem(item);
            scene()->setSceneRect(scene()->itemsBoundingRect());
            fitToScene();
            return;
        }
        auto *text = scene()->addText(message_.isEmpty() ? QStringLiteral("No preview available.") : message_);
        auto font = text->font();
        font.setPointSize(font.pointSize() + 1);
        text->setFont(font);
        text->setDefaultTextColor(palette().color(QPalette::Text));
        scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-40, -40, 40, 40));
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
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("-Tsvg"), QString::fromStdString(dot->string()), QStringLiteral("-o"),
                          QString::fromStdString(svgPath.string())});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000) || !process.waitForFinished(30000))
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
    if (process_) {
        process_->disconnect();
        process_->kill();
        process_->waitForFinished(1000);
        process_ = nullptr;
    }
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
    const auto key = QStringLiteral("%1:%2:%3:%4:%5:%6")
                         .arg(static_cast<qulonglong>(run->id))
                         .arg(static_cast<int>(stage))
                         .arg(odbExists ? 1 : 0)
                         .arg(defExists ? 1 : 0)
                         .arg(static_cast<qlonglong>(stamp))
                         .arg(static_cast<qulonglong>(size));
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
                                       QWidget *parent)
    : QWidget(parent), stage_(stage), controller_(controller) {
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
            leftTabs_->addTab(netlistList_, QStringLiteral("Netlists"));
        }
        runTree_ = new QTreeWidget;
        runTree_->setHeaderLabels({QStringLiteral("Run / Stage"), QStringLiteral("Status")});
        runTree_->setRootIsDecorated(true);
        runTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        runTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        if (physicalStageMode()) {
            auto *runsPage = new QWidget;
            auto *runsLayout = new QVBoxLayout(runsPage);
            runsLayout->setContentsMargins(0, 0, 0, 0);
            runsLayout->setSpacing(4);
            runsLayout->addWidget(runTree_, 1);
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
            leftTabs_->addTab(runsPage, QStringLiteral("Runs"));
        } else {
            leftTabs_->addTab(runTree_, QStringLiteral("Runs"));
        }
        artifactsList_ = new QListWidget;
        leftTabs_->addTab(artifactsList_, QStringLiteral("Artifacts"));
        if (netlistList_)
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
        auto *previewPage = new QWidget;
        auto *previewLayout = new QVBoxLayout(previewPage);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(2);
        auto *overlayRow = new QHBoxLayout;
        overlayRow->setContentsMargins(6, 4, 6, 0);
        overlayRow->addWidget(new QLabel(QStringLiteral("Overlay")));
        heatMapCombo_ = new QComboBox;
        heatMapCombo_->addItem(QStringLiteral("None"), static_cast<int>(ice::LayoutHeatMapKind::none));
        heatMapCombo_->addItem(QStringLiteral("Placement density"),
                               static_cast<int>(ice::LayoutHeatMapKind::placement_density));
        heatMapCombo_->addItem(QStringLiteral("Routing congestion"),
                               static_cast<int>(ice::LayoutHeatMapKind::routing_congestion));
        heatMapCombo_->addItem(QStringLiteral("Power density (CSV)"), 100);
        overlayRow->addWidget(heatMapCombo_);
        auto *exportHeatMapButton = new QPushButton(QStringLiteral("Export CSV"));
        exportHeatMapButton->setToolTip(
            QStringLiteral("Write the selected OpenROAD heat map under the run reports directory."));
        overlayRow->addWidget(exportHeatMapButton);
        overlayRow->addStretch(1);
        previewLayout->addLayout(overlayRow);
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
            if (kind.isEmpty()) {
                QMessageBox::information(this, QStringLiteral("Export heat map"),
                                         QStringLiteral("Select a heat map to export."));
                return;
            }
            QString error;
            const auto path = controller_->exportHeatMap(stage_, kind, error);
            if (path.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("Export heat map"), error);
                return;
            }
            refreshArtifacts();
            QMessageBox::information(this, QStringLiteral("Export heat map"), QStringLiteral("Wrote %1.").arg(path));
        });
        previewCanvas_ = new ImplementationPreviewCanvas;
        previewLayout->addWidget(previewCanvas_, 1);
        QObject::connect(heatMapCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
            if (!previewCanvas_ || !heatMapCombo_)
                return;
            previewCanvas_->setHeatMap(static_cast<ice::LayoutHeatMapKind>(heatMapCombo_->currentData().toInt()));
        });
        middleTabs_->addTab(previewPage, QStringLiteral("Preview"));
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
        metricsTable_->horizontalHeader()->setStretchLastSection(true);
        metricsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        metricsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        rightTabs_->addTab(metricsTable_, QStringLiteral("Metrics"));
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
                QObject::connect(dieAreaCheck_, &QCheckBox::toggled, this, [this](bool checked) {
                    if (dieWidthSpin_)
                        dieWidthSpin_->setEnabled(checked);
                    if (dieHeightSpin_)
                        dieHeightSpin_->setEnabled(checked);
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
            rightTabs_->addTab(formPage, stageTitle(stage_));
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
                             if (!value.isValid())
                                 return;
                             controller_->setActiveRunId(static_cast<ice::ObjectId>(value.toULongLong()));
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
        if (runStageButton_) {
            QObject::connect(runStageButton_, &QPushButton::clicked, this, [this] {
                if (!controller_->activeRun() || controller_->busy())
                    return;
                controller_->run(stage_, stage_);
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
    QSignalBlocker blocker(runTree_);
    runTree_->clear();
    const auto *project = controller_->project();
    if (!project)
        return;
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
        }
    }
    if (runStageButton_) {
        runStageButton_->setEnabled(run && !busy && ready);
        runStageButton_->setToolTip(hint);
    }
    if (cancelStageButton_)
        cancelStageButton_->setEnabled(busy);
    if (applyStageButton_)
        applyStageButton_->setEnabled(run && !busy);
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
            die.width = toUnits(dieWidthSpin_->value());
            die.height = toUnits(dieHeightSpin_->value());
            config.dieArea = die;
        } else {
            config.dieArea.reset();
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
    if (!synthesisMode() || !middleTabs_)
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
        editor->setLanguage(TextDocumentLanguage::SystemVerilog);
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
        if (!pdk.implementationReady)
            label += QStringLiteral(" — no implementation data");
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
    int row = 0;
    for (const auto &[name, value] : run->metrics) {
        metricsTable_->insertRow(row);
        metricsTable_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(name)));
        metricsTable_->setItem(row, 1, new QTableWidgetItem(QString::number(value, 'g', 12)));
        ++row;
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
               artifact.ends_with("/reports/synthesis.svg") || artifact == "reports/synthesis.svg" ||
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
             {"reports/placement-heatmap.csv", "reports/routing-heatmap.csv", "reports/power-heatmap.csv"}) {
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
            if (artifact.ends_with("/reports/synthesis.svg") || artifact == "reports/synthesis.svg")
                return QStringLiteral("Synthesis graph (synthesis.svg)");
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

void ImplementationPage::refreshPreview() {
    if (!previewCanvas_)
        return;
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
                return;
            }
        }
    }
    auto preview = controller_->stagePreview(stage_);
    previewCanvas_->setPreview(preview.project, preview.cell, preview.message, preview.model,
                               preview.project ? preview.project.get() : controller_->project());
}
