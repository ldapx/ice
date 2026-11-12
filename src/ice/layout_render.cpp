#include "layout_render.hpp"

#include "ui_colors.hpp"

#include <QBrush>
#include <QFont>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <utility>

namespace ice {

QString layoutPreviewLayerKey(const LayoutPreviewLayer &layer) {
    if (!layer.name.isEmpty())
        return layer.name;
    if (layer.id != 0)
        return QStringLiteral("id:%1").arg(layer.id);
    return QStringLiteral("layer:%1/%2").arg(layer.number).arg(layer.datatype);
}

namespace {

struct DisplayTransform {
    qreal scale = 1.0;
    QPointF offset;

    QPointF map(QPointF point) const {
        return QPointF(point.x() * scale + offset.x(), -point.y() * scale + offset.y());
    }

    QRectF map(const QRectF &rectangle) const {
        const auto first = map(QPointF(rectangle.left(), rectangle.top() + rectangle.height()));
        const auto second = map(QPointF(rectangle.left() + rectangle.width(), rectangle.top()));
        return QRectF(first, second).normalized();
    }
};

Point applyTransformChain(Point point, const std::vector<Transform> &chain) {
    for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator)
        point = iterator->apply(point);
    return point;
}

void includeRect(QRectF &bounds, bool &valid, const QRectF &rectangle) {
    if (!rectangle.isValid())
        return;
    bounds = valid ? bounds.united(rectangle) : rectangle;
    valid = true;
}

LayoutPreviewLayer layerFor(const Project &project, ObjectId id) {
    LayoutPreviewLayer layer;
    layer.id = id;
    if (const auto found = project.layers.find(id); found != project.layers.end()) {
        layer.number = found->second.number;
        layer.datatype = found->second.datatype;
        layer.name = QString::fromStdString(found->second.name);
    }
    return layer;
}

LayoutPreviewLayer layerIdOnly(ObjectId id) {
    LayoutPreviewLayer layer;
    layer.id = id;
    return layer;
}

LayerStyle styleFor(const LayoutPreviewLayer &layer, const std::map<ObjectId, LayerStyle> &styles,
                    const std::map<std::pair<int, int>, LayerStyle> &deviceStyles) {
    if (layer.id != 0) {
        if (const auto found = styles.find(layer.id); found != styles.end())
            return found->second;
    }
    if (const auto found = deviceStyles.find({layer.number, layer.datatype}); found != deviceStyles.end())
        return found->second;
    return layerStyleForNumber(layer.number);
}

QPolygonF pathPolygon(QPointF start, QPointF end, qreal width) {
    const qreal dx = end.x() - start.x();
    const qreal dy = end.y() - start.y();
    const qreal length = std::hypot(dx, dy);
    if (length < 1.0e-9 || width <= 0.0)
        return {};
    const qreal ux = dx / length;
    const qreal uy = dy / length;
    const qreal nx = -uy;
    const qreal ny = ux;
    const qreal half = width * 0.5;
    const QPointF before(start.x() - ux * half, start.y() - uy * half);
    const QPointF after(end.x() + ux * half, end.y() + uy * half);
    QPolygonF polygon;
    polygon << before + QPointF(nx * half, ny * half) << after + QPointF(nx * half, ny * half)
            << after - QPointF(nx * half, ny * half) << before - QPointF(nx * half, ny * half);
    return polygon;
}

QBrush brushFor(const LayerStyle &style, qreal alpha, bool patterns) {
    if (style.pattern == Qt::NoBrush)
        return QBrush(Qt::NoBrush);
    QColor color = style.color;
    color.setAlphaF(std::clamp(alpha / 255.0, 0.0, 1.0));
    return QBrush(color, patterns ? style.pattern : Qt::SolidPattern);
}

QPen penFor(const LayerStyle &style, qreal width) {
    QPen pen(style.color, width);
    pen.setCosmetic(true);
    return pen;
}

void paintPolygon(QPainter &painter, const QPolygonF &polygon, const LayerStyle &style,
                  const LayoutRenderer::PaintOptions &options) {
    if (polygon.size() < 3)
        return;
    painter.setPen(penFor(style, options.outlineWidth));
    painter.setBrush(brushFor(style, options.shapeAlpha, options.showPatterns));
    painter.drawPolygon(polygon);
}

void drawHaloText(QPainter &painter, const QRectF &rectangle, int alignment, const QString &text, const QColor &color) {
    auto halo = uiColor(UiColor::Background2);
    halo.setAlpha(225);
    QPen haloPen(halo, 2.0);
    haloPen.setCosmetic(true);
    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(haloPen);
    static const QPointF offsets[] = {{-1.5, 0.0},  {1.5, 0.0},  {0.0, -1.5}, {0.0, 1.5},
                                      {-1.0, -1.0}, {1.0, -1.0}, {-1.0, 1.0}, {1.0, 1.0}};
    for (const auto &offset : offsets)
        painter.drawText(rectangle.translated(offset), alignment, text);
    painter.setPen(color);
    painter.drawText(rectangle, alignment, text);
    painter.restore();
}

void paintText(QPainter &painter, const QRectF &rectangle, const QString &text, const LayerStyle &style,
               const LayoutRenderer::PaintOptions &options) {
    if (!options.showText || text.isEmpty() || !rectangle.isValid())
        return;
    QFont font = painter.font();
    font.setPixelSize(std::max(1, static_cast<int>(std::round(rectangle.height() * 0.85))));
    painter.setFont(font);
    drawHaloText(painter, rectangle, Qt::AlignLeft | Qt::AlignTop, text, style.color);
}

void paintModel(QPainter &painter, const LayoutPreviewModel &model, const LayoutRenderer::LayerStyleMap &styles,
                const LayoutRenderer::DeviceStyleMap &deviceStyles, const DisplayTransform &transform,
                const LayoutRenderer::PaintOptions &options) {
    const auto accent = uiColor(UiColor::Accent4);
    std::map<int, QString> netTypes;
    for (const auto &net : model.nets)
        netTypes[net.id] = net.type.toLower();
    const auto layerVisible = [&](const LayoutPreviewLayer &layer) {
        return options.hiddenLayers.find(layoutPreviewLayerKey(layer)) == options.hiddenLayers.end();
    };
    const auto netVisible = [&](int net_id, bool special) {
        if (special && !options.showSpecialNets)
            return false;
        const auto type = netTypes.find(net_id);
        if (type == netTypes.end())
            return true;
        return options.hiddenNetTypes.find(type->second) == options.hiddenNetTypes.end();
    };

    for (const auto &connection : model.plannedConnections) {
        QPen pen(accent);
        pen.setCosmetic(true);
        pen.setWidthF(1.5);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(transform.map(connection.line.p1()), transform.map(connection.line.p2()));
    }

    if (options.showRows) {
        QPen pen(QColor(120, 120, 120, 150), 0.8);
        pen.setCosmetic(true);
        pen.setStyle(Qt::DotLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (const auto &row : model.rows)
            painter.drawRect(transform.map(row.rectangle));
    }
    if (options.showBlockages) {
        QPen pen(QColor(180, 60, 60, 190), 0.8);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(QColor(180, 60, 60, 45));
        for (const auto &blockage : model.blockages)
            painter.drawRect(transform.map(blockage.rectangle));
    }
    if (options.showGuides) {
        for (const auto &guide : model.guides) {
            QPen pen(guide.congested ? QColor(220, 38, 38, 220) : QColor(14, 165, 233, 220), 1.0);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(transform.map(guide.rectangle));
        }
    }

    for (const auto &shape : model.shapes) {
        if (!layerVisible(shape.layer) || !netVisible(shape.netId, shape.special))
            continue;
        const auto style = styleFor(shape.layer, styles, deviceStyles);
        if (shape.kind == LayoutPreviewShape::Kind::text)
            continue;
        if (shape.kind == LayoutPreviewShape::Kind::path) {
            if (!options.showPaths || shape.centerline.size() < 2)
                continue;
            for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
                const auto layerId = index < shape.segmentLayers.size() ? shape.segmentLayers[index] : shape.layer.id;
                const auto segmentStyle =
                    layerId == shape.layer.id ? style : styleFor(layerIdOnly(layerId), styles, deviceStyles);
                const auto polygon =
                    pathPolygon(transform.map(shape.centerline[index]), transform.map(shape.centerline[index + 1]),
                                shape.width * transform.scale);
                paintPolygon(painter, polygon, segmentStyle, options);
            }
            for (std::size_t index = 0; index < shape.centerline.size(); ++index) {
                if (index >= shape.pointVias.size())
                    continue;
                for (const auto layerId : shape.pointVias[index]) {
                    const auto viaStyle = styleFor(layerIdOnly(layerId), styles, deviceStyles);
                    const auto center = transform.map(shape.centerline[index]);
                    const auto half = shape.width * transform.scale * 0.5;
                    const QRectF via(center.x() - half, center.y() - half, half * 2.0, half * 2.0);
                    painter.setPen(penFor(viaStyle, options.outlineWidth));
                    painter.setBrush(brushFor(viaStyle, options.shapeAlpha, options.showPatterns));
                    painter.drawRect(via);
                }
            }
            continue;
        }
        QPolygonF polygon;
        polygon.reserve(shape.polygon.size());
        for (const auto &point : shape.polygon)
            polygon << transform.map(point);
        paintPolygon(painter, polygon, style, options);
    }

    if (options.showPlannedDevices) {
        for (const auto &device : model.plannedDevices) {
            if (!device.category.isEmpty() &&
                options.hiddenInstanceCategories.find(device.category) != options.hiddenInstanceCategories.end())
                continue;
            const auto rectangle = transform.map(device.rectangle);
            QColor fill = accent;
            fill.setAlphaF(std::clamp(options.shapeAlpha / 255.0, 0.0, 1.0) * 0.5);
            painter.setPen(penFor(LayerStyle{accent, Qt::SolidPattern}, options.outlineWidth));
            painter.setBrush(fill);
            painter.drawRoundedRect(rectangle, 2.0, 2.0);
            if (options.showLabels && rectangle.height() > 10.0) {
                QFont font = painter.font();
                font.setPixelSize(std::max(6, static_cast<int>(rectangle.height() * 0.2)));
                painter.setFont(font);
                painter.setPen(accent);
                painter.drawText(rectangle.adjusted(3, 2, -3, -2), Qt::AlignCenter, device.name);
            }
        }
    }

    // Text labels stay above geometry and below pin overlays, matching the
    // canvas z-order.
    for (const auto &shape : model.shapes) {
        if (shape.kind != LayoutPreviewShape::Kind::text || !layerVisible(shape.layer) ||
            !netVisible(shape.netId, shape.special))
            continue;
        const auto style = styleFor(shape.layer, styles, deviceStyles);
        paintText(painter, transform.map(shape.textRectangle), shape.text, style, options);
    }

    if (options.showPorts) {
        for (const auto &port : model.ports) {
            if (!layerVisible(port.layer) || !netVisible(port.netId, false))
                continue;
            const auto style = styleFor(port.layer, styles, deviceStyles);
            const auto rectangle = transform.map(port.rectangle);
            QPen pen = penFor(style, options.outlineWidth);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            QColor fill = style.color;
            fill.setAlphaF(std::clamp(options.portAlpha / 255.0, 0.0, 1.0));
            painter.setBrush(fill);
            painter.drawRect(rectangle);
            if (options.showLabels && rectangle.height() > 8.0 && !port.label.isEmpty()) {
                QFont font = painter.font();
                font.setPixelSize(std::max(5, static_cast<int>(rectangle.height() * 0.35)));
                painter.setFont(font);
                drawHaloText(painter, rectangle.adjusted(2, 1, -2, -1), Qt::AlignCenter, port.label, style.color);
            }
        }
    }
}

DisplayTransform fittedTransform(const QRectF &bounds, const QRectF &target) {
    DisplayTransform transform;
    if (!bounds.isValid() || bounds.width() <= 0.0 || bounds.height() <= 0.0 || !target.isValid())
        return transform;
    transform.scale = std::min(target.width() / bounds.width(), target.height() / bounds.height());
    const auto drawWidth = bounds.width() * transform.scale;
    const auto drawHeight = bounds.height() * transform.scale;
    const auto left = target.left() + (target.width() - drawWidth) * 0.5;
    const auto top = target.top() + (target.height() - drawHeight) * 0.5;
    transform.offset =
        QPointF(left - bounds.left() * transform.scale, top + (bounds.top() + bounds.height()) * transform.scale);
    return transform;
}

DisplayTransform centeredTransform(const QRectF &bounds, QPointF center) {
    DisplayTransform transform;
    if (!bounds.isValid())
        return transform;
    const auto centerX = bounds.left() + bounds.width() * 0.5;
    const auto centerY = bounds.top() + bounds.height() * 0.5;
    transform.offset = QPointF(center.x() - centerX, center.y() + centerY);
    return transform;
}

} // namespace

bool LayoutPreviewModel::empty() const {
    return shapes.empty() && ports.empty() && plannedDevices.empty() && plannedConnections.empty();
}

LayoutPreviewModel buildLayoutDevicePreviewModel(const LayoutDeviceCell &device) {
    LayoutPreviewModel model;
    for (const auto &shape : device.shapes) {
        if (shape.polygon.size() < 3)
            continue;
        LayoutPreviewShape preview;
        preview.layer.number = shape.layer;
        preview.layer.datatype = shape.datatype;
        preview.kind = LayoutPreviewShape::Kind::polygon;
        for (const auto &point : shape.polygon)
            preview.polygon << QPointF(point.x, point.y);
        model.shapes.push_back(std::move(preview));
    }
    for (const auto &port : device.ports) {
        LayoutPreviewPort preview;
        preview.layer.number = port.layer;
        preview.layer.datatype = port.datatype;
        preview.rectangle = QRectF(port.rectangle.x, port.rectangle.y, port.rectangle.width, port.rectangle.height);
        preview.label = QString::fromStdString(port.label.empty() ? port.name : port.label);
        model.ports.push_back(std::move(preview));
    }
    QRectF bounds;
    bool valid = false;
    for (const auto &shape : model.shapes)
        includeRect(bounds, valid, shape.polygon.boundingRect());
    for (const auto &port : model.ports)
        includeRect(bounds, valid, port.rectangle);
    if (device.bounds.valid())
        bounds = QRectF(device.bounds.x, device.bounds.y, device.bounds.width, device.bounds.height);
    model.bounds = valid ? bounds : QRectF{};
    return model;
}

LayoutPreviewModel buildLayoutCellPreviewModel(const Project &project, const LayoutCell &cell, bool includeInstances,
                                               bool includePorts, bool includePlannedDevices,
                                               const std::vector<Transform> &rootTransform) {
    LayoutPreviewModel model;
    QRectF bounds;
    bool valid = false;
    std::set<ObjectId> ancestors{cell.id};

    std::function<void(const LayoutCell &, const std::vector<Transform> &, bool)> collect;
    collect = [&](const LayoutCell &current, const std::vector<Transform> &chain, bool root) {
        for (const auto &[_, shape] : current.shapes) {
            const auto layer = layerFor(project, shape.layer);
            if (shape.isText()) {
                LayoutPreviewShape preview;
                preview.layer = layer;
                preview.kind = LayoutPreviewShape::Kind::text;
                preview.text = QString::fromStdString(shape.text);
                const auto first = applyTransformChain({shape.rectangle.x, shape.rectangle.y}, chain);
                const auto second = applyTransformChain(
                    {shape.rectangle.x + shape.rectangle.width, shape.rectangle.y + shape.rectangle.height}, chain);
                preview.textRectangle = QRectF(QPointF(std::min(first.x, second.x), std::min(first.y, second.y)),
                                               QPointF(std::max(first.x, second.x), std::max(first.y, second.y)))
                                            .normalized();
                includeRect(bounds, valid, preview.textRectangle);
                model.shapes.push_back(std::move(preview));
                continue;
            }
            if (shape.isPath()) {
                if (shape.centerline.size() < 2)
                    continue;
                LayoutPreviewShape preview;
                preview.layer = layer;
                preview.kind = LayoutPreviewShape::Kind::path;
                preview.width = shape.width;
                preview.segmentLayers = shape.segmentLayers;
                preview.pointVias = shape.pointVias;
                for (const auto &point : shape.centerline) {
                    const auto mapped = applyTransformChain(
                        {static_cast<Coordinate>(point.x), static_cast<Coordinate>(point.y)}, chain);
                    preview.centerline.push_back(QPointF(mapped.x, mapped.y));
                }
                if (!preview.centerline.empty()) {
                    qreal minX = preview.centerline.front().x();
                    qreal minY = preview.centerline.front().y();
                    qreal maxX = minX;
                    qreal maxY = minY;
                    for (const auto &point : preview.centerline) {
                        minX = std::min(minX, point.x());
                        minY = std::min(minY, point.y());
                        maxX = std::max(maxX, point.x());
                        maxY = std::max(maxY, point.y());
                    }
                    const auto margin = std::max<qreal>(0.0, shape.width * 0.5);
                    includeRect(
                        bounds, valid,
                        QRectF(minX - margin, minY - margin, maxX - minX + 2.0 * margin, maxY - minY + 2.0 * margin));
                }
                model.shapes.push_back(std::move(preview));
                continue;
            }
            LayoutPreviewShape preview;
            preview.layer = layer;
            preview.kind = LayoutPreviewShape::Kind::polygon;
            if (shape.polygon.size() >= 3) {
                for (const auto &point : shape.polygon) {
                    const auto mapped = applyTransformChain({point.x, point.y}, chain);
                    preview.polygon << QPointF(mapped.x, mapped.y);
                }
            } else {
                const auto &rectangle = shape.rectangle;
                const std::array<Point, 4> corners = {
                    Point{rectangle.x, rectangle.y},
                    Point{rectangle.x + rectangle.width, rectangle.y},
                    Point{rectangle.x + rectangle.width, rectangle.y + rectangle.height},
                    Point{rectangle.x, rectangle.y + rectangle.height},
                };
                for (const auto &corner : corners) {
                    const auto mapped = applyTransformChain(corner, chain);
                    preview.polygon << QPointF(mapped.x, mapped.y);
                }
            }
            includeRect(bounds, valid, preview.polygon.boundingRect());
            model.shapes.push_back(std::move(preview));
        }

        if (includePorts) {
            for (const auto &[_, port] : current.ports) {
                if (!port.visible)
                    continue;
                LayoutPreviewPort preview;
                preview.layer = layerFor(project, port.layer);
                const std::array<Point, 4> corners = {
                    Point{port.rectangle.x, port.rectangle.y},
                    Point{port.rectangle.x + port.rectangle.width, port.rectangle.y},
                    Point{port.rectangle.x + port.rectangle.width, port.rectangle.y + port.rectangle.height},
                    Point{port.rectangle.x, port.rectangle.y + port.rectangle.height},
                };
                QPolygonF polygon;
                for (const auto &corner : corners) {
                    const auto mapped = applyTransformChain(corner, chain);
                    polygon << QPointF(mapped.x, mapped.y);
                }
                preview.rectangle = polygon.boundingRect();
                preview.label = QString::fromStdString(port.label.empty() ? port.name : port.label);
                includeRect(bounds, valid, preview.rectangle);
                model.ports.push_back(std::move(preview));
            }
        }

        if (root && includePlannedDevices) {
            for (const auto &[_, device] : current.plannedDevices) {
                LayoutPreviewPlannedDevice preview;
                preview.rectangle = QRectF(device.x, device.y, device.width, device.height);
                preview.name = QString::fromStdString(device.name);
                includeRect(bounds, valid, preview.rectangle);
                model.plannedDevices.push_back(std::move(preview));
            }

            const auto endpointPoint = [&](ObjectId endpoint, ObjectId port) -> std::optional<Point> {
                if (const auto device = current.plannedDevices.find(endpoint); device != current.plannedDevices.end())
                    return Point{device->second.x + device->second.width / 2,
                                 device->second.y + device->second.height / 2};
                if (const auto pin = current.ports.find(endpoint); pin != current.ports.end())
                    return Point{pin->second.rectangle.x + pin->second.rectangle.width / 2,
                                 pin->second.rectangle.y + pin->second.rectangle.height / 2};
                const auto instance = current.instances.find(endpoint);
                if (instance == current.instances.end())
                    return std::nullopt;
                const auto child = project.layoutCells.find(instance->second.cell);
                if (child == project.layoutCells.end())
                    return std::nullopt;
                const Transform transform{instance->second.x, instance->second.y, instance->second.orientation};
                if (port != 0) {
                    const auto childPort = child->second.ports.find(port);
                    if (childPort != child->second.ports.end()) {
                        const auto center =
                            transform.apply({childPort->second.rectangle.x + childPort->second.rectangle.width / 2,
                                             childPort->second.rectangle.y + childPort->second.rectangle.height / 2});
                        return Point{center.x, center.y};
                    }
                }

                bool havePoint = false;
                Point minimum;
                Point maximum;
                const auto add = [&](Point point) {
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
                for (const auto &[_, childPort] : child->second.ports) {
                    add({childPort.rectangle.x, childPort.rectangle.y});
                    add({childPort.rectangle.x + childPort.rectangle.width,
                         childPort.rectangle.y + childPort.rectangle.height});
                }
                if (!havePoint)
                    return Point{instance->second.x, instance->second.y};
                return Point{(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
            };

            for (const auto &[_, connection] : current.plannedConnections) {
                if (connection.routed)
                    continue;
                const auto from = endpointPoint(connection.from, connection.fromPort);
                const auto to = endpointPoint(connection.to, connection.toPort);
                if (!from || !to)
                    continue;
                LayoutPreviewConnection preview;
                preview.line = QLineF(from->x, from->y, to->x, to->y);
                if (const auto net = current.nets.find(connection.net); net != current.nets.end())
                    preview.net = QString::fromStdString(net->second.name);
                model.plannedConnections.push_back(std::move(preview));
            }
        }

        if (!includeInstances)
            return;
        for (const auto &[_, instance] : current.instances) {
            if (ancestors.contains(instance.cell))
                continue;
            const auto child = project.layoutCells.find(instance.cell);
            if (child == project.layoutCells.end())
                continue;
            auto childChain = chain;
            childChain.push_back(Transform{instance.x, instance.y, instance.orientation});
            ancestors.insert(instance.cell);
            collect(child->second, childChain, false);
            ancestors.erase(instance.cell);
        }
    };
    collect(cell, rootTransform, true);
    model.bounds = valid ? bounds : QRectF{};
    return model;
}

LayoutRenderer::DeviceStyleMap LayoutRenderer::deviceStylesFor(const Project &project,
                                                               const LayoutDeviceLibrary &library,
                                                               const LayerStyleMap &styles) {
    DeviceStyleMap result;
    for (const auto &layer : library.layers) {
        LayerStyle style;
        bool found = false;
        for (const auto &[id, projectLayer] : project.layers) {
            if (projectLayer.number != layer.number || projectLayer.name != layer.name)
                continue;
            if (const auto entry = styles.find(id); entry != styles.end()) {
                style = entry->second;
                found = true;
            }
            break;
        }
        if (!found)
            style = layerStyleForNumber(layer.number);
        result[{layer.number, layer.datatype}] = style;
    }
    return result;
}

void LayoutRenderer::paintFitted(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                                 const DeviceStyleMap &deviceStyles, const QRectF &target,
                                 const PaintOptions &options) {
    if (model.empty() || !target.isValid())
        return;
    const auto transform = fittedTransform(model.bounds, target);
    painter.save();
    painter.setClipRect(target);
    painter.setRenderHint(QPainter::Antialiasing);
    paintModel(painter, model, styles, deviceStyles, transform, options);
    painter.restore();
}

void LayoutRenderer::paintAt(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                             const DeviceStyleMap &deviceStyles, QPointF center, const PaintOptions &options) {
    if (model.empty())
        return;
    const auto transform = centeredTransform(model.bounds, center);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    paintModel(painter, model, styles, deviceStyles, transform, options);
    painter.restore();
}

void LayoutRenderer::paintAtOrigin(QPainter &painter, const LayoutPreviewModel &model, const LayerStyleMap &styles,
                                   const DeviceStyleMap &deviceStyles, QPointF origin, const PaintOptions &options) {
    if (model.empty())
        return;
    DisplayTransform transform;
    transform.offset = origin;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    paintModel(painter, model, styles, deviceStyles, transform, options);
    painter.restore();
}

} // namespace ice
