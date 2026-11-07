#include "app_window.hpp"
#include "app_widgets.hpp"
#include "icelib/core.hpp"
#include "icelib/drc.hpp"
#include "icelib/interchange.hpp"
#include "icelib/layout_library.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/pdk.hpp"
#include "icelib/pex.hpp"
#include "icelib/routing.hpp"
#include "icelib/simulation.hpp"
#include "icelib/simulation_report.hpp"
#include "icelib/simulation_store.hpp"
#include "icelib/vcd.hpp"
#include "icelib/verification.hpp"
#include "layout_canvas.hpp"
#include "layout_devices.hpp"
#include "properties_panel.hpp"
#include "python_highlighter.hpp"
#include "schematic_canvas.hpp"
#include "schematic_utils.hpp"
#include "shortcuts.hpp"
#include "side_panels.hpp"

#include "appearance.hpp"
#include "icons.hpp"
#include "package_ui.hpp"
#include "procedural_layout.hpp"
#include "simulation_plot.hpp"
#include "symbol_ui.hpp"
#include "text_document_editor.hpp"
#include "text_geometry.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsColorizeEffect>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtSvgWidgets/QGraphicsSvgItem>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace {

bool layerAvailableForLayout(const ice::Project &project, ice::ObjectId layoutId, const ice::Layer &layer) {
    if (layoutId == 0 || (layer.layout == 0 && layer.pdk.empty()))
        return true;
    const auto cell = project.layoutCells.find(layoutId);
    if (cell == project.layoutCells.end())
        return false;
    if (layer.layout != 0)
        return layer.layout == layoutId;
    return !layer.pdk.empty() && layer.pdk == cell->second.pdk;
}

std::vector<std::reference_wrapper<const ice::Layer>> layoutLayers(const ice::Project &project,
                                                                   ice::ObjectId layoutId) {
    std::vector<std::reference_wrapper<const ice::Layer>> result;
    for (const auto &[_, layer] : project.layers)
        if (layerAvailableForLayout(project, layoutId, layer))
            result.emplace_back(layer);
    return result;
}

QString layerPurposeSuffix(const ice::Layer &layer) {
    const auto name = QString::fromStdString(layer.name);
    const auto dot = name.indexOf('.');
    if (dot < 0 || dot + 1 >= name.size())
        return {};
    return name.mid(dot + 1).toLower();
}

bool layerIsPinLayer(const ice::Layer &layer) {
    return layerPurposeSuffix(layer) == QStringLiteral("pin") || layer.datatype == 2;
}

bool layerIsDrawingLayer(const ice::Layer &layer) {
    const auto purpose = layerPurposeSuffix(layer);
    return purpose.isEmpty() || purpose == QStringLiteral("drawing");
}

QPen layoutLabelHaloPen() {
    auto color = ice::uiColor(ice::UiColor::Background2);
    color.setAlpha(225);
    QPen pen(color, 2.0);
    pen.setCosmetic(true);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

QPainterPath layoutShapeScenePath(const ice::Shape &shape) {
    if (shape.isText())
        return {};
    if (shape.polygon.size() >= 3) {
        QPolygonF polygon;
        polygon.reserve(static_cast<int>(shape.polygon.size()));
        for (const auto &point : shape.polygon)
            polygon << QPointF(point.x, -point.y);
        QPainterPath path;
        path.addPolygon(polygon);
        path.closeSubpath();
        return path;
    }
    if (shape.isPath() && !shape.centerline.empty()) {
        QPainterPath centerline;
        centerline.moveTo(shape.centerline.front().x, -shape.centerline.front().y);
        for (std::size_t index = 1; index < shape.centerline.size(); ++index)
            centerline.lineTo(shape.centerline[index].x, -shape.centerline[index].y);
        QPainterPathStroker stroker;
        stroker.setWidth(std::max<qreal>(1.0, shape.width));
        stroker.setCapStyle(Qt::FlatCap);
        stroker.setJoinStyle(Qt::MiterJoin);
        return stroker.createStroke(centerline);
    }
    QPainterPath path;
    path.addRect(QRectF(static_cast<qreal>(shape.rectangle.x),
                        -static_cast<qreal>(shape.rectangle.y) - static_cast<qreal>(shape.rectangle.height),
                        static_cast<qreal>(shape.rectangle.width), static_cast<qreal>(shape.rectangle.height)));
    return path;
}

QRectF layoutPlannedDeviceSceneRect(const ice::PlannedDevice &device) {
    return QRectF(static_cast<qreal>(device.x), -static_cast<qreal>(device.y) - static_cast<qreal>(device.height),
                  static_cast<qreal>(device.width), static_cast<qreal>(device.height));
}

QPointF layoutPlannedEdgeScenePoint(const QRectF &rect, const QPointF &toward) {
    const auto center = rect.center();
    const auto delta = toward - center;
    if (delta.isNull())
        return center;
    const auto scaleX = delta.x() != 0.0 ? rect.width() * 0.5 / std::abs(delta.x()) : std::numeric_limits<qreal>::max();
    const auto scaleY =
        delta.y() != 0.0 ? rect.height() * 0.5 / std::abs(delta.y()) : std::numeric_limits<qreal>::max();
    return center + delta * std::min(scaleX, scaleY);
}

QRectF layoutRectToScene(const ice::Rect &rect) {
    return QRectF(static_cast<qreal>(rect.x), -static_cast<qreal>(rect.y) - static_cast<qreal>(rect.height),
                  static_cast<qreal>(rect.width), static_cast<qreal>(rect.height));
}

std::optional<ice::Rect> layoutInstanceBounds(const ice::Project &project, const ice::LayoutInstance &instance) {
    const auto child = project.layoutCells.find(instance.cell);
    if (child == project.layoutCells.end())
        return std::nullopt;
    const ice::Transform transform{instance.x, instance.y, instance.orientation};
    bool havePoint = false;
    ice::Point minimum;
    ice::Point maximum;
    const auto add = [&](ice::Point point) {
        const auto mapped = transform.apply(point);
        if (!havePoint) {
            minimum = maximum = mapped;
            havePoint = true;
            return;
        }
        minimum.x = std::min(minimum.x, mapped.x);
        minimum.y = std::min(minimum.y, mapped.y);
        maximum.x = std::max(maximum.x, mapped.x);
        maximum.y = std::max(maximum.y, mapped.y);
    };
    for (const auto &[_, shape] : child->second.shapes) {
        if (shape.isPath()) {
            for (const auto &point : shape.centerline)
                add({point.x, point.y});
        } else if (!shape.polygon.empty()) {
            for (const auto &point : shape.polygon)
                add({point.x, point.y});
        } else {
            add({shape.rectangle.x, shape.rectangle.y});
            add({shape.rectangle.x + shape.rectangle.width, shape.rectangle.y + shape.rectangle.height});
        }
    }
    for (const auto &[_, port] : child->second.ports) {
        add({port.rectangle.x, port.rectangle.y});
        add({port.rectangle.x + port.rectangle.width, port.rectangle.y + port.rectangle.height});
    }
    if (!havePoint)
        return ice::Rect{instance.x, instance.y, 0, 0};
    return ice::Rect{static_cast<ice::Coordinate>(std::floor(minimum.x)),
                     static_cast<ice::Coordinate>(std::floor(minimum.y)),
                     static_cast<ice::Coordinate>(std::ceil(maximum.x - minimum.x)),
                     static_cast<ice::Coordinate>(std::ceil(maximum.y - minimum.y))};
}

std::optional<ice::Rect> layoutInstancePortRect(const ice::Project &project, const ice::LayoutInstance &instance,
                                                ice::ObjectId portId) {
    const auto child = project.layoutCells.find(instance.cell);
    if (child == project.layoutCells.end() || portId == 0)
        return std::nullopt;
    const auto port = child->second.ports.find(portId);
    if (port == child->second.ports.end())
        return std::nullopt;
    const ice::Transform transform{instance.x, instance.y, instance.orientation};
    const auto first = transform.apply({port->second.rectangle.x, port->second.rectangle.y});
    const auto second = transform.apply({port->second.rectangle.x + port->second.rectangle.width,
                                         port->second.rectangle.y + port->second.rectangle.height});
    return ice::Rect{static_cast<ice::Coordinate>(std::floor(std::min(first.x, second.x))),
                     static_cast<ice::Coordinate>(std::floor(std::min(first.y, second.y))),
                     static_cast<ice::Coordinate>(std::ceil(std::abs(second.x - first.x))),
                     static_cast<ice::Coordinate>(std::ceil(std::abs(second.y - first.y)))};
}

std::optional<QRectF> layoutConnectionEndpointSceneRect(const ice::Project &project, const ice::LayoutCell &cell,
                                                        ice::ObjectId endpoint, ice::ObjectId port) {
    if (const auto device = cell.plannedDevices.find(endpoint); device != cell.plannedDevices.end())
        return layoutPlannedDeviceSceneRect(device->second);
    if (const auto pin = cell.ports.find(endpoint); pin != cell.ports.end())
        return layoutRectToScene(pin->second.rectangle);
    const auto instance = cell.instances.find(endpoint);
    if (instance == cell.instances.end())
        return std::nullopt;
    if (const auto rect = layoutInstancePortRect(project, instance->second, port))
        return layoutRectToScene(*rect);
    if (const auto rect = layoutInstanceBounds(project, instance->second))
        return layoutRectToScene(*rect);
    return std::nullopt;
}

char plannedDeviceSpiceElement(const ice::Project &project, const ice::PlannedDevice &device) {
    if (device.schematicInstance != 0) {
        for (const auto &[_, schematic] : project.schematicCells) {
            const auto instance = schematic.instances.find(device.schematicInstance);
            if (instance == schematic.instances.end())
                continue;
            const auto symbol = schematic.symbols.find(instance->second.symbol);
            if (symbol != schematic.symbols.end()) {
                auto symbolName = symbol->second.name;
                std::transform(symbolName.begin(), symbolName.end(), symbolName.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                if (symbolName == "gnd" || symbolName == "gnd!" || symbolName == "0")
                    return 'G';
                if (symbolName == "input" || symbolName == "output")
                    return 'P';
                if (symbol->second.sourceCell != 0)
                    return 'X';
                return ice::spiceElementForSymbol(symbol->second.name);
            }
            break;
        }
    }
    auto name = device.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (name == "gnd" || name == "gnd!" || name == "0")
        return 'G';
    if (name == "input" || name == "output")
        return 'P';
    return ice::spiceElementForSymbol(device.name);
}

std::optional<ice::PortDirection> plannedDevicePortDirection(const ice::Project &project,
                                                             const ice::PlannedDevice &device) {
    if (device.schematicInstance != 0) {
        for (const auto &[_, schematic] : project.schematicCells) {
            const auto instance = schematic.instances.find(device.schematicInstance);
            if (instance == schematic.instances.end())
                continue;
            const auto symbol = schematic.symbols.find(instance->second.symbol);
            if (symbol != schematic.symbols.end()) {
                auto symbolName = symbol->second.name;
                std::transform(symbolName.begin(), symbolName.end(), symbolName.begin(),
                               [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                if (symbolName == "input")
                    return ice::PortDirection::input;
                if (symbolName == "output")
                    return ice::PortDirection::output;
            }
            break;
        }
    }
    auto name = device.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (name == "input")
        return ice::PortDirection::input;
    if (name == "output")
        return ice::PortDirection::output;
    return std::nullopt;
}

ice::ObjectId layoutPortForSchematicPin(const ice::LayoutCell &child, const std::string &pin) {
    const auto normalized = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value;
    };
    const auto wanted = normalized(pin);
    if (wanted.empty())
        return 0;
    for (const auto &[id, port] : child.ports) {
        if (normalized(port.name) == wanted || normalized(port.label) == wanted)
            return id;
    }

    std::set<std::string> aliases;
    if (wanted == "1" || wanted == "a" || wanted == "plus" || wanted == "p")
        aliases = {"plus", "p", "a", "1", "left", "l"};
    else if (wanted == "2" || wanted == "b" || wanted == "minus" || wanted == "n" || wanted == "k")
        aliases = {"minus", "n", "b", "2", "k", "right", "r"};
    else if (wanted == "g" || wanted == "gate")
        aliases = {"g", "gate"};
    else if (wanted == "d" || wanted == "drain")
        aliases = {"d", "drain"};
    else if (wanted == "s" || wanted == "source")
        aliases = {"s", "source"};
    else if (wanted == "b" || wanted == "bulk" || wanted == "body")
        aliases = {"b", "bulk", "body"};
    for (const auto &[id, port] : child.ports) {
        if (aliases.contains(normalized(port.name)) || aliases.contains(normalized(port.label)))
            return id;
    }
    return 0;
}

struct LayoutConnectivity {
    std::set<ice::ObjectId> conductors;
    std::map<ice::ObjectId, std::set<ice::ObjectId>> viaConductors;
    std::set<std::pair<ice::ObjectId, ice::ObjectId>> directPairs;
};

struct LayoutNetTrace {
    std::set<ice::ObjectId> shapes;
    std::set<ice::ObjectId> connections;
    std::set<ice::ObjectId> devices;
    std::set<ice::ObjectId> layers;
    std::string label;
    bool layerInStack = true;
    std::size_t viaShapes = 0;
};

std::string layerKey(std::string name) {
    std::string result;
    result.reserve(name.size());
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isalnum(value))
            result.push_back(static_cast<char>(std::tolower(value)));
    }
    return result;
}

ice::ObjectId drawingLayerForPin(const ice::Project &project, ice::ObjectId cellId, ice::ObjectId layerId) {
    const auto pin = project.layers.find(layerId);
    if (pin == project.layers.end())
        return 0;
    if (!layerIsPinLayer(pin->second))
        return layerId;

    auto baseName = pin->second.name;
    if (const auto dot = baseName.find('.'); dot != std::string::npos)
        baseName = baseName.substr(0, dot);
    const auto baseKey = layerKey(baseName);
    for (const auto &[id, layer] : project.layers) {
        if (id == layerId || layer.number != pin->second.number || layerIsPinLayer(layer))
            continue;
        if (!layerAvailableForLayout(project, cellId, layer))
            continue;
        if (layerKey(layer.name) == baseKey || layer.name == baseName)
            return id;
    }
    for (const auto &[id, layer] : project.layers) {
        if (id == layerId || layer.number != pin->second.number || layerIsPinLayer(layer))
            continue;
        if (layerAvailableForLayout(project, cellId, layer))
            return id;
    }
    return 0;
}

struct LayoutRouteEndpoint {
    ice::Rect rect;
    ice::ObjectId layer = 0;
};

std::optional<LayoutRouteEndpoint> layoutRouteEndpoint(const ice::Project &project, const ice::LayoutCell &cell,
                                                       ice::ObjectId endpointId, ice::ObjectId portId) {
    // Top-level layout pins, including converted source/ground pins.
    if (const auto pin = cell.ports.find(endpointId); pin != cell.ports.end()) {
        if (!pin->second.rectangle.valid() || pin->second.layer == 0)
            return std::nullopt;
        return LayoutRouteEndpoint{pin->second.rectangle, pin->second.layer};
    }
    const auto instance = cell.instances.find(endpointId);
    if (instance == cell.instances.end() || portId == 0)
        return std::nullopt;
    const auto child = project.layoutCells.find(instance->second.cell);
    if (child == project.layoutCells.end() || !child->second.library || child->second.device.empty())
        return std::nullopt;
    const auto port = child->second.ports.find(portId);
    if (port == child->second.ports.end())
        return std::nullopt;
    const auto rect = layoutInstancePortRect(project, instance->second, portId);
    if (!rect)
        return std::nullopt;
    return LayoutRouteEndpoint{*rect, port->second.layer};
}

ice::PortSide layoutPointSide(const ice::Rect &rect, ice::LayoutPoint point) {
    const auto centerX = static_cast<double>(rect.x) + rect.width * 0.5;
    const auto centerY = static_cast<double>(rect.y) + rect.height * 0.5;
    const auto dx = point.x - centerX;
    const auto dy = point.y - centerY;
    const auto halfWidth = std::max(1.0, rect.width * 0.5);
    const auto halfHeight = std::max(1.0, rect.height * 0.5);
    if (std::abs(dx) / halfWidth >= std::abs(dy) / halfHeight)
        return dx >= 0.0 ? ice::PortSide::right : ice::PortSide::left;
    return dy >= 0.0 ? ice::PortSide::top : ice::PortSide::bottom;
}

struct PinRoutePoint {
    ice::LayoutPoint point;
    ice::LayoutPoint normal;
};

PinRoutePoint pinRoutePoint(const ice::Rect &rect, ice::LayoutPoint toward,
                            std::optional<ice::PortSide> preferredSide = std::nullopt) {
    if (rect.width <= 0 || rect.height <= 0)
        return {{rect.x + rect.width / 2, rect.y + rect.height / 2}, {0, 0}};
    const auto centerX = rect.x + rect.width / 2;
    const auto centerY = rect.y + rect.height / 2;
    if (preferredSide) {
        switch (*preferredSide) {
        case ice::PortSide::left:
            return {{rect.x, centerY}, {-1, 0}};
        case ice::PortSide::right:
            return {{rect.x + rect.width, centerY}, {1, 0}};
        case ice::PortSide::bottom:
            return {{centerX, rect.y}, {0, -1}};
        case ice::PortSide::top:
            return {{centerX, rect.y + rect.height}, {0, 1}};
        }
    }
    const auto dx = toward.x - centerX;
    const auto dy = toward.y - centerY;
    if (rect.width < rect.height) {
        return dy >= 0 ? PinRoutePoint{{centerX, rect.y + rect.height}, {0, 1}}
                       : PinRoutePoint{{centerX, rect.y}, {0, -1}};
    }
    if (rect.height < rect.width) {
        return dx >= 0 ? PinRoutePoint{{rect.x + rect.width, centerY}, {1, 0}}
                       : PinRoutePoint{{rect.x, centerY}, {-1, 0}};
    }
    const auto absDx = dx >= 0 ? dx : -dx;
    const auto absDy = dy >= 0 ? dy : -dy;
    if (absDx >= absDy)
        return dx >= 0 ? PinRoutePoint{{rect.x + rect.width, centerY}, {1, 0}}
                       : PinRoutePoint{{rect.x, centerY}, {-1, 0}};
    return dy >= 0 ? PinRoutePoint{{centerX, rect.y + rect.height}, {0, 1}} : PinRoutePoint{{centerX, rect.y}, {0, -1}};
}

bool parseIndexedLayer(const std::string &key, const std::string &prefix, int &index) {
    if (!key.starts_with(prefix))
        return false;
    const auto digits = key.substr(prefix.size());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    try {
        index = std::stoi(digits);
    } catch (const std::exception &) {
        return false;
    }
    return index >= 0;
}

LayoutConnectivity inferLayoutConnectivity(const ice::Project &project) {
    LayoutConnectivity connectivity;
    std::map<std::string, ice::ObjectId> layersByKey;
    for (const auto &[id, layer] : project.layers) {
        const auto key = layerKey(layer.name);
        if (!key.empty())
            layersByKey.emplace(key, id);
    }
    const auto layerFor = [&](const std::string &key) -> ice::ObjectId {
        const auto layer = layersByKey.find(key);
        return layer == layersByKey.end() ? 0 : layer->second;
    };
    const auto addConductor = [&](ice::ObjectId layer) {
        if (layer != 0)
            connectivity.conductors.insert(layer);
    };

    std::vector<std::pair<int, ice::ObjectId>> metals;
    std::vector<std::pair<int, ice::ObjectId>> topMetals;
    for (const auto &[key, id] : layersByKey) {
        int index = 0;
        if (parseIndexedLayer(key, "metal", index))
            metals.emplace_back(index, id);
        else if (parseIndexedLayer(key, "topmetal", index))
            topMetals.emplace_back(index, id);
    }
    std::sort(metals.begin(), metals.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    std::sort(topMetals.begin(), topMetals.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });

    std::vector<std::pair<int, bool>> stack; // layer index, is top metal
    for (const auto &entry : metals)
        stack.emplace_back(entry.first, false);
    for (const auto &entry : topMetals)
        stack.emplace_back(entry.first, true);
    if (!stack.empty()) {
        for (const auto &[_, id] : metals)
            addConductor(id);
        for (const auto &[_, id] : topMetals)
            addConductor(id);
        for (std::size_t index = 0; index + 1 < stack.size(); ++index) {
            const auto lowerLayer = stack[index].second ? layerFor("topmetal" + std::to_string(stack[index].first))
                                                        : layerFor("metal" + std::to_string(stack[index].first));
            const auto upperLayer = stack[index + 1].second
                                        ? layerFor("topmetal" + std::to_string(stack[index + 1].first))
                                        : layerFor("metal" + std::to_string(stack[index + 1].first));
            if (lowerLayer == 0 || upperLayer == 0)
                continue;
            std::string viaKey;
            if (!stack[index].second && !stack[index + 1].second)
                viaKey = "via" + std::to_string(stack[index].first);
            else if (!stack[index].second && stack[index + 1].second)
                viaKey = "topvia" + std::to_string(stack[index + 1].first);
            else
                viaKey = "topvia" + std::to_string(stack[index + 1].first);
            const auto viaLayer = layerFor(viaKey);
            if (viaLayer != 0) {
                auto &allowed = connectivity.viaConductors[viaLayer];
                allowed.insert(lowerLayer);
                allowed.insert(upperLayer);
            } else {
                connectivity.directPairs.emplace(std::min(lowerLayer, upperLayer), std::max(lowerLayer, upperLayer));
            }
        }
    }

    // A contact layer connects active/poly to the first metal layer. Include
    // those device layers so a trace can continue from metal into a device.
    const auto contactLayer = layerFor("contact");
    if (contactLayer != 0) {
        std::set<ice::ObjectId> contactConductors;
        for (const auto &key : {"activ", "gatpoly", "poly2", "metal1"}) {
            if (const auto layer = layerFor(key); layer != 0)
                contactConductors.insert(layer);
        }
        if (contactConductors.size() >= 2) {
            auto &allowed = connectivity.viaConductors[contactLayer];
            for (const auto layer : contactConductors) {
                allowed.insert(layer);
                addConductor(layer);
            }
        }
    }

    if (connectivity.conductors.empty()) {
        // Generic projects may not use technology layer names. Fall back to a
        // permissive stack so tracing still works for custom layer naming.
        std::vector<ice::ObjectId> viaLayers;
        for (const auto &[key, id] : layersByKey) {
            const bool via = key.find("via") != std::string::npos || key.find("contact") != std::string::npos;
            const bool ignored = key.find("text") != std::string::npos || key.find("label") != std::string::npos ||
                                 key.find("boundary") != std::string::npos;
            if (via) {
                viaLayers.push_back(id);
            } else if (!ignored) {
                addConductor(id);
            }
        }
        if (connectivity.conductors.empty()) {
            for (const auto &[_, layer] : project.layers)
                addConductor(layer.id);
        }
        for (const auto via : viaLayers)
            connectivity.viaConductors[via] = connectivity.conductors;
    }
    return connectivity;
}

struct AutoRouteGeometry {
    ice::ObjectId layer = 0;
    std::vector<ice::LayoutPoint> points;
    std::vector<ice::ObjectId> segmentLayers;
    std::vector<std::vector<ice::ObjectId>> pointVias;
};

struct AutoRouteObstacleSegment {
    ice::LayoutPoint a;
    ice::LayoutPoint b;
    ice::ObjectId layer = 0;
    ice::Coordinate halfWidth = 1;
};

struct AutoRouteObstacleVia {
    ice::LayoutPoint point;
    std::set<ice::ObjectId> conductorLayers;
    ice::Coordinate halfWidth = 1;
};

struct AutoRouteObstacles {
    std::vector<AutoRouteObstacleSegment> segments;
    std::vector<AutoRouteObstacleVia> vias;
};

bool routeSegmentsConflict(ice::LayoutPoint a, ice::LayoutPoint b, ice::Coordinate halfWidth,
                           const AutoRouteObstacleSegment &obstacle, ice::Coordinate margin) {
    const auto clearance = static_cast<double>(halfWidth + obstacle.halfWidth + margin);
    const auto aHorizontal = a.y == b.y;
    const auto bHorizontal = obstacle.a.y == obstacle.b.y;
    const auto minAX = std::min(a.x, b.x);
    const auto maxAX = std::max(a.x, b.x);
    const auto minAY = std::min(a.y, b.y);
    const auto maxAY = std::max(a.y, b.y);
    const auto minBX = std::min(obstacle.a.x, obstacle.b.x);
    const auto maxBX = std::max(obstacle.a.x, obstacle.b.x);
    const auto minBY = std::min(obstacle.a.y, obstacle.b.y);
    const auto maxBY = std::max(obstacle.a.y, obstacle.b.y);
    if (aHorizontal && bHorizontal) {
        return std::abs(static_cast<double>(a.y) - obstacle.a.y) <= clearance &&
               std::max(minAX, minBX) <= std::min(maxAX, maxBX) + clearance;
    }
    if (!aHorizontal && !bHorizontal) {
        return std::abs(static_cast<double>(a.x) - obstacle.a.x) <= clearance &&
               std::max(minAY, minBY) <= std::min(maxAY, maxBY) + clearance;
    }
    if (aHorizontal) {
        return obstacle.a.x >= minAX - clearance && obstacle.a.x <= maxAX + clearance && a.y >= minBY - clearance &&
               a.y <= maxBY + clearance;
    }
    return a.x >= minBX - clearance && a.x <= maxBX + clearance && obstacle.a.y >= minAY - clearance &&
           obstacle.a.y <= maxAY + clearance;
}

double routePointSegmentDistance(ice::LayoutPoint point, ice::LayoutPoint a, ice::LayoutPoint b) {
    const auto minX = static_cast<double>(std::min(a.x, b.x));
    const auto maxX = static_cast<double>(std::max(a.x, b.x));
    const auto minY = static_cast<double>(std::min(a.y, b.y));
    const auto maxY = static_cast<double>(std::max(a.y, b.y));
    const auto x = std::clamp(static_cast<double>(point.x), minX, maxX);
    const auto y = std::clamp(static_cast<double>(point.y), minY, maxY);
    return std::hypot(static_cast<double>(point.x) - x, static_cast<double>(point.y) - y);
}

AutoRouteObstacles autoRouteObstacles(const ice::LayoutCell &cell, const ice::PlannedConnection &connection,
                                      const LayoutConnectivity &connectivity) {
    AutoRouteObstacles result;
    for (const auto &[_, shape] : cell.shapes) {
        if (shape.routedConnection == 0 || shape.routedConnection == connection.id)
            continue;
        const auto other = cell.plannedConnections.find(shape.routedConnection);
        if (other != cell.plannedConnections.end() && !other->second.routed)
            continue;
        const auto otherNet = other != cell.plannedConnections.end() ? other->second.net : shape.net.value_or(0);
        if (connection.net != 0 && otherNet != 0 && connection.net == otherNet)
            continue;
        if (shape.centerline.size() < 2)
            continue;
        const auto halfWidth = std::max<ice::Coordinate>(1, shape.width) / 2;
        for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
            if (shape.centerline[index].x == shape.centerline[index + 1].x &&
                shape.centerline[index].y == shape.centerline[index + 1].y)
                continue;
            result.segments.push_back(
                {shape.centerline[index], shape.centerline[index + 1], shape.layerForSegment(index), halfWidth});
        }
        for (std::size_t index = 0; index < shape.centerline.size() && index < shape.pointVias.size(); ++index) {
            for (const auto via : shape.pointVias[index]) {
                AutoRouteObstacleVia obstacle;
                obstacle.point = shape.centerline[index];
                obstacle.halfWidth = halfWidth;
                if (const auto entry = connectivity.viaConductors.find(via); entry != connectivity.viaConductors.end())
                    obstacle.conductorLayers = entry->second;
                else
                    obstacle.conductorLayers.insert(via);
                result.vias.push_back(std::move(obstacle));
            }
        }
    }
    return result;
}

int autoRouteConflictCount(const std::vector<ice::LayoutPoint> &points, const std::vector<ice::ObjectId> &segmentLayers,
                           const std::vector<std::vector<ice::ObjectId>> &pointVias, ice::Coordinate width,
                           const AutoRouteObstacles &obstacles, const LayoutConnectivity &connectivity,
                           ice::Coordinate margin) {
    if (points.size() < 2 || segmentLayers.size() + 1 != points.size())
        return std::numeric_limits<int>::max() / 2;
    const auto halfWidth = std::max<ice::Coordinate>(1, width) / 2;
    int conflicts = 0;
    for (std::size_t index = 0; index + 1 < points.size(); ++index) {
        for (const auto &obstacle : obstacles.segments) {
            if (obstacle.layer != segmentLayers[index])
                continue;
            if (routeSegmentsConflict(points[index], points[index + 1], halfWidth, obstacle, margin))
                ++conflicts;
        }
        for (const auto &via : obstacles.vias) {
            if (!via.conductorLayers.contains(segmentLayers[index]))
                continue;
            if (routePointSegmentDistance(via.point, points[index], points[index + 1]) <=
                halfWidth + via.halfWidth + margin)
                ++conflicts;
        }
    }
    for (std::size_t index = 0; index < points.size() && index < pointVias.size(); ++index) {
        for (const auto viaLayer : pointVias[index]) {
            const auto via = connectivity.viaConductors.find(viaLayer);
            if (via == connectivity.viaConductors.end())
                continue;
            for (const auto &obstacle : obstacles.segments) {
                if (!via->second.contains(obstacle.layer))
                    continue;
                if (routePointSegmentDistance(points[index], obstacle.a, obstacle.b) <=
                    halfWidth + obstacle.halfWidth + margin)
                    ++conflicts;
            }
        }
    }
    return conflicts;
}

std::vector<ice::LayoutPoint> buildAutoRoutePoints(const PinRoutePoint &start, const PinRoutePoint &end,
                                                   const ice::LayoutPoint &startStub, const ice::LayoutPoint &endStub,
                                                   std::optional<ice::Coordinate> variant) {
    std::vector<ice::LayoutPoint> points;
    const auto append = [&](ice::LayoutPoint point) {
        if (points.empty() || points.back().x != point.x || points.back().y != point.y)
            points.push_back(point);
    };
    append(start.point);
    append(startStub);
    if (start.normal.x != 0) {
        const auto x = variant.value_or(startStub.x);
        append({x, startStub.y});
        append({x, endStub.y});
    } else if (start.normal.y != 0) {
        const auto y = variant.value_or(startStub.y);
        append({startStub.x, y});
        append({endStub.x, y});
    } else if (end.normal.x == 0) {
        const auto middleY = (startStub.y + endStub.y) / 2;
        append({startStub.x, middleY});
        append({endStub.x, middleY});
    } else {
        append({endStub.x, startStub.y});
    }
    append(endStub);
    append(end.point);
    return points;
}

std::vector<ice::Coordinate> buildAutoRouteVariants(const PinRoutePoint &start, const PinRoutePoint &end,
                                                    const ice::LayoutPoint &startStub,
                                                    const ice::LayoutPoint &endStub) {
    std::vector<ice::Coordinate> variants;
    const auto add = [&](ice::Coordinate value) {
        if (std::find(variants.begin(), variants.end(), value) == variants.end())
            variants.push_back(value);
    };
    if (start.normal.x != 0) {
        const auto mid = startStub.x + (endStub.x - startStub.x) / 2;
        add(end.normal.x != 0 ? mid : startStub.x);
        add(startStub.x);
        add(endStub.x);
        add(mid);
        add(startStub.x + (endStub.x - startStub.x) / 4);
        add(startStub.x + (endStub.x - startStub.x) * 3 / 4);
    } else if (start.normal.y != 0) {
        const auto mid = startStub.y + (endStub.y - startStub.y) / 2;
        add(end.normal.y != 0 ? mid : startStub.y);
        add(startStub.y);
        add(endStub.y);
        add(mid);
        add(startStub.y + (endStub.y - startStub.y) / 4);
        add(startStub.y + (endStub.y - startStub.y) * 3 / 4);
    }
    return variants;
}

double autoRouteLength(const std::vector<ice::LayoutPoint> &points) {
    double length = 0.0;
    for (std::size_t index = 0; index + 1 < points.size(); ++index)
        length += std::abs(static_cast<double>(points[index + 1].x - points[index].x)) +
                  std::abs(static_cast<double>(points[index + 1].y - points[index].y));
    return length;
}

std::optional<AutoRouteGeometry> computeLayoutAutoRoute(const ice::Project &project, ice::ObjectId cellId,
                                                        const ice::PlannedConnection &connection, ice::Coordinate width,
                                                        std::string *error = nullptr) {
    const auto fail = [error](const char *message) -> std::optional<AutoRouteGeometry> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const auto cellIt = project.layoutCells.find(cellId);
    if (cellIt == project.layoutCells.end() || cellIt->second.library)
        return fail("The layout cell is not editable.");

    const auto source = layoutRouteEndpoint(project, cellIt->second, connection.from, connection.fromPort);
    const auto target = layoutRouteEndpoint(project, cellIt->second, connection.to, connection.toPort);
    if (!source || !target)
        return fail("Automatic routing requires placed device pins or layout pins on both ends.");
    if (source->layer == 0 || target->layer == 0)
        return fail("One of the pins has no conductive layer.");

    const auto sourceLayer = drawingLayerForPin(project, cellId, source->layer);
    const auto targetLayer = drawingLayerForPin(project, cellId, target->layer);
    if (sourceLayer == 0 || targetLayer == 0)
        return fail("A pin layer has no drawing layer to route from.");

    const auto connectivity = inferLayoutConnectivity(project);
    std::map<ice::ObjectId, std::vector<std::pair<ice::ObjectId, ice::ObjectId>>> graph;
    for (const auto &[via, conductors] : connectivity.viaConductors) {
        for (const auto left : conductors) {
            for (const auto right : conductors) {
                if (left != right)
                    graph[left].push_back({right, via});
            }
        }
    }
    const auto bfs = [&](ice::ObjectId sourceLayer, std::map<ice::ObjectId, int> &distance,
                         std::map<ice::ObjectId, std::pair<ice::ObjectId, ice::ObjectId>> &predecessor) {
        std::queue<ice::ObjectId> queue;
        distance[sourceLayer] = 0;
        queue.push(sourceLayer);
        while (!queue.empty()) {
            const auto current = queue.front();
            queue.pop();
            const auto edges = graph.find(current);
            if (edges == graph.end())
                continue;
            for (const auto &[next, via] : edges->second) {
                if (distance.contains(next))
                    continue;
                distance[next] = distance[current] + 1;
                predecessor[next] = {current, via};
                queue.push(next);
            }
        }
    };
    std::map<ice::ObjectId, int> distanceFrom;
    std::map<ice::ObjectId, int> distanceTo;
    std::map<ice::ObjectId, std::pair<ice::ObjectId, ice::ObjectId>> predecessorFrom;
    std::map<ice::ObjectId, std::pair<ice::ObjectId, ice::ObjectId>> predecessorTo;
    bfs(sourceLayer, distanceFrom, predecessorFrom);
    bfs(targetLayer, distanceTo, predecessorTo);

    const auto viaStack = [&](ice::ObjectId from, ice::ObjectId to,
                              const std::map<ice::ObjectId, std::pair<ice::ObjectId, ice::ObjectId>> &predecessor) {
        std::vector<ice::ObjectId> vias;
        auto current = to;
        while (current != from) {
            const auto previous = predecessor.find(current);
            if (previous == predecessor.end())
                return std::vector<ice::ObjectId>{};
            vias.push_back(previous->second.second);
            current = previous->second.first;
        }
        std::reverse(vias.begin(), vias.end());
        return vias;
    };
    const auto layerAvailable = [&](ice::ObjectId layer) {
        const auto layerIt = project.layers.find(layer);
        return layerIt != project.layers.end() && layerAvailableForLayout(project, cellId, layerIt->second);
    };

    struct LayerOption {
        ice::ObjectId layer = 0;
        int viaCount = 0;
        std::vector<ice::ObjectId> sourceVias;
        std::vector<ice::ObjectId> targetVias;
    };
    std::vector<LayerOption> layerOptions;
    for (const auto &[layer, distance] : distanceFrom) {
        const auto other = distanceTo.find(layer);
        if (other == distanceTo.end())
            continue;
        const auto layerIt = project.layers.find(layer);
        if (layerIt == project.layers.end() || !layerAvailableForLayout(project, cellId, layerIt->second) ||
            layerIsPinLayer(layerIt->second))
            continue;
        if (layerKey(layerIt->second.name).find("metal") == std::string::npos)
            continue;
        const auto sourceVias = viaStack(sourceLayer, layer, predecessorFrom);
        const auto targetVias = viaStack(targetLayer, layer, predecessorTo);
        if ((sourceLayer != layer && sourceVias.empty()) || (targetLayer != layer && targetVias.empty()))
            continue;
        bool available = layerAvailable(layer);
        for (const auto via : sourceVias)
            available = available && layerAvailable(via);
        for (const auto via : targetVias)
            available = available && layerAvailable(via);
        if (!available)
            continue;
        layerOptions.push_back({layer, distance + other->second, sourceVias, targetVias});
    }
    if (layerOptions.empty())
        return fail("No metal layer is reachable from both pins.");
    std::sort(layerOptions.begin(), layerOptions.end(), [](const auto &left, const auto &right) {
        if (left.viaCount != right.viaCount)
            return left.viaCount < right.viaCount;
        return left.layer < right.layer;
    });

    const ice::LayoutPoint sourceCenter{source->rect.x + source->rect.width / 2,
                                        source->rect.y + source->rect.height / 2};
    const ice::LayoutPoint targetCenter{target->rect.x + target->rect.width / 2,
                                        target->rect.y + target->rect.height / 2};
    const auto start = pinRoutePoint(source->rect, targetCenter, connection.fromSide);
    const auto end = pinRoutePoint(target->rect, sourceCenter, connection.toSide);
    if (start.point.x == end.point.x && start.point.y == end.point.y)
        return fail("The two pins already touch; no route is needed.");

    // Leave and enter the pins from the outside of their narrow sides so the
    // route does not run along the pin rectangle. Keep the stubs inside the
    // gap so they never overshoot into the opposite pin.
    const auto clearance = std::max<ice::Coordinate>(1, project.snap(400));
    const auto stub = [&](const PinRoutePoint &pin, ice::LayoutPoint other) {
        const auto delta = (other.x - pin.point.x) * pin.normal.x + (other.y - pin.point.y) * pin.normal.y;
        const auto length = delta > 0 ? std::min(clearance, std::max<ice::Coordinate>(1, delta / 2)) : clearance;
        return ice::LayoutPoint{pin.point.x + pin.normal.x * length, pin.point.y + pin.normal.y * length};
    };
    const auto startStub = stub(start, end.point);
    const auto endStub = stub(end, start.point);

    const auto makeGeometry = [](const LayerOption &option, const std::vector<ice::LayoutPoint> &points) {
        AutoRouteGeometry geometry;
        geometry.layer = option.layer;
        geometry.points = points;
        geometry.segmentLayers.assign(points.size() > 0 ? points.size() - 1 : 0, option.layer);
        geometry.pointVias.resize(points.size());
        if (!points.empty()) {
            geometry.pointVias.front() = option.sourceVias;
            geometry.pointVias.back() = option.targetVias;
        }
        return geometry;
    };

    const auto margin = std::max<ice::Coordinate>(1, project.grid);
    const auto obstacles = autoRouteObstacles(cellIt->second, connection, connectivity);
    const auto variants = buildAutoRouteVariants(start, end, startStub, endStub);

    AutoRouteGeometry baseline;
    bool haveBaseline = false;
    std::optional<AutoRouteGeometry> bestCandidate;
    int bestViaCount = std::numeric_limits<int>::max();
    double bestLength = std::numeric_limits<double>::max();
    std::optional<AutoRouteGeometry> leastConflictCandidate;
    int leastConflictCount = std::numeric_limits<int>::max();
    int leastConflictViaCount = std::numeric_limits<int>::max();
    double leastConflictLength = std::numeric_limits<double>::max();
    for (const auto &option : layerOptions) {
        const auto consider = [&](const std::vector<ice::LayoutPoint> &points, bool makeBaseline) {
            auto geometry = makeGeometry(option, points);
            if (makeBaseline && !haveBaseline) {
                baseline = geometry;
                haveBaseline = true;
            }
            const auto conflicts = autoRouteConflictCount(geometry.points, geometry.segmentLayers, geometry.pointVias,
                                                          width, obstacles, connectivity, margin);
            const auto length = autoRouteLength(points);
            if (conflicts < leastConflictCount ||
                (conflicts == leastConflictCount &&
                 (option.viaCount < leastConflictViaCount ||
                  (option.viaCount == leastConflictViaCount && length < leastConflictLength)))) {
                leastConflictCandidate = geometry;
                leastConflictCount = conflicts;
                leastConflictViaCount = option.viaCount;
                leastConflictLength = length;
            }
            if (conflicts != 0)
                return;
            if (!bestCandidate || option.viaCount < bestViaCount ||
                (option.viaCount == bestViaCount && length < bestLength)) {
                bestCandidate = std::move(geometry);
                bestViaCount = option.viaCount;
                bestLength = length;
            }
        };
        if (variants.empty()) {
            consider(buildAutoRoutePoints(start, end, startStub, endStub, std::nullopt), true);
            continue;
        }
        for (std::size_t index = 0; index < variants.size(); ++index)
            consider(buildAutoRoutePoints(start, end, startStub, endStub, variants[index]), index == 0);
    }
    if (!haveBaseline)
        return fail("No route could be generated.");
    if (bestCandidate)
        return bestCandidate;

    // No conflict-free route was found. Try short layer bridges on interior
    // segments before falling back to the original overlapping route.
    const auto &bridgeBase = leastConflictCandidate ? *leastConflictCandidate : baseline;
    const auto viaStackBetween = [&](ice::ObjectId from,
                                     ice::ObjectId to) -> std::optional<std::vector<ice::ObjectId>> {
        std::map<ice::ObjectId, int> distance;
        std::map<ice::ObjectId, std::pair<ice::ObjectId, ice::ObjectId>> predecessor;
        bfs(from, distance, predecessor);
        if (!distance.contains(to))
            return std::nullopt;
        return viaStack(from, to, predecessor);
    };
    const auto makeBridgeGeometry =
        [&](const std::vector<ice::ObjectId> &segmentLayers) -> std::optional<AutoRouteGeometry> {
        if (segmentLayers.size() + 1 != bridgeBase.points.size() || segmentLayers.empty())
            return std::nullopt;
        if (segmentLayers.front() != bridgeBase.layer || segmentLayers.back() != bridgeBase.layer)
            return std::nullopt;
        AutoRouteGeometry geometry;
        geometry.layer = bridgeBase.layer;
        geometry.points = bridgeBase.points;
        geometry.segmentLayers = segmentLayers;
        geometry.pointVias.resize(bridgeBase.points.size());
        geometry.pointVias.front() = bridgeBase.pointVias.front();
        geometry.pointVias.back() = bridgeBase.pointVias.back();
        for (std::size_t index = 1; index + 1 < bridgeBase.points.size(); ++index) {
            if (segmentLayers[index - 1] == segmentLayers[index])
                continue;
            const auto stack = viaStackBetween(segmentLayers[index - 1], segmentLayers[index]);
            if (!stack)
                return std::nullopt;
            geometry.pointVias[index] = *stack;
        }
        for (const auto &vias : geometry.pointVias)
            for (const auto via : vias)
                if (!layerAvailable(via))
                    return std::nullopt;
        return geometry;
    };

    std::vector<ice::ObjectId> bridgeLayers{bridgeBase.layer};
    for (const auto &[neighbor, via] : graph[bridgeBase.layer]) {
        const auto layerIt = project.layers.find(neighbor);
        if (layerIt == project.layers.end() || !layerAvailableForLayout(project, cellId, layerIt->second) ||
            layerIsPinLayer(layerIt->second))
            continue;
        if (layerKey(layerIt->second.name).find("metal") == std::string::npos)
            continue;
        if (std::find(bridgeLayers.begin(), bridgeLayers.end(), neighbor) == bridgeLayers.end())
            bridgeLayers.push_back(neighbor);
    }
    std::sort(bridgeLayers.begin(), bridgeLayers.end(), [&](ice::ObjectId left, ice::ObjectId right) {
        if (left == bridgeBase.layer)
            return false;
        if (right == bridgeBase.layer)
            return true;
        return left < right;
    });
    if (bridgeBase.points.size() >= 3 && bridgeLayers.size() > 1) {
        std::vector<ice::ObjectId> segmentLayers(bridgeBase.points.size() - 1, bridgeBase.layer);
        auto currentConflicts = autoRouteConflictCount(bridgeBase.points, segmentLayers, bridgeBase.pointVias, width,
                                                       obstacles, connectivity, margin);
        std::optional<AutoRouteGeometry> bestBridge;
        int bestBridgeConflicts = currentConflicts;
        for (int pass = 0; pass < 3 && bestBridgeConflicts > 0; ++pass) {
            bool changed = false;
            for (std::size_t index = 1; index + 1 < segmentLayers.size(); ++index) {
                const auto original = segmentLayers[index];
                auto segmentBest = original;
                auto segmentBestConflicts = bestBridgeConflicts;
                std::optional<AutoRouteGeometry> segmentBestGeometry;
                for (const auto alt : bridgeLayers) {
                    segmentLayers[index] = alt;
                    auto geometry = makeBridgeGeometry(segmentLayers);
                    if (!geometry)
                        continue;
                    const auto conflicts =
                        autoRouteConflictCount(geometry->points, geometry->segmentLayers, geometry->pointVias, width,
                                               obstacles, connectivity, margin);
                    if (conflicts < segmentBestConflicts) {
                        segmentBestConflicts = conflicts;
                        segmentBest = alt;
                        segmentBestGeometry = std::move(geometry);
                    }
                }
                segmentLayers[index] = segmentBest;
                if (segmentBest != original) {
                    bestBridgeConflicts = segmentBestConflicts;
                    bestBridge = std::move(segmentBestGeometry);
                    changed = true;
                }
            }
            if (!changed)
                break;
            if (bestBridgeConflicts == 0)
                break;
        }
        if (bestBridge && bestBridgeConflicts == 0)
            return bestBridge;
        if (auto geometry = makeBridgeGeometry(segmentLayers)) {
            if (autoRouteConflictCount(geometry->points, geometry->segmentLayers, geometry->pointVias, width, obstacles,
                                       connectivity, margin) == 0)
                return geometry;
        }
    }

    return baseline;
}

struct SchematicPinView {
    QPointF point;
    ice::PinDirection direction = ice::PinDirection::left;
    ice::ObjectId instance = 0;
};

struct SchematicBranchRoute {
    ice::ObjectId fromPin = 0;
    ice::ObjectId toPin = 0;
    ice::PinDirection fromDirection = ice::PinDirection::left;
    ice::PinDirection toDirection = ice::PinDirection::left;
    std::vector<QPointF> points;
};

struct SchematicNetRoute {
    std::vector<SchematicBranchRoute> branches;
};

class DocumentWindow : public QMainWindow {
  public:
    explicit DocumentWindow(ice::AppearanceMode appearanceMode = ice::AppearanceMode::System)
        : appearanceMode_(appearanceMode) {
        ice::setPdkPackageRoots({configuredPackageInstallDirectory()});
        setWindowTitle(QStringLiteral("ICE"));
        if (const auto logo = ice::appLogo(); !logo.isNull())
            setWindowIcon(QIcon(logo));
        setDockNestingEnabled(true);
        createMenus();
        createActivityDock();
        createDocuments();
        createStatusBar();
        createProjectWatcher();
        toolEscapeShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        toolEscapeShortcut_->setContext(Qt::WindowShortcut);
        toolEscapeShortcut_->setEnabled(false);
        connect(toolEscapeShortcut_, &QShortcut::activated, this, [this] { selectSchematicTool(); });
        showStartup();
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (appearanceMode_ != ice::AppearanceMode::System)
                return;
            ice::applyAppearance(appearanceMode_);
            refreshAppearance();
        });
    }

    static bool directoryHasSymbols(const std::filesystem::path &directory) {
        std::error_code status;
        if (!std::filesystem::is_directory(directory, status) || status)
            return false;
        for (const auto &entry : std::filesystem::directory_iterator(directory, status)) {
            if (status)
                return false;
            if (entry.is_regular_file() && entry.path().extension() == ".toml")
                return true;
        }
        return false;
    }

    std::vector<SymbolDefinition> symbolLibraryForPdk(const std::string &pdk) const {
        auto symbols = genericSymbolLibrary();
        if (pdk != ice::genericPdkId) {
            const auto packageRoot = installedPdkPackageRoot(pdk);
            const auto symbolDirectory =
                packageRoot.empty() ? configuredPackageInstallDirectory() / pdk / "symbols" : packageRoot / "symbols";
            if (directoryHasSymbols(symbolDirectory)) {
                const auto pdkSymbols = packageSymbolLibrary(symbolDirectory);
                symbols.insert(symbols.end(), pdkSymbols.begin(), pdkSymbols.end());
            }
        }
        return symbols;
    }

    // Older projects can contain IHP instances placed before the package
    // sidecars declared model/size defaults. Seed only completely empty
    // parameter lists so existing user-edited values are never overwritten.
    void backfillSchematicInstanceParameters(ice::Project &project) const {
        std::map<std::string, std::vector<SymbolDefinition>> packageLibraries;
        for (auto &[_, cell] : project.schematicCells) {
            if (cell.isTextSource())
                continue;
            const auto pdk = cell.pdk.empty() ? std::string(ice::genericPdkId) : cell.pdk;
            if (pdk == ice::genericPdkId)
                continue;
            auto library = packageLibraries.find(pdk);
            if (library == packageLibraries.end()) {
                const auto packageRoot = installedPdkPackageRoot(pdk);
                const auto symbolDirectory = packageRoot.empty() ? configuredPackageInstallDirectory() / pdk / "symbols"
                                                                 : packageRoot / "symbols";
                auto definitions = directoryHasSymbols(symbolDirectory) ? packageSymbolLibrary(symbolDirectory)
                                                                        : std::vector<SymbolDefinition>{};
                library = packageLibraries.emplace(pdk, std::move(definitions)).first;
            }
            if (library->second.empty())
                continue;
            std::map<std::string, const SymbolDefinition *> defaults;
            for (const auto &definition : library->second)
                defaults.emplace(definition.name.toStdString(), &definition);
            for (auto &[_, instance] : cell.instances) {
                if (!instance.parameters.empty())
                    continue;
                const auto symbol = cell.symbols.find(instance.symbol);
                if (symbol == cell.symbols.end())
                    continue;
                const auto definition = defaults.find(symbol->second.name);
                if (definition == defaults.end())
                    continue;
                ice::seedDefaultParameters(project, instance, instanceDefaults(*definition->second));
            }
        }
    }

    bool setProject(ice::Project project) {
        if (reportDirty_ && !saveReportSource()) {
            QMessageBox::warning(this, QStringLiteral("Simulation Script"),
                                 QStringLiteral("Unable to save the current simulation script changes."));
            return false;
        }
        if (!ensureProjectPackages(project))
            return false;
        suppressProjectWatcher_ = false;
        projectDiskChangePending_ = false;
        clearProjectWatcher();
        lastSchematicPlacementDevice_.reset();
        flattenedLayouts_->clear();
        ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
        if (layoutDevicesPanel_) {
            layoutDevicesPanel_->resetDevices();
            const auto &library = layoutDevicesPanel_->library();
            for (auto &[_, cell] : project.layoutCells) {
                if (!cell.library || !layoutPdkIdsMatch(cell.pdk, library.pdk))
                    continue;
                if (cell.device.empty()) {
                    if (const auto *device = findLayoutDeviceForCellName(library, cell.name))
                        cell.device = device->name;
                }
            }

            struct InstanceReference {
                ice::ObjectId parent = 0;
                ice::ObjectId instance = 0;
            };
            std::map<ice::ObjectId, std::vector<InstanceReference>> references;
            for (const auto &[parentId, parent] : project.layoutCells) {
                for (const auto &[instanceId, instance] : parent.instances) {
                    const auto childIt = project.layoutCells.find(instance.cell);
                    if (childIt == project.layoutCells.end() || !childIt->second.library ||
                        childIt->second.device.empty())
                        continue;
                    references[instance.cell].push_back({parentId, instanceId});
                }
            }
            for (const auto &[childId, childReferences] : references) {
                if (childReferences.size() <= 1)
                    continue;
                const auto source = project.layoutCells.at(childId);
                for (std::size_t index = 1; index < childReferences.size(); ++index) {
                    const auto prefix =
                        layoutPdkIdsMatch(source.pdk, library.pdk) ? layoutDeviceCellPrefix(library) : std::string{};
                    const auto cloneId =
                        cloneLibraryDeviceCell(project, source, uniqueDeviceCellName(project, source.device, prefix));
                    project.layoutCells.at(childReferences[index].parent)
                        .instances.at(childReferences[index].instance)
                        .cell = cloneId;
                }
            }

            for (auto &[_, cell] : project.layoutCells) {
                if (!cell.library)
                    continue;
                const auto *device = findLayoutDeviceForCell(library, cell);
                if (!device)
                    continue;
                if (cell.device.empty())
                    cell.device = device->name;
                syncLayoutDeviceParameters(project, cell, *device);
                if (cell.ports.empty() && !device->ports.empty())
                    applyLayoutDevicePorts(project, cell, library, device->ports);
            }
        }
        backfillSchematicInstanceParameters(project);
        project_ = std::make_unique<ice::ProjectSession>(std::move(project));
        project_->subscribe([this] { updateFileTabDirtyIndicators(); });
        captureSavedFileBaselines();
        conflictedSchematicFiles_.clear();
        conflictedLayoutFiles_.clear();
        conflictedProceduralFiles_.clear();
        externalSchematicFiles_.clear();
        externalLayoutFiles_.clear();
        externalProceduralFiles_.clear();
        resetSectionHistories();
        layoutViewInitialized_ = false;
        activeSchematicId_ = 0;
        activeLayoutId_ = 0;
        activeProceduralLayoutId_ = 0;
        activeLayoutPathId_ = 0;
        activeLayoutPathCellId_ = 0;
        activeLayoutPathWidth_ = 0;
        activeSimulationId_ = 0;
        activeReportId_ = 0;
        selectingReport_ = false;
        loadedCachedRunPath_.clear();
        if (schematicViewStack_ && schematicCanvasPage_)
            schematicViewStack_->setCurrentWidget(schematicCanvasPage_);
        if (layoutViewStack_ && layoutCanvasPage_)
            layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
        openSchematicTabs_.clear();
        openLayoutTabs_.clear();
        knownSchematicCells_.clear();
        knownLayoutCells_.clear();
        knownProceduralLayouts_.clear();
        // A freshly opened project starts with no document tabs. Seed the
        // known sets so the first refresh does not treat every existing file
        // as newly-created and open it automatically.
        if (project_) {
            for (const auto &[id, _] : project_->project().schematicCells)
                knownSchematicCells_.insert(id);
            for (const auto &[id, _] : project_->project().layoutCells)
                knownLayoutCells_.insert(id);
            for (const auto &[id, _] : project_->project().proceduralLayouts)
                knownProceduralLayouts_.insert(id);
        }
        clearLayoutDeviceSelection();
        if (layoutSelectTool_)
            layoutSelectTool_->setChecked(true);
        if (layoutCanvas_)
            layoutCanvas_->setTool(LayoutCanvas::Tool::select);
        projectPath_.clear();
        schematicSymbolsLoaded_ = false;
        schematicSymbolsPdk_.clear();
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->clear();
        if (simulationNetlist_)
            simulationNetlist_->clear();
        builtSimulationNetlistSchematicId_ = 0;
        builtSimulationNetlist_.clear();
        refreshSimulationSchematics();
        refreshEmbeddedPanels();
        refreshSchematic();
        refreshLayout();
        setWindowTitle(QStringLiteral("ICE - %1").arg(QString::fromStdString(project_->project().name)));
        statusBar_->setProjectName(QString::fromStdString(project_->project().name));
        statusBar()->showMessage(QStringLiteral("Project loaded"));
        showWorkspace();
        return true;
    }

    void setProjectPath(const QString &path) {
        if (path.isEmpty()) {
            projectPath_.clear();
            clearProjectWatcher();
            return;
        }
        const QFileInfo info(path);
        projectPath_ = info.isDir() ? QDir(path).filePath(QStringLiteral("project.icepro")) : path;
        if (activityPanel_)
            activityPanel_->setProjectPath(QFileInfo(projectPath_).absolutePath());
        configureProjectWatcher();
    }

  private:
    bool ensureProjectPackages(const ice::Project &project) {
        if (project.packageDependencies.empty())
            return true;
        const auto installDirectory = configuredPackageInstallDirectory();
        auto installed = installedPackages(installDirectory);
        const auto isInstalled = [&](const std::string &name) {
            return std::any_of(installed.begin(), installed.end(),
                               [&](const auto &entry) { return ice::packageMatchesPdk(entry.spec, name); });
        };
        std::vector<std::string> missing;
        for (const auto &dependency : project.packageDependencies)
            if (!isInstalled(dependency))
                missing.push_back(dependency);
        if (missing.empty())
            return true;
        if (!installMissingPackages(this, missing, configuredPackageIndexDirectory(), installDirectory))
            return false;
        installed = installedPackages(installDirectory);
        for (const auto &dependency : project.packageDependencies) {
            if (!isInstalled(dependency)) {
                QMessageBox::critical(
                    this, QStringLiteral("Missing Packages"),
                    QStringLiteral("The project cannot be opened because package %1 is still not installed.")
                        .arg(QString::fromStdString(dependency)));
                return false;
            }
        }
        return true;
    }

    ice::Project makeNewProject(const QString &name) const {
        ice::Project project;
        project.name = name.toStdString();
        project.identity = project.name;
        project.units = {};
        project.grid = 1;
        return project;
    }

    QStringList loadRecentProjects() const {
        QSettings settings;
        auto projects = settings.value(QStringLiteral("recentProjects")).toStringList();
        projects.removeAll(QString());
        return projects;
    }

    void storeRecentProjects(const QStringList &projects) {
        QSettings settings;
        settings.setValue(QStringLiteral("recentProjects"), projects);
    }

    void addRecentProject(const QString &path) {
        if (path.isEmpty())
            return;
        const auto normalized = QFileInfo(path).absoluteFilePath();
        auto projects = loadRecentProjects();
        projects.removeAll(normalized);
        projects.prepend(normalized);
        while (projects.size() > 10)
            projects.removeLast();
        storeRecentProjects(projects);
        if (startupView_)
            startupView_->setRecentProjects(projects);
    }

    void removeRecentProject(const QString &path) {
        auto projects = loadRecentProjects();
        projects.removeAll(QFileInfo(path).absoluteFilePath());
        storeRecentProjects(projects);
        if (startupView_)
            startupView_->setRecentProjects(projects);
    }

    void createProject(const QString &rawName, const QString &rawFolder, bool createFolder) {
        const auto name = rawName.trimmed();
        const auto folder = rawFolder.trimmed();
        if (name.isEmpty() || folder.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Create Project"),
                                     QStringLiteral("A project name and project folder are required."));
            return;
        }
        if (std::any_of(name.begin(), name.end(),
                        [](QChar character) { return QStringLiteral("<>:\"/\\|?*").contains(character); })) {
            QMessageBox::information(this, QStringLiteral("Create Project"),
                                     QStringLiteral("The project name contains characters that are not valid in a "
                                                    "file name."));
            return;
        }
        std::filesystem::path root(folder.toStdString());
        if (createFolder)
            root /= name.toStdString();
        const auto projectFile = root / "project.icepro";
        std::error_code status;
        if (std::filesystem::exists(projectFile, status) && !status) {
            QMessageBox::information(
                this, QStringLiteral("Create Project"),
                QStringLiteral("A project already exists at:\n%1").arg(QString::fromStdString(root.string())));
            return;
        }
        try {
            std::filesystem::create_directories(root);
            auto project = makeNewProject(name);
            project.save(root);
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Project"), QString::fromStdString(error.what()));
            return;
        }
        auto project = makeNewProject(name);
        if (!setProject(std::move(project)))
            return;
        projectPath_ = QString::fromStdString(projectFile.string());
        if (activityPanel_)
            activityPanel_->setProjectPath(QString::fromStdString(root.string()));
        configureProjectWatcher();
        addRecentProject(projectPath_);
        if (startupView_)
            startupView_->clearCreateForm();
        statusBar()->showMessage(QStringLiteral("New project created"));
    }

    void openProjectPath(const QString &path) {
        if (path.isEmpty())
            return;
        QFileInfo info(path);
        const auto projectPath = info.isDir() ? QDir(path).filePath(QStringLiteral("project.icepro")) : path;
        if (!QFileInfo::exists(projectPath)) {
            QMessageBox::warning(this, QStringLiteral("Open Project"),
                                 QStringLiteral("The project file no longer exists:\n%1").arg(projectPath));
            removeRecentProject(path);
            return;
        }
        try {
            auto project = ice::Project::load(projectPath.toStdString());
            if (!setProject(std::move(project)))
                return;
            projectPath_ = projectPath;
            if (activityPanel_)
                activityPanel_->setProjectPath(QFileInfo(projectPath).absolutePath());
            configureProjectWatcher();
            addRecentProject(projectPath_);
            statusBar()->showMessage(QStringLiteral("Project opened"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Open Project"), QString::fromStdString(error.what()));
        }
    }

    void newProject() {
        CreateProjectDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createProject(dialog.projectName(), dialog.projectFolder(), dialog.createFolder());
    }

    void openProject() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open ICE Project"), QString(),
                                                       QStringLiteral("ICE projects (*.icepro);;All files (*)"));
        if (!path.isEmpty())
            openProjectPath(path);
    }

    bool resolveExternalFileConflicts() {
        if (!project_)
            return true;
        const auto ask = [&](const QString &label, bool externalDeleted) -> std::optional<bool> {
            QMessageBox box(this);
            box.setIcon(QMessageBox::Warning);
            box.setWindowTitle(QStringLiteral("External Changes Detected"));
            box.setText(
                QStringLiteral("The file \"%1\" was changed externally and also has unsaved GUI edits.%2\n\n"
                               "Which version should be preserved when saving?")
                    .arg(label, externalDeleted ? QStringLiteral("\n\nThe external version was deleted.") : QString()));
            box.addButton(QStringLiteral("Keep My Version"), QMessageBox::AcceptRole);
            auto *external = box.addButton(QStringLiteral("Use External Version"), QMessageBox::DestructiveRole);
            auto *cancel = box.addButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == cancel || box.clickedButton() == nullptr)
                return std::optional<bool>{};
            return std::optional<bool>(box.clickedButton() == external);
        };

        std::map<ice::ObjectId, bool> schematicChoices;
        std::map<ice::ObjectId, bool> layoutChoices;
        std::map<ice::ObjectId, bool> proceduralChoices;
        for (const auto id : conflictedSchematicFiles_) {
            const auto current = project_->project().schematicCells.find(id);
            const auto external = externalSchematicFiles_.find(id);
            QString name = QString::number(id);
            if (current != project_->project().schematicCells.end())
                name = QString::fromStdString(current->second.name);
            else if (external != externalSchematicFiles_.end())
                name = QString::fromStdString(external->second.name);
            const auto choice = ask(name + QStringLiteral(".icesch"), external == externalSchematicFiles_.end());
            if (!choice)
                return false;
            schematicChoices[id] = *choice;
        }
        for (const auto id : conflictedLayoutFiles_) {
            const auto current = project_->project().layoutCells.find(id);
            const auto external = externalLayoutFiles_.find(id);
            QString name = QString::number(id);
            if (current != project_->project().layoutCells.end())
                name = QString::fromStdString(current->second.name);
            else if (external != externalLayoutFiles_.end())
                name = QString::fromStdString(external->second.name);
            const auto choice = ask(name + QStringLiteral(".icelay"), external == externalLayoutFiles_.end());
            if (!choice)
                return false;
            layoutChoices[id] = *choice;
        }
        for (const auto id : conflictedProceduralFiles_) {
            const auto current = project_->project().proceduralLayouts.find(id);
            const auto external = externalProceduralFiles_.find(id);
            QString name = QString::number(id);
            if (current != project_->project().proceduralLayouts.end())
                name = QString::fromStdString(current->second.name);
            else if (external != externalProceduralFiles_.end())
                name = QString::fromStdString(external->second.name);
            const auto choice = ask(name + QStringLiteral(".icelay"), external == externalProceduralFiles_.end());
            if (!choice)
                return false;
            proceduralChoices[id] = *choice;
        }

        for (const auto &[id, useExternal] : schematicChoices) {
            if (!useExternal)
                continue;
            const auto external = externalSchematicFiles_.find(id);
            if (external != externalSchematicFiles_.end()) {
                project_->project().schematicCells[id] = external->second;
                savedSchematicSignatures_[id] = schematicFileSignature(external->second);
            } else {
                project_->project().schematicCells.erase(id);
                savedSchematicSignatures_.erase(id);
            }
        }
        for (const auto &[id, useExternal] : layoutChoices) {
            if (!useExternal)
                continue;
            const auto external = externalLayoutFiles_.find(id);
            if (external != externalLayoutFiles_.end()) {
                project_->project().layoutCells[id] = external->second;
                savedLayoutSignatures_[id] = layoutFileSignature(external->second);
            } else {
                project_->project().layoutCells.erase(id);
                savedLayoutSignatures_.erase(id);
            }
        }
        for (const auto &[id, useExternal] : proceduralChoices) {
            if (!useExternal)
                continue;
            const auto external = externalProceduralFiles_.find(id);
            if (external != externalProceduralFiles_.end()) {
                project_->project().proceduralLayouts[id] = external->second;
                savedProceduralSignatures_[id] = proceduralFileSignature(external->second);
            } else {
                project_->project().proceduralLayouts.erase(id);
                savedProceduralSignatures_.erase(id);
            }
        }

        conflictedSchematicFiles_.clear();
        conflictedLayoutFiles_.clear();
        conflictedProceduralFiles_.clear();
        externalSchematicFiles_.clear();
        externalLayoutFiles_.clear();
        externalProceduralFiles_.clear();
        projectDiskChangePending_ = false;
        refreshProject();
        return true;
    }

    void saveProject() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Save Project"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const bool hadExternalConflicts = !conflictedSchematicFiles_.empty() || !conflictedLayoutFiles_.empty() ||
                                          !conflictedProceduralFiles_.empty();
        if (projectDiskChangePending_ && !hadExternalConflicts)
            reloadProjectFromDisk(false);
        const bool hadExternalConflictsAfterReload = !conflictedSchematicFiles_.empty() ||
                                                     !conflictedLayoutFiles_.empty() ||
                                                     !conflictedProceduralFiles_.empty();
        if (hadExternalConflictsAfterReload) {
            if (!resolveExternalFileConflicts())
                return;
        }
        if (projectDiskChangePending_) {
            const auto response =
                QMessageBox::warning(this, QStringLiteral("Save Project"),
                                     QStringLiteral("Project files changed on disk since the GUI loaded them.\n\n"
                                                    "Saving now will overwrite those external changes. Continue?"),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (response != QMessageBox::Yes) {
                statusBar()->showMessage(
                    QStringLiteral("Save cancelled. Use File > Reload Project from Disk to load external changes."),
                    10000);
                return;
            }
        }

        auto path = projectPath_;
        if (path.isEmpty())
            path =
                QFileDialog::getSaveFileName(this, QStringLiteral("Save ICE Project"), QStringLiteral("project.icepro"),
                                             QStringLiteral("ICE projects (*.icepro);;All files (*)"));
        if (path.isEmpty())
            return;
        suppressProjectWatcher_ = true;
        if (projectWatcherResumeTimer_)
            projectWatcherResumeTimer_->stop();
        clearProjectWatcher();
        try {
            project_->save(path.toStdString());
            captureSavedFileBaselines();
            conflictedSchematicFiles_.clear();
            conflictedLayoutFiles_.clear();
            conflictedProceduralFiles_.clear();
            externalSchematicFiles_.clear();
            externalLayoutFiles_.clear();
            externalProceduralFiles_.clear();
            updateFileTabDirtyIndicators();
            projectPath_ = QFileInfo(path).isDir() ? QDir(path).filePath(QStringLiteral("project.icepro")) : path;
            if (activityPanel_)
                activityPanel_->setProjectPath(QFileInfo(projectPath_).absolutePath());
            projectDiskChangePending_ = false;
            configureProjectWatcher();
            if (projectWatcherResumeTimer_)
                projectWatcherResumeTimer_->start();
            addRecentProject(projectPath_);
            statusBar()->showMessage(QStringLiteral("Project saved"));
        } catch (const ice::ProjectError &error) {
            suppressProjectWatcher_ = false;
            configureProjectWatcher();
            QMessageBox::critical(this, QStringLiteral("Save Project"), QString::fromStdString(error.what()));
        }
    }

    void createProjectWatcher() {
        projectWatcher_ = new QFileSystemWatcher(this);
        simulationRunWatcher_ = new QFileSystemWatcher(this);
        simulationRunWatcherTimer_ = new QTimer(this);
        simulationRunWatcherTimer_->setSingleShot(true);
        simulationRunWatcherTimer_->setInterval(150);
        connect(simulationRunWatcherTimer_, &QTimer::timeout, this, [this] {
            loadCachedSimulationRun();
            loadCachedScriptGraphs();
            configureSimulationRunWatcher();
        });
        const auto runChanged = [this](const QString &) {
            if (simulationRunWatcherTimer_)
                simulationRunWatcherTimer_->start();
        };
        connect(simulationRunWatcher_, &QFileSystemWatcher::directoryChanged, this, runChanged);
        connect(simulationRunWatcher_, &QFileSystemWatcher::fileChanged, this, runChanged);
        projectWatchDebounceTimer_ = new QTimer(this);
        projectWatchDebounceTimer_->setSingleShot(true);
        projectWatchDebounceTimer_->setInterval(300);
        connect(projectWatchDebounceTimer_, &QTimer::timeout, this, [this] { reloadProjectFromDisk(false); });

        projectWatcherResumeTimer_ = new QTimer(this);
        projectWatcherResumeTimer_->setSingleShot(true);
        projectWatcherResumeTimer_->setInterval(300);
        connect(projectWatcherResumeTimer_, &QTimer::timeout, this, [this] {
            suppressProjectWatcher_ = false;
            configureProjectWatcher();
        });

        const auto changed = [this](const QString &) { scheduleProjectReloadFromDisk(); };
        connect(projectWatcher_, &QFileSystemWatcher::directoryChanged, this, changed);
        connect(projectWatcher_, &QFileSystemWatcher::fileChanged, this, changed);
    }

    void clearProjectWatcher() {
        if (projectWatchDebounceTimer_)
            projectWatchDebounceTimer_->stop();
        if (projectWatcherResumeTimer_)
            projectWatcherResumeTimer_->stop();
        if (simulationRunWatcherTimer_)
            simulationRunWatcherTimer_->stop();
        if (simulationRunWatcher_) {
            const auto paths = simulationRunWatcher_->directories();
            if (!paths.isEmpty())
                simulationRunWatcher_->removePaths(paths);
            const auto files = simulationRunWatcher_->files();
            if (!files.isEmpty())
                simulationRunWatcher_->removePaths(files);
        }
        if (!projectWatcher_)
            return;
        const auto directories = projectWatcher_->directories();
        if (!directories.isEmpty())
            projectWatcher_->removePaths(directories);
        const auto files = projectWatcher_->files();
        if (!files.isEmpty())
            projectWatcher_->removePaths(files);
    }

    void configureProjectWatcher() {
        if (!projectWatcher_)
            return;
        clearProjectWatcher();
        if (!project_ || projectPath_.isEmpty())
            return;

        const QFileInfo projectInfo(projectPath_);
        const auto root = projectInfo.isDir() ? projectInfo.absoluteFilePath() : projectInfo.absolutePath();
        QStringList watched;
        const auto addDirectory = [&](const QString &name, const QStringList &patterns) {
            const QDir directory(QDir(root).filePath(name));
            if (!directory.exists())
                return;
            watched << directory.absolutePath();
            for (const auto &entry : directory.entryInfoList(patterns, QDir::Files))
                watched << entry.absoluteFilePath();
        };
        addDirectory(QStringLiteral("sch"), {QStringLiteral("*.icesch")});
        addDirectory(QStringLiteral("lay"), {QStringLiteral("*.icelay")});
        addDirectory(QStringLiteral("sim"), {QStringLiteral("*.icesim")});
        if (QFileInfo::exists(projectPath_))
            watched << projectPath_;
        if (!watched.isEmpty())
            projectWatcher_->addPaths(watched);
        configureSimulationRunWatcher();
    }

    void configureSimulationRunWatcher() {
        if (!simulationRunWatcher_)
            return;
        const auto paths = simulationRunWatcher_->directories();
        if (!paths.isEmpty())
            simulationRunWatcher_->removePaths(paths);
        const auto files = simulationRunWatcher_->files();
        if (!files.isEmpty())
            simulationRunWatcher_->removePaths(files);
        if (!project_ || projectPath_.isEmpty())
            return;
        const QFileInfo projectInfo(projectPath_);
        const auto root = projectInfo.isDir() ? projectInfo.absoluteFilePath() : projectInfo.absolutePath();
        QStringList watched{root};
        const QDir buildDir(QDir(root).filePath(QStringLiteral("build")));
        if (buildDir.exists())
            watched << buildDir.absolutePath();
        const QDir base(QDir(root).filePath(QStringLiteral("build/simulations")));
        if (!base.exists()) {
            if (!watched.isEmpty())
                simulationRunWatcher_->addPaths(watched);
            return;
        }
        watched << base.absolutePath();
        for (const auto &entry : base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            watched << entry.absoluteFilePath();
            const QDir runsDir(QDir(entry.absoluteFilePath()).filePath(QStringLiteral("runs")));
            if (runsDir.exists())
                watched << runsDir.absolutePath();
        }
        const QDir scriptsBase(QDir(root).filePath(QStringLiteral("build/simulation-scripts")));
        if (scriptsBase.exists()) {
            watched << scriptsBase.absolutePath();
            for (const auto &entry : scriptsBase.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
                watched << entry.absoluteFilePath();
        }
        if (!watched.isEmpty())
            simulationRunWatcher_->addPaths(watched);
    }

    void scheduleProjectReloadFromDisk() {
        if (suppressProjectWatcher_ || !project_ || projectPath_.isEmpty())
            return;
        projectDiskChangePending_ = true;
        if (projectWatchDebounceTimer_)
            projectWatchDebounceTimer_->start();
    }

    bool mergeExternalProject(const ice::Project &disk) {
        if (!project_)
            return false;
        auto merged = project_->project();
        auto savedSchematic = savedSchematicSignatures_;
        auto savedLayout = savedLayoutSignatures_;
        auto savedProcedural = savedProceduralSignatures_;
        auto externalSchematic = std::map<ice::ObjectId, ice::SchematicCell>{};
        auto externalLayout = std::map<ice::ObjectId, ice::LayoutCell>{};
        auto externalProcedural = std::map<ice::ObjectId, ice::ProceduralLayout>{};
        auto conflictedSchematic = std::set<ice::ObjectId>{};
        auto conflictedLayout = std::set<ice::ObjectId>{};
        auto conflictedProcedural = std::set<ice::ObjectId>{};
        bool adopted = false;

        auto mergeSchematic = [&](const ice::SchematicCell &diskCell) {
            const auto id = diskCell.id;
            const auto diskSignature = schematicFileSignature(diskCell);
            auto current = merged.schematicCells.find(id);
            const auto saved = savedSchematic.find(id);
            if (current == merged.schematicCells.end()) {
                if (saved == savedSchematic.end()) {
                    merged.schematicCells.emplace(id, diskCell);
                    savedSchematic[id] = diskSignature;
                    adopted = true;
                } else if (diskSignature != saved->second) {
                    externalSchematic[id] = diskCell;
                    conflictedSchematic.insert(id);
                }
                return;
            }
            const auto currentSignature = schematicFileSignature(current->second);
            if (currentSignature == diskSignature) {
                savedSchematic[id] = diskSignature;
                return;
            }
            if (saved == savedSchematic.end()) {
                // A newly-created schematic can exist only in the GUI while
                // the agent loads the last saved project from disk. Both
                // sessions then allocate the same first id. If the GUI cell
                // is still an empty placeholder (the usual "top" cell), the
                // disk cell is the completed version the user is expecting;
                // adopt it instead of hiding it as an external conflict.
                const auto &localCell = current->second;
                const bool emptyPlaceholder = localCell.parameters.empty() && localCell.symbols.empty() &&
                                              localCell.instances.empty() && localCell.nets.empty() &&
                                              (!localCell.isTextSource() || localCell.sourceText().empty());
                if (emptyPlaceholder) {
                    current->second = diskCell;
                    savedSchematic[id] = diskSignature;
                    adopted = true;
                    return;
                }
                externalSchematic[id] = diskCell;
                conflictedSchematic.insert(id);
                return;
            }
            const bool localDirty = currentSignature != saved->second;
            const bool diskChanged = diskSignature != saved->second;
            if (!diskChanged)
                return;
            if (!localDirty) {
                current->second = diskCell;
                savedSchematic[id] = diskSignature;
                adopted = true;
            } else {
                externalSchematic[id] = diskCell;
                conflictedSchematic.insert(id);
            }
        };

        auto mergeLayout = [&](const ice::LayoutCell &diskCell) {
            const auto id = diskCell.id;
            const auto diskSignature = layoutFileSignature(diskCell);
            auto current = merged.layoutCells.find(id);
            const auto saved = savedLayout.find(id);
            if (current == merged.layoutCells.end()) {
                if (saved == savedLayout.end()) {
                    merged.layoutCells.emplace(id, diskCell);
                    savedLayout[id] = diskSignature;
                    adopted = true;
                } else if (diskSignature != saved->second) {
                    externalLayout[id] = diskCell;
                    conflictedLayout.insert(id);
                }
                return;
            }
            const auto currentSignature = layoutFileSignature(current->second);
            if (currentSignature == diskSignature) {
                savedLayout[id] = diskSignature;
                return;
            }
            if (saved == savedLayout.end()) {
                externalLayout[id] = diskCell;
                conflictedLayout.insert(id);
                return;
            }
            const bool localDirty = currentSignature != saved->second;
            const bool diskChanged = diskSignature != saved->second;
            if (!diskChanged)
                return;
            if (!localDirty) {
                current->second = diskCell;
                savedLayout[id] = diskSignature;
                adopted = true;
            } else {
                externalLayout[id] = diskCell;
                conflictedLayout.insert(id);
            }
        };

        auto mergeProcedural = [&](const ice::ProceduralLayout &diskLayout) {
            const auto id = diskLayout.id;
            const auto diskSignature = proceduralFileSignature(diskLayout);
            auto current = merged.proceduralLayouts.find(id);
            const auto saved = savedProcedural.find(id);
            if (current == merged.proceduralLayouts.end()) {
                if (saved == savedProcedural.end()) {
                    merged.proceduralLayouts.emplace(id, diskLayout);
                    savedProcedural[id] = diskSignature;
                    adopted = true;
                } else if (diskSignature != saved->second) {
                    externalProcedural[id] = diskLayout;
                    conflictedProcedural.insert(id);
                }
                return;
            }
            const auto currentSignature = proceduralFileSignature(current->second);
            if (currentSignature == diskSignature) {
                savedProcedural[id] = diskSignature;
                return;
            }
            if (saved == savedProcedural.end()) {
                externalProcedural[id] = diskLayout;
                conflictedProcedural.insert(id);
                return;
            }
            const bool localDirty = currentSignature != saved->second;
            const bool diskChanged = diskSignature != saved->second;
            if (!diskChanged)
                return;
            if (!localDirty) {
                current->second = diskLayout;
                savedProcedural[id] = diskSignature;
                adopted = true;
            } else {
                externalProcedural[id] = diskLayout;
                conflictedProcedural.insert(id);
            }
        };

        std::set<ice::ObjectId> diskSchematicIds;
        for (const auto &[id, cell] : disk.schematicCells) {
            diskSchematicIds.insert(id);
            mergeSchematic(cell);
        }
        for (auto it = merged.schematicCells.begin(); it != merged.schematicCells.end();) {
            const auto id = it->first;
            if (diskSchematicIds.contains(id)) {
                ++it;
                continue;
            }
            const auto saved = savedSchematic.find(id);
            if (saved == savedSchematic.end()) {
                ++it;
                continue;
            }
            if (schematicFileSignature(it->second) != saved->second) {
                conflictedSchematic.insert(id);
                ++it;
            } else {
                savedSchematic.erase(id);
                it = merged.schematicCells.erase(it);
                adopted = true;
            }
        }

        std::set<ice::ObjectId> diskLayoutIds;
        for (const auto &[id, cell] : disk.layoutCells) {
            diskLayoutIds.insert(id);
            mergeLayout(cell);
        }
        for (auto it = merged.layoutCells.begin(); it != merged.layoutCells.end();) {
            const auto id = it->first;
            if (diskLayoutIds.contains(id)) {
                ++it;
                continue;
            }
            const auto saved = savedLayout.find(id);
            if (saved == savedLayout.end()) {
                ++it;
                continue;
            }
            if (layoutFileSignature(it->second) != saved->second) {
                conflictedLayout.insert(id);
                ++it;
            } else {
                savedLayout.erase(id);
                it = merged.layoutCells.erase(it);
                adopted = true;
            }
        }

        std::set<ice::ObjectId> diskProceduralIds;
        for (const auto &[id, layout] : disk.proceduralLayouts) {
            diskProceduralIds.insert(id);
            mergeProcedural(layout);
        }
        for (auto it = merged.proceduralLayouts.begin(); it != merged.proceduralLayouts.end();) {
            const auto id = it->first;
            if (diskProceduralIds.contains(id)) {
                ++it;
                continue;
            }
            const auto saved = savedProcedural.find(id);
            if (saved == savedProcedural.end()) {
                ++it;
                continue;
            }
            if (proceduralFileSignature(it->second) != saved->second) {
                conflictedProcedural.insert(id);
                ++it;
            } else {
                savedProcedural.erase(id);
                it = merged.proceduralLayouts.erase(it);
                adopted = true;
            }
        }

        const auto diagnostics = merged.validate();
        if (!diagnostics.empty()) {
            statusBar()->showMessage(QStringLiteral("External project changes could not be merged: %1")
                                         .arg(QString::fromStdString(diagnostics.front().message)),
                                     10000);
            return false;
        }

        project_->project() = std::move(merged);
        savedSchematicSignatures_ = std::move(savedSchematic);
        savedLayoutSignatures_ = std::move(savedLayout);
        savedProceduralSignatures_ = std::move(savedProcedural);
        externalSchematicFiles_ = std::move(externalSchematic);
        externalLayoutFiles_ = std::move(externalLayout);
        externalProceduralFiles_ = std::move(externalProcedural);
        conflictedSchematicFiles_ = std::move(conflictedSchematic);
        conflictedLayoutFiles_ = std::move(conflictedLayout);
        conflictedProceduralFiles_ = std::move(conflictedProcedural);
        projectDiskChangePending_ = !conflictedSchematicFiles_.empty() || !conflictedLayoutFiles_.empty() ||
                                    !conflictedProceduralFiles_.empty();
        const auto conflictCount =
            conflictedSchematicFiles_.size() + conflictedLayoutFiles_.size() + conflictedProceduralFiles_.size();
        refreshProject();
        if (conflictCount > 0) {
            statusBar()->showMessage(
                QStringLiteral("External changes detected; %1 file(s) have conflicts. Choose on save.")
                    .arg(conflictCount),
                12000);
        } else if (adopted) {
            statusBar()->showMessage(QStringLiteral("Project files refreshed from disk"));
        }
        return true;
    }

    bool adoptProjectFromDisk(ice::Project loaded) {
        const auto path = projectPath_;
        const auto openSchematics = openSchematicTabs_;
        const auto openLayouts = openLayoutTabs_;
        const auto knownSchematics = knownSchematicCells_;
        const auto knownLayouts = knownLayoutCells_;
        const auto knownProcedural = knownProceduralLayouts_;
        const auto activeSchematic = activeSchematicId_;
        const auto activeLayout = activeLayoutId_;
        const auto activeProcedural = activeProceduralLayoutId_;
        const auto activeSimulation = activeSimulationId_;
        if (!setProject(std::move(loaded)))
            return false;
        projectPath_ = path;
        if (activityPanel_)
            activityPanel_->setProjectPath(QFileInfo(projectPath_).absolutePath());
        openSchematicTabs_ = openSchematics;
        openLayoutTabs_ = openLayouts;
        knownSchematicCells_ = knownSchematics;
        knownLayoutCells_ = knownLayouts;
        knownProceduralLayouts_ = knownProcedural;
        activeSchematicId_ = activeSchematic;
        activeLayoutId_ = activeLayout;
        activeProceduralLayoutId_ = activeProcedural;
        activeSimulationId_ = activeSimulation;
        refreshEmbeddedPanels();
        refreshSchematic();
        refreshLayout();
        configureProjectWatcher();
        return true;
    }

    void reloadProjectFromDisk(bool confirmDiscard) {
        if (!project_ || projectPath_.isEmpty() || suppressProjectWatcher_)
            return;
        if (!confirmDiscard &&
            (QApplication::activeModalWidget() || (drcProcess_ && drcProcess_->state() != QProcess::NotRunning))) {
            if (projectWatchDebounceTimer_)
                projectWatchDebounceTimer_->start();
            return;
        }
        if (projectWatchDebounceTimer_)
            projectWatchDebounceTimer_->stop();

        if (confirmDiscard && project_->dirty()) {
            if (QMessageBox::question(this, QStringLiteral("Reload Project"),
                                      QStringLiteral("Reloading will discard the unsaved GUI changes. Continue?"),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
                return;
        }

        const auto path = projectPath_;
        try {
            auto loaded = ice::Project::load(path.toStdString());
            if (confirmDiscard || !project_->dirty()) {
                if (!adoptProjectFromDisk(std::move(loaded))) {
                    statusBar()->showMessage(QStringLiteral("Project reload failed."), 10000);
                    return;
                }
                projectDiskChangePending_ = false;
                statusBar()->showMessage(QStringLiteral("Project refreshed from disk"));
            } else {
                if (!mergeExternalProject(loaded)) {
                    statusBar()->showMessage(QStringLiteral("External project changes could not be applied."), 10000);
                    return;
                }
            }
            projectPath_ = path;
            if (activityPanel_)
                activityPanel_->setProjectPath(QFileInfo(projectPath_).absolutePath());
            configureProjectWatcher();
        } catch (const ice::ProjectError &error) {
            statusBar()->showMessage(
                QStringLiteral("Project reload failed: %1").arg(QString::fromStdString(error.what())), 10000);
        }
    }

    void openLayoutInterchange() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open GDS/OASIS Layout"), QString(),
                                                       QStringLiteral("GDSII Stream (*.gds *.gdsii);;OASIS "
                                                                      "(unsupported) (*.oas *.oasis);;All files (*)"));
        if (path.isEmpty())
            return;
        try {
            const auto suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("oas") || suffix == QStringLiteral("oasis"))
                throw ice::ProjectError("interchange.oasis", "Binary OASIS import is not supported yet");
            if (suffix != QStringLiteral("gds") && suffix != QStringLiteral("gdsii"))
                throw ice::ProjectError("interchange.format", "Unsupported layout format (expected .gds/.gdsii)");
            bool accepted = false;
            const auto pinLayer = QInputDialog::getInt(this, QStringLiteral("GDS Pin Labels"),
                                                       QStringLiteral("Pin label GDS layer (-1 imports text only):"),
                                                       -1, -1, 65535, 1, &accepted);
            if (!accepted)
                return;
            ice::LayoutImportOptions options;
            if (pinLayer >= 0)
                options.pinLabelLayer = pinLayer;
            setProject(ice::LayoutInterchange::importGds(path.toStdString(), options));
            projectPath_.clear();
            if (activityPanel_)
                activityPanel_->setProjectPath({});
            statusBar()->showMessage(QStringLiteral("Layout interchange opened"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Open GDS/OASIS Layout"), QString::fromStdString(error.what()));
        } catch (const std::exception &error) {
            QMessageBox::critical(this, QStringLiteral("Open GDS/OASIS Layout"), QString::fromUtf8(error.what()));
        }
    }

    void saveLayoutInterchange() {
        if (!project_ || project_->project().layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Save GDS/OASIS Layout"),
                                     QStringLiteral("Create or open a project with a layout cell first."));
            return;
        }
        auto cellId = activeLayoutId_;
        if (!project_->project().layoutCells.contains(cellId))
            cellId = project_->project().layoutCells.begin()->first;
        const auto path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Save GDS/OASIS Layout"),
                                         QString::fromStdString(project_->project().layoutCells.at(cellId).name),
                                         QStringLiteral("GDSII Stream (*.gds *.gdsii);;OASIS "
                                                        "(unsupported) (*.oas *.oasis);;All files (*)"));
        if (path.isEmpty())
            return;
        try {
            const auto suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("oas") || suffix == QStringLiteral("oasis"))
                throw ice::ProjectError("interchange.oasis", "Binary OASIS export is not supported yet");
            if (suffix != QStringLiteral("gds") && suffix != QStringLiteral("gdsii"))
                throw ice::ProjectError("interchange.format", "Unsupported layout format (expected .gds/.gdsii)");
            ice::LayoutInterchange::exportGds(project_->project(), cellId, path.toStdString());
            statusBar()->showMessage(QStringLiteral("Layout interchange saved"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Save GDS/OASIS Layout"), QString::fromStdString(error.what()));
        }
    }

    void closeProject() {
        if (reportDirty_)
            saveReportSource();
        suppressProjectWatcher_ = false;
        projectDiskChangePending_ = false;
        clearProjectWatcher();
        activeSchematicId_ = 0;
        activeLayoutId_ = 0;
        activeProceduralLayoutId_ = 0;
        activeLayoutPathId_ = 0;
        activeLayoutPathCellId_ = 0;
        activeLayoutPathWidth_ = 0;
        activeSimulationId_ = 0;
        activeReportId_ = 0;
        selectingReport_ = false;
        loadedCachedRunPath_.clear();
        lastSchematicPlacementDevice_.reset();
        project_.reset();
        savedSchematicSignatures_.clear();
        savedLayoutSignatures_.clear();
        savedProceduralSignatures_.clear();
        conflictedSchematicFiles_.clear();
        conflictedLayoutFiles_.clear();
        conflictedProceduralFiles_.clear();
        externalSchematicFiles_.clear();
        externalLayoutFiles_.clear();
        externalProceduralFiles_.clear();
        resetSectionHistories();
        updateHistoryPanels();
        clearLayoutDeviceSelection();
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->resetDevices();
        openSchematicTabs_.clear();
        openLayoutTabs_.clear();
        knownSchematicCells_.clear();
        knownLayoutCells_.clear();
        knownProceduralLayouts_.clear();
        projectPath_.clear();
        if (activityPanel_)
            activityPanel_->setProjectPath({});
        refreshEmbeddedPanels();
        if (schematicCanvas_) {
            schematicCanvas_->scene()->clear();
            schematicCanvas_->clearSelection();
        }
        if (schematicViewStack_ && schematicCanvasPage_)
            schematicViewStack_->setCurrentWidget(schematicCanvasPage_);
        if (textSchematicEditor_)
            textSchematicEditor_->setText({});
        if (schematicSymbolsPanel_)
            schematicSymbolsPanel_->clear();
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->clear();
        schematicSymbolsLoaded_ = false;
        schematicSymbolsPdk_.clear();
        if (layoutCanvas_)
            layoutCanvas_->scene()->clear();
        if (layoutPropertiesPanel_)
            layoutPropertiesPanel_->clearSelection();
        simulationOutput_->setPlainText(QStringLiteral("No project open."));
        schematicSelector_->clear();
        simulationPlot_->clear();
        if (simulationPanel_)
            simulationPanel_->clear();
        activeSimulationId_ = 0;
        rebuildGraphTabs({});
        setWindowTitle(QStringLiteral("ICE"));
        statusBar_->setProjectName(QStringLiteral("No project open"));
        updateSelectionStatus();
        statusBar()->showMessage(QStringLiteral("Ready — no project open"));
        showStartup();
    }

    bool resolveSchematicPdk(const std::string &pdk, QString *pdkLabel, bool *addDependency) {
        *pdkLabel = QStringLiteral("Generic PDK");
        *addDependency = false;
        if (pdk == ice::genericPdkId)
            return true;
        const auto installDirectory = configuredPackageInstallDirectory();
        const auto packages = installedPackages(installDirectory);
        const auto package = std::find_if(packages.begin(), packages.end(),
                                          [&](const auto &entry) { return ice::packageMatchesPdk(entry.spec, pdk); });
        if (package == packages.end()) {
            QMessageBox::critical(
                this, QStringLiteral("Create Schematic"),
                QStringLiteral("The selected PDK package %1 is not installed.").arg(QString::fromStdString(pdk)));
            return false;
        }
        if (!packageProvidesPdk(package->spec)) {
            QMessageBox::critical(
                this, QStringLiteral("Create Schematic"),
                QStringLiteral("Package %1 does not provide a PDK.").arg(QString::fromStdString(pdk)));
            return false;
        }
        *pdkLabel = packagePdkDisplayName(package->spec);
        const auto &dependencies = project_->project().packageDependencies;
        if (std::find(dependencies.begin(), dependencies.end(), pdk) == dependencies.end()) {
            const auto displayName = packagePdkDisplayName(package->spec);
            const auto response = QMessageBox::question(
                this, QStringLiteral("Add Package Dependency"),
                QStringLiteral("This schematic uses the %1 PDK from package %2. Add %2 to the project's "
                               "package dependency list?")
                    .arg(displayName, QString::fromStdString(pdk)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (response != QMessageBox::Yes)
                return false;
            *addDependency = true;
        }
        return true;
    }

    void createSchematic() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateSchematicDialog dialog(installDirectory, nextSchematicDefaultName(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createSchematic(dialog.schematicName(), dialog.pdkId());
    }

    void createSchematic(const QString &name, const std::string &pdk) {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (name.trimmed().isEmpty())
            return;
        QString pdkLabel;
        bool addDependency = false;
        if (!resolveSchematicPdk(pdk, &pdkLabel, &addDependency))
            return;
        try {
            project_->execute("create schematic", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                activeSchematicId_ = ice::createSchematicCell(project, name.trimmed().toStdString(), pdk);
            });
            project_->setLastHistoryLabel("Created schematic " + name.trimmed().toStdString());
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Schematic created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Schematic"), QString::fromStdString(error.what()));
        }
    }

    void showAddSchematicMenu() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        QMenu menu(this);
        auto *schematicAction = menu.addAction(QStringLiteral("Schematic"));
        setLucideIcon(schematicAction, QStringLiteral("microchip"));
        auto *textAction = menu.addAction(QStringLiteral("Text SPICE Schematic"));
        setLucideIcon(textAction, QStringLiteral("notebook-text"));
        auto *svBlockAction = menu.addAction(QStringLiteral("SystemVerilog Block"));
        setLucideIcon(svBlockAction, QStringLiteral("notebook-text"));
        const auto *chosen = menu.exec(QCursor::pos());
        if (chosen == schematicAction)
            createSchematic();
        else if (chosen == textAction)
            createTextSchematic();
        else if (chosen == svBlockAction)
            createSystemVerilogBlock();
    }

    static QString systemVerilogIdentifier(QString text) {
        const auto trimmed = text.trimmed();
        QString result;
        result.reserve(trimmed.size());
        for (const auto character : trimmed) {
            if (character.isLetterOrNumber() || character == QLatin1Char('_'))
                result += character;
            else if (!result.endsWith(QLatin1Char('_')))
                result += QLatin1Char('_');
        }
        while (result.endsWith(QLatin1Char('_')))
            result.chop(1);
        if (result.isEmpty())
            result = QStringLiteral("digital_block");
        if (result.front().isDigit())
            result.prepend(QStringLiteral("block_"));
        return result;
    }

    static QString systemVerilogTemplate(const QString &name, bool testbench) {
        const auto module = systemVerilogIdentifier(name);
        if (testbench) {
            return QStringLiteral("`timescale 1ns/1ps\n"
                                  "// SystemVerilog testbench: %1\n"
                                  "module %2_tb;\n"
                                  "    logic clk = 1'b0;\n"
                                  "    logic rst = 1'b1;\n"
                                  "    logic out;\n"
                                  "\n"
                                  "    // Instantiate and drive the design under test here.\n"
                                  "    always #5 clk = ~clk;\n"
                                  "\n"
                                  "    initial begin\n"
                                  "        $dumpfile(\"%2.vcd\");\n"
                                  "        $dumpvars(0, %2_tb);\n"
                                  "        #20 rst = 1'b0;\n"
                                  "        #200 $finish;\n"
                                  "    end\n"
                                  "endmodule\n")
                .arg(name, module);
        }
        return QStringLiteral("// SystemVerilog block: %1\n"
                              "module %2 (\n"
                              "    input  logic clk,\n"
                              "    input  logic rst,\n"
                              "    output logic out\n"
                              ");\n"
                              "    assign out = 1'b0;\n"
                              "endmodule\n")
            .arg(name, module);
    }

    void createSystemVerilogBlock() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create SystemVerilog Block"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateSchematicDialog dialog(installDirectory, nextSchematicDefaultName() + QStringLiteral(" Block"), this);
        dialog.setWindowTitle(QStringLiteral("Create SystemVerilog Block"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        createSystemVerilogCell(dialog.schematicName(), dialog.pdkId(), false);
    }

    void createSystemVerilogCell(const QString &name, const std::string &pdk, bool testbench) {
        if (!project_)
            return;
        const auto trimmedName = name.trimmed();
        if (trimmedName.isEmpty())
            return;
        QString pdkLabel;
        bool addDependency = false;
        if (!resolveSchematicPdk(pdk, &pdkLabel, &addDependency))
            return;
        ice::ObjectId createdId = 0;
        try {
            project_->execute("create SystemVerilog cell", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                createdId = ice::createSystemVerilogCell(project, trimmedName.toStdString(), pdk,
                                                         systemVerilogTemplate(trimmedName, testbench).toStdString());
            });
            project_->setLastHistoryLabel("Created SystemVerilog cell " + trimmedName.toStdString());
            activeSchematicId_ = createdId;
            refreshProject();
            selectSchematicTab(createdId);
            if (documents_ && schematicPage_)
                documents_->setCurrentWidget(schematicPage_);
            statusBar()->showMessage(
                QStringLiteral("SystemVerilog %1 created with %2")
                    .arg(testbench ? QStringLiteral("testbench") : QStringLiteral("block"), pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create SystemVerilog Cell"),
                                  QString::fromStdString(error.what()));
        }
    }

    void createTextSchematic() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Text SPICE Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateSchematicDialog dialog(installDirectory, nextSchematicDefaultName(), this);
        dialog.setWindowTitle(QStringLiteral("Create Text SPICE Schematic"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        createTextSchematic(dialog.schematicName(), dialog.pdkId());
    }

    void createTextSchematic(const QString &name, const std::string &pdk) {
        if (!project_)
            return;
        const auto trimmedName = name.trimmed();
        if (trimmedName.isEmpty())
            return;
        QString pdkLabel;
        bool addDependency = false;
        if (!resolveSchematicPdk(pdk, &pdkLabel, &addDependency))
            return;
        ice::ObjectId createdId = 0;
        try {
            project_->execute("create text schematic", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                const auto text =
                    QStringLiteral(
                        "* SPICE schematic: %1\n.title %1\n* Add SPICE devices and directives below.\n.end\n")
                        .arg(trimmedName);
                createdId = ice::createTextSchematicCell(project, trimmedName.toStdString(), pdk, text.toStdString());
            });
            project_->setLastHistoryLabel("Created text SPICE schematic " + trimmedName.toStdString());
            activeSchematicId_ = createdId;
            refreshProject();
            selectSchematicTab(createdId);
            if (documents_ && schematicPage_)
                documents_->setCurrentWidget(schematicPage_);
            statusBar()->showMessage(QStringLiteral("Text SPICE schematic created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Text SPICE Schematic"),
                                  QString::fromStdString(error.what()));
        }
    }

    bool resolveLayoutPdk(const std::string &pdk, QString *pdkLabel, std::optional<ice::PackagePdk> *selectedPdk,
                          bool *addDependency) {
        *pdkLabel = QStringLiteral("Generic PDK");
        selectedPdk->reset();
        *addDependency = false;
        if (pdk == ice::genericPdkId)
            return true;
        const auto installDirectory = configuredPackageInstallDirectory();
        const auto packages = installedPackages(installDirectory);
        const auto package = std::find_if(packages.begin(), packages.end(),
                                          [&](const auto &entry) { return ice::packageMatchesPdk(entry.spec, pdk); });
        if (package == packages.end()) {
            QMessageBox::critical(
                this, QStringLiteral("Create Layout Cell"),
                QStringLiteral("The selected PDK package %1 is not installed.").arg(QString::fromStdString(pdk)));
            return false;
        }
        if (!packageProvidesPdk(package->spec)) {
            QMessageBox::critical(
                this, QStringLiteral("Create Layout Cell"),
                QStringLiteral("Package %1 does not provide a PDK.").arg(QString::fromStdString(pdk)));
            return false;
        }
        *pdkLabel = packagePdkDisplayName(package->spec);
        if (package->spec.pdk)
            *selectedPdk = *package->spec.pdk;
        const auto &dependencies = project_->project().packageDependencies;
        if (std::find(dependencies.begin(), dependencies.end(), pdk) == dependencies.end()) {
            const auto displayName = packagePdkDisplayName(package->spec);
            const auto response =
                QMessageBox::question(this, QStringLiteral("Add Package Dependency"),
                                      QStringLiteral("This layout uses the %1 PDK from package %2. Add %2 to the "
                                                     "project's package dependency list?")
                                          .arg(displayName, QString::fromStdString(pdk)),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (response != QMessageBox::Yes)
                return false;
            *addDependency = true;
        }
        return true;
    }

    void createLayoutCell() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layout Cell"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateLayoutDialog dialog(installDirectory, nextLayoutDefaultName(), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createLayoutCell(dialog.layoutName(), dialog.pdkId());
    }

    void createLayoutCell(const QString &name, const std::string &pdk) {
        if (!project_ || name.trimmed().isEmpty())
            return;
        QString pdkLabel;
        std::optional<ice::PackagePdk> selectedPdk;
        bool addDependency = false;
        if (!resolveLayoutPdk(pdk, &pdkLabel, &selectedPdk, &addDependency))
            return;
        try {
            project_->execute("create layout cell", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                if (selectedPdk)
                    addPdkLayers(project, *selectedPdk, pdk);
                activeLayoutId_ = ice::createLayoutCell(project, name.trimmed().toStdString(), pdk);
            });
            project_->setLastHistoryLabel("Created layout cell " + name.trimmed().toStdString());
            activeProceduralLayoutId_ = 0;
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Layout cell created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Layout Cell"), QString::fromStdString(error.what()));
        }
    }

    void showAddLayoutMenu() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layout"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        QMenu menu(this);
        auto *staticAction = menu.addAction(QStringLiteral("Static Layout"));
        setLucideIcon(staticAction, QStringLiteral("notebook-pen"));
        auto *proceduralAction = menu.addAction(QStringLiteral("Procedural Layout"));
        setLucideIcon(proceduralAction, QStringLiteral("notebook-text"));
        const auto *chosen = menu.exec(QCursor::pos());
        if (chosen == staticAction)
            createLayoutCell();
        else if (chosen == proceduralAction)
            createProceduralLayout();
    }

    void createProceduralLayout() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Procedural Layout"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateLayoutDialog dialog(installDirectory, nextLayoutDefaultName(), this);
        dialog.setWindowTitle(QStringLiteral("Create Procedural Layout"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        createProceduralLayout(dialog.layoutName(), dialog.pdkId());
    }

    void createProceduralLayout(const QString &name, const std::string &pdk) {
        if (!project_ || name.trimmed().isEmpty())
            return;
        QString pdkLabel;
        std::optional<ice::PackagePdk> selectedPdk;
        bool addDependency = false;
        if (!resolveLayoutPdk(pdk, &pdkLabel, &selectedPdk, &addDependency))
            return;
        ice::ObjectId proceduralId = 0;
        QString generationError;
        bool generated = false;
        try {
            project_->execute("create procedural layout", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                if (selectedPdk)
                    addPdkLayers(project, *selectedPdk, pdk);
                proceduralId = ice::createProceduralLayout(project, name.trimmed().toStdString(), pdk,
                                                           ice::defaultProceduralLayoutScript());
                generated = ice::regenerateProceduralLayout(project, proceduralId, &generationError);
            });
            project_->setLastHistoryLabel("Created procedural layout " + name.trimmed().toStdString());
            const auto &procedural = project_->project().proceduralLayouts.at(proceduralId);
            activeProceduralLayoutId_ = proceduralId;
            activeLayoutId_ = procedural.generatedCell;
            refreshProject();
            selectLayoutTab(proceduralId);
            if (documents_ && layoutPage_)
                documents_->setCurrentWidget(layoutPage_);
            if (!generated) {
                QMessageBox::warning(this, QStringLiteral("Create Procedural Layout"),
                                     QStringLiteral("The procedural layout was created, but its initial generation "
                                                    "failed:\n%1")
                                         .arg(generationError));
            }
            statusBar()->showMessage(QStringLiteral("Procedural layout created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Procedural Layout"),
                                  QString::fromStdString(error.what()));
        }
    }

    void regenerateProceduralLayout(ice::ObjectId proceduralId) {
        if (!project_)
            return;
        const auto proceduralIt = project_->project().proceduralLayouts.find(proceduralId);
        if (proceduralIt == project_->project().proceduralLayouts.end())
            return;
        const auto name = proceduralIt->second.name;
        QString error;
        try {
            project_->execute("regenerate procedural layout", [&](ice::Project &project) {
                if (!ice::regenerateProceduralLayout(project, proceduralId, &error))
                    throw ice::ProjectError("layout.procedural.generate", error.toStdString());
            });
        } catch (const ice::ProjectError &exception) {
            if (error.isEmpty())
                error = QString::fromStdString(exception.what());
            QMessageBox::critical(
                this, QStringLiteral("Regenerate Layout"),
                QStringLiteral("Unable to regenerate '%1':\n%2").arg(QString::fromStdString(name), error));
            return;
        }
        project_->setLastHistoryLabel("Regenerated layout " + name);
        refreshProject();
        statusBar()->showMessage(
            QStringLiteral("Procedural layout '%1' regenerated").arg(QString::fromStdString(name)));
    }

    void onProceduralScriptEdited(const QString &script) {
        if (!project_ || activeProceduralLayoutId_ == 0)
            return;
        auto &procedural = project_->project().proceduralLayouts;
        auto it = procedural.find(activeProceduralLayoutId_);
        if (it == procedural.end())
            return;
        const auto text = script.toStdString();
        if (it->second.script == text)
            return;
        it->second.script = text;
        project_->markDirty();
        updateFileTabDirtyIndicators();
    }

    void onTextSchematicEdited(const QString &text) {
        if (!project_ || activeSchematicId_ == 0)
            return;
        auto &cells = project_->project().schematicCells;
        const auto it = cells.find(activeSchematicId_);
        if (it == cells.end() || !it->second.isTextSource())
            return;
        const auto value = text.toStdString();
        auto &source = it->second.systemVerilogSchematic ? it->second.systemVerilogText : it->second.spiceText;
        if (source == value)
            return;
        source = value;
        project_->markDirty();
        updateFileTabDirtyIndicators();
    }

    void showTextSchematicEditor(ice::ObjectId schematicId) {
        if (!project_)
            return;
        const auto it = project_->project().schematicCells.find(schematicId);
        if (it == project_->project().schematicCells.end() || !it->second.isTextSource())
            return;
        activeSchematicId_ = schematicId;
        if (schematicViewStack_ && textSchematicEditorPage_)
            schematicViewStack_->setCurrentWidget(textSchematicEditorPage_);
        if (textSchematicEditor_) {
            textSchematicEditor_->setLanguage(textSourceLanguage(it->second));
            const auto text = QString::fromStdString(it->second.sourceText());
            if (textSchematicEditor_->text() != text)
                textSchematicEditor_->setText(text);
        }
    }

    void activateSchematicTab(int index) {
        if (!schematicCellTabs_ || index < 0 || !project_)
            return;
        const auto id = static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index).toULongLong());
        const auto it = project_->project().schematicCells.find(id);
        if (it == project_->project().schematicCells.end())
            return;
        activeSchematicId_ = id;
        if (it->second.isTextSource()) {
            showTextSchematicEditor(id);
            updateEmptyWorkspaceState();
            return;
        }
        if (schematicViewStack_ && schematicCanvasPage_)
            schematicViewStack_->setCurrentWidget(schematicCanvasPage_);
        refreshSchematic();
        updateEmptyWorkspaceState();
    }

    void selectSchematicTab(ice::ObjectId id) {
        if (!schematicCellTabs_)
            return;
        if (project_ && project_->project().schematicCells.contains(id) && !openSchematicTabs_.contains(id)) {
            openSchematicTabs_.insert(id);
            knownSchematicCells_.insert(id);
            refreshEmbeddedPanels();
        }
        for (int index = 0; index < schematicCellTabs_->count(); ++index) {
            if (static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index).toULongLong()) != id)
                continue;
            if (schematicCellTabs_->currentIndex() == index)
                activateSchematicTab(index);
            else
                schematicCellTabs_->setCurrentIndex(index);
            if (documents_ && schematicPage_)
                documents_->setCurrentWidget(schematicPage_);
            return;
        }
    }

    // An instance whose symbol was created from another schematic cell
    // (Symbol::sourceCell) descends into that cell, so "go down" follows the
    // selected instance and "go up" returns to the cell that instantiates the
    // active schematic.
    void openSelectedSchematicCell() {
        if (!project_ || activeSchematicId_ == 0) {
            statusBar()->showMessage(QStringLiteral("Open a schematic first"));
            return;
        }
        const auto instanceId = schematicCanvas_ ? schematicCanvas_->selectedInstanceId() : 0;
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a schematic instance first"));
            return;
        }
        const auto activeIt = project_->project().schematicCells.find(activeSchematicId_);
        if (activeIt == project_->project().schematicCells.end())
            return;
        const auto instanceIt = activeIt->second.instances.find(instanceId);
        if (instanceIt == activeIt->second.instances.end())
            return;
        const auto symbolIt = activeIt->second.symbols.find(instanceIt->second.symbol);
        if (symbolIt == activeIt->second.symbols.end())
            return;
        const auto childIt = project_->project().schematicCells.find(symbolIt->second.sourceCell);
        if (childIt == project_->project().schematicCells.end()) {
            statusBar()->showMessage(QStringLiteral("This instance does not reference another schematic"));
            return;
        }
        const auto childId = childIt->first;
        const auto childName = QString::fromStdString(childIt->second.name);
        selectSchematicTab(childId);
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(childName));
    }

    void openSchematicParentCell() {
        if (!project_ || activeSchematicId_ == 0) {
            statusBar()->showMessage(QStringLiteral("Open a schematic first"));
            return;
        }
        for (const auto &[id, cell] : project_->project().schematicCells) {
            if (id == activeSchematicId_)
                continue;
            const auto instantiatesActive =
                std::any_of(cell.instances.begin(), cell.instances.end(), [&](const auto &entry) {
                    const auto symbol = cell.symbols.find(entry.second.symbol);
                    return symbol != cell.symbols.end() && symbol->second.sourceCell == activeSchematicId_;
                });
            if (!instantiatesActive)
                continue;
            const auto name = QString::fromStdString(cell.name);
            selectSchematicTab(id);
            statusBar()->showMessage(QStringLiteral("Opened %1").arg(name));
            return;
        }
        statusBar()->showMessage(QStringLiteral("Already at the top of the schematic hierarchy"));
    }

    void showProceduralLayoutEditor(ice::ObjectId proceduralId) {
        if (!project_)
            return;
        const auto it = project_->project().proceduralLayouts.find(proceduralId);
        if (it == project_->project().proceduralLayouts.end())
            return;
        activeProceduralLayoutId_ = proceduralId;
        activeLayoutId_ = it->second.generatedCell;
        if (layoutViewStack_ && proceduralEditorPage_)
            layoutViewStack_->setCurrentWidget(proceduralEditorPage_);
        if (proceduralEditor_) {
            const auto script = QString::fromStdString(it->second.script);
            if (proceduralEditor_->script() != script)
                proceduralEditor_->setScript(script);
        }
    }

    void activateLayoutTab(int index) {
        if (!layoutCellTabs_ || index < 0 || !project_)
            return;
        const auto id = static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index).toULongLong());
        const auto &project = project_->project();
        if (project.proceduralLayouts.contains(id)) {
            showProceduralLayoutEditor(id);
            updateEmptyWorkspaceState();
            return;
        }
        activeProceduralLayoutId_ = 0;
        activeLayoutId_ = id;
        if (layoutViewStack_ && layoutCanvasPage_)
            layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
        refreshLayout();
        updateEmptyWorkspaceState();
    }

    void selectLayoutTab(ice::ObjectId id) {
        if (!layoutCellTabs_)
            return;
        if (project_ &&
            (project_->project().layoutCells.contains(id) || project_->project().proceduralLayouts.contains(id)) &&
            !openLayoutTabs_.contains(id)) {
            openLayoutTabs_.insert(id);
            knownLayoutCells_.insert(id);
            knownProceduralLayouts_.insert(id);
            refreshEmbeddedPanels();
        }
        for (int index = 0; index < layoutCellTabs_->count(); ++index) {
            if (static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index).toULongLong()) != id)
                continue;
            if (layoutCellTabs_->currentIndex() == index)
                activateLayoutTab(index);
            else
                layoutCellTabs_->setCurrentIndex(index);
            return;
        }
    }

    void closeSchematicTab(int index) {
        if (!schematicCellTabs_ || !project_ || index < 0)
            return;
        const auto id = static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index).toULongLong());
        if (id == 0)
            return;
        ice::ObjectId nextId = 0;
        if (activeSchematicId_ == id) {
            if (index + 1 < schematicCellTabs_->count())
                nextId = static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index + 1).toULongLong());
            else if (index > 0)
                nextId = static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index - 1).toULongLong());
        }
        openSchematicTabs_.erase(id);
        if (activeSchematicId_ == id)
            activeSchematicId_ = nextId;
        refreshEmbeddedPanels();
        if (openSchematicTabs_.empty()) {
            if (schematicCanvas_)
                schematicCanvas_->scene()->clear();
            if (textSchematicEditor_)
                textSchematicEditor_->setText({});
            if (schematicNetlistPanel_)
                schematicNetlistPanel_->clear();
            if (schematicPropertiesPanel_)
                schematicPropertiesPanel_->clearSelection();
        } else {
            refreshSchematic();
        }
        statusBar()->showMessage(QStringLiteral("Schematic file closed"));
    }

    void closeLayoutTab(int index) {
        if (!layoutCellTabs_ || !project_ || index < 0)
            return;
        const auto id = static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index).toULongLong());
        if (id == 0)
            return;
        ice::ObjectId nextId = 0;
        if (activeProceduralLayoutId_ == id || (activeProceduralLayoutId_ == 0 && activeLayoutId_ == id)) {
            if (index + 1 < layoutCellTabs_->count())
                nextId = static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index + 1).toULongLong());
            else if (index > 0)
                nextId = static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index - 1).toULongLong());
        }
        openLayoutTabs_.erase(id);
        if (nextId != 0) {
            const auto procedural = project_->project().proceduralLayouts.find(nextId);
            if (procedural != project_->project().proceduralLayouts.end()) {
                activeProceduralLayoutId_ = nextId;
                activeLayoutId_ = procedural->second.generatedCell;
            } else {
                activeProceduralLayoutId_ = 0;
                activeLayoutId_ = nextId;
            }
        } else if (activeProceduralLayoutId_ == id || activeLayoutId_ == id) {
            activeProceduralLayoutId_ = 0;
            activeLayoutId_ = 0;
        }
        refreshEmbeddedPanels();
        if (openLayoutTabs_.empty()) {
            if (layoutCanvas_)
                layoutCanvas_->scene()->clear();
            if (layoutPropertiesPanel_)
                layoutPropertiesPanel_->clearSelection();
            refreshLayoutProperties();
        } else {
            refreshLayout();
        }
        statusBar()->showMessage(QStringLiteral("Layout file closed"));
    }

    void makeLayoutFromSchematic(ice::ObjectId schematicId) {
        if (!project_)
            return;
        const auto schematicIt = project_->project().schematicCells.find(schematicId);
        if (schematicIt == project_->project().schematicCells.end())
            return;
        const auto schematicName = QString::fromStdString(schematicIt->second.name);
        try {
            project_->execute("make layout from schematic", [&](ice::Project &project) {
                activeLayoutId_ = ice::createLayoutFromSchematic(project, schematicId);
                ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Make Layout from Schematic"),
                                  QString::fromStdString(error.what()));
            return;
        }
        layoutViewInitialized_ = false;
        activeProceduralLayoutId_ = 0;
        if (layoutViewStack_ && layoutCanvasPage_)
            layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
        if (documents_ && layoutPage_)
            documents_->setCurrentWidget(layoutPage_);
        if (layoutSelectTool_)
            layoutSelectTool_->setChecked(true);
        if (layoutCanvas_)
            layoutCanvas_->setTool(LayoutCanvas::Tool::select);
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout created from schematic '%1'").arg(schematicName));
    }

    ice::ObjectId createLayoutDeviceInstanceCell(ice::Project &project, const ice::LayoutDeviceCell &device,
                                                 const ice::LayoutDeviceLibrary &library,
                                                 const std::map<std::string, std::string> *values = nullptr,
                                                 const ice::LayoutDeviceGeometry *geometry = nullptr) const {
        const auto cellId = ice::createLayoutCell(
            project, uniqueDeviceCellName(project, device.name, layoutDeviceCellPrefix(library)), library.pdk);
        auto &cell = project.layoutCells.at(cellId);
        cell.library = true;
        cell.device = device.name;
        syncLayoutDeviceParameters(project, cell, device);
        if (values) {
            for (auto &parameter : cell.parameters) {
                if (const auto value = values->find(parameter.name); value != values->end())
                    parameter.value = value->second;
            }
        }
        if (geometry)
            applyLayoutDeviceShapes(project, cell, library, geometry->shapes, geometry->ports);
        else
            applyLayoutDeviceShapes(project, cell, library, device.shapes, device.ports);
        ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
        return cellId;
    }

    void placeLayoutDevice(const QPointF &position) {
        if (!project_)
            return;
        if (!selectedLayoutDevice_) {
            statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
            return;
        }
        if (project_->project().layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Place Device"),
                                     QStringLiteral("Create a layout cell before placing a device."));
            return;
        }
        if (!layoutDevicesPanel_)
            return;
        auto activeId = activeLayoutId_;
        if (!project_->project().layoutCells.contains(activeId))
            activeId = project_->project().layoutCells.begin()->first;
        if (project_->project().layoutCells.at(activeId).library) {
            statusBar()->showMessage(QStringLiteral("Library cells are read-only"));
            return;
        }
        const auto bounds = selectedLayoutDevice_->bounds;
        const qreal centerX = bounds.valid() ? bounds.x + bounds.width / 2.0 : 0.0;
        const qreal centerY = bounds.valid() ? bounds.y + bounds.height / 2.0 : 0.0;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x() - centerX));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y() - centerY));
        const auto &library = layoutDevicesPanel_->library();
        bool placed = false;
        std::string placedName;
        try {
            project_->execute("place layout device", [&](ice::Project &project) {
                const auto childId = createLayoutDeviceInstanceCell(project, *selectedLayoutDevice_, library);
                if (childId == activeId)
                    return;
                auto &cell = project.layoutCells.at(activeId);
                const auto baseName = selectedLayoutDevice_->name;
                std::string name;
                for (int index = 1;; ++index) {
                    name = baseName + "_" + std::to_string(index);
                    const bool exists = std::any_of(cell.instances.begin(), cell.instances.end(),
                                                    [&](const auto &entry) { return entry.second.name == name; });
                    if (!exists)
                        break;
                }
                const auto instanceId = project.nextId();
                cell.instances.emplace(instanceId,
                                       ice::LayoutInstance{instanceId, childId, name, x, y, ice::Orientation::r0});
                placedName = name;
                placed = true;
            });
            if (!placed) {
                statusBar()->showMessage(QStringLiteral("Cannot place a cell inside itself"));
                return;
            }
            project_->setLastHistoryLabel("Placed layout device " + selectedLayoutDevice_->name + " " + placedName);
            activeLayoutId_ = activeId;
            refreshProject();
            if (layoutPropertiesPanel_ && layoutLeftTabs_ && layoutLeftTabs_->currentWidget() == layoutDevicesPanel_)
                layoutPropertiesPanel_->setLayoutDevice(selectedLayoutDevice_);
            statusBar()->showMessage(
                QStringLiteral("Placed %1").arg(QString::fromStdString(selectedLayoutDevice_->name)));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
        }
    }

    void activateLayoutPlannedDevice(ice::ObjectId deviceId) {
        if (!project_ || !layoutDevicesPanel_ || deviceId == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto planned = cellIt->second.plannedDevices.find(deviceId);
        if (planned == cellIt->second.plannedDevices.end())
            return;

        const auto element = plannedDeviceSpiceElement(project_->project(), planned->second);
        if (element == 'V' || element == 'I' || element == 'G' || element == 'P') {
            QMenu pinMenu(this);
            auto *convert = pinMenu.addAction(QStringLiteral("Convert to Pin"));
            auto *chosenPin = pinMenu.exec(QCursor::pos());
            if (chosenPin == convert)
                convertPlannedDeviceToPin(deviceId);
            return;
        }

        QMenu menu(this);
        auto *manual = menu.addAction(QStringLiteral("Draw Layout Manually"));
        auto *place = menu.addAction(QStringLiteral("Place Layout Device…"));
        place->setEnabled(!layoutDevicesPanel_->library().cells.empty());
        auto *chosen = menu.exec(QCursor::pos());
        if (chosen == manual) {
            const auto name = planned->second.name;
            project_->execute("remove planned device", [&](ice::Project &project) {
                auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                cell->second.plannedDevices.erase(deviceId);
                std::erase_if(cell->second.plannedConnections, [&](const auto &entry) {
                    return entry.second.from == deviceId || entry.second.to == deviceId;
                });
            });
            project_->setLastHistoryLabel("Removed layout planned device " + name);
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Planned device removed; draw its layout manually."));
            return;
        }
        if (chosen == place)
            placeLayoutDeviceForPlanned(deviceId);
    }

    void convertPlannedDeviceToPin(ice::ObjectId deviceId) {
        if (!project_ || deviceId == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto plannedIt = cellIt->second.plannedDevices.find(deviceId);
        if (plannedIt == cellIt->second.plannedDevices.end())
            return;
        const auto planned = plannedIt->second;
        const auto element = plannedDeviceSpiceElement(project_->project(), planned);
        if (element != 'V' && element != 'I' && element != 'G' && element != 'P')
            return;
        const auto portDirection =
            element == 'P' ? plannedDevicePortDirection(project_->project(), planned) : std::nullopt;

        std::set<ice::ObjectId> connectedNets;
        for (const auto &[_, connection] : cellIt->second.plannedConnections) {
            if (connection.from != deviceId && connection.to != deviceId)
                continue;
            if (connection.net != 0)
                connectedNets.insert(connection.net);
        }

        const auto layers = layoutLayers(project_->project(), activeLayoutId_);
        if (layers.empty()) {
            QMessageBox::information(
                this, QStringLiteral("Convert to Pin"),
                QStringLiteral("Create a layout layer before converting sources or grounds to pins."));
            return;
        }
        const auto isMetalLayer = [](const ice::Layer &layer) {
            return layerKey(layer.name).find("metal") != std::string::npos;
        };
        const auto isPinLayer = [](const ice::Layer &layer) { return layerIsPinLayer(layer); };

        ice::ObjectId existingPin = 0;
        if (element != 'P' && connectedNets.size() == 1) {
            const auto net = *connectedNets.begin();
            for (const auto &[portId, port] : cellIt->second.ports) {
                if (port.net == net) {
                    existingPin = portId;
                    break;
                }
            }
        }

        // Prefer a metal pin layer, then any pin layer, then a metal drawing
        // layer, then whatever the layout provides.
        ice::ObjectId defaultLayer = 0;
        for (const auto &[_, layer] : project_->project().layers) {
            if (layerAvailableForLayout(project_->project(), activeLayoutId_, layer) && isPinLayer(layer) &&
                isMetalLayer(layer)) {
                defaultLayer = layer.id;
                break;
            }
        }
        if (defaultLayer == 0) {
            for (const auto &[_, layer] : project_->project().layers) {
                if (layerAvailableForLayout(project_->project(), activeLayoutId_, layer) && isPinLayer(layer)) {
                    defaultLayer = layer.id;
                    break;
                }
            }
        }
        if (defaultLayer == 0) {
            for (const auto &[_, layer] : project_->project().layers) {
                if (layerAvailableForLayout(project_->project(), activeLayoutId_, layer) && isMetalLayer(layer)) {
                    defaultLayer = layer.id;
                    break;
                }
            }
        }
        if (defaultLayer == 0)
            defaultLayer = layers.front().get().id;

        std::vector<ice::ObjectId> candidateLayers;
        const auto appendCandidates = [&](const auto &predicate) {
            for (const auto &layerRef : layers) {
                const auto &layer = layerRef.get();
                if (!predicate(layer) ||
                    std::find(candidateLayers.begin(), candidateLayers.end(), layer.id) != candidateLayers.end())
                    continue;
                candidateLayers.push_back(layer.id);
            }
        };
        appendCandidates([&](const ice::Layer &layer) { return isPinLayer(layer) && isMetalLayer(layer); });
        appendCandidates(isPinLayer);
        if (candidateLayers.empty())
            appendCandidates([&](const ice::Layer &layer) { return isMetalLayer(layer); });
        if (candidateLayers.empty())
            appendCandidates([](const ice::Layer &) { return true; });
        if (candidateLayers.empty())
            return;

        auto pinLayer = candidateLayers.front();
        if (existingPin == 0 && candidateLayers.size() > 1) {
            QDialog layerDialog(this);
            layerDialog.setWindowTitle(QStringLiteral("Convert to Pin"));
            auto *layerLayout = new QVBoxLayout(&layerDialog);
            layerLayout->addWidget(new QLabel(QStringLiteral("Pin layer:"), &layerDialog));
            auto *layerCombo = new QComboBox(&layerDialog);
            for (const auto layerId : candidateLayers)
                layerCombo->addItem(QString::fromStdString(project_->project().layers.at(layerId).name));
            if (const auto it = std::find(candidateLayers.begin(), candidateLayers.end(), defaultLayer);
                it != candidateLayers.end())
                layerCombo->setCurrentIndex(static_cast<int>(std::distance(candidateLayers.begin(), it)));
            layerLayout->addWidget(layerCombo);
            auto *layerButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &layerDialog);
            connect(layerButtons, &QDialogButtonBox::accepted, &layerDialog, &QDialog::accept);
            connect(layerButtons, &QDialogButtonBox::rejected, &layerDialog, &QDialog::reject);
            layerLayout->addWidget(layerButtons);
            if (layerDialog.exec() != QDialog::Accepted)
                return;
            pinLayer = candidateLayers.at(static_cast<std::size_t>(layerCombo->currentIndex()));
        }

        const auto pinSize = std::max<ice::Coordinate>(1, project_->project().snap(400));
        ice::ObjectId usedPort = 0;
        try {
            project_->execute("convert planned device to pin", [&](ice::Project &project) {
                auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                auto &layout = cell->second;
                const auto normalized = [](std::string value) {
                    std::transform(value.begin(), value.end(), value.begin(),
                                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
                    return value;
                };
                std::set<std::string> names;
                for (const auto &[_, port] : layout.ports)
                    names.insert(normalized(port.name));

                const auto pinNet = connectedNets.size() == 1 ? *connectedNets.begin() : ice::ObjectId{0};
                std::string base;
                if (element != 'P' && pinNet != 0) {
                    if (const auto netIt = layout.nets.find(pinNet); netIt != layout.nets.end())
                        base = netIt->second.name;
                    if (base == "0")
                        base = "GND";
                }
                if (base.empty()) {
                    if (element == 'G')
                        base = "GND";
                    else if (!planned.name.empty())
                        base = planned.name;
                    else
                        base = "PIN";
                }
                std::string name = base;
                for (int suffix = 2; names.contains(normalized(name)); ++suffix)
                    name = base + "_" + std::to_string(suffix);
                names.insert(normalized(name));

                auto portId = existingPin;
                if (portId == 0) {
                    auto x = planned.x + planned.width / 2 - pinSize / 2;
                    auto y = planned.y + planned.height / 2 - pinSize / 2;
                    x = std::clamp(x, planned.x, planned.x + std::max<ice::Coordinate>(0, planned.width - pinSize));
                    y = std::clamp(y, planned.y, planned.y + std::max<ice::Coordinate>(0, planned.height - pinSize));
                    portId = ice::addPort(project, activeLayoutId_, name, pinLayer, ice::Rect{x, y, pinSize, pinSize},
                                          portDirection.value_or(ice::PortDirection::inout), ice::PortSide::left,
                                          pinNet, name);
                    if (planned.schematicInstance != 0) {
                        auto &port = project.layoutCells.at(activeLayoutId_).ports.at(portId);
                        port.schematicInstance = planned.schematicInstance;
                    }
                }
                usedPort = portId;

                for (auto connection = layout.plannedConnections.begin();
                     connection != layout.plannedConnections.end();) {
                    auto &value = connection->second;
                    bool touches = false;
                    if (value.from == deviceId) {
                        value.from = portId;
                        value.fromPort = 0;
                        touches = true;
                    }
                    if (value.to == deviceId) {
                        value.to = portId;
                        value.toPort = 0;
                        touches = true;
                    }
                    if (touches && value.from == value.to) {
                        connection = layout.plannedConnections.erase(connection);
                        continue;
                    }
                    ++connection;
                }
                layout.plannedDevices.erase(deviceId);
                updateRoutedConnectionPaths(project);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Convert to Pin"), QString::fromStdString(error.what()));
            return;
        }
        if (usedPort == 0) {
            statusBar()->showMessage(QStringLiteral("Could not create a layout pin"));
            return;
        }
        project_->setLastHistoryLabel("Converted layout planned device " + planned.name + " to pin");
        refreshProject();
        QTimer::singleShot(0, this, [this, portId = usedPort] { selectLayoutShapeItem(portId); });
        statusBar()->showMessage(
            QStringLiteral("Converted %1 to a layout pin.").arg(QString::fromStdString(planned.name)));
    }

    void placeLayoutDeviceForPlanned(ice::ObjectId deviceId) {
        if (!project_ || !layoutDevicesPanel_ || deviceId == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto plannedIt = cellIt->second.plannedDevices.find(deviceId);
        if (plannedIt == cellIt->second.plannedDevices.end())
            return;
        const auto &planned = plannedIt->second;
        const auto &library = layoutDevicesPanel_->library();

        std::string symbolName;
        std::vector<std::string> schematicPins;
        std::map<std::string, std::string> initialValues;
        if (planned.schematicInstance != 0) {
            for (const auto &[_, schematic] : project_->project().schematicCells) {
                const auto instance = schematic.instances.find(planned.schematicInstance);
                if (instance == schematic.instances.end())
                    continue;
                if (const auto symbol = schematic.symbols.find(instance->second.symbol);
                    symbol != schematic.symbols.end()) {
                    symbolName = symbol->second.name;
                    for (const auto &pin : symbol->second.pins)
                        schematicPins.push_back(pin.name);
                }
                for (const auto &parameter : instance->second.parameters)
                    initialValues[parameter.name] = parameter.value;
                break;
            }
        }
        if (symbolName.empty())
            symbolName = planned.name;

        LayoutDevicePlacementDialog dialog(library, symbolName, schematicPins, initialValues, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto *device = dialog.selectedDevice();
        if (!device)
            return;
        auto values = dialog.parameterValues();

        std::optional<ice::LayoutDeviceGeometry> geometry;
        if (device->generator) {
            QString generationError;
            WaitCursorGuard waitCursor;
            statusBar()->showMessage(QStringLiteral("Generating %1…").arg(QString::fromStdString(device->name)));
            geometry = generateLayoutDeviceGeometry(*device, values, &generationError, library.pdk);
            if (!geometry) {
                QMessageBox::warning(this, QStringLiteral("Place Layout Device"),
                                     QStringLiteral("Could not generate %1:\n\n%2")
                                         .arg(QString::fromStdString(device->name), generationError));
                return;
            }
        }

        const auto bounds = geometry ? geometry->bounds : device->bounds;
        const auto centerX = bounds.valid() ? bounds.x + bounds.width / 2.0 : 0.0;
        const auto centerY = bounds.valid() ? bounds.y + bounds.height / 2.0 : 0.0;
        const auto targetX = planned.x + planned.width / 2.0;
        const auto targetY = planned.y + planned.height / 2.0;
        const auto instanceX = static_cast<ice::Coordinate>(std::lround(targetX - centerX));
        const auto instanceY = static_cast<ice::Coordinate>(std::lround(targetY - centerY));
        const auto plannedName = planned.name;
        const auto schematicInstance = planned.schematicInstance;
        const auto deviceCopy = *device;
        const auto cellId = activeLayoutId_;

        ice::ObjectId instanceId = 0;
        std::string instanceName;
        try {
            project_->execute("place planned layout device", [&](ice::Project &project) {
                auto cell = project.layoutCells.find(cellId);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                const auto childId = createLayoutDeviceInstanceCell(project, deviceCopy, library, &values,
                                                                    geometry ? &*geometry : nullptr);
                if (childId == cellId)
                    return;
                auto &layout = cell->second;
                const auto baseName = plannedName.empty() ? deviceCopy.name : plannedName;
                auto name = baseName;
                for (int suffix = 2;; ++suffix) {
                    const bool exists = std::any_of(layout.instances.begin(), layout.instances.end(),
                                                    [&](const auto &entry) { return entry.second.name == name; });
                    if (!exists)
                        break;
                    name = baseName + "_" + std::to_string(suffix);
                }
                instanceId = project.nextId();
                ice::LayoutInstance instance{instanceId, childId, name, instanceX, instanceY, ice::Orientation::r0};
                instance.schematicInstance = schematicInstance;
                layout.instances.emplace(instanceId, instance);
                const auto &childCell = project.layoutCells.at(childId);
                for (auto connection = layout.plannedConnections.begin();
                     connection != layout.plannedConnections.end();) {
                    auto &value = connection->second;
                    bool touches = false;
                    if (value.from == deviceId) {
                        value.from = instanceId;
                        value.fromPort = layoutPortForSchematicPin(childCell, value.fromPin);
                        touches = true;
                    }
                    if (value.to == deviceId) {
                        value.to = instanceId;
                        value.toPort = layoutPortForSchematicPin(childCell, value.toPin);
                        touches = true;
                    }
                    if (touches && value.from == value.to) {
                        connection = layout.plannedConnections.erase(connection);
                        continue;
                    }
                    ++connection;
                }
                layout.plannedDevices.erase(deviceId);
                instanceName = name;
                updateRoutedConnectionPaths(project);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Layout Device"), QString::fromStdString(error.what()));
            return;
        }
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Could not place this device inside itself"));
            return;
        }
        project_->setLastHistoryLabel("Placed layout device " + deviceCopy.name + " as " + instanceName);
        refreshProject();
        if (layoutCanvas_)
            layoutCanvas_->selectLayoutInstance(instanceId);
        statusBar()->showMessage(
            QStringLiteral("Placed %1 as %2; double-click a connection to route it.")
                .arg(QString::fromStdString(deviceCopy.name), QString::fromStdString(instanceName)));
    }

    bool layoutPlannedEndpointIsRoutable(const ice::LayoutCell &cell, ice::ObjectId endpoint,
                                         ice::ObjectId port) const {
        if (const auto pin = cell.ports.find(endpoint); pin != cell.ports.end())
            return pin->second.layer != 0 && pin->second.rectangle.valid();
        if (port == 0 || !project_)
            return false;
        const auto instance = cell.instances.find(endpoint);
        if (instance == cell.instances.end())
            return false;
        const auto child = project_->project().layoutCells.find(instance->second.cell);
        return child != project_->project().layoutCells.end() && child->second.library &&
               !child->second.device.empty() && child->second.ports.contains(port);
    }

    void activateLayoutPlannedConnection(ice::ObjectId connectionId) {
        if (!project_ || connectionId == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto connection = cellIt->second.plannedConnections.find(connectionId);
        if (connection == cellIt->second.plannedConnections.end())
            return;

        const bool fromRoutable =
            layoutPlannedEndpointIsRoutable(cellIt->second, connection->second.from, connection->second.fromPort);
        const bool toRoutable =
            layoutPlannedEndpointIsRoutable(cellIt->second, connection->second.to, connection->second.toPort);
        QMenu menu(this);
        auto *manual = menu.addAction(QStringLiteral("Draw Connection Manually"));
        auto *automatic = menu.addAction(QStringLiteral("Route Automatically"));
        automatic->setEnabled(fromRoutable && toRoutable);
        if (!automatic->isEnabled())
            automatic->setToolTip(
                QStringLiteral("Automatic routing requires placed device pins or layout pins on both ends."));
        auto *chosen = menu.exec(QCursor::pos());
        if (chosen == manual) {
            project_->execute("break planned connection", [&](ice::Project &project) {
                auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell != project.layoutCells.end() && !cell->second.library)
                    cell->second.plannedConnections.erase(connectionId);
                updateRoutedConnectionPaths(project);
            });
            project_->setLastHistoryLabel("Broke layout planned connection");
            refreshProject();
            if (layoutCanvas_) {
                if (layoutPathTool_)
                    layoutPathTool_->setChecked(true);
                layoutCanvas_->setTool(LayoutCanvas::Tool::path);
                updateLayoutPathConfig();
                layoutCanvas_->setFocus();
            }
            statusBar()->showMessage(QStringLiteral("Connection removed; draw it manually with the Path tool."));
            return;
        }
        if (chosen == automatic)
            routeLayoutPlannedConnection(connectionId);
    }

    void routeLayoutPlannedConnection(ice::ObjectId connectionId) {
        if (!project_ || connectionId == 0)
            return;
        const auto &project = project_->project();
        const auto cellIt = project.layoutCells.find(activeLayoutId_);
        if (cellIt == project.layoutCells.end() || cellIt->second.library)
            return;
        const auto connectionIt = cellIt->second.plannedConnections.find(connectionId);
        if (connectionIt == cellIt->second.plannedConnections.end())
            return;
        const auto connection = connectionIt->second;

        std::string routeError;
        const auto routeWidth = std::max<ice::Coordinate>(1, layoutPathWidth_);
        const auto geometry = computeLayoutAutoRoute(project, activeLayoutId_, connection, routeWidth, &routeError);
        if (!geometry) {
            if (routeError == "The two pins already touch; no route is needed.")
                statusBar()->showMessage(QString::fromStdString(routeError));
            else
                QMessageBox::information(this, QStringLiteral("Automatic Routing"), QString::fromStdString(routeError));
            return;
        }
        const auto &points = geometry->points;
        const auto &pointVias = geometry->pointVias;
        const auto common = geometry->layer;

        ice::ObjectId pathId = 0;
        try {
            project_->execute("route planned connection", [&](ice::Project &project) {
                auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                auto shape = ice::Shape{};
                shape.id = project.nextId();
                shape.layer = common;
                if (connection.net != 0)
                    shape.net = connection.net;
                shape.routedConnection = connectionId;
                shape.name = ice::nextShapeName(cell->second, "Routed Path");
                shape.centerline = points;
                shape.segmentLayers = geometry->segmentLayers;
                shape.pointVias = pointVias;
                shape.width = std::max(1, layoutPathWidth_);
                rebuildLayoutPathGeometry(shape);
                pathId = shape.id;
                cell->second.shapes.emplace(shape.id, std::move(shape));
                if (const auto planned = cell->second.plannedConnections.find(connectionId);
                    planned != cell->second.plannedConnections.end())
                    planned->second.routed = true;
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Automatic Routing"), QString::fromStdString(error.what()));
            return;
        }
        if (pathId == 0)
            return;
        project_->setLastHistoryLabel("Routed layout planned connection");
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Automatically routed the connection."));
    }

    void openLayoutInstanceCell(ice::ObjectId instanceId) {
        if (!project_ || instanceId == 0)
            return;
        const auto activeIt = project_->project().layoutCells.find(activeLayoutId_);
        if (activeIt == project_->project().layoutCells.end())
            return;
        const auto instanceIt = activeIt->second.instances.find(instanceId);
        if (instanceIt == activeIt->second.instances.end())
            return;
        const auto cellId = instanceIt->second.cell;
        const auto childIt = project_->project().layoutCells.find(cellId);
        if (childIt == project_->project().layoutCells.end())
            return;
        openLayoutTabs_.insert(cellId);
        activeLayoutId_ = cellId;
        activeProceduralLayoutId_ = 0;
        if (layoutViewStack_ && layoutCanvasPage_)
            layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
        layoutViewInitialized_ = false;
        refreshEmbeddedPanels();
        refreshLayout();
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(QString::fromStdString(childIt->second.name)));
    }

    void openLayoutParentCell() {
        if (!project_)
            return;
        for (const auto &[id, cell] : project_->project().layoutCells) {
            if (id == activeLayoutId_)
                continue;
            for (const auto &[_, instance] : cell.instances) {
                if (instance.cell != activeLayoutId_)
                    continue;
                openLayoutTabs_.insert(id);
                activeLayoutId_ = id;
                activeProceduralLayoutId_ = 0;
                if (layoutViewStack_ && layoutCanvasPage_)
                    layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
                layoutViewInitialized_ = false;
                refreshEmbeddedPanels();
                refreshLayout();
                statusBar()->showMessage(QStringLiteral("Opened %1").arg(QString::fromStdString(cell.name)));
                return;
            }
        }
        statusBar()->showMessage(QStringLiteral("Already at the top of the layout hierarchy"));
    }

    void openSelectedLayoutCell() {
        if (!layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout instance first"));
            return;
        }
        openLayoutInstanceCell(instanceId);
    }

    void updateLayoutPathConfig() {
        if (!project_ || !layoutCanvas_)
            return;
        const auto &project = project_->project();
        const auto layers = layoutLayers(project, activeLayoutId_);
        const auto findLayer = [&](const char *name) -> ice::ObjectId {
            const auto wanted = QString::fromLatin1(name);
            for (const auto &[id, layer] : project.layers) {
                if (!layerAvailableForLayout(project, activeLayoutId_, layer))
                    continue;
                if (QString::fromStdString(layer.name).compare(wanted, Qt::CaseInsensitive) == 0)
                    return id;
            }
            return 0;
        };
        static constexpr std::array<std::pair<const char *, const char *>, 7> steps = {{
            {"Metal1", "Via1"},
            {"Metal2", "Via2"},
            {"Metal3", "Via3"},
            {"Metal4", "Via4"},
            {"Metal5", "TopVia1"},
            {"TopMetal1", "TopVia2"},
            {"TopMetal2", nullptr},
        }};
        LayoutPathConfig config;
        config.width = std::max(1, layoutPathWidth_);
        config.cell = activeLayoutId_;
        for (const auto &[metal, via] : steps) {
            const auto layerId = findLayer(metal);
            if (layerId == 0)
                break;
            const auto viaId = via ? findLayer(via) : 0;
            config.layers.push_back({layerId, viaId, metal});
        }
        if (config.layers.empty() && !layers.empty()) {
            for (const auto &layerRef : layers) {
                const auto &layer = layerRef.get();
                config.layers.push_back({layer.id, 0, layer.name});
            }
        }
        layoutCanvas_->setPathConfig(config);
    }

    void setLayoutPathWidth(int width) {
        if (width <= 0)
            return;
        layoutPathWidth_ = std::min(width, 1000000);
        if (layoutCanvas_) {
            layoutCanvas_->setPathWidth(layoutPathWidth_);
            if (layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
                updateLayoutPathConfig();
        }
        statusBar()->showMessage(QStringLiteral("Path width: %1 nm").arg(layoutPathWidth_));
    }

    static void rebuildLayoutPathGeometry(ice::Shape &shape) {
        if (!shape.isPath() || shape.centerline.size() < 2)
            return;
        std::vector<QPointF> centerline;
        centerline.reserve(shape.centerline.size());
        for (const auto &point : shape.centerline)
            centerline.emplace_back(point.x, -point.y);
        const auto polygon = layoutPathOutline(centerline, shape.width);
        shape.polygon.clear();
        shape.polygon.reserve(static_cast<std::size_t>(polygon.size()));
        ice::Coordinate minX = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate minY = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate maxX = std::numeric_limits<ice::Coordinate>::lowest();
        ice::Coordinate maxY = std::numeric_limits<ice::Coordinate>::lowest();
        for (const auto &point : polygon) {
            const auto x = static_cast<ice::Coordinate>(std::lround(point.x()));
            const auto y = static_cast<ice::Coordinate>(std::lround(-point.y()));
            shape.polygon.push_back({x, y});
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
        if (shape.polygon.size() < 3 || maxX <= minX || maxY <= minY)
            return;
        shape.rectangle = {minX, minY, maxX - minX, maxY - minY};
    }

    static void updateRoutedConnectionPaths(ice::Project &project) {
        for (auto &[cellId, cell] : project.layoutCells) {
            if (cell.library)
                continue;
            std::map<ice::ObjectId, ice::ObjectId> pathByConnection;
            for (auto shape = cell.shapes.begin(); shape != cell.shapes.end();) {
                if (shape->second.routedConnection == 0) {
                    ++shape;
                    continue;
                }
                const auto connectionId = shape->second.routedConnection;
                const auto connection = cell.plannedConnections.find(connectionId);
                if (connection == cell.plannedConnections.end() || !connection->second.routed ||
                    pathByConnection.contains(connectionId)) {
                    shape = cell.shapes.erase(shape);
                    continue;
                }
                pathByConnection.emplace(connectionId, shape->first);
                ++shape;
            }
            for (auto &[connectionId, connection] : cell.plannedConnections) {
                if (!connection.routed)
                    continue;
                const auto path = pathByConnection.find(connectionId);
                if (path == pathByConnection.end()) {
                    connection.routed = false;
                    continue;
                }
                std::string error;
                const auto pathShape = cell.shapes.find(path->second);
                const auto routeWidth = pathShape != cell.shapes.end()
                                            ? std::max<ice::Coordinate>(1, pathShape->second.width)
                                            : std::max<ice::Coordinate>(1, project.grid);
                const auto geometry = computeLayoutAutoRoute(project, cellId, connection, routeWidth, &error);
                if (!geometry) {
                    if (const auto shape = cell.shapes.find(path->second); shape != cell.shapes.end())
                        cell.shapes.erase(shape);
                    connection.routed = false;
                    continue;
                }
                auto &shape = cell.shapes.at(path->second);
                shape.layer = geometry->layer;
                if (connection.net != 0)
                    shape.net = connection.net;
                else
                    shape.net.reset();
                shape.centerline = geometry->points;
                shape.segmentLayers = geometry->segmentLayers;
                shape.pointVias = geometry->pointVias;
                if (shape.width <= 0)
                    shape.width = std::max<ice::Coordinate>(1, project.grid);
                rebuildLayoutPathGeometry(shape);
            }
        }
    }

    void finishActiveLayoutPath() {
        activeLayoutPathId_ = 0;
        activeLayoutPathWidth_ = 0;
    }

    void moveLayoutPathPoint(ice::ObjectId shapeId, int index, QPointF position) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x()));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y()));
        const auto &currentProject = project_->project();
        if (const auto cellIt = currentProject.layoutCells.find(activeLayoutId_);
            cellIt != currentProject.layoutCells.end() && !cellIt->second.library) {
            const auto shapeIt = cellIt->second.shapes.find(shapeId);
            if (shapeIt != cellIt->second.shapes.end() && shapeIt->second.routedConnection != 0) {
                const auto connectionId = shapeIt->second.routedConnection;
                const auto connectionIt = cellIt->second.plannedConnections.find(connectionId);
                if (connectionIt == cellIt->second.plannedConnections.end())
                    return;
                const auto lastIndex = static_cast<int>(shapeIt->second.centerline.size()) - 1;
                if (index != 0 && index != lastIndex)
                    return;
                const bool fromEnd = index == 0;
                const auto endpoint = fromEnd ? connectionIt->second.from : connectionIt->second.to;
                const auto port = fromEnd ? connectionIt->second.fromPort : connectionIt->second.toPort;
                const auto pin = layoutRouteEndpoint(currentProject, cellIt->second, endpoint, port);
                if (!pin)
                    return;
                const auto side = layoutPointSide(pin->rect, ice::LayoutPoint{x, y});
                try {
                    project_->execute("change routed pin side", [&](ice::Project &project) {
                        auto &cell = project.layoutCells.at(activeLayoutId_);
                        const auto connection = cell.plannedConnections.find(connectionId);
                        if (connection == cell.plannedConnections.end())
                            return;
                        (fromEnd ? connection->second.fromSide : connection->second.toSide) = side;
                        updateRoutedConnectionPaths(project);
                    });
                } catch (const ice::ProjectError &error) {
                    statusBar()->showMessage(
                        QStringLiteral("Change routed pin side: %1").arg(QString::fromStdString(error.what())), 5000);
                    return;
                }
                project_->setLastHistoryLabel("Changed layout routed pin side");
                refreshProject();
                return;
            }
        }
        try {
            project_->execute("move layout path point", [&](ice::Project &project) {
                const auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                const auto shape = cell->second.shapes.find(shapeId);
                if (shape == cell->second.shapes.end() || !shape->second.isPath())
                    return;
                if (index < 0 || index >= static_cast<int>(shape->second.centerline.size()))
                    return;
                shape->second.centerline[static_cast<std::size_t>(index)] = {x, y};
                rebuildLayoutPathGeometry(shape->second);
            });
        } catch (const ice::ProjectError &error) {
            statusBar()->showMessage(QStringLiteral("Move path point: %1").arg(QString::fromStdString(error.what())),
                                     5000);
            return;
        }
        refreshProject();
    }

    void insertLayoutPathPoint(ice::ObjectId shapeId, int index, QPointF position) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x()));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y()));
        try {
            project_->execute("insert layout path point", [&](ice::Project &project) {
                const auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                const auto shape = cell->second.shapes.find(shapeId);
                if (shape == cell->second.shapes.end() || !shape->second.isPath())
                    return;
                if (index <= 0 || index >= static_cast<int>(shape->second.centerline.size()))
                    return;
                auto &path = shape->second;
                if (path.segmentLayers.empty() && path.centerline.size() > 1)
                    path.segmentLayers.assign(path.centerline.size() - 1, path.layer);
                if (path.pointVias.empty())
                    path.pointVias.assign(path.centerline.size(), std::vector<ice::ObjectId>{});
                if (!path.segmentLayers.empty() && index - 1 < static_cast<int>(path.segmentLayers.size()))
                    path.segmentLayers.insert(path.segmentLayers.begin() + index - 1, path.segmentLayers[index - 1]);
                // Insert an explicit empty vector: `insert(pos, {})` selects the
                // initializer-list overload and adds nothing, which would leave
                // the via count out of step with the centerline and make the
                // project invalid (and the next edit throw).
                if (path.pointVias.size() == path.centerline.size())
                    path.pointVias.insert(path.pointVias.begin() + index, std::vector<ice::ObjectId>{});
                path.centerline.insert(path.centerline.begin() + index, {x, y});
                rebuildLayoutPathGeometry(path);
            });
        } catch (const ice::ProjectError &error) {
            statusBar()->showMessage(QStringLiteral("Insert path point: %1").arg(QString::fromStdString(error.what())),
                                     5000);
            return;
        }
        refreshProject();
    }

    void deleteLayoutPathPoint(ice::ObjectId shapeId, int index) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        try {
            project_->execute("delete layout path point", [&](ice::Project &project) {
                const auto cell = project.layoutCells.find(activeLayoutId_);
                if (cell == project.layoutCells.end() || cell->second.library)
                    return;
                const auto shape = cell->second.shapes.find(shapeId);
                if (shape == cell->second.shapes.end() || !shape->second.isPath() ||
                    shape->second.centerline.size() <= 2)
                    return;
                if (index < 0 || index >= static_cast<int>(shape->second.centerline.size()))
                    return;
                auto &path = shape->second;
                if (path.segmentLayers.empty() && path.centerline.size() > 1)
                    path.segmentLayers.assign(path.centerline.size() - 1, path.layer);
                if (!path.segmentLayers.empty()) {
                    const auto removeIndex =
                        std::min<std::size_t>(static_cast<std::size_t>(index), path.segmentLayers.size() - 1);
                    path.segmentLayers.erase(path.segmentLayers.begin() + removeIndex);
                }
                if (index < static_cast<int>(path.pointVias.size()))
                    path.pointVias.erase(path.pointVias.begin() + index);
                path.centerline.erase(path.centerline.begin() + index);
                rebuildLayoutPathGeometry(path);
            });
        } catch (const ice::ProjectError &error) {
            statusBar()->showMessage(QStringLiteral("Delete path point: %1").arg(QString::fromStdString(error.what())),
                                     5000);
            return;
        }
        refreshProject();
    }

    void addLayoutPathSegment(QPointF start, QPointF end, int width, ice::ObjectId layerId,
                              const std::vector<ice::ObjectId> &viaLayers) {
        if (!project_ || width <= 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library ||
            !project_->project().layers.contains(layerId) ||
            !layerAvailableForLayout(project_->project(), activeLayoutId_, project_->project().layers.at(layerId)))
            return;
        const auto toModelPoint = [](QPointF point) {
            return ice::LayoutPoint{static_cast<ice::Coordinate>(std::lround(point.x())),
                                    static_cast<ice::Coordinate>(std::lround(-point.y()))};
        };
        const auto startPoint = toModelPoint(start);
        const auto endPoint = toModelPoint(end);
        try {
            project_->execute("draw layout path", [&](ice::Project &project) {
                auto &cell = project.layoutCells.at(activeLayoutId_);
                auto shape = activeLayoutPathId_ == 0 ? cell.shapes.end() : cell.shapes.find(activeLayoutPathId_);
                const bool append =
                    shape != cell.shapes.end() && shape->second.isPath() && activeLayoutPathWidth_ == width;
                if (!append) {
                    const auto shapeId = project.nextId();
                    ice::Shape path{shapeId, layerId, std::nullopt, {}, {}};
                    path.name = ice::nextShapeName(cell, "Path");
                    path.centerline = {startPoint, endPoint};
                    path.segmentLayers = {layerId};
                    path.pointVias = {{}, {}};
                    for (const auto viaLayer : viaLayers) {
                        if (viaLayer != 0 && project.layers.contains(viaLayer) &&
                            layerAvailableForLayout(project, activeLayoutId_, project.layers.at(viaLayer)))
                            path.pointVias.front().push_back(viaLayer);
                    }
                    path.width = width;
                    rebuildLayoutPathGeometry(path);
                    cell.shapes.emplace(shapeId, std::move(path));
                    activeLayoutPathId_ = shapeId;
                    activeLayoutPathCellId_ = activeLayoutId_;
                    activeLayoutPathWidth_ = width;
                } else {
                    activeLayoutPathCellId_ = activeLayoutId_;
                    auto &path = shape->second;
                    if (path.segmentLayers.empty())
                        path.segmentLayers.assign(path.centerline.size() > 0 ? path.centerline.size() - 1 : 0,
                                                  path.layer);
                    if (path.pointVias.size() != path.centerline.size())
                        path.pointVias.resize(path.centerline.size());
                    for (const auto viaLayer : viaLayers) {
                        if (viaLayer == 0 || !project.layers.contains(viaLayer) ||
                            !layerAvailableForLayout(project, activeLayoutId_, project.layers.at(viaLayer)))
                            continue;
                        auto &vias = path.pointVias.back();
                        if (std::find(vias.begin(), vias.end(), viaLayer) == vias.end())
                            vias.push_back(viaLayer);
                    }
                    if (path.centerline.empty() || path.centerline.back().x != endPoint.x ||
                        path.centerline.back().y != endPoint.y) {
                        path.centerline.push_back(endPoint);
                        path.segmentLayers.push_back(layerId);
                        path.pointVias.emplace_back();
                    }
                    rebuildLayoutPathGeometry(path);
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Draw Path"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void selectLayoutShapeItem(ice::ObjectId shapeId) {
        if (layoutCanvas_)
            layoutCanvas_->clearLayoutNetHighlight();
        if (!layoutCanvas_ || shapeId == 0 ||
            (layoutCanvas_->currentTool() != LayoutCanvas::Tool::select &&
             layoutCanvas_->currentTool() != LayoutCanvas::Tool::move))
            return;
        for (auto *item : layoutCanvas_->scene()->items()) {
            if (item->data(0).toULongLong() != shapeId)
                continue;
            const auto kind = item->data(3).toString();
            if (kind == QStringLiteral("resize-handle") || kind == QStringLiteral("polygon-handle") ||
                !item->data(0).isValid())
                continue;
            layoutCanvas_->scene()->clearSelection();
            item->setSelected(true);
            break;
        }
    }

    // Hierarchy selection of a placed instance opens the owning layout and
    // selects the instance group on the canvas. Deferred so the tree rebuild
    // caused by the tab switch never runs inside the tree's own signal.
    void selectLayoutInstanceFromHierarchy(ice::ObjectId ownerCellId, ice::ObjectId instanceId) {
        if (!project_ || ownerCellId == 0 || instanceId == 0)
            return;
        QTimer::singleShot(0, this, [this, ownerCellId, instanceId] {
            if (!project_ || !layoutCanvas_)
                return;
            const auto cell = project_->project().layoutCells.find(ownerCellId);
            if (cell == project_->project().layoutCells.end() || cell->second.library ||
                !cell->second.instances.contains(instanceId))
                return;
            selectLayoutTab(ownerCellId);
            layoutCanvas_->selectLayoutInstance(instanceId);
            refreshLayoutProperties(true);
        });
    }

    void selectLayoutPin(ice::ObjectId cellId, ice::ObjectId pinId) {
        if (!project_ || cellId == 0 || pinId == 0)
            return;
        const auto &project = project_->project();
        const auto cell = project.layoutCells.find(cellId);
        if (cell == project.layoutCells.end() || !cell->second.ports.contains(pinId))
            return;
        selectLayoutTab(cellId);
        QTimer::singleShot(0, this, [this, pinId] { selectLayoutShapeItem(pinId); });
    }

    void setLayoutPinVisible(ice::ObjectId cellId, ice::ObjectId pinId, bool visible) {
        if (!project_ || cellId == 0 || pinId == 0)
            return;
        try {
            project_->execute("set layout pin visibility", [&](ice::Project &project) {
                const auto cell = project.layoutCells.find(cellId);
                if (cell == project.layoutCells.end())
                    return;
                const auto found = cell->second.ports.find(pinId);
                if (found == cell->second.ports.end())
                    return;
                auto port = found->second;
                port.visible = visible;
                ice::updatePort(project, cellId, port);
            });
        } catch (const ice::ProjectError &error) {
            statusBar()->showMessage(QString::fromStdString(error.what()), 5000);
        }
        // The visibility toggle originates from a tree item signal; defer the
        // tree rebuild so Qt is not left holding a deleted item.
        QTimer::singleShot(0, this, [this] { refreshProject(); });
    }

    std::vector<ice::ObjectId> selectedLayoutShapeIds() const {
        std::vector<ice::ObjectId> result;
        if (!project_ || !layoutCanvas_ || activeLayoutId_ == 0)
            return result;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end())
            return result;
        for (auto *item : layoutCanvas_->scene()->selectedItems()) {
            const auto kind = item->data(3).toString();
            if (kind == QStringLiteral("resize-handle") || kind == QStringLiteral("polygon-handle") ||
                !item->data(0).isValid())
                continue;
            const auto id = static_cast<ice::ObjectId>(item->data(0).toULongLong());
            if (id == 0 || !cellIt->second.shapes.contains(id))
                continue;
            if (std::find(result.begin(), result.end(), id) == result.end())
                result.push_back(id);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    void selectLayoutShapeItems(const std::vector<ice::ObjectId> &shapeIds) {
        if (layoutCanvas_)
            layoutCanvas_->clearLayoutNetHighlight();
        if (!layoutCanvas_ || shapeIds.empty() ||
            (layoutCanvas_->currentTool() != LayoutCanvas::Tool::select &&
             layoutCanvas_->currentTool() != LayoutCanvas::Tool::move))
            return;
        layoutCanvas_->scene()->clearSelection();
        for (auto *item : layoutCanvas_->scene()->items()) {
            if (!item->data(0).isValid())
                continue;
            const auto kind = item->data(3).toString();
            if (kind == QStringLiteral("resize-handle") || kind == QStringLiteral("polygon-handle"))
                continue;
            const auto id = static_cast<ice::ObjectId>(item->data(0).toULongLong());
            if (std::find(shapeIds.begin(), shapeIds.end(), id) != shapeIds.end())
                item->setSelected(true);
        }
    }

    std::string layoutNetName(ice::ObjectId netId) const {
        if (!project_ || netId == 0)
            return {};
        const auto findInSchematic = [&](ice::ObjectId schematicId) -> std::string {
            const auto cell = project_->project().schematicCells.find(schematicId);
            if (cell == project_->project().schematicCells.end())
                return {};
            const auto net = cell->second.nets.find(netId);
            return net == cell->second.nets.end() ? std::string{} : net->second.name;
        };
        if (activeLayoutId_ != 0) {
            const auto preferred = ice::schematicForLayout(project_->project(), activeLayoutId_);
            if (preferred != 0) {
                const auto name = findInSchematic(preferred);
                if (!name.empty())
                    return name;
            }
        }
        for (const auto &[cellId, _] : project_->project().schematicCells) {
            const auto name = findInSchematic(cellId);
            if (!name.empty())
                return name;
        }
        return {};
    }

    LayoutNetTrace traceLayoutNet(ice::ObjectId seedShapeId) const {
        LayoutNetTrace trace;
        if (!project_ || activeLayoutId_ == 0)
            return trace;
        const auto cell = project_->project().layoutCells.find(activeLayoutId_);
        if (cell == project_->project().layoutCells.end())
            return trace;
        const auto connectivity = inferLayoutConnectivity(project_->project());
        const auto shapeLayers = [](const ice::Shape &shape) {
            std::set<ice::ObjectId> layers;
            if (shape.isPath()) {
                if (shape.segmentLayers.empty()) {
                    if (shape.layer != 0)
                        layers.insert(shape.layer);
                } else {
                    for (const auto layer : shape.segmentLayers) {
                        if (layer != 0)
                            layers.insert(layer);
                    }
                }
            } else if (shape.layer != 0) {
                layers.insert(shape.layer);
            }
            return layers;
        };

        struct Entry {
            ice::ObjectId id = 0;
            ice::ObjectId primaryLayer = 0;
            std::set<ice::ObjectId> layers;
            bool conductor = false;
            bool via = false;
            QPainterPath path;
            QPainterPath expanded;
            QRectF bounds;
        };
        std::vector<Entry> entries;
        QPainterPathStroker stroker;
        stroker.setWidth(1.0);
        stroker.setJoinStyle(Qt::MiterJoin);
        for (const auto &[id, shape] : cell->second.shapes) {
            if (shape.isText())
                continue;
            auto path = layoutShapeScenePath(shape);
            if (path.isEmpty())
                continue;
            auto layers = shapeLayers(shape);
            bool conductor = false;
            bool via = false;
            for (const auto layer : layers) {
                if (connectivity.conductors.contains(layer))
                    conductor = true;
                if (connectivity.viaConductors.contains(layer))
                    via = true;
            }
            if (!conductor && !via)
                continue;
            Entry entry;
            entry.id = id;
            entry.primaryLayer = shape.layer;
            entry.layers = std::move(layers);
            entry.conductor = conductor;
            entry.via = via;
            entry.bounds = path.boundingRect().adjusted(-1.0, -1.0, 1.0, 1.0);
            entry.expanded = stroker.createStroke(path);
            entry.expanded.addPath(path);
            entry.path = std::move(path);
            entries.push_back(std::move(entry));
        }

        std::size_t seed = entries.size();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].id == seedShapeId) {
                seed = index;
                break;
            }
        }
        if (seed == entries.size()) {
            trace.shapes.insert(seedShapeId);
            trace.layerInStack = false;
            return trace;
        }

        std::vector<std::vector<std::size_t>> conductorNeighbors(entries.size());
        for (std::size_t first = 0; first < entries.size(); ++first) {
            if (!entries[first].conductor)
                continue;
            for (std::size_t second = first + 1; second < entries.size(); ++second) {
                if (!entries[second].conductor)
                    continue;
                bool sharesLayer = false;
                for (const auto layer : entries[first].layers) {
                    if (entries[second].layers.contains(layer)) {
                        sharesLayer = true;
                        break;
                    }
                }
                bool directPair = false;
                if (!sharesLayer) {
                    for (const auto left : entries[first].layers) {
                        for (const auto right : entries[second].layers) {
                            if (connectivity.directPairs.contains({std::min(left, right), std::max(left, right)})) {
                                directPair = true;
                                break;
                            }
                        }
                        if (directPair)
                            break;
                    }
                }
                if (!sharesLayer && !directPair)
                    continue;
                if (!entries[first].bounds.intersects(entries[second].bounds))
                    continue;
                if (!entries[first].expanded.intersects(entries[second].path) &&
                    !entries[second].expanded.intersects(entries[first].path))
                    continue;
                conductorNeighbors[first].push_back(second);
                conductorNeighbors[second].push_back(first);
            }
        }

        std::vector<std::vector<std::size_t>> viaGroup(entries.size());
        std::vector<std::vector<std::size_t>> conductorVias(entries.size());
        for (std::size_t viaIndex = 0; viaIndex < entries.size(); ++viaIndex) {
            if (!entries[viaIndex].via)
                continue;
            const auto allowedLayers = connectivity.viaConductors.find(entries[viaIndex].primaryLayer);
            if (allowedLayers == connectivity.viaConductors.end())
                continue;
            for (std::size_t conductorIndex = 0; conductorIndex < entries.size(); ++conductorIndex) {
                if (!entries[conductorIndex].conductor)
                    continue;
                bool allowed = false;
                for (const auto layer : entries[conductorIndex].layers) {
                    if (allowedLayers->second.contains(layer)) {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed)
                    continue;
                if (!entries[viaIndex].bounds.intersects(entries[conductorIndex].bounds))
                    continue;
                if (!entries[viaIndex].expanded.intersects(entries[conductorIndex].path) &&
                    !entries[conductorIndex].expanded.intersects(entries[viaIndex].path))
                    continue;
                viaGroup[viaIndex].push_back(conductorIndex);
                conductorVias[conductorIndex].push_back(viaIndex);
            }
        }

        std::vector<bool> visited(entries.size(), false);
        std::vector<std::size_t> pending{seed};
        visited[seed] = true;
        while (!pending.empty()) {
            const auto current = pending.back();
            pending.pop_back();
            if (entries[current].conductor) {
                for (const auto neighbor : conductorNeighbors[current]) {
                    if (!visited[neighbor]) {
                        visited[neighbor] = true;
                        pending.push_back(neighbor);
                    }
                }
                for (const auto via : conductorVias[current]) {
                    if (!visited[via]) {
                        visited[via] = true;
                        pending.push_back(via);
                    }
                }
            }
            if (entries[current].via) {
                for (const auto conductor : viaGroup[current]) {
                    if (!visited[conductor]) {
                        visited[conductor] = true;
                        pending.push_back(conductor);
                    }
                }
            }
        }

        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (!visited[index])
                continue;
            trace.shapes.insert(entries[index].id);
            for (const auto layer : entries[index].layers)
                trace.layers.insert(layer);
            if (entries[index].via)
                ++trace.viaShapes;
        }

        for (const auto id : trace.shapes) {
            const auto shape = cell->second.shapes.find(id);
            if (shape == cell->second.shapes.end() || !shape->second.net)
                continue;
            const auto name = layoutNetName(*shape->second.net);
            if (!name.empty()) {
                trace.label = name;
                break;
            }
        }
        if (trace.label.empty()) {
            for (const auto &[_, shape] : cell->second.shapes) {
                if (!shape.isText() || shape.text.empty())
                    continue;
                const QRectF textRect(
                    static_cast<qreal>(shape.rectangle.x),
                    -static_cast<qreal>(shape.rectangle.y) - static_cast<qreal>(shape.rectangle.height),
                    static_cast<qreal>(shape.rectangle.width), static_cast<qreal>(shape.rectangle.height));
                QPainterPath textPath;
                textPath.addRect(textRect);
                for (const auto traceId : trace.shapes) {
                    const auto traced = cell->second.shapes.find(traceId);
                    if (traced == cell->second.shapes.end())
                        continue;
                    const auto path = layoutShapeScenePath(traced->second);
                    if (path.isEmpty())
                        continue;
                    if (path.intersects(textPath) || path.contains(textRect.center())) {
                        trace.label = shape.text;
                        break;
                    }
                }
                if (!trace.label.empty())
                    break;
            }
        }
        const auto plannedName = plannedNetNameForShapes(cell->second, trace.shapes, trace.connections, trace.devices);
        if (trace.label.empty())
            trace.label = plannedName;
        return trace;
    }

    std::string plannedNetNameForShapes(const ice::LayoutCell &cell, const std::set<ice::ObjectId> &shapeIds,
                                        std::set<ice::ObjectId> &connectionIds,
                                        std::set<ice::ObjectId> &deviceIds) const {
        std::vector<QPainterPath> shapePaths;
        shapePaths.reserve(shapeIds.size());
        for (const auto shapeId : shapeIds) {
            const auto shape = cell.shapes.find(shapeId);
            if (shape == cell.shapes.end())
                continue;
            auto path = layoutShapeScenePath(shape->second);
            if (!path.isEmpty())
                shapePaths.push_back(std::move(path));
        }
        if (shapePaths.empty())
            return {};

        QPainterPathStroker hitStroker;
        hitStroker.setWidth(4.0);
        const auto touches = [&](const QPainterPath &path) {
            auto hit = hitStroker.createStroke(path);
            hit.addPath(path);
            return std::any_of(shapePaths.begin(), shapePaths.end(),
                               [&](const QPainterPath &shapePath) { return hit.intersects(shapePath); });
        };

        std::set<ice::ObjectId> touchedDevices;
        for (const auto &[deviceId, device] : cell.plannedDevices) {
            QPainterPath box;
            box.addRect(layoutPlannedDeviceSceneRect(device));
            if (touches(box))
                touchedDevices.insert(deviceId);
        }

        std::set<ice::ObjectId> touchedConnections;
        for (const auto &[connectionId, connection] : cell.plannedConnections) {
            if (connection.routed)
                continue;
            const auto fromRect =
                layoutConnectionEndpointSceneRect(project_->project(), cell, connection.from, connection.fromPort);
            const auto toRect =
                layoutConnectionEndpointSceneRect(project_->project(), cell, connection.to, connection.toPort);
            if (!fromRect || !toRect)
                continue;
            QPainterPath line;
            line.moveTo(layoutPlannedEdgeScenePoint(*fromRect, toRect->center()));
            line.lineTo(layoutPlannedEdgeScenePoint(*toRect, fromRect->center()));
            if (touches(line) || touchedDevices.contains(connection.from) || touchedDevices.contains(connection.to)) {
                touchedConnections.insert(connectionId);
                touchedDevices.insert(connection.from);
                touchedDevices.insert(connection.to);
            }
        }
        if (touchedConnections.empty() && touchedDevices.empty())
            return {};

        std::string netName;
        ice::ObjectId netId = 0;
        for (const auto connectionId : touchedConnections) {
            const auto connection = cell.plannedConnections.find(connectionId);
            if (connection == cell.plannedConnections.end())
                continue;
            const auto net = cell.nets.find(connection->second.net);
            if (net != cell.nets.end() && !net->second.name.empty()) {
                netId = net->first;
                netName = net->second.name;
                break;
            }
        }
        if (netId != 0) {
            for (const auto &[connectionId, connection] : cell.plannedConnections) {
                if (connection.net != netId)
                    continue;
                touchedConnections.insert(connectionId);
                touchedDevices.insert(connection.from);
                touchedDevices.insert(connection.to);
            }
        }
        connectionIds.insert(touchedConnections.begin(), touchedConnections.end());
        deviceIds.insert(touchedDevices.begin(), touchedDevices.end());
        return netName;
    }

    void showNetTraceStack() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Net Trace Layer Stack"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto connectivity = inferLayoutConnectivity(project_->project());
        const auto nameOf = [&](ice::ObjectId layerId) -> QString {
            const auto layer = project_->project().layers.find(layerId);
            return layer == project_->project().layers.end() ? QStringLiteral("#%1").arg(layerId)
                                                             : QString::fromStdString(layer->second.name);
        };
        QStringList conductors;
        for (const auto layer : connectivity.conductors)
            conductors << nameOf(layer);
        conductors.sort(Qt::CaseInsensitive);

        QStringList lines;
        lines << QStringLiteral("Conductive layers: %1")
                     .arg(conductors.isEmpty() ? QStringLiteral("none") : conductors.join(QStringLiteral(", ")));
        lines << QString();
        lines << QStringLiteral("Via connections:");
        bool haveVia = false;
        for (const auto &[viaLayer, allowedLayers] : connectivity.viaConductors) {
            QStringList allowed;
            for (const auto layer : allowedLayers)
                allowed << nameOf(layer);
            allowed.sort(Qt::CaseInsensitive);
            lines << QStringLiteral("  %1: %2").arg(nameOf(viaLayer), allowed.join(QStringLiteral(" <-> ")));
            haveVia = true;
        }
        if (!haveVia)
            lines << QStringLiteral("  none");
        lines << QString();
        lines << QStringLiteral("Direct connections (no via required):");
        if (connectivity.directPairs.empty()) {
            lines << QStringLiteral("  none");
        } else {
            for (const auto &[first, second] : connectivity.directPairs)
                lines << QStringLiteral("  %1 <-> %2").arg(nameOf(first), nameOf(second));
        }
        lines << QString();
        lines << QStringLiteral("The stack is inferred from Metal*/TopMetal*/Via*/TopVia*/Contact layer names. "
                                "Net tracing follows touching shapes and crosses layers through via shapes.");
        QMessageBox::information(this, QStringLiteral("Net Trace Layer Stack"), lines.join(QChar('\n')));
    }

    void highlightLayoutNet(ice::ObjectId shapeId) {
        if (!project_ || activeLayoutId_ == 0 || !layoutCanvas_)
            return;
        const auto cell = project_->project().layoutCells.find(activeLayoutId_);
        if (cell == project_->project().layoutCells.end())
            return;
        const auto shape = cell->second.shapes.find(shapeId);
        if (shape == cell->second.shapes.end())
            return;
        if (shape->second.isText() || shape->second.isPath()) {
            statusBar()->showMessage(QStringLiteral("Select a rectangle or polygon to trace its net"));
            return;
        }

        const auto trace = traceLayoutNet(shapeId);
        std::string label = trace.label;
        if (label.empty() && shape->second.net)
            label = "net " + std::to_string(*shape->second.net);
        if (label.empty())
            label = "Traced net";
        layoutCanvas_->highlightLayoutNet(
            std::vector<ice::ObjectId>(trace.shapes.begin(), trace.shapes.end()),
            std::vector<ice::ObjectId>(trace.connections.begin(), trace.connections.end()),
            std::vector<ice::ObjectId>(trace.devices.begin(), trace.devices.end()));

        const auto layerName = [&](ice::ObjectId layerId) -> std::string {
            const auto layer = project_->project().layers.find(layerId);
            return layer == project_->project().layers.end() ? std::string{} : layer->second.name;
        };
        if (!trace.layerInStack) {
            const auto name = layerName(shape->second.layer);
            statusBar()->showMessage(
                QStringLiteral("Layer %1 is not in the inferred connectivity stack")
                    .arg(name.empty() ? QStringLiteral("#%1").arg(shape->second.layer) : QString::fromStdString(name)));
            return;
        }
        if (trace.shapes.size() <= 1 && trace.connections.empty()) {
            statusBar()->showMessage(QStringLiteral("Net trace: no connected shapes on layer %1")
                                         .arg(QString::fromStdString(layerName(shape->second.layer))));
            return;
        }
        const auto summary = QStringLiteral("%1 shape(s), %2 via shape(s), %3 layer(s), %4 planned connection(s)")
                                 .arg(trace.shapes.size())
                                 .arg(trace.viaShapes)
                                 .arg(trace.layers.size())
                                 .arg(trace.connections.size());
        if (trace.label.empty() && !shape->second.net)
            statusBar()->showMessage(QStringLiteral("Traced net: %1").arg(summary));
        else
            statusBar()->showMessage(QStringLiteral("Traced net %1: %2").arg(QString::fromStdString(label), summary));
    }

    void refreshLayoutBooleanActions() {
        bool enabled = false;
        if (project_ && activeLayoutId_ != 0) {
            const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
            if (cellIt != project_->project().layoutCells.end() && !cellIt->second.library) {
                const auto ids = selectedLayoutShapeIds();
                if (ids.size() >= 2) {
                    enabled = true;
                    std::optional<ice::ObjectId> layer;
                    for (const auto id : ids) {
                        const auto shapeIt = cellIt->second.shapes.find(id);
                        if (shapeIt == cellIt->second.shapes.end() || shapeIt->second.isPath() ||
                            shapeIt->second.isText() || shapeIt->second.textGroup != 0 ||
                            (layer && *layer != shapeIt->second.layer)) {
                            enabled = false;
                            break;
                        }
                        layer = shapeIt->second.layer;
                    }
                }
            }
        }
        for (auto *action : {layoutUnionTool_, layoutIntersectionTool_, layoutDifferenceTool_, layoutXorTool_}) {
            if (action)
                action->setEnabled(enabled);
        }
    }

    void applyLayoutBoolean(ice::BooleanOperation operation) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto shapeIds = selectedLayoutShapeIds();
        if (shapeIds.size() < 2) {
            statusBar()->showMessage(QStringLiteral("Select at least two rectangles or polygons on one layer"));
            return;
        }
        if (std::any_of(shapeIds.begin(), shapeIds.end(), [&](ice::ObjectId id) {
                const auto shape = cellIt->second.shapes.find(id);
                return shape != cellIt->second.shapes.end() && shape->second.textGroup != 0;
            })) {
            statusBar()->showMessage(QStringLiteral("Text polygons cannot be used in Boolean operations"));
            return;
        }

        QString name;
        switch (operation) {
        case ice::BooleanOperation::union_:
            name = QStringLiteral("Union");
            break;
        case ice::BooleanOperation::difference:
            name = QStringLiteral("Difference");
            break;
        case ice::BooleanOperation::intersection:
            name = QStringLiteral("Intersection");
            break;
        case ice::BooleanOperation::xor_:
            name = QStringLiteral("Xor");
            break;
        }

        std::vector<ice::ObjectId> resultIds;
        try {
            project_->execute(name.toLower().toStdString() + " layout shapes", [&](ice::Project &project) {
                resultIds = ice::booleanLayoutShapes(project, activeLayoutId_, shapeIds, operation);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, name, QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
        statusBar()->showMessage(QStringLiteral("%1: %2 shapes → %3 polygon%4")
                                     .arg(name)
                                     .arg(shapeIds.size())
                                     .arg(resultIds.size())
                                     .arg(resultIds.size() == 1 ? QString() : QStringLiteral("s")));
        QTimer::singleShot(0, this, [this, resultIds] { selectLayoutShapeItems(resultIds); });
    }

    void moveLayoutPolygonPoint(ice::ObjectId shapeId, int index, QPointF position) {
        if (!project_ || activeLayoutId_ == 0 || index < 0)
            return;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x()));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y()));
        try {
            project_->execute("move layout polygon point", [&](ice::Project &project) {
                ice::movePolygonPoint(project, activeLayoutId_, shapeId, static_cast<std::size_t>(index), {x, y});
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Edit Polygon"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
        QTimer::singleShot(0, this, [this, shapeId] { selectLayoutShapeItem(shapeId); });
    }

    void resizeLayoutRectangle(ice::ObjectId shapeId, int width, int height) {
        if (!project_ || activeLayoutId_ == 0 || shapeId == 0 || width <= 0 || height <= 0)
            return;
        try {
            project_->execute("resize layout rectangle", [&](ice::Project &project) {
                const auto cellIt = project.layoutCells.find(activeLayoutId_);
                if (cellIt == project.layoutCells.end() || cellIt->second.library)
                    return;
                const auto shapeIt = cellIt->second.shapes.find(shapeId);
                if (shapeIt == cellIt->second.shapes.end() || shapeIt->second.isPath() || shapeIt->second.isText() ||
                    !shapeIt->second.polygon.empty())
                    return;
                const auto grid = std::max<ice::Coordinate>(1, project.grid);
                auto rectangle = shapeIt->second.rectangle;
                const auto requestedWidth = static_cast<ice::Coordinate>(width);
                const auto requestedHeight = static_cast<ice::Coordinate>(height);
                if (requestedWidth != rectangle.width)
                    rectangle.width = std::max(grid, ((requestedWidth + grid - 1) / grid) * grid);
                if (requestedHeight != rectangle.height)
                    rectangle.height = std::max(grid, ((requestedHeight + grid - 1) / grid) * grid);
                ice::resizeRectangle(project, activeLayoutId_, shapeId, rectangle);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Resize Rectangle"), QString::fromStdString(error.what()));
            return;
        }
        QTimer::singleShot(0, this, [this, shapeId] {
            refreshProject();
            selectLayoutShapeItem(shapeId);
        });
    }

    void addLayoutPolygon(const std::vector<QPointF> &points) {
        if (!project_ || activeLayoutId_ == 0 || points.size() < 3)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto availableLayers = layoutLayers(project_->project(), activeLayoutId_);
        if (availableLayers.empty()) {
            QMessageBox::information(this, QStringLiteral("Create Polygon"),
                                     QStringLiteral("Create a layer before drawing polygons."));
            return;
        }
        if (availableLayers.size() > 1 && activeLayoutLayerId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Create Polygon"),
                                     QStringLiteral("Select a layer before drawing polygons."));
            return;
        }
        const auto layerId = activeLayoutLayerId_ != 0 ? activeLayoutLayerId_ : availableLayers.front().get().id;
        std::vector<ice::LayoutPoint> modelPoints;
        modelPoints.reserve(points.size());
        for (const auto &point : points)
            modelPoints.push_back({static_cast<ice::Coordinate>(std::lround(point.x())),
                                   static_cast<ice::Coordinate>(std::lround(-point.y()))});
        ice::ObjectId shapeId = 0;
        try {
            project_->execute("create layout polygon", [&](ice::Project &project) {
                shapeId = ice::addPolygon(project, activeLayoutId_, layerId, modelPoints);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Polygon"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
        if (shapeId != 0)
            QTimer::singleShot(0, this, [this, shapeId] { selectLayoutShapeItem(shapeId); });
    }

    void addLayoutText(QPointF position) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto availableLayers = layoutLayers(project_->project(), activeLayoutId_);
        if (availableLayers.empty()) {
            QMessageBox::information(this, QStringLiteral("Draw Text"),
                                     QStringLiteral("Create a layer before drawing text."));
            return;
        }
        if (availableLayers.size() > 1 && activeLayoutLayerId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Draw Text"),
                                     QStringLiteral("Select a layer before drawing text."));
            return;
        }
        const auto layerId = activeLayoutLayerId_ != 0 ? activeLayoutLayerId_ : availableLayers.front().get().id;
        bool accepted = false;
        const auto text = QInputDialog::getText(this, QStringLiteral("Draw Text"), QStringLiteral("Text:"),
                                                QLineEdit::Normal, QString(), &accepted);
        if (!accepted || text.trimmed().isEmpty())
            return;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x()));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y()));
        const auto grid = std::max<ice::Coordinate>(1, project_->project().grid);
        const auto height = ((layoutTextHeight_ + grid - 1) / grid) * grid;
        QFont font;
        font.setPixelSize(std::max(1, static_cast<int>(height)));
        const QFontMetricsF metrics(font);
        const auto width = std::max<ice::Coordinate>(
            grid, static_cast<ice::Coordinate>(std::ceil(metrics.horizontalAdvance(text) / grid)) * grid);
        try {
            project_->execute("create layout text", [&](ice::Project &project) {
                auto &cell = project.layoutCells.at(activeLayoutId_);
                const auto shapeId = project.nextId();
                ice::Shape shape{shapeId, layerId, std::nullopt, {x, y, width, height}, {}};
                shape.name = ice::nextShapeName(cell, "Text");
                shape.text = text.toStdString();
                cell.shapes.emplace(shapeId, std::move(shape));
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Draw Text"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void addLayoutTextPolygon(QPointF position) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto availableLayers = layoutLayers(project_->project(), activeLayoutId_);
        if (availableLayers.empty()) {
            QMessageBox::information(this, QStringLiteral("Draw Text Polygon"),
                                     QStringLiteral("Create a layer before drawing text polygons."));
            return;
        }
        if (availableLayers.size() > 1 && activeLayoutLayerId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Draw Text Polygon"),
                                     QStringLiteral("Select a layer before drawing text polygons."));
            return;
        }
        const auto layerId = activeLayoutLayerId_ != 0 ? activeLayoutLayerId_ : availableLayers.front().get().id;
        bool accepted = false;
        const auto text = QInputDialog::getText(this, QStringLiteral("Draw Text Polygon"), QStringLiteral("Text:"),
                                                QLineEdit::Normal, QString(), &accepted);
        if (!accepted || text.trimmed().isEmpty())
            return;

        const auto grid = std::max<ice::Coordinate>(1, project_->project().grid);
        const auto height = ((layoutTextHeight_ + grid - 1) / grid) * grid;
        const auto polygons =
            ice::textPolygonGeometry(text, static_cast<int>(height), QPointF(position.x(), -position.y()));
        if (polygons.empty()) {
            QMessageBox::information(this, QStringLiteral("Draw Text Polygon"),
                                     QStringLiteral("The selected text produced no filled polygons."));
            return;
        }

        ice::ObjectId firstShape = 0;
        try {
            project_->execute("create text polygon", [&](ice::Project &project) {
                auto &cell = project.layoutCells.at(activeLayoutId_);
                const auto groupName = ice::nextShapeName(cell, "Text Polygon");
                const auto groupText = text.toStdString();
                for (const auto &points : polygons) {
                    if (points.size() < 3)
                        continue;
                    ice::Coordinate minX = std::numeric_limits<ice::Coordinate>::max();
                    ice::Coordinate minY = std::numeric_limits<ice::Coordinate>::max();
                    ice::Coordinate maxX = std::numeric_limits<ice::Coordinate>::lowest();
                    ice::Coordinate maxY = std::numeric_limits<ice::Coordinate>::lowest();
                    for (const auto &point : points) {
                        minX = std::min(minX, point.x);
                        minY = std::min(minY, point.y);
                        maxX = std::max(maxX, point.x);
                        maxY = std::max(maxY, point.y);
                    }
                    if (maxX <= minX || maxY <= minY)
                        continue;
                    std::vector<ice::LayoutPoint> layoutPoints;
                    layoutPoints.reserve(points.size());
                    for (const auto &point : points)
                        layoutPoints.push_back({point.x, point.y});
                    const auto shapeId = project.nextId();
                    ice::Shape shape{shapeId, layerId, std::nullopt, ice::Rect{minX, minY, maxX - minX, maxY - minY},
                                     std::move(layoutPoints)};
                    shape.name = groupName;
                    shape.textPolygon = true;
                    shape.textSource = groupText;
                    if (firstShape == 0)
                        firstShape = shapeId;
                    shape.textGroup = firstShape;
                    cell.shapes.emplace(shapeId, std::move(shape));
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Draw Text Polygon"), QString::fromStdString(error.what()));
            return;
        }
        if (firstShape == 0) {
            QMessageBox::information(this, QStringLiteral("Draw Text Polygon"),
                                     QStringLiteral("The selected text produced no filled polygons."));
            return;
        }
        refreshProject();
        QTimer::singleShot(0, this, [this, firstShape] { selectLayoutShapeItem(firstShape); });
        if (layoutPropertiesPanel_ && layoutRightTabs_)
            layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
        statusBar()->showMessage(QStringLiteral("Created text polygon on %1")
                                     .arg(QString::fromStdString(project_->project().layers.at(layerId).name)));
    }

    void addLayoutPin(QPointF first, QPointF second) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library)
            return;
        const auto availableLayers = layoutLayers(project_->project(), activeLayoutId_);
        if (availableLayers.empty()) {
            QMessageBox::information(this, QStringLiteral("Create Pin"),
                                     QStringLiteral("Create a layer before drawing pins."));
            return;
        }
        if (availableLayers.size() > 1 && activeLayoutLayerId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Create Pin"),
                                     QStringLiteral("Select a layer before drawing pins."));
            return;
        }
        const auto layerId = activeLayoutLayerId_ != 0 ? activeLayoutLayerId_ : availableLayers.front().get().id;
        const auto rectangle = QRectF(first, second).normalized();
        if (rectangle.width() < 1.0 || rectangle.height() < 1.0)
            return;
        const auto x = static_cast<ice::Coordinate>(std::lround(rectangle.left()));
        const auto y = static_cast<ice::Coordinate>(std::lround(-rectangle.bottom()));
        const auto width = static_cast<ice::Coordinate>(std::lround(rectangle.width()));
        const auto height = static_cast<ice::Coordinate>(std::lround(rectangle.height()));
        std::string name = "PIN1";
        for (int index = 1;; ++index) {
            name = "PIN" + std::to_string(index);
            const bool exists = std::any_of(cellIt->second.ports.begin(), cellIt->second.ports.end(),
                                            [&](const auto &entry) { return entry.second.name == name; });
            if (!exists)
                break;
        }
        ice::ObjectId portId = 0;
        try {
            project_->execute("create layout pin", [&](ice::Project &project) {
                portId = ice::addPort(project, activeLayoutId_, name, layerId, ice::Rect{x, y, width, height},
                                      ice::PortDirection::inout, ice::PortSide::left, 0, name);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Pin"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
        if (portId != 0)
            QTimer::singleShot(0, this, [this, portId] { selectLayoutShapeItem(portId); });
        if (layoutPropertiesPanel_ && layoutRightTabs_)
            layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
        statusBar()->showMessage(QStringLiteral("Created pin %1 on %2")
                                     .arg(QString::fromStdString(name),
                                          QString::fromStdString(project_->project().layers.at(layerId).name)));
    }

    void promoteInstancePin(ice::ObjectId instanceId, ice::ObjectId childPortId) {
        if (!project_ || activeLayoutId_ == 0 || instanceId == 0 || childPortId == 0)
            return;
        const auto &project = project_->project();
        const auto parentIt = project.layoutCells.find(activeLayoutId_);
        if (parentIt == project.layoutCells.end() || parentIt->second.library)
            return;
        const auto instanceIt = parentIt->second.instances.find(instanceId);
        if (instanceIt == parentIt->second.instances.end())
            return;
        const auto childIt = project.layoutCells.find(instanceIt->second.cell);
        if (childIt == project.layoutCells.end())
            return;
        const auto childPortIt = childIt->second.ports.find(childPortId);
        if (childPortIt == childIt->second.ports.end())
            return;
        for (const auto &[_, existing] : parentIt->second.ports) {
            if (existing.sourceInstance == instanceId && existing.sourcePort == childPortId) {
                selectLayoutShapeItem(existing.id);
                return;
            }
        }

        const auto &childPort = childPortIt->second;
        const ice::Transform transform{instanceIt->second.x, instanceIt->second.y, instanceIt->second.orientation};
        const auto first = transform.apply({childPort.rectangle.x, childPort.rectangle.y});
        const auto second = transform.apply(
            {childPort.rectangle.x + childPort.rectangle.width, childPort.rectangle.y + childPort.rectangle.height});
        const ice::Rect rectangle{std::min(first.x, second.x), std::min(first.y, second.y),
                                  std::abs(second.x - first.x), std::abs(second.y - first.y)};
        std::string name = childPort.name;
        for (int suffix = 2;; ++suffix) {
            const bool duplicate = std::any_of(parentIt->second.ports.begin(), parentIt->second.ports.end(),
                                               [&](const auto &entry) { return entry.second.name == name; });
            if (!duplicate)
                break;
            name = childPort.name + "_" + std::to_string(suffix == 2 ? 1 : suffix - 1);
            if (suffix > 1000)
                break;
        }
        ice::ObjectId netId = 0;
        if (childPort.net != 0) {
            const auto childNet = childIt->second.nets.find(childPort.net);
            if (childNet != childIt->second.nets.end()) {
                for (const auto &[id, net] : parentIt->second.nets) {
                    if (net.name == childNet->second.name) {
                        netId = id;
                        break;
                    }
                }
                if (netId == 0) {
                    try {
                        project_->execute("promote layout pin", [&](ice::Project &project) {
                            netId = ice::createLayoutNet(project, activeLayoutId_, childNet->second.name);
                        });
                    } catch (const ice::ProjectError &) {
                        netId = 0;
                    }
                }
            }
        }
        ice::ObjectId portId = 0;
        try {
            project_->execute("promote layout pin", [&](ice::Project &project) {
                portId = ice::addPort(project, activeLayoutId_, name, childPort.layer, rectangle, childPort.direction,
                                      childPort.side, netId, childPort.label);
                auto &port = project.layoutCells.at(activeLayoutId_).ports.at(portId);
                port.sourceInstance = instanceId;
                port.sourcePort = childPortId;
                ice::updatePort(project, activeLayoutId_, port);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Promote Pin"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
        if (portId != 0)
            QTimer::singleShot(0, this, [this, portId] { selectLayoutShapeItem(portId); });
    }

    void removeSchematic(ice::ObjectId cellId) {
        if (!project_ || cellId == 0)
            return;
        const auto &cells = project_->project().schematicCells;
        const auto cell = cells.find(cellId);
        if (cell == cells.end())
            return;
        QStringList simulations;
        for (const auto &[_, simulation] : project_->project().simulations)
            if (simulation.schematic == cellId)
                simulations << QString::fromStdString(simulation.name);
        if (!simulations.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Remove Schematic"),
                QStringLiteral("Schematic '%1' is used by these simulations:\n%2\n\nDelete or reassign them first.")
                    .arg(QString::fromStdString(cell->second.name), simulations.join(QStringLiteral("\n"))));
            return;
        }
        const auto name = QString::fromStdString(cell->second.name);
        if (QMessageBox::question(this, QStringLiteral("Remove Schematic"),
                                  QStringLiteral("Remove schematic '%1'?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("remove schematic", [&](ice::Project &project) { project.schematicCells.erase(cellId); });
        if (activeSchematicId_ == cellId)
            activeSchematicId_ = 0;
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Schematic '%1' removed").arg(name));
    }

    static bool layoutNameExists(const ice::Project &project, const std::string &name, ice::ObjectId exceptCell,
                                 ice::ObjectId exceptProcedural) {
        for (const auto &[id, cell] : project.layoutCells)
            if (id != exceptCell && cell.name == name)
                return true;
        for (const auto &[id, procedural] : project.proceduralLayouts)
            if (id != exceptProcedural && procedural.name == name)
                return true;
        return false;
    }

    void renameLayoutCell(ice::ObjectId cellId, const std::string &name) {
        if (!project_ || cellId == 0 || name.empty())
            return;
        try {
            project_->execute("rename layout", [&](ice::Project &project) {
                const auto cellIt = project.layoutCells.find(cellId);
                if (cellIt == project.layoutCells.end() || cellIt->second.library)
                    return;
                if (layoutNameExists(project, name, cellId, 0))
                    throw ice::ProjectError("layout.name", "A layout with this name already exists");
                cellIt->second.name = name;
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Rename Layout"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void renameProceduralLayout(ice::ObjectId proceduralId, const std::string &name) {
        if (!project_ || proceduralId == 0 || name.empty())
            return;
        try {
            project_->execute("rename procedural layout", [&](ice::Project &project) {
                const auto proceduralIt = project.proceduralLayouts.find(proceduralId);
                if (proceduralIt == project.proceduralLayouts.end())
                    return;
                if (layoutNameExists(project, name, 0, proceduralId))
                    throw ice::ProjectError("layout.name", "A layout with this name already exists");
                proceduralIt->second.name = name;
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Rename Script"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void renameLayoutInstance(ice::ObjectId cellId, ice::ObjectId instanceId, const std::string &name) {
        if (!project_ || cellId == 0 || instanceId == 0 || name.empty())
            return;
        try {
            project_->execute("rename layout instance", [&](ice::Project &project) {
                const auto cellIt = project.layoutCells.find(cellId);
                if (cellIt == project.layoutCells.end() || cellIt->second.library)
                    return;
                const auto instanceIt = cellIt->second.instances.find(instanceId);
                if (instanceIt == cellIt->second.instances.end())
                    return;
                for (const auto &[id, instance] : cellIt->second.instances)
                    if (id != instanceId && instance.name == name)
                        throw ice::ProjectError("layout.instance.name", "An instance with this name already exists");
                instanceIt->second.name = name;
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Rename Instance"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void renameLayoutPlannedDevice(ice::ObjectId cellId, ice::ObjectId deviceId, const std::string &name) {
        if (!project_ || cellId == 0 || deviceId == 0 || name.empty())
            return;
        try {
            project_->execute("rename planned device", [&](ice::Project &project) {
                const auto cellIt = project.layoutCells.find(cellId);
                if (cellIt == project.layoutCells.end() || cellIt->second.library)
                    return;
                const auto deviceIt = cellIt->second.plannedDevices.find(deviceId);
                if (deviceIt == cellIt->second.plannedDevices.end())
                    return;
                for (const auto &[id, device] : cellIt->second.plannedDevices)
                    if (id != deviceId && device.name == name)
                        throw ice::ProjectError("layout.device.name", "A device with this name already exists");
                deviceIt->second.name = name;
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Rename Device"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void renameLayoutShape(ice::ObjectId cellId, ice::ObjectId shapeId, const std::string &name) {
        if (!project_ || cellId == 0 || shapeId == 0 || name.empty())
            return;
        try {
            project_->execute("rename layout shape", [&](ice::Project &project) {
                const auto cellIt = project.layoutCells.find(cellId);
                if (cellIt == project.layoutCells.end() || cellIt->second.library)
                    return;
                const auto shapeIt = cellIt->second.shapes.find(shapeId);
                if (shapeIt == cellIt->second.shapes.end())
                    return;
                if (shapeIt->second.textGroup != 0) {
                    for (auto &[_, shape] : cellIt->second.shapes)
                        if (shape.textGroup == shapeIt->second.textGroup)
                            shape.name = name;
                } else {
                    shapeIt->second.name = name;
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Rename Shape"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
    }

    void removeLayoutCell(ice::ObjectId cellId) {
        if (!project_ || cellId == 0)
            return;
        const auto &cells = project_->project().layoutCells;
        const auto cell = cells.find(cellId);
        if (cell == cells.end())
            return;
        if (cell->second.library) {
            QMessageBox::information(this, QStringLiteral("Remove Layout Cell"),
                                     QStringLiteral("Library cells are read-only and cannot be removed."));
            return;
        }
        QStringList references;
        for (const auto &[otherId, otherCell] : cells) {
            if (otherId == cellId)
                continue;
            for (const auto &[_, instance] : otherCell.instances)
                if (instance.cell == cellId)
                    references << QStringLiteral("%1 in %2")
                                      .arg(QString::fromStdString(instance.name),
                                           QString::fromStdString(otherCell.name));
        }
        if (!references.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Remove Layout Cell"),
                QStringLiteral("Layout cell '%1' is used as an instance in:\n%2\n\nRemove those instances first.")
                    .arg(QString::fromStdString(cell->second.name), references.join(QStringLiteral("\n"))));
            return;
        }
        const auto name = QString::fromStdString(cell->second.name);
        if (QMessageBox::question(this, QStringLiteral("Remove Layout Cell"),
                                  QStringLiteral("Remove layout cell '%1'?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("remove layout cell", [&](ice::Project &project) { project.layoutCells.erase(cellId); });
        if (activeLayoutId_ == cellId)
            activeLayoutId_ = 0;
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout cell '%1' removed").arg(name));
    }

    void removeLayoutItem(ice::ObjectId id) {
        if (!project_ || id == 0)
            return;
        if (project_->project().proceduralLayouts.contains(id))
            removeProceduralLayout(id);
        else
            removeLayoutCell(id);
    }

    void removeProceduralLayout(ice::ObjectId proceduralId) {
        if (!project_ || proceduralId == 0)
            return;
        const auto &project = project_->project();
        const auto proceduralIt = project.proceduralLayouts.find(proceduralId);
        if (proceduralIt == project.proceduralLayouts.end())
            return;
        const auto generatedId = proceduralIt->second.generatedCell;
        QStringList references;
        for (const auto &[otherId, otherCell] : project.layoutCells) {
            if (otherId == generatedId)
                continue;
            for (const auto &[_, instance] : otherCell.instances)
                if (instance.cell == generatedId)
                    references << QStringLiteral("%1 in %2")
                                      .arg(QString::fromStdString(instance.name),
                                           QString::fromStdString(otherCell.name));
        }
        if (!references.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Remove Procedural Layout"),
                QStringLiteral("The generated layout of '%1' is used as an instance in:\n%2\n\nRemove those "
                               "instances first.")
                    .arg(QString::fromStdString(proceduralIt->second.name), references.join(QStringLiteral("\n"))));
            return;
        }
        const auto name = QString::fromStdString(proceduralIt->second.name);
        if (QMessageBox::question(
                this, QStringLiteral("Remove Procedural Layout"),
                QStringLiteral("Remove procedural layout '%1' and its generated static layout?").arg(name),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("remove procedural layout", [&](ice::Project &project) {
            project.proceduralLayouts.erase(proceduralId);
            project.layoutCells.erase(generatedId);
        });
        if (activeProceduralLayoutId_ == proceduralId) {
            activeProceduralLayoutId_ = 0;
            activeLayoutId_ = 0;
            if (layoutViewStack_ && layoutCanvasPage_)
                layoutViewStack_->setCurrentWidget(layoutCanvasPage_);
        }
        if (activeLayoutId_ == generatedId)
            activeLayoutId_ = 0;
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Procedural layout '%1' removed").arg(name));
    }

    void createLayer() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layer"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        int number = 1;
        for (const auto &[_, layer] : project_->project().layers)
            number = std::max(number, layer.number + 1);
        CreateLayerDialog dialog(QStringLiteral("metal1"), number, layerStyleForNumber(number), this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto name = dialog.layerName();
        if (name.isEmpty())
            return;
        const auto color = dialog.color().name(QColor::HexRgb).toStdString();
        const auto pattern = dialog.patternIndex();
        ice::ObjectId created = 0;
        project_->execute("create layer", [&](ice::Project &project) {
            created = ice::addLayer(project, name.toStdString(), number, activeLayoutId_);
            auto &layer = project.layers.at(created);
            layer.color = color;
            layer.pattern = pattern;
        });
        activeLayoutLayerId_ = created;
        project_->setLastHistoryLabel("Created layer " + name.toStdString());
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layer '%1' created").arg(name));
    }

    void managePackages() {
        QSettings settings;
        PackageManagerOptions options;
        options.indexDirectory = configuredPackageIndexDirectory();
        options.installDirectory = configuredPackageInstallDirectory();
        options.projectPath = std::filesystem::path(projectPath_.toStdString());
        options.hasProject = project_ != nullptr;
        options.activatePackage = [this](const ice::PackageSpec &spec) { activatePdkTechnology(spec); };

        PackageManagerDialog dialog(std::move(options), this);
        dialog.exec();
        if (dialog.indexDirectory() == defaultPackageIndexDirectory())
            settings.remove(QStringLiteral("packages/indexDirectory"));
        else
            settings.setValue(QStringLiteral("packages/indexDirectory"),
                              QString::fromStdString(dialog.indexDirectory().string()));
        settings.setValue(QStringLiteral("packages/localRepository"),
                          QString::fromStdString(dialog.installDirectory().string()));
    }

    void activatePdkTechnology(const ice::PackageSpec &spec) {
        if (!project_ || !packageProvidesPdk(spec))
            return;
        const auto displayName = packagePdkDisplayName(spec);
        project_->execute("activate PDK technology", [&](ice::Project &project) {
            if (std::find(project.packageDependencies.begin(), project.packageDependencies.end(), spec.name) ==
                project.packageDependencies.end())
                project.packageDependencies.push_back(spec.name);
            if (spec.pdk) {
                const auto technology = spec.pdk->id.empty() ? spec.name : spec.pdk->id;
                for (const auto &layer : spec.pdk->layers) {
                    const auto exists =
                        std::find_if(project.layers.begin(), project.layers.end(), [&](const auto &entry) {
                            const auto &existing = entry.second;
                            return existing.name == layer.name && existing.layout == 0 &&
                                   (existing.pdk.empty() || existing.pdk == technology);
                        });
                    if (exists == project.layers.end())
                        ice::addLayer(project, layer.name, layer.number, 0, technology);
                }
            }
            if (project.schematicCells.empty()) {
                const auto schematic =
                    spec.pdk && !spec.pdk->defaultSchematic.empty() ? spec.pdk->defaultSchematic : spec.name + "_top";
                ice::createSchematicCell(project, schematic, spec.name);
            }
        });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("%1 technology activated").arg(displayName));
    }

    void refreshLayout() {
        if (!layoutCanvas_ || !project_)
            return;
        updateFileTabDirtyIndicators();
        layoutCanvas_->scene()->clear();
        const auto &project = project_->project();
        layoutCanvas_->setGridSpacing(static_cast<qreal>(project.grid));
        if (gridStatusLabel_)
            gridStatusLabel_->setText(QStringLiteral("Grid: %1").arg(project.grid));
        auto cellIt = project.layoutCells.find(activeLayoutId_);
        if (cellIt == project.layoutCells.end()) {
            if (project.layoutCells.empty()) {
                if (layoutDevicesPanel_)
                    layoutDevicesPanel_->setPdk({});
                refreshLayoutProperties();
                return;
            }
            cellIt = project.layoutCells.begin();
            activeLayoutId_ = cellIt->first;
        }
        if (activeLayoutPathCellId_ != activeLayoutId_) {
            finishActiveLayoutPath();
            activeLayoutPathCellId_ = activeLayoutId_;
        }
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->setPdk(cellIt->second.pdk);
        if (layersPanel_)
            layersPanel_->setProject(project, activeLayoutId_);
        const bool readOnly = cellIt->second.library;
        if (layoutPlaceDeviceTool_)
            layoutPlaceDeviceTool_->setEnabled(!readOnly);
        if (layoutMoveTool_)
            layoutMoveTool_->setEnabled(!readOnly);
        if (layoutRectangleTool_)
            layoutRectangleTool_->setEnabled(!readOnly);
        const auto availableLayers = layoutLayers(project, activeLayoutId_);
        if (layoutPathTool_)
            layoutPathTool_->setEnabled(!readOnly && !availableLayers.empty());
        if (layoutPolygonTool_)
            layoutPolygonTool_->setEnabled(!readOnly && !availableLayers.empty());
        if (layoutTextTool_)
            layoutTextTool_->setEnabled(!readOnly && !availableLayers.empty());
        if (layoutTextPolygonTool_)
            layoutTextPolygonTool_->setEnabled(!readOnly && !availableLayers.empty());
        if (layoutPinTool_)
            layoutPinTool_->setEnabled(!readOnly && !availableLayers.empty());
        if (layoutDeleteTool_)
            layoutDeleteTool_->setEnabled(!readOnly);
        if (layoutCanvas_) {
            layoutCanvas_->setReadOnly(readOnly);
            if (readOnly) {
                if (layoutSelectTool_)
                    layoutSelectTool_->setChecked(true);
                layoutCanvas_->setTool(LayoutCanvas::Tool::select);
            }
        }
        if (layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
            updateLayoutPathConfig();
        const auto styles = layerStylesFor(project);
        layoutCanvas_->setLayerStyles(styles);
        const auto deviceStyles =
            layoutDevicesPanel_ ? ice::LayoutRenderer::deviceStylesFor(project, layoutDevicesPanel_->library(), styles)
                                : ice::LayoutRenderer::DeviceStyleMap{};
        layoutCanvas_->setDeviceLayerStyles(deviceStyles);
        if (layoutPropertiesPanel_)
            layoutPropertiesPanel_->setLayoutPreviewStyles(styles, deviceStyles);
        if (availableLayers.size() == 1)
            activeLayoutLayerId_ = availableLayers.front().get().id;
        else if (!project.layers.contains(activeLayoutLayerId_) ||
                 !layerAvailableForLayout(project, activeLayoutId_, project.layers.at(activeLayoutLayerId_)))
            activeLayoutLayerId_ = 0;
        if (activeLayoutLayerId_ != 0 && styles.contains(activeLayoutLayerId_))
            layoutCanvas_->setRectangleStyle(styles.at(activeLayoutLayerId_));
        if (layersPanel_ && activeLayoutLayerId_ != 0)
            layersPanel_->setSelectedLayer(activeLayoutLayerId_);
        const auto layerVisible = [&](ice::ObjectId layerId) {
            const auto layer = project.layers.find(layerId);
            return layer == project.layers.end() || layer->second.visible;
        };
        std::map<ice::ObjectId, QGraphicsItemGroup *> textPolygonItems;
        for (const auto &[id, shape] : cellIt->second.shapes) {
            if (!layerVisible(shape.layer))
                continue;
            const auto style = styles.contains(shape.layer)
                                   ? styles.at(shape.layer)
                                   : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
            const QPen pen(style.color, 1);
            QBrush brush(QColor(style.color.red(), style.color.green(), style.color.blue(), 110), style.pattern);
            if (style.pattern == Qt::NoBrush)
                brush = QBrush(Qt::NoBrush);
            if (shape.textGroup != 0) {
                if (textPolygonItems.contains(shape.textGroup))
                    continue;
                auto *group = new QGraphicsItemGroup;
                for (const auto &[memberId, member] : cellIt->second.shapes) {
                    if (member.textGroup != shape.textGroup || !layerVisible(member.layer) || member.polygon.size() < 3)
                        continue;
                    const auto memberStyle = styles.contains(member.layer)
                                                 ? styles.at(member.layer)
                                                 : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
                    QPolygonF polygon;
                    polygon.reserve(static_cast<int>(member.polygon.size()));
                    for (const auto &point : member.polygon)
                        polygon << QPointF(point.x, -point.y);
                    auto *child = new QGraphicsPolygonItem(polygon);
                    child->setPen(QPen(memberStyle.color, 1));
                    QBrush memberBrush(
                        QColor(memberStyle.color.red(), memberStyle.color.green(), memberStyle.color.blue(), 110),
                        memberStyle.pattern);
                    if (memberStyle.pattern == Qt::NoBrush)
                        memberBrush = QBrush(Qt::NoBrush);
                    child->setBrush(memberBrush);
                    child->setAcceptedMouseButtons(Qt::NoButton);
                    group->addToGroup(child);
                }
                if (group->childItems().empty()) {
                    delete group;
                    continue;
                }
                group->setData(0, QVariant::fromValue<qulonglong>(shape.textGroup));
                group->setData(3, QStringLiteral("text-polygon"));
                group->setZValue(1.0);
                if (!readOnly) {
                    group->setFlag(QGraphicsItem::ItemIsSelectable);
                    group->setFlag(QGraphicsItem::ItemIsMovable);
                }
                group->setToolTip(QStringLiteral("%1 · %2").arg(QString::fromStdString(shape.name),
                                                                QString::fromStdString(shape.textSource)));
                layoutCanvas_->scene()->addItem(group);
                textPolygonItems.emplace(shape.textGroup, group);
                continue;
            }
            QGraphicsItem *item = nullptr;
            if (shape.isText()) {
                auto *textItem = new QGraphicsSimpleTextItem(QString::fromStdString(shape.text));
                QFont font = textItem->font();
                font.setPixelSize(std::max(1, static_cast<int>(shape.rectangle.height)));
                textItem->setFont(font);
                textItem->setBrush(QBrush(style.color));
                textItem->setPos(shape.rectangle.x, -shape.rectangle.y - shape.rectangle.height);
                textItem->setPen(layoutLabelHaloPen());
                textItem->setZValue(2.0);
                item = textItem;
                layoutCanvas_->scene()->addItem(item);
            } else if (shape.isPath()) {
                std::vector<QPointF> points;
                points.reserve(shape.centerline.size());
                for (const auto &point : shape.centerline)
                    points.emplace_back(point.x, -point.y);
                std::vector<QColor> segmentColors;
                segmentColors.reserve(shape.centerline.size() > 0 ? shape.centerline.size() - 1 : 0);
                for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
                    const auto layerId = shape.layerForSegment(index);
                    const auto segmentStyle = styles.contains(layerId) ? styles.at(layerId) : style;
                    segmentColors.push_back(segmentStyle.color);
                }
                std::vector<std::vector<QColor>> pointViaColors;
                pointViaColors.reserve(shape.pointVias.size());
                for (const auto &vias : shape.pointVias) {
                    std::vector<QColor> colors;
                    colors.reserve(vias.size());
                    for (const auto layerId : vias) {
                        const auto viaStyle = styles.contains(layerId) ? styles.at(layerId) : style;
                        colors.push_back(viaStyle.color);
                    }
                    pointViaColors.push_back(std::move(colors));
                }
                item = createLayoutPathItem(
                    id, points, shape.width, segmentColors, pointViaColors, readOnly, true, shape.routedConnection != 0,
                    [this](ice::ObjectId pathId, int index, QPointF position) {
                        QTimer::singleShot(
                            0, this, [this, pathId, index, position] { moveLayoutPathPoint(pathId, index, position); });
                    },
                    [this](ice::ObjectId pathId, int index, QPointF position) {
                        QTimer::singleShot(0, this, [this, pathId, index, position] {
                            insertLayoutPathPoint(pathId, index, position);
                        });
                    },
                    [this](ice::ObjectId pathId, int index) {
                        QTimer::singleShot(0, this, [this, pathId, index] { deleteLayoutPathPoint(pathId, index); });
                    });
                layoutCanvas_->scene()->addItem(item);
            } else if (shape.polygon.size() >= 3) {
                QPolygonF polygon;
                for (const auto &point : shape.polygon)
                    polygon << QPointF(point.x, -point.y);
                item = layoutCanvas_->scene()->addPolygon(polygon, pen, brush);
            } else {
                item = layoutCanvas_->scene()->addRect(QRectF(shape.rectangle.x,
                                                              -shape.rectangle.y - shape.rectangle.height,
                                                              shape.rectangle.width, shape.rectangle.height),
                                                       pen, brush);
            }
            item->setData(0, QVariant::fromValue<qulonglong>(id));
            if (!readOnly) {
                item->setFlag(QGraphicsItem::ItemIsSelectable);
                item->setFlag(QGraphicsItem::ItemIsMovable);
            }
            if (shape.polygon.empty() && !shape.isText() && !shape.isPath() && !readOnly) {
                if (auto *rectItem = dynamic_cast<QGraphicsRectItem *>(item)) {
                    const auto rect = rectItem->rect();
                    for (int handle = 0; handle < 8; ++handle) {
                        auto *resizeHandle = createLayoutResizeHandleItem(id, handle);
                        resizeHandle->setPos(layoutResizeHandlePosition(rect, handle));
                        layoutCanvas_->scene()->addItem(resizeHandle);
                    }
                }
            }
            if (shape.polygon.size() >= 3 && !shape.isPath() && !shape.isText() && !readOnly) {
                if (auto *polygonItem = dynamic_cast<QGraphicsPolygonItem *>(item)) {
                    const auto polygon = polygonItem->polygon();
                    for (int index = 0; index < polygon.size(); ++index) {
                        auto *vertexHandle = createLayoutPolygonHandleItem(id, index);
                        vertexHandle->setPos(polygon[index]);
                        layoutCanvas_->scene()->addItem(vertexHandle);
                    }
                }
            }
            if (readOnly)
                item->setToolTip(QStringLiteral("Library cell shape %1").arg(id));
            else if (shape.isPath() && shape.routedConnection != 0)
                item->setToolTip(QStringLiteral("Routed path %1").arg(id));
            else
                item->setToolTip(QStringLiteral("Shape %1").arg(id));
        }

        for (const auto &[id, port] : cellIt->second.ports) {
            if (!port.visible || !layerVisible(port.layer))
                continue;
            const auto style = styles.contains(port.layer)
                                   ? styles.at(port.layer)
                                   : LayerStyle{ice::uiColor(ice::UiColor::Accent4), Qt::SolidPattern};
            const QRectF rect(port.rectangle.x, -port.rectangle.y - port.rectangle.height, port.rectangle.width,
                              port.rectangle.height);
            QPen pen(style.color, 1.5);
            pen.setCosmetic(true);
            QColor fill = style.color;
            fill.setAlpha(45);
            auto *item = layoutCanvas_->scene()->addRect(rect, pen, QBrush(fill));
            item->setZValue(6.0);
            item->setData(0, QVariant::fromValue<qulonglong>(id));
            item->setData(2, QStringLiteral("layout-pin"));
            if (!readOnly) {
                item->setFlag(QGraphicsItem::ItemIsSelectable);
                if (!port.locked)
                    item->setFlag(QGraphicsItem::ItemIsMovable);
            }
            auto *label = new QGraphicsSimpleTextItem(QString::fromStdString(port.label), item);
            QFont font = label->font();
            font.setPixelSize(std::max(1, static_cast<int>(std::max<qreal>(1.0, rect.height() * 0.35))));
            label->setFont(font);
            label->setBrush(QBrush(style.color));
            const auto labelBounds = label->boundingRect();
            label->setPos(rect.center().x() - labelBounds.width() / 2.0,
                          rect.center().y() - labelBounds.height() / 2.0);
            label->setPen(layoutLabelHaloPen());
            label->setAcceptedMouseButtons(Qt::NoButton);
            item->setToolTip(QStringLiteral("Pin %1 · %2 · %3")
                                 .arg(QString::fromStdString(port.name),
                                      QString::fromStdString(ice::portDirectionName(port.direction)),
                                      QString::fromStdString(ice::portSideName(port.side))));
            if (!readOnly && !port.locked) {
                for (int handle = 0; handle < 8; ++handle) {
                    auto *resizeHandle = createLayoutResizeHandleItem(id, handle);
                    resizeHandle->setPos(layoutResizeHandlePosition(rect, handle));
                    layoutCanvas_->scene()->addItem(resizeHandle);
                }
            }
        }

        std::vector<ice::ObjectId> ancestors{activeLayoutId_};
        for (const auto &[_, instance] : cellIt->second.instances) {
            const auto childIt = project.layoutCells.find(instance.cell);
            if (childIt == project.layoutCells.end() ||
                std::find(ancestors.begin(), ancestors.end(), instance.cell) != ancestors.end())
                continue;
            auto *group = new QGraphicsItemGroup;
            group->setData(0, QVariant::fromValue<qulonglong>(instance.id));
            group->setData(1, QStringLiteral("layout-instance"));
            if (!readOnly)
                group->setFlag(QGraphicsItem::ItemIsSelectable);
            group->setToolTip(QStringLiteral("Instance %1").arg(QString::fromStdString(instance.name)));
            layoutCanvas_->scene()->addItem(group);
            const ice::Transform transform{instance.x, instance.y, instance.orientation};

            auto childModel =
                ice::buildLayoutCellPreviewModel(project, childIt->second, true, false, false, {transform});
            std::erase_if(childModel.shapes, [&](const ice::LayoutPreviewShape &shape) {
                return shape.layer.id != 0 && !layerVisible(shape.layer.id);
            });
            auto *previewItem = createLayoutPreviewItem(childModel, styles, deviceStyles);
            group->addToGroup(previewItem);
            for (const auto &[portId, port] : childIt->second.ports) {
                if (!port.visible || !layerVisible(port.layer))
                    continue;
                const auto style = styles.contains(port.layer)
                                       ? styles.at(port.layer)
                                       : LayerStyle{ice::uiColor(ice::UiColor::Accent4), Qt::SolidPattern};
                const auto first = transform.apply({port.rectangle.x, port.rectangle.y});
                const auto second = transform.apply(
                    {port.rectangle.x + port.rectangle.width, port.rectangle.y + port.rectangle.height});
                const QRectF pinRect(QPointF(std::min(first.x, second.x), -std::max(first.y, second.y)),
                                     QPointF(std::max(first.x, second.x), -std::min(first.y, second.y)));
                QPen pinPen(style.color, 1.0, Qt::DashLine);
                pinPen.setCosmetic(true);
                QColor pinFill = style.color;
                pinFill.setAlpha(30);
                auto *pinItem = layoutCanvas_->scene()->addRect(pinRect.normalized(), pinPen, QBrush(pinFill));
                pinItem->setZValue(7.0);
                pinItem->setData(3, QStringLiteral("instance-pin"));
                pinItem->setData(4, QVariant::fromValue<qulonglong>(instance.id));
                pinItem->setData(5, QVariant::fromValue<qulonglong>(portId));
                pinItem->setToolTip(QStringLiteral("Instance %1 pin %2 · double-click to promote")
                                        .arg(QString::fromStdString(instance.name), QString::fromStdString(port.name)));
                auto *pinLabel = new QGraphicsSimpleTextItem(QString::fromStdString(port.label), pinItem);
                QFont pinFont = pinLabel->font();
                pinFont.setPixelSize(std::max(1, static_cast<int>(std::max<qreal>(1.0, pinRect.height() * 0.3))));
                pinLabel->setFont(pinFont);
                pinLabel->setBrush(QBrush(style.color));
                const auto pinLabelBounds = pinLabel->boundingRect();
                pinLabel->setPos(pinRect.center().x() - pinLabelBounds.width() / 2.0,
                                 pinRect.center().y() - pinLabelBounds.height() / 2.0);
                pinLabel->setPen(layoutLabelHaloPen());
                pinLabel->setAcceptedMouseButtons(Qt::NoButton);
            }
            const auto bounds = group->childrenBoundingRect();
            if (bounds.isValid() && !bounds.isNull()) {
                auto *highlight = new QGraphicsRectItem(bounds.adjusted(-2, -2, 2, 2), group);
                auto accent = ice::uiColor(ice::UiColor::Accent4);
                QPen pen(accent, 1.5, Qt::DashLine);
                pen.setCosmetic(true);
                highlight->setPen(pen);
                QColor fill = accent;
                fill.setAlpha(25);
                highlight->setBrush(fill);
                highlight->setData(0, QStringLiteral("layout-instance-highlight"));
                highlight->setZValue(1000);
                highlight->setAcceptedMouseButtons(Qt::NoButton);
                highlight->setVisible(false);
            }
        }

        const auto plannedRect = [](const ice::PlannedDevice &device) {
            return QRectF(static_cast<qreal>(device.x),
                          -static_cast<qreal>(device.y) - static_cast<qreal>(device.height),
                          static_cast<qreal>(device.width), static_cast<qreal>(device.height));
        };
        // Stop a connection at the box edge instead of drawing through the box.
        const auto plannedEdge = [](const QRectF &rect, const QPointF &toward) {
            const auto center = rect.center();
            const auto delta = toward - center;
            if (delta.isNull())
                return center;
            const auto scaleX =
                delta.x() != 0.0 ? rect.width() * 0.5 / std::abs(delta.x()) : std::numeric_limits<qreal>::max();
            const auto scaleY =
                delta.y() != 0.0 ? rect.height() * 0.5 / std::abs(delta.y()) : std::numeric_limits<qreal>::max();
            return center + delta * std::min(scaleX, scaleY);
        };
        for (const auto &[connectionId, connection] : cellIt->second.plannedConnections) {
            if (connection.routed)
                continue;
            const auto fromRect = layoutConnectionEndpointSceneRect(project_->project(), cellIt->second,
                                                                    connection.from, connection.fromPort);
            const auto toRect = layoutConnectionEndpointSceneRect(project_->project(), cellIt->second, connection.to,
                                                                  connection.toPort);
            if (!fromRect || !toRect)
                continue;
            const QLineF segment(plannedEdge(*fromRect, toRect->center()), plannedEdge(*toRect, fromRect->center()));
            auto *item = createLayoutPlannedConnectionItem(connectionId, segment, ice::uiColor(ice::UiColor::Accent4));
            layoutCanvas_->scene()->addItem(item);
            if (const auto net = cellIt->second.nets.find(connection.net); net != cellIt->second.nets.end())
                item->setToolTip(QString::fromStdString(net->second.name));
        }
        for (const auto &[deviceId, device] : cellIt->second.plannedDevices) {
            layoutCanvas_->scene()->addItem(createLayoutPlannedDeviceItem(deviceId, QString::fromStdString(device.name),
                                                                          plannedRect(device), readOnly));
        }
        layoutCanvas_->restoreLayoutInstanceSelection();
        layoutCanvas_->refreshInteractionState();
        if (!layoutViewInitialized_) {
            layoutCanvas_->fitToContent();
            layoutViewInitialized_ = true;
        }
        refreshLayoutProperties();
        // The hierarchy is rebuilt on project changes, which drops its current
        // row; restore it from the canvas selection so both stay in step.
        if (layoutHierarchy_)
            layoutHierarchy_->selectInstance(layoutCanvas_->selectedLayoutInstance());
    }

    void clearLayoutDeviceSelection() {
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->clearDeviceSelection();
        selectedLayoutDevice_ = nullptr;
        if (layoutCanvas_)
            layoutCanvas_->setDevicePreview(nullptr);
    }

    void transformSelectedLayoutInstance(ice::Orientation step, const QString &operation) {
        if (!project_ || !layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout device first"));
            return;
        }
        project_->execute(operation.toStdString(), [&](ice::Project &project) {
            const auto cellIt = project.layoutCells.find(activeLayoutId_);
            if (cellIt == project.layoutCells.end() || cellIt->second.library)
                return;
            if (auto instance = cellIt->second.instances.find(instanceId); instance != cellIt->second.instances.end())
                instance->second.orientation = composeOrientation(instance->second.orientation, step);
            updateRoutedConnectionPaths(project);
        });
        if (const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
            cellIt != project_->project().layoutCells.end()) {
            if (const auto instance = cellIt->second.instances.find(instanceId);
                instance != cellIt->second.instances.end())
                project_->setLastHistoryLabel(operation.toStdString() + " " + instance->second.name);
        }
        refreshProject();
    }

    void snapSelectedLayoutInstance() {
        if (!project_ || !layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout device first"));
            return;
        }
        project_->execute("snap layout device", [&](ice::Project &project) {
            const auto cellIt = project.layoutCells.find(activeLayoutId_);
            if (cellIt == project.layoutCells.end() || cellIt->second.library)
                return;
            if (auto instance = cellIt->second.instances.find(instanceId); instance != cellIt->second.instances.end()) {
                instance->second.x = project.snap(instance->second.x);
                instance->second.y = project.snap(instance->second.y);
            }
            updateRoutedConnectionPaths(project);
        });
        if (const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
            cellIt != project_->project().layoutCells.end()) {
            if (const auto instance = cellIt->second.instances.find(instanceId);
                instance != cellIt->second.instances.end())
                project_->setLastHistoryLabel("Snapped " + instance->second.name + " to grid");
        }
        refreshProject();
    }

    void rotateSelectedLayoutFigures() {
        if (!project_ || !layoutCanvas_)
            return;
        std::set<ice::ObjectId> ids;
        for (auto *item : layoutCanvas_->scene()->selectedItems()) {
            if (item->data(3).toString() == QStringLiteral("resize-handle") ||
                item->data(1).toString() == QStringLiteral("layout-instance"))
                continue;
            const auto id = static_cast<ice::ObjectId>(item->data(0).toULongLong());
            if (id != 0)
                ids.insert(id);
        }
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (ids.empty() && instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout figure first"));
            return;
        }
        project_->execute("rotate layout figures", [&](ice::Project &project) {
            auto cellIt = project.layoutCells.find(activeLayoutId_);
            if (cellIt == project.layoutCells.end() || cellIt->second.library)
                return;
            if (instanceId != 0) {
                if (auto it = cellIt->second.instances.find(instanceId); it != cellIt->second.instances.end())
                    it->second.orientation = composeOrientation(it->second.orientation, ice::Orientation::r90);
            }
            for (const auto id : ids) {
                if (auto shapeIt = cellIt->second.shapes.find(id); shapeIt != cellIt->second.shapes.end()) {
                    auto &shape = shapeIt->second;
                    ice::Coordinate minX = shape.rectangle.x, maxX = shape.rectangle.x + shape.rectangle.width;
                    ice::Coordinate minY = shape.rectangle.y, maxY = shape.rectangle.y + shape.rectangle.height;
                    if (!shape.polygon.empty()) {
                        minX = maxX = shape.polygon.front().x;
                        minY = maxY = shape.polygon.front().y;
                        for (const auto &point : shape.polygon) {
                            minX = std::min(minX, point.x);
                            maxX = std::max(maxX, point.x);
                            minY = std::min(minY, point.y);
                            maxY = std::max(maxY, point.y);
                        }
                    }
                    const auto centerX = (minX + maxX) * 0.5;
                    const auto centerY = (minY + maxY) * 0.5;
                    if (shape.isText()) {
                        // Layout text stays horizontal; rotate its anchor with
                        // the selected group without swapping its font size.
                        const auto textCenterX = shape.rectangle.x + shape.rectangle.width * 0.5;
                        const auto textCenterY = shape.rectangle.y + shape.rectangle.height * 0.5;
                        const auto x = textCenterX - centerX;
                        const auto y = textCenterY - centerY;
                        shape.rectangle.x = centerX - y - shape.rectangle.width * 0.5;
                        shape.rectangle.y = centerY + x - shape.rectangle.height * 0.5;
                    } else if (shape.polygon.empty()) {
                        std::swap(shape.rectangle.width, shape.rectangle.height);
                        shape.rectangle.x = centerX - shape.rectangle.width * 0.5;
                        shape.rectangle.y = centerY - shape.rectangle.height * 0.5;
                    } else {
                        for (auto &point : shape.polygon) {
                            const auto x = point.x - centerX;
                            const auto y = point.y - centerY;
                            point.x = centerX - y;
                            point.y = centerY + x;
                        }
                    }
                } else if (auto device = cellIt->second.plannedDevices.find(id);
                           device != cellIt->second.plannedDevices.end()) {
                    const auto centerX = device->second.x + device->second.width * 0.5;
                    const auto centerY = device->second.y + device->second.height * 0.5;
                    std::swap(device->second.width, device->second.height);
                    device->second.x = centerX - device->second.width * 0.5;
                    device->second.y = centerY - device->second.height * 0.5;
                }
            }
            updateRoutedConnectionPaths(project);
        });
        project_->setLastHistoryLabel("Rotated layout figures");
        refreshProject();
    }

    void refreshLayoutProperties(bool bringToFront = false) {
        if (layoutCanvas_)
            layoutCanvas_->clearLayoutNetHighlight();
        refreshLayoutBooleanActions();
        if (!layoutPropertiesPanel_)
            return;
        if (layoutCanvas_ && layoutCanvas_->currentTool() == LayoutCanvas::Tool::path) {
            layoutPropertiesPanel_->setPathTool(layoutCanvas_->pathWidth(), layoutCanvas_->pathLayerName());
            if (bringToFront && layoutRightTabs_)
                layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
            return;
        }
        const auto previewModelFor = [&](const ice::LayoutCell &cell) {
            auto model = ice::buildLayoutCellPreviewModel(project_->project(), cell);
            const auto layerVisible = [&](ice::ObjectId layerId) {
                const auto layer = project_->project().layers.find(layerId);
                return layer == project_->project().layers.end() || layer->second.visible;
            };
            std::erase_if(model.shapes, [&](const ice::LayoutPreviewShape &shape) {
                return shape.layer.id != 0 && !layerVisible(shape.layer.id);
            });
            std::erase_if(model.ports, [&](const ice::LayoutPreviewPort &port) {
                return port.layer.id != 0 && !layerVisible(port.layer.id);
            });
            return model;
        };
        if (project_ && layoutCanvas_ &&
            (layoutCanvas_->currentTool() == LayoutCanvas::Tool::select ||
             layoutCanvas_->currentTool() == LayoutCanvas::Tool::move)) {
            const auto instanceId = layoutCanvas_->selectedLayoutInstance();
            if (instanceId != 0) {
                const auto activeIt = project_->project().layoutCells.find(activeLayoutId_);
                if (activeIt != project_->project().layoutCells.end()) {
                    const auto instanceIt = activeIt->second.instances.find(instanceId);
                    if (instanceIt != activeIt->second.instances.end()) {
                        const auto childIt = project_->project().layoutCells.find(instanceIt->second.cell);
                        if (childIt != project_->project().layoutCells.end()) {
                            layoutPropertiesPanel_->setLayoutCell(childIt->second, previewModelFor(childIt->second));
                            if (bringToFront && layoutRightTabs_)
                                layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                            return;
                        }
                    }
                }
            }

            const auto activeIt = project_->project().layoutCells.find(activeLayoutId_);
            if (activeIt != project_->project().layoutCells.end()) {
                ice::ObjectId selectedPortId = 0;
                bool multiplePorts = false;
                for (auto *item : layoutCanvas_->scene()->selectedItems()) {
                    if (item->data(3).toString() == QStringLiteral("resize-handle") ||
                        item->data(3).toString() == QStringLiteral("polygon-handle"))
                        continue;
                    const auto id = static_cast<ice::ObjectId>(item->data(0).toULongLong());
                    if (id == 0 || !activeIt->second.ports.contains(id))
                        continue;
                    if (selectedPortId != 0 && selectedPortId != id) {
                        multiplePorts = true;
                        break;
                    }
                    selectedPortId = id;
                }
                if (!multiplePorts && selectedPortId != 0) {
                    const auto &port = activeIt->second.ports.at(selectedPortId);
                    QString layerName;
                    if (const auto layerIt = project_->project().layers.find(port.layer);
                        layerIt != project_->project().layers.end())
                        layerName = QString::fromStdString(layerIt->second.name);
                    QString netName;
                    if (const auto netIt = activeIt->second.nets.find(port.net); netIt != activeIt->second.nets.end())
                        netName = QString::fromStdString(netIt->second.name);
                    layoutPropertiesPanel_->setLayoutPort(port, layerName.toStdString(), netName.toStdString());
                    if (bringToFront && layoutRightTabs_)
                        layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                    return;
                }
                ice::ObjectId selectedShapeId = 0;
                bool multipleShapes = false;
                for (auto *item : layoutCanvas_->scene()->selectedItems()) {
                    const auto kind = item->data(3).toString();
                    if (kind == QStringLiteral("resize-handle") || kind == QStringLiteral("polygon-handle"))
                        continue;
                    const auto id = static_cast<ice::ObjectId>(item->data(0).toULongLong());
                    if (id == 0 || !activeIt->second.shapes.contains(id))
                        continue;
                    if (selectedShapeId != 0 && selectedShapeId != id) {
                        multipleShapes = true;
                        break;
                    }
                    selectedShapeId = id;
                }
                if (!multipleShapes && selectedShapeId != 0) {
                    const auto shapeIt = activeIt->second.shapes.find(selectedShapeId);
                    if (shapeIt != activeIt->second.shapes.end()) {
                        const auto &shape = shapeIt->second;
                        QString typeName;
                        if (shape.textGroup != 0)
                            typeName = QStringLiteral("Text Polygon");
                        else if (shape.isText())
                            typeName = QStringLiteral("Text");
                        else if (shape.isPath())
                            typeName =
                                shape.routedConnection != 0 ? QStringLiteral("Routed Path") : QStringLiteral("Path");
                        else if (!shape.polygon.empty())
                            typeName = QStringLiteral("Polygon");
                        else
                            typeName = QStringLiteral("Rectangle");
                        const auto displayName = shape.name.empty()
                                                     ? QStringLiteral("%1 #%2").arg(typeName).arg(selectedShapeId)
                                                     : QString::fromStdString(shape.name);
                        QString layerName;
                        if (const auto layerIt = project_->project().layers.find(shape.layer);
                            layerIt != project_->project().layers.end())
                            layerName = QString::fromStdString(layerIt->second.name);
                        layoutPropertiesPanel_->setLayoutShape(shape, layerName.toStdString(),
                                                               displayName.toStdString(), typeName.toStdString());
                        if (bringToFront && layoutRightTabs_)
                            layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                        return;
                    }
                }
            }
        }
        if (layoutLeftTabs_ && layoutDevicesPanel_ && layoutLeftTabs_->currentWidget() == layoutDevicesPanel_ &&
            selectedLayoutDevice_) {
            layoutPropertiesPanel_->setLayoutDevice(selectedLayoutDevice_);
            return;
        }
        if (!project_) {
            layoutPropertiesPanel_->clearSelection();
            return;
        }
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt != project_->project().layoutCells.end()) {
            layoutPropertiesPanel_->setLayoutOverview(cellIt->second, previewModelFor(cellIt->second),
                                                      static_cast<int>(project_->project().grid));
            if (bringToFront && layoutRightTabs_)
                layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
        } else {
            layoutPropertiesPanel_->clearSelection();
        }
    }

    void placeComment(QPointF position) {
        if (!project_ || project_->project().schematicCells.empty())
            return;
        bool accepted = false;
        const auto text = QInputDialog::getText(this, QStringLiteral("Place Comment"), QStringLiteral("Comment text:"),
                                                QLineEdit::Normal, QString(), &accepted);
        if (!accepted || text.trimmed().isEmpty())
            return;
        const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                ? activeSchematicId_
                                : project_->project().schematicCells.begin()->first;
        project_->execute("place comment", [&](ice::Project &project) {
            auto &cell = project.schematicCells.at(cellId);
            const auto id = project.nextId();
            cell.comments.emplace(id, ice::SchematicComment{id, text.trimmed().toStdString(),
                                                            static_cast<ice::Coordinate>(position.x()),
                                                            static_cast<ice::Coordinate>(-position.y())});
        });
        refreshProject();
    }

    void renameSchematicNet(ice::ObjectId netId) {
        if (!project_ || netId == 0)
            return;
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            cellIt = project_->project().schematicCells.begin();
        if (cellIt == project_->project().schematicCells.end())
            return;
        const auto netIt = cellIt->second.nets.find(netId);
        if (netIt == cellIt->second.nets.end())
            return;
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Rename Net Label"), QStringLiteral("Net name:"),
                                  QLineEdit::Normal, QString::fromStdString(netIt->second.name), &accepted)
                .trimmed();
        if (!accepted || name.isEmpty() || name.toStdString() == netIt->second.name)
            return;
        const auto cellId = cellIt->first;
        bool changed = false;
        project_->execute("rename net label", [&](ice::Project &project) {
            auto &cell = project.schematicCells.at(cellId);
            auto net = cell.nets.find(netId);
            if (net == cell.nets.end())
                return;
            net->second.name = name.toStdString();
            changed = true;
        });
        if (!changed)
            return;
        project_->setLastHistoryLabel("Renamed net label to " + name.toStdString());
        refreshProject();
    }

    void placeNetLabel(QPointF position) {
        if (!project_ || project_->project().schematicCells.empty())
            return;
        position = nearestWireLabelPosition(position);
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Place Net Label"), QStringLiteral("Net name:"),
                                                QLineEdit::Normal, QStringLiteral("NET1"), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                ? activeSchematicId_
                                : project_->project().schematicCells.begin()->first;
        project_->execute("place net label", [&](ice::Project &project) {
            auto &cell = project.schematicCells.at(cellId);
            const auto id = project.nextId();
            cell.nets.emplace(id, ice::Net{id,
                                           name.trimmed().toStdString(),
                                           {},
                                           static_cast<ice::Coordinate>(position.x()),
                                           static_cast<ice::Coordinate>(-position.y())});
        });
        refreshProject();
    }

    QPointF nearestWireLabelPosition(QPointF position) const {
        if (!schematicCanvas_ || !schematicCanvas_->scene())
            return position;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        QPointF bestPosition = position;
        for (auto *item : schematicCanvas_->scene()->items()) {
            auto *wire = dynamic_cast<QGraphicsPathItem *>(item);
            if (!wire || wire->data(0).toString() != QStringLiteral("wire"))
                continue;
            const auto path = wire->path();
            for (int index = 1; index < path.elementCount(); ++index) {
                const auto first = path.elementAt(index - 1);
                const auto second = path.elementAt(index);
                const QPointF start(first.x, first.y);
                const QPointF end(second.x, second.y);
                const auto delta = end - start;
                const auto lengthSquared = QPointF::dotProduct(delta, delta);
                const auto fraction = lengthSquared > 0.0
                                          ? std::clamp(QPointF::dotProduct(position - start, delta) / lengthSquared,
                                                       qreal(0.0), qreal(1.0))
                                          : qreal(0.0);
                const auto candidate = start + delta * fraction;
                const auto distance = QLineF(position, candidate).length();
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestPosition = candidate;
                }
            }
        }
        if (bestDistance < std::numeric_limits<qreal>::max())
            return bestPosition + QPointF(0.0, -18.0);

        // If a wire is not currently rendered, keep the label near the
        // closest visible pin rather than falling back to the canvas origin.
        for (auto *item : schematicCanvas_->scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            const auto distance = QLineF(position, item->scenePos()).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPosition = item->scenePos() + QPointF(0.0, -18.0);
            }
        }
        if (bestDistance < std::numeric_limits<qreal>::max())
            return bestPosition;
        return position;
    }

    std::optional<SymbolDefinition> chooseSchematicPlacementDevice() {
        if (!project_ || project_->project().schematicCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Place Device"),
                                     QStringLiteral("Create a project and schematic first."));
            return std::nullopt;
        }
        try {
            const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                    ? activeSchematicId_
                                    : project_->project().schematicCells.begin()->first;
            const auto &cell = project_->project().schematicCells.at(cellId);
            const auto pdk = cell.pdk.empty() ? std::string(ice::genericPdkId) : cell.pdk;
            std::vector<SymbolDefinition> pdkSymbols;
            if (pdk != ice::genericPdkId) {
                const auto symbolDirectory = configuredPackageInstallDirectory() / pdk / "symbols";
                if (!directoryHasSymbols(symbolDirectory)) {
                    QMessageBox::warning(
                        this, QStringLiteral("Package PDK symbols"),
                        QStringLiteral("The PDK package %1 is not installed or does not provide schematic symbols. "
                                       "Install or update it from Manage Packages before placing symbols.")
                            .arg(QString::fromStdString(pdk)));
                    return std::nullopt;
                }
                pdkSymbols = packageSymbolLibrary(symbolDirectory);
                if (pdkSymbols.empty()) {
                    QMessageBox::warning(
                        this, QStringLiteral("Package PDK symbols"),
                        QStringLiteral("The PDK package %1 does not provide a valid schematic symbol library.")
                            .arg(QString::fromStdString(pdk)));
                    return std::nullopt;
                }
            }
            SymbolLibraryDialog dialog(genericSymbolLibrary(), std::move(pdkSymbols), this);
            if (dialog.exec() != QDialog::Accepted)
                return std::nullopt;
            return dialog.selected();
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
            return std::nullopt;
        }
    }

    void placeSymbol() {
        if (documents_ && layoutPage_ && documents_->currentWidget() == layoutPage_) {
            if (layoutLeftTabs_ && layoutDevicesPanel_)
                layoutLeftTabs_->setCurrentWidget(layoutDevicesPanel_);
            if (layoutDevicesPanel_)
                layoutDevicesPanel_->focusDeviceList();
            if (layoutPlaceDeviceTool_)
                layoutPlaceDeviceTool_->setChecked(true);
            if (layoutCanvas_) {
                layoutCanvas_->setTool(LayoutCanvas::Tool::placeDevice);
                layoutCanvas_->setFocus();
            }
            if (!selectedLayoutDevice_)
                statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
            else
                statusBar()->showMessage(QStringLiteral("Click in the layout to place %1")
                                             .arg(QString::fromStdString(selectedLayoutDevice_->name)));
            return;
        }
        if (const auto definition = chooseSchematicPlacementDevice())
            activateSchematicPlaceSymbolTool(*definition, true);
    }

    // Returns the id of the newly placed instance, or 0 when nothing was placed.
    ice::ObjectId placeSymbolDefinition(const SymbolDefinition &definition, QPointF position) {
        if (!project_ || !schematicCanvas_ || project_->project().schematicCells.empty())
            return 0;
        try {
            const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                    ? activeSchematicId_
                                    : project_->project().schematicCells.begin()->first;
            std::string placedName;
            ice::ObjectId placedInstanceId = 0;
            project_->execute("place symbol", [&](ice::Project &project) {
                const auto symbolId = ice::addSymbol(project, cellId, definition.name.toStdString(),
                                                     definition.svg.toStdString(), definition.cell);
                auto &symbol = project.schematicCells.at(cellId).symbols.at(symbolId);
                symbol.label = definition.label;
                for (auto pin : definition.pins) {
                    pin.id = project.nextId();
                    pin.direction = pin.x < 0   ? ice::PinDirection::left
                                    : pin.x > 0 ? ice::PinDirection::right
                                    : pin.y > 0 ? ice::PinDirection::up
                                                : ice::PinDirection::down;
                    symbol.pins.push_back(std::move(pin));
                }
                const auto instanceName =
                    nextInstanceName(project.schematicCells.at(cellId), definition.name.toStdString());
                placedName = instanceName;
                const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, instanceName,
                                                                  static_cast<ice::Coordinate>(position.x()),
                                                                  static_cast<ice::Coordinate>(-position.y()));
                // Seed the symbol's default parameters (resistance, capacitance,
                // ...) so they are editable in the properties panel right away.
                auto &placedInstance = project.schematicCells.at(cellId).instances.at(instanceId);
                ice::seedDefaultParameters(project, placedInstance, instanceDefaults(definition));
                placedInstanceId = instanceId;
            });
            project_->setLastHistoryLabel("Placed " + definition.name.toStdString() + " " + placedName);
            refreshProject();
            return placedInstanceId;
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
            return 0;
        }
    }

    // Selection is view state, but it is discussed with the domain model so the
    // status bar and properties panel always describe the selected instance.
    void updateSelectionStatus() {
        const auto selectedIds =
            schematicCanvas_ ? schematicCanvas_->selectedInstanceIds() : std::vector<ice::ObjectId>{};
        if (!selectionStatus_)
            return;
        if (selectedIds.empty()) {
            selectionStatus_->setText(QStringLiteral("Selection: none"));
            if (!schematicPropertiesPanel_)
                return;
            if (!project_ || project_->project().schematicCells.empty()) {
                schematicPropertiesPanel_->clearSelection();
                return;
            }
            const auto &cells = project_->project().schematicCells;
            auto cell = cells.find(activeSchematicId_);
            if (cell == cells.end())
                cell = cells.begin();
            if (cell == cells.end()) {
                schematicPropertiesPanel_->clearSelection();
                return;
            }
            schematicPropertiesPanel_->setSchematic(cell->second, generatedSchematicSymbol(cell->second).svg);
            return;
        }
        if (selectedIds.size() > 1) {
            selectionStatus_->setText(QStringLiteral("Selection: %1 items").arg(selectedIds.size()));
            if (schematicPropertiesPanel_)
                schematicPropertiesPanel_->clearSelection();
            return;
        }
        const auto instanceId = selectedIds.front();
        if (!project_ || !schematicPropertiesPanel_) {
            selectionStatus_->setText(QStringLiteral("Selection: none"));
            return;
        }
        for (const auto &[_, cell] : project_->project().schematicCells) {
            const auto comm = cell.comments.find(instanceId);
            if (comm != cell.comments.end()) {
                selectionStatus_->setText(
                    QStringLiteral("Selection: Comment \"%1\"").arg(QString::fromStdString(comm->second.text)));
                schematicPropertiesPanel_->clearSelection();
                return;
            }
            const auto instance = cell.instances.find(instanceId);
            if (instance == cell.instances.end())
                continue;
            const auto symbol = cell.symbols.find(instance->second.symbol);
            const auto symbolName = symbol == cell.symbols.end() ? std::string() : symbol->second.name;
            const auto symbolSvg = symbol == cell.symbols.end() ? std::string() : symbol->second.artwork;
            const auto type = symbolName.empty() ? QStringLiteral("Schematic") : QString::fromStdString(symbolName);
            selectionStatus_->setText(
                QStringLiteral("Selection: %1 (%2)").arg(QString::fromStdString(instance->second.name), type));
            schematicPropertiesPanel_->setInstance(instance->second, symbolName, symbolSvg);
            return;
        }
        selectionStatus_->setText(QStringLiteral("Selection: none"));
        schematicPropertiesPanel_->clearSelection();
    }

    void updateSchematicNetlistState() {
        if (!schematicNetlistPanel_)
            return;
        if (!project_ || project_->project().schematicCells.empty()) {
            schematicNetlistPanel_->clear();
            return;
        }
        const auto &cells = project_->project().schematicCells;
        auto cell = cells.find(activeSchematicId_);
        if (cell == cells.end())
            cell = cells.begin();
        if (cell != cells.end())
            schematicNetlistPanel_->setCell(project_->project(), &cell->second);
    }

    void updateInstanceParameter(ice::ObjectId instanceId, ice::ObjectId parameterId, const std::string &name,
                                 const std::string &value) {
        if (!project_)
            return;
        try {
            project_->execute("edit parameter", [&](ice::Project &project) {
                for (auto &[_, cell] : project.schematicCells) {
                    const auto instance = cell.instances.find(instanceId);
                    if (instance == cell.instances.end())
                        continue;
                    for (auto &parameter : instance->second.parameters) {
                        if ((parameterId != 0 && parameter.id == parameterId) ||
                            (parameterId == 0 && parameter.name == name)) {
                            parameter.value = value;
                            return;
                        }
                    }
                    return;
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Edit Parameter"), QString::fromStdString(error.what()));
            updateSelectionStatus();
        }
        updateSchematicNetlistState();
        updateSimulationNetlistState();
        // The parameter callback runs while the properties table is still
        // handling the edit, so redraw the schematic after that event returns.
        QTimer::singleShot(0, this, [this] {
            if (project_)
                refreshSchematic();
        });
    }

    void updateSchematicParameter(ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name,
                                  const std::string &value) {
        if (!project_)
            return;
        try {
            project_->execute("edit schematic parameter", [&](ice::Project &project) {
                const auto cell = project.schematicCells.find(cellId);
                if (cell == project.schematicCells.end())
                    return;
                for (auto &parameter : cell->second.parameters) {
                    if ((parameterId != 0 && parameter.id == parameterId) ||
                        (parameterId == 0 && parameter.name == name)) {
                        parameter.value = value;
                        return;
                    }
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Edit Parameter"), QString::fromStdString(error.what()));
            updateSelectionStatus();
        }
    }

    void updateLayoutPort(const ice::Port &port, const std::string &netName) {
        if (!project_ || activeLayoutId_ == 0)
            return;
        try {
            project_->execute("edit layout pin", [&](ice::Project &project) {
                auto &cell = project.layoutCells.at(activeLayoutId_);
                auto edited = port;
                if (!netName.empty()) {
                    edited.net = 0;
                    for (const auto &[id, net] : cell.nets) {
                        if (net.name == netName) {
                            edited.net = id;
                            break;
                        }
                    }
                    if (edited.net == 0)
                        edited.net = ice::createLayoutNet(project, activeLayoutId_, netName);
                } else {
                    edited.net = 0;
                }
                ice::updatePort(project, activeLayoutId_, edited);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::warning(this, QStringLiteral("Edit Pin"), QString::fromStdString(error.what()));
            refreshLayoutProperties();
            return;
        }
        refreshProject();
        QTimer::singleShot(0, this, [this, portId = port.id] { selectLayoutShapeItem(portId); });
    }

    void updateLayoutCellParameter(ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name,
                                   const std::string &value) {
        if (!project_)
            return;
        const auto cellIt = project_->project().layoutCells.find(cellId);
        if (cellIt == project_->project().layoutCells.end())
            return;
        const auto cellName = cellIt->second.name;
        const auto *device =
            layoutDevicesPanel_ ? findLayoutDeviceForCell(layoutDevicesPanel_->library(), cellIt->second) : nullptr;
        if (!device || !device->generator) {
            try {
                project_->execute("edit layout parameter", [&](ice::Project &project) {
                    const auto editIt = project.layoutCells.find(cellId);
                    if (editIt == project.layoutCells.end())
                        return;
                    for (auto &parameter : editIt->second.parameters) {
                        if ((parameterId != 0 && parameter.id == parameterId) ||
                            (parameterId == 0 && parameter.name == name)) {
                            parameter.value = value;
                            return;
                        }
                    }
                });
            } catch (const ice::ProjectError &error) {
                QMessageBox::critical(this, QStringLiteral("Edit Parameter"), QString::fromStdString(error.what()));
                updateSelectionStatus();
            }
            return;
        }

        auto values = parameterValues(device->parameters);
        for (const auto &parameter : cellIt->second.parameters)
            values[parameter.name] = parameter.value;
        values[name] = value;

        QString generationError;
        std::optional<ice::LayoutDeviceGeometry> geometry;
        {
            WaitCursorGuard waitCursor;
            statusBar()->showMessage(QStringLiteral("Regenerating %1…").arg(QString::fromStdString(device->name)));
            geometry =
                generateLayoutDeviceGeometry(*device, values, &generationError, layoutDevicesPanel_->library().pdk);
        }
        if (!geometry) {
            QMessageBox::warning(this, QStringLiteral("Regenerate Device"),
                                 QStringLiteral("Could not regenerate %1 geometry:\n\n%2")
                                     .arg(QString::fromStdString(device->name), generationError));
            refreshLayoutProperties();
            return;
        }

        try {
            project_->execute("regenerate layout parameter", [&](ice::Project &project) {
                const auto editIt = project.layoutCells.find(cellId);
                if (editIt == project.layoutCells.end())
                    return;
                auto &cell = editIt->second;
                syncLayoutDeviceParameters(project, cell, *device);
                for (auto &parameter : cell.parameters) {
                    if (const auto generated = values.find(parameter.name); generated != values.end())
                        parameter.value = generated->second;
                }
                if (cell.library)
                    applyLayoutDeviceShapes(project, cell, layoutDevicesPanel_->library(), geometry->shapes,
                                            geometry->ports);
                updateRoutedConnectionPaths(project);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Regenerate Device"), QString::fromStdString(error.what()));
            refreshLayoutProperties();
            return;
        }

        refreshProject();
        statusBar()->showMessage(QStringLiteral("Regenerated %1 geometry (%2 polygons)")
                                     .arg(QString::fromStdString(cellName))
                                     .arg(geometry->shapes.size()));
    }

    void updateLayoutDeviceParameter(const std::string &deviceName, const std::string &name, const std::string &value) {
        if (!layoutDevicesPanel_)
            return;
        const auto *device = findLayoutDevice(layoutDevicesPanel_->library(), deviceName);
        if (!device)
            return;
        auto values = parameterValues(device->parameters);
        values[name] = value;
        if (!device->generator) {
            layoutDevicesPanel_->setParameterValue(deviceName, name, value);
            return;
        }

        QString generationError;
        std::optional<ice::LayoutDeviceGeometry> geometry;
        {
            WaitCursorGuard waitCursor;
            statusBar()->showMessage(QStringLiteral("Regenerating %1…").arg(QString::fromStdString(deviceName)));
            geometry =
                generateLayoutDeviceGeometry(*device, values, &generationError, layoutDevicesPanel_->library().pdk);
        }
        if (!geometry) {
            QMessageBox::warning(this, QStringLiteral("Regenerate Device"),
                                 QStringLiteral("Could not regenerate %1 geometry:\n\n%2")
                                     .arg(QString::fromStdString(deviceName), generationError));
            refreshLayoutProperties();
            return;
        }

        for (const auto &[parameterName, parameterValue] : values)
            layoutDevicesPanel_->setParameterValue(deviceName, parameterName, parameterValue);
        layoutDevicesPanel_->setDeviceGeometry(deviceName, *geometry);
        selectedLayoutDevice_ = device;
        if (layoutCanvas_)
            layoutCanvas_->setDevicePreview(device);
        refreshLayoutProperties();
        statusBar()->showMessage(QStringLiteral("Updated default %1 PCell parameters for new placements")
                                     .arg(QString::fromStdString(deviceName)));
    }

    void addSchematicParameter(ice::ObjectId cellId, const std::string &name) {
        if (!project_)
            return;
        project_->execute("add schematic parameter", [&](ice::Project &project) {
            const auto cell = project.schematicCells.find(cellId);
            if (cell == project.schematicCells.end())
                return;
            cell->second.parameters.push_back({project.nextId(), name, ""});
        });
        updateSelectionStatus();
    }

    void removeSchematicParameters(ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds) {
        if (!project_)
            return;
        project_->execute("remove schematic parameters", [&](ice::Project &project) {
            const auto cell = project.schematicCells.find(cellId);
            if (cell == project.schematicCells.end())
                return;
            auto &parameters = cell->second.parameters;
            parameters.erase(std::remove_if(parameters.begin(), parameters.end(),
                                            [&](const ice::Parameter &parameter) {
                                                return std::find(parameterIds.begin(), parameterIds.end(),
                                                                 parameter.id) != parameterIds.end();
                                            }),
                             parameters.end());
        });
        updateSelectionStatus();
    }

    void editSelectedSymbol() {
        if (!schematicCanvas_)
            return;
        openSymbolProperties(schematicCanvas_->selectedInstanceId());
    }

    // Double-clicking, or the properties toolbar button, opens the instance
    // property editor for the symbol under the cursor.
    void openSymbolProperties(ice::ObjectId instanceId) {
        if (!project_ || instanceId == 0)
            return;
        for (const auto &[cellId, cell] : project_->project().schematicCells) {
            const auto commIt = cell.comments.find(instanceId);
            if (commIt != cell.comments.end()) {
                bool accepted = false;
                const auto newText =
                    QInputDialog::getText(this, QStringLiteral("Edit Comment"), QStringLiteral("Comment:"),
                                          QLineEdit::Normal, QString::fromStdString(commIt->second.text), &accepted);
                if (accepted && !newText.trimmed().isEmpty()) {
                    project_->execute("edit comment", [&](ice::Project &project) {
                        auto cIt = project.schematicCells.find(cellId);
                        if (cIt != project.schematicCells.end()) {
                            auto it = cIt->second.comments.find(instanceId);
                            if (it != cIt->second.comments.end())
                                it->second.text = newText.trimmed().toStdString();
                        }
                    });
                    refreshProject();
                }
                return;
            }
        }
        for (const auto &[_, cell] : project_->project().schematicCells) {
            const auto instance = cell.instances.find(instanceId);
            if (instance == cell.instances.end())
                continue;
            const auto symbol = cell.symbols.find(instance->second.symbol);
            const std::string symbolName = symbol == cell.symbols.end() ? std::string() : symbol->second.name;
            SymbolPropertiesDialog dialog(symbolName, instance->second, this);
            if (dialog.exec() != QDialog::Accepted)
                return;
            const auto name = dialog.instanceName();
            const auto orientation = dialog.orientation();
            const auto parameters = dialog.parameters();
            const auto x = dialog.positionX();
            const auto y = dialog.positionY();
            if (name.isEmpty()) {
                QMessageBox::information(this, QStringLiteral("Device Properties"),
                                         QStringLiteral("An instance name is required."));
                return;
            }
            project_->execute("edit symbol properties", [&](ice::Project &project) {
                for (auto &[_, cell] : project.schematicCells) {
                    const auto target = cell.instances.find(instanceId);
                    if (target == cell.instances.end())
                        continue;
                    target->second.name = name.toStdString();
                    target->second.x = x;
                    target->second.y = y;
                    target->second.orientation = orientation;
                    std::vector<ice::Parameter> updated;
                    for (const auto &edit : parameters) {
                        if (edit.name.trimmed().isEmpty())
                            continue;
                        const auto id = edit.id != 0 ? edit.id : project.nextId();
                        updated.push_back(ice::Parameter{id, edit.name.toStdString(), edit.value.toStdString()});
                    }
                    target->second.parameters = std::move(updated);
                    updateAutomaticNetLabelPositions(project);
                    break;
                }
            });
            refreshProject();
            return;
        }
    }

    // ── Simulation management ────────────────────────────────────────────────

    // Graph plots let the user rename the display label of a signal; the edit
    // is stored on the owning SimulationGraph so it survives tab rebuilds.
    SimulationPlot *createGraphPlot() {
        auto *plot = new SimulationPlot;
        plot->setLabelEditCallback([this](ice::ObjectId graphId, const std::string &signal, const QString &label) {
            renameGraphSignalLabel(graphId, signal, label);
        });
        plot->setYAxisLabelEditCallback(
            [this](ice::ObjectId graphId, const QString &label) { renameGraphYAxisLabel(graphId, label); });
        return plot;
    }

    void renameGraphSignalLabel(ice::ObjectId graphId, const std::string &signal, const QString &label) {
        if (!project_ || activeSimulationId_ == 0 || graphId == 0 || signal.empty())
            return;
        const auto simId = activeSimulationId_;
        const auto text = label.trimmed().toStdString();
        auto found = false;
        project_->execute("rename graph label", [&found, simId, graphId, signal, text](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            for (auto &graph : it->second.graphs) {
                if (graph.id != graphId)
                    continue;
                found = true;
                if (text.empty())
                    graph.signalLabels.erase(signal);
                else
                    graph.signalLabels[signal] = text;
                break;
            }
        });
        if (found)
            refreshGraphAfterLabelEdit(simId, graphId, QStringLiteral("Signal label updated"));
    }

    void renameGraphYAxisLabel(ice::ObjectId graphId, const QString &label) {
        if (!project_ || activeSimulationId_ == 0 || graphId == 0)
            return;
        const auto simId = activeSimulationId_;
        const auto text = label.trimmed().toStdString();
        auto found = false;
        project_->execute("rename graph y axis", [&found, simId, graphId, text](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            for (auto &graph : it->second.graphs) {
                if (graph.id != graphId)
                    continue;
                found = true;
                graph.yAxisLabel = text;
                break;
            }
        });
        if (found)
            refreshGraphAfterLabelEdit(simId, graphId, QStringLiteral("Y axis label updated"));
    }

    void refreshGraphAfterLabelEdit(ice::ObjectId simId, ice::ObjectId graphId, const QString &message) {
        const auto &simulation = project_->project().simulations.at(simId);
        rebuildGraphTabs(simulation.graphs);
        simulationPanel_->setProject(project_->project());
        // Rebuild keeps the previous tab index; select the edited graph's tab.
        for (int index = 0; index < graphTabs_->count(); ++index) {
            if (static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(index).toULongLong()) == graphId) {
                graphTabs_->setCurrentIndex(index);
                break;
            }
        }
        statusBar()->showMessage(message);
    }

    // Build (or rebuild) graphTabs_ from a SimulationItem's graphs vector.
    void rebuildGraphTabs(const std::vector<ice::SimulationGraph> &graphs) {
        loadedCachedRunPath_.clear();
        // Preserve the current tab index so we can restore it.
        const int prevIndex = graphTabs_ ? graphTabs_->currentIndex() : 0;
        if (!graphTabs_)
            return;
        // Keep existing results by graph id so changing display settings
        // (axis selections, limits, ...) redraws the current data instead of
        // discarding it and forcing another simulation run.
        struct PreviousPlot {
            ice::SimulationGraph graph;
            std::shared_ptr<const ice::SimulationResult> result;
        };
        std::map<ice::ObjectId, PreviousPlot> previousPlots;
        for (int i = 0; i < graphTabs_->count(); ++i) {
            const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong());
            const auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(i));
            if (graphId != 0 && plot && plot->resultPtr() && !plot->resultPtr()->time.empty())
                previousPlots.emplace(graphId, PreviousPlot{plot->graph(), plot->resultPtr()});
        }

        // Remove all tabs except the first (which holds simulationPlot_).
        // We always keep simulationPlot_ alive; additional tabs use fresh plots.
        while (graphTabs_->count() > 0) {
            const int last = graphTabs_->count() - 1;
            QWidget *widget = graphTabs_->widget(last);
            graphTabs_->removeTab(last);
            if (widget != simulationPlot_)
                widget->deleteLater();
        }

        if (graphs.empty()) {
            // Show a single empty plot labelled "Graph 1".
            simulationPlot_->clear();
            simulationPlot_->setGraph({});
            simulationPlot_->setGridVisible(graphGridVisible_);
            graphTabs_->addTab(simulationPlot_, QStringLiteral("Graph 1"));
            return;
        }
        for (std::size_t i = 0; i < graphs.size(); ++i) {
            SimulationPlot *plot = (i == 0) ? simulationPlot_ : createGraphPlot();
            auto displayGraph = graphs[i];
            std::vector<std::string> normalizedSignals;
            for (const auto &signal : displayGraph.ySignals) {
                for (const auto &part : parseSignalList(QString::fromStdString(signal))) {
                    const auto value = part.toStdString();
                    if (std::find(normalizedSignals.begin(), normalizedSignals.end(), value) == normalizedSignals.end())
                        normalizedSignals.push_back(value);
                }
            }
            if (!normalizedSignals.empty() || displayGraph.ySignals.empty())
                displayGraph.ySignals = std::move(normalizedSignals);
            plot->clear();
            plot->setGraph(displayGraph);
            plot->setGridVisible(graphGridVisible_);
            if (const auto previous = previousPlots.find(graphs[i].id); previous != previousPlots.end())
                plot->setResult(previous->second.result);
            const auto tabName = QString::fromStdString(graphs[i].name);
            const auto idx = graphTabs_->addTab(plot, tabName);
            graphTabs_->tabBar()->setTabData(idx, QVariant::fromValue<qulonglong>(graphs[i].id));
        }
        const int restored = (prevIndex >= 0 && prevIndex < graphTabs_->count()) ? prevIndex : 0;
        graphTabs_->setCurrentIndex(restored);
    }

    void applySimulationResultToGraphTabs(std::shared_ptr<const ice::SimulationResult> result) {
        if (!graphTabs_)
            return;
        for (int index = 0; index < graphTabs_->count(); ++index) {
            if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                plot->setResult(result);
        }
    }

    void loadCachedSimulationRun() {
        if (!project_ || projectPath_.isEmpty() || activeSimulationId_ == 0)
            return;
        const auto simulation = project_->project().simulations.find(activeSimulationId_);
        if (simulation == project_->project().simulations.end())
            return;
        const auto root = std::filesystem::path(QFileInfo(projectPath_).absolutePath().toStdString());
        auto stored = ice::latestSimulationRun(root, activeSimulationId_);
        if (simulation->second.schematic != 0) {
            auto bySchematic = ice::latestSimulationRun(root, std::nullopt, simulation->second.schematic);
            if (bySchematic) {
                if (!stored)
                    stored = std::move(bySchematic);
                else {
                    const auto &left = stored->createdAt.empty() ? stored->path.filename().string() : stored->createdAt;
                    const auto &right =
                        bySchematic->createdAt.empty() ? bySchematic->path.filename().string() : bySchematic->createdAt;
                    if (right > left)
                        stored = std::move(bySchematic);
                }
            }
        }
        if (!stored)
            return;
        const auto path = QString::fromStdString(stored->path.string());
        if (loadedCachedRunPath_ == path)
            return;
        loadedCachedRunPath_ = path;
        auto result = std::make_shared<ice::SimulationResult>(std::move(stored->result));
        applySimulationResultToGraphTabs(result);
        if (simulationOutput_ && result->state != ice::JobState::succeeded) {
            simulationOutput_->setPlainText(
                QStringLiteral("Loaded last stored run %1: %2%3")
                    .arg(QString::fromStdString(stored->runId),
                         QString::fromStdString(ice::jobStateName(result->state)),
                         result->error.empty() ? QString()
                                               : QStringLiteral("\n%1").arg(QString::fromStdString(result->error))));
        }
    }

    // Called when the SimulationPanel selection changes.
    void updateAnalysisControlLine() {
        if (!analysisControl_)
            return;
        const auto analysis = analysisSelector_->currentData().toString();
        const bool transient = analysis == QStringLiteral("transient");
        if (useInitialConditions_)
            useInitialConditions_->setEnabled(transient);
        QString line;
        if (transient) {
            line = QStringLiteral(".tran %1 %2").arg(stepTime_->value()).arg(stopTime_->value());
            if (useInitialConditions_ && useInitialConditions_->isChecked())
                line += QStringLiteral(" uic");
        } else if (analysis == QStringLiteral("dc")) {
            line = QStringLiteral(".dc V1 0 1 0.01");
        } else if (analysis == QStringLiteral("ac")) {
            line = QStringLiteral(".ac dec 10 1 1e6");
        } else if (analysis == QStringLiteral("digital")) {
            line = QStringLiteral("SystemVerilog testbench: $finish / VCD traces");
        } else {
            line = QStringLiteral(".op");
        }
        analysisControl_->setText(line);
        analysisControl_->setCursorPosition(0);
    }

    void loadSimulationById(ice::ObjectId id, ice::ObjectId graphId = 0) {
        const bool sameSimulation = (id == activeSimulationId_);
        activeSimulationId_ = id;
        if (!project_ || id == 0) {
            rebuildGraphTabs({});
            updateSimulationNetlistState();
            return;
        }
        const auto &sims = project_->project().simulations;
        auto it = sims.find(id);
        if (it == sims.end()) {
            rebuildGraphTabs({});
            updateSimulationNetlistState();
            return;
        }
        const auto &sim = it->second;

        // Block signals so we don't re-save while populating.
        QSignalBlocker b1(schematicSelector_), b2(simulatorSelector_), b3(analysisSelector_), b4(stopTime_),
            b5(stepTime_), b6(additionalDirectives_), b7(useInitialConditions_), b8(signalsCapture_);

        const auto schIdx = schematicSelector_->findData(QVariant::fromValue<qulonglong>(sim.schematic));
        if (schIdx >= 0)
            schematicSelector_->setCurrentIndex(schIdx);
        simulatorSelector_->setCurrentIndex(simulatorSelector_->findData(QString::fromStdString(sim.simulator)));
        auto analysis = QString::fromStdString(sim.analysis).trimmed().toLower();
        if (analysis == QStringLiteral("tran"))
            analysis = QStringLiteral("transient");
        else if (analysis == QStringLiteral("dc sweep"))
            analysis = QStringLiteral("dc");
        else if (analysis == QStringLiteral("ac sweep"))
            analysis = QStringLiteral("ac");
        else if (analysis == QStringLiteral("operating point"))
            analysis = QStringLiteral("op");
        int analysisIndex = analysisSelector_->findData(analysis);
        if (analysisIndex < 0)
            analysisIndex = 0;
        analysisSelector_->setCurrentIndex(analysisIndex);
        stopTime_->setValue(sim.stop);
        stepTime_->setValue(sim.step);
        QStringList additional;
        for (const auto &line : QString::fromStdString(sim.directives).split(QLatin1Char('\n'))) {
            const auto trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !isAnalysisControlDirective(trimmed))
                additional << trimmed;
        }
        additionalDirectives_->setText(additional.join(QLatin1Char(' ')));
        signalsCapture_->setText(QString::fromStdString(sim.captureSignals));
        useInitialConditions_->setChecked(sim.use_initial_conditions);
        updateAnalysisControlLine();
        applySimulationLanguageDefaults();

        // Avoid rebuilding the graph tabs (and losing plot results) when the
        // selection did not actually change, e.g. after a property commit.
        if (!sameSimulation)
            rebuildGraphTabs(sim.graphs);
        if (graphId != 0)
            selectGraphTab(graphId);
        updateSimulationNetlistState();
        loadCachedSimulationRun();
    }

    // Switch the graph tab widget to the tab holding the given graph id.
    void selectGraphTab(ice::ObjectId graphId) {
        if (!graphTabs_ || graphId == 0)
            return;
        for (int i = 0; i < graphTabs_->count(); ++i) {
            if (static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong()) == graphId) {
                graphTabs_->setCurrentIndex(i);
                return;
            }
        }
    }

    // Write form fields back into the active SimulationItem.
    void commitSimulationProperties() {
        if (!project_ || activeSimulationId_ == 0)
            return;
        project_->execute("edit simulation properties", [this](ice::Project &project) {
            auto it = project.simulations.find(activeSimulationId_);
            if (it == project.simulations.end())
                return;
            auto &sim = it->second;
            sim.schematic = static_cast<ice::ObjectId>(schematicSelector_->currentData().toULongLong());
            sim.simulator = simulatorSelector_->currentData().toString().toStdString();
            sim.analysis = analysisSelector_->currentData().toString().toStdString();
            sim.stop = stopTime_->value();
            sim.step = stepTime_->value();
            sim.use_initial_conditions = useInitialConditions_->isChecked();
            sim.directives = additionalDirectives_->text().toStdString();
            sim.captureSignals = signalsCapture_->text().toStdString();
        });
        updateSimulationNetlistState();
    }

    void pickSimulationSignal() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        const auto schematicId = selectedSimulationSchematicId();
        if (schematicId == 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Select a schematic in the Simulation properties first."));
            return;
        }
        if (!schematicCellTabs_ || !schematicCanvas_ || !documents_ || !schematicPage_) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("The schematic editor is not available."));
            return;
        }
        int targetIndex = -1;
        for (int index = 0; index < schematicCellTabs_->count(); ++index) {
            if (schematicCellTabs_->tabBar()->tabData(index).toULongLong() == schematicId) {
                targetIndex = index;
                break;
            }
        }
        if (targetIndex < 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("The selected schematic is no longer available."));
            return;
        }
        schematicCellTabs_->setCurrentIndex(targetIndex);
        activeSchematicId_ = schematicId;
        refreshSchematic();
        selectSchematicTool();
        schematicCanvas_->setSignalPickMode(true);
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(false);
        documents_->setCurrentWidget(schematicPage_);
        schematicCanvas_->setFocus();
        statusBar()->showMessage(QStringLiteral("Click a node or wire to capture it (Esc cancels)"));
    }

    void finishSimulationSignalPick(ice::ObjectId netId) {
        if (!project_ || netId == 0 || !signalsCapture_)
            return;
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            return;
        const auto netIt = cellIt->second.nets.find(netId);
        if (netIt == cellIt->second.nets.end())
            return;
        const auto node = netIt->second.name.empty() ? "n" + std::to_string(netId) : netIt->second.name;
        const auto signal = QStringLiteral("v(%1)").arg(QString::fromStdString(node));
        auto signalNames = parseSignalList(signalsCapture_->text());
        const auto normalize = [](QString text) {
            text = text.trimmed().toLower();
            if (text.startsWith(QStringLiteral("v(")) && text.endsWith(QLatin1Char(')')))
                text = text.mid(2, text.size() - 3);
            return text;
        };
        const bool alreadyPresent = std::any_of(signalNames.begin(), signalNames.end(), [&](const QString &existing) {
            return normalize(existing) == normalize(signal);
        });
        if (!alreadyPresent) {
            signalNames << signal;
            signalsCapture_->setText(signalNames.join(QStringLiteral(", ")));
            commitSimulationProperties();
            statusBar()->showMessage(QStringLiteral("Captured signal %1").arg(signal));
        } else {
            statusBar()->showMessage(QStringLiteral("Signal %1 is already captured").arg(signal));
        }
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        signalsCapture_->setFocus();
        signalsCapture_->setCursorPosition(signalsCapture_->text().size());
    }

    void cancelSimulationSignalPick() {
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        if (signalsCapture_) {
            signalsCapture_->setFocus();
            signalsCapture_->setCursorPosition(signalsCapture_->text().size());
        }
        statusBar()->showMessage(QStringLiteral("Signal selection cancelled"));
    }

    // Add a new SimulationItem to the project.
    void addSimulation() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Add Simulation"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Add Simulation"), QStringLiteral("Simulation name:"),
                                  QLineEdit::Normal, QStringLiteral("sim1"), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        ice::ObjectId newId = 0;
        project_->execute("add simulation", [&](ice::Project &project) {
            newId = project.nextId();
            ice::SimulationItem sim;
            sim.id = newId;
            sim.name = name.trimmed().toStdString();
            // Default to first schematic if available.
            if (!project.schematicCells.empty())
                sim.schematic = project.schematicCells.begin()->first;
            if (sim.schematic != 0 && ice::schematicContainsHdl(project, sim.schematic)) {
                sim.stop = 1.0e-6;
                sim.step = 1.0e-9;
            }
            // Add one default graph. nextId() still returns newId here because
            // the simulation is not in the map yet, so allocate newId + 1.
            ice::SimulationGraph graph;
            graph.id = newId + 1;
            graph.name = "Graph 1";
            sim.graphs.push_back(std::move(graph));
            project.simulations.emplace(newId, std::move(sim));
        });
        simulationPanel_->setProject(project_->project());
        simulationPanel_->selectSimulation(newId);
        statusBar()->showMessage(QStringLiteral("Simulation added"));
    }

    void addSystemVerilogTestbench() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Add SystemVerilog Testbench"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateSchematicDialog dialog(installDirectory, nextSchematicDefaultName() + QStringLiteral(" Testbench"), this);
        dialog.setWindowTitle(QStringLiteral("Add SystemVerilog Testbench"));
        if (dialog.exec() != QDialog::Accepted)
            return;
        const auto name = dialog.schematicName().trimmed();
        if (name.isEmpty())
            return;
        const auto pdk = dialog.pdkId();
        QString pdkLabel;
        bool addDependency = false;
        if (!resolveSchematicPdk(pdk, &pdkLabel, &addDependency))
            return;

        ice::ObjectId testbenchId = 0;
        ice::ObjectId simulationId = 0;
        try {
            project_->execute("add SystemVerilog testbench", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                testbenchId = ice::createSystemVerilogCell(project, name.toStdString(), pdk,
                                                           systemVerilogTemplate(name, true).toStdString());
                const auto baseName = name.toStdString() + " Simulation";
                auto simulationName = baseName;
                const auto simulationNameExists = [&project](const std::string &candidate) {
                    return std::any_of(project.simulations.begin(), project.simulations.end(),
                                       [&](const auto &entry) { return entry.second.name == candidate; });
                };
                for (int suffix = 2; simulationNameExists(simulationName); ++suffix)
                    simulationName = baseName + " " + std::to_string(suffix);

                simulationId = project.nextId();
                ice::SimulationItem simulation;
                simulation.id = simulationId;
                simulation.name = simulationName;
                simulation.schematic = testbenchId;
                simulation.simulator = "iverilog";
                simulation.analysis = "digital";
                project.simulations.emplace(simulationId, std::move(simulation));

                ice::SimulationGraph graph;
                graph.id = project.nextId();
                graph.name = "Graph 1";
                graph.xAxis = "time";
                project.simulations.at(simulationId).graphs.push_back(std::move(graph));
            });
            project_->setLastHistoryLabel("Created SystemVerilog testbench " + name.toStdString());
            refreshProject();
            if (simulationPanel_) {
                simulationPanel_->setProject(project_->project());
                simulationPanel_->selectSimulation(simulationId);
            }
            if (documents_ && simulationPage_)
                documents_->setCurrentWidget(simulationPage_);
            if (simulationTabs_ && simulationTestbenchPage_) {
                updateSimulationTestbenchTab();
                simulationTabs_->setCurrentWidget(simulationTestbenchPage_);
            }
            statusBar()->showMessage(QStringLiteral("SystemVerilog testbench %1 created with %2").arg(name, pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Add SystemVerilog Testbench"),
                                  QString::fromStdString(error.what()));
        }
    }

    QString reportSlug(const QString &name) const {
        return QString::fromStdString(ice::sanitizedReportSlug(name.toStdString()));
    }

    QString uniqueReportPath(const QString &directory, const QString &base, const QString &extension) {
        QDir dir(directory);
        QString candidate = base + extension;
        if (!QFileInfo::exists(dir.filePath(candidate)))
            return dir.filePath(candidate);
        for (int index = 2;; ++index) {
            candidate = QStringLiteral("%1-%2%3").arg(base).arg(index).arg(extension);
            if (!QFileInfo::exists(dir.filePath(candidate)))
                return dir.filePath(candidate);
        }
    }

    const ice::ProjectReport *selectedReport() const {
        if (!project_ || activeReportId_ == 0)
            return nullptr;
        const auto it = project_->project().reports.find(activeReportId_);
        return it == project_->project().reports.end() ? nullptr : &it->second;
    }

    void setReportStatus(const QString &text, bool error = false) {
        if (!reportStatusLabel_)
            return;
        reportStatusLabel_->setText(text);
        reportStatusLabel_->setStyleSheet(
            error ? QStringLiteral("color: #d9534f; font-weight: bold;")
                  : QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    }

    void appendReportMessage(const QString &message) {
        if (activityPanel_)
            activityPanel_->appendMessage(message);
    }

    void showReportMessages() {
        if (activityDock_)
            activityDock_->show();
        if (activityPanel_)
            activityPanel_->showMessagesTab();
    }

    void refreshReportPanel() {
        const auto *report = selectedReport();
        if (!report) {
            clearScriptGraphs();
            if (reportTitleLabel_)
                reportTitleLabel_->setText(QStringLiteral("No simulation script selected"));
            if (reportCodeEditor_) {
                const QSignalBlocker blocker(reportCodeEditor_);
                reportCodeEditor_->clear();
                reportCodeEditor_->setReadOnly(true);
            }
            setReportStatus(QString());
            reportDirty_ = false;
            if (reportSaveTimer_)
                reportSaveTimer_->stop();
            if (reportSaveButton_)
                reportSaveButton_->setEnabled(false);
            if (reportRunButton_)
                reportRunButton_->setEnabled(false);
            if (reportOpenButton_)
                reportOpenButton_->setEnabled(false);
            if (simulationRightStack_ && simulationEditorPage_ &&
                simulationRightStack_->currentWidget() == reportsView_)
                simulationRightStack_->setCurrentWidget(simulationEditorPage_);
            return;
        }
        if (reportTitleLabel_)
            reportTitleLabel_->setText(QString::fromStdString(report->name));
        if (reportCodeEditor_)
            reportCodeEditor_->setReadOnly(false);
        if (reportSaveButton_)
            reportSaveButton_->setEnabled(true);
        if (reportRunButton_)
            reportRunButton_->setEnabled(true);
        if (reportOpenButton_)
            reportOpenButton_->setEnabled(true);
    }

    void loadReportSource() {
        if (!reportCodeEditor_)
            return;
        const auto *report = selectedReport();
        if (!report || projectPath_.isEmpty()) {
            refreshReportPanel();
            return;
        }
        const auto root = QFileInfo(projectPath_).absolutePath();
        const auto path = QDir(root).filePath(QString::fromStdString(report->path));
        QFile file(path);
        loadingReport_ = true;
        {
            const QSignalBlocker blocker(reportCodeEditor_);
            reportCodeEditor_->setReadOnly(false);
            if (!file.open(QIODevice::ReadOnly)) {
                reportCodeEditor_->clear();
                reportCodeEditor_->setReadOnly(true);
                const auto message = QStringLiteral("Unable to open %1").arg(QString::fromStdString(report->path));
                setReportStatus(message, true);
                appendReportMessage(QStringLiteral("[Simulation script] %1").arg(message));
            } else {
                reportCodeEditor_->setPlainText(QString::fromUtf8(file.readAll()));
                setReportStatus(QStringLiteral("Saved"));
            }
        }
        loadingReport_ = false;
        reportDirty_ = false;
        if (reportSaveTimer_)
            reportSaveTimer_->stop();
        refreshReportPanel();
    }

    bool saveReportSource() {
        const auto *report = selectedReport();
        if (!report || !reportCodeEditor_ || projectPath_.isEmpty())
            return false;
        const auto root = QFileInfo(projectPath_).absolutePath();
        const auto path = QDir(root).filePath(QString::fromStdString(report->path));
        const auto directory = QFileInfo(path).absolutePath();
        QDir().mkpath(directory);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            const auto message = QStringLiteral("Unable to save %1").arg(QString::fromStdString(report->path));
            setReportStatus(message, true);
            appendReportMessage(QStringLiteral("[Simulation script] %1").arg(message));
            return false;
        }
        const auto contents = reportCodeEditor_->toPlainText().toUtf8();
        if (file.write(contents) != contents.size() || !file.commit()) {
            const auto message = QStringLiteral("Unable to save %1").arg(QString::fromStdString(report->path));
            setReportStatus(message, true);
            appendReportMessage(QStringLiteral("[Simulation script] %1").arg(message));
            return false;
        }
        reportDirty_ = false;
        if (reportSaveTimer_)
            reportSaveTimer_->stop();
        setReportStatus(QStringLiteral("Saved"));
        return true;
    }

    void selectReport(ice::ObjectId id) {
        if (selectingReport_)
            return;
        if (activeReportId_ != 0 && reportDirty_ && !saveReportSource()) {
            if (activeReportId_ != id) {
                selectingReport_ = true;
                if (simulationPanel_)
                    simulationPanel_->selectReport(activeReportId_);
                selectingReport_ = false;
            }
            return;
        }
        selectingReport_ = true;
        activeReportId_ = id;
        clearScriptGraphs();
        if (simulationPanel_)
            simulationPanel_->selectReport(id);
        if (simulationRightStack_ && reportsView_)
            simulationRightStack_->setCurrentWidget(reportsView_);
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        selectingReport_ = false;
        loadReportSource();
        loadCachedScriptGraphs();
    }

    std::filesystem::path reportRunnerPath() const {
        const auto appDir = QCoreApplication::applicationDirPath().toStdString();
        const std::vector<std::filesystem::path> candidates = {
            std::filesystem::path(appDir) / "assets" / "simulation" / "report_runner.py",
            std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "simulation" / "report_runner.py",
#ifdef ICE_SOURCE_DIR
            std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "simulation" / "report_runner.py",
#endif
            std::filesystem::path("assets") / "simulation" / "report_runner.py",
            std::filesystem::path("/usr/share/ice/assets/simulation/report_runner.py"),
        };
        for (const auto &candidate : candidates) {
            std::error_code status;
            if (std::filesystem::is_regular_file(candidate, status) && !status)
                return candidate;
        }
        return {};
    }

    void clearScriptGraphs() {
        if (!scriptGraphTabs_)
            return;
        while (scriptGraphTabs_->count() > 0) {
            auto *widget = scriptGraphTabs_->widget(0);
            scriptGraphTabs_->removeTab(0);
            delete widget;
        }
        scriptGraphTabs_->hide();
    }

    void displayScriptGraphs(const std::vector<ice::SimulationScriptGraph> &graphs) {
        if (!scriptGraphTabs_)
            return;
        clearScriptGraphs();
        for (const auto &graph : graphs) {
            if (graph.x.empty() || graph.series.empty())
                continue;
            auto result = std::make_shared<ice::SimulationResult>();
            result->state = ice::JobState::succeeded;
            result->analysis = "simulation script";
            result->time = graph.x;
            result->traces = graph.series;

            ice::SimulationGraph definition;
            definition.name = graph.name;
            definition.xAxis = graph.xAxis;
            for (const auto &series : graph.series)
                definition.ySignals.push_back(series.name);
            definition.xMin = graph.xMin;
            definition.xMax = graph.xMax;
            definition.yMin = graph.yMin;
            definition.yMax = graph.yMax;

            auto *plot = new SimulationPlot;
            plot->setGridVisible(graphGridVisible_);
            plot->setGraph(definition);
            plot->setResult(result);
            scriptGraphTabs_->addTab(plot, QString::fromStdString(graph.name));
        }
        if (scriptGraphTabs_->count() > 0) {
            scriptGraphTabs_->show();
            scriptGraphTabs_->setCurrentIndex(0);
            if (scriptViewSplitter_)
                scriptViewSplitter_->setSizes({600, 300});
        }
    }

    void loadCachedScriptGraphs() {
        const auto *report = selectedReport();
        if (!report || projectPath_.isEmpty()) {
            clearScriptGraphs();
            return;
        }
        const auto root = QFileInfo(projectPath_).absolutePath();
        const auto path =
            QString::fromStdString(ice::projectSimulationScriptGraphPath(root.toStdString(), *report).string());
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            clearScriptGraphs();
            return;
        }
        try {
            displayScriptGraphs(ice::parseSimulationScriptGraphs(QString::fromUtf8(file.readAll()).toStdString()));
        } catch (const ice::ProjectError &) {
            clearScriptGraphs();
        }
    }

    void runSelectedReport() {
        const auto *report = selectedReport();
        if (!report) {
            showReportMessages();
            appendReportMessage(QStringLiteral("[Simulation script] No simulation script selected."));
            return;
        }
        const auto reportName = QString::fromStdString(report->name);
        showReportMessages();
        clearScriptGraphs();
        if (reportCodeEditor_ && reportDirty_ && !saveReportSource()) {
            appendReportMessage(
                QStringLiteral("[Simulation script] %1 could not be saved, so it was not run.").arg(reportName));
            return;
        }
        if (projectPath_.isEmpty()) {
            appendReportMessage(
                QStringLiteral("[Simulation script] %1: project path is not available.").arg(reportName));
            return;
        }
        const auto root = QFileInfo(projectPath_).absolutePath();
        const auto reportFile = QDir(root).filePath(QString::fromStdString(report->path));
        if (!QFileInfo::exists(reportFile)) {
            appendReportMessage(QStringLiteral("[Simulation script] %1: script file is missing: %2")
                                    .arg(reportName, QString::fromStdString(report->path)));
            return;
        }
        const auto runner = reportRunnerPath();
        const auto python = pythonExecutable();
        if (runner.empty() || python.isEmpty()) {
            appendReportMessage(
                QStringLiteral("[Simulation script] %1: Python script runner is not available.").arg(reportName));
            return;
        }

        const auto graphPath =
            QString::fromStdString(ice::projectSimulationScriptGraphPath(root.toStdString(), *report).string());
        QDir().mkpath(QFileInfo(graphPath).absolutePath());
        QFile::remove(graphPath);

        appendReportMessage(QStringLiteral("[Simulation script] %1: running…").arg(reportName));
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PYTHONPATH"), icepyPythonPath());
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.setProgram(python);
        process.setArguments({QString::fromStdString(runner.string()), QStringLiteral("--project-root"), root,
                              QStringLiteral("--report"), reportFile, QStringLiteral("--graph-output"), graphPath});
        process.start();
        if (!process.waitForStarted(5000)) {
            appendReportMessage(QStringLiteral("[Simulation script] %1: unable to start Python: %2")
                                    .arg(reportName, process.errorString()));
            statusBar()->showMessage(QStringLiteral("Simulation script failed"));
            return;
        }
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished(2000);
            appendReportMessage(QStringLiteral("[Simulation script] %1: timed out after 30 seconds.").arg(reportName));
            statusBar()->showMessage(QStringLiteral("Simulation script timed out"));
            return;
        }
        const auto stdoutText = QString::fromUtf8(process.readAllStandardOutput());
        const auto stderrText = QString::fromUtf8(process.readAllStandardError());
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            QString detail = stderrText.trimmed();
            if (detail.isEmpty())
                detail = stdoutText.trimmed();
            if (detail.isEmpty())
                detail = QStringLiteral("Python exited with code %1.").arg(process.exitCode());
            appendReportMessage(QStringLiteral("[Simulation script] %1 failed:\n%2").arg(reportName, detail));
            statusBar()->showMessage(QStringLiteral("Simulation script failed"));
            return;
        }
        appendReportMessage(QStringLiteral("[Simulation script] %1 finished.").arg(reportName));
        QFile graphFile(graphPath);
        if (graphFile.open(QIODevice::ReadOnly)) {
            try {
                const auto graphs =
                    ice::parseSimulationScriptGraphs(QString::fromUtf8(graphFile.readAll()).toStdString());
                if (!graphs.empty())
                    displayScriptGraphs(graphs);
            } catch (const ice::ProjectError &error) {
                appendReportMessage(QStringLiteral("[Simulation script] %1: unable to read graphs: %2")
                                        .arg(reportName, QString::fromStdString(error.what())));
            }
        }
        const auto logs = stderrText.trimmed();
        if (!logs.isEmpty())
            appendReportMessage(QStringLiteral("[Simulation script] %1 logs:\n%2").arg(reportName, logs));
        const auto output = stdoutText.trimmed();
        if (!output.isEmpty())
            appendReportMessage(QStringLiteral("[Simulation script] %1 output:\n%2").arg(reportName, output));
        else
            appendReportMessage(QStringLiteral("[Simulation script] %1 produced no output.").arg(reportName));
        statusBar()->showMessage(QStringLiteral("Simulation script finished"));
    }

    void openReportFile(ice::ObjectId id) {
        if (!project_ || projectPath_.isEmpty() || id == 0)
            return;
        const auto it = project_->project().reports.find(id);
        if (it == project_->project().reports.end())
            return;
        const auto root = QFileInfo(projectPath_).absolutePath();
        const auto path = QDir(root).filePath(QString::fromStdString(it->second.path));
        if (!QFileInfo::exists(path)) {
            QMessageBox::warning(this, QStringLiteral("Open Simulation Script"),
                                 QStringLiteral("Simulation script file is missing:\n%1").arg(path));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }

    void openSelectedReportFile() {
        if (reportDirty_ && !saveReportSource())
            return;
        openReportFile(activeReportId_);
    }

    void addReport() {
        if (!project_ || projectPath_.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Add Simulation Script"),
                                     QStringLiteral("Open and save a project first."));
            return;
        }
        if (reportDirty_ && !saveReportSource())
            return;
        bool ok = false;
        auto name = QInputDialog::getText(this, QStringLiteral("Add Simulation Script"),
                                          QStringLiteral("Simulation script name:"), QLineEdit::Normal,
                                          QStringLiteral("Simulation Script"), &ok)
                        .trimmed();
        if (!ok || name.isEmpty())
            return;
        QString uniqueName = name;
        for (int suffix = 2;; ++suffix) {
            const bool used =
                std::any_of(project_->project().reports.begin(), project_->project().reports.end(),
                            [&](const auto &entry) { return entry.second.name == uniqueName.toStdString(); });
            if (!used)
                break;
            uniqueName = QStringLiteral("%1 %2").arg(name).arg(suffix);
        }
        name = uniqueName;

        const auto root = QFileInfo(projectPath_).absolutePath();
        QDir reportsDir(QDir(root).filePath(QStringLiteral("scripts")));
        if (!reportsDir.mkpath(QStringLiteral("."))) {
            QMessageBox::critical(this, QStringLiteral("Add Simulation Script"),
                                  QStringLiteral("Unable to create the simulation scripts directory."));
            return;
        }
        const auto target = uniqueReportPath(reportsDir.absolutePath(), reportSlug(name), QStringLiteral(".py"));
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly)) {
            QMessageBox::critical(this, QStringLiteral("Add Simulation Script"),
                                  QStringLiteral("Unable to create simulation script file:\n%1").arg(target));
            return;
        }
        const auto templateText = QString::fromStdString(ice::defaultSimulationReportTemplate()).toUtf8();
        if (file.write(templateText) != templateText.size() || !file.commit()) {
            QMessageBox::critical(this, QStringLiteral("Add Simulation Script"),
                                  QStringLiteral("Unable to write simulation script file:\n%1").arg(target));
            return;
        }

        const auto relative = QDir(root).relativeFilePath(target);
        ice::ObjectId newId = 0;
        try {
            const auto reportName = name;
            const auto reportPath = relative;
            project_->execute("add simulation script", [&](ice::Project &project) {
                newId = project.nextId();
                project.reports.emplace(newId,
                                        ice::ProjectReport{newId, reportName.toStdString(), reportPath.toStdString()});
            });
            project_->setLastHistoryLabel("Added simulation script " + name.toStdString());
            activeReportId_ = newId;
            refreshProject();
            selectReport(newId);
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Add Simulation Script"), QString::fromStdString(error.what()));
        }
    }

    void deleteReport(ice::ObjectId id) {
        if (!project_)
            return;
        const auto it = project_->project().reports.find(id);
        if (it == project_->project().reports.end())
            return;
        if (activeReportId_ == id && reportDirty_ && !saveReportSource())
            return;
        if (QMessageBox::question(
                this, QStringLiteral("Delete Simulation Script"),
                QStringLiteral("Delete simulation script metadata for '%1'? The Python file will remain.")
                    .arg(QString::fromStdString(it->second.name)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("delete simulation script", [id](ice::Project &project) { project.reports.erase(id); });
        const bool deletedActive = activeReportId_ == id;
        if (deletedActive)
            activeReportId_ = 0;
        refreshProject();
        if (deletedActive) {
            if (simulationRightStack_ && simulationEditorPage_)
                simulationRightStack_->setCurrentWidget(simulationEditorPage_);
            if (documents_ && simulationPage_)
                documents_->setCurrentWidget(simulationPage_);
        }
        statusBar()->showMessage(QStringLiteral("Simulation script deleted"));
    }

    // Delete the currently selected SimulationItem.
    void deleteSimulation() {
        if (!project_ || activeSimulationId_ == 0)
            return;
        const auto id = activeSimulationId_;
        const auto res = QMessageBox::question(this, QStringLiteral("Delete Simulation"),
                                               QStringLiteral("Delete this simulation and all its graphs?"),
                                               QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes)
            return;
        project_->execute("delete simulation", [id](ice::Project &project) { project.simulations.erase(id); });
        activeSimulationId_ = 0;
        simulationPanel_->setProject(project_->project());
        rebuildGraphTabs({});
        updateSimulationNetlistState();
        statusBar()->showMessage(QStringLiteral("Simulation deleted"));
    }

    // Add a new graph to the given SimulationItem.
    void addGraph(ice::ObjectId simId) {
        if (!project_ || simId == 0)
            return;
        bool ok = false;
        std::size_t graphCount = 0;
        if (const auto it = project_->project().simulations.find(simId); it != project_->project().simulations.end())
            graphCount = it->second.graphs.size();
        const auto name = QInputDialog::getText(this, QStringLiteral("Add Graph"), QStringLiteral("Graph name:"),
                                                QLineEdit::Normal, QStringLiteral("Graph %1").arg(graphCount + 1), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        ice::ObjectId newGraphId = 0;
        project_->execute("add graph", [&](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            ice::SimulationGraph graph;
            graph.id = project.nextId();
            newGraphId = graph.id;
            graph.name = name.trimmed().toStdString();
            const auto signalNames = parseSignalList(signalsCapture_ ? signalsCapture_->text() : QString());
            if (!signalNames.isEmpty())
                graph.ySignals.push_back(signalNames.front().toStdString());
            it->second.graphs.push_back(std::move(graph));
        });
        if (newGraphId == 0)
            return;
        // Refresh the panel tree, select the simulation and the new graph tab.
        simulationPanel_->setProject(project_->project());
        activeSimulationId_ = 0; // force rebuild on next load
        simulationPanel_->selectSimulation(simId, newGraphId);
        statusBar()->showMessage(QStringLiteral("Graph added"));
    }

    // Delete the given graph from the given SimulationItem.
    void deleteGraph(ice::ObjectId simId, ice::ObjectId graphId) {
        if (!project_ || simId == 0 || graphId == 0)
            return;
        const auto simIt = project_->project().simulations.find(simId);
        if (simIt == project_->project().simulations.end())
            return;
        // Don't delete the last graph.
        if (simIt->second.graphs.size() <= 1) {
            QMessageBox::information(this, QStringLiteral("Delete Graph"),
                                     QStringLiteral("A simulation must have at least one graph."));
            return;
        }
        const auto res =
            QMessageBox::question(this, QStringLiteral("Delete Graph"), QStringLiteral("Delete this graph?"),
                                  QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes)
            return;
        project_->execute("delete graph", [simId, graphId](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            auto &graphs = it->second.graphs;
            graphs.erase(std::remove_if(graphs.begin(), graphs.end(),
                                        [graphId](const ice::SimulationGraph &g) { return g.id == graphId; }),
                         graphs.end());
        });
        // Rebuild tabs from the surviving graphs and refresh the panel tree.
        const auto &updatedSim = project_->project().simulations.at(simId);
        rebuildGraphTabs(updatedSim.graphs);
        simulationPanel_->setProject(project_->project());
        statusBar()->showMessage(QStringLiteral("Graph deleted"));
    }

    // Delete the currently visible graph tab.
    void deleteCurrentGraph() {
        if (!project_ || activeSimulationId_ == 0 || !graphTabs_)
            return;
        const int tabIdx = graphTabs_->currentIndex();
        if (tabIdx < 0)
            return;
        const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(tabIdx).toULongLong());
        deleteGraph(activeSimulationId_, graphId);
    }

    // Edit the current graph's name, axis signal selections, and limits.
    void editGraph() {
        if (!project_ || activeSimulationId_ == 0 || !graphTabs_)
            return;
        const int tabIdx = graphTabs_->currentIndex();
        if (tabIdx < 0)
            return;
        const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(tabIdx).toULongLong());
        const auto simId = activeSimulationId_;
        const auto &sim = project_->project().simulations.at(simId);
        const auto git = std::find_if(sim.graphs.begin(), sim.graphs.end(),
                                      [graphId](const ice::SimulationGraph &g) { return g.id == graphId; });
        if (git == sim.graphs.end())
            return;
        const auto &currentGraph = *git;

        // Build a small dialog for editing the name, axis selections, and limits.
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Edit Graph"));
        dlg.resize(500, 500);
        auto *dlgLayout = new QVBoxLayout(&dlg);
        auto *form = new QFormLayout;
        auto *nameEdit = new QLineEdit(QString::fromStdString(currentGraph.name));
        auto *xAxisCombo = new QComboBox;
        const auto capturedSignals = parseSignalList(signalsCapture_ ? signalsCapture_->text() : QString());
        xAxisCombo->addItem(QStringLiteral("Time (s)"), QStringLiteral("time"));
        for (const auto &signal : capturedSignals)
            xAxisCombo->addItem(signal, signal);
        int xIndex = xAxisCombo->findData(QString::fromStdString(currentGraph.xAxis));
        if (xIndex < 0 && currentGraph.xAxis == "Time (s)")
            xIndex = xAxisCombo->findData(QStringLiteral("time"));
        if (xIndex < 0)
            xIndex = 0;
        xAxisCombo->setCurrentIndex(xIndex);

        // One explicit check box per signal: the previous list items relied on
        // the item check indicator, which the widget style does not draw.
        auto *ySignalsContainer = new QWidget;
        auto *ySignalsLayout = new QVBoxLayout(ySignalsContainer);
        ySignalsLayout->setContentsMargins(6, 4, 6, 4);
        ySignalsLayout->setSpacing(4);
        auto *ySignalsArea = new QScrollArea;
        ySignalsArea->setWidgetResizable(true);
        ySignalsArea->setMinimumHeight(130);
        ySignalsArea->setFrameShape(QFrame::StyledPanel);
        ySignalsArea->setWidget(ySignalsContainer);
        ySignalsArea->setAccessibleName(QStringLiteral("Y axis signals"));
        struct YSignalChoice {
            QCheckBox *box = nullptr;
            QString data;
        };
        std::vector<YSignalChoice> ySignalChoices;
        const auto selectedYSignal = [&](const QString &data) {
            for (const auto &signal : currentGraph.ySignals) {
                for (const auto &part : parseSignalList(QString::fromStdString(signal))) {
                    if (part.compare(data, Qt::CaseInsensitive) == 0)
                        return true;
                    if (data == QStringLiteral("time") &&
                        part.compare(QStringLiteral("Time (s)"), Qt::CaseInsensitive) == 0)
                        return true;
                }
            }
            return false;
        };
        const auto addYSignal = [&](const QString &label, const QString &data) {
            auto *box = new QCheckBox(label);
            box->setChecked(selectedYSignal(data));
            ySignalsLayout->addWidget(box);
            ySignalChoices.push_back({box, data});
        };
        addYSignal(QStringLiteral("Time (s)"), QStringLiteral("time"));
        for (const auto &signal : capturedSignals)
            addYSignal(signal, signal);
        ySignalsLayout->addStretch(1);
        bool anyYChecked = false;
        for (const auto &choice : ySignalChoices)
            anyYChecked |= choice.box->isChecked();
        if (!anyYChecked && !ySignalChoices.empty())
            ySignalChoices[ySignalChoices.size() > 1 ? 1 : 0].box->setChecked(true);

        const auto makeLimitEdit = [](const std::optional<double> &limit) {
            auto *edit = new QLineEdit;
            edit->setPlaceholderText(QStringLiteral("auto"));
            if (limit)
                edit->setText(QString::number(*limit, 'g', 12));
            return edit;
        };
        auto *xMinEdit = makeLimitEdit(currentGraph.xMin);
        auto *xMaxEdit = makeLimitEdit(currentGraph.xMax);
        auto *yMinEdit = makeLimitEdit(currentGraph.yMin);
        auto *yMaxEdit = makeLimitEdit(currentGraph.yMax);
        const auto makeLimitRow = [](QLineEdit *minimum, QLineEdit *maximum) {
            auto *row = new QWidget;
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(minimum);
            rowLayout->addWidget(new QLabel(QStringLiteral("to")));
            rowLayout->addWidget(maximum);
            return row;
        };
        form->addRow(QStringLiteral("Graph name:"), nameEdit);
        form->addRow(QStringLiteral("X axis:"), xAxisCombo);
        form->addRow(QStringLiteral("Y axis signals:"), ySignalsArea);
        form->addRow(QStringLiteral("X limits:"), makeLimitRow(xMinEdit, xMaxEdit));
        form->addRow(QStringLiteral("Y limits:"), makeLimitRow(yMinEdit, yMaxEdit));
        dlgLayout->addLayout(form);
        auto *limitsHint = new QLabel(
            QStringLiteral("X selects one captured signal or time. Check one or more Y signals; they share the X "
                           "axis. Leave a limit blank to auto-scale that axis. Limits only change how the graph is "
                           "drawn; they do not affect simulation parameters or data acquisition."));
        limitsHint->setWordWrap(true);
        dlgLayout->addWidget(limitsHint);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        dlgLayout->addWidget(buttons);

        std::optional<double> newXMin;
        std::optional<double> newXMax;
        std::optional<double> newYMin;
        std::optional<double> newYMax;
        std::vector<std::string> newYSignals;
        const auto parseLimit = [](const QLineEdit *edit, std::optional<double> &limit) {
            limit.reset();
            const auto text = edit->text().trimmed();
            if (text.isEmpty())
                return true;
            bool ok = false;
            const auto value = text.toDouble(&ok);
            if (!ok || !std::isfinite(value))
                return false;
            limit = value;
            return true;
        };
        connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
            newYSignals.clear();
            for (const auto &choice : ySignalChoices) {
                if (!choice.box->isChecked())
                    continue;
                const auto signal = choice.data.toStdString();
                if (!signal.empty() && std::find(newYSignals.begin(), newYSignals.end(), signal) == newYSignals.end())
                    newYSignals.push_back(signal);
            }
            if (newYSignals.empty()) {
                QMessageBox::warning(&dlg, QStringLiteral("Y axis signals"),
                                     QStringLiteral("Select at least one Y axis signal."));
                return;
            }
            if (!parseLimit(xMinEdit, newXMin) || !parseLimit(xMaxEdit, newXMax) || !parseLimit(yMinEdit, newYMin) ||
                !parseLimit(yMaxEdit, newYMax)) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("Enter numeric axis limits or leave a field blank for automatic "
                                                    "scaling."));
                return;
            }
            if (newXMin && newXMax && *newXMax <= *newXMin) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("The X maximum must be greater than the X minimum."));
                return;
            }
            if (newYMin && newYMax && *newYMax <= *newYMin) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("The Y maximum must be greater than the Y minimum."));
                return;
            }
            dlg.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted)
            return;

        const auto newName = nameEdit->text().trimmed();
        const auto newXAxis = xAxisCombo->currentData().toString();

        project_->execute("edit graph", [simId, graphId, newName, newXAxis, newYSignals, newXMin, newXMax, newYMin,
                                         newYMax](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            for (auto &g : it->second.graphs) {
                if (g.id != graphId)
                    continue;
                g.name = newName.toStdString();
                g.xAxis = newXAxis.toStdString();
                g.ySignals = newYSignals;
                g.xMin = newXMin;
                g.xMax = newXMax;
                g.yMin = newYMin;
                g.yMax = newYMax;
                break;
            }
        });
        // Rebuild tabs to reflect new name and refresh the panel tree.
        const auto &updatedSim = project_->project().simulations.at(simId);
        rebuildGraphTabs(updatedSim.graphs);
        simulationPanel_->setProject(project_->project());
        // Restore to the same graph (by id).
        for (int i = 0; i < graphTabs_->count(); ++i) {
            if (static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong()) == graphId) {
                graphTabs_->setCurrentIndex(i);
                break;
            }
        }
        statusBar()->showMessage(QStringLiteral("Graph updated"));
    }

    // Refresh the active graph's plot (called when switching tabs).
    void refreshGraphPlot() {
        if (graphTabs_ && graphTabs_->currentWidget())
            graphTabs_->currentWidget()->update();
    }

    ice::SimulationRequest buildSimulationRequest(const ice::SimulationItem &sim, QStringList &netlistNotes) const {
        ice::SimulationRequest request;
        request.analysis = sim.analysis;
        request.simulator = sim.simulator;
        request.stop = sim.stop;
        request.step = sim.step;
        request.use_initial_conditions = sim.use_initial_conditions;
        if (!projectPath_.isEmpty())
            request.workingDirectory = std::filesystem::path(QFileInfo(projectPath_).absolutePath().toStdString());
        // The circuit comes from the schematic the simulation points at, so
        // Run simulates what you drew instead of hand-typed directives. The
        // additional directives box is reserved for model cards / .include
        // lines.
        const auto &project = project_->project();
        const auto cellId = sim.schematic != 0 && project.schematicCells.contains(sim.schematic)
                                ? sim.schematic
                                : (project.schematicCells.empty() ? 0 : project.schematicCells.begin()->first);
        bool hdl = false;
        if (cellId != 0) {
            const auto &cell = project.schematicCells.at(cellId);
            if (cell.systemVerilogSchematic) {
                hdl = true;
                request.simulator = "iverilog";
                request.analysis = "digital";
                std::istringstream source(cell.systemVerilogText);
                std::string line;
                while (std::getline(source, line))
                    request.netlist.push_back(line);
                netlistNotes << QStringLiteral("SystemVerilog source cell");
            } else if (ice::schematicContainsHdl(project, cellId)) {
                std::string error;
                if (!ice::prepareMixedSignalRequest(request, project, cellId, &error))
                    throw ice::ProjectError("simulation.mixed", error);
                ice::applyPdkSimulationSetup(cell.pdk, request);
                netlistNotes << QStringLiteral("Mixed-signal co-simulation (ngspice + Icarus Verilog)");
            } else {
                request.simulator = "ngspice";
                std::vector<std::string> notes;
                const auto lines = ice::buildSimulationNetlist(project, cellId, &notes);
                request.netlist.insert(request.netlist.end(), lines.begin(), lines.end());
                ice::applyPdkSimulationSetup(cell.pdk, request);
                for (const auto &noteText : notes)
                    netlistNotes << QString::fromStdString(noteText);
            }
        }
        if (!hdl) {
            for (const auto &line : additionalDirectives_->text().split('\n')) {
                const auto trimmed = line.trimmed();
                // Skip analysis/control directives the runner already handles from
                // the simulation properties; keep model cards and includes.
                if (trimmed.isEmpty() || isAnalysisControlDirective(trimmed))
                    continue;
                request.netlist.push_back(trimmed.toStdString());
            }
        }
        // The captured-signal list tells the adapter which vectors to fetch
        // back from ngspice. Graph axis selections then pick from those traces.
        for (const auto &signal : parseSignalList(QString::fromStdString(sim.captureSignals)))
            request.probeSignals.push_back(signal.toStdString());

        return request;
    }

    ice::ObjectId selectedSimulationSchematicId() const {
        if (!schematicSelector_)
            return 0;
        return static_cast<ice::ObjectId>(schematicSelector_->currentData().toULongLong());
    }

    bool selectedSimulationSchematicIsText() const {
        if (!project_)
            return false;
        const auto id = selectedSimulationSchematicId();
        if (id == 0)
            return false;
        const auto cell = project_->project().schematicCells.find(id);
        return cell != project_->project().schematicCells.end() && cell->second.isTextSource();
    }

    bool selectedSimulationSchematicIsHdl() const {
        if (!project_)
            return false;
        const auto id = selectedSimulationSchematicId();
        if (id == 0)
            return false;
        const auto cell = project_->project().schematicCells.find(id);
        return cell != project_->project().schematicCells.end() && cell->second.systemVerilogSchematic;
    }

    ice::SimulationItem currentSimulationFormItem() const {
        ice::SimulationItem sim;
        sim.schematic = selectedSimulationSchematicId();
        sim.simulator = simulatorSelector_->currentData().toString().toStdString();
        sim.analysis = analysisSelector_->currentData().toString().toStdString();
        sim.stop = stopTime_->value();
        sim.step = stepTime_->value();
        sim.use_initial_conditions = useInitialConditions_->isChecked();
        sim.directives = additionalDirectives_->text().toStdString();
        sim.captureSignals = signalsCapture_->text().toStdString();
        return sim;
    }

    QString simulationNetlistText() {
        if (!project_ || selectedSimulationSchematicId() == 0)
            return {};
        const auto sim = currentSimulationFormItem();
        QStringList notes;
        try {
            const auto request = buildSimulationRequest(sim, notes);
            QString text;
            for (const auto &line : request.netlist)
                text += QString::fromStdString(line) + QLatin1Char('\n');
            if (!notes.isEmpty()) {
                if (!text.isEmpty())
                    text += QLatin1Char('\n');
                text += QStringLiteral("* Notes\n");
                for (const auto &note : notes)
                    text += QStringLiteral("* %1\n").arg(note);
            }
            return text.isEmpty() ? QStringLiteral("* Empty netlist") : text;
        } catch (const ice::ProjectError &error) {
            return QString::fromStdString(error.what());
        }
    }

    void updateSimulationBuildButton() {
        const auto schematicId = selectedSimulationSchematicId();
        const bool textSchematic = selectedSimulationSchematicIsText();
        const bool hasSimulation = project_ && activeSimulationId_ != 0;
        const bool hasSchematic = project_ && schematicId != 0;
        const bool upToDate = builtSimulationNetlistSchematicId_ == schematicId &&
                              builtSimulationNetlist_ == currentSimulationNetlist_ &&
                              builtSimulationNetlistSchematicId_ != 0;
        if (simulationBuildNetlistButton_) {
            simulationBuildNetlistButton_->setVisible(!textSchematic);
            simulationBuildNetlistButton_->setEnabled(!textSchematic && hasSimulation && hasSchematic && !upToDate);
        }
        if (simulationRunButton_)
            simulationRunButton_->setEnabled(hasSimulation && hasSchematic && (textSchematic || upToDate));
    }

    void onSimulationTestbenchEdited(const QString &text) {
        if (!project_ || activeSimulationId_ == 0)
            return;
        auto &project = project_->project();
        const auto simulation = project.simulations.find(activeSimulationId_);
        if (simulation == project.simulations.end())
            return;
        const auto cell = project.schematicCells.find(simulation->second.schematic);
        if (cell == project.schematicCells.end() || !cell->second.systemVerilogSchematic)
            return;
        const auto value = text.toStdString();
        if (cell->second.systemVerilogText == value)
            return;
        cell->second.systemVerilogText = value;
        project_->markDirty();
        updateFileTabDirtyIndicators();
        updateSimulationNetlistState();
    }

    void updateSimulationTestbenchTab() {
        if (!project_ || !simulationTabs_ || simulationTestbenchTabIndex_ < 0)
            return;
        const auto id = selectedSimulationSchematicId();
        const auto cell = project_->project().schematicCells.find(id);
        const bool hdl = cell != project_->project().schematicCells.end() && cell->second.systemVerilogSchematic;
        simulationTabs_->setTabVisible(simulationTestbenchTabIndex_, hdl);
        if (!hdl)
            return;
        if (simulationTestbenchEditor_) {
            const auto text = QString::fromStdString(cell->second.systemVerilogText);
            if (simulationTestbenchEditor_->text() != text)
                simulationTestbenchEditor_->setText(text);
        }
        simulationTabs_->setTabToolTip(
            simulationTestbenchTabIndex_,
            QStringLiteral("SystemVerilog testbench: %1").arg(QString::fromStdString(cell->second.name)));
    }

    void applySimulationLanguageDefaults() {
        if (!project_ || !schematicSelector_ || !simulatorSelector_ || !analysisSelector_)
            return;
        const auto id = selectedSimulationSchematicId();
        const auto cell = project_->project().schematicCells.find(id);
        const bool hdl = cell != project_->project().schematicCells.end() && cell->second.systemVerilogSchematic;
        const bool mixed = cell != project_->project().schematicCells.end() && !hdl &&
                           ice::schematicContainsHdl(project_->project(), id);
        QSignalBlocker simulatorBlocker(simulatorSelector_);
        QSignalBlocker analysisBlocker(analysisSelector_);
        if (hdl) {
            const auto simulator = simulatorSelector_->findData(QStringLiteral("iverilog"));
            if (simulator >= 0)
                simulatorSelector_->setCurrentIndex(simulator);
            const auto analysis = analysisSelector_->findData(QStringLiteral("digital"));
            if (analysis >= 0)
                analysisSelector_->setCurrentIndex(analysis);
        } else if (mixed) {
            const auto simulator = simulatorSelector_->findData(QStringLiteral("mixed"));
            if (simulator >= 0)
                simulatorSelector_->setCurrentIndex(simulator);
            const auto analysis = analysisSelector_->findData(QStringLiteral("transient"));
            if (analysis >= 0)
                analysisSelector_->setCurrentIndex(analysis);
        } else {
            if (simulatorSelector_->currentData().toString() == QStringLiteral("iverilog") ||
                simulatorSelector_->currentData().toString() == QStringLiteral("mixed")) {
                const auto simulator = simulatorSelector_->findData(QStringLiteral("ngspice"));
                if (simulator >= 0)
                    simulatorSelector_->setCurrentIndex(simulator);
            }
            if (analysisSelector_->currentData().toString() == QStringLiteral("digital")) {
                const auto analysis = analysisSelector_->findData(QStringLiteral("transient"));
                if (analysis >= 0)
                    analysisSelector_->setCurrentIndex(analysis);
            }
        }
        updateAnalysisControlLine();
    }

    void updateSimulationControlStates() {
        const bool hasSimulation = project_ && activeSimulationId_ != 0;
        const bool hasSchematic = project_ && selectedSimulationSchematicId() != 0;
        const bool textSchematic = selectedSimulationSchematicIsText();
        const auto setPropertyEnabled = [hasSimulation](QWidget *widget) {
            if (widget)
                widget->setEnabled(hasSimulation);
        };
        setPropertyEnabled(schematicSelector_);
        setPropertyEnabled(simulatorSelector_);
        setPropertyEnabled(analysisSelector_);
        setPropertyEnabled(stopTime_);
        setPropertyEnabled(stepTime_);
        setPropertyEnabled(useInitialConditions_);
        setPropertyEnabled(analysisControl_);
        setPropertyEnabled(signalsCapture_);
        const bool hdl = selectedSimulationSchematicIsHdl();
        if (stopTime_)
            stopTime_->setEnabled(hasSimulation && !hdl);
        if (stepTime_)
            stepTime_->setEnabled(hasSimulation && !hdl);
        if (useInitialConditions_)
            useInitialConditions_->setEnabled(hasSimulation && !hdl && analysisSelector_ &&
                                              analysisSelector_->currentData().toString() ==
                                                  QStringLiteral("transient"));
        if (additionalDirectives_)
            additionalDirectives_->setEnabled(hasSimulation && !hdl);
        if (analysisControl_)
            analysisControl_->setEnabled(hasSimulation);
        if (signalSelectButton_) {
            signalSelectButton_->setVisible(!textSchematic);
            signalSelectButton_->setEnabled(hasSimulation && !textSchematic);
        }
        if (simulationAddGraphButton_)
            simulationAddGraphButton_->setEnabled(hasSimulation);
        if (simulationDeleteGraphButton_)
            simulationDeleteGraphButton_->setEnabled(hasSimulation);
        if (simulationEditGraphButton_)
            simulationEditGraphButton_->setEnabled(hasSimulation);
        if (simulationGridButton_)
            simulationGridButton_->setEnabled(hasSimulation);
        if (simulationLoadVcdButton_)
            simulationLoadVcdButton_->setEnabled(hasSimulation);
        if (simulationTestbenchRunButton_)
            simulationTestbenchRunButton_->setEnabled(hasSimulation && hasSchematic && hdl);
        updateSimulationTestbenchTab();
        updateSimulationBuildButton();
    }

    void updateSimulationNetlistState() {
        if (!simulationNetlist_)
            return;
        if (!project_ || selectedSimulationSchematicId() == 0) {
            currentSimulationNetlist_.clear();
            builtSimulationNetlist_.clear();
            builtSimulationNetlistSchematicId_ = 0;
            simulationNetlist_->clear();
            updateSimulationControlStates();
            return;
        }
        currentSimulationNetlist_ = simulationNetlistText();
        if (selectedSimulationSchematicIsText())
            simulationNetlist_->setPlainText(currentSimulationNetlist_);
        updateSimulationControlStates();
    }

    void buildSimulationNetlist() {
        const auto schematicId = selectedSimulationSchematicId();
        if (!simulationNetlist_ || !project_ || activeSimulationId_ == 0 || schematicId == 0)
            return;
        simulationNetlist_->setPlainText(currentSimulationNetlist_);
        builtSimulationNetlistSchematicId_ = schematicId;
        builtSimulationNetlist_ = currentSimulationNetlist_;
        updateSimulationBuildButton();
        statusBar()->showMessage(QStringLiteral("Simulation netlist built"));
    }

    static QString formatSimulationReport(const ice::SimulationRequest &request, const ice::SimulationResult &result,
                                          const QStringList &netlistNotes) {
        std::ostringstream output;
        output << "State: " << ice::jobStateName(result.state) << "\n"
               << "Analysis: " << result.analysis << "\n"
               << "Simulator: " << result.simulator << " " << result.simulator_version << "\n";
        if (!request.netlist.empty()) {
            output << "Netlist:\n";
            for (const auto &line : request.netlist)
                output << "  " << line << "\n";
        }
        for (const auto &note : netlistNotes)
            output << "Note: " << note.toStdString() << "\n";
        if (!result.error.empty())
            output << "Error: " << result.error << "\n";
        if (!result.time.empty() && !result.traces.empty()) {
            output << "Samples: " << result.time.size() << "\n"
                   << "Trace: " << result.traces.front().name << "\n"
                   << "First sample: " << result.traces.front().values.front() << "\n"
                   << "Last sample: " << result.traces.front().values.back() << "\n";
        }
        return QString::fromStdString(output.str());
    }

    void loadVcdGraphs() {
        if (!project_ || activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Load VCD"), QStringLiteral("Select a simulation first."));
            return;
        }
        const auto directory = projectPath_.isEmpty() ? QString() : QFileInfo(projectPath_).absolutePath();
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Load VCD"), directory,
                                                       QStringLiteral("VCD files (*.vcd);;All files (*)"));
        if (path.isEmpty())
            return;
        auto vcd = ice::parseVcdFile(path.toStdString());
        if (vcd.traces.empty()) {
            QMessageBox::warning(this, QStringLiteral("Load VCD"),
                                 QStringLiteral("No graphable traces were found in %1").arg(path));
            return;
        }

        std::vector<std::string> traceNames;
        traceNames.reserve(vcd.traces.size());
        std::string capture;
        for (const auto &trace : vcd.traces) {
            traceNames.push_back(trace.name);
            if (!capture.empty())
                capture += ",";
            capture += trace.name;
        }
        bool graphsUpdated = false;
        project_->execute("load VCD traces", [&](ice::Project &project) {
            auto it = project.simulations.find(activeSimulationId_);
            if (it == project.simulations.end())
                return;
            it->second.captureSignals = capture;
            for (auto &graph : it->second.graphs) {
                if (!graph.ySignals.empty())
                    continue;
                const auto count = std::min<std::size_t>(4, traceNames.size());
                for (std::size_t index = 0; index < count; ++index)
                    graph.ySignals.push_back(traceNames[index]);
                if (graph.xAxis.empty())
                    graph.xAxis = "time";
            }
            graphsUpdated = true;
        });
        if (!graphsUpdated)
            return;
        if (const auto updated = project_->project().simulations.find(activeSimulationId_);
            updated != project_->project().simulations.end()) {
            QSignalBlocker blocker(signalsCapture_);
            signalsCapture_->setText(QString::fromStdString(updated->second.captureSignals));
            if (simulationPanel_)
                simulationPanel_->setProject(project_->project());
            rebuildGraphTabs(updated->second.graphs);
        }
        auto result = std::make_shared<ice::SimulationResult>();
        result->state = ice::JobState::succeeded;
        result->analysis = "VCD";
        result->simulator = "vcd";
        result->simulator_version = "VCD";
        result->time = std::move(vcd.time);
        result->traces = std::move(vcd.traces);
        result->warnings = std::move(vcd.warnings);
        applySimulationResultToGraphTabs(result);
        if (simulationTabs_ && simulationGraphsPage_)
            simulationTabs_->setCurrentWidget(simulationGraphsPage_);
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        statusBar()->showMessage(
            QStringLiteral("Loaded VCD %1 (%2 traces)").arg(QFileInfo(path).fileName()).arg(result->traces.size()));
    }

    void runSimulation() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        if (selectedSimulationSchematicId() == 0) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a schematic first."));
            return;
        }
        const auto simIt = project_->project().simulations.find(activeSimulationId_);
        if (simIt == project_->project().simulations.end()) {
            activeSimulationId_ = 0;
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        const auto formSim = currentSimulationFormItem();
        // Honor edits currently visible in the form while retaining the
        // selected simulation's graphs.
        ice::SimulationItem sim = simIt->second;
        sim.schematic = formSim.schematic;
        sim.simulator = formSim.simulator;
        sim.analysis = formSim.analysis;
        sim.stop = formSim.stop;
        sim.step = formSim.step;
        sim.use_initial_conditions = formSim.use_initial_conditions;
        sim.directives = formSim.directives;
        sim.captureSignals = formSim.captureSignals;
        simulationOutput_->setPlainText(QStringLiteral("Running simulation..."));
        documents_->setCurrentWidget(simulationPage_);
        if (simulationTabs_ && simulationGraphsPage_)
            simulationTabs_->setCurrentWidget(simulationGraphsPage_);
        QApplication::processEvents();

        QStringList netlistNotes;
        ice::SimulationRequest request;
        try {
            request = buildSimulationRequest(sim, netlistNotes);
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Run Simulation"), QString::fromStdString(error.what()));
            return;
        }
        const auto timeout = request.simulator == "mixed" ? std::chrono::seconds(120) : std::chrono::seconds(10);
        auto result = ice::SimulationRunner().run(request, timeout);

        // Populate empty capture and graph fields from the generated traces so
        // the first run produces a usable plot, including for digital VCDs.
        bool graphsUpdated = false;
        if (result.state == ice::JobState::succeeded && !result.traces.empty()) {
            auto updatedSim = sim;
            if (ice::populateSimulationSignalsFromTraces(updatedSim, result.traces)) {
                project_->execute("populate simulation trace signals", [&](ice::Project &project) {
                    auto it = project.simulations.find(sim.id);
                    if (it != project.simulations.end())
                        ice::populateSimulationSignalsFromTraces(it->second, result.traces);
                });
                graphsUpdated = true;
                if (const auto updated = project_->project().simulations.find(sim.id);
                    updated != project_->project().simulations.end())
                    sim = updated->second;
            }
        }
        auto report = formatSimulationReport(request, result, netlistNotes);
        QString storedPath;

        const auto projectRoot = projectPath_.isEmpty()
                                     ? std::filesystem::path{}
                                     : std::filesystem::path(QFileInfo(projectPath_).absolutePath().toStdString());
        if (!projectRoot.empty()) {
            ice::SimulationRunContext context;
            context.projectRoot = projectRoot;
            context.projectIdentity = project_->project().identity;
            context.projectName = project_->project().name;
            context.simulationId = sim.id;
            context.simulationName = sim.name;
            context.schematicId = sim.schematic;
            if (const auto schematic = project_->project().schematicCells.find(sim.schematic);
                schematic != project_->project().schematicCells.end())
                context.schematicName = schematic->second.name;
            context.request = request;
            context.directives = sim.directives;
            context.netlistNotes.reserve(static_cast<std::size_t>(netlistNotes.size()));
            for (const auto &note : netlistNotes)
                context.netlistNotes.push_back(note.toStdString());
            context.graphs = sim.graphs;
            try {
                const auto artifact = ice::writeSimulationRun(context, result);
                storedPath = QString::fromStdString(artifact.path.string());
                report += QStringLiteral("\nStored result: %1\n").arg(storedPath);
            } catch (const ice::ProjectError &error) {
                report += QStringLiteral("\nUnable to store result: %1\n").arg(QString::fromStdString(error.what()));
            }
        } else {
            report += QStringLiteral("\nResult not stored: save the project first.\n");
        }
        simulationOutput_->setPlainText(report);

        if (graphsUpdated) {
            QSignalBlocker blocker(signalsCapture_);
            signalsCapture_->setText(QString::fromStdString(sim.captureSignals));
            if (simulationPanel_)
                simulationPanel_->setProject(project_->project());
            rebuildGraphTabs(sim.graphs);
        }

        // Show result in the currently active graph tab.
        if (graphTabs_) {
            auto resultPtr = std::make_shared<ice::SimulationResult>(std::move(result));
            if (graphsUpdated)
                applySimulationResultToGraphTabs(resultPtr);
            else if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->currentWidget()))
                plot->setResult(resultPtr);
        }
        statusBar()->showMessage(storedPath.isEmpty() ? QStringLiteral("Simulation finished")
                                                      : QStringLiteral("Simulation stored: %1").arg(storedPath));
    }

    void ensureDrcProcess() {
        if (drcProcess_)
            return;
        drcProcess_ = new QProcess(this);
        drcProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(drcProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            drcLog_ += QString::fromLocal8Bit(drcProcess_->readAllStandardOutput());
            constexpr int maximumLogSize = 200000;
            if (drcLog_.size() > maximumLogSize)
                drcLog_ = drcLog_.right(maximumLogSize);
        });
        connect(drcProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            const auto message = QStringLiteral("Unable to start gdscheck: %1").arg(drcProcess_->errorString());
            if (activityPanel_)
                activityPanel_->setDrcError(message);
            statusBar()->showMessage(QStringLiteral("DRC failed: gdscheck could not be started"));
        });
        connect(drcProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus status) { finishDrc(exitCode, status); });
    }

    void runDrc() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run DRC"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (drcProcess_ && drcProcess_->state() != QProcess::NotRunning) {
            statusBar()->showMessage(QStringLiteral("DRC is already running"));
            return;
        }
        const auto &project = project_->project();
        if (project.layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Run DRC"),
                                     QStringLiteral("Create or import a layout cell first."));
            return;
        }
        auto cellId = activeLayoutId_;
        if (!project.layoutCells.contains(cellId))
            cellId = project.layoutCells.begin()->first;
        const auto &cell = project.layoutCells.at(cellId);
        const auto processName = ice::gdsCheckProcessForPdk(cell.pdk);
        if (processName.empty()) {
            QMessageBox::warning(
                this, QStringLiteral("Run DRC"),
                QStringLiteral("The active layout cell uses the Generic PDK. gdscheck requires a PDK process deck. "
                               "Create the layout with an installed PDK such as IHP SG13G2 first."));
            return;
        }

        auto temporary = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/ice-drc-XXXXXX"));
        if (!temporary->isValid()) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"),
                                  QStringLiteral("Unable to create a temporary directory for the DRC run."));
            return;
        }
        const auto gdsPath = temporary->filePath(QStringLiteral("layout.gds"));
        const auto reportPath = temporary->filePath(QStringLiteral("drc.lyrdb"));
        const bool flattened = flattenedLayouts_->contains(cellId);
        try {
            if (flattened)
                ice::LayoutInterchange::exportFlattenedGds(project, cellId, gdsPath.toStdString());
            else
                ice::LayoutInterchange::exportGds(project, cellId, gdsPath.toStdString());
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"), QString::fromStdString(error.what()));
            return;
        } catch (const std::exception &error) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"), QString::fromUtf8(error.what()));
            return;
        }

        ensureDrcProcess();
        drcTempDir_ = std::move(temporary);
        drcReportPath_ = reportPath;
        drcLog_.clear();
        const ice::GdsCheckOptions options{gdsPath.toStdString(), reportPath.toStdString(), processName, "TOP", "main"};
        QStringList arguments;
        for (const auto &argument : ice::gdsCheckArguments(options))
            arguments << QString::fromStdString(argument);
        drcProcess_->setProgram(QStringLiteral("gdscheck"));
        drcProcess_->setArguments(arguments);
        drcProcess_->setWorkingDirectory(drcTempDir_->path());

        if (activityDock_)
            activityDock_->show();
        if (activityPanel_) {
            activityPanel_->setDrcRunning(QStringLiteral("Running gdscheck for %1 with process %2%3...")
                                              .arg(QString::fromStdString(cell.name),
                                                   QString::fromStdString(processName),
                                                   flattened ? QStringLiteral(" (flattened)") : QString()));
            activityPanel_->showDrcTab();
        }
        statusBar()->showMessage(QStringLiteral("Running DRC with gdscheck..."));
        drcProcess_->start();
    }

    void finishDrc(int exitCode, QProcess::ExitStatus status) {
        const bool reportExists = !drcReportPath_.isEmpty() && QFileInfo::exists(drcReportPath_);
        if (reportExists) {
            try {
                const auto report = ice::parseGdsCheckLyrdb(drcReportPath_.toStdString());
                if (activityPanel_)
                    activityPanel_->setDrcReport(report, drcReportPath_);
                if (activityDock_)
                    activityDock_->show();
                if (activityPanel_)
                    activityPanel_->showDrcTab();
                const auto violations = report.drc.size();
                statusBar()->showMessage(violations == 0
                                             ? QStringLiteral("DRC finished: no violations")
                                             : QStringLiteral("DRC finished: %1 violation(s)").arg(violations));
                return;
            } catch (const ice::ProjectError &error) {
                const auto message =
                    QStringLiteral("Unable to parse the gdscheck report: %1").arg(QString::fromStdString(error.what()));
                if (activityPanel_)
                    activityPanel_->setDrcError(message);
                statusBar()->showMessage(QStringLiteral("DRC report parsing failed"));
                return;
            }
        }

        auto details = drcLog_.trimmed();
        if (details.size() > 2000)
            details = details.right(2000);
        if (details.isEmpty())
            details = QStringLiteral("gdscheck did not generate a report.");
        if (status == QProcess::CrashExit)
            details.prepend(QStringLiteral("gdscheck crashed. "));
        const auto message = QStringLiteral("DRC failed (exit code %1): %2").arg(exitCode).arg(details);
        if (activityPanel_)
            activityPanel_->setDrcError(message);
        if (activityDock_)
            activityDock_->show();
        if (activityPanel_)
            activityPanel_->showDrcTab();
        statusBar()->showMessage(QStringLiteral("DRC failed"));
    }

    void runLvs() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run LVS"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto &project = project_->project();
        if (project.layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Run LVS"),
                                     QStringLiteral("Create or import a layout cell first."));
            return;
        }
        if (project.schematicCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Run LVS"), QStringLiteral("Create a schematic cell first."));
            return;
        }

        auto layoutId = activeLayoutId_;
        if (!project.layoutCells.contains(layoutId))
            layoutId = project.layoutCells.begin()->first;
        const auto &layout = project.layoutCells.at(layoutId);
        if (layout.library) {
            QMessageBox::information(this, QStringLiteral("Run LVS"),
                                     QStringLiteral("LVS compares user layout cells, not library cells."));
            return;
        }

        auto schematicId = ice::schematicForLayout(project, layoutId);
        if (schematicId == 0) {
            std::vector<std::pair<QString, ice::ObjectId>> choices;
            choices.reserve(project.schematicCells.size());
            for (const auto &[id, cell] : project.schematicCells)
                choices.emplace_back(QString::fromStdString(cell.name), id);
            std::sort(choices.begin(), choices.end(),
                      [](const auto &left, const auto &right) { return left.first < right.first; });
            QStringList names;
            int current = 0;
            for (std::size_t index = 0; index < choices.size(); ++index) {
                names << choices[index].first;
                if (choices[index].second == activeSchematicId_)
                    current = static_cast<int>(index);
            }
            bool accepted = false;
            const auto selected = QInputDialog::getItem(this, QStringLiteral("Run LVS"), QStringLiteral("Schematic:"),
                                                        names, current, false, &accepted);
            if (!accepted)
                return;
            schematicId = choices[static_cast<std::size_t>(names.indexOf(selected))].second;
        }

        try {
            const auto report = ice::runLvs(project, schematicId, layoutId);
            if (activityPanel_)
                activityPanel_->setLvsReport(report);
            if (activityDock_)
                activityDock_->show();
            if (activityPanel_)
                activityPanel_->showLvsTab();

            int errors = 0;
            int warnings = 0;
            for (const auto &mismatch : report.mismatches) {
                if (mismatch.severity == ice::VerificationSeverity::error)
                    ++errors;
                else if (mismatch.severity == ice::VerificationSeverity::warning)
                    ++warnings;
            }
            if (report.matched()) {
                statusBar()->showMessage(
                    QStringLiteral("LVS matched: %1 vs %2")
                        .arg(QString::fromStdString(report.schematic), QString::fromStdString(report.layout)));
            } else {
                statusBar()->showMessage(
                    QStringLiteral("LVS completed: %1 error(s), %2 warning(s)").arg(errors).arg(warnings));
            }
        } catch (const ice::ProjectError &error) {
            const auto message = QString::fromStdString(error.what());
            if (activityPanel_)
                activityPanel_->setLvsError(message);
            if (activityDock_)
                activityDock_->show();
            QMessageBox::critical(this, QStringLiteral("Run LVS"), message);
        } catch (const std::exception &error) {
            const auto message = QString::fromUtf8(error.what());
            if (activityPanel_)
                activityPanel_->setLvsError(message);
            if (activityDock_)
                activityDock_->show();
            QMessageBox::critical(this, QStringLiteral("Run LVS"), message);
        }
    }

    void runPex() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run PEX"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto &project = project_->project();
        if (project.layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Run PEX"),
                                     QStringLiteral("Create or import a layout cell first."));
            return;
        }

        auto layoutId = activeLayoutId_;
        if (const auto procedural = project.proceduralLayouts.find(layoutId);
            procedural != project.proceduralLayouts.end() &&
            project.layoutCells.contains(procedural->second.generatedCell))
            layoutId = procedural->second.generatedCell;
        if (!project.layoutCells.contains(layoutId)) {
            const auto candidate = std::find_if(project.layoutCells.begin(), project.layoutCells.end(),
                                                [](const auto &entry) { return !entry.second.library; });
            if (candidate == project.layoutCells.end()) {
                QMessageBox::information(this, QStringLiteral("Run PEX"),
                                         QStringLiteral("PEX requires a user layout cell, not a library cell."));
                return;
            }
            layoutId = candidate->first;
        }
        if (project.layoutCells.at(layoutId).library) {
            QMessageBox::information(this, QStringLiteral("Run PEX"),
                                     QStringLiteral("PEX extracts user layout cells, not library cells."));
            return;
        }

        try {
            ice::PexReport report;
            project_->execute("extract layout parasitics",
                              [&](ice::Project &mutableProject) { report = ice::runPex(mutableProject, layoutId); });
            project_->setLastHistoryLabel("Extracted parasitics from " + report.layout);
            activeSchematicId_ = report.schematicCell;
            refreshProject();
            selectSchematicTab(report.schematicCell);
            if (documents_ && schematicPage_)
                documents_->setCurrentWidget(schematicPage_);
            statusBar()->showMessage(QStringLiteral("PEX created %1: %2 net(s), %3 device(s), R=%4 ohm, C=%5 fF")
                                         .arg(QString::fromStdString(report.schematic))
                                         .arg(report.nets.size())
                                         .arg(report.devices.size())
                                         .arg(QString::number(report.totalResistance, 'g', 4))
                                         .arg(QString::number(report.totalCapacitance, 'g', 4)));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Run PEX"), QString::fromStdString(error.what()));
        } catch (const std::exception &error) {
            QMessageBox::critical(this, QStringLiteral("Run PEX"), QString::fromUtf8(error.what()));
        }
    }

    void refreshProject() {
        if (!project_)
            return;
        refreshSimulationSchematics();
        refreshEmbeddedPanels();
        refreshSchematic();
        refreshLayout();
        updateHistoryPanels();
        updateSimulationNetlistState();
    }

    struct SectionHistoryEntry {
        QString label;
        ice::Project project;
    };

    int historyScope(const QString &label) const {
        const auto lower = label.toLower();
        const bool layout = lower.contains(QStringLiteral("layout")) || lower.contains(QStringLiteral("layer")) ||
                            lower.contains(QStringLiteral("planned device")) ||
                            lower.contains(QStringLiteral("planned connection"));
        const bool schematic =
            !layout && (lower.contains(QStringLiteral("schematic")) || lower.contains(QStringLiteral("symbol")) ||
                        lower.contains(QStringLiteral("net")) || lower.contains(QStringLiteral("pin")) ||
                        lower.contains(QStringLiteral("label")) || lower.contains(QStringLiteral("parameter")) ||
                        lower.contains(QStringLiteral("rotat")) || lower.contains(QStringLiteral("flip")) ||
                        lower.contains(QStringLiteral("mov")) || lower.contains(QStringLiteral("delet")) ||
                        lower.contains(QStringLiteral("connect")) || lower.startsWith(QStringLiteral("placed ")));
        return (layout ? 2 : 0) | (schematic || lower.contains(QStringLiteral("pdk technology")) ? 1 : 0);
    }

    void resetSectionHistories() {
        schematicRouteCache_.clear();
        schematicHistory_.clear();
        layoutHistory_.clear();
        schematicHistoryPosition_ = 0;
        layoutHistoryPosition_ = 0;
        syncedGlobalHistoryPosition_ = project_ ? project_->historyPosition() : 0;
        const auto snapshot = project_ ? project_->project() : ice::Project{};
        schematicHistory_.push_back({QStringLiteral("Initial state"), snapshot});
        layoutHistory_.push_back({QStringLiteral("Initial state"), snapshot});
    }

    void syncSectionHistories() {
        if (!project_)
            return;
        const auto globalPosition = project_->historyPosition();
        if (globalPosition < syncedGlobalHistoryPosition_) {
            resetSectionHistories();
            return;
        }
        const auto &history = project_->history();
        while (syncedGlobalHistoryPosition_ < globalPosition && syncedGlobalHistoryPosition_ < history.size()) {
            const auto label = QString::fromStdString(history[syncedGlobalHistoryPosition_]);
            const auto scope = historyScope(label);
            if (scope & 1) {
                schematicHistory_.resize(schematicHistoryPosition_ + 1);
                schematicHistory_.push_back({label, project_->project()});
                ++schematicHistoryPosition_;
            }
            if (scope & 2) {
                layoutHistory_.resize(layoutHistoryPosition_ + 1);
                layoutHistory_.push_back({label, project_->project()});
                ++layoutHistoryPosition_;
            }
            ++syncedGlobalHistoryPosition_;
        }
    }

    void restoreSectionHistory(bool schematic, std::size_t position) {
        if (!project_)
            return;
        auto restored = project_->project();
        const auto &entries = schematic ? schematicHistory_ : layoutHistory_;
        if (position >= entries.size())
            return;
        if (schematic)
            restored.schematicCells = entries[position].project.schematicCells;
        else {
            restored.layers = entries[position].project.layers;
            restored.layoutCells = entries[position].project.layoutCells;
            restored.proceduralLayouts = entries[position].project.proceduralLayouts;
        }
        project_->replaceProject(std::move(restored));
        schematicRouteCache_.clear();
        if (schematic)
            schematicHistoryPosition_ = position;
        else
            layoutHistoryPosition_ = position;
        refreshProject();
    }

    void undoCurrentSection() {
        syncSectionHistories();
        const bool schematic = !documents_ || documents_->currentWidget() == schematicPage_;
        auto &position = schematic ? schematicHistoryPosition_ : layoutHistoryPosition_;
        if (position > 0)
            restoreSectionHistory(schematic, position - 1);
    }

    void redoCurrentSection() {
        syncSectionHistories();
        const bool schematic = !documents_ || documents_->currentWidget() == schematicPage_;
        const auto &entries = schematic ? schematicHistory_ : layoutHistory_;
        auto &position = schematic ? schematicHistoryPosition_ : layoutHistoryPosition_;
        if (position + 1 < entries.size())
            restoreSectionHistory(schematic, position + 1);
    }

    void updateHistoryPanels() {
        if (!schematicHistoryPanel_ || !layoutHistoryPanel_)
            return;
        syncSectionHistories();
        QStringList schematic;
        QStringList layout;
        std::vector<std::size_t> schematicPositions;
        std::vector<std::size_t> layoutPositions;
        for (std::size_t index = 1; index <= schematicHistoryPosition_; ++index) {
            schematic.append(schematicHistory_[index].label);
            schematicPositions.push_back(index);
        }
        for (std::size_t index = 1; index <= layoutHistoryPosition_; ++index) {
            layout.append(layoutHistory_[index].label);
            layoutPositions.push_back(index);
        }
        schematicHistoryPanel_->setHistory(schematic, schematicPositions);
        layoutHistoryPanel_->setHistory(layout, layoutPositions);
    }

    void jumpToSchematicHistory(std::size_t position) { restoreSectionHistory(true, position); }
    void jumpToLayoutHistory(std::size_t position) { restoreSectionHistory(false, position); }

    static std::string schematicFileSignature(const ice::SchematicCell &cell) {
        ice::Project project;
        project.schematicCells.emplace(cell.id, cell);
        return project.serialize();
    }

    static std::string layoutFileSignature(const ice::LayoutCell &cell) {
        ice::Project project;
        project.layoutCells.emplace(cell.id, cell);
        return project.serialize();
    }

    static std::string proceduralFileSignature(const ice::ProceduralLayout &layout) {
        ice::Project project;
        project.proceduralLayouts.emplace(layout.id, layout);
        return project.serialize();
    }

    void captureSavedFileBaselines() {
        savedSchematicSignatures_.clear();
        savedLayoutSignatures_.clear();
        savedProceduralSignatures_.clear();
        if (!project_)
            return;
        for (const auto &[id, cell] : project_->project().schematicCells)
            savedSchematicSignatures_[id] = schematicFileSignature(cell);
        for (const auto &[id, cell] : project_->project().layoutCells)
            savedLayoutSignatures_[id] = layoutFileSignature(cell);
        for (const auto &[id, layout] : project_->project().proceduralLayouts)
            savedProceduralSignatures_[id] = proceduralFileSignature(layout);
    }

    bool schematicFileDirty(ice::ObjectId id) const {
        if (!project_)
            return false;
        const auto cell = project_->project().schematicCells.find(id);
        if (cell == project_->project().schematicCells.end())
            return false;
        const auto saved = savedSchematicSignatures_.find(id);
        return saved == savedSchematicSignatures_.end() || schematicFileSignature(cell->second) != saved->second;
    }

    bool layoutFileDirty(ice::ObjectId id) const {
        if (!project_)
            return false;
        const auto procedural = project_->project().proceduralLayouts.find(id);
        if (procedural != project_->project().proceduralLayouts.end()) {
            const auto saved = savedProceduralSignatures_.find(id);
            return saved == savedProceduralSignatures_.end() ||
                   proceduralFileSignature(procedural->second) != saved->second;
        }
        const auto cell = project_->project().layoutCells.find(id);
        if (cell == project_->project().layoutCells.end())
            return false;
        const auto saved = savedLayoutSignatures_.find(id);
        return saved == savedLayoutSignatures_.end() || layoutFileSignature(cell->second) != saved->second;
    }

    static QString fileTabLabel(const QString &name, const QString &extension, bool dirty, bool conflict) {
        QString label = name + QLatin1Char('.') + extension;
        if (conflict)
            label.prepend(QStringLiteral("⚠ "));
        if (dirty)
            label.prepend(QStringLiteral("● "));
        return label;
    }

    static QString schematicFileExtension(const ice::SchematicCell &cell) {
        if (cell.systemVerilogSchematic)
            return QStringLiteral("sv");
        return cell.textSchematic ? QStringLiteral("sp") : QStringLiteral("icesch");
    }

    static TextDocumentLanguage textSourceLanguage(const ice::SchematicCell &cell) {
        return cell.systemVerilogSchematic ? TextDocumentLanguage::SystemVerilog : TextDocumentLanguage::Spice;
    }

    void syncOpenFileTabs() {
        if (!project_) {
            openSchematicTabs_.clear();
            openLayoutTabs_.clear();
            knownSchematicCells_.clear();
            knownLayoutCells_.clear();
            knownProceduralLayouts_.clear();
            return;
        }
        std::set<ice::ObjectId> schematicIds;
        ice::ObjectId newlyDiscoveredSchematic = 0;
        for (const auto &[id, _] : project_->project().schematicCells) {
            schematicIds.insert(id);
            if (knownSchematicCells_.insert(id).second) {
                openSchematicTabs_.insert(id);
                newlyDiscoveredSchematic = id;
            }
        }
        for (auto it = knownSchematicCells_.begin(); it != knownSchematicCells_.end();) {
            if (!schematicIds.contains(*it)) {
                openSchematicTabs_.erase(*it);
                it = knownSchematicCells_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = openSchematicTabs_.begin(); it != openSchematicTabs_.end();) {
            if (!schematicIds.contains(*it))
                it = openSchematicTabs_.erase(it);
            else
                ++it;
        }
        if (newlyDiscoveredSchematic != 0)
            activeSchematicId_ = newlyDiscoveredSchematic;

        std::set<ice::ObjectId> layoutIds;
        for (const auto &[id, _] : project_->project().proceduralLayouts) {
            layoutIds.insert(id);
            if (knownProceduralLayouts_.insert(id).second)
                openLayoutTabs_.insert(id);
        }
        for (auto it = knownProceduralLayouts_.begin(); it != knownProceduralLayouts_.end();) {
            if (!project_->project().proceduralLayouts.contains(*it))
                it = knownProceduralLayouts_.erase(it);
            else
                ++it;
        }
        for (const auto &[id, cell] : project_->project().layoutCells) {
            layoutIds.insert(id);
            if (knownLayoutCells_.insert(id).second && !cell.library)
                openLayoutTabs_.insert(id);
        }
        for (auto it = knownLayoutCells_.begin(); it != knownLayoutCells_.end();) {
            if (!project_->project().layoutCells.contains(*it))
                it = knownLayoutCells_.erase(it);
            else
                ++it;
        }
        for (auto it = openLayoutTabs_.begin(); it != openLayoutTabs_.end();) {
            if (!layoutIds.contains(*it))
                it = openLayoutTabs_.erase(it);
            else
                ++it;
        }
    }

    void updateFileTabDirtyIndicators() {
        if (!project_)
            return;
        if (schematicCellTabs_) {
            for (int index = 0; index < schematicCellTabs_->count(); ++index) {
                const auto id = static_cast<ice::ObjectId>(schematicCellTabs_->tabBar()->tabData(index).toULongLong());
                const auto cell = project_->project().schematicCells.find(id);
                if (cell == project_->project().schematicCells.end())
                    continue;
                const bool conflict = conflictedSchematicFiles_.contains(id);
                schematicCellTabs_->setTabText(index, fileTabLabel(QString::fromStdString(cell->second.name),
                                                                   schematicFileExtension(cell->second),
                                                                   schematicFileDirty(id), conflict));
            }
        }
        if (layoutCellTabs_) {
            for (int index = 0; index < layoutCellTabs_->count(); ++index) {
                const auto id = static_cast<ice::ObjectId>(layoutCellTabs_->tabBar()->tabData(index).toULongLong());
                const auto procedural = project_->project().proceduralLayouts.find(id);
                if (procedural != project_->project().proceduralLayouts.end()) {
                    const bool conflict = conflictedProceduralFiles_.contains(id);
                    layoutCellTabs_->setTabText(index,
                                                fileTabLabel(QString::fromStdString(procedural->second.name),
                                                             QStringLiteral("py"), layoutFileDirty(id), conflict));
                    continue;
                }
                const auto cell = project_->project().layoutCells.find(id);
                if (cell == project_->project().layoutCells.end())
                    continue;
                const bool conflict = conflictedLayoutFiles_.contains(id);
                layoutCellTabs_->setTabText(index,
                                            fileTabLabel(QString::fromStdString(cell->second.name),
                                                         QStringLiteral("icelay"), layoutFileDirty(id), conflict));
            }
        }
    }

    void openSchematicFile(ice::ObjectId id) {
        if (!project_ || !project_->project().schematicCells.contains(id))
            return;
        openSchematicTabs_.insert(id);
        knownSchematicCells_.insert(id);
        refreshEmbeddedPanels();
        selectSchematicTab(id);
        if (documents_ && schematicPage_)
            documents_->setCurrentWidget(schematicPage_);
    }

    void openLayoutFile(ice::ObjectId id) {
        if (!project_)
            return;
        if (!project_->project().layoutCells.contains(id) && !project_->project().proceduralLayouts.contains(id))
            return;
        openLayoutTabs_.insert(id);
        knownLayoutCells_.insert(id);
        knownProceduralLayouts_.insert(id);
        refreshEmbeddedPanels();
        selectLayoutTab(id);
        if (documents_ && layoutPage_)
            documents_->setCurrentWidget(layoutPage_);
    }

    void refreshEmbeddedPanels() {
        const ice::Project empty;
        const auto &project = project_ ? project_->project() : empty;
        syncOpenFileTabs();
        if (activeSchematicId_ != 0 && !openSchematicTabs_.contains(activeSchematicId_))
            activeSchematicId_ = 0;
        if (activeProceduralLayoutId_ != 0 && !openLayoutTabs_.contains(activeProceduralLayoutId_))
            activeProceduralLayoutId_ = 0;
        if (activeLayoutId_ != 0 && !openLayoutTabs_.contains(activeLayoutId_))
            activeLayoutId_ = 0;
        if (schematicHierarchy_)
            schematicHierarchy_->setProject(project);
        if (layoutHierarchy_)
            layoutHierarchy_->setProject(project);
        if (layersPanel_)
            layersPanel_->setProject(project, activeLayoutId_);
        if (schematicCellTabs_) {
            const auto selected = activeSchematicId_;
            schematicCellTabs_->clear();
            for (const auto &[id, cell] : project.schematicCells) {
                if (!openSchematicTabs_.contains(id))
                    continue;
                const int tab = schematicCellTabs_->count();
                schematicCellTabs_->addTab(
                    new QWidget, fileTabLabel(QString::fromStdString(cell.name), schematicFileExtension(cell),
                                              schematicFileDirty(id), conflictedSchematicFiles_.contains(id)));
                schematicCellTabs_->tabBar()->setTabData(tab, QVariant::fromValue<qulonglong>(id));
                const auto pdk = cell.pdk.empty() || cell.pdk == ice::genericPdkId ? QStringLiteral("Generic PDK")
                                                                                   : QString::fromStdString(cell.pdk);
                const auto tooltip =
                    cell.systemVerilogSchematic
                        ? QStringLiteral("SystemVerilog · PDK: %1").arg(pdk)
                        : (cell.textSchematic ? QStringLiteral("Text SPICE schematic · PDK: %1").arg(pdk)
                                              : QStringLiteral("PDK: %1").arg(pdk));
                schematicCellTabs_->setTabToolTip(tab, tooltip);
            }
            bool selectedTab = false;
            for (int index = 0; index < schematicCellTabs_->count(); ++index) {
                if (schematicCellTabs_->tabBar()->tabData(index).toULongLong() != selected)
                    continue;
                if (schematicCellTabs_->currentIndex() == index)
                    activateSchematicTab(index);
                else
                    schematicCellTabs_->setCurrentIndex(index);
                selectedTab = true;
                break;
            }
            // Newly discovered external files are opened automatically. When
            // there was no active tab, activate the tab Qt selected so text
            // schematics switch to the source editor instead of leaving the
            // canvas visible.
            if (!selectedTab && selected == 0 && schematicCellTabs_->count() > 0) {
                int fallback = schematicCellTabs_->currentIndex();
                if (fallback < 0)
                    fallback = 0;
                if (schematicCellTabs_->currentIndex() == fallback)
                    activateSchematicTab(fallback);
                else
                    schematicCellTabs_->setCurrentIndex(fallback);
            }
        }
        if (layoutCellTabs_) {
            if (activeProceduralLayoutId_ != 0 && !project.proceduralLayouts.contains(activeProceduralLayoutId_))
                activeProceduralLayoutId_ = 0;
            const auto selected = activeProceduralLayoutId_ != 0 ? activeProceduralLayoutId_ : activeLayoutId_;
            int selectedIndex = -1;
            {
                // Rebuilding the tab bar must not emit transient currentChanged
                // signals. Adding the first tab to an empty QTabWidget would
                // otherwise activate layout 0, switching away from the layout
                // being edited and cancelling an in-progress path.
                const QSignalBlocker blocker(layoutCellTabs_);
                layoutCellTabs_->clear();
                for (const auto &[id, procedural] : project.proceduralLayouts) {
                    if (!openLayoutTabs_.contains(id))
                        continue;
                    const auto index = layoutCellTabs_->count();
                    layoutCellTabs_->addTab(new QWidget,
                                            fileTabLabel(QString::fromStdString(procedural.name), QStringLiteral("py"),
                                                         layoutFileDirty(id), conflictedProceduralFiles_.contains(id)));
                    layoutCellTabs_->tabBar()->setTabData(index, QVariant::fromValue<qulonglong>(id));
                    layoutCellTabs_->setTabToolTip(index, QStringLiteral("Procedural layout (Python script)"));
                }
                for (const auto &[id, cell] : project.layoutCells) {
                    if (!openLayoutTabs_.contains(id))
                        continue;
                    const auto index = layoutCellTabs_->count();
                    layoutCellTabs_->addTab(new QWidget,
                                            fileTabLabel(QString::fromStdString(cell.name), QStringLiteral("icelay"),
                                                         layoutFileDirty(id), conflictedLayoutFiles_.contains(id)));
                    layoutCellTabs_->tabBar()->setTabData(index, QVariant::fromValue<qulonglong>(id));
                    if (cell.generatedBy != 0) {
                        const auto procedural = project.proceduralLayouts.find(cell.generatedBy);
                        if (procedural != project.proceduralLayouts.end())
                            layoutCellTabs_->setTabToolTip(index,
                                                           QStringLiteral("Generated by procedural layout '%1'")
                                                               .arg(QString::fromStdString(procedural->second.name)));
                    }
                }
                for (int index = 0; index < layoutCellTabs_->count(); ++index) {
                    if (layoutCellTabs_->tabBar()->tabData(index).toULongLong() != selected)
                        continue;
                    selectedIndex = index;
                    layoutCellTabs_->setCurrentIndex(index);
                    break;
                }
            }
            if (selectedIndex >= 0) {
                activateLayoutTab(selectedIndex);
            } else if (selected == 0 && layoutCellTabs_->count() > 0) {
                int fallback = layoutCellTabs_->currentIndex();
                if (fallback < 0)
                    fallback = 0;
                layoutCellTabs_->setCurrentIndex(fallback);
                activateLayoutTab(fallback);
            }
        }
        // Refresh simulation panel and project-level simulation scripts.
        if (simulationPanel_)
            simulationPanel_->setProject(project);
        refreshReportPanel();
        updateEmptyWorkspaceState();
    }

    void refreshSimulationSchematics() {
        if (!schematicSelector_)
            return;
        const auto selected = schematicSelector_->currentData().toULongLong();
        QSignalBlocker blocker(schematicSelector_);
        schematicSelector_->clear();
        if (project_) {
            for (const auto &[id, cell] : project_->project().schematicCells)
                schematicSelector_->addItem(QString::fromStdString(cell.name), QVariant::fromValue<qulonglong>(id));
        }
        const int index = schematicSelector_->findData(QVariant::fromValue<qulonglong>(selected));
        if (index >= 0)
            schematicSelector_->setCurrentIndex(index);
        updateSimulationNetlistState();
    }

    static std::optional<QPointF> pinScenePosition(const ice::SchematicCell &cell, ice::ObjectId pinId) {
        for (const auto &[_, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            for (const auto &pin : symbol->second.pins) {
                if (pin.id != pinId)
                    continue;
                const auto local =
                    rotatePoint({static_cast<qreal>(pin.x), -static_cast<qreal>(pin.y)}, instance.orientation);
                return QPointF(instance.x + local.x(), -instance.y + local.y());
            }
        }
        return std::nullopt;
    }

    static void updateAutomaticNetLabelPositions(ice::Project &project) {
        for (auto &[_, cell] : project.schematicCells) {
            for (auto &[_, net] : cell.nets) {
                if (net.pins.empty())
                    continue;
                std::vector<QPointF> positions;
                for (const auto pinId : net.pins)
                    if (const auto position = pinScenePosition(cell, pinId))
                        positions.push_back(*position);
                if (positions.empty())
                    continue;
                QPointF center;
                for (const auto position : positions)
                    center += position;
                center /= static_cast<qreal>(positions.size());
                net.x = static_cast<ice::Coordinate>(center.x());
                net.y = static_cast<ice::Coordinate>(-center.y() + 18.0);
            }
        }
    }

    static std::string lowerText(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value;
    }

    static std::optional<std::string> instanceParameter(const ice::SchematicInstance &instance,
                                                        std::initializer_list<const char *> names) {
        for (const auto &parameter : instance.parameters) {
            const auto key = lowerText(parameter.name);
            for (const auto *name : names)
                if (key == name)
                    return parameter.value;
        }
        return std::nullopt;
    }

    static bool isSchematicSourceSymbol(const ice::Symbol &symbol, const ice::SchematicInstance &instance) {
        const auto name = lowerText(symbol.name);
        if (name.find("source") != std::string::npos || name.find("battery") != std::string::npos)
            return true;
        return instanceParameter(instance, {"voltage", "current", "amplitude", "pulsed_voltage"}).has_value();
    }

    static std::optional<double> parseQuantityValue(const std::string &text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::nullopt;
        const auto last = text.find_last_not_of(" \t\r\n");
        const auto trimmed = text.substr(first, last - first + 1);
        try {
            std::size_t consumed = 0;
            const auto value = std::stod(trimmed, &consumed);
            while (consumed < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[consumed])))
                ++consumed;
            double scale = 1.0;
            if (consumed < trimmed.size()) {
                switch (trimmed[consumed]) {
                case 'f':
                    scale = 1.0e-15;
                    break;
                case 'p':
                    scale = 1.0e-12;
                    break;
                case 'n':
                    scale = 1.0e-9;
                    break;
                case 'u':
                    scale = 1.0e-6;
                    break;
                case 'm':
                    scale = 1.0e-3;
                    break;
                case 'k':
                    scale = 1.0e3;
                    break;
                case 'M':
                    scale = 1.0e6;
                    break;
                case 'G':
                    scale = 1.0e9;
                    break;
                default:
                    return std::nullopt;
                }
                ++consumed;
                while (consumed < trimmed.size() && std::isalpha(static_cast<unsigned char>(trimmed[consumed])))
                    ++consumed;
                if (consumed != trimmed.size())
                    return std::nullopt;
            }
            if (!std::isfinite(value))
                return std::nullopt;
            return value * scale;
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }

    static QString formatQuantityValue(double value) {
        if (!std::isfinite(value))
            return {};
        constexpr std::array<std::pair<double, const char *>, 9> units = {
            std::pair{1.0e9, "G"},  std::pair{1.0e6, "M"},   std::pair{1.0e3, "k"},
            std::pair{1.0, ""},     std::pair{1.0e-3, "m"},  std::pair{1.0e-6, "u"},
            std::pair{1.0e-9, "n"}, std::pair{1.0e-12, "p"}, std::pair{1.0e-15, "f"}};
        const auto magnitude = std::abs(value);
        for (const auto &[scale, suffix] : units) {
            if (magnitude >= scale || scale == 1.0e-15)
                return QString::number(value / scale, 'g', 4) + QString::fromLatin1(suffix);
        }
        return QString::number(value, 'g', 4);
    }

    static QString withUnit(const std::string &value, const QString &unit) {
        const auto text = QString::fromStdString(value).trimmed();
        if (text.endsWith(unit, Qt::CaseInsensitive))
            return text;
        return text + QLatin1Char(' ') + unit;
    }

    static QRectF svgVisibleBounds(QSvgRenderer &renderer, const QSizeF &size) {
        if (!renderer.isValid() || !size.isValid() || size.isEmpty())
            return {};
        constexpr qreal maxDimension = 512.0;
        const auto scale = std::min(maxDimension / std::max(size.width(), size.height()), 8.0);
        const auto width = std::max(1, qRound(size.width() * scale));
        const auto height = std::max(1, qRound(size.height() * scale));
        QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter, QRectF(0.0, 0.0, width, height));
        painter.end();
        int minX = width;
        int minY = height;
        int maxX = -1;
        int maxY = -1;
        for (int y = 0; y < height; ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                if (qAlpha(line[x]) <= 8)
                    continue;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
        if (maxX < minX || maxY < minY)
            return {};
        return QRectF(minX / scale, minY / scale, (maxX - minX + 1) / scale, (maxY - minY + 1) / scale);
    }

    const QRectF &schematicArtworkVisibleBounds(QSvgRenderer &renderer, const std::string &artwork,
                                                const QSizeF &size) {
        if (const auto cached = schematicArtworkBoundsCache_.find(artwork);
            cached != schematicArtworkBoundsCache_.end())
            return cached->second;
        return schematicArtworkBoundsCache_.emplace(artwork, svgVisibleBounds(renderer, size)).first->second;
    }

    struct SchematicValueLabel {
        QString text;
        bool onRight = false;
    };

    static SchematicValueLabel schematicValueLabel(const ice::Symbol &symbol, const ice::SchematicInstance &instance) {
        if (const auto value = instanceParameter(instance, {"resistance"}))
            return {QString::fromStdString(*value), false};
        if (const auto value = instanceParameter(instance, {"capacitance"}))
            return {QString::fromStdString(*value), false};
        if (const auto value = instanceParameter(instance, {"inductance"}))
            return {QString::fromStdString(*value), false};

        const auto name = lowerText(symbol.name);
        if (name.find("sine") != std::string::npos) {
            QStringList lines;
            if (const auto amplitude = instanceParameter(instance, {"amplitude"}))
                lines.append(QStringLiteral("A = %1").arg(withUnit(*amplitude, QStringLiteral("V"))));
            if (const auto frequency = instanceParameter(instance, {"frequency", "freq"})) {
                if (const auto parsed = parseQuantityValue(*frequency); parsed && *parsed > 0.0)
                    lines.append(QStringLiteral("T = %1 s").arg(formatQuantityValue(1.0 / *parsed)));
            } else if (const auto period = instanceParameter(instance, {"period"})) {
                lines.append(QStringLiteral("T = %1").arg(withUnit(*period, QStringLiteral("s"))));
            }
            if (!lines.isEmpty())
                return {lines.join('\n'), true};
        }
        if (name.find("pulse") != std::string::npos) {
            QStringList lines;
            if (const auto voltage = instanceParameter(instance, {"pulsed_voltage", "amplitude", "voltage"}))
                lines.append(QStringLiteral("V = %1").arg(withUnit(*voltage, QStringLiteral("V"))));
            if (const auto period = instanceParameter(instance, {"period"}))
                lines.append(QStringLiteral("T = %1").arg(withUnit(*period, QStringLiteral("s"))));
            if (!lines.isEmpty())
                return {lines.join('\n'), true};
        }
        if (const auto voltage = instanceParameter(instance, {"voltage"}))
            return {withUnit(*voltage, QStringLiteral("V")), true};
        if (const auto current = instanceParameter(instance, {"current"}))
            return {withUnit(*current, QStringLiteral("A")), true};
        return {};
    }

    void refreshSchematic() {
        if (!schematicCanvas_)
            return;
        updateFileTabDirtyIndicators();
        auto *scene = schematicCanvas_->scene();
        scene->clear();
        if (!project_ || project_->project().schematicCells.empty()) {
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clear();
            if (schematicNetlistPanel_)
                schematicNetlistPanel_->clear();
            schematicSymbolsLoaded_ = false;
            schematicSymbolsPdk_.clear();
            return;
        }
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            cellIt = project_->project().schematicCells.begin();
        if (cellIt == project_->project().schematicCells.end())
            return;
        const auto &cell = cellIt->second;
        if (cell.isTextSource()) {
            if (schematicViewStack_ && textSchematicEditorPage_)
                schematicViewStack_->setCurrentWidget(textSchematicEditorPage_);
            if (textSchematicEditor_) {
                textSchematicEditor_->setLanguage(textSourceLanguage(cell));
                const auto text = QString::fromStdString(cell.sourceText());
                if (textSchematicEditor_->text() != text)
                    textSchematicEditor_->setText(text);
            }
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clear();
            if (schematicNetlistPanel_)
                schematicNetlistPanel_->clear();
            schematicSymbolsLoaded_ = false;
            schematicSymbolsPdk_.clear();
            return;
        }
        if (schematicViewStack_ && schematicCanvasPage_)
            schematicViewStack_->setCurrentWidget(schematicCanvasPage_);
        const auto pdk = cell.pdk.empty() ? std::string(ice::genericPdkId) : cell.pdk;
        if (schematicSymbolsPanel_) {
            if (!schematicSymbolsLoaded_ || schematicSymbolsPdk_ != pdk) {
                schematicSymbolsLoaded_ = true;
                schematicSymbolsPdk_ = pdk;
                schematicBaseSymbols_ = symbolLibraryForPdk(pdk);
            }
            auto symbols = schematicBaseSymbols_;
            for (const auto &[id, other] : project_->project().schematicCells) {
                if (id == cell.id || other.isTextSource())
                    continue;
                symbols.push_back(generatedSchematicSymbol(other));
            }
            schematicSymbolsPanel_->setSymbols(std::move(symbols));
        }
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->setCell(project_->project(), &cell);
        std::map<ice::ObjectId, QGraphicsTextItem *> netLabelItems;
        for (const auto &[netId, net] : cell.nets) {
            if (net.name.empty())
                continue;
            auto *label = scene->addText(QString::fromStdString(net.name));
            label->setDefaultTextColor(ice::uiColor(ice::UiColor::Accent5));
            label->setData(0, QStringLiteral("net-label"));
            label->setData(1, QVariant::fromValue<qulonglong>(netId));
            label->setPos(net.x, -net.y);
            netLabelItems.emplace(netId, label);
        }
        std::map<ice::ObjectId, ice::ObjectId> pinNetIds;
        for (const auto &[netId, net] : cell.nets)
            for (const auto pinId : net.pins)
                pinNetIds[pinId] = netId;
        for (const auto &[instanceId, instance] : cell.instances) {
            auto symbolIt = cell.symbols.find(instance.symbol);
            if (symbolIt == cell.symbols.end())
                continue;
            auto *group = new QGraphicsItemGroup;
            scene->addItem(group);
            group->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
            group->setData(0, QStringLiteral("symbol"));
            group->setData(1, QVariant::fromValue<qulonglong>(instanceId));
            group->setPos(instance.x, -instance.y);
            switch (instance.orientation) {
            case ice::Orientation::r90:
                group->setRotation(90);
                break;
            case ice::Orientation::r180:
                group->setRotation(180);
                break;
            case ice::Orientation::r270:
                group->setRotation(270);
                break;
            case ice::Orientation::mx:
                group->setTransform(QTransform(1, 0, 0, -1, 0, 0));
                break;
            case ice::Orientation::my:
                group->setTransform(QTransform(-1, 0, 0, 1, 0, 0));
                break;
            case ice::Orientation::m45:
                group->setTransform(QTransform(0, 1, 1, 0, 0, 0));
                break;
            case ice::Orientation::m135:
                group->setTransform(QTransform(0, -1, -1, 0, 0, 0));
                break;
            default:
                group->setRotation(0);
                break;
            }
            group->setToolTip(QString::fromStdString(instance.name));
            const auto artworkText =
                symbolIt->second.artwork.empty()
                    ? std::string("<svg viewBox='0 0 120 60'><rect x='2' y='2' width='116' height='56' fill='none' "
                                  "stroke='black' stroke-width='4'/></svg>")
                    : symbolIt->second.artwork;
            auto *renderer = new QSvgRenderer(QByteArray::fromStdString(artworkText), scene);
            auto *artwork = new QGraphicsSvgItem;
            artwork->setSharedRenderer(renderer);
            artwork->setParentItem(group);
            // Center artwork of any size on the symbol origin; pins are
            // defined relative to the artwork center (see symbols/*.toml).
            const auto artworkSize = renderer->defaultSize();
            const QRectF artworkRect(-artworkSize.width() / 2.0, -artworkSize.height() / 2.0, artworkSize.width(),
                                     artworkSize.height());
            const auto &visibleArtwork = schematicArtworkVisibleBounds(*renderer, artworkText, artworkSize);
            const auto visibleInGroup = visibleArtwork.isValid() && !visibleArtwork.isEmpty()
                                            ? visibleArtwork.translated(artworkRect.topLeft())
                                            : artworkRect;
            artwork->setPos(artworkRect.topLeft());
            artwork->setFlag(QGraphicsItem::ItemIsSelectable, false);
            artwork->setData(0, QStringLiteral("symbol-artwork"));
            auto *tint = new QGraphicsColorizeEffect(artwork);
            tint->setColor(ice::uiColor(ice::UiColor::Axis1));
            tint->setStrength(1.0);
            tint->setEnabled(true);
            artwork->setGraphicsEffect(tint);
            artwork->setData(3, QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(tint)));
            constexpr qreal symbolAnnotationGap = 2.0;
            const bool sourceSymbol = isSchematicSourceSymbol(symbolIt->second, instance);
            const auto valueLabel = schematicValueLabel(symbolIt->second, instance);
            auto *label = new QGraphicsTextItem(QString::fromStdString(instance.name), group);
            label->setDefaultTextColor(ice::uiColor(ice::UiColor::Axis1));
            if (sourceSymbol) {
                const auto bounds = label->boundingRect();
                label->setPos(visibleInGroup.left() - bounds.width() - symbolAnnotationGap,
                              visibleInGroup.center().y() - bounds.height() / 2.0);
            } else if (symbolIt->second.label) {
                const auto bounds = label->boundingRect();
                label->setPos(symbolIt->second.label->x - bounds.width() / 2.0,
                              -symbolIt->second.label->y - bounds.height() / 2.0);
            } else {
                const auto bounds = label->boundingRect();
                label->setPos(visibleInGroup.center().x() - bounds.width() / 2.0,
                              visibleInGroup.bottom() + symbolAnnotationGap);
            }
            label->setData(0, QStringLiteral("symbol-label"));
            label->setFlag(QGraphicsItem::ItemIsSelectable, false);
            if (!valueLabel.text.isEmpty()) {
                auto *valueItem = new QGraphicsTextItem(valueLabel.text, group);
                valueItem->setDefaultTextColor(ice::uiColor(ice::UiColor::Text1));
                valueItem->setData(0, QStringLiteral("symbol-value"));
                valueItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
                valueItem->setAcceptedMouseButtons(Qt::NoButton);
                const auto bounds = valueItem->boundingRect();
                if (valueLabel.onRight) {
                    valueItem->setPos(visibleInGroup.right() + symbolAnnotationGap,
                                      visibleInGroup.center().y() - bounds.height() / 2.0);
                } else {
                    qreal valueGap = symbolAnnotationGap;
                    if (symbolIt->second.label) {
                        const auto referenceBounds = label->boundingRect();
                        const auto referenceBottom = -symbolIt->second.label->y + referenceBounds.height() / 2.0;
                        valueGap = std::max<qreal>(0.0, visibleInGroup.top() - referenceBottom);
                    }
                    valueItem->setPos(visibleInGroup.center().x() - bounds.width() / 2.0,
                                      visibleInGroup.bottom() + valueGap);
                }
            }
            for (const auto &pin : symbolIt->second.pins) {
                auto *pinItem = new QGraphicsEllipseItem(QRectF(-5, -5, 10, 10), group);
                pinItem->setPen(QPen(ice::uiColor(ice::UiColor::Axis1)));
                pinItem->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent1)));
                pinItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
                pinItem->setData(0, QStringLiteral("pin"));
                pinItem->setData(1, QVariant::fromValue<qulonglong>(pin.id));
                pinItem->setData(2, static_cast<int>(rotateDirection(pin.direction, instance.orientation)));
                if (const auto net = pinNetIds.find(pin.id); net != pinNetIds.end())
                    pinItem->setData(3, QVariant::fromValue<qulonglong>(net->second));
                pinItem->setPos(pin.x, -pin.y);
                pinItem->setToolTip(QString::fromStdString(pin.name));
                pinItem->setVisible(false);
            }
            // Selection feedback is drawn as a child of the symbol group so it
            // follows the symbol and its transform.
            if (const auto bounds = symbolHighlightRect(group); !bounds.isNull()) {
                auto *highlight = new QGraphicsRectItem(bounds, group);
                highlight->setPen(QPen(ice::uiColor(ice::UiColor::Accent4), 2));
                QColor fill(ice::uiColor(ice::UiColor::Accent4));
                fill.setAlpha(45);
                highlight->setBrush(fill);
                highlight->setZValue(-1);
                highlight->setFlag(QGraphicsItem::ItemIsSelectable, false);
                highlight->setData(0, QStringLiteral("symbol-highlight"));
                highlight->setVisible(false);
            }
        }
        for (const auto &[commentId, comment] : cell.comments) {
            auto *group = new QGraphicsItemGroup;
            scene->addItem(group);
            group->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
            group->setData(0, QStringLiteral("comment"));
            group->setData(1, QVariant::fromValue<qulonglong>(commentId));
            group->setPos(comment.x, -comment.y);
            group->setToolTip(QString::fromStdString(comment.text));

            auto *textItem = new QGraphicsTextItem(QString::fromStdString(comment.text), group);
            textItem->setDefaultTextColor(ice::uiColor(ice::UiColor::Text1));
            QFont font = textItem->font();
            font.setPointSize(11);
            textItem->setFont(font);
            textItem->setData(0, QStringLiteral("comment-text"));
            textItem->setFlag(QGraphicsItem::ItemIsSelectable, false);

            const auto textBounds = textItem->boundingRect();
            const auto highlightBounds = textBounds.adjusted(-4, -4, 4, 4);
            auto *highlight = new QGraphicsRectItem(highlightBounds, group);
            highlight->setPen(QPen(ice::uiColor(ice::UiColor::Accent4), 2));
            QColor fill(ice::uiColor(ice::UiColor::Accent4));
            fill.setAlpha(45);
            highlight->setBrush(fill);
            highlight->setZValue(-1);
            highlight->setFlag(QGraphicsItem::ItemIsSelectable, false);
            highlight->setData(0, QStringLiteral("symbol-highlight"));
            highlight->setVisible(false);
        }
        std::map<ice::ObjectId, SchematicPinView> pinViews;
        for (auto *item : scene->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            auto *owner = item->parentItem();
            pinViews[item->data(1).toULongLong()] = {item->scenePos(),
                                                     static_cast<ice::PinDirection>(item->data(2).toInt()),
                                                     owner ? owner->data(1).toULongLong() : ice::ObjectId{0}};
        }
        reconcileSchematicRoutes(cellIt->first, cellIt->second, pinViews, scene);
        if (const auto routeIt = schematicRouteCache_.find(cellIt->first); routeIt != schematicRouteCache_.end()) {
            for (const auto &[netId, netRoute] : routeIt->second) {
                for (const auto &branch : netRoute.branches) {
                    if (branch.points.size() < 2)
                        continue;
                    QPainterPath path;
                    path.moveTo(branch.points.front());
                    for (std::size_t index = 1; index < branch.points.size(); ++index)
                        path.lineTo(branch.points[index]);
                    auto *wire = scene->addPath(path, QPen(ice::uiColor(ice::UiColor::Accent4), 2));
                    wire->setZValue(-1);
                    wire->setData(0, QStringLiteral("wire"));
                    wire->setData(1, QVariant::fromValue<qulonglong>(netId));
                    wire->setData(4, QVariant::fromValue<qulonglong>(branch.fromPin));
                    wire->setData(5, QVariant::fromValue<qulonglong>(branch.toPin));
                }
            }
        }
        for (const auto &[netId, net] : cell.nets) {
            const auto label = netLabelItems.find(netId);
            if (label == netLabelItems.end())
                continue;
            std::optional<QPointF> position;
            if (net.labelAnchor)
                position = schematicNetLabelPosition(cellIt->first, net, *label->second);
            if (!position)
                position =
                    schematicNetNearestLabelPosition(cellIt->first, netId, *label->second, QPointF(net.x, -net.y));
            if (position)
                label->second->setPos(*position);
        }
        // Selection is view state over the domain model, so re-apply it after
        // the scene has been rebuilt from the database.
        schematicCanvas_->restoreSelection();
        updateSelectionStatus();
        schematicCanvas_->refreshPinVisibility();
    }

    static QRectF symbolObstacleRect(QGraphicsItem *symbol) {
        for (auto *child : symbol->childItems())
            if (child->data(0).toString() == QStringLiteral("symbol-artwork"))
                return child->sceneBoundingRect();
        return symbol->sceneBoundingRect();
    }

    static bool schematicSegmentIntersectsRect(QPointF first, QPointF second, QRectF rect, qreal margin) {
        rect = rect.normalized().adjusted(-margin, -margin, margin, margin);
        if (rect.width() <= 0.0 || rect.height() <= 0.0)
            return false;
        constexpr qreal epsilon = 0.001;
        if (std::abs(first.y() - second.y()) <= epsilon) {
            if (first.y() <= rect.top() + epsilon || first.y() >= rect.bottom() - epsilon)
                return false;
            return std::min(std::max(first.x(), second.x()), rect.right()) -
                       std::max(std::min(first.x(), second.x()), rect.left()) >
                   epsilon;
        }
        if (std::abs(first.x() - second.x()) <= epsilon) {
            if (first.x() <= rect.left() + epsilon || first.x() >= rect.right() - epsilon)
                return false;
            return std::min(std::max(first.y(), second.y()), rect.bottom()) -
                       std::max(std::min(first.y(), second.y()), rect.top()) >
                   epsilon;
        }
        return false;
    }

    bool schematicBranchValid(const SchematicBranchRoute &branch,
                              const std::map<ice::ObjectId, SchematicPinView> &pinViews, QGraphicsScene *scene) const {
        const auto from = pinViews.find(branch.fromPin);
        const auto to = pinViews.find(branch.toPin);
        if (from == pinViews.end() || to == pinViews.end() || branch.fromPin == branch.toPin)
            return false;
        if (branch.fromDirection != from->second.direction || branch.toDirection != to->second.direction)
            return false;
        if (branch.points.size() < 2)
            return false;
        constexpr qreal tolerance = 0.5;
        if (QLineF(branch.points.front(), from->second.point).length() > tolerance ||
            QLineF(branch.points.back(), to->second.point).length() > tolerance)
            return false;
        constexpr qreal clearance = 2.0;
        constexpr qreal minimumExtension = 30.0;
        for (auto *item : scene->items()) {
            if (item->data(0).toString() != QStringLiteral("symbol"))
                continue;
            const auto instance = item->data(1).toULongLong();
            if (instance == from->second.instance || instance == to->second.instance)
                continue;
            const auto rect = symbolObstacleRect(item);
            if (!rect.isValid() || rect.isEmpty())
                continue;
            for (std::size_t index = 0; index + 1 < branch.points.size(); ++index)
                if (schematicSegmentIntersectsRect(branch.points[index], branch.points[index + 1], rect, clearance))
                    return false;
        }
        for (const auto &[pinId, view] : pinViews) {
            if (pinId == branch.fromPin || pinId == branch.toPin)
                continue;
            const auto exit = view.point + directionVector(view.direction) * minimumExtension;
            const QRectF extension = QRectF(view.point, exit).normalized();
            for (std::size_t index = 0; index + 1 < branch.points.size(); ++index)
                if (schematicSegmentIntersectsRect(branch.points[index], branch.points[index + 1], extension,
                                                   clearance + 0.5))
                    return false;
        }
        return true;
    }

    std::optional<QPointF> schematicNetLabelPosition(ice::ObjectId cellId, const ice::Net &net,
                                                     const QGraphicsTextItem &label) const {
        if (!net.labelAnchor)
            return std::nullopt;
        const auto cellRoutes = schematicRouteCache_.find(cellId);
        if (cellRoutes == schematicRouteCache_.end())
            return std::nullopt;
        const auto netRoute = cellRoutes->second.find(net.id);
        if (netRoute == cellRoutes->second.end())
            return std::nullopt;
        const SchematicBranchRoute *branch = nullptr;
        bool reversed = false;
        for (const auto &candidate : netRoute->second.branches) {
            if (candidate.fromPin == net.labelAnchor->fromPin && candidate.toPin == net.labelAnchor->toPin) {
                branch = &candidate;
                break;
            }
            if (candidate.fromPin == net.labelAnchor->toPin && candidate.toPin == net.labelAnchor->fromPin) {
                branch = &candidate;
                reversed = true;
                break;
            }
        }
        if (!branch || branch->points.size() < 2)
            return std::nullopt;
        std::vector<qreal> cumulative(branch->points.size(), 0.0);
        for (std::size_t index = 1; index < branch->points.size(); ++index)
            cumulative[index] =
                cumulative[index - 1] + QLineF(branch->points[index - 1], branch->points[index]).length();
        const auto totalLength = cumulative.back();
        if (totalLength <= 0.0)
            return std::nullopt;
        const auto fraction =
            std::clamp(reversed ? 1.0 - net.labelAnchor->fraction : net.labelAnchor->fraction, 0.0, 1.0);
        const auto side = (net.labelAnchor->sideB != reversed) ? 1.0 : -1.0;
        const auto target = fraction * totalLength;
        const auto bounds = label.boundingRect();
        const auto width = bounds.width() > 0.0 ? bounds.width() : 80.0;
        const auto height = bounds.height() > 0.0 ? bounds.height() : 18.0;
        for (std::size_t index = 1; index < branch->points.size(); ++index) {
            const auto start = branch->points[index - 1];
            const auto end = branch->points[index];
            const auto delta = end - start;
            const auto length = QLineF(start, end).length();
            if (length <= 0.0)
                continue;
            if (target > cumulative[index] + 0.001 && index + 1 < branch->points.size())
                continue;
            const auto local = std::clamp((target - cumulative[index - 1]) / length, qreal(0.0), qreal(1.0));
            const auto point = start + delta * local;
            const QPointF normal(-delta.y() / length, delta.x() / length);
            const qreal clearance = std::abs(normal.x()) * width / 2.0 + std::abs(normal.y()) * height / 2.0 + 2.0;
            return point + normal * side * clearance - bounds.center();
        }
        return std::nullopt;
    }

    std::optional<QPointF> schematicNetNearestLabelPosition(ice::ObjectId cellId, ice::ObjectId netId,
                                                            const QGraphicsTextItem &label, QPointF candidate) const {
        const auto cellRoutes = schematicRouteCache_.find(cellId);
        if (cellRoutes == schematicRouteCache_.end())
            return std::nullopt;
        const auto netRoute = cellRoutes->second.find(netId);
        if (netRoute == cellRoutes->second.end())
            return std::nullopt;
        const auto bounds = label.boundingRect();
        const auto width = bounds.width() > 0.0 ? bounds.width() : 80.0;
        const auto height = bounds.height() > 0.0 ? bounds.height() : 18.0;
        const QPointF labelCenter = bounds.center();
        const QPointF candidateCenter = candidate + labelCenter;
        constexpr qreal margin = 2.0;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        std::optional<QPointF> best;
        for (const auto &branch : netRoute->second.branches) {
            for (std::size_t index = 1; index < branch.points.size(); ++index) {
                const auto start = branch.points[index - 1];
                const auto end = branch.points[index];
                const auto delta = end - start;
                const auto lengthSquared = QPointF::dotProduct(delta, delta);
                if (lengthSquared <= 0.0)
                    continue;
                const auto length = std::sqrt(lengthSquared);
                const auto fraction = std::clamp(QPointF::dotProduct(candidateCenter - start, delta) / lengthSquared,
                                                 qreal(0.0), qreal(1.0));
                const auto point = start + delta * fraction;
                const QPointF normal(-delta.y() / length, delta.x() / length);
                const qreal side = QPointF::dotProduct(candidateCenter - point, normal) < 0.0 ? -1.0 : 1.0;
                const qreal clearance =
                    std::abs(normal.x()) * width / 2.0 + std::abs(normal.y()) * height / 2.0 + margin;
                const auto anchored = point + normal * side * clearance - labelCenter;
                const auto distance = QLineF(candidate, anchored).length();
                if (distance >= bestDistance)
                    continue;
                bestDistance = distance;
                best = anchored;
            }
        }
        return best;
    }

    void reconcileSchematicRoutes(ice::ObjectId cellId, const ice::SchematicCell &cell,
                                  const std::map<ice::ObjectId, SchematicPinView> &pinViews, QGraphicsScene *scene) {
        auto &cellCache = schematicRouteCache_[cellId];
        for (auto entry = cellCache.begin(); entry != cellCache.end();) {
            if (!cell.nets.contains(entry->first))
                entry = cellCache.erase(entry);
            else
                ++entry;
        }
        for (const auto &[netId, net] : cell.nets) {
            std::vector<ice::ObjectId> pins;
            std::set<ice::ObjectId> pinSet;
            for (const auto pin : net.pins) {
                if (!pinViews.contains(pin) || !pinSet.insert(pin).second)
                    continue;
                pins.push_back(pin);
            }
            if (pins.size() < 2) {
                cellCache.erase(netId);
                continue;
            }
            auto &route = cellCache[netId];
            if (net.labelAnchor && pinSet.contains(net.labelAnchor->fromPin) &&
                pinSet.contains(net.labelAnchor->toPin) && net.labelAnchor->fromPin != net.labelAnchor->toPin) {
                const auto present =
                    std::any_of(route.branches.begin(), route.branches.end(), [&](const SchematicBranchRoute &branch) {
                        return (branch.fromPin == net.labelAnchor->fromPin && branch.toPin == net.labelAnchor->toPin) ||
                               (branch.fromPin == net.labelAnchor->toPin && branch.toPin == net.labelAnchor->fromPin);
                    });
                if (!present) {
                    SchematicBranchRoute branch;
                    branch.fromPin = net.labelAnchor->fromPin;
                    branch.toPin = net.labelAnchor->toPin;
                    route.branches.insert(route.branches.begin(), branch);
                }
            }

            std::map<ice::ObjectId, ice::ObjectId> parent;
            for (const auto pin : pins)
                parent.emplace(pin, pin);
            std::function<ice::ObjectId(ice::ObjectId)> findRoot = [&](ice::ObjectId pin) {
                auto root = pin;
                while (parent.at(root) != root)
                    root = parent.at(root);
                while (parent.at(pin) != pin) {
                    const auto next = parent.at(pin);
                    parent[pin] = root;
                    pin = next;
                }
                return root;
            };
            const auto unite = [&](ice::ObjectId left, ice::ObjectId right) {
                const auto leftRoot = findRoot(left);
                const auto rightRoot = findRoot(right);
                if (leftRoot != rightRoot)
                    parent[rightRoot] = leftRoot;
            };

            std::vector<SchematicBranchRoute> branches;
            std::set<std::pair<ice::ObjectId, ice::ObjectId>> seen;
            for (auto &branch : route.branches) {
                if (!pinSet.contains(branch.fromPin) || !pinSet.contains(branch.toPin) ||
                    branch.fromPin == branch.toPin)
                    continue;
                const auto key = branch.fromPin < branch.toPin ? std::make_pair(branch.fromPin, branch.toPin)
                                                               : std::make_pair(branch.toPin, branch.fromPin);
                if (!seen.insert(key).second)
                    continue;
                if (findRoot(branch.fromPin) == findRoot(branch.toPin))
                    continue;
                unite(branch.fromPin, branch.toPin);
                branches.push_back(std::move(branch));
            }

            // Keep existing branches and add the shortest links needed to
            // connect any pins that are not yet covered by the cached tree.
            while (true) {
                std::map<ice::ObjectId, std::vector<ice::ObjectId>> components;
                for (const auto pin : pins)
                    components[findRoot(pin)].push_back(pin);
                if (components.size() <= 1)
                    break;
                const std::vector<std::pair<ice::ObjectId, std::vector<ice::ObjectId>>> componentList(
                    components.begin(), components.end());
                double bestDistance = std::numeric_limits<double>::max();
                ice::ObjectId bestFrom = 0;
                ice::ObjectId bestTo = 0;
                for (std::size_t left = 0; left < componentList.size(); ++left) {
                    for (std::size_t right = left + 1; right < componentList.size(); ++right) {
                        for (const auto leftPin : componentList[left].second) {
                            for (const auto rightPin : componentList[right].second) {
                                const auto distance =
                                    QLineF(pinViews.at(leftPin).point, pinViews.at(rightPin).point).length();
                                const auto better =
                                    distance < bestDistance - 0.001 ||
                                    (std::abs(distance - bestDistance) <= 0.001 &&
                                     std::make_pair(leftPin, rightPin) < std::make_pair(bestFrom, bestTo));
                                if (!better)
                                    continue;
                                bestDistance = distance;
                                bestFrom = leftPin;
                                bestTo = rightPin;
                            }
                        }
                    }
                }
                if (bestFrom == 0)
                    break;
                SchematicBranchRoute branch;
                branch.fromPin = bestFrom;
                branch.toPin = bestTo;
                branches.push_back(branch);
                unite(bestFrom, bestTo);
            }

            for (auto &branch : branches) {
                const auto from = pinViews.find(branch.fromPin);
                const auto to = pinViews.find(branch.toPin);
                if (from == pinViews.end() || to == pinViews.end())
                    continue;
                if (schematicBranchValid(branch, pinViews, scene))
                    continue;
                branch.points.clear();
                const auto path = routeWire(from->second.point, from->second.direction, to->second.point,
                                            to->second.direction, scene, branch.fromPin, branch.toPin);
                for (int index = 0; index < path.elementCount(); ++index) {
                    const auto element = path.elementAt(index);
                    const QPointF point(element.x, element.y);
                    if (branch.points.empty() || QLineF(branch.points.back(), point).length() > 0.001)
                        branch.points.push_back(point);
                }
                branch.fromDirection = from->second.direction;
                branch.toDirection = to->second.direction;
            }
            route.branches = std::move(branches);
        }
    }

    QPainterPath routeWire(QPointF start, ice::PinDirection startDirection, QPointF target,
                           ice::PinDirection targetDirection, QGraphicsScene *scene, ice::ObjectId startPin,
                           ice::ObjectId targetPin) const {
        constexpr qreal minimumExtension = 30.0;
        const auto startVector = directionVector(startDirection);
        const auto targetVector = directionVector(targetDirection);
        const QPointF startExit = start + startVector * minimumExtension;
        const QPointF targetExit = target + targetVector * minimumExtension;

        ice::SchematicRouteRequest request;
        request.start = {start.x(), start.y()};
        request.startExit = {startExit.x(), startExit.y()};
        request.target = {target.x(), target.y()};
        request.targetExit = {targetExit.x(), targetExit.y()};
        request.clearance = 2.0;

        for (auto *item : scene->items()) {
            const auto kind = item->data(0).toString();
            if (kind == QStringLiteral("symbol")) {
                const auto rect = symbolObstacleRect(item);
                if (rect.isValid() && !rect.isEmpty())
                    request.obstacleRects.push_back({rect.left(), rect.top(), rect.right(), rect.bottom()});
            } else if (kind == QStringLiteral("pin")) {
                const auto pinId = item->data(1).toULongLong();
                if (pinId == startPin || pinId == targetPin)
                    continue;
                const auto position = item->scenePos();
                const auto direction = static_cast<ice::PinDirection>(item->data(2).toInt());
                const auto exit = position + directionVector(direction) * minimumExtension;
                request.obstacleSegments.push_back({{position.x(), position.y()}, {exit.x(), exit.y()}});
            }
        }

        // A minimum-extension point must remain reachable even when projected
        // artwork bounds extend slightly past the pin.
        const auto contains = [](const ice::RouteRect &rect, const ice::RoutePoint &point) {
            return point.x > std::min(rect.left, rect.right) && point.x < std::max(rect.left, rect.right) &&
                   point.y > std::min(rect.top, rect.bottom) && point.y < std::max(rect.top, rect.bottom);
        };
        std::erase_if(request.obstacleRects, [&](const ice::RouteRect &rect) {
            return contains(rect, request.startExit) || contains(rect, request.targetExit);
        });

        const auto route = ice::routeSchematicWire(request);
        QPainterPath path;
        if (route.points.empty()) {
            path.moveTo(start);
            path.lineTo(target);
            return path;
        }
        path.moveTo(route.points.front().x, route.points.front().y);
        for (std::size_t index = 1; index < route.points.size(); ++index)
            path.lineTo(route.points[index].x, route.points[index].y);
        return path;
    }

    QAction *addCommand(const QString &text, const QKeySequence &shortcut = {}) {
        auto *action = new QAction(text, this);
        commands_.append(action);
        shortcuts_.registerAction(action, ice::ShortcutRegistry::idForText(QStringLiteral("global"), text), text,
                                  ice::ShortcutScope::global, shortcut);
        return action;
    }

    void setAppearanceMode(ice::AppearanceMode mode) {
        appearanceMode_ = mode;
        ice::saveAppearanceMode(mode);
        ice::applyAppearance(mode);
        refreshAppearance();
        statusBar()->showMessage(mode == ice::AppearanceMode::System
                                     ? QStringLiteral("Following the system appearance")
                                     : (mode == ice::AppearanceMode::Dark
                                            ? QStringLiteral("Dark appearance applied")
                                            : QStringLiteral("Light appearance applied")));
    }

    void refreshAppearance() {
        const auto canvas = ice::uiColor(ice::UiColor::Background2);
        if (schematicCanvas_)
            schematicCanvas_->setBackgroundBrush(canvas);
        if (layoutCanvas_)
            layoutCanvas_->setBackgroundBrush(canvas);
        // Scene items cache their colors, so rebuild both editors after an
        // appearance change; the canvas transform and selection are preserved.
        refreshSchematic();
        refreshLayout();
        if (layersPanel_ && project_)
            layersPanel_->setProject(project_->project(), activeLayoutId_);
        if (schematicPropertiesPanel_)
            schematicPropertiesPanel_->refreshAppearance();
        if (startupView_)
            startupView_->refreshAppearance();
        if (reportHighlighter_)
            reportHighlighter_->refreshColors();
        if (proceduralEditor_)
            proceduralEditor_->refreshSyntaxColors();
        if (textSchematicEditor_)
            textSchematicEditor_->refreshSyntaxColors();
        if (simulationTestbenchEditor_)
            simulationTestbenchEditor_->refreshSyntaxColors();
        if (graphTabs_) {
            for (int index = 0; index < graphTabs_->count(); ++index)
                if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                    plot->update();
        }
        for (auto *action : findChildren<QAction *>()) {
            const auto name = action->property("lucideIcon").toString();
            if (!name.isEmpty())
                action->setIcon(ice::lucideIcon(name));
        }
        for (auto *button : findChildren<QAbstractButton *>()) {
            const auto name = button->property("lucideIcon").toString();
            if (!name.isEmpty())
                button->setIcon(ice::lucideIcon(name, button->iconSize().width()));
        }
        update();
    }

    bool invokeFocusedTextAction(const char *method) {
        auto *focus = QApplication::focusWidget();
        if (!focus ||
            !(focus->inherits("QLineEdit") || focus->inherits("QPlainTextEdit") || focus->inherits("QTextEdit")))
            return false;
        return QMetaObject::invokeMethod(focus, method);
    }

    void deleteCurrentSelection() {
        if (auto *focus = QApplication::focusWidget();
            focus &&
            (focus->inherits("QLineEdit") || focus->inherits("QPlainTextEdit") || focus->inherits("QTextEdit"))) {
            QKeyEvent event(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(focus, &event);
            return;
        }
        if (documents_ && layoutPage_ && documents_->currentWidget() == layoutPage_) {
            if (layoutCanvas_)
                layoutCanvas_->deleteSelectedShapes();
            return;
        }
        if (schematicCanvas_)
            schematicCanvas_->deleteSelectedSymbols();
    }

    void createMenus() {
        auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        auto *newAction = addCommand(QStringLiteral("New Project"), QKeySequence::New);
        auto *openAction = addCommand(QStringLiteral("Open Project"), QKeySequence::Open);
        auto *reloadProjectAction = addCommand(QStringLiteral("Reload Project from Disk"));
        auto *saveAction = addCommand(QStringLiteral("Save Project"), QKeySequence::Save);
        fileMenu->addAction(newAction);
        fileMenu->addAction(openAction);
        fileMenu->addAction(reloadProjectAction);
        fileMenu->addAction(saveAction);
        fileMenu->addSeparator();
        auto *openLayoutAction = addCommand(QStringLiteral("Open GDS/OASIS Layout"));
        auto *saveLayoutAction = addCommand(QStringLiteral("Save GDS/OASIS Layout"));
        fileMenu->addAction(openLayoutAction);
        fileMenu->addAction(saveLayoutAction);
        fileMenu->addSeparator();
        auto *closeAction = addCommand(QStringLiteral("Close Project"), QKeySequence::Close);
        fileMenu->addAction(closeAction);
        fileMenu->addSeparator();
        auto *exitAction = addCommand(QStringLiteral("Exit"), QKeySequence::Quit);
        fileMenu->addAction(exitAction);
        connect(exitAction, &QAction::triggered, this, [this] { close(); });

        auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        auto *undoAction = addCommand(QStringLiteral("Undo"), QKeySequence(QStringLiteral("Ctrl+Z")));
        auto *redoAction = addCommand(QStringLiteral("Redo"), QKeySequence(QStringLiteral("Ctrl+Y")));
        editMenu->addAction(undoAction);
        editMenu->addAction(redoAction);
        connect(undoAction, &QAction::triggered, this, [this] {
            if (!invokeFocusedTextAction("undo"))
                undoCurrentSection();
        });
        connect(redoAction, &QAction::triggered, this, [this] {
            if (!invokeFocusedTextAction("redo"))
                redoCurrentSection();
        });
        editMenu->addSeparator();
        auto *cutAction = addCommand(QStringLiteral("Cut"), QKeySequence::Cut);
        auto *copyAction = addCommand(QStringLiteral("Copy"), QKeySequence::Copy);
        auto *pasteAction = addCommand(QStringLiteral("Paste"), QKeySequence::Paste);
        editMenu->addAction(cutAction);
        editMenu->addAction(copyAction);
        editMenu->addAction(pasteAction);
        connect(cutAction, &QAction::triggered, this, [this] { invokeFocusedTextAction("cut"); });
        connect(copyAction, &QAction::triggered, this, [this] { invokeFocusedTextAction("copy"); });
        connect(pasteAction, &QAction::triggered, this, [this] { invokeFocusedTextAction("paste"); });
        // A shortcut is deliberately omitted here: the properties editor is a
        // modal dialog, so a window level shortcut would steal Return from the
        // line edits inside it.
        auto *propertiesAction = addCommand(QStringLiteral("Device Properties"));
        editMenu->addAction(propertiesAction);
        auto *deleteAction = addCommand(QStringLiteral("Delete"));
        editMenu->addAction(deleteAction);
        connect(deleteAction, &QAction::triggered, this, [this] { deleteCurrentSelection(); });
        connect(propertiesAction, &QAction::triggered, this, [this] { editSelectedSymbol(); });

        auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        viewMenu->addAction(addCommand(QStringLiteral("Project / Hierarchy")));
        viewMenu->addAction(addCommand(QStringLiteral("Properties / Diagnostics")));
        auto *agentAction = addCommand(QStringLiteral("Activity / Agent"));
        viewMenu->addAction(agentAction);
        connect(agentAction, &QAction::triggered, this, [this] {
            if (activityDock_)
                activityDock_->show();
            if (activityPanel_)
                activityPanel_->showAgentTab();
        });
        viewMenu->addSeparator();
        auto *paletteAction = addCommand(QStringLiteral("Command Palette"), QKeySequence(QStringLiteral("Ctrl+K")));
        viewMenu->addAction(paletteAction);
        connect(paletteAction, &QAction::triggered, this, &DocumentWindow::showCommandPalette);

        auto *projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
        auto *createSchematicAction = addCommand(QStringLiteral("Create Schematic"));
        projectMenu->addAction(createSchematicAction);
        auto *createLayoutAction = addCommand(QStringLiteral("Create Layout Cell"));
        projectMenu->addAction(createLayoutAction);
        auto *createLayerAction = addCommand(QStringLiteral("Create Layer"));
        projectMenu->addAction(createLayerAction);
        auto *placeSymbolAction = addCommand(QStringLiteral("Place Device"), QKeySequence(QStringLiteral("P")));
        projectMenu->addAction(placeSymbolAction);
        projectMenu->addSeparator();
        auto *validateAction = addCommand(QStringLiteral("Validate Project"));
        projectMenu->addAction(validateAction);
        auto *managePackagesAction = addCommand(QStringLiteral("Manage Packages"));
        setLucideIcon(managePackagesAction, QStringLiteral("package"));
        projectMenu->addAction(managePackagesAction);

        auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
        auto *simulationAction = addCommand(QStringLiteral("Run Simulation"), QKeySequence(QStringLiteral("F6")));
        toolsMenu->addAction(simulationAction);
        auto *drcAction = addCommand(QStringLiteral("Run DRC"), QKeySequence(QStringLiteral("F7")));
        toolsMenu->addAction(drcAction);
        auto *lvsAction = addCommand(QStringLiteral("Run LVS"), QKeySequence(QStringLiteral("F8")));
        toolsMenu->addAction(lvsAction);
        auto *pexAction = addCommand(QStringLiteral("Run PEX"), QKeySequence(QStringLiteral("F9")));
        toolsMenu->addAction(pexAction);
        auto *netStackAction = addCommand(QStringLiteral("Net Trace Layer Stack"));
        toolsMenu->addAction(netStackAction);

        auto *settingsMenu = menuBar()->addMenu(QStringLiteral("&Settings"));
        auto *shortcutsAction =
            addCommand(QStringLiteral("Keyboard Shortcuts…"), QKeySequence(QStringLiteral("Ctrl+Shift+K")));
        settingsMenu->addAction(shortcutsAction);
        settingsMenu->addSeparator();
        auto *appearanceMenu = settingsMenu->addMenu(QStringLiteral("Appearance"));
        auto *systemAppearanceAction = addCommand(QStringLiteral("System Theme"));
        auto *lightAppearanceAction = addCommand(QStringLiteral("Light Theme"));
        auto *darkAppearanceAction = addCommand(QStringLiteral("Dark Theme"));
        auto *appearanceGroup = new QActionGroup(this);
        appearanceGroup->setExclusive(true);
        for (auto *action : {systemAppearanceAction, lightAppearanceAction, darkAppearanceAction}) {
            action->setCheckable(true);
            appearanceGroup->addAction(action);
            appearanceMenu->addAction(action);
        }
        switch (appearanceMode_) {
        case ice::AppearanceMode::Light:
            lightAppearanceAction->setChecked(true);
            break;
        case ice::AppearanceMode::Dark:
            darkAppearanceAction->setChecked(true);
            break;
        case ice::AppearanceMode::System:
            systemAppearanceAction->setChecked(true);
            break;
        }

        auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        helpMenu->addAction(shortcutsAction);
        auto *aboutAction = addCommand(QStringLiteral("About ICE"));
        helpMenu->addAction(aboutAction);
        connect(shortcutsAction, &QAction::triggered, this, [this] { ice::showShortcutSettings(this, shortcuts_); });
        connect(aboutAction, &QAction::triggered, this, [this] {
            const auto info = ice::buildInfo();
            QMessageBox::about(this, QStringLiteral("About ICE"),
                               QStringLiteral("ICE %1\nCommit: %2\nBuild: %3 (%4)")
                                   .arg(QString::fromStdString(info.version), QString::fromStdString(info.commit),
                                        QString::fromStdString(info.buildTime),
                                        QString::fromStdString(info.buildType)));
        });

        connect(newAction, &QAction::triggered, this, &DocumentWindow::newProject);
        connect(openAction, &QAction::triggered, this, &DocumentWindow::openProject);
        connect(reloadProjectAction, &QAction::triggered, this, [this] { reloadProjectFromDisk(true); });
        connect(saveAction, &QAction::triggered, this, &DocumentWindow::saveProject);
        connect(openLayoutAction, &QAction::triggered, this, &DocumentWindow::openLayoutInterchange);
        connect(saveLayoutAction, &QAction::triggered, this, &DocumentWindow::saveLayoutInterchange);
        connect(closeAction, &QAction::triggered, this, &DocumentWindow::closeProject);
        connect(createSchematicAction, &QAction::triggered, this, [this] { createSchematic(); });
        connect(createLayoutAction, &QAction::triggered, this, [this] { createLayoutCell(); });
        connect(createLayerAction, &QAction::triggered, this, &DocumentWindow::createLayer);
        connect(managePackagesAction, &QAction::triggered, this, &DocumentWindow::managePackages);
        connect(placeSymbolAction, &QAction::triggered, this, &DocumentWindow::placeSymbol);
        connect(validateAction, &QAction::triggered, this, [this] {
            if (!project_)
                return;
            const auto diagnostics = project_->project().validate();
            if (diagnostics.empty()) {
                statusBar()->showMessage(QStringLiteral("Project validation passed"), 5000);
                return;
            }
            QStringList messages;
            for (const auto &diagnostic : diagnostics) {
                if (messages.size() >= 10)
                    break;
                messages << QString::fromStdString(diagnostic.message);
            }
            QMessageBox::warning(this, QStringLiteral("Validate Project"),
                                 QStringLiteral("%1 diagnostic(s):\n\n%2")
                                     .arg(diagnostics.size())
                                     .arg(messages.join(QLatin1Char('\n'))));
        });
        connect(simulationAction, &QAction::triggered, this, &DocumentWindow::runSimulation);
        connect(drcAction, &QAction::triggered, this, &DocumentWindow::runDrc);
        connect(lvsAction, &QAction::triggered, this, &DocumentWindow::runLvs);
        connect(pexAction, &QAction::triggered, this, &DocumentWindow::runPex);
        connect(netStackAction, &QAction::triggered, this, &DocumentWindow::showNetTraceStack);
        connect(systemAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::System); });
        connect(lightAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::Light); });
        connect(darkAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::Dark); });
    }

    void showCommandPalette() {
        CommandPalette palette(commands_, this);
        palette.exec();
    }

    void createProjectDock() {
        auto *dock = new QDockWidget(QStringLiteral("Project / Hierarchy"), this);
        dock->setObjectName(QStringLiteral("projectHierarchyDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        projectPanel_ = new ProjectPanel;
        projectPanel_->setLayoutFlattenState(flattenedLayouts_);
        projectPanel_->setOnFlattenChanged([this] { refreshEmbeddedPanels(); });
        projectPanel_->setOnRunPex([this](ice::ObjectId id) {
            activeLayoutId_ = id;
            runPex();
        });
        dock->setWidget(projectPanel_);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
    }

    void createInspectorDock() {
        auto *dock = new QDockWidget(QStringLiteral("Properties / Diagnostics"), this);
        dock->setObjectName(QStringLiteral("inspectorDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        auto *tabs = new QTabWidget;
        tabs->addTab(new PropertiesPanel, QStringLiteral("Properties"));
        tabs->addTab(new DiagnosticsPanel, QStringLiteral("Diagnostics"));
        dock->setWidget(tabs);
        addDockWidget(Qt::RightDockWidgetArea, dock);
    }

    void createActivityDock() {
        activityDock_ = new QDockWidget(this);
        activityDock_->setObjectName(QStringLiteral("activityDock"));
        activityDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        auto *titleBar = new QWidget(activityDock_);
        titleBar->setFixedHeight(0);
        activityDock_->setTitleBarWidget(titleBar);
        activityPanel_ = new ActivityPanel;
        activityPanel_->setProjectChangedCallback([this] { scheduleProjectReloadFromDisk(); });
        activityDock_->setWidget(activityPanel_);
        addDockWidget(Qt::BottomDockWidgetArea, activityDock_);
    }

    void showStartup() {
        if (workspaceStack_ && startupView_)
            workspaceStack_->setCurrentWidget(startupView_);
        if (activityDock_)
            activityDock_->hide();
        if (startupView_)
            startupView_->setRecentProjects(loadRecentProjects());
    }

    void showWorkspace() {
        if (workspaceStack_ && documents_)
            workspaceStack_->setCurrentWidget(documents_);
        if (activityDock_) {
            activityDock_->show();
            if (!activityDockSizeInitialized_) {
                activityDockSizeInitialized_ = true;
                QTimer::singleShot(0, this, [this] {
                    if (!activityDock_ || !activityDock_->isVisible())
                        return;
                    resizeDocks({activityDock_}, {activityDock_->minimumSizeHint().height()}, Qt::Vertical);
                });
            }
        }
    }

    void activateSchematicPlaceSymbolTool(const SymbolDefinition &definition, bool clearPanelSelection = false) {
        if (schematicPlaceSymbolTool_)
            schematicPlaceSymbolTool_->setChecked(true);
        if (schematicLeftTabs_ && schematicSymbolsPanel_)
            schematicLeftTabs_->setCurrentWidget(schematicSymbolsPanel_);
        selectedSchematicSymbol_ = definition;
        lastSchematicPlacementDevice_ = definition;
        if (clearPanelSelection && schematicSymbolsPanel_)
            schematicSymbolsPanel_->selectSymbol(definition.name);
        if (schematicCanvas_) {
            schematicCanvas_->setPlaceSymbolPreview(definition.svg.toUtf8());
            schematicCanvas_->setCommentMode(false);
            schematicCanvas_->setPlaceSymbolMode(true);
        }
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(true);
        statusBar()->showMessage(QStringLiteral("Click in the schematic to place %1").arg(definition.name));
    }

    void activateSchematicPlaceSymbolTool() {
        const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
        if (symbol)
            activateSchematicPlaceSymbolTool(*symbol);
    }

    // Device-letter shortcuts pick a symbol by name out of the library that
    // refreshSchematic caches, so pressing a key never hits the filesystem.
    const SymbolDefinition *schematicDeviceDefinition(const QString &name) const {
        for (const auto &definition : schematicBaseSymbols_)
            if (definition.name == name)
                return &definition;
        return nullptr;
    }

    void activateSchematicDevice(const QString &name) {
        if (!project_ || project_->project().schematicCells.empty()) {
            statusBar()->showMessage(QStringLiteral("Create a schematic before placing %1").arg(name));
            return;
        }
        const auto *definition = schematicDeviceDefinition(name);
        if (!definition) {
            statusBar()->showMessage(
                QStringLiteral("%1 is not available in this schematic's symbol library").arg(name));
            return;
        }
        activateSchematicPlaceSymbolTool(*definition, true);
        if (schematicCanvas_)
            schematicCanvas_->setFocus();
    }

    // Shift + wheel while placing scrolls through the device library.
    void cycleSchematicPlacementDevice(int delta) {
        if (schematicBaseSymbols_.empty())
            return;
        const auto count = static_cast<int>(schematicBaseSymbols_.size());
        int index = -1;
        if (selectedSchematicSymbol_) {
            for (int candidate = 0; candidate < count; ++candidate) {
                if (schematicBaseSymbols_[candidate].name == selectedSchematicSymbol_->name) {
                    index = candidate;
                    break;
                }
            }
        }
        index = index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count;
        activateSchematicPlaceSymbolTool(schematicBaseSymbols_[index], true);
    }

    void selectSchematicTool() {
        if (schematicSelectTool_)
            schematicSelectTool_->setChecked(true);
        if (schematicCanvas_) {
            schematicCanvas_->setSignalPickMode(false);
            schematicCanvas_->setPlaceSymbolMode(false);
            schematicCanvas_->setCommentMode(false);
        }
        if (schematicSymbolsPanel_)
            schematicSymbolsPanel_->clearSymbolSelection();
        selectedSchematicSymbol_.reset();
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Select tool"));
    }

    // Default names offered by the create dialogs and empty workspace panels:
    // Schematic01, Schematic02, ... and Layout01, Layout02, ... skipping names
    // the project already uses.
    QString nextSchematicDefaultName() const {
        QStringList names;
        if (project_) {
            for (const auto &entry : project_->project().schematicCells)
                names.push_back(QString::fromStdString(entry.second.name));
        }
        return nextDefaultCellName(QStringLiteral("Schematic"), names);
    }

    QString nextLayoutDefaultName() const {
        QStringList names;
        if (project_) {
            for (const auto &entry : project_->project().layoutCells)
                names.push_back(QString::fromStdString(entry.second.name));
            for (const auto &entry : project_->project().proceduralLayouts)
                names.push_back(QString::fromStdString(entry.second.name));
        }
        return nextDefaultCellName(QStringLiteral("Layout"), names);
    }

    void updateEmptyWorkspaceState() {
        const bool hasOpenSchematics = project_ && !openSchematicTabs_.empty();
        const bool hasOpenLayouts = project_ && !openLayoutTabs_.empty();
        bool showingCanvasSchematic = false;
        if (project_ && hasOpenSchematics) {
            const auto active = project_->project().schematicCells.find(activeSchematicId_);
            if (active != project_->project().schematicCells.end()) {
                showingCanvasSchematic = !active->second.isTextSource();
            } else {
                for (const auto id : openSchematicTabs_) {
                    const auto cell = project_->project().schematicCells.find(id);
                    if (cell != project_->project().schematicCells.end() && !cell->second.isTextSource()) {
                        showingCanvasSchematic = true;
                        break;
                    }
                }
            }
        }
        bool showingCanvasLayout = false;
        if (project_ && hasOpenLayouts) {
            const bool proceduralActive = activeProceduralLayoutId_ != 0 &&
                                          project_->project().proceduralLayouts.contains(activeProceduralLayoutId_);
            if (!proceduralActive) {
                const auto active = project_->project().layoutCells.find(activeLayoutId_);
                if (active != project_->project().layoutCells.end()) {
                    showingCanvasLayout = true;
                } else {
                    for (const auto id : openLayoutTabs_) {
                        if (project_->project().layoutCells.contains(id)) {
                            showingCanvasLayout = true;
                            break;
                        }
                    }
                }
            }
        }
        if (schematicToolbarColumn_)
            schematicToolbarColumn_->setVisible(showingCanvasSchematic);
        if (layoutToolbarColumn_)
            layoutToolbarColumn_->setVisible(showingCanvasLayout);
        if (layoutOptionsColumn_)
            layoutOptionsColumn_->setVisible(showingCanvasLayout);

        if (schematicEditorStack_ && schematicEditorPage_ && schematicEmptyPanel_) {
            const bool showingEmpty = schematicEditorStack_->currentWidget() == schematicEmptyPanel_;
            if (hasOpenSchematics) {
                schematicEditorStack_->setCurrentWidget(schematicEditorPage_);
            } else {
                std::vector<std::pair<ice::ObjectId, QString>> candidates;
                if (project_) {
                    for (const auto &[id, cell] : project_->project().schematicCells)
                        candidates.emplace_back(id, fileTabLabel(QString::fromStdString(cell.name),
                                                                 schematicFileExtension(cell), schematicFileDirty(id),
                                                                 conflictedSchematicFiles_.contains(id)));
                }
                schematicEmptyPanel_->setOpenCandidates(candidates);
                schematicEmptyPanel_->refreshPdks(configuredPackageInstallDirectory());
                schematicEditorStack_->setCurrentWidget(schematicEmptyPanel_);
                if (!showingEmpty)
                    schematicEmptyPanel_->prepareForDisplay();
                schematicEmptyPanel_->setDefaultName(nextSchematicDefaultName());
            }
        }
        if (layoutEditorStack_ && layoutEditorPage_ && layoutEmptyPanel_) {
            const bool showingEmpty = layoutEditorStack_->currentWidget() == layoutEmptyPanel_;
            layoutEditorStack_->setCurrentWidget(hasOpenLayouts ? layoutEditorPage_ : layoutEmptyPanel_);
            if (!hasOpenLayouts) {
                std::vector<std::pair<ice::ObjectId, QString>> candidates;
                if (project_) {
                    for (const auto &[id, layout] : project_->project().proceduralLayouts)
                        candidates.emplace_back(id, fileTabLabel(QString::fromStdString(layout.name),
                                                                 QStringLiteral("py"), layoutFileDirty(id),
                                                                 conflictedProceduralFiles_.contains(id)));
                    for (const auto &[id, cell] : project_->project().layoutCells)
                        candidates.emplace_back(id, fileTabLabel(QString::fromStdString(cell.name),
                                                                 QStringLiteral("icelay"), layoutFileDirty(id),
                                                                 conflictedLayoutFiles_.contains(id)));
                }
                layoutEmptyPanel_->setOpenCandidates(candidates);
                layoutEmptyPanel_->refreshPdks(configuredPackageInstallDirectory());
                if (!showingEmpty)
                    layoutEmptyPanel_->prepareForDisplay();
                layoutEmptyPanel_->setDefaultName(nextLayoutDefaultName());
            }
        }
    }

    void createDocuments() {
        documents_ = new QTabWidget;
        auto *documents = documents_;
        documents->setTabsClosable(false);
        auto *schematicPage = new QWidget;
        schematicPage_ = schematicPage;
        auto *schematicLayout = new QVBoxLayout(schematicPage);
        schematicLayout->setContentsMargins(0, 0, 0, 0);
        schematicLayout->setSpacing(0);
        schematicCellTabs_ = new QTabWidget;
        schematicCellTabs_->setDocumentMode(true);
        schematicCellTabs_->setTabsClosable(true);
        auto *schematicContent = new QWidget;
        auto *schematicContentLayout = new QHBoxLayout(schematicContent);
        schematicHierarchy_ = new ProjectPanel(false, false, false, true);
        schematicHierarchy_->setOnAddClicked([this] { showAddSchematicMenu(); });
        schematicHierarchy_->setOnRemoveClicked([this](ice::ObjectId id) { removeSchematic(id); });
        schematicHierarchy_->setOnMakeLayoutFromSchematic([this](ice::ObjectId id) { makeLayoutFromSchematic(id); });
        schematicHierarchy_->setOnActivateSchematic([this](ice::ObjectId id) { selectSchematicTab(id); });
        schematicSymbolsPanel_ = new SymbolsPanel;
        schematicSymbolsPanel_->setOnSelectionChanged([this] {
            if (!schematicCanvas_)
                return;
            const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
            if (symbol)
                selectedSchematicSymbol_ = *symbol;
            schematicCanvas_->setPlaceSymbolPreview(symbol ? symbol->svg.toUtf8() : QByteArray());
        });
        schematicSymbolsPanel_->setOnSymbolSelected([this] { activateSchematicPlaceSymbolTool(); });
        schematicLeftTabs_ = new QTabWidget;
        schematicLeftTabs_->addTab(schematicHierarchy_, QStringLiteral("Hierarchy"));
        schematicLeftTabs_->addTab(schematicSymbolsPanel_, QStringLiteral("Devices"));
        schematicContentLayout->addWidget(schematicLeftTabs_, 0);
        auto *schematicToolbar = createEditorToolbar(false);
        schematicToolbar->setOrientation(Qt::Vertical);
        schematicToolbarColumn_ = new QWidget;
        auto *schematicToolbarLayout = new QVBoxLayout(schematicToolbarColumn_);
        schematicToolbarLayout->setContentsMargins(0, 0, 0, 0);
        schematicToolbarLayout->addStretch(1);
        schematicToolbarLayout->addWidget(schematicToolbar, 0, Qt::AlignHCenter);
        schematicToolbarLayout->addStretch(1);
        schematicContentLayout->addWidget(schematicToolbarColumn_, 0);
        schematicEditorPage_ = new QWidget;
        auto *schematicEditorLayout = new QVBoxLayout(schematicEditorPage_);
        schematicEditorLayout->setContentsMargins(0, 0, 0, 0);
        schematicEditorLayout->setSpacing(0);
        schematicEditorLayout->addWidget(schematicCellTabs_);
        schematicCanvasPage_ = makeEditor(QStringLiteral("Schematic editor"), false);
        textSchematicEditor_ = new TextDocumentEditor;
        textSchematicEditor_->setOnTextEdited([this](const QString &text) { onTextSchematicEdited(text); });
        textSchematicEditorPage_ = textSchematicEditor_;
        schematicViewStack_ = new QStackedWidget;
        schematicViewStack_->addWidget(schematicCanvasPage_);
        schematicViewStack_->addWidget(textSchematicEditorPage_);
        schematicEditorLayout->addWidget(schematicViewStack_, 1);
        schematicEmptyPanel_ = new EmptySchematicPanel;
        schematicEmptyPanel_->setOnCreate(
            [this](EmptySchematicPanel::Kind kind, const QString &name, const std::string &pdk) {
                switch (kind) {
                case EmptySchematicPanel::Kind::SchematicDrawing:
                    createSchematic(name, pdk);
                    break;
                case EmptySchematicPanel::Kind::TextSpice:
                    createTextSchematic(name, pdk);
                    break;
                case EmptySchematicPanel::Kind::SystemVerilog:
                    createSystemVerilogCell(name, pdk, false);
                    break;
                }
            });
        schematicEmptyPanel_->setOnOpen([this](ice::ObjectId id) { openSchematicFile(id); });
        schematicEditorStack_ = new QStackedWidget;
        schematicEditorStack_->addWidget(schematicEmptyPanel_);
        schematicEditorStack_->addWidget(schematicEditorPage_);
        schematicContentLayout->addWidget(schematicEditorStack_, 1);
        auto *schematicProperties = new QTabWidget;
        schematicPropertiesPanel_ = new PropertiesPanel(false);
        schematicPropertiesPanel_->setEditButtonVisible(true);
        schematicPropertiesPanel_->setOnParameterChanged(
            [this](ice::ObjectId instanceId, ice::ObjectId parameterId, const std::string &name,
                   const std::string &value) { updateInstanceParameter(instanceId, parameterId, name, value); });
        schematicPropertiesPanel_->setOnCellParameterChanged(
            [this](ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name, const std::string &value) {
                updateSchematicParameter(cellId, parameterId, name, value);
            });
        schematicPropertiesPanel_->setOnCellParameterAdded(
            [this](ice::ObjectId cellId, const std::string &name) { addSchematicParameter(cellId, name); });
        schematicPropertiesPanel_->setOnCellParametersRemoved(
            [this](ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds) {
                removeSchematicParameters(cellId, parameterIds);
            });
        schematicProperties->addTab(schematicPropertiesPanel_, QStringLiteral("Properties"));
        schematicNetlistPanel_ = new SchematicNetlistPanel;
        schematicProperties->addTab(schematicNetlistPanel_, QStringLiteral("Netlist"));
        schematicHistoryPanel_ = new HistoryPanel;
        schematicHistoryPanel_->setOnHistorySelected(
            [this](std::size_t position) { jumpToSchematicHistory(position); });
        schematicProperties->addTab(schematicHistoryPanel_, QStringLiteral("History"));
        schematicContentLayout->addWidget(schematicProperties, 0);
        schematicLayout->addWidget(schematicContent, 1);
        documents->addTab(schematicPage, QStringLiteral("Schematic"));

        auto *layoutPage = new QWidget;
        layoutPage_ = layoutPage;
        auto *layoutLayout = new QVBoxLayout(layoutPage);
        layoutLayout->setContentsMargins(0, 0, 0, 0);
        layoutLayout->setSpacing(0);
        layoutCellTabs_ = new QTabWidget;
        layoutCellTabs_->setDocumentMode(true);
        layoutCellTabs_->setTabsClosable(true);
        auto *layoutContent = new QWidget;
        auto *layoutContentLayout = new QHBoxLayout(layoutContent);
        layoutHierarchy_ = new ProjectPanel(false, false, true, true);
        layoutHierarchy_->setLayoutFlattenState(flattenedLayouts_);
        layoutHierarchy_->setOnFlattenChanged([this] { refreshEmbeddedPanels(); });
        layoutHierarchy_->setOnAddClicked([this] { showAddLayoutMenu(); });
        layoutHierarchy_->setOnRemoveClicked([this](ice::ObjectId id) { removeLayoutItem(id); });
        layoutHierarchy_->setOnActivateLayout([this](ice::ObjectId id) { selectLayoutTab(id); });
        layoutHierarchy_->setOnRunPex([this](ice::ObjectId id) {
            activeLayoutId_ = id;
            runPex();
        });
        layoutHierarchy_->setOnInstanceSelected([this](ice::ObjectId ownerId, ice::ObjectId instanceId) {
            selectLayoutInstanceFromHierarchy(ownerId, instanceId);
        });
        layoutHierarchy_->setOnActivatePin([this](ice::ObjectId cellId, ice::ObjectId pinId) {
            QTimer::singleShot(0, this, [this, cellId, pinId] { selectLayoutPin(cellId, pinId); });
        });
        layoutHierarchy_->setOnPinVisibilityChanged([this](ice::ObjectId cellId, ice::ObjectId pinId, bool visible) {
            setLayoutPinVisible(cellId, pinId, visible);
        });
        layoutHierarchy_->setOnRegenerateProceduralLayout([this](ice::ObjectId id) { regenerateProceduralLayout(id); });
        layoutHierarchy_->setOnRenameLayout(
            [this](ice::ObjectId id, const std::string &name) { renameLayoutCell(id, name); });
        layoutHierarchy_->setOnRenameProceduralLayout(
            [this](ice::ObjectId id, const std::string &name) { renameProceduralLayout(id, name); });
        layoutHierarchy_->setOnRenameInstance(
            [this](ice::ObjectId cellId, ice::ObjectId instanceId, const std::string &name) {
                renameLayoutInstance(cellId, instanceId, name);
            });
        layoutHierarchy_->setOnRenamePlannedDevice(
            [this](ice::ObjectId cellId, ice::ObjectId deviceId, const std::string &name) {
                renameLayoutPlannedDevice(cellId, deviceId, name);
            });
        layoutHierarchy_->setOnRenameShape(
            [this](ice::ObjectId cellId, ice::ObjectId shapeId, const std::string &name) {
                renameLayoutShape(cellId, shapeId, name);
            });
        layoutLeftTabs_ = new QTabWidget;
        layoutLeftTabs_->addTab(layoutHierarchy_, QStringLiteral("Hierarchy"));
        layoutDevicesPanel_ = new LayoutDevicesPanel;
        layoutLeftTabs_->addTab(layoutDevicesPanel_, QStringLiteral("Devices"));
        layoutContentLayout->addWidget(layoutLeftTabs_, 0);
        auto *layoutToolbar = createEditorToolbar(true);
        layoutToolbar->setOrientation(Qt::Vertical);
        layoutToolbarColumn_ = new QWidget;
        auto *layoutToolbarLayout = new QVBoxLayout(layoutToolbarColumn_);
        layoutToolbarLayout->setContentsMargins(0, 0, 0, 0);
        layoutToolbarLayout->addStretch(1);
        layoutToolbarLayout->addWidget(layoutToolbar, 0, Qt::AlignHCenter);
        layoutToolbarLayout->addStretch(1);
        layoutContentLayout->addWidget(layoutToolbarColumn_, 0);
        layoutEditorPage_ = new QWidget;
        auto *layoutEditorLayout = new QVBoxLayout(layoutEditorPage_);
        layoutEditorLayout->setContentsMargins(0, 0, 0, 0);
        layoutEditorLayout->setSpacing(0);
        layoutEditorLayout->addWidget(layoutCellTabs_);
        layoutCanvasPage_ = makeEditor(QStringLiteral("Layout editor"), true);
        proceduralEditor_ = new ProceduralLayoutEditor;
        proceduralEditor_->setOnScriptEdited([this](const QString &script) { onProceduralScriptEdited(script); });
        proceduralEditor_->setOnRegenerate([this] {
            if (activeProceduralLayoutId_ != 0)
                regenerateProceduralLayout(activeProceduralLayoutId_);
        });
        proceduralEditorPage_ = proceduralEditor_;
        layoutViewStack_ = new QStackedWidget;
        layoutViewStack_->addWidget(layoutCanvasPage_);
        layoutViewStack_->addWidget(proceduralEditorPage_);
        layoutEditorLayout->addWidget(layoutViewStack_, 1);
        layoutEmptyPanel_ = new EmptyLayoutPanel;
        layoutEmptyPanel_->setOnCreate(
            [this](EmptyLayoutPanel::Kind kind, const QString &name, const std::string &pdk) {
                switch (kind) {
                case EmptyLayoutPanel::Kind::StaticLayout:
                    createLayoutCell(name, pdk);
                    break;
                case EmptyLayoutPanel::Kind::ProceduralLayout:
                    createProceduralLayout(name, pdk);
                    break;
                }
            });
        layoutEmptyPanel_->setOnOpen([this](ice::ObjectId id) { openLayoutFile(id); });
        layoutEditorStack_ = new QStackedWidget;
        layoutEditorStack_->addWidget(layoutEmptyPanel_);
        layoutEditorStack_->addWidget(layoutEditorPage_);
        layoutContentLayout->addWidget(layoutEditorStack_, 1);
        auto *layoutOptionsToolbar = createLayoutOptionsToolbar();
        layoutOptionsToolbar->setOrientation(Qt::Vertical);
        layoutOptionsColumn_ = new QWidget;
        auto *layoutOptionsLayout = new QVBoxLayout(layoutOptionsColumn_);
        layoutOptionsLayout->setContentsMargins(0, 0, 0, 0);
        layoutOptionsLayout->addStretch(1);
        layoutOptionsLayout->addWidget(layoutOptionsToolbar, 0, Qt::AlignHCenter);
        layoutOptionsLayout->addStretch(1);
        layoutContentLayout->addWidget(layoutOptionsColumn_, 0);
        layoutRightTabs_ = new QTabWidget;
        auto *layoutRightTabs = layoutRightTabs_;
        layoutPropertiesPanel_ = new PropertiesPanel(false);
        layoutPropertiesPanel_->setOnLayoutCellParameterChanged(
            [this](ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name, const std::string &value) {
                updateLayoutCellParameter(cellId, parameterId, name, value);
            });
        layoutPropertiesPanel_->setOnLayoutDeviceParameterChanged(
            [this](const std::string &deviceName, const std::string &name, const std::string &value) {
                updateLayoutDeviceParameter(deviceName, name, value);
            });
        layoutPropertiesPanel_->setOnLayoutPortChanged(
            [this](const ice::Port &port, const std::string &netName) { updateLayoutPort(port, netName); });
        layoutPropertiesPanel_->setOnPathWidthChanged([this](int width) { setLayoutPathWidth(width); });
        layoutPropertiesPanel_->setOnGridSpacingChanged([this](int spacing) {
            if (!project_ || spacing <= 0)
                return;
            project_->execute("set layout grid", [&](ice::Project &project) { project.grid = spacing; });
            if (layoutCanvas_)
                layoutCanvas_->setGridSpacing(spacing);
            if (gridStatusLabel_)
                gridStatusLabel_->setText(QStringLiteral("Grid: %1").arg(spacing));
        });
        layoutPropertiesPanel_->setOnLayoutShapeSizeChanged(
            [this](ice::ObjectId shapeId, int width, int height) { resizeLayoutRectangle(shapeId, width, height); });
        layoutRightTabs->addTab(layoutPropertiesPanel_, QStringLiteral("Properties"));
        layersPanel_ = new LayersPanel;
        layersPanel_->setOnCreateLayer([this] { createLayer(); });
        layersPanel_->setOnLayerSelected([this](ice::ObjectId layerId) {
            activeLayoutLayerId_ = layerId;
            if (!project_ || !layoutCanvas_)
                return;
            const auto styles = layerStylesFor(project_->project());
            if (const auto style = styles.find(layerId); style != styles.end())
                layoutCanvas_->setRectangleStyle(style->second);
        });
        layersPanel_->setOnLayerVisibilityChanged([this](ice::ObjectId layerId, bool visible) {
            if (!project_)
                return;
            project_->execute("toggle layer visibility", [&](ice::Project &project) {
                const auto layer = project.layers.find(layerId);
                if (layer == project.layers.end() ||
                    !layerAvailableForLayout(project, activeLayoutId_, layer->second) ||
                    layer->second.visible == visible)
                    return;
                layer->second.visible = visible;
            });
            refreshLayout();
        });
        layersPanel_->setOnLayerGroupVisibilityChanged(
            [this](const std::vector<ice::ObjectId> &layerIds, bool visible) {
                if (!project_ || layerIds.empty())
                    return;
                bool changed = false;
                project_->execute(visible ? "show layer group" : "hide layer group", [&](ice::Project &project) {
                    for (const auto layerId : layerIds) {
                        const auto layer = project.layers.find(layerId);
                        if (layer == project.layers.end() ||
                            !layerAvailableForLayout(project, activeLayoutId_, layer->second) ||
                            layer->second.visible == visible)
                            continue;
                        layer->second.visible = visible;
                        changed = true;
                    }
                });
                if (changed)
                    refreshLayout();
            });
        layersPanel_->setOnLayerVisibilityFilter([this](bool pinOnly) {
            if (!project_)
                return;
            bool changed = false;
            project_->execute(
                pinOnly ? "show only pin layers" : "show only drawing layers", [&](ice::Project &project) {
                    for (auto &[_, layer] : project.layers) {
                        if (!layerAvailableForLayout(project, activeLayoutId_, layer))
                            continue;
                        const bool visible = pinOnly ? layerIsPinLayer(layer) : layerIsDrawingLayer(layer);
                        if (layer.visible == visible)
                            continue;
                        layer.visible = visible;
                        changed = true;
                    }
                });
            if (changed)
                refreshLayout();
        });
        layersPanel_->setOnAllLayersVisibilityChanged([this](bool visible) {
            if (!project_)
                return;
            bool changed = false;
            project_->execute(visible ? "show all layers" : "hide all layers", [&](ice::Project &project) {
                for (auto &[_, layer] : project.layers) {
                    if (!layerAvailableForLayout(project, activeLayoutId_, layer))
                        continue;
                    if (layer.visible == visible)
                        continue;
                    layer.visible = visible;
                    changed = true;
                }
            });
            if (!changed)
                return;
            layersPanel_->setProject(project_->project(), activeLayoutId_);
            refreshLayout();
        });
        layoutRightTabs->addTab(layersPanel_, QStringLiteral("Layers"));
        layoutHistoryPanel_ = new HistoryPanel;
        layoutHistoryPanel_->setOnHistorySelected([this](std::size_t position) { jumpToLayoutHistory(position); });
        layoutRightTabs->addTab(layoutHistoryPanel_, QStringLiteral("History"));
        layoutContentLayout->addWidget(layoutRightTabs, 0);
        layoutLayout->addWidget(layoutContent, 1);
        documents->addTab(layoutPage, QStringLiteral("Layout"));

        if (layoutDevicesPanel_) {
            layoutDevicesPanel_->setOnDeviceSelected([this](const ice::LayoutDeviceCell *cell) {
                selectedLayoutDevice_ = cell;
                if (layoutCanvas_)
                    layoutCanvas_->setDevicePreview(cell);
                if (layoutPropertiesPanel_ && layoutLeftTabs_ &&
                    layoutLeftTabs_->currentWidget() == layoutDevicesPanel_) {
                    layoutPropertiesPanel_->setLayoutDevice(cell);
                    if (layoutRightTabs_)
                        layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                }
            });
            layoutDevicesPanel_->setOnDeviceActivated([this] {
                if (!selectedLayoutDevice_ || (layoutCanvas_ && layoutCanvas_->isReadOnly()))
                    return;
                if (layoutPlaceDeviceTool_)
                    layoutPlaceDeviceTool_->setChecked(true);
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::placeDevice);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(QStringLiteral("Click in the layout to place %1")
                                             .arg(QString::fromStdString(selectedLayoutDevice_->name)));
            });
            selectedLayoutDevice_ = layoutDevicesPanel_->selectedDevice();
            if (layoutCanvas_) {
                layoutCanvas_->setDevicePreview(selectedLayoutDevice_);
                layoutCanvas_->setDevicePlacementCallback([this](QPointF position) { placeLayoutDevice(position); });
                layoutCanvas_->setInstanceActivatedCallback(
                    [this](ice::ObjectId instanceId) { openLayoutInstanceCell(instanceId); });
                layoutCanvas_->setPlannedDeviceActivatedCallback(
                    [this](ice::ObjectId deviceId) { activateLayoutPlannedDevice(deviceId); });
                layoutCanvas_->setPlannedConnectionActivatedCallback(
                    [this](ice::ObjectId connectionId) { activateLayoutPlannedConnection(connectionId); });
                layoutCanvas_->setInstanceSelectionCallback([this](ice::ObjectId instanceId) {
                    if (layoutHierarchy_)
                        layoutHierarchy_->selectInstance(instanceId);
                    refreshLayoutProperties(true);
                });
                layoutCanvas_->setToolEscapeCallback([this] {
                    if (layoutSelectTool_)
                        layoutSelectTool_->setChecked(true);
                    if (layoutCanvas_)
                        layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                    clearLayoutDeviceSelection();
                    refreshLayoutProperties(true);
                });
            }
        }
        connect(layoutLeftTabs_, &QTabWidget::currentChanged, this, [this](int) {
            if (!layoutLeftTabs_ || !layoutDevicesPanel_)
                return;
            if (layoutLeftTabs_->currentWidget() == layoutDevicesPanel_) {
                refreshLayoutProperties();
                if (layoutRightTabs_)
                    layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
            } else {
                refreshLayout();
            }
        });

        simulationPage_ = new QWidget;
        auto *simulationLayout = new QVBoxLayout(simulationPage_);
        simulationLayout->setContentsMargins(0, 0, 0, 0);
        simulationLayout->setSpacing(0);
        auto *simulationSplit = new QSplitter(Qt::Horizontal);

        // ── Left panel: SimulationPanel (hierarchy-like tree) ────────────
        simulationPanel_ = new SimulationPanel;
        simulationPanel_->setMinimumWidth(180);
        simulationPanel_->setMaximumWidth(280);
        simulationSplit->addWidget(simulationPanel_);

        // ── Right: simulation editor (properties, graphs, netlist tabs) ──
        auto *simEditor = new QWidget;
        auto *simEditorLayout = new QVBoxLayout(simEditor);
        simEditorLayout->setContentsMargins(4, 4, 4, 4);
        simulationTabs_ = new QTabWidget;

        // Properties form – links to the selected SimulationItem fields
        auto *propertiesPage = new QWidget;
        auto *propertiesLayout = new QVBoxLayout(propertiesPage);
        auto *propertiesColumns = new QHBoxLayout;
        auto *propertiesColumn = new QVBoxLayout;
        auto *simulationForm = new QFormLayout;
        schematicSelector_ = new QComboBox;
        simulatorSelector_ = new QComboBox;
        simulatorSelector_->addItem(QStringLiteral("ngspice"), QStringLiteral("ngspice"));
        simulatorSelector_->addItem(QStringLiteral("Icarus Verilog (SystemVerilog)"), QStringLiteral("iverilog"));
        simulatorSelector_->addItem(QStringLiteral("Mixed-signal (ngspice + Icarus)"), QStringLiteral("mixed"));
        analysisSelector_ = new QComboBox;
        analysisSelector_->addItem(QStringLiteral("Transient"), QStringLiteral("transient"));
        analysisSelector_->addItem(QStringLiteral("DC sweep"), QStringLiteral("dc"));
        analysisSelector_->addItem(QStringLiteral("AC sweep"), QStringLiteral("ac"));
        analysisSelector_->addItem(QStringLiteral("Operating point"), QStringLiteral("op"));
        analysisSelector_->addItem(QStringLiteral("Digital / VCD"), QStringLiteral("digital"));
        stopTime_ = new QDoubleSpinBox;
        stopTime_->setRange(1e-12, 1e9);
        stopTime_->setDecimals(9);
        stopTime_->setValue(1.0);
        stepTime_ = new QDoubleSpinBox;
        stepTime_->setRange(1e-12, 1e9);
        stepTime_->setDecimals(9);
        stepTime_->setValue(0.01);
        useInitialConditions_ = new QCheckBox(QStringLiteral("Use initial conditions"));
        analysisControl_ = new QLineEdit;
        analysisControl_->setReadOnly(true);
        analysisControl_->setPlaceholderText(QStringLiteral("Generated analysis control line"));
        signalsCapture_ = new QLineEdit;
        signalsCapture_->setClearButtonEnabled(true);
        signalsCapture_->setPlaceholderText(QStringLiteral("e.g. v(out), v(in)  (comma-separated)"));
        signalSelectButton_ = new QPushButton;
        signalSelectButton_->setFixedWidth(32);
        signalSelectButton_->setToolTip(QStringLiteral("Select a node or wire from the schematic"));
        signalSelectButton_->setAccessibleName(QStringLiteral("Select signal from schematic"));
        signalSelectButton_->setEnabled(false);
        setLucideIcon(signalSelectButton_, QStringLiteral("mouse-pointer-2"), 16);
        auto *signalRow = new QWidget;
        auto *signalRowLayout = new QHBoxLayout(signalRow);
        signalRowLayout->setContentsMargins(0, 0, 0, 0);
        signalRowLayout->addWidget(signalsCapture_, 1);
        signalRowLayout->addWidget(signalSelectButton_);
        additionalDirectives_ = new QLineEdit;
        additionalDirectives_->setPlaceholderText(
            QStringLiteral("Additional SPICE directives, for example: .include models.sp"));
        simulationForm->addRow(QStringLiteral("Schematic"), schematicSelector_);
        simulationForm->addRow(QStringLiteral("Simulator"), simulatorSelector_);
        simulationForm->addRow(QStringLiteral("Analysis"), analysisSelector_);
        simulationForm->addRow(QStringLiteral("Stop time / end"), stopTime_);
        simulationForm->addRow(QStringLiteral("Step / increment"), stepTime_);
        simulationForm->addRow(QString(), useInitialConditions_);
        simulationForm->addRow(QStringLiteral("Analysis Control Line"), analysisControl_);
        simulationForm->addRow(QStringLiteral("Signals to capture"), signalRow);
        simulationForm->addRow(QStringLiteral("Additional directives"), additionalDirectives_);
        propertiesColumn->addLayout(simulationForm);
        propertiesColumn->addStretch();

        auto *runButton = new QPushButton(QStringLiteral("Run Simulation"));
        setLucideIcon(runButton, QStringLiteral("play"), 16);
        runButton->setToolTip(QStringLiteral("Run simulation"));
        runButton->setAccessibleName(QStringLiteral("Run simulation"));
        runButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        runButton->setEnabled(false);
        simulationRunButton_ = runButton;
        simulationBuildNetlistButton_ = new QPushButton(QStringLiteral("Build Netlist"));
        setLucideIcon(simulationBuildNetlistButton_, QStringLiteral("refresh-cw"), 16);
        simulationBuildNetlistButton_->setToolTip(QStringLiteral("Generate the netlist for this simulation"));
        simulationBuildNetlistButton_->setAccessibleName(QStringLiteral("Build simulation netlist"));
        simulationBuildNetlistButton_->setEnabled(false);
        simulationBuildNetlistButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *runRow = new QHBoxLayout;
        runRow->addWidget(simulationBuildNetlistButton_, 1);
        runRow->addWidget(runButton, 1);
        propertiesColumn->addLayout(runRow);

        simulationNetlist_ = new QPlainTextEdit;
        simulationNetlist_->setReadOnly(true);
        simulationNetlist_->setLineWrapMode(QPlainTextEdit::NoWrap);
        simulationNetlist_->setPlaceholderText(QStringLiteral("Build the netlist to see it here."));
        simulationOutput_ = new QPlainTextEdit;
        simulationOutput_->setReadOnly(true);
        simulationOutput_->setPlaceholderText(QStringLiteral("Simulation status and result summary"));
        auto *rightSplitter = new QSplitter(Qt::Vertical);
        rightSplitter->setChildrenCollapsible(false);
        rightSplitter->addWidget(simulationNetlist_);
        rightSplitter->addWidget(simulationOutput_);
        rightSplitter->setStretchFactor(0, 1);
        rightSplitter->setStretchFactor(1, 1);
        propertiesColumns->addLayout(propertiesColumn, 1);
        propertiesColumns->addWidget(rightSplitter, 1);
        propertiesLayout->addLayout(propertiesColumns, 1);
        simulationTabs_->addTab(propertiesPage, QStringLiteral("Properties"));

        // Graph tabs area
        auto *graphsPage = new QWidget;
        simulationGraphsPage_ = graphsPage;
        auto *graphsLayout = new QHBoxLayout(graphsPage);
        graphsLayout->setContentsMargins(0, 0, 0, 0);
        graphsLayout->setSpacing(4);
        auto *graphControls = new QWidget;
        graphControls->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        auto *graphControlsLayout = new QVBoxLayout(graphControls);
        graphControlsLayout->setContentsMargins(0, 0, 0, 0);
        graphControlsLayout->setSpacing(2);
        simulationAddGraphButton_ = new QPushButton;
        simulationAddGraphButton_->setToolTip(QStringLiteral("Add graph to this simulation"));
        simulationAddGraphButton_->setAccessibleName(QStringLiteral("Add graph"));
        simulationAddGraphButton_->setFixedWidth(32);
        simulationAddGraphButton_->setEnabled(false);
        setLucideIcon(simulationAddGraphButton_, QStringLiteral("plus"), 16);
        simulationDeleteGraphButton_ = new QPushButton;
        simulationDeleteGraphButton_->setToolTip(QStringLiteral("Delete current graph"));
        simulationDeleteGraphButton_->setAccessibleName(QStringLiteral("Delete current graph"));
        simulationDeleteGraphButton_->setFixedWidth(32);
        simulationDeleteGraphButton_->setEnabled(false);
        setLucideIcon(simulationDeleteGraphButton_, QStringLiteral("trash"), 16);
        simulationEditGraphButton_ = new QPushButton;
        simulationEditGraphButton_->setToolTip(QStringLiteral("Edit graph name, axis selections, and limits"));
        simulationEditGraphButton_->setAccessibleName(QStringLiteral("Edit graph"));
        simulationEditGraphButton_->setFixedWidth(32);
        simulationEditGraphButton_->setEnabled(false);
        setLucideIcon(simulationEditGraphButton_, QStringLiteral("pencil"), 16);
        simulationGridButton_ = new QPushButton;
        simulationGridButton_->setCheckable(true);
        simulationGridButton_->setChecked(true);
        simulationGridButton_->setToolTip(QStringLiteral("Show or hide the graph grid"));
        simulationGridButton_->setAccessibleName(QStringLiteral("Toggle graph grid"));
        simulationGridButton_->setFixedWidth(32);
        simulationGridButton_->setEnabled(false);
        setLucideIcon(simulationGridButton_, QStringLiteral("grid-3x3"), 16);
        simulationLoadVcdButton_ = new QPushButton;
        simulationLoadVcdButton_->setToolTip(QStringLiteral("Load VCD traces into this simulation's graphs"));
        simulationLoadVcdButton_->setAccessibleName(QStringLiteral("Load VCD"));
        simulationLoadVcdButton_->setFixedWidth(32);
        simulationLoadVcdButton_->setEnabled(false);
        setLucideIcon(simulationLoadVcdButton_, QStringLiteral("notebook-text"), 16);
        graphControlsLayout->addStretch(1);
        graphControlsLayout->addWidget(simulationAddGraphButton_);
        graphControlsLayout->addWidget(simulationDeleteGraphButton_);
        graphControlsLayout->addWidget(simulationEditGraphButton_);
        graphControlsLayout->addWidget(simulationGridButton_);
        graphControlsLayout->addWidget(simulationLoadVcdButton_);
        graphControlsLayout->addStretch(1);

        graphTabs_ = new QTabWidget;
        graphTabs_->setDocumentMode(true);
        graphTabs_->setTabsClosable(false);
        simulationPlot_ = createGraphPlot();
        graphTabs_->addTab(simulationPlot_, QStringLiteral("Graph 1"));
        graphsLayout->addWidget(graphControls, 0);
        graphsLayout->addWidget(graphTabs_, 1);
        simulationTabs_->addTab(graphsPage, QStringLiteral("Graphs"));

        auto *testbenchPage = new QWidget;
        simulationTestbenchPage_ = testbenchPage;
        auto *testbenchLayout = new QVBoxLayout(testbenchPage);
        testbenchLayout->setContentsMargins(0, 0, 0, 0);
        auto *testbenchToolbar = new QHBoxLayout;
        auto *testbenchHint = new QLabel(QStringLiteral(
            "SystemVerilog testbench source for the selected simulation. Changes are saved with the project."));
        testbenchHint->setWordWrap(true);
        simulationTestbenchRunButton_ = new QPushButton(QStringLiteral("Run Simulation"));
        setLucideIcon(simulationTestbenchRunButton_, QStringLiteral("play"), 16);
        simulationTestbenchRunButton_->setToolTip(QStringLiteral("Run the selected simulation"));
        simulationTestbenchRunButton_->setAccessibleName(QStringLiteral("Run simulation from testbench"));
        simulationTestbenchRunButton_->setEnabled(false);
        connect(simulationTestbenchRunButton_, &QPushButton::clicked, this, [this] { runSimulation(); });
        testbenchToolbar->addWidget(testbenchHint, 1);
        testbenchToolbar->addWidget(simulationTestbenchRunButton_);
        testbenchLayout->addLayout(testbenchToolbar);
        simulationTestbenchEditor_ = new TextDocumentEditor;
        simulationTestbenchEditor_->setLanguage(TextDocumentLanguage::SystemVerilog);
        simulationTestbenchEditor_->setOnTextEdited([this](const QString &text) { onSimulationTestbenchEdited(text); });
        testbenchLayout->addWidget(simulationTestbenchEditor_, 1);
        simulationTestbenchTabIndex_ = simulationTabs_->addTab(testbenchPage, QStringLiteral("Testbench"));
        simulationTabs_->setTabVisible(simulationTestbenchTabIndex_, false);
        simEditorLayout->addWidget(simulationTabs_, 1);

        simulationEditorPage_ = simEditor;
        simulationRightStack_ = new QStackedWidget;
        simulationRightStack_->addWidget(simEditor);
        simulationSplit->addWidget(simulationRightStack_);
        simulationLayout->addWidget(simulationSplit, 1);

        // ── Wire SimulationPanel ─────────────────────────────────────────
        simulationPanel_->setOnAddClicked([this] { addSimulation(); });
        simulationPanel_->setOnAddSystemVerilogTestbench([this] { addSystemVerilogTestbench(); });
        simulationPanel_->setOnAddReport([this] { addReport(); });
        simulationPanel_->setOnOpenReport([this](ice::ObjectId reportId) { openReportFile(reportId); });
        simulationPanel_->setOnDeleteClicked([this] { deleteSimulation(); });
        simulationPanel_->setOnDeleteReport([this](ice::ObjectId reportId) { deleteReport(reportId); });
        simulationPanel_->setOnSelection([this](ice::ObjectId simId, ice::ObjectId graphId) {
            if (simId != 0 && reportDirty_)
                saveReportSource();
            loadSimulationById(simId, graphId);
            if (simId != 0 && simulationRightStack_ && simulationEditorPage_)
                simulationRightStack_->setCurrentWidget(simulationEditorPage_);
            if (simId != 0 && documents_ && simulationPage_)
                documents_->setCurrentWidget(simulationPage_);
        });
        simulationPanel_->setOnReportSelection([this](ice::ObjectId reportId) { selectReport(reportId); });
        simulationPanel_->setOnAddGraph([this](ice::ObjectId simId) { addGraph(simId); });
        simulationPanel_->setOnDeleteGraph(
            [this](ice::ObjectId simId, ice::ObjectId graphId) { deleteGraph(simId, graphId); });

        // ── Wire properties form → save back to project ──────────────────
        auto saveSimProps = [this] { commitSimulationProperties(); };
        connect(schematicSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, saveSimProps] {
            applySimulationLanguageDefaults();
            saveSimProps();
            updateSimulationNetlistState();
        });
        connect(simulatorSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [saveSimProps] { saveSimProps(); });
        connect(analysisSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stopTime_, &QDoubleSpinBox::editingFinished, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stepTime_, &QDoubleSpinBox::editingFinished, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stopTime_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { updateAnalysisControlLine(); });
        connect(stepTime_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { updateAnalysisControlLine(); });
        connect(useInitialConditions_, &QCheckBox::toggled, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(signalsCapture_, &QLineEdit::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(signalSelectButton_, &QPushButton::clicked, this, [this] { pickSimulationSignal(); });
        connect(additionalDirectives_, &QLineEdit::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(additionalDirectives_, &QLineEdit::textChanged, this,
                [this](const QString &) { updateSimulationNetlistState(); });
        connect(simulationBuildNetlistButton_, &QPushButton::clicked, this, [this] { buildSimulationNetlist(); });
        updateAnalysisControlLine();
        updateSimulationControlStates();

        // ── Wire graph buttons ────────────────────────────────────────────
        connect(simulationAddGraphButton_, &QPushButton::clicked, this, [this] { addGraph(activeSimulationId_); });
        connect(simulationDeleteGraphButton_, &QPushButton::clicked, this, [this] { deleteCurrentGraph(); });
        connect(simulationEditGraphButton_, &QPushButton::clicked, this, [this] { editGraph(); });
        connect(simulationGridButton_, &QPushButton::toggled, this, [this](bool visible) {
            graphGridVisible_ = visible;
            if (!graphTabs_)
                return;
            for (int index = 0; index < graphTabs_->count(); ++index)
                if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                    plot->setGridVisible(visible);
        });
        connect(simulationLoadVcdButton_, &QPushButton::clicked, this, [this] { loadVcdGraphs(); });
        connect(graphTabs_, &QTabWidget::currentChanged, this, [this](int) { refreshGraphPlot(); });
        connect(simulationTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (simulationTabs_ && simulationGraphsPage_ && simulationTabs_->widget(index) == simulationGraphsPage_)
                loadCachedSimulationRun();
        });

        // ── Wire schematic / layout tabs ─────────────────────────────────
        connect(schematicCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0)
                return;
            activateSchematicTab(index);
        });
        connect(layoutCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0)
                return;
            activateLayoutTab(index);
        });
        connect(schematicCellTabs_, &QTabWidget::tabCloseRequested, this,
                [this](int index) { closeSchematicTab(index); });
        connect(layoutCellTabs_, &QTabWidget::tabCloseRequested, this, [this](int index) { closeLayoutTab(index); });
        connect(documents_, &QTabWidget::currentChanged, this, [this](int) {
            if (schematicCanvas_ && schematicCanvas_->isSignalPickMode() &&
                documents_->currentWidget() != schematicPage_)
                schematicCanvas_->setSignalPickMode(false);
            if (documents_ && documents_->currentWidget() == simulationPage_) {
                if (activeSimulationId_ == 0 && project_ && !project_->project().simulations.empty()) {
                    if (simulationPanel_)
                        simulationPanel_->selectSimulation(project_->project().simulations.begin()->first);
                } else {
                    loadCachedSimulationRun();
                }
            }
        });

        connect(runButton, &QPushButton::clicked, this, &DocumentWindow::runSimulation);
        documents->addTab(simulationPage_, QStringLiteral("Simulation"));

        auto *reportsView = new QWidget;
        reportsView_ = reportsView;
        auto *reportsLayout = new QVBoxLayout(reportsView_);
        reportsLayout->setContentsMargins(0, 0, 0, 0);
        auto *reportsToolbar = new QHBoxLayout;
        reportsToolbar->setContentsMargins(4, 4, 4, 4);
        reportTitleLabel_ = new QLabel(QStringLiteral("No simulation script selected"));
        reportTitleLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
        reportsToolbar->addWidget(reportTitleLabel_, 1);
        reportStatusLabel_ = new QLabel;
        reportsToolbar->addWidget(reportStatusLabel_);
        reportSaveButton_ = new QPushButton(QStringLiteral("Save"));
        reportSaveButton_->setToolTip(QStringLiteral("Save the simulation script source"));
        reportsToolbar->addWidget(reportSaveButton_);
        reportRunButton_ = new QPushButton(QStringLiteral("Run"));
        reportRunButton_->setToolTip(QStringLiteral("Save and run the selected simulation script"));
        setLucideIcon(reportRunButton_, QStringLiteral("play"), 16);
        reportsToolbar->addWidget(reportRunButton_);
        reportOpenButton_ = new QPushButton(QStringLiteral("Open File"));
        reportOpenButton_->setToolTip(QStringLiteral("Open the simulation script source in an external editor"));
        setLucideIcon(reportOpenButton_, QStringLiteral("notebook-text"), 16);
        reportsToolbar->addWidget(reportOpenButton_);
        reportsLayout->addLayout(reportsToolbar);

        reportCodeEditor_ = new QPlainTextEdit;
        reportCodeEditor_->setReadOnly(true);
        reportCodeEditor_->setLineWrapMode(QPlainTextEdit::NoWrap);
        auto reportFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        reportFont.setPointSizeF(std::max(reportFont.pointSizeF(), 10.0));
        reportCodeEditor_->setFont(reportFont);
        reportCodeEditor_->setTabStopDistance(reportCodeEditor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        reportCodeEditor_->setPlaceholderText(QStringLiteral("Select a simulation script to edit its Python source."));
        reportHighlighter_ = new PythonSyntaxHighlighter(reportCodeEditor_->document());
        scriptViewSplitter_ = new QSplitter(Qt::Vertical);
        scriptViewSplitter_->addWidget(reportCodeEditor_);
        scriptGraphTabs_ = new QTabWidget;
        scriptGraphTabs_->setDocumentMode(true);
        scriptGraphTabs_->hide();
        scriptViewSplitter_->addWidget(scriptGraphTabs_);
        scriptViewSplitter_->setStretchFactor(0, 3);
        scriptViewSplitter_->setStretchFactor(1, 2);
        reportsLayout->addWidget(scriptViewSplitter_, 1);
        simulationRightStack_->addWidget(reportsView_);

        reportSaveTimer_ = new QTimer(this);
        reportSaveTimer_->setSingleShot(true);
        reportSaveTimer_->setInterval(600);
        connect(reportSaveTimer_, &QTimer::timeout, this, [this] { saveReportSource(); });
        connect(reportCodeEditor_, &QPlainTextEdit::textChanged, this, [this] {
            if (loadingReport_)
                return;
            reportDirty_ = true;
            setReportStatus(QStringLiteral("Unsaved changes"));
            if (reportSaveTimer_)
                reportSaveTimer_->start();
        });
        connect(reportSaveButton_, &QPushButton::clicked, this, [this] { saveReportSource(); });
        connect(reportRunButton_, &QPushButton::clicked, this, [this] { runSelectedReport(); });
        connect(reportOpenButton_, &QPushButton::clicked, this, [this] { openSelectedReportFile(); });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            if (reportDirty_)
                saveReportSource();
        });
        reportSaveButton_->setEnabled(false);
        reportRunButton_->setEnabled(false);
        reportOpenButton_->setEnabled(false);

        workspaceStack_ = new WorkspaceStack;
        startupView_ = new StartupView;
        startupView_->setOnCreateProject([this](const QString &name, const QString &location, bool createFolder) {
            createProject(name, location, createFolder);
        });
        startupView_->setOnOpenProject([this] { openProject(); });
        startupView_->setOnOpenRecent([this](const QString &path) { openProjectPath(path); });
        startupView_->setOnRemoveRecent([this](const QString &path) { removeRecentProject(path); });
        startupView_->setRecentProjects(loadRecentProjects());
        workspaceStack_->addWidget(startupView_);
        workspaceStack_->addWidget(documents);
        setCentralWidget(workspaceStack_);
        updateEmptyWorkspaceState();
        shortcuts_.attachScope(ice::ShortcutScope::schematic, schematicCanvas_);
        shortcuts_.attachScope(ice::ShortcutScope::layout, layoutCanvas_);
    }

    QWidget *makeEditor(const QString &title, bool layoutEditor) {
        auto *editor = new QWidget;
        auto *editorLayout = new QVBoxLayout(editor);
        QGraphicsView *view = nullptr;
        if (!layoutEditor) {
            schematicCanvas_ = createSchematicCanvas();
            schematicCanvas_->setCallbacks(
                [this](ice::ObjectId id, QPointF position) {
                    if (!project_)
                        return;
                    std::string historyLabel;
                    bool moved = false;
                    for (const auto &[_, cell] : project_->project().schematicCells) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            historyLabel =
                                "Moved " +
                                (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) + " " +
                                instance->second.name;
                            break;
                        }
                    }
                    project_->execute("move symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells) {
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                moved = it->second.x != static_cast<ice::Coordinate>(position.x()) ||
                                        it->second.y != static_cast<ice::Coordinate>(-position.y());
                                it->second.x = static_cast<ice::Coordinate>(position.x());
                                it->second.y = static_cast<ice::Coordinate>(-position.y());
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                            if (auto it = cell.comments.find(id); it != cell.comments.end()) {
                                moved = it->second.x != static_cast<ice::Coordinate>(position.x()) ||
                                        it->second.y != static_cast<ice::Coordinate>(-position.y());
                                it->second.x = static_cast<ice::Coordinate>(position.x());
                                it->second.y = static_cast<ice::Coordinate>(-position.y());
                                return;
                            }
                        }
                    });
                    if (moved && !historyLabel.empty())
                        project_->setLastHistoryLabel(std::move(historyLabel));
                },
                [this](ice::ObjectId id) {
                    if (!project_)
                        return;
                    std::string historyLabel;
                    for (const auto &[_, cell] : project_->project().schematicCells) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            historyLabel =
                                "Rotated " +
                                (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) + " " +
                                instance->second.name + " - 90°";
                            break;
                        }
                    }
                    project_->execute("rotate symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                it->second.orientation =
                                    composeOrientation(it->second.orientation, ice::Orientation::r90);
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                    if (!historyLabel.empty())
                        project_->setLastHistoryLabel(std::move(historyLabel));
                    refreshProject();
                },
                [this](ice::ObjectId firstPin, ice::ObjectId secondPin) {
                    if (!project_ || firstPin == 0 || secondPin == 0 || firstPin == secondPin)
                        return;
                    auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
                    if (cellIt == project_->project().schematicCells.end())
                        cellIt = project_->project().schematicCells.begin();
                    if (cellIt == project_->project().schematicCells.end())
                        return;
                    const auto cellId = cellIt->first;
                    const auto findNet = [&](ice::ObjectId pin) {
                        for (const auto &[id, net] : cellIt->second.nets)
                            if (std::find(net.pins.begin(), net.pins.end(), pin) != net.pins.end())
                                return id;
                        return ice::ObjectId{0};
                    };
                    const auto firstNet = findNet(firstPin);
                    const auto secondNet = findNet(secondPin);
                    if (firstNet != 0 && firstNet == secondNet)
                        return;
                    std::vector<SchematicBranchRoute> inheritedBranches;
                    for (const auto netId : {firstNet, secondNet}) {
                        if (netId == 0)
                            continue;
                        if (const auto cached = schematicRouteCache_[cellId].find(netId);
                            cached != schematicRouteCache_[cellId].end())
                            inheritedBranches.insert(inheritedBranches.end(), cached->second.branches.begin(),
                                                     cached->second.branches.end());
                    }
                    ice::ObjectId finalNet = 0;
                    project_->execute("connect pins", [&](ice::Project &project) {
                        auto &cell = project.schematicCells.at(cellId);
                        const auto netForPin = [&](ice::ObjectId pin) -> ice::ObjectId {
                            for (const auto &[id, net] : cell.nets)
                                if (std::find(net.pins.begin(), net.pins.end(), pin) != net.pins.end())
                                    return id;
                            return 0;
                        };
                        const auto first = netForPin(firstPin);
                        const auto second = netForPin(secondPin);
                        if (first != 0 && first == second) {
                            finalNet = first;
                            return;
                        }
                        if (first == 0 && second == 0) {
                            finalNet = project.nextId();
                            cell.nets.emplace(
                                finalNet, ice::Net{finalNet, "N" + std::to_string(finalNet), {firstPin, secondPin}});
                        } else if (first != 0 && second == 0) {
                            finalNet = first;
                            auto &pins = cell.nets.at(first).pins;
                            if (std::find(pins.begin(), pins.end(), secondPin) == pins.end())
                                pins.push_back(secondPin);
                        } else if (first == 0 && second != 0) {
                            finalNet = second;
                            auto &pins = cell.nets.at(second).pins;
                            if (std::find(pins.begin(), pins.end(), firstPin) == pins.end())
                                pins.push_back(firstPin);
                        } else {
                            finalNet = first;
                            auto &target = cell.nets.at(first);
                            auto &source = cell.nets.at(second);
                            for (const auto pin : source.pins)
                                if (std::find(target.pins.begin(), target.pins.end(), pin) == target.pins.end())
                                    target.pins.push_back(pin);
                            const auto automaticName = [](const ice::Net &net) {
                                return net.name.empty() || net.name == "N" + std::to_string(net.id);
                            };
                            if (automaticName(target) && !source.name.empty() && !automaticName(source))
                                target.name = source.name;
                            if (!target.labelAnchor && source.labelAnchor)
                                target.labelAnchor = source.labelAnchor;
                            cell.nets.erase(second);
                        }
                        // A pin may only belong to one net. Clean up any
                        // duplicate memberships and make sure both edited pins
                        // are present in the final net.
                        std::vector<ice::ObjectId> removeNets;
                        for (auto &[id, net] : cell.nets) {
                            if (id != finalNet) {
                                net.pins.erase(std::remove_if(net.pins.begin(), net.pins.end(),
                                                              [&](ice::ObjectId pin) {
                                                                  return pin == firstPin || pin == secondPin;
                                                              }),
                                               net.pins.end());
                                if (net.pins.size() < 2)
                                    removeNets.push_back(id);
                            }
                        }
                        for (const auto id : removeNets)
                            cell.nets.erase(id);
                        auto &final = cell.nets.at(finalNet);
                        std::set<ice::ObjectId> uniquePins;
                        final.pins.erase(
                            std::remove_if(final.pins.begin(), final.pins.end(),
                                           [&](ice::ObjectId pin) { return !uniquePins.insert(pin).second; }),
                            final.pins.end());
                        for (const auto pin : {firstPin, secondPin})
                            if (std::find(final.pins.begin(), final.pins.end(), pin) == final.pins.end())
                                final.pins.push_back(pin);
                        if (!final.labelAnchor)
                            final.labelAnchor = ice::NetLabelAnchor{firstPin, secondPin, 0.5, false};
                        updateAutomaticNetLabelPositions(project);
                    });
                    if (finalNet != 0) {
                        auto &cellCache = schematicRouteCache_[cellId];
                        cellCache.erase(firstNet);
                        cellCache.erase(secondNet);
                        auto &branches = cellCache[finalNet].branches;
                        branches = std::move(inheritedBranches);
                        const auto duplicate = std::any_of(branches.begin(), branches.end(), [&](const auto &branch) {
                            return (branch.fromPin == firstPin && branch.toPin == secondPin) ||
                                   (branch.fromPin == secondPin && branch.toPin == firstPin);
                        });
                        if (!duplicate) {
                            SchematicBranchRoute branch;
                            branch.fromPin = firstPin;
                            branch.toPin = secondPin;
                            branches.push_back(branch);
                        }
                    }
                    refreshProject();
                },
                [this](QPointF position) { placeComment(position); },
                [this] {
                    if (!project_)
                        return;
                    project_->markDirty();
                    refreshSchematic();
                },
                [this](ice::ObjectId) { updateSelectionStatus(); },
                [this](ice::ObjectId instanceId) { openSymbolProperties(instanceId); });
            schematicCanvas_->setDisconnectCallback([this](ice::ObjectId pinId) {
                if (!project_)
                    return;
                auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
                if (cellIt == project_->project().schematicCells.end())
                    cellIt = project_->project().schematicCells.begin();
                if (cellIt == project_->project().schematicCells.end())
                    return;
                const auto cellId = cellIt->first;
                bool changed = false;
                project_->execute("disconnect pin", [&](ice::Project &project) {
                    auto &cell = project.schematicCells.at(cellId);
                    for (auto netIt = cell.nets.begin(); netIt != cell.nets.end();) {
                        auto &pins = netIt->second.pins;
                        const auto before = pins.size();
                        pins.erase(std::remove(pins.begin(), pins.end(), pinId), pins.end());
                        if (pins.size() == before) {
                            ++netIt;
                            continue;
                        }
                        changed = true;
                        if (pins.size() < 2)
                            netIt = cell.nets.erase(netIt);
                        else
                            ++netIt;
                    }
                    if (changed)
                        updateAutomaticNetLabelPositions(project);
                });
                if (changed)
                    refreshProject();
            });
            schematicCanvas_->setNetLabelMovedCallback(
                [this](ice::ObjectId netId, const ice::NetLabelAnchor &anchor, QPointF position) {
                    if (!project_)
                        return;
                    auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
                    if (cellIt == project_->project().schematicCells.end())
                        cellIt = project_->project().schematicCells.begin();
                    if (cellIt == project_->project().schematicCells.end())
                        return;
                    const auto cellId = cellIt->first;
                    bool changed = false;
                    project_->execute("move net label", [&](ice::Project &project) {
                        auto &cell = project.schematicCells.at(cellId);
                        auto netIt = cell.nets.find(netId);
                        if (netIt == cell.nets.end())
                            return;
                        const auto x = static_cast<ice::Coordinate>(position.x());
                        const auto y = static_cast<ice::Coordinate>(-position.y());
                        if (netIt->second.x == x && netIt->second.y == y && netIt->second.labelAnchor &&
                            netIt->second.labelAnchor->fromPin == anchor.fromPin &&
                            netIt->second.labelAnchor->toPin == anchor.toPin &&
                            netIt->second.labelAnchor->fraction == anchor.fraction &&
                            netIt->second.labelAnchor->sideB == anchor.sideB)
                            return;
                        netIt->second.x = x;
                        netIt->second.y = y;
                        netIt->second.labelAnchor = anchor;
                        changed = true;
                    });
                    if (changed)
                        refreshProject();
                    else
                        refreshSchematic();
                });
            schematicCanvas_->setNetLabelActivatedCallback([this](ice::ObjectId netId) { renameSchematicNet(netId); });
            schematicCanvas_->setFlipCallback([this](ice::ObjectId id) {
                if (!project_)
                    return;
                std::string historyLabel;
                for (const auto &[_, cell] : project_->project().schematicCells) {
                    if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                        const auto symbol = cell.symbols.find(instance->second.symbol);
                        historyLabel = "Flipped " +
                                       (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) +
                                       " " + instance->second.name + " horizontally";
                        break;
                    }
                }
                project_->execute("flip symbol", [&](ice::Project &project) {
                    for (auto &[_, cell] : project.schematicCells)
                        if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                            it->second.orientation = composeOrientation(it->second.orientation, ice::Orientation::my);
                            updateAutomaticNetLabelPositions(project);
                            return;
                        }
                });
                if (!historyLabel.empty())
                    project_->setLastHistoryLabel(std::move(historyLabel));
                refreshProject();
            });
            schematicCanvas_->setSymbolPlacementCallback([this](QPointF position) {
                if (!selectedSchematicSymbol_) {
                    statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
                    return;
                }
                const auto symbolName = selectedSchematicSymbol_->name;
                if (const auto instanceId = placeSymbolDefinition(*selectedSchematicSymbol_, position);
                    instanceId != 0) {
                    // Highlight the device that was just dropped so the user
                    // can immediately rotate or edit it.
                    schematicCanvas_->selectInstance(instanceId);
                    statusBar()->showMessage(QStringLiteral("Placed %1").arg(symbolName));
                }
            });
            schematicCanvas_->setPlaceSymbolCycleCallback([this](int delta) { cycleSchematicPlacementDevice(delta); });
            schematicCanvas_->setToolEscapeCallback([this] { selectSchematicTool(); });
            schematicCanvas_->setSignalPickedCallback(
                [this](ice::ObjectId netId) { finishSimulationSignalPick(netId); });
            schematicCanvas_->setSignalPickCancelledCallback([this] { cancelSimulationSignalPick(); });
            schematicCanvas_->setDeleteCallback([this](std::vector<ice::ObjectId> ids) {
                if (!project_)
                    return;
                std::vector<std::string> deletedNames;
                for (const auto &[_, cell] : project_->project().schematicCells) {
                    for (const auto id : ids) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            deletedNames.push_back(
                                (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) + " " +
                                instance->second.name);
                        } else if (const auto comm = cell.comments.find(id); comm != cell.comments.end()) {
                            deletedNames.push_back("comment");
                        }
                    }
                }
                project_->execute("delete symbols", [&](ice::Project &project) {
                    for (auto &[_, cell] : project.schematicCells) {
                        std::vector<ice::ObjectId> removedPins;
                        for (auto id : ids) {
                            auto instance = cell.instances.find(id);
                            if (instance != cell.instances.end()) {
                                auto symbol = cell.symbols.find(instance->second.symbol);
                                if (symbol != cell.symbols.end())
                                    for (const auto &pin : symbol->second.pins)
                                        removedPins.push_back(pin.id);
                                cell.instances.erase(instance);
                            }
                            cell.comments.erase(id);
                        }
                        for (auto &[_, net] : cell.nets)
                            net.pins.erase(std::remove_if(net.pins.begin(), net.pins.end(),
                                                          [&](ice::ObjectId pin) {
                                                              return std::find(removedPins.begin(), removedPins.end(),
                                                                               pin) != removedPins.end();
                                                          }),
                                           net.pins.end());
                        for (auto it = cell.nets.begin(); it != cell.nets.end();)
                            if (it->second.pins.size() < 2)
                                it = cell.nets.erase(it);
                            else
                                ++it;
                    }
                });
                if (deletedNames.size() == 1)
                    project_->setLastHistoryLabel("Deleted " + deletedNames.front());
                else
                    project_->setLastHistoryLabel("Deleted " + std::to_string(deletedNames.size()) +
                                                  " schematic devices");
                refreshProject();
            });
            view = schematicCanvas_;
        } else {
            layoutCanvas_ = createLayoutCanvas();
            layoutCanvas_->setGridVisible(layoutGridVisible_);
            layoutCanvas_->setSnapToGrid(layoutSnapToGrid_);
            connect(layoutCanvas_->scene(), &QGraphicsScene::selectionChanged, this,
                    [this] { refreshLayoutProperties(); });
            layoutCanvas_->setPathSegmentCallback([this](QPointF start, QPointF end, int width, ice::ObjectId layer,
                                                         const std::vector<ice::ObjectId> &viaLayers) {
                addLayoutPathSegment(start, end, width, layer, viaLayers);
            });
            layoutCanvas_->setPathLayerChangedCallback([this](ice::ObjectId, const std::string &name) {
                statusBar()->showMessage(QStringLiteral("Path layer: %1 · width: %2 nm")
                                             .arg(QString::fromStdString(name))
                                             .arg(layoutPathWidth_));
                if (layoutPropertiesPanel_ && layoutCanvas_ && layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
                    layoutPropertiesPanel_->setPathTool(layoutCanvas_->pathWidth(), name);
            });
            layoutCanvas_->setPathFinishedCallback([this] { finishActiveLayoutPath(); });
            layoutCanvas_->setTextPlacedCallback([this](QPointF position) { addLayoutText(position); });
            layoutCanvas_->setTextPolygonPlacedCallback([this](QPointF position) { addLayoutTextPolygon(position); });
            layoutCanvas_->setPinPlacedCallback([this](QPointF first, QPointF second) { addLayoutPin(first, second); });
            layoutCanvas_->setInstancePinActivatedCallback(
                [this](ice::ObjectId instance, ice::ObjectId port) { promoteInstancePin(instance, port); });
            layoutCanvas_->setPolygonCreatedCallback(
                [this](const std::vector<QPointF> &points) { addLayoutPolygon(points); });
            layoutCanvas_->setPolygonPointMovedCallback(
                [this](ice::ObjectId id, int index, QPointF position) { moveLayoutPolygonPoint(id, index, position); });
            layoutCanvas_->setShapeActivatedCallback([this](ice::ObjectId id) { highlightLayoutNet(id); });
            layoutCanvas_->setCallbacks(
                [this](QPointF first, QPointF second) {
                    if (!project_ || layoutLayers(project_->project(), activeLayoutId_).empty()) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Create a layer before drawing shapes."));
                        return;
                    }
                    if (layoutLayers(project_->project(), activeLayoutId_).size() > 1 && activeLayoutLayerId_ == 0) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Select a layer before drawing shapes."));
                        return;
                    }
                    const auto cellId = project_->project().layoutCells.contains(activeLayoutId_)
                                            ? activeLayoutId_
                                            : (project_->project().layoutCells.empty()
                                                   ? 0
                                                   : project_->project().layoutCells.begin()->first);
                    if (cellId == 0) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Create a layout cell before drawing shapes."));
                        return;
                    }
                    if (project_->project().layoutCells.at(cellId).library)
                        return;
                    const auto rectangle = QRectF(first, second).normalized();
                    const auto available = layoutLayers(project_->project(), activeLayoutId_);
                    const auto layerId = activeLayoutLayerId_ != 0 ? activeLayoutLayerId_ : available.front().get().id;
                    project_->execute("create layout rectangle", [&](ice::Project &project) {
                        ice::addRectangle(project, cellId, layerId,
                                          {static_cast<ice::Coordinate>(rectangle.left()),
                                           static_cast<ice::Coordinate>(-rectangle.bottom()),
                                           static_cast<ice::Coordinate>(rectangle.width()),
                                           static_cast<ice::Coordinate>(rectangle.height())});
                    });
                    refreshProject();
                },
                [this](ice::ObjectId objectId, QPointF delta) {
                    if (!project_)
                        return;
                    project_->execute("move layout object", [&](ice::Project &project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end() || cellIt->second.library)
                            return;
                        if (cellIt->second.shapes.contains(objectId)) {
                            const auto dx = static_cast<ice::Coordinate>(delta.x());
                            const auto dy = static_cast<ice::Coordinate>(-delta.y());
                            const auto group = cellIt->second.shapes.at(objectId).textGroup;
                            if (group != 0) {
                                for (const auto &[memberId, member] : cellIt->second.shapes)
                                    if (member.textGroup == group)
                                        ice::moveRectangle(project, activeLayoutId_, memberId, dx, dy);
                            } else {
                                ice::moveRectangle(project, activeLayoutId_, objectId, dx, dy);
                            }
                            if (cellIt->second.shapes.at(objectId).routedConnection == 0)
                                updateRoutedConnectionPaths(project);
                            return;
                        }
                        if (cellIt->second.ports.contains(objectId)) {
                            ice::movePort(project, activeLayoutId_, objectId, static_cast<ice::Coordinate>(delta.x()),
                                          static_cast<ice::Coordinate>(-delta.y()));
                            updateRoutedConnectionPaths(project);
                            return;
                        }
                        if (auto planned = cellIt->second.plannedDevices.find(objectId);
                            planned != cellIt->second.plannedDevices.end()) {
                            planned->second.x += static_cast<ice::Coordinate>(delta.x());
                            planned->second.y -= static_cast<ice::Coordinate>(delta.y());
                            updateRoutedConnectionPaths(project);
                            return;
                        }
                        if (auto instance = cellIt->second.instances.find(objectId);
                            instance != cellIt->second.instances.end()) {
                            instance->second.x += static_cast<ice::Coordinate>(delta.x());
                            instance->second.y -= static_cast<ice::Coordinate>(delta.y());
                            updateRoutedConnectionPaths(project);
                        }
                    });
                    refreshProject();
                },
                [this](ice::ObjectId objectId, QRectF sceneRect) {
                    if (!project_)
                        return;
                    const auto rect = ice::Rect{static_cast<ice::Coordinate>(sceneRect.left()),
                                                static_cast<ice::Coordinate>(-sceneRect.bottom()),
                                                static_cast<ice::Coordinate>(sceneRect.width()),
                                                static_cast<ice::Coordinate>(sceneRect.height())};
                    project_->execute("resize layout object", [&](ice::Project &project) {
                        auto &cell = project.layoutCells.at(activeLayoutId_);
                        if (cell.ports.contains(objectId))
                            ice::resizePort(project, activeLayoutId_, objectId, rect);
                        else
                            ice::resizeRectangle(project, activeLayoutId_, objectId, rect);
                        updateRoutedConnectionPaths(project);
                    });
                    refreshProject();
                    QTimer::singleShot(0, this, [this, objectId] { selectLayoutShapeItem(objectId); });
                },
                [this](std::vector<ice::ObjectId> ids) {
                    if (!project_)
                        return;
                    project_->execute("delete layout objects", [&](ice::Project &project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end() || cellIt->second.library)
                            return;
                        std::set<ice::ObjectId> deleteIds(ids.begin(), ids.end());
                        for (const auto id : ids) {
                            const auto shape = cellIt->second.shapes.find(id);
                            if (shape == cellIt->second.shapes.end() || shape->second.textGroup == 0)
                                continue;
                            for (const auto &[memberId, member] : cellIt->second.shapes)
                                if (member.textGroup == shape->second.textGroup)
                                    deleteIds.insert(memberId);
                        }
                        for (const auto id : deleteIds) {
                            if (cellIt->second.plannedDevices.erase(id) != 0) {
                                std::erase_if(cellIt->second.plannedConnections, [&](const auto &entry) {
                                    return entry.second.from == id || entry.second.to == id;
                                });
                                continue;
                            }
                            if (cellIt->second.instances.erase(id) != 0) {
                                std::erase_if(cellIt->second.plannedConnections, [&](const auto &entry) {
                                    return entry.second.from == id || entry.second.to == id;
                                });
                                continue;
                            }
                            if (const auto port = cellIt->second.ports.find(id); port != cellIt->second.ports.end()) {
                                if (!port->second.locked) {
                                    std::erase_if(cellIt->second.plannedConnections, [&](const auto &entry) {
                                        return entry.second.from == id || entry.second.to == id;
                                    });
                                    cellIt->second.ports.erase(port);
                                }
                                continue;
                            }
                            const auto shape = cellIt->second.shapes.find(id);
                            if (shape != cellIt->second.shapes.end()) {
                                if (shape->second.routedConnection != 0) {
                                    if (const auto connection =
                                            cellIt->second.plannedConnections.find(shape->second.routedConnection);
                                        connection != cellIt->second.plannedConnections.end())
                                        connection->second.routed = false;
                                }
                                cellIt->second.shapes.erase(shape);
                                continue;
                            }
                            cellIt->second.instances.erase(id);
                        }
                        updateRoutedConnectionPaths(project);
                    });
                    refreshProject();
                },
                [this] {
                    if (project_)
                        project_->markDirty();
                });
            view = layoutCanvas_;
        }
        view->setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        view->setAlignment(Qt::AlignCenter);
        view->setAccessibleName(title);
        editorLayout->addWidget(view, 1);
        return editor;
    }

    QToolBar *createEditorToolbar(bool layoutEditor) {
        auto *toolbar =
            new QToolBar(layoutEditor ? QStringLiteral("Layout tools") : QStringLiteral("Schematic tools"), this);
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setAccessibleName(layoutEditor ? QStringLiteral("Layout toolbar")
                                                : QStringLiteral("Schematic toolbar"));

        auto addTool = [this, toolbar](const QString &iconName, const QString &name) {
            auto *action = toolbar->addAction(name);
            setLucideIcon(action, iconName);
            action->setToolTip(name);
            action->setStatusTip(name);
            commands_.append(action);
            return action;
        };
        auto addShortcut = [this](QAction *action, const QString &id, ice::ShortcutScope scope,
                                  const QKeySequence &sequence) {
            shortcuts_.registerAction(action, id, action->text(), scope, sequence);
        };

        auto *selectTool = addTool(QStringLiteral("mouse-pointer-2"), QStringLiteral("Select"));
        selectTool->setToolTip(QStringLiteral("Select (Space)"));
        selectTool->setStatusTip(QStringLiteral("Select (Space)"));
        addShortcut(selectTool, layoutEditor ? QStringLiteral("layout.select") : QStringLiteral("schematic.select"),
                    layoutEditor ? ice::ShortcutScope::layout : ice::ShortcutScope::schematic,
                    QKeySequence(Qt::Key_Space));
        if (layoutEditor) {
            selectTool->setCheckable(true);
            selectTool->setChecked(true);
            layoutSelectTool_ = selectTool;

            auto *moveTool = addTool(QStringLiteral("hand"), QStringLiteral("Move"));
            moveTool->setCheckable(true);
            moveTool->setToolTip(QStringLiteral("Move objects (M)"));
            moveTool->setStatusTip(QStringLiteral("Move objects (M)"));
            layoutMoveTool_ = moveTool;
            addShortcut(moveTool, QStringLiteral("layout.move"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("M")));
            connect(moveTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::move);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(QStringLiteral("Move: drag objects and instances"));
            });

            auto *placeSymbolTool = addTool(QStringLiteral("circuit-board"), QStringLiteral("Place Symbol"));
            placeSymbolTool->setCheckable(true);
            placeSymbolTool->setToolTip(QStringLiteral("Place device (P)"));
            placeSymbolTool->setStatusTip(QStringLiteral("Place device (P)"));
            layoutPlaceDeviceTool_ = placeSymbolTool;

            auto *rectangleTool = addTool(QStringLiteral("square"), QStringLiteral("Create Rectangle"));
            rectangleTool->setCheckable(true);
            rectangleTool->setToolTip(
                QStringLiteral("Create rectangle (R) · click to start, click to finish, or type width,height"));
            rectangleTool->setStatusTip(rectangleTool->toolTip());
            layoutRectangleTool_ = rectangleTool;
            addShortcut(rectangleTool, QStringLiteral("layout.rectangle"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("R")));

            auto *polygonTool = addTool(QStringLiteral("pentagon"), QStringLiteral("Create Polygon"));
            polygonTool->setCheckable(true);
            layoutPolygonTool_ = polygonTool;
            addShortcut(polygonTool, QStringLiteral("layout.polygon"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("G")));
            connect(polygonTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::polygon);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(
                    QStringLiteral("Polygon: click vertices · double-click/Escape/Space = finish"));
            });

            auto *pathTool = addTool(QStringLiteral("waypoints"), QStringLiteral("Draw Path"));
            pathTool->setCheckable(true);
            pathTool->setToolTip(QStringLiteral("Draw path (W)"));
            pathTool->setStatusTip(QStringLiteral("Draw path (W)"));
            layoutPathTool_ = pathTool;
            addShortcut(pathTool, QStringLiteral("layout.path"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("W")));

            auto *textTool = addTool(QStringLiteral("message-square"), QStringLiteral("Comment"));
            textTool->setCheckable(true);
            textTool->setToolTip(QStringLiteral("Comment / text label (Shift+C)"));
            textTool->setStatusTip(textTool->toolTip());
            layoutTextTool_ = textTool;
            addShortcut(textTool, QStringLiteral("layout.text"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("Shift+C")));

            // Keep the polygon tools together: the main toolbar button creates
            // a normal polygon, while the dropdown also offers text polygons.
            auto *textPolygonTool = new QAction(QStringLiteral("Draw Text Polygon"), toolbar);
            setLucideIcon(textPolygonTool, QStringLiteral("type"));
            textPolygonTool->setCheckable(true);
            textPolygonTool->setToolTip(QStringLiteral("Draw text as polygon (Shift+T)"));
            textPolygonTool->setStatusTip(QStringLiteral("Draw text as polygon (Shift+T)"));
            commands_.append(textPolygonTool);
            layoutTextPolygonTool_ = textPolygonTool;
            addShortcut(textPolygonTool, QStringLiteral("layout.text-polygon"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("Shift+T")));
            auto *polygonMenu = new QMenu(QStringLiteral("Polygon Tools"), toolbar);
            polygonMenu->addAction(polygonTool);
            polygonMenu->addAction(textPolygonTool);
            if (auto *polygonButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(polygonTool))) {
                polygonButton->setMenu(polygonMenu);
                polygonButton->setPopupMode(QToolButton::MenuButtonPopup);
                polygonButton->setToolTip(QStringLiteral("Create polygon (G) · dropdown for text polygon"));
                polygonButton->setStatusTip(QStringLiteral("Create polygon (G) · dropdown for text polygon"));
                const auto followDefaultAction = [polygonButton](QAction *action) {
                    polygonButton->setDefaultAction(action);
                    polygonButton->setToolTip(action->toolTip());
                    polygonButton->setStatusTip(action->statusTip());
                };
                connect(polygonTool, &QAction::triggered, polygonButton,
                        [followDefaultAction, polygonTool] { followDefaultAction(polygonTool); });
                connect(textPolygonTool, &QAction::triggered, polygonButton,
                        [followDefaultAction, textPolygonTool] { followDefaultAction(textPolygonTool); });
            }

            auto *pinTool = addTool(QStringLiteral("circle-dot"), QStringLiteral("Create Pin"));
            pinTool->setCheckable(true);
            pinTool->setToolTip(QStringLiteral("Create pin by dragging (N)"));
            pinTool->setStatusTip(QStringLiteral("Create pin by dragging (N)"));
            layoutPinTool_ = pinTool;
            addShortcut(pinTool, QStringLiteral("layout.pin"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("N")));
            connect(pinTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::pin);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(QStringLiteral("Pin: drag to size a pin on the active layer"));
            });
            connect(textTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::text);
                    layoutCanvas_->setFocus();
                }
                QString message = QStringLiteral("Comment: click to place a text annotation");
                if (project_ && activeLayoutLayerId_ != 0) {
                    if (const auto layer = project_->project().layers.find(activeLayoutLayerId_);
                        layer != project_->project().layers.end())
                        message += QStringLiteral(" on %1").arg(QString::fromStdString(layer->second.name));
                }
                message += QStringLiteral(" · height %1 nm").arg(layoutTextHeight_);
                statusBar()->showMessage(message);
            });
            connect(textPolygonTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::textPolygon);
                    layoutCanvas_->setFocus();
                }
                QString message = QStringLiteral("Text polygon: click to place filled polygon text");
                if (project_ && activeLayoutLayerId_ != 0) {
                    if (const auto layer = project_->project().layers.find(activeLayoutLayerId_);
                        layer != project_->project().layers.end())
                        message += QStringLiteral(" on %1").arg(QString::fromStdString(layer->second.name));
                }
                message += QStringLiteral(" · height %1 nm").arg(layoutTextHeight_);
                statusBar()->showMessage(message);
            });

            auto *rulerTool = addTool(QStringLiteral("ruler"), QStringLiteral("Ruler"));
            rulerTool->setCheckable(true);
            rulerTool->setToolTip(QStringLiteral("Measure distance (L)"));
            rulerTool->setStatusTip(QStringLiteral("Measure distance (L)"));
            layoutRulerTool_ = rulerTool;
            addShortcut(rulerTool, QStringLiteral("layout.ruler"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("L")));
            connect(rulerTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::ruler);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(QStringLiteral("Ruler: click point A, then point B"));
            });

            auto *toolGroup = new QActionGroup(toolbar);
            toolGroup->setExclusive(true);
            for (auto *action : {selectTool, moveTool, placeSymbolTool, rectangleTool, polygonTool, pathTool, rulerTool,
                                 textTool, textPolygonTool, pinTool})
                toolGroup->addAction(action);

            connect(selectTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_)
                    layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                clearLayoutDeviceSelection();
                refreshLayoutProperties(true);
            });
            connect(placeSymbolTool, &QAction::triggered, this, [this] {
                if (layoutLeftTabs_ && layoutDevicesPanel_)
                    layoutLeftTabs_->setCurrentWidget(layoutDevicesPanel_);
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::placeDevice);
                    layoutCanvas_->setFocus();
                }
                if (!selectedLayoutDevice_)
                    statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
                else
                    statusBar()->showMessage(QStringLiteral("Click in the layout to place %1")
                                                 .arg(QString::fromStdString(selectedLayoutDevice_->name)));
            });
            connect(rectangleTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::rectangle);
                    layoutCanvas_->setFocus();
                }
                refreshLayoutProperties(false);
                statusBar()->showMessage(QStringLiteral("Rectangle: click to start, move to size, click to finish · or "
                                                        "type width,height in the size field"));
            });
            connect(pathTool, &QAction::triggered, this, [this, pathTool] {
                if (!pathTool->isChecked() || !layoutCanvas_ || layoutCanvas_->isReadOnly())
                    return;
                updateLayoutPathConfig();
                if (!layoutCanvas_->pathAvailable()) {
                    statusBar()->showMessage(QStringLiteral("No layout layers are available for paths"));
                    if (layoutSelectTool_)
                        layoutSelectTool_->setChecked(true);
                    layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                    return;
                }
                layoutCanvas_->setTool(LayoutCanvas::Tool::path);
                layoutCanvas_->setFocus();
                if (layoutRightTabs_)
                    layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                refreshLayoutProperties(true);
                statusBar()->showMessage(
                    QStringLiteral("Path: %1 · %2 nm · Shift = right angles · wheel = layer · Ctrl+wheel = zoom · "
                                   "Escape/Space = finish")
                        .arg(QString::fromStdString(layoutCanvas_->pathLayerName()))
                        .arg(layoutCanvas_->pathWidth()));
            });
            toolbar->addSeparator();
            auto *hierarchyUpTool = addTool(QStringLiteral("arrow-up"), QStringLiteral("Hierarchy Go Up"));
            hierarchyUpTool->setToolTip(QStringLiteral("Go to the parent layout (Alt+Up)"));
            hierarchyUpTool->setStatusTip(hierarchyUpTool->toolTip());
            addShortcut(hierarchyUpTool, QStringLiteral("layout.hierarchy-up"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("Alt+Up")));
            connect(hierarchyUpTool, &QAction::triggered, this, [this] { openLayoutParentCell(); });
            auto *hierarchyDownTool = addTool(QStringLiteral("arrow-down"), QStringLiteral("Hierarchy Go Down"));
            hierarchyDownTool->setToolTip(QStringLiteral("Open the selected layout instance (Alt+Down)"));
            hierarchyDownTool->setStatusTip(hierarchyDownTool->toolTip());
            addShortcut(hierarchyDownTool, QStringLiteral("layout.hierarchy-down"), ice::ShortcutScope::layout,
                        QKeySequence(QStringLiteral("Alt+Down")));
            connect(hierarchyDownTool, &QAction::triggered, this, [this] { openSelectedLayoutCell(); });
            return toolbar;
        }

        selectTool->setCheckable(true);
        schematicSelectTool_ = selectTool;
        auto *placeSymbolTool = addTool(QStringLiteral("circuit-board"), QStringLiteral("Place Device"));
        placeSymbolTool->setCheckable(true);
        placeSymbolTool->setToolTip(QStringLiteral("Place device (P) · R resistor · X NMOS · I input · O output · "
                                                   "C capacitor · D diode · G ground · L inductor · S sine source"));
        placeSymbolTool->setStatusTip(placeSymbolTool->toolTip());
        schematicPlaceSymbolTool_ = placeSymbolTool;
        // Single-letter device shortcuts arm Place Device with one symbol;
        // they are registered so the Keyboard Shortcuts dialog can rebind them.
        const auto addDeviceShortcut = [this](const QString &id, const QString &label, const QString &device,
                                              const QString &key) {
            auto *action = new QAction(label, this);
            shortcuts_.registerAction(action, id, label, ice::ShortcutScope::schematic, QKeySequence(key));
            connect(action, &QAction::triggered, this, [this, device] { activateSchematicDevice(device); });
        };
        addDeviceShortcut(QStringLiteral("schematic.place-resistor"), QStringLiteral("Place Resistor"),
                          QStringLiteral("Resistor"), QStringLiteral("R"));
        addDeviceShortcut(QStringLiteral("schematic.place-nmos"), QStringLiteral("Place NMOS"), QStringLiteral("NMOS"),
                          QStringLiteral("X"));
        addDeviceShortcut(QStringLiteral("schematic.place-input"), QStringLiteral("Place Input"),
                          QStringLiteral("Input"), QStringLiteral("I"));
        addDeviceShortcut(QStringLiteral("schematic.place-output"), QStringLiteral("Place Output"),
                          QStringLiteral("Output"), QStringLiteral("O"));
        addDeviceShortcut(QStringLiteral("schematic.place-capacitor"), QStringLiteral("Place Capacitor"),
                          QStringLiteral("Capacitor"), QStringLiteral("C"));
        addDeviceShortcut(QStringLiteral("schematic.place-diode"), QStringLiteral("Place Diode"),
                          QStringLiteral("Diode"), QStringLiteral("D"));
        addDeviceShortcut(QStringLiteral("schematic.place-ground"), QStringLiteral("Place Ground"),
                          QStringLiteral("Ground"), QStringLiteral("G"));
        addDeviceShortcut(QStringLiteral("schematic.place-inductor"), QStringLiteral("Place Inductor"),
                          QStringLiteral("Inductor"), QStringLiteral("L"));
        addDeviceShortcut(QStringLiteral("schematic.place-sine-source"), QStringLiteral("Place Sine Voltage Source"),
                          QStringLiteral("Sine Voltage Source"), QStringLiteral("S"));
        auto *placeCommentTool = addTool(QStringLiteral("message-square"), QStringLiteral("Comment"));
        placeCommentTool->setToolTip(QStringLiteral("Comment (Shift+C)"));
        placeCommentTool->setStatusTip(QStringLiteral("Comment (Shift+C)"));
        placeCommentTool->setCheckable(true);
        schematicPlaceCommentTool_ = placeCommentTool;
        addShortcut(placeCommentTool, QStringLiteral("schematic.comment"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Shift+C")));
        auto *toolGroup = new QActionGroup(toolbar);
        toolGroup->setExclusive(true);
        for (auto *action : {selectTool, placeSymbolTool, placeCommentTool})
            toolGroup->addAction(action);
        selectTool->setChecked(true);
        const auto ensureToolChecked = [this, selectTool, placeSymbolTool, placeCommentTool] {
            if (!selectTool->isChecked() && !placeSymbolTool->isChecked() && !placeCommentTool->isChecked()) {
                selectTool->setChecked(true);
                if (schematicCanvas_) {
                    schematicCanvas_->setPlaceSymbolMode(false);
                    schematicCanvas_->setCommentMode(false);
                }
                if (schematicSymbolsPanel_)
                    schematicSymbolsPanel_->clearSymbolSelection();
                if (toolEscapeShortcut_)
                    toolEscapeShortcut_->setEnabled(false);
            }
        };
        for (auto *action : {selectTool, placeSymbolTool, placeCommentTool})
            connect(action, &QAction::toggled, this, [ensureToolChecked](bool) { ensureToolChecked(); });
        connect(selectTool, &QAction::triggered, this, [this, selectTool] {
            if (!selectTool->isChecked())
                return;
            if (schematicCanvas_) {
                schematicCanvas_->setPlaceSymbolMode(false);
                schematicCanvas_->setCommentMode(false);
            }
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clearSymbolSelection();
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(false);
        });
        connect(placeSymbolTool, &QAction::triggered, this, [this, placeSymbolTool, selectTool] {
            if (!placeSymbolTool->isChecked())
                return;
            // Re-using the tool keeps the device chosen last time, so placing
            // several copies never reopens the chooser.
            if (lastSchematicPlacementDevice_) {
                activateSchematicPlaceSymbolTool(*lastSchematicPlacementDevice_, true);
            } else if (const auto definition = chooseSchematicPlacementDevice()) {
                activateSchematicPlaceSymbolTool(*definition, true);
            } else {
                selectTool->setChecked(true);
                return;
            }
            if (schematicCanvas_)
                schematicCanvas_->setFocus();
        });
        connect(placeCommentTool, &QAction::triggered, this, [this, placeCommentTool] {
            if (!placeCommentTool->isChecked())
                return;
            if (schematicCanvas_) {
                schematicCanvas_->setPlaceSymbolMode(false);
                schematicCanvas_->setCommentMode(true);
                schematicCanvas_->setFocus();
            }
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clearSymbolSelection();
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(true);
            statusBar()->showMessage(QStringLiteral("Click in the schematic to place a comment"));
        });

        toolbar->addSeparator();
        auto *toggleGridTool = addTool(QStringLiteral("grid-3x3"), QStringLiteral("Toggle Grid"));
        toggleGridTool->setCheckable(true);
        toggleGridTool->setChecked(true);
        toggleGridTool->setToolTip(QStringLiteral("Toggle grid (Ctrl+G)"));
        toggleGridTool->setStatusTip(QStringLiteral("Toggle grid (Ctrl+G)"));
        addShortcut(toggleGridTool, QStringLiteral("schematic.toggle-grid"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Ctrl+G")));
        connect(toggleGridTool, &QAction::toggled, this, [this](bool checked) {
            if (schematicCanvas_)
                schematicCanvas_->setGridVisible(checked);
        });
        auto *snapTool = addTool(QStringLiteral("magnet"), QStringLiteral("Snap to Grid"));
        snapTool->setCheckable(true);
        snapTool->setChecked(true);
        snapTool->setToolTip(QStringLiteral("Snap to grid (Shift+G)"));
        snapTool->setStatusTip(QStringLiteral("Snap to grid (Shift+G)"));
        addShortcut(snapTool, QStringLiteral("schematic.snap"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Shift+G")));
        connect(snapTool, &QAction::toggled, this, [this](bool checked) {
            if (schematicCanvas_)
                schematicCanvas_->setSnapToGrid(checked);
        });
        auto *fitTool = addTool(QStringLiteral("scan"), QStringLiteral("Fit to View"));
        fitTool->setToolTip(QStringLiteral("Fit to view (F)"));
        fitTool->setStatusTip(QStringLiteral("Fit to view (F)"));
        addShortcut(fitTool, QStringLiteral("schematic.fit"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("F")));
        connect(fitTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->fitToContent();
        });

        toolbar->addSeparator();
        auto *rotateTool = addTool(QStringLiteral("rotate-cw"), QStringLiteral("Rotate"));
        rotateTool->setToolTip(QStringLiteral("Rotate selected devices (Shift+R)"));
        rotateTool->setStatusTip(QStringLiteral("Rotate selected devices (Shift+R)"));
        addShortcut(rotateTool, QStringLiteral("schematic.rotate"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Shift+R")));
        connect(rotateTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->rotateSelectedSymbols();
        });
        auto *flipTool = addTool(QStringLiteral("flip-horizontal-2"), QStringLiteral("Flip Horizontal"));
        flipTool->setToolTip(QStringLiteral("Flip selected devices horizontally (H)"));
        flipTool->setStatusTip(QStringLiteral("Flip selected devices horizontally (H)"));
        addShortcut(flipTool, QStringLiteral("schematic.flip-horizontal"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("H")));
        connect(flipTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->flipSelectedSymbols();
        });

        toolbar->addSeparator();
        auto *upTool = addTool(QStringLiteral("arrow-up"), QStringLiteral("Go Up the Hierarchy"));
        upTool->setToolTip(QStringLiteral("Go to the parent schematic (Alt+Up)"));
        upTool->setStatusTip(upTool->toolTip());
        addShortcut(upTool, QStringLiteral("schematic.hierarchy-up"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Alt+Up")));
        connect(upTool, &QAction::triggered, this, [this] { openSchematicParentCell(); });
        auto *downTool = addTool(QStringLiteral("arrow-down"), QStringLiteral("Go Down the Hierarchy"));
        downTool->setToolTip(QStringLiteral("Open the selected schematic instance (Alt+Down)"));
        downTool->setStatusTip(downTool->toolTip());
        addShortcut(downTool, QStringLiteral("schematic.hierarchy-down"), ice::ShortcutScope::schematic,
                    QKeySequence(QStringLiteral("Alt+Down")));
        connect(downTool, &QAction::triggered, this, [this] { openSelectedSchematicCell(); });

        toolbar->addSeparator();
        auto *wrapTool = addTool(QStringLiteral("package"), QStringLiteral("Wrap into Symbol"));
        connect(wrapTool, &QAction::triggered, this,
                [this] { statusBar()->showMessage(QStringLiteral("Wrapping symbols is not available yet")); });
        auto *deleteTool = addTool(QStringLiteral("trash"), QStringLiteral("Delete"));
        deleteTool->setToolTip(QStringLiteral("Delete selected devices (Del)"));
        deleteTool->setStatusTip(QStringLiteral("Delete selected devices (Del)"));
        addShortcut(deleteTool, QStringLiteral("schematic.delete"), ice::ShortcutScope::schematic,
                    QKeySequence::Delete);
        connect(deleteTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->deleteSelectedSymbols();
        });
        return toolbar;
    }

    QToolBar *createLayoutOptionsToolbar() {
        auto *toolbar = new QToolBar(QStringLiteral("Layout options"), this);
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setAccessibleName(QStringLiteral("Layout options toolbar"));

        auto addTool = [this, toolbar](const QString &iconName, const QString &name) {
            auto *action = toolbar->addAction(name);
            setLucideIcon(action, iconName);
            action->setToolTip(name);
            action->setStatusTip(name);
            commands_.append(action);
            return action;
        };
        auto addShortcut = [this](QAction *action, const QString &id, const QKeySequence &sequence) {
            shortcuts_.registerAction(action, id, action->text(), ice::ShortcutScope::layout, sequence);
        };

        auto *gridTool = addTool(QStringLiteral("grid-3x3"), QStringLiteral("Toggle Grid"));
        gridTool->setCheckable(true);
        gridTool->setChecked(layoutGridVisible_);
        gridTool->setToolTip(QStringLiteral("Toggle grid (Ctrl+G)"));
        gridTool->setStatusTip(QStringLiteral("Toggle grid (Ctrl+G)"));
        addShortcut(gridTool, QStringLiteral("layout.toggle-grid"), QKeySequence(QStringLiteral("Ctrl+G")));
        connect(gridTool, &QAction::toggled, this, [this](bool checked) {
            layoutGridVisible_ = checked;
            if (layoutCanvas_)
                layoutCanvas_->setGridVisible(checked);
        });
        auto *snapTool = addTool(QStringLiteral("magnet"), QStringLiteral("Snap to Grid"));
        snapTool->setCheckable(true);
        snapTool->setChecked(layoutSnapToGrid_);
        snapTool->setToolTip(QStringLiteral("Snap to grid (Shift+G)"));
        snapTool->setStatusTip(QStringLiteral("Snap to grid (Shift+G)"));
        addShortcut(snapTool, QStringLiteral("layout.snap"), QKeySequence(QStringLiteral("Shift+G")));
        connect(snapTool, &QAction::toggled, this, [this](bool enabled) {
            layoutSnapToGrid_ = enabled;
            if (layoutCanvas_)
                layoutCanvas_->setSnapToGrid(enabled);
        });
        auto *fitTool = addTool(QStringLiteral("scan"), QStringLiteral("Fit to View"));
        fitTool->setToolTip(QStringLiteral("Fit to view (F)"));
        fitTool->setStatusTip(QStringLiteral("Fit to view (F)"));
        addShortcut(fitTool, QStringLiteral("layout.fit"), QKeySequence(QStringLiteral("F")));
        connect(fitTool, &QAction::triggered, this, [this] {
            if (layoutCanvas_)
                layoutCanvas_->fitToContent();
        });

        toolbar->addSeparator();
        auto *rotateTool = addTool(QStringLiteral("rotate-cw"), QStringLiteral("Rotate"));
        rotateTool->setToolTip(QStringLiteral("Rotate selected device (Shift+R)"));
        rotateTool->setStatusTip(QStringLiteral("Rotate selected device (Shift+R)"));
        addShortcut(rotateTool, QStringLiteral("layout.rotate"), QKeySequence(QStringLiteral("Shift+R")));
        connect(rotateTool, &QAction::triggered, this, [this] { rotateSelectedLayoutFigures(); });
        auto *flipHorizontalTool = addTool(QStringLiteral("flip-horizontal-2"), QStringLiteral("Flip Horizontally"));
        flipHorizontalTool->setToolTip(QStringLiteral("Flip selected device horizontally (H)"));
        flipHorizontalTool->setStatusTip(QStringLiteral("Flip selected device horizontally (H)"));
        addShortcut(flipHorizontalTool, QStringLiteral("layout.flip-horizontal"), QKeySequence(QStringLiteral("H")));
        connect(flipHorizontalTool, &QAction::triggered, this, [this] {
            transformSelectedLayoutInstance(ice::Orientation::my, QStringLiteral("flip layout device"));
        });
        auto *flipVerticalTool = addTool(QStringLiteral("flip-vertical-2"), QStringLiteral("Flip Vertically"));
        flipVerticalTool->setToolTip(QStringLiteral("Flip selected device vertically (J)"));
        flipVerticalTool->setStatusTip(QStringLiteral("Flip selected device vertically (J)"));
        addShortcut(flipVerticalTool, QStringLiteral("layout.flip-vertical"), QKeySequence(QStringLiteral("J")));
        connect(flipVerticalTool, &QAction::triggered, this, [this] {
            transformSelectedLayoutInstance(ice::Orientation::mx, QStringLiteral("flip layout device"));
        });

        toolbar->addSeparator();
        auto *unionTool = addTool(QStringLiteral("squares-unite"), QStringLiteral("Union"));
        unionTool->setToolTip(
            QStringLiteral("Union selected rectangles and polygons (requires two shapes on one layer)"));
        unionTool->setStatusTip(unionTool->toolTip());
        layoutUnionTool_ = unionTool;
        connect(unionTool, &QAction::triggered, this, [this] { applyLayoutBoolean(ice::BooleanOperation::union_); });
        addShortcut(unionTool, QStringLiteral("layout.union"), QKeySequence(QStringLiteral("U")));

        auto *intersectionTool = addTool(QStringLiteral("squares-intersect"), QStringLiteral("Intersection"));
        intersectionTool->setToolTip(
            QStringLiteral("Intersect selected rectangles and polygons (requires two shapes on one layer)"));
        intersectionTool->setStatusTip(intersectionTool->toolTip());
        layoutIntersectionTool_ = intersectionTool;
        connect(intersectionTool, &QAction::triggered, this,
                [this] { applyLayoutBoolean(ice::BooleanOperation::intersection); });
        addShortcut(intersectionTool, QStringLiteral("layout.intersection"), QKeySequence(QStringLiteral("I")));

        auto *differenceTool = addTool(QStringLiteral("squares-subtract"), QStringLiteral("Difference"));
        differenceTool->setToolTip(
            QStringLiteral("Subtract newer shapes from the oldest selected shape (requires two shapes on one layer)"));
        differenceTool->setStatusTip(differenceTool->toolTip());
        layoutDifferenceTool_ = differenceTool;
        connect(differenceTool, &QAction::triggered, this,
                [this] { applyLayoutBoolean(ice::BooleanOperation::difference); });
        addShortcut(differenceTool, QStringLiteral("layout.difference"), QKeySequence(QStringLiteral("D")));

        auto *xorTool = addTool(QStringLiteral("squares-exclude"), QStringLiteral("Exclusive Or"));
        xorTool->setToolTip(
            QStringLiteral("Keep the non-overlapping area of selected shapes (requires two shapes on one layer)"));
        xorTool->setStatusTip(xorTool->toolTip());
        layoutXorTool_ = xorTool;
        connect(xorTool, &QAction::triggered, this, [this] { applyLayoutBoolean(ice::BooleanOperation::xor_); });
        addShortcut(xorTool, QStringLiteral("layout.xor"), QKeySequence(QStringLiteral("X")));
        refreshLayoutBooleanActions();

        toolbar->addSeparator();
        auto *deleteTool = addTool(QStringLiteral("trash"), QStringLiteral("Delete"));
        deleteTool->setToolTip(QStringLiteral("Delete selected objects (Del)"));
        deleteTool->setStatusTip(QStringLiteral("Delete selected objects (Del)"));
        layoutDeleteTool_ = deleteTool;
        addShortcut(deleteTool, QStringLiteral("layout.delete"), QKeySequence::Delete);
        connect(deleteTool, &QAction::triggered, this, [this] {
            if (layoutCanvas_)
                layoutCanvas_->deleteSelectedShapes();
        });
        return toolbar;
    }

    void createStatusBar() {
        auto *bar = new StatusBar(this);
        setStatusBar(bar);
        statusBar_ = bar;
        bar->setProjectName(QStringLiteral("No project open"));
        bar->showMessage(QStringLiteral("Ready — no project open"));
        bar->addPermanentWidget(new QLabel(QStringLiteral("DBU: 1 nm")));
        gridStatusLabel_ = new QLabel(QStringLiteral("Grid: 1"));
        bar->addPermanentWidget(gridStatusLabel_);
        selectionStatus_ = new QLabel(QStringLiteral("Selection: none"));
        bar->addPermanentWidget(selectionStatus_);
    }

    ProjectPanel *projectPanel_ = nullptr;
    ProjectPanel *schematicHierarchy_ = nullptr;
    ProjectPanel *layoutHierarchy_ = nullptr;
    LayersPanel *layersPanel_ = nullptr;
    QTabWidget *schematicCellTabs_ = nullptr;
    QTabWidget *layoutCellTabs_ = nullptr;
    QStackedWidget *schematicEditorStack_ = nullptr;
    QWidget *schematicEditorPage_ = nullptr;
    QStackedWidget *schematicViewStack_ = nullptr;
    QWidget *schematicCanvasPage_ = nullptr;
    QWidget *textSchematicEditorPage_ = nullptr;
    TextDocumentEditor *textSchematicEditor_ = nullptr;
    EmptySchematicPanel *schematicEmptyPanel_ = nullptr;
    QWidget *schematicToolbarColumn_ = nullptr;
    QStackedWidget *layoutEditorStack_ = nullptr;
    QWidget *layoutEditorPage_ = nullptr;
    QStackedWidget *layoutViewStack_ = nullptr;
    QWidget *layoutCanvasPage_ = nullptr;
    QWidget *proceduralEditorPage_ = nullptr;
    ProceduralLayoutEditor *proceduralEditor_ = nullptr;
    EmptyLayoutPanel *layoutEmptyPanel_ = nullptr;
    QWidget *layoutToolbarColumn_ = nullptr;
    QWidget *layoutOptionsColumn_ = nullptr;
    ice::ObjectId activeSchematicId_ = 0;
    // Per-cell, per-net branch routes. Unchanged branches are reused so editing
    // one net does not perturb unrelated schematic wiring.
    std::map<ice::ObjectId, std::map<ice::ObjectId, SchematicNetRoute>> schematicRouteCache_;
    // Visible SVG ink bounds keyed by artwork source. Measuring an SVG requires
    // an offscreen render and alpha scan, so cache it for the session.
    std::map<std::string, QRectF> schematicArtworkBoundsCache_;
    ice::ObjectId activeLayoutId_ = 0;
    ice::ObjectId activeLayoutLayerId_ = 0;
    ice::ObjectId activeProceduralLayoutId_ = 0;
    ice::ObjectId activeLayoutPathId_ = 0;
    ice::ObjectId activeLayoutPathCellId_ = 0;
    int activeLayoutPathWidth_ = 0;
    std::set<ice::ObjectId> openSchematicTabs_;
    std::set<ice::ObjectId> openLayoutTabs_;
    std::set<ice::ObjectId> knownSchematicCells_;
    std::set<ice::ObjectId> knownLayoutCells_;
    std::set<ice::ObjectId> knownProceduralLayouts_;
    std::map<ice::ObjectId, std::string> savedSchematicSignatures_;
    std::map<ice::ObjectId, std::string> savedLayoutSignatures_;
    std::map<ice::ObjectId, std::string> savedProceduralSignatures_;
    std::set<ice::ObjectId> conflictedSchematicFiles_;
    std::set<ice::ObjectId> conflictedLayoutFiles_;
    std::set<ice::ObjectId> conflictedProceduralFiles_;
    std::map<ice::ObjectId, ice::SchematicCell> externalSchematicFiles_;
    std::map<ice::ObjectId, ice::LayoutCell> externalLayoutFiles_;
    std::map<ice::ObjectId, ice::ProceduralLayout> externalProceduralFiles_;
    std::unique_ptr<ice::ProjectSession> project_;
    std::shared_ptr<std::set<ice::ObjectId>> flattenedLayouts_ = std::make_shared<std::set<ice::ObjectId>>();
    QString projectPath_;
    QFileSystemWatcher *projectWatcher_ = nullptr;
    QFileSystemWatcher *simulationRunWatcher_ = nullptr;
    QTimer *simulationRunWatcherTimer_ = nullptr;
    QTimer *projectWatchDebounceTimer_ = nullptr;
    QTimer *projectWatcherResumeTimer_ = nullptr;
    bool suppressProjectWatcher_ = false;
    bool projectDiskChangePending_ = false;
    QStackedWidget *workspaceStack_ = nullptr;
    StartupView *startupView_ = nullptr;
    QDockWidget *activityDock_ = nullptr;
    ActivityPanel *activityPanel_ = nullptr;
    bool activityDockSizeInitialized_ = false;
    QProcess *drcProcess_ = nullptr;
    std::unique_ptr<QTemporaryDir> drcTempDir_;
    QString drcReportPath_;
    QString drcLog_;
    QTabWidget *documents_ = nullptr;
    QWidget *schematicPage_ = nullptr;
    QWidget *layoutPage_ = nullptr;
    SchematicCanvas *schematicCanvas_ = nullptr;
    LayoutCanvas *layoutCanvas_ = nullptr;
    bool layoutViewInitialized_ = false;
    QPlainTextEdit *simulationOutput_ = nullptr;
    QTabWidget *simulationTabs_ = nullptr;
    QPlainTextEdit *simulationNetlist_ = nullptr;
    QWidget *simulationTestbenchPage_ = nullptr;
    TextDocumentEditor *simulationTestbenchEditor_ = nullptr;
    QPushButton *simulationTestbenchRunButton_ = nullptr;
    int simulationTestbenchTabIndex_ = -1;
    QWidget *simulationPage_ = nullptr;
    QWidget *simulationEditorPage_ = nullptr;
    QWidget *simulationGraphsPage_ = nullptr;
    QStackedWidget *simulationRightStack_ = nullptr;
    QWidget *reportsView_ = nullptr;
    QSplitter *scriptViewSplitter_ = nullptr;
    QTabWidget *scriptGraphTabs_ = nullptr;
    QLabel *reportTitleLabel_ = nullptr;
    QPlainTextEdit *reportCodeEditor_ = nullptr;
    PythonSyntaxHighlighter *reportHighlighter_ = nullptr;
    QLabel *reportStatusLabel_ = nullptr;
    QPushButton *reportSaveButton_ = nullptr;
    QPushButton *reportRunButton_ = nullptr;
    QPushButton *reportOpenButton_ = nullptr;
    QTimer *reportSaveTimer_ = nullptr;
    QString loadedCachedRunPath_;
    ice::ObjectId activeReportId_ = 0;
    bool selectingReport_ = false;
    bool loadingReport_ = false;
    bool reportDirty_ = false;
    QComboBox *schematicSelector_ = nullptr;
    QComboBox *simulatorSelector_ = nullptr;
    QComboBox *analysisSelector_ = nullptr;
    QDoubleSpinBox *stopTime_ = nullptr;
    QDoubleSpinBox *stepTime_ = nullptr;
    QLineEdit *analysisControl_ = nullptr;
    QLineEdit *signalsCapture_ = nullptr;
    QLineEdit *additionalDirectives_ = nullptr;
    QCheckBox *useInitialConditions_ = nullptr;
    QPushButton *signalSelectButton_ = nullptr;
    QPushButton *simulationBuildNetlistButton_ = nullptr;
    QPushButton *simulationRunButton_ = nullptr;
    QPushButton *simulationAddGraphButton_ = nullptr;
    QPushButton *simulationDeleteGraphButton_ = nullptr;
    QPushButton *simulationEditGraphButton_ = nullptr;
    QPushButton *simulationGridButton_ = nullptr;
    QPushButton *simulationLoadVcdButton_ = nullptr;
    QString currentSimulationNetlist_;
    QString builtSimulationNetlist_;
    ice::ObjectId builtSimulationNetlistSchematicId_ = 0;
    SimulationPlot *simulationPlot_ = nullptr;
    SimulationPanel *simulationPanel_ = nullptr;
    QTabWidget *graphTabs_ = nullptr;
    bool graphGridVisible_ = true;
    ice::ObjectId activeSimulationId_ = 0;
    PropertiesPanel *schematicPropertiesPanel_ = nullptr;
    PropertiesPanel *layoutPropertiesPanel_ = nullptr;
    QTabWidget *layoutRightTabs_ = nullptr;
    LayoutDevicesPanel *layoutDevicesPanel_ = nullptr;
    QTabWidget *layoutLeftTabs_ = nullptr;
    const ice::LayoutDeviceCell *selectedLayoutDevice_ = nullptr;
    SymbolsPanel *schematicSymbolsPanel_ = nullptr;
    QTabWidget *schematicLeftTabs_ = nullptr;
    SchematicNetlistPanel *schematicNetlistPanel_ = nullptr;
    HistoryPanel *schematicHistoryPanel_ = nullptr;
    HistoryPanel *layoutHistoryPanel_ = nullptr;
    std::vector<SectionHistoryEntry> schematicHistory_;
    std::vector<SectionHistoryEntry> layoutHistory_;
    std::size_t schematicHistoryPosition_ = 0;
    std::size_t layoutHistoryPosition_ = 0;
    std::size_t syncedGlobalHistoryPosition_ = 0;
    std::string schematicSymbolsPdk_;
    bool schematicSymbolsLoaded_ = false;
    std::vector<SymbolDefinition> schematicBaseSymbols_;
    std::optional<SymbolDefinition> selectedSchematicSymbol_;
    // Kept after the tool is left so re-activating Place Device can reuse it.
    std::optional<SymbolDefinition> lastSchematicPlacementDevice_;
    StatusBar *statusBar_ = nullptr;
    QLabel *selectionStatus_ = nullptr;
    QLabel *gridStatusLabel_ = nullptr;
    QAction *schematicSelectTool_ = nullptr;
    QAction *schematicPlaceSymbolTool_ = nullptr;
    QAction *schematicPlaceCommentTool_ = nullptr;
    QAction *layoutSelectTool_ = nullptr;
    QAction *layoutMoveTool_ = nullptr;
    QAction *layoutPlaceDeviceTool_ = nullptr;
    QAction *layoutRectangleTool_ = nullptr;
    QAction *layoutPathTool_ = nullptr;
    QAction *layoutPolygonTool_ = nullptr;
    QAction *layoutTextTool_ = nullptr;
    QAction *layoutTextPolygonTool_ = nullptr;
    QAction *layoutPinTool_ = nullptr;
    QAction *layoutRulerTool_ = nullptr;
    QAction *layoutUnionTool_ = nullptr;
    QAction *layoutIntersectionTool_ = nullptr;
    QAction *layoutDifferenceTool_ = nullptr;
    QAction *layoutXorTool_ = nullptr;
    bool layoutGridVisible_ = true;
    bool layoutSnapToGrid_ = true;
    QAction *layoutDeleteTool_ = nullptr;
    int layoutPathWidth_ = 200;
    int layoutTextHeight_ = 1000;
    QShortcut *toolEscapeShortcut_ = nullptr;
    ice::AppearanceMode appearanceMode_ = ice::AppearanceMode::System;
    ice::ShortcutRegistry shortcuts_;
    QList<QAction *> commands_;
};

} // namespace

class AppWindowAdapter final : public AppWindow {
  public:
    explicit AppWindowAdapter(std::unique_ptr<DocumentWindow> window) : window_(std::move(window)) {}

    bool setProject(ice::Project project) override { return window_->setProject(std::move(project)); }
    void setProjectPath(const QString &path) override { window_->setProjectPath(path); }
    void show() override { window_->show(); }
    void resize(int width, int height) override { window_->resize(width, height); }
    void showMessage(const QString &message) override { window_->statusBar()->showMessage(message); }

  private:
    std::unique_ptr<DocumentWindow> window_;
};

std::unique_ptr<AppWindow> createAppWindow(ice::AppearanceMode appearance) {
    return std::make_unique<AppWindowAdapter>(std::make_unique<DocumentWindow>(appearance));
}
