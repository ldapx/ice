#include "layout_canvas.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QApplication>
#include <QGraphicsItemGroup>
#include <QGraphicsRectItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

LayoutCanvas::LayoutCanvas(QWidget *parent) : QGraphicsView(parent) {}
LayoutCanvas::~LayoutCanvas() = default;

class LayoutCanvasImpl : public LayoutCanvas {
  public:
    explicit LayoutCanvasImpl(QWidget *parent = nullptr) : LayoutCanvas(parent) {
        setScene(new QGraphicsScene(this));
        setSceneRect(-1.0e9, -1.0e9, 2.0e9, 2.0e9);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        applyNavigationTransform();
    }

    void setTool(Tool tool) {
        if (readOnly_ && tool != Tool::select)
            tool = Tool::select;
        tool_ = tool;
        if (tool_ == Tool::select)
            unsetCursor();
        else
            setCursor(Qt::CrossCursor);
        viewport()->update();
    }

    void setReadOnly(bool readOnly) {
        readOnly_ = readOnly;
        if (readOnly_) {
            selectedLayoutInstance_ = 0;
            applyLayoutInstanceSelection();
            setTool(Tool::select);
        }
    }

    bool isReadOnly() const { return readOnly_; }
    Tool currentTool() const { return tool_; }

    void setGridVisible(bool visible) {
        gridVisible_ = visible;
        viewport()->update();
    }

    void setDevicePreview(const ice::LayoutDeviceCell *cell) {
        devicePreview_ = cell;
        viewport()->update();
    }

    void restoreLayoutInstanceSelection() { applyLayoutInstanceSelection(); }

    void setDevicePlacementCallback(std::function<void(QPointF)> callback) {
        devicePlacementRequested_ = std::move(callback);
    }

    void setInstanceActivatedCallback(std::function<void(ice::ObjectId)> callback) {
        instanceActivated_ = std::move(callback);
    }

    void setInstanceSelectionCallback(std::function<void(ice::ObjectId)> callback) {
        instanceSelectionChanged_ = std::move(callback);
    }

    ice::ObjectId selectedLayoutInstance() const { return selectedLayoutInstance_; }

    void setToolEscapeCallback(std::function<void()> callback) { toolEscapeRequested_ = std::move(callback); }

    void deleteSelectedShapes() {
        if (readOnly_)
            return;
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).isValid())
                ids.push_back(item->data(0).toULongLong());
        if (!ids.empty() && deleted_)
            deleted_(ids);
    }

    void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                      std::function<void(ice::ObjectId, QPointF)> shapeMoved,
                      std::function<void(std::vector<ice::ObjectId>)> deleted, std::function<void()> changed) {
        rectangleCreated_ = std::move(rectangleCreated);
        shapeMoved_ = std::move(shapeMoved);
        deleted_ = std::move(deleted);
        changed_ = std::move(changed);
    }

    void drawGrid(QPainter &painter, const QRectF &exposed, qreal scale) {
        if (scale <= 0.0)
            return;
        const qreal targetPixels = 24.0;
        const qreal rawGrid = targetPixels / scale;
        const qreal magnitude = std::pow(10.0, std::floor(std::log10(rawGrid)));
        const qreal normalized = rawGrid / magnitude;
        const qreal factor = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
        const qreal major = magnitude * factor;
        const int left = static_cast<int>(std::floor(exposed.left() / major));
        const int right = static_cast<int>(std::ceil(exposed.right() / major));
        const int top = static_cast<int>(std::floor(exposed.top() / major));
        const int bottom = static_cast<int>(std::ceil(exposed.bottom() / major));
        if (right - left > 500 || bottom - top > 500)
            return;
        // Minor subdivisions like the schematic grid (5x per major step).
        // Cosmetic 1px pens so lines stay visible at any zoom; scene-unit
        // widths would shrink to sub-pixel sizes when zoomed out.
        const qreal minor = major / 5.0;
        const bool drawMinor = (right - left) * 5 + (bottom - top) * 5 <= 5000;
        if (drawMinor) {
            QPen minorPen(ice::uiColor(ice::UiColor::Grid1));
            minorPen.setCosmetic(true);
            minorPen.setWidth(1);
            painter.setPen(minorPen);
            const int minorLeft = static_cast<int>(std::floor(exposed.left() / minor));
            const int minorRight = static_cast<int>(std::ceil(exposed.right() / minor));
            const int minorTop = static_cast<int>(std::floor(exposed.top() / minor));
            const int minorBottom = static_cast<int>(std::ceil(exposed.bottom() / minor));
            for (int x = minorLeft; x <= minorRight; ++x)
                painter.drawLine(QPointF(x * minor, exposed.top()), QPointF(x * minor, exposed.bottom()));
            for (int y = minorTop; y <= minorBottom; ++y)
                painter.drawLine(QPointF(exposed.left(), y * minor), QPointF(exposed.right(), y * minor));
        }
        QPen majorPen(ice::uiColor(ice::UiColor::Grid2));
        majorPen.setCosmetic(true);
        majorPen.setWidth(1);
        painter.setPen(majorPen);
        for (int x = left; x <= right; ++x)
            painter.drawLine(QPointF(x * major, exposed.top()), QPointF(x * major, exposed.bottom()));
        for (int y = top; y <= bottom; ++y)
            painter.drawLine(QPointF(exposed.left(), y * major), QPointF(exposed.right(), y * major));
    }

    void setLayerStyles(const std::map<ice::ObjectId, LayerStyle> &styles) { layerStyles_ = styles; }
    void setInitialView(const QRectF &bounds) {
        if (!bounds.isValid() || bounds.isNull())
            return;
        const auto available = viewport()->size();
        const qreal width = std::max<qreal>(1.0, bounds.width());
        const qreal height = std::max<qreal>(1.0, bounds.height());
        zoom_ = std::clamp(std::min(available.width() / width, available.height() / height) * 0.85, qreal(1.0e-6),
                           qreal(1.0e6));
        centerScene_ = bounds.center();
        applyNavigationTransform();
    }

    void fitToContent() {
        const auto bounds = scene()->itemsBoundingRect();
        if (!bounds.isValid() || bounds.isNull()) {
            zoom_ = 1.0;
            centerScene_ = QPointF(0.0, 0.0);
            applyNavigationTransform();
            return;
        }
        setInitialView(bounds.adjusted(-20, -20, 20, 20));
    }

  protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override {
        painter->fillRect(rect, ice::uiColor(ice::UiColor::Background2));
        if (gridVisible_)
            drawGrid(*painter, rect, transform().m11());
        drawCanvasAxes(*painter, rect);
    }

    void drawForeground(QPainter *painter, const QRectF &rect) override {
        if (tool_ != Tool::placeDevice || !devicePreview_ || !viewport()->underMouse())
            return;
        painter->save();
        painter->setClipRect(rect);
        painter->setRenderHint(QPainter::Antialiasing);
        const auto &colors = layerPalette();
        const auto bounds = devicePreview_->bounds;
        const qreal centerX = bounds.valid() ? bounds.x + bounds.width / 2.0 : 0.0;
        const qreal centerY = bounds.valid() ? bounds.y + bounds.height / 2.0 : 0.0;
        for (const auto &shape : devicePreview_->shapes) {
            QPolygonF polygon;
            polygon.reserve(static_cast<int>(shape.polygon.size()));
            for (const auto &point : shape.polygon) {
                polygon << QPointF(cursorScenePosition_.x() + point.x - centerX,
                                   cursorScenePosition_.y() - point.y + centerY);
            }
            auto color = colors[(static_cast<std::size_t>(shape.layer) * 31 + shape.datatype) % colors.size()];
            QPen pen(color);
            pen.setCosmetic(true);
            painter->setPen(pen);
            color.setAlpha(55);
            painter->setBrush(color);
            painter->drawPolygon(polygon);
        }
        painter->restore();
    }

    void drawScaleIndicator(QPainter &painter) {
        if (zoom_ <= 0.0)
            return;

        const qreal targetPixels = 100.0;
        const qreal rawLength = targetPixels / zoom_;
        const qreal magnitude = std::pow(10.0, std::floor(std::log10(rawLength)));
        const qreal normalized = rawLength / magnitude;
        const qreal factor = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
        const qreal length = magnitude * factor;
        const qreal pixels = length * zoom_;

        const qreal left = 14.0;
        const qreal bottom = viewport()->height() - 16.0;
        const auto color = ice::uiColor(ice::UiColor::Axis1);
        QPen pen(color);
        pen.setWidthF(2.0);
        painter.setPen(pen);
        painter.drawLine(QPointF(left, bottom), QPointF(left + pixels, bottom));
        painter.drawLine(QPointF(left, bottom - 5.0), QPointF(left, bottom + 1.0));
        painter.drawLine(QPointF(left + pixels, bottom - 5.0), QPointF(left + pixels, bottom + 1.0));
        painter.setPen(color);
        painter.drawText(QPointF(left, bottom - 8.0), formatScaleLength(length));
    }

    void paintEvent(QPaintEvent *event) override {
        QGraphicsView::paintEvent(event);
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        drawScaleIndicator(painter);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            panStartPosition_ = event->pos();
            panStartCenterScene_ = centerScene_;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (tool_ == Tool::placeDevice && event->button() == Qt::LeftButton) {
            if (readOnly_) {
                QApplication::beep();
                event->accept();
                return;
            }
            if (devicePlacementRequested_)
                devicePlacementRequested_(mapToScene(event->pos()));
            event->accept();
            return;
        }
        if (tool_ == Tool::rectangle && event->button() == Qt::LeftButton) {
            if (readOnly_) {
                QApplication::beep();
                event->accept();
                return;
            }
            drawing_ = true;
            drawStart_ = mapToScene(event->pos());
            const auto previewColor = ice::uiColor(ice::UiColor::Accent3);
            drawPreview_ =
                scene()->addRect(QRectF(drawStart_, drawStart_), QPen(previewColor, 1),
                                 QBrush(QColor(previewColor.red(), previewColor.green(), previewColor.blue(), 50)));
            event->accept();
            return;
        }
        if (tool_ == Tool::select && event->button() == Qt::LeftButton) {
            auto *clicked = itemAt(event->pos());
            auto *instance = layoutInstanceOwner(clicked);
            if (instance && !readOnly_) {
                const auto id = instance->data(0).toULongLong();
                if (event->modifiers().testFlag(Qt::ControlModifier) && selectedLayoutInstance_ == id) {
                    scene()->clearSelection();
                    selectedLayoutInstance_ = 0;
                    applyLayoutInstanceSelection();
                    event->accept();
                    return;
                }
                scene()->clearSelection();
                selectedLayoutInstance_ = id;
                applyLayoutInstanceSelection();
                movingItem_ = nullptr;
                draggingInstance_ = instance;
                dragStartScene_ = mapToScene(event->pos());
                dragStartPosition_ = instance->pos();
                event->accept();
                return;
            }
            if (selectedLayoutInstance_ != 0) {
                selectedLayoutInstance_ = 0;
                applyLayoutInstanceSelection();
            }
            movingItem_ = clicked;
            if (movingItem_)
                movingStartPosition_ = movingItem_->pos();
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        cursorScenePosition_ = mapToScene(event->pos());
        if (tool_ == Tool::placeDevice)
            viewport()->update();
        if (draggingInstance_) {
            const auto delta = mapToScene(event->pos()) - dragStartScene_;
            draggingInstance_->setPos(dragStartPosition_ + delta);
            event->accept();
            return;
        }
        if (panning_) {
            const auto delta = event->pos() - panStartPosition_;
            centerScene_ = panStartCenterScene_ - QPointF(delta.x() / zoom_, delta.y() / zoom_);
            applyNavigationTransform();
            event->accept();
            return;
        }
        if (drawing_ && drawPreview_) {
            drawPreview_->setRect(QRectF(drawStart_, mapToScene(event->pos())).normalized());
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() == 0) {
            event->ignore();
            return;
        }
        const auto scenePosition = mapToScene(event->position().toPoint());
        const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        zoom_ = std::clamp(zoom_ * factor, qreal(1.0e-6), qreal(1.0e6));
        const auto viewportCenter = QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5);
        const auto cursor = event->position();
        centerScene_ = scenePosition -
                       QPointF((cursor.x() - viewportCenter.x()) / zoom_, (cursor.y() - viewportCenter.y()) / zoom_);
        applyNavigationTransform();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::MiddleButton) {
            panning_ = false;
            setTool(tool_);
            event->accept();
            return;
        }
        if (drawing_ && event->button() == Qt::LeftButton) {
            const auto end = mapToScene(event->pos());
            const auto rectangle = QRectF(drawStart_, end).normalized();
            if (drawPreview_) {
                scene()->removeItem(drawPreview_);
                delete drawPreview_;
                drawPreview_ = nullptr;
            }
            drawing_ = false;
            if (rectangle.width() >= 1.0 && rectangle.height() >= 1.0 && rectangleCreated_)
                rectangleCreated_(drawStart_, end);
            event->accept();
            return;
        }
        if (draggingInstance_ && event->button() == Qt::LeftButton) {
            auto *instance = draggingInstance_;
            draggingInstance_ = nullptr;
            const auto delta = instance->pos() - dragStartPosition_;
            if ((delta.x() != 0.0 || delta.y() != 0.0) && shapeMoved_)
                shapeMoved_(instance->data(0).toULongLong(), delta);
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
        if (tool_ == Tool::select && event->button() == Qt::LeftButton && movingItem_) {
            const auto delta = movingItem_->pos() - movingStartPosition_;
            if ((delta.x() != 0.0 || delta.y() != 0.0) && shapeMoved_)
                shapeMoved_(movingItem_->data(0).toULongLong(), delta);
            movingItem_ = nullptr;
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (tool_ == Tool::select && event->button() == Qt::LeftButton) {
            if (auto *instance = layoutInstanceOwner(itemAt(event->pos()))) {
                draggingInstance_ = nullptr;
                movingItem_ = nullptr;
                if (instanceActivated_)
                    instanceActivated_(instance->data(0).toULongLong());
                event->accept();
                return;
            }
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape && (tool_ == Tool::placeDevice || tool_ == Tool::rectangle)) {
            setTool(Tool::select);
            if (toolEscapeRequested_)
                toolEscapeRequested_();
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && deleted_ && !readOnly_) {
            std::vector<ice::ObjectId> ids;
            for (auto *item : scene()->selectedItems())
                if (item->data(0).isValid()) {
                    const auto id = item->data(0).toULongLong();
                    if (id != 0)
                        ids.push_back(id);
                }
            if (!ids.empty())
                deleted_(std::move(ids));
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

  private:
    QGraphicsItemGroup *layoutInstanceOwner(QGraphicsItem *item) const {
        for (auto *cursor = item; cursor; cursor = cursor->parentItem()) {
            if (cursor->data(1).toString() == QStringLiteral("layout-instance"))
                return dynamic_cast<QGraphicsItemGroup *>(cursor);
        }
        return nullptr;
    }

    void applyLayoutInstanceSelection() {
        if (readOnly_)
            selectedLayoutInstance_ = 0;
        bool found = false;
        for (auto *item : scene()->items()) {
            if (item->data(1).toString() != QStringLiteral("layout-instance"))
                continue;
            auto *group = dynamic_cast<QGraphicsItemGroup *>(item);
            if (!group)
                continue;
            const bool selected =
                selectedLayoutInstance_ != 0 && group->data(0).toULongLong() == selectedLayoutInstance_;
            group->setSelected(selected);
            for (auto *child : group->childItems())
                if (child->data(0).toString() == QStringLiteral("layout-instance-highlight"))
                    child->setVisible(selected);
            found |= selected;
        }
        if (!found)
            selectedLayoutInstance_ = 0;
        if (notifiedLayoutInstance_ != selectedLayoutInstance_) {
            notifiedLayoutInstance_ = selectedLayoutInstance_;
            if (instanceSelectionChanged_)
                instanceSelectionChanged_(selectedLayoutInstance_);
        }
    }

    void applyNavigationTransform() {
        setTransform(QTransform(zoom_, 0.0, 0.0, zoom_, 0.0, 0.0), false);
        centerOn(centerScene_);
        viewport()->update();
    }

    QString formatScaleLength(qreal length) const {
        if (length >= 1000000.0)
            return QStringLiteral("%1 mm").arg(length / 1000000.0, 0, 'g', 4);
        if (length >= 1000.0)
            return QStringLiteral("%1 um").arg(length / 1000.0, 0, 'g', 4);
        if (length >= 1.0)
            return QStringLiteral("%1 nm").arg(length, 0, 'g', 4);
        return QStringLiteral("%1 pm").arg(length * 1000.0, 0, 'g', 4);
    }

    Tool tool_ = Tool::select;
    bool panning_ = false;
    bool drawing_ = false;
    QPoint panStartPosition_;
    QPointF panStartCenterScene_;
    QPointF drawStart_;
    QGraphicsRectItem *drawPreview_ = nullptr;
    QGraphicsItem *movingItem_ = nullptr;
    QGraphicsItemGroup *draggingInstance_ = nullptr;
    QPointF dragStartScene_;
    QPointF dragStartPosition_;
    QPointF movingStartPosition_;
    ice::ObjectId selectedLayoutInstance_ = 0;
    std::function<void(QPointF, QPointF)> rectangleCreated_;
    std::function<void(ice::ObjectId, QPointF)> shapeMoved_;
    std::function<void(std::vector<ice::ObjectId>)> deleted_;
    std::function<void()> changed_;
    std::function<void(QPointF)> devicePlacementRequested_;
    std::function<void(ice::ObjectId)> instanceActivated_;
    std::function<void(ice::ObjectId)> instanceSelectionChanged_;
    std::function<void()> toolEscapeRequested_;
    ice::ObjectId notifiedLayoutInstance_ = 0;
    const ice::LayoutDeviceCell *devicePreview_ = nullptr;
    QPointF cursorScenePosition_;
    std::map<ice::ObjectId, LayerStyle> layerStyles_;
    bool gridVisible_ = true;
    bool readOnly_ = false;
    qreal zoom_ = 1.0;
    QPointF centerScene_;
};

// Item data roles used to identify schematic canvas items.
//
// role 0: item kind ("symbol", "symbol-artwork", "symbol-label",
//        "symbol-highlight", "pin", "wire", "net-label")
// role 1: the stable domain id of the object the item projects
LayoutCanvas *createLayoutCanvas(QWidget *parent) { return new LayoutCanvasImpl(parent); }
