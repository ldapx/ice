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
                if (layerId != shape.layer.id && !layerVisible(layerIdOnly(layerId)))
                    continue;
                const auto segmentStyle =
                    layerId == shape.layer.id ? style : styleFor(layerIdOnly(layerId), styles, deviceStyles);
                const auto polygon = pathPolygon(
                    transform.map(shape.centerline[index]), transform.map(shape.centerline[index + 1]),
                    (index < shape.segmentWidths.size() ? shape.segmentWidths[index] : shape.width) * transform.scale);
                paintPolygon(painter, polygon, segmentStyle, options);
            }
            for (std::size_t index = 0; index < shape.centerline.size(); ++index) {
                if (index >= shape.pointVias.size())
                    continue;
                for (const auto layerId : shape.pointVias[index]) {
                    if (!layerVisible(layerIdOnly(layerId)))
                        continue;
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

    // Text labels stay above geometry and below port overlays, matching the
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
            if ((!port.intrinsic && !layerVisible(port.layer)) || !netVisible(port.netId, false))
                continue;
            const auto style =
                port.intrinsic ? LayerStyle{accent, Qt::SolidPattern} : styleFor(port.layer, styles, deviceStyles);
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

// DEF-imported cells only carry a master name, not an OpenDB master type, so
// classify the common standard-cell families from their LEF names.
QString instanceCategoryForName(const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (lower.find("__fill") != std::string::npos || lower.find("__spacer") != std::string::npos)
        return QStringLiteral("fill");
    if (lower.find("__endcap") != std::string::npos)
        return QStringLiteral("endcap");
    if (lower.find("__tap") != std::string::npos || lower.find("welltap") != std::string::npos)
        return QStringLiteral("tap");
    if (lower.find("__antenna") != std::string::npos)
        return QStringLiteral("antenna");
    if (lower.find("__cover") != std::string::npos)
        return QStringLiteral("cover");
    return QStringLiteral("stdcell");
}

// Fallback heat maps mirror the ODB reader so the DEF preview keeps the same
// overlays when the optional ice-openroad helper is not built.
std::optional<LayoutPreviewHeatMap> computePlacementDensity(const LayoutPreviewModel &model, int bins) {
    const auto bounds = model.bounds;
    if (bins <= 0 || !bounds.isValid() || bounds.width() <= 0.0 || bounds.height() <= 0.0)
        return std::nullopt;
    const double binWidth = bounds.width() / bins;
    const double binHeight = bounds.height() / bins;
    const double binArea = binWidth * binHeight;
    if (binArea <= 0.0)
        return std::nullopt;
    LayoutPreviewHeatMap map;
    map.bounds = bounds;
    map.columns = bins;
    map.rows = bins;
    map.values.assign(static_cast<std::size_t>(bins) * bins, 0.0);
    const auto accumulate = [&](const QRectF &rectangle) {
        const auto clipped = rectangle.intersected(bounds);
        if (!clipped.isValid() || clipped.width() <= 0.0 || clipped.height() <= 0.0)
            return;
        const int x0 =
            std::clamp(static_cast<int>(std::floor((clipped.left() - bounds.left()) / binWidth)), 0, bins - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((clipped.right() - bounds.left()) / binWidth)), 0, bins);
        const int y0 =
            std::clamp(static_cast<int>(std::floor((clipped.top() - bounds.top()) / binHeight)), 0, bins - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil((clipped.bottom() - bounds.top()) / binHeight)), 0, bins);
        for (int y = y0; y < y1; ++y) {
            const double top = bounds.top() + y * binHeight;
            const double bottom = top + binHeight;
            const double overlapY = std::min(clipped.bottom(), bottom) - std::max(clipped.top(), top);
            if (overlapY <= 0.0)
                continue;
            for (int x = x0; x < x1; ++x) {
                const double left = bounds.left() + x * binWidth;
                const double right = left + binWidth;
                const double overlapX = std::min(clipped.right(), right) - std::max(clipped.left(), left);
                if (overlapX <= 0.0)
                    continue;
                map.values[static_cast<std::size_t>(y) * bins + x] += 100.0 * overlapX * overlapY / binArea;
            }
        }
    };
    for (const auto &device : model.plannedDevices) {
        const auto category = device.category.toLower();
        if (category == QStringLiteral("fill") || category == QStringLiteral("endcap") ||
            category == QStringLiteral("tap") || category == QStringLiteral("antenna") ||
            category == QStringLiteral("cover"))
            continue;
        accumulate(device.rectangle);
    }
    double maximum = 0.0;
    for (auto &value : map.values) {
        value = std::min(100.0, value);
        maximum = std::max(maximum, value);
    }
    if (maximum <= 0.0)
        return std::nullopt;
    map.minimum = 0.0;
    map.maximum = maximum;
    return map;
}

std::optional<LayoutPreviewHeatMap> computeRoutingCongestion(const LayoutPreviewModel &model, int bins) {
    const auto bounds = model.bounds;
    if (bins <= 0 || !bounds.isValid() || bounds.width() <= 0.0 || bounds.height() <= 0.0)
        return std::nullopt;
    const double binWidth = bounds.width() / bins;
    const double binHeight = bounds.height() / bins;
    const double binArea = binWidth * binHeight;
    if (binArea <= 0.0)
        return std::nullopt;
    LayoutPreviewHeatMap map;
    map.bounds = bounds;
    map.columns = bins;
    map.rows = bins;
    map.values.assign(static_cast<std::size_t>(bins) * bins, 0.0);
    const auto accumulate = [&](const QRectF &rectangle) {
        const auto clipped = rectangle.intersected(bounds);
        if (!clipped.isValid() || clipped.width() <= 0.0 || clipped.height() <= 0.0)
            return;
        const int x0 =
            std::clamp(static_cast<int>(std::floor((clipped.left() - bounds.left()) / binWidth)), 0, bins - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil((clipped.right() - bounds.left()) / binWidth)), 0, bins);
        const int y0 =
            std::clamp(static_cast<int>(std::floor((clipped.top() - bounds.top()) / binHeight)), 0, bins - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil((clipped.bottom() - bounds.top()) / binHeight)), 0, bins);
        for (int y = y0; y < y1; ++y) {
            const double top = bounds.top() + y * binHeight;
            const double bottom = top + binHeight;
            const double overlapY = std::min(clipped.bottom(), bottom) - std::max(clipped.top(), top);
            if (overlapY <= 0.0)
                continue;
            for (int x = x0; x < x1; ++x) {
                const double left = bounds.left() + x * binWidth;
                const double right = left + binWidth;
                const double overlapX = std::min(clipped.right(), right) - std::max(clipped.left(), left);
                if (overlapX <= 0.0)
                    continue;
                map.values[static_cast<std::size_t>(y) * bins + x] += 100.0 * overlapX * overlapY / binArea;
            }
        }
    };
    for (const auto &shape : model.shapes) {
        if (shape.netId == 0 || shape.kind == LayoutPreviewShape::Kind::text)
            continue;
        if (shape.kind == LayoutPreviewShape::Kind::path) {
            for (std::size_t index = 0; index + 1 < shape.centerline.size(); ++index) {
                const auto start = shape.centerline[index];
                const auto end = shape.centerline[index + 1];
                const auto width = index < shape.segmentWidths.size() ? shape.segmentWidths[index] : shape.width;
                const auto half = std::max<qreal>(0.0, width * 0.5);
                accumulate(QRectF(QPointF(std::min(start.x(), end.x()) - half, std::min(start.y(), end.y()) - half),
                                  QPointF(std::max(start.x(), end.x()) + half, std::max(start.y(), end.y()) + half))
                               .normalized());
            }
            continue;
        }
        accumulate(shape.polygon.boundingRect());
    }
    double maximum = 0.0;
    for (auto &value : map.values) {
        value = std::min(100.0, value);
        maximum = std::max(maximum, value);
    }
    if (maximum <= 0.0)
        return std::nullopt;
    map.minimum = 0.0;
    map.maximum = maximum;
    return map;
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
                                               const std::vector<Transform> &rootTransform,
                                               const LayoutReferencePreviewResolver &referenceResolver,
                                               bool includeInstanceDevices, bool includeNets) {
    LayoutPreviewModel model;
    QRectF bounds;
    bool valid = false;
    std::set<ObjectId> ancestors{cell.id};

    const auto appendReferenceModel = [&](const LayoutPreviewModel &source, const std::vector<Transform> &chain) {
        for (const auto &shape : source.shapes) {
            auto preview = shape;
            if (preview.kind == LayoutPreviewShape::Kind::text) {
                const auto first =
                    applyTransformChain({static_cast<Coordinate>(std::llround(preview.textRectangle.left())),
                                         static_cast<Coordinate>(std::llround(preview.textRectangle.top()))},
                                        chain);
                const auto second =
                    applyTransformChain({static_cast<Coordinate>(std::llround(preview.textRectangle.right())),
                                         static_cast<Coordinate>(std::llround(preview.textRectangle.bottom()))},
                                        chain);
                preview.textRectangle = QRectF(QPointF(std::min(first.x, second.x), std::min(first.y, second.y)),
                                               QPointF(std::max(first.x, second.x), std::max(first.y, second.y)))
                                            .normalized();
                includeRect(bounds, valid, preview.textRectangle);
            } else if (preview.kind == LayoutPreviewShape::Kind::path) {
                for (auto &point : preview.centerline) {
                    const auto mapped = applyTransformChain({static_cast<Coordinate>(std::llround(point.x())),
                                                             static_cast<Coordinate>(std::llround(point.y()))},
                                                            chain);
                    point = QPointF(mapped.x, mapped.y);
                }
                if (!preview.centerline.empty()) {
                    auto minX = preview.centerline.front().x();
                    auto minY = preview.centerline.front().y();
                    auto maxX = minX;
                    auto maxY = minY;
                    for (const auto &point : preview.centerline) {
                        minX = std::min(minX, point.x());
                        minY = std::min(minY, point.y());
                        maxX = std::max(maxX, point.x());
                        maxY = std::max(maxY, point.y());
                    }
                    const auto margin = std::max<qreal>(0.0, preview.width * 0.5);
                    includeRect(
                        bounds, valid,
                        QRectF(minX - margin, minY - margin, maxX - minX + 2.0 * margin, maxY - minY + 2.0 * margin));
                }
            } else {
                for (auto &point : preview.polygon) {
                    const auto mapped = applyTransformChain({static_cast<Coordinate>(std::llround(point.x())),
                                                             static_cast<Coordinate>(std::llround(point.y()))},
                                                            chain);
                    point = QPointF(mapped.x, mapped.y);
                }
                includeRect(bounds, valid, preview.polygon.boundingRect());
            }
            model.shapes.push_back(std::move(preview));
        }
        for (const auto &port : source.ports) {
            auto preview = port;
            const auto &rectangle = port.rectangle;
            const std::array<Point, 4> corners = {
                Point{static_cast<Coordinate>(std::llround(rectangle.left())),
                      static_cast<Coordinate>(std::llround(rectangle.top()))},
                Point{static_cast<Coordinate>(std::llround(rectangle.right())),
                      static_cast<Coordinate>(std::llround(rectangle.top()))},
                Point{static_cast<Coordinate>(std::llround(rectangle.right())),
                      static_cast<Coordinate>(std::llround(rectangle.bottom()))},
                Point{static_cast<Coordinate>(std::llround(rectangle.left())),
                      static_cast<Coordinate>(std::llround(rectangle.bottom()))},
            };
            QPolygonF polygon;
            for (const auto &corner : corners) {
                const auto mapped = applyTransformChain(corner, chain);
                polygon << QPointF(mapped.x, mapped.y);
            }
            preview.rectangle = polygon.boundingRect();
            includeRect(bounds, valid, preview.rectangle);
            model.ports.push_back(std::move(preview));
        }
    };

    std::function<void(const LayoutCell &, const std::vector<Transform> &, bool)> collect;
    collect = [&](const LayoutCell &current, const std::vector<Transform> &chain, bool root) {
        if (includeNets && root) {
            for (const auto &[netId, net] : current.nets) {
                LayoutPreviewNet preview;
                preview.id = static_cast<int>(netId);
                preview.name = QString::fromStdString(net.name);
                model.nets.push_back(std::move(preview));
            }
        }
        const auto applyNet = [&](LayoutPreviewShape &preview, const Shape &shape) {
            if (!includeNets || !shape.net)
                return;
            preview.netId = static_cast<int>(*shape.net);
            const auto net = current.nets.find(*shape.net);
            if (net != current.nets.end())
                preview.net = QString::fromStdString(net->second.name);
        };
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
                applyNet(preview, shape);
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
                preview.segmentWidths.reserve(shape.segmentWidths.size());
                for (const auto width : shape.segmentWidths)
                    preview.segmentWidths.push_back(static_cast<qreal>(width));
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
                applyNet(preview, shape);
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
            applyNet(preview, shape);
            includeRect(bounds, valid, preview.polygon.boundingRect());
            model.shapes.push_back(std::move(preview));
        }

        if (includePorts) {
            for (const auto &[_, port] : current.ports) {
                if (!port.visible)
                    continue;
                LayoutPreviewPort preview;
                preview.layer = layerFor(project, port.layer);
                preview.intrinsic = port.layer == 0;
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
                if (!device.placeholder)
                    continue;
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
                if (const auto port = current.ports.find(endpoint); port != current.ports.end())
                    return Point{port->second.rectangle.x + port->second.rectangle.width / 2,
                                 port->second.rectangle.y + port->second.rectangle.height / 2};
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
            if (child->second.reference && referenceResolver) {
                if (const auto resolved = referenceResolver(child->second)) {
                    appendReferenceModel(*resolved, childChain);
                    continue;
                }
            }
            ancestors.insert(instance.cell);
            collect(child->second, childChain, false);
            ancestors.erase(instance.cell);
        }
    };
    collect(cell, rootTransform, true);

    if (includeInstanceDevices) {
        // Imported DEF cells place their standard cells as child layout cells.
        // Report each top-level instance as a preview device so the physical
        // display controls can group instances the same way an ODB preview does.
        std::map<ObjectId, QRectF> masterBounds;
        for (const auto &[_, instance] : cell.instances) {
            const auto child = project.layoutCells.find(instance.cell);
            if (child == project.layoutCells.end())
                continue;
            auto found = masterBounds.find(instance.cell);
            if (found == masterBounds.end()) {
                const auto childModel =
                    buildLayoutCellPreviewModel(project, child->second, true, true, false, {}, referenceResolver);
                found = masterBounds.emplace(instance.cell, childModel.bounds).first;
            }
            if (!found->second.isValid())
                continue;
            const Transform transform{instance.x, instance.y, instance.orientation};
            const auto &master = found->second;
            const std::array<Point, 4> corners = {
                Point{static_cast<Coordinate>(std::llround(master.left())),
                      static_cast<Coordinate>(std::llround(master.top()))},
                Point{static_cast<Coordinate>(std::llround(master.left() + master.width())),
                      static_cast<Coordinate>(std::llround(master.top()))},
                Point{static_cast<Coordinate>(std::llround(master.left() + master.width())),
                      static_cast<Coordinate>(std::llround(master.top() + master.height()))},
                Point{static_cast<Coordinate>(std::llround(master.left())),
                      static_cast<Coordinate>(std::llround(master.top() + master.height()))},
            };
            qreal minX = 0.0;
            qreal minY = 0.0;
            qreal maxX = 0.0;
            qreal maxY = 0.0;
            bool firstCorner = true;
            for (const auto &corner : corners) {
                const auto mapped = transform.apply(corner);
                if (firstCorner) {
                    minX = maxX = mapped.x;
                    minY = maxY = mapped.y;
                    firstCorner = false;
                    continue;
                }
                minX = std::min<qreal>(minX, mapped.x);
                minY = std::min<qreal>(minY, mapped.y);
                maxX = std::max<qreal>(maxX, mapped.x);
                maxY = std::max<qreal>(maxY, mapped.y);
            }
            LayoutPreviewPlannedDevice device;
            device.rectangle = QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).normalized();
            device.name = QString::fromStdString(instance.name);
            device.id = static_cast<int>(instance.id);
            device.master = QString::fromStdString(child->second.name);
            device.category = instanceCategoryForName(child->second.name);
            device.orientation = QString::fromStdString(orientationName(instance.orientation));
            includeRect(bounds, valid, device.rectangle);
            model.plannedDevices.push_back(std::move(device));
        }
    }

    model.bounds = valid ? bounds : QRectF{};
    if (includeNets && includeInstanceDevices) {
        constexpr int heatMapBins = 32;
        model.placementDensity = computePlacementDensity(model, heatMapBins);
        model.routingCongestion = computeRoutingCongestion(model, heatMapBins);
    }
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
