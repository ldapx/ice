#include "schematic_canvas.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QApplication>
#include <QBrush>
#include <QEvent>
#include <QGraphicsColorizeEffect>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QImage>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPen>
#include <QShowEvent>
#include <QSvgRenderer>
#include <QTimer>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

SchematicCanvas::SchematicCanvas(QWidget *parent) : QGraphicsView(parent) {}
SchematicCanvas::~SchematicCanvas() = default;

QGraphicsItem *symbolOwner(QGraphicsItem *item) {
    for (auto *cursor = item; cursor; cursor = cursor->parentItem())
        if (cursor->data(0).toString() == QStringLiteral("symbol"))
            return cursor;
    return nullptr;
}

QGraphicsItem *symbolChild(QGraphicsItem *symbol, const QString &kind) {
    for (auto *child : symbol->childItems())
        if (child->data(0).toString() == kind)
            return child;
    return nullptr;
}

QGraphicsColorizeEffect *symbolArtworkEffect(QGraphicsItem *symbol) {
    auto *artwork = symbolChild(symbol, QStringLiteral("symbol-artwork"));
    if (!artwork)
        return nullptr;
    const auto handle = artwork->data(3).toULongLong();
    return reinterpret_cast<QGraphicsColorizeEffect *>(handle);
}

void applySymbolSelectedStyle(QGraphicsItem *symbol, bool selected) {
    if (!symbol)
        return;
    symbol->setSelected(selected);
    if (auto *highlight = symbolChild(symbol, QStringLiteral("symbol-highlight")))
        highlight->setVisible(selected);
    if (auto *label = symbolChild(symbol, QStringLiteral("symbol-label")))
        static_cast<QGraphicsTextItem *>(label)->setDefaultTextColor(selected ? ice::uiColor(ice::UiColor::Accent4)
                                                                              : ice::uiColor(ice::UiColor::Axis1));
    if (auto *effect = symbolArtworkEffect(symbol)) {
        effect->setColor(selected ? ice::uiColor(ice::UiColor::Accent4) : ice::uiColor(ice::UiColor::Axis1));
        effect->setEnabled(true);
    }
}

class SchematicCanvasImpl : public SchematicCanvas {
  public:
    explicit SchematicCanvasImpl(QWidget *parent = nullptr) : SchematicCanvas(parent) {
        setScene(new QGraphicsScene(this));
        setSceneRect(-1.0e9, -1.0e9, 2.0e9, 2.0e9);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        applyNavigationTransform();
    }

    void setCallbacks(std::function<void(ice::ObjectId, QPointF)> moved, std::function<void(ice::ObjectId)> rotated,
                      std::function<void(ice::ObjectId, ice::ObjectId)> connected,
                      std::function<void(QPointF)> netLabelPlaced, std::function<void()> changed,
                      std::function<void(ice::ObjectId)> symbolSelected = {},
                      std::function<void(ice::ObjectId)> symbolActivated = {}) {
        moved_ = std::move(moved);
        rotated_ = std::move(rotated);
        connected_ = std::move(connected);
        netLabelPlaced_ = std::move(netLabelPlaced);
        changed_ = std::move(changed);
        symbolSelected_ = std::move(symbolSelected);
        symbolActivated_ = std::move(symbolActivated);
    }

    void setDeleteCallback(std::function<void(std::vector<ice::ObjectId>)> callback) override {
        deleteSelected_ = std::move(callback);
    }

    void setNetLabelMode(bool enabled) {
        netLabelMode_ = enabled;
        if (enabled) {
            signalPickMode_ = false;
            setCursor(Qt::CrossCursor);
        } else if (!placeSymbolMode_ && !signalPickMode_) {
            unsetCursor();
        }
        updatePinVisibility();
    }

    void setPlaceSymbolMode(bool enabled) {
        placeSymbolMode_ = enabled;
        if (enabled) {
            signalPickMode_ = false;
            setCursor(Qt::CrossCursor);
        } else if (!netLabelMode_ && !signalPickMode_) {
            unsetCursor();
        }
        updatePinVisibility();
    }

    bool isSignalPickMode() const { return signalPickMode_; }

    void setSignalPickMode(bool enabled) {
        signalPickMode_ = enabled;
        if (enabled)
            setCursor(Qt::CrossCursor);
        else if (!placeSymbolMode_ && !netLabelMode_)
            unsetCursor();
        updatePinVisibility();
    }

    void setSignalPickedCallback(std::function<void(ice::ObjectId)> callback) { signalPicked_ = std::move(callback); }

    void setSignalPickCancelledCallback(std::function<void()> callback) { signalPickCancelled_ = std::move(callback); }

    void setFlipCallback(std::function<void(ice::ObjectId)> callback) { flipped_ = std::move(callback); }

    void setGridVisible(bool visible) {
        gridVisible_ = visible;
        viewport()->update();
    }

    void setSnapToGrid(bool enabled) { snapToGrid_ = enabled; }

    void setSymbolPlacementCallback(std::function<void(QPointF)> callback) {
        symbolPlacementRequested_ = std::move(callback);
    }

    void setToolEscapeCallback(std::function<void()> callback) { toolEscapeRequested_ = std::move(callback); }

    void setPlaceSymbolPreview(const QByteArray &artwork) {
        placeSymbolArtwork_ = artwork;
        viewport()->update();
    }

    void refreshPinVisibility() { updatePinVisibility(); }

    ice::ObjectId selectedInstanceId() const { return selectedInstance_; }

    // Re-applies the selection highlight after the scene has been rebuilt.
    void restoreSelection() {
        if (selectedInstance_ != 0 && !sceneContains(selectedInstance_))
            selectedInstance_ = 0;
        applySelection(selectedInstance_);
    }

    void clearSelection() {
        selectedInstance_ = 0;
        applySelection(0);
        if (symbolSelected_)
            symbolSelected_(0);
    }

    void deleteSelectedSymbols() {
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        if (!ids.empty() && deleteSelected_)
            deleteSelected_(ids);
    }

    void rotateSelectedSymbols() {
        if (!scene() || !rotated_)
            return;
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        for (const auto id : ids)
            rotated_(id);
    }

    void flipSelectedSymbols() {
        if (!scene() || !flipped_)
            return;
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        for (const auto id : ids)
            flipped_(id);
    }

    void fitToContent() {
        QRectF bounds;
        for (auto *item : scene()->items()) {
            const auto kind = item->data(0).toString();
            if (!item->parentItem() && kind != QStringLiteral("canvas-title"))
                bounds |= item->sceneBoundingRect();
        }
        if (!bounds.isValid() || bounds.isNull()) {
            zoom_ = 1.0;
            centerScene_ = QPointF(0.0, 0.0);
            applyNavigationTransform();
            return;
        }
        const qreal width = std::max<qreal>(200.0, bounds.width());
        const qreal height = std::max<qreal>(200.0, bounds.height());
        const auto available = viewport()->size();
        zoom_ = std::clamp(std::min(available.width() / width, available.height() / height) * 0.85, qreal(0.05),
                           qreal(100.0));
        centerScene_ = bounds.center();
        applyNavigationTransform();
    }

    void drawBackground(QPainter *painter, const QRectF &rect) override {
        painter->fillRect(rect, ice::uiColor(ice::UiColor::Background2));
        if (gridVisible_) {
            const qreal step = 20.0;
            QPen minor(ice::uiColor(ice::UiColor::Grid1));
            minor.setWidthF(0.5);
            painter->setPen(minor);
            const int left = static_cast<int>(std::floor(rect.left() / step));
            const int right = static_cast<int>(std::ceil(rect.right() / step));
            const int top = static_cast<int>(std::floor(rect.top() / step));
            const int bottom = static_cast<int>(std::ceil(rect.bottom() / step));
            for (int x = left; x <= right; ++x)
                painter->drawLine(QPointF(x * step, rect.top()), QPointF(x * step, rect.bottom()));
            for (int y = top; y <= bottom; ++y)
                painter->drawLine(QPointF(rect.left(), y * step), QPointF(rect.right(), y * step));
            QPen major(ice::uiColor(ice::UiColor::Grid2));
            major.setWidthF(1.0);
            painter->setPen(major);
            const auto majorStep = step * 5;
            const int majorLeft = static_cast<int>(std::floor(rect.left() / majorStep));
            const int majorRight = static_cast<int>(std::ceil(rect.right() / majorStep));
            const int majorTop = static_cast<int>(std::floor(rect.top() / majorStep));
            const int majorBottom = static_cast<int>(std::ceil(rect.bottom() / majorStep));
            for (int x = majorLeft; x <= majorRight; ++x)
                painter->drawLine(QPointF(x * majorStep, rect.top()), QPointF(x * majorStep, rect.bottom()));
            for (int y = majorTop; y <= majorBottom; ++y)
                painter->drawLine(QPointF(rect.left(), y * majorStep), QPointF(rect.right(), y * majorStep));
        }
        drawCanvasAxes(*painter, rect);
    }

    void drawForeground(QPainter *painter, const QRectF &) override {
        if (!placeSymbolMode_ || placeSymbolArtwork_.isEmpty() || !viewport()->underMouse())
            return;
        QSvgRenderer renderer(placeSymbolArtwork_);
        if (!renderer.isValid())
            return;
        const auto artworkSize = renderer.defaultSize();
        if (artworkSize.isEmpty())
            return;
        const auto center = snapScenePosition(cursorScenePosition_);
        const QRectF target(center.x() - artworkSize.width() / 2.0, center.y() - artworkSize.height() / 2.0,
                            artworkSize.width(), artworkSize.height());
        const qreal ratio = devicePixelRatioF();
        const QSize pixelSize(static_cast<int>(std::ceil(target.width() * ratio)),
                              static_cast<int>(std::ceil(target.height() * ratio)));
        QImage artwork(pixelSize, QImage::Format_ARGB32_Premultiplied);
        artwork.setDevicePixelRatio(ratio);
        artwork.fill(Qt::transparent);
        {
            QPainter artworkPainter(&artwork);
            artworkPainter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&artworkPainter, QRectF(QPointF(0.0, 0.0), target.size()));
        }
        {
            QPainter tintPainter(&artwork);
            tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tintPainter.fillRect(artwork.rect(), ice::uiColor(ice::UiColor::Axis1));
        }
        painter->save();
        painter->setOpacity(0.45);
        painter->drawImage(target.topLeft(), artwork);
        painter->restore();
    }

  protected:
    void showEvent(QShowEvent *event) override {
        QGraphicsView::showEvent(event);
        if (viewInitialized_)
            return;
        // Defer one event-loop turn: the tab/widget layout is finalized after
        // showEvent, and centering earlier leaves the origin slightly offset.
        QTimer::singleShot(0, this, [this] {
            if (viewInitialized_)
                return;
            viewInitialized_ = true;
            applyNavigationTransform();
        });
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
        if (signalPickMode_ && event->button() == Qt::LeftButton) {
            const auto netId = netIdAtPosition(event->pos());
            if (netId != 0) {
                setSignalPickMode(false);
                if (signalPicked_)
                    signalPicked_(netId);
            } else {
                QApplication::beep();
            }
            event->accept();
            return;
        }
        if (placeSymbolMode_ && event->button() == Qt::LeftButton) {
            if (symbolPlacementRequested_)
                symbolPlacementRequested_(snapScenePosition(mapToScene(event->pos())));
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            auto *item = itemAt(event->pos());
            if (netLabelMode_ && (!item || item->data(0).toString() != QStringLiteral("pin"))) {
                if (netLabelPlaced_)
                    netLabelPlaced_(snapScenePosition(mapToScene(event->pos())));
                event->accept();
                return;
            }
            if (item && item->data(0).toString() == QStringLiteral("pin")) {
                wireStart_ = item;
                setPinHighlights(item);
                updatePinVisibility();
                event->accept();
                return;
            }
            if (auto *symbol = symbolOwner(item)) {
                selectSymbol(symbol, event->modifiers().testFlag(Qt::ControlModifier));
                if (symbol->isSelected()) {
                    // The drag is tracked by the stable domain id, so a scene
                    // rebuild during the interaction cannot leave a stale
                    // graphics item behind.
                    dragInstance_ = symbol->data(1).toULongLong();
                    dragOriginScene_ = mapToScene(event->pos());
                    dragOriginPosition_ = symbol->pos();
                    dragged_ = false;
                }
                event->accept();
                return;
            }
            // A press on empty background clears the selection and still
            // starts a rubber-band drag for the view.
            if (selectedInstance_ != 0)
                clearSelection();
        }
        dragInstance_ = 0;
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        cursorScenePosition_ = mapToScene(event->pos());
        updatePinVisibility();
        if (placeSymbolMode_)
            viewport()->update();
        if (panning_) {
            const auto delta = event->pos() - panStartPosition_;
            centerScene_ = panStartCenterScene_ - QPointF(delta.x() / zoom_, delta.y() / zoom_);
            applyNavigationTransform();
            event->accept();
            return;
        }
        if (wireStart_) {
            auto *candidate = itemAt(event->pos());
            highlightCandidate(candidate);
        }
        // A drag follows the symbol item that currently projects the pressed
        // instance; the scene may be rebuilt while dragging.
        QGraphicsItem *dragged = dragInstance_ != 0 ? symbolById(dragInstance_) : nullptr;
        if (dragInstance_ != 0 && !dragged) {
            dragInstance_ = 0;
        } else if (dragged) {
            const auto delta = mapToScene(event->pos()) - dragOriginScene_;
            if (!dragged_ && (std::abs(delta.x()) > 2.0 || std::abs(delta.y()) > 2.0))
                dragged_ = true;
            if (dragged_)
                dragged->setPos(snapScenePosition(dragOriginPosition_ + delta));
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            if (auto *symbol = symbolOwner(itemAt(event->pos()))) {
                if (symbolActivated_)
                    symbolActivated_(symbol->data(1).toULongLong());
                event->accept();
                return;
            }
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() == 0) {
            event->ignore();
            return;
        }
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const auto scenePosition = mapToScene(event->position().toPoint());
            const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
            zoom_ = std::clamp(zoom_ * factor, qreal(0.05), qreal(100.0));
            const auto viewportCenter = QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5);
            const auto cursor = event->position();
            centerScene_ = scenePosition - QPointF((cursor.x() - viewportCenter.x()) / zoom_,
                                                   (cursor.y() - viewportCenter.y()) / zoom_);
        } else {
            const auto delta = event->angleDelta();
            centerScene_ -= QPointF(-delta.x() / zoom_, delta.y() / zoom_);
        }
        applyNavigationTransform();
        cursorScenePosition_ = mapToScene(event->position().toPoint());
        updatePinVisibility();
        if (placeSymbolMode_)
            viewport()->update();
        event->accept();
    }

    void leaveEvent(QEvent *event) override {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("pin"))
                item->setVisible(false);
        QGraphicsView::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (panning_ && event->button() == Qt::MiddleButton) {
            panning_ = false;
            if (placeSymbolMode_)
                setCursor(Qt::CrossCursor);
            else
                unsetCursor();
            event->accept();
            return;
        }
        if (wireStart_) {
            if (event->button() == Qt::LeftButton && itemIsInScene(wireStart_)) {
                auto *end = itemAt(event->pos());
                if (end && end != wireStart_ && end->data(0).toString() == QStringLiteral("pin"))
                    connected_(wireStart_->data(1).toULongLong(), end->data(1).toULongLong());
            }
            wireStart_ = nullptr;
            clearPinHighlights();
            updatePinVisibility();
            event->accept();
            return;
        }
        // The drag ends on any release so the canvas never keeps drag state
        // across interactions that rebuild the scene.
        dragInstance_ = 0;
        QGraphicsView::mouseReleaseEvent(event);
        if (moved_ && scene()) {
            std::vector<std::pair<ice::ObjectId, QPointF>> movedItems;
            for (auto *item : scene()->items()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    movedItems.emplace_back(item->data(1).toULongLong(), item->pos());
            }
            for (const auto &[id, position] : movedItems)
                moved_(id, position);
        }
        if (changed_)
            changed_();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape && signalPickMode_) {
            setSignalPickMode(false);
            if (signalPickCancelled_)
                signalPickCancelled_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && (placeSymbolMode_ || netLabelMode_)) {
            if (toolEscapeRequested_)
                toolEscapeRequested_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_R && scene()) {
            for (auto *item : scene()->selectedItems())
                if (item->data(0).toString() == QStringLiteral("symbol") && rotated_)
                    rotated_(item->data(1).toULongLong());
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && scene()) {
            std::vector<ice::ObjectId> ids;
            for (auto *item : scene()->selectedItems()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    ids.push_back(item->data(1).toULongLong());
            }
            if (!ids.empty() && deleteSelected_)
                deleteSelected_(ids);
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

  public:
    std::function<void(std::vector<ice::ObjectId>)> deleteSelected_;

  private:
    void applyNavigationTransform() {
        setTransform(QTransform(zoom_, 0.0, 0.0, zoom_, 0.0, 0.0), false);
        centerOn(centerScene_);
        viewport()->update();
    }

    void setPinHighlights(QGraphicsItem *source) {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item);
            if (pin)
                pin->setBrush(item == source ? QBrush(ice::uiColor(ice::UiColor::Accent5))
                                             : QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
    }

    void highlightCandidate(QGraphicsItem *candidate) {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item);
            if (!pin)
                continue;
            pin->setBrush(item == candidate && item != wireStart_ ? QBrush(ice::uiColor(ice::UiColor::Accent3))
                                                                  : QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
        if (auto *pin = dynamic_cast<QGraphicsEllipseItem *>(wireStart_))
            pin->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent5)));
    }

    void clearPinHighlights() {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            if (auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item))
                pin->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
    }

    bool sceneContains(ice::ObjectId instanceId) const {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol") && item->data(1).toULongLong() == instanceId)
                return true;
        return false;
    }

    QGraphicsItem *symbolById(ice::ObjectId instanceId) {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol") && item->data(1).toULongLong() == instanceId)
                return item;
        return nullptr;
    }

    // Selection is view state expressed over stable domain ids, so it survives
    // a scene rebuild and can be mirrored by the properties panel.
    void selectSymbol(QGraphicsItem *symbol, bool additive) {
        if (additive && symbol->isSelected()) {
            applySymbolSelectedStyle(symbol, false);
            selectedInstance_ = 0;
            if (symbolSelected_)
                symbolSelected_(0);
            return;
        }
        if (!additive) {
            for (auto *item : scene()->items())
                if (item != symbol && item->data(0).toString() == QStringLiteral("symbol"))
                    applySymbolSelectedStyle(item, false);
        }
        applySymbolSelectedStyle(symbol, true);
        selectedInstance_ = symbol->data(1).toULongLong();
        if (symbolSelected_)
            symbolSelected_(selectedInstance_);
    }

    void applySelection(ice::ObjectId instanceId) {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                applySymbolSelectedStyle(item, item->data(1).toULongLong() == instanceId);
    }

    // Graphics items are rebuilt whenever the scene is refreshed, so every
    // item captured by an in-flight interaction is validated before use.
    bool itemIsInScene(QGraphicsItem *item) const {
        if (!item)
            return false;
        for (auto *candidate : scene()->items())
            if (candidate == item)
                return true;
        return false;
    }

    QPointF snapScenePosition(const QPointF &position) const {
        if (!snapToGrid_)
            return position;
        constexpr qreal step = 20.0;
        return {std::round(position.x() / step) * step, std::round(position.y() / step) * step};
    }

    // Resolve the net under the cursor. Clicking a wire only needs to be near
    // its stroke, not exactly on the two-pixel line.
    ice::ObjectId netIdAtPosition(const QPoint &position) const {
        if (auto *item = itemAt(position)) {
            const auto kind = item->data(0).toString();
            if (kind == QStringLiteral("wire") || kind == QStringLiteral("net-label"))
                return item->data(1).toULongLong();
            if (kind == QStringLiteral("pin"))
                return item->data(3).toULongLong();
        }
        const auto scenePosition = mapToScene(position);
        const qreal tolerance = std::max(6.0 / std::max(zoom_, qreal(0.0001)), 3.0);
        const QRectF searchArea(scenePosition.x() - tolerance, scenePosition.y() - tolerance, tolerance * 2.0,
                                tolerance * 2.0);
        for (auto *candidate : scene()->items(searchArea)) {
            if (candidate->data(0).toString() != QStringLiteral("wire"))
                continue;
            auto *wire = dynamic_cast<QGraphicsPathItem *>(candidate);
            if (!wire)
                continue;
            QPainterPathStroker stroker;
            stroker.setWidth(tolerance * 2.0);
            const auto hitArea = stroker.createStroke(wire->path());
            if (hitArea.contains(wire->mapFromScene(scenePosition)))
                return wire->data(1).toULongLong();
        }
        return 0;
    }

    void updatePinVisibility() {
        const bool selectTool = !placeSymbolMode_ && !netLabelMode_;
        const bool showAll = selectTool && (wireStart_ != nullptr || signalPickMode_);
        const bool cursorOver = viewport()->underMouse();
        const qreal radius = 12.0 / std::max(zoom_, qreal(0.0001));
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            const bool visible =
                selectTool &&
                (showAll || (cursorOver && QLineF(cursorScenePosition_, item->scenePos()).length() <= radius));
            item->setVisible(visible);
        }
    }

    bool panning_ = false;
    QPoint panStartPosition_;
    QPointF panStartCenterScene_;
    QGraphicsItem *wireStart_ = nullptr;
    ice::ObjectId dragInstance_ = 0;
    QPointF dragOriginScene_;
    QPointF dragOriginPosition_;
    bool dragged_ = false;
    std::function<void(ice::ObjectId, QPointF)> moved_;
    std::function<void(ice::ObjectId)> rotated_;
    std::function<void(ice::ObjectId)> flipped_;
    std::function<void(ice::ObjectId, ice::ObjectId)> connected_;
    std::function<void(QPointF)> netLabelPlaced_;
    std::function<void()> changed_;
    std::function<void(ice::ObjectId)> symbolSelected_;
    std::function<void(ice::ObjectId)> symbolActivated_;
    std::function<void(ice::ObjectId)> signalPicked_;
    std::function<void()> signalPickCancelled_;
    bool netLabelMode_ = false;
    bool placeSymbolMode_ = false;
    bool signalPickMode_ = false;
    bool gridVisible_ = true;
    bool snapToGrid_ = true;
    QByteArray placeSymbolArtwork_;
    QPointF cursorScenePosition_;
    std::function<void(QPointF)> symbolPlacementRequested_;
    std::function<void()> toolEscapeRequested_;
    ice::ObjectId selectedInstance_ = 0;
    bool viewInitialized_ = false;
    qreal zoom_ = 1.0;
    QPointF centerScene_;
};
SchematicCanvas *createSchematicCanvas(QWidget *parent) { return new SchematicCanvasImpl(parent); }
