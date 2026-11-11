#include "schematic_canvas.hpp"

#include "icons.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QApplication>
#include <QBrush>
#include <QCursor>
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
#include <set>

SchematicCanvas::SchematicCanvas(QWidget *parent) : QGraphicsView(parent) {}
SchematicCanvas::~SchematicCanvas() = default;

namespace {

bool isSelectableSchematicItem(const QGraphicsItem *item) {
    if (!item)
        return false;
    const auto kind = item->data(0).toString();
    return kind == QStringLiteral("symbol") || kind == QStringLiteral("comment");
}

QGraphicsItem *schematicItemOwner(QGraphicsItem *item) {
    for (auto *cursor = item; cursor; cursor = cursor->parentItem())
        if (isSelectableSchematicItem(cursor))
            return cursor;
    return nullptr;
}

QGraphicsItem *symbolChild(QGraphicsItem *symbol, const QString &kind) {
    if (!symbol)
        return nullptr;
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

// Comment placement shows the toolbar's comment glyph as the cursor so the
// canvas makes the pending click obvious. Falls back to the native I-beam when
// the icon asset is unavailable. Rebuilt per use so theme changes are picked up.
QCursor commentPlacementCursor() {
    const auto pixmap = ice::lucideIcon(QStringLiteral("message-square"), 20).pixmap(QSize(20, 20));
    return pixmap.isNull() ? QCursor(Qt::IBeamCursor) : QCursor(pixmap);
}

void applySymbolSelectedStyle(QGraphicsItem *symbol, bool selected) {
    if (!symbol)
        return;
    if (symbol->isSelected() != selected)
        symbol->setSelected(selected);
    const bool comment = symbolChild(symbol, QStringLiteral("comment-text")) != nullptr;
    if (auto *highlight = symbolChild(symbol, QStringLiteral("symbol-highlight")))
        highlight->setVisible(selected && comment);
    if (auto *label = symbolChild(symbol, QStringLiteral("symbol-label")))
        static_cast<QGraphicsTextItem *>(label)->setDefaultTextColor(selected ? ice::uiColor(ice::UiColor::Accent4)
                                                                              : ice::uiColor(ice::UiColor::Axis1));
    for (auto *child : symbol->childItems()) {
        if (child->data(0).toString() == QStringLiteral("symbol-value"))
            static_cast<QGraphicsTextItem *>(child)->setDefaultTextColor(selected ? ice::uiColor(ice::UiColor::Accent4)
                                                                                  : ice::uiColor(ice::UiColor::Text1));
    }
    if (auto *commentText = symbolChild(symbol, QStringLiteral("comment-text")))
        static_cast<QGraphicsTextItem *>(commentText)
            ->setDefaultTextColor(selected ? ice::uiColor(ice::UiColor::Accent4) : ice::uiColor(ice::UiColor::Text1));
    if (auto *effect = symbolArtworkEffect(symbol)) {
        const auto hovered = symbol->data(4).toBool();
        effect->setColor(hovered
                             ? ice::uiColor(ice::UiColor::Accent5)
                             : (selected ? ice::uiColor(ice::UiColor::Accent4) : ice::uiColor(ice::UiColor::Axis1)));
        effect->setEnabled(true);
    }
}

void applySymbolHoverStyle(QGraphicsItem *symbol, bool hovered) {
    if (!symbol)
        return;
    symbol->setData(4, hovered);
    if (auto *effect = symbolArtworkEffect(symbol)) {
        effect->setColor(
            hovered ? ice::uiColor(ice::UiColor::Accent5)
                    : (symbol->isSelected() ? ice::uiColor(ice::UiColor::Accent4) : ice::uiColor(ice::UiColor::Axis1)));
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
        setRubberBandSelectionMode(Qt::IntersectsItemBoundingRect);
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
                      std::function<void(QPointF)> commentPlaced, std::function<void()> changed,
                      std::function<void(ice::ObjectId)> symbolSelected = {},
                      std::function<void(ice::ObjectId)> symbolActivated = {}) override {
        moved_ = std::move(moved);
        rotated_ = std::move(rotated);
        connected_ = std::move(connected);
        commentPlaced_ = std::move(commentPlaced);
        changed_ = std::move(changed);
        symbolSelected_ = std::move(symbolSelected);
        symbolActivated_ = std::move(symbolActivated);
    }

    void setDeleteCallback(std::function<void(std::vector<ice::ObjectId>)> callback) override {
        deleteSelected_ = std::move(callback);
    }

    void setDisconnectCallback(std::function<void(ice::ObjectId)> callback) override {
        disconnected_ = std::move(callback);
    }

    void setNetLabelMovedCallback(
        std::function<void(ice::ObjectId, const ice::NetLabelAnchor &, QPointF)> callback) override {
        netLabelMoved_ = std::move(callback);
    }

    void setNetLabelActivatedCallback(std::function<void(ice::ObjectId)> callback) override {
        netLabelActivated_ = std::move(callback);
    }

    void setNetLabelMode(bool enabled) override { setCommentMode(enabled); }

    bool isCommentMode() const override { return commentMode_; }

    void setCommentMode(bool enabled) override {
        commentMode_ = enabled;
        if (enabled) {
            placeSymbolMode_ = false;
            signalPickMode_ = false;
            setCursor(commentPlacementCursor());
        } else if (!placeSymbolMode_ && !signalPickMode_) {
            unsetCursor();
        }
        updatePinVisibility();
    }

    void setPlaceSymbolMode(bool enabled) override {
        placeSymbolMode_ = enabled;
        if (enabled) {
            commentMode_ = false;
            signalPickMode_ = false;
            setCursor(Qt::CrossCursor);
        } else if (!commentMode_ && !signalPickMode_) {
            unsetCursor();
        }
        updatePinVisibility();
    }

    bool isSignalPickMode() const override { return signalPickMode_; }

    void setSignalPickMode(bool enabled) override {
        signalPickMode_ = enabled;
        if (enabled) {
            commentMode_ = false;
            placeSymbolMode_ = false;
            setCursor(Qt::CrossCursor);
        } else if (!placeSymbolMode_ && !commentMode_) {
            unsetCursor();
        }
        updatePinVisibility();
    }

    void setSignalPickedCallback(std::function<void(ice::ObjectId)> callback) override {
        signalPicked_ = std::move(callback);
    }

    void setSignalPickCancelledCallback(std::function<void()> callback) override {
        signalPickCancelled_ = std::move(callback);
    }

    void setFlipCallback(std::function<void(ice::ObjectId)> callback) override { flipped_ = std::move(callback); }

    void setGridVisible(bool visible) override {
        gridVisible_ = visible;
        viewport()->update();
    }

    void setSnapToGrid(bool enabled) override { snapToGrid_ = enabled; }

    void setSymbolPlacementCallback(std::function<void(QPointF)> callback) override {
        symbolPlacementRequested_ = std::move(callback);
    }

    void setPlaceSymbolCycleCallback(std::function<void(int)> callback) override {
        placeSymbolCycleRequested_ = std::move(callback);
    }

    void setToolEscapeCallback(std::function<void()> callback) override { toolEscapeRequested_ = std::move(callback); }

    void setPlaceSymbolPreview(const QByteArray &artwork) override {
        placeSymbolArtwork_ = artwork;
        viewport()->update();
    }

    void refreshPinVisibility() override { updatePinVisibility(); }

    ice::ObjectId selectedInstanceId() const override {
        return selectedInstances_.size() == 1 ? *selectedInstances_.begin() : 0;
    }

    std::vector<ice::ObjectId> selectedInstanceIds() const override {
        return std::vector<ice::ObjectId>(selectedInstances_.begin(), selectedInstances_.end());
    }

    void restoreSelection() override {
        for (auto it = selectedInstances_.begin(); it != selectedInstances_.end();) {
            if (!sceneContains(*it))
                it = selectedInstances_.erase(it);
            else
                ++it;
        }
        applySelection(selectedInstances_);
        if (symbolSelected_)
            symbolSelected_(selectedInstanceId());
    }

    void clearSelection() override {
        selectedInstances_.clear();
        applySelection({});
        if (symbolSelected_)
            symbolSelected_(0);
    }

    void selectInstance(ice::ObjectId instanceId) override { selectSymbol(itemById(instanceId), false); }

    void deleteSelectedSymbols() override {
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (isSelectableSchematicItem(item))
                ids.push_back(item->data(1).toULongLong());
        if (ids.empty()) {
            for (auto id : selectedInstances_)
                ids.push_back(id);
        }
        if (!ids.empty() && deleteSelected_)
            deleteSelected_(ids);
    }

    void rotateSelectedSymbols() override {
        if (!scene() || !rotated_)
            return;
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        if (ids.empty()) {
            for (auto id : selectedInstances_)
                if (auto *item = itemById(id); item && item->data(0).toString() == QStringLiteral("symbol"))
                    ids.push_back(id);
        }
        for (const auto id : ids)
            rotated_(id);
    }

    void flipSelectedSymbols() override {
        if (!scene() || !flipped_)
            return;
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        if (ids.empty()) {
            for (auto id : selectedInstances_)
                if (auto *item = itemById(id); item && item->data(0).toString() == QStringLiteral("symbol"))
                    ids.push_back(id);
        }
        for (const auto id : ids)
            flipped_(id);
    }

    void fitToContent() override {
        QRectF bounds;
        for (auto *item : scene()->items()) {
            if (!item->parentItem())
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
            const auto kind = item ? item->data(0).toString() : QString();
            if (commentMode_ && kind != QStringLiteral("pin") && kind != QStringLiteral("net-label")) {
                if (commentPlaced_)
                    commentPlaced_(snapScenePosition(mapToScene(event->pos())));
                event->accept();
                return;
            }
            if (kind == QStringLiteral("pin")) {
                wireStart_ = item;
                setPinHighlights(item);
                updatePinVisibility();
                event->accept();
                return;
            }
            if (kind == QStringLiteral("net-label")) {
                netLabelDrag_ = item;
                netLabelDragNetId_ = item->data(1).toULongLong();
                netLabelDragOriginScene_ = mapToScene(event->pos());
                netLabelDragOriginPosition_ = item->pos();
                netLabelDragCandidate_ = netLabelDragOriginPosition_;
                event->accept();
                return;
            }
            if (auto *target = schematicItemOwner(item)) {
                const auto targetId = target->data(1).toULongLong();
                const bool additive =
                    event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::ShiftModifier);
                if (additive) {
                    selectSymbol(target, true);
                } else if (!selectedInstances_.contains(targetId)) {
                    selectSymbol(target, false);
                }
                if (selectedInstances_.contains(targetId)) {
                    dragOriginScene_ = mapToScene(event->pos());
                    dragOriginPositions_.clear();
                    for (auto id : selectedInstances_) {
                        if (auto *selItem = itemById(id))
                            dragOriginPositions_[id] = selItem->pos();
                    }
                    dragged_ = false;
                }
                event->accept();
                return;
            }
            // A press on empty background clears the selection and starts a rubber-band drag
            if (!event->modifiers().testFlag(Qt::ControlModifier) && !event->modifiers().testFlag(Qt::ShiftModifier)) {
                if (!selectedInstances_.empty())
                    clearSelection();
            }
        }
        dragOriginPositions_.clear();
        dragged_ = false;
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        cursorScenePosition_ = mapToScene(event->pos());
        updatePinVisibility();
        updateHover(event->pos());
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
        if (netLabelDrag_) {
            if (!itemIsInScene(netLabelDrag_)) {
                netLabelDrag_ = nullptr;
                netLabelDragNetId_ = 0;
            } else {
                const auto delta = mapToScene(event->pos()) - netLabelDragOriginScene_;
                netLabelDragCandidate_ = netLabelDragOriginPosition_ + delta;
                netLabelDrag_->setPos(clampNetLabelPosition(netLabelDragNetId_, netLabelDrag_, netLabelDragCandidate_));
                event->accept();
                return;
            }
        }
        if (!dragOriginPositions_.empty()) {
            const auto delta = mapToScene(event->pos()) - dragOriginScene_;
            if (!dragged_ && (std::abs(delta.x()) > 2.0 || std::abs(delta.y()) > 2.0))
                dragged_ = true;
            if (dragged_) {
                for (const auto &[id, originPos] : dragOriginPositions_) {
                    if (auto *draggedItem = itemById(id))
                        draggedItem->setPos(snapScenePosition(originPos + delta));
                }
            }
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            if (auto *item = itemAt(event->pos()); item && item->data(0).toString() == QStringLiteral("net-label")) {
                if (netLabelActivated_)
                    netLabelActivated_(item->data(1).toULongLong());
                event->accept();
                return;
            }
            if (auto *item = schematicItemOwner(itemAt(event->pos()))) {
                if (symbolActivated_)
                    symbolActivated_(item->data(1).toULongLong());
                event->accept();
                return;
            }
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        if (placeSymbolMode_ && event->modifiers().testFlag(Qt::ShiftModifier)) {
            // Some platforms transpose the wheel deltas while Shift is held.
            const int steps = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
            if (steps != 0 && placeSymbolCycleRequested_)
                placeSymbolCycleRequested_(steps > 0 ? 1 : -1);
            event->accept();
            return;
        }
        if (event->angleDelta().y() == 0) {
            event->ignore();
            return;
        }
        const auto scenePosition = mapToScene(event->position().toPoint());
        const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        zoom_ = std::clamp(zoom_ * factor, qreal(0.05), qreal(100.0));
        const auto viewportCenter = QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5);
        const auto cursor = event->position();
        centerScene_ = scenePosition -
                       QPointF((cursor.x() - viewportCenter.x()) / zoom_, (cursor.y() - viewportCenter.y()) / zoom_);
        applyNavigationTransform();
        cursorScenePosition_ = mapToScene(event->position().toPoint());
        updatePinVisibility();
        if (placeSymbolMode_)
            viewport()->update();
        event->accept();
    }

    void leaveEvent(QEvent *event) override {
        clearHover();
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
            else if (commentMode_)
                setCursor(commentPlacementCursor());
            else
                unsetCursor();
            event->accept();
            return;
        }
        if (netLabelDrag_) {
            if (event->button() == Qt::LeftButton && netLabelDragNetId_ != 0 && itemIsInScene(netLabelDrag_)) {
                const auto position = netLabelDrag_->pos();
                if (netLabelMoved_ && position != netLabelDragOriginPosition_) {
                    if (const auto projection =
                            projectNetLabel(netLabelDragNetId_, netLabelDrag_, netLabelDragCandidate_))
                        netLabelMoved_(netLabelDragNetId_, projection->anchor, projection->position);
                }
            }
            netLabelDrag_ = nullptr;
            netLabelDragNetId_ = 0;
            event->accept();
            return;
        }
        if (wireStart_) {
            if (event->button() == Qt::LeftButton && itemIsInScene(wireStart_)) {
                auto *end = itemAt(event->pos());
                if (end && end != wireStart_ && end->data(0).toString() == QStringLiteral("pin"))
                    connected_(wireStart_->data(1).toULongLong(), end->data(1).toULongLong());
                else if (disconnected_)
                    disconnected_(wireStart_->data(1).toULongLong());
            }
            wireStart_ = nullptr;
            clearPinHighlights();
            updatePinVisibility();
            event->accept();
            return;
        }
        if (!dragOriginPositions_.empty()) {
            if (dragged_) {
                std::vector<std::pair<ice::ObjectId, QPointF>> movedItems;
                for (const auto &[id, originPos] : dragOriginPositions_) {
                    if (auto *draggedItem = itemById(id)) {
                        if (draggedItem->pos() != originPos)
                            movedItems.emplace_back(id, draggedItem->pos());
                    }
                }
                for (const auto &[id, position] : movedItems) {
                    if (moved_)
                        moved_(id, position);
                }
                if (!movedItems.empty() && changed_)
                    changed_();
            } else if (event->button() == Qt::LeftButton) {
                if (!event->modifiers().testFlag(Qt::ControlModifier) &&
                    !event->modifiers().testFlag(Qt::ShiftModifier)) {
                    if (auto *clicked = schematicItemOwner(itemAt(event->pos()))) {
                        const auto clickedId = clicked->data(1).toULongLong();
                        if (selectedInstances_.size() > 1 && selectedInstances_.contains(clickedId))
                            selectSymbol(clicked, false);
                    }
                }
            }
            dragOriginPositions_.clear();
            dragged_ = false;
            event->accept();
            return;
        }

        // Complete any rubber band drag
        QGraphicsView::mouseReleaseEvent(event);
        syncSelectionFromScene();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape && signalPickMode_) {
            setSignalPickMode(false);
            if (signalPickCancelled_)
                signalPickCancelled_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && (placeSymbolMode_ || commentMode_)) {
            if (toolEscapeRequested_)
                toolEscapeRequested_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape && !selectedInstances_.empty()) {
            clearSelection();
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
            if (isSelectableSchematicItem(item) && item->data(1).toULongLong() == instanceId)
                return true;
        return false;
    }

    QGraphicsItem *itemById(ice::ObjectId instanceId) {
        for (auto *item : scene()->items())
            if (isSelectableSchematicItem(item) && item->data(1).toULongLong() == instanceId)
                return item;
        return nullptr;
    }

    QGraphicsItem *symbolById(ice::ObjectId instanceId) { return itemById(instanceId); }

    void setNetHovered(ice::ObjectId netId, bool hovered) {
        if (netId == 0 || !scene())
            return;
        const auto color = ice::uiColor(hovered ? ice::UiColor::Accent5 : ice::UiColor::Accent4);
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("wire") || item->data(1).toULongLong() != netId)
                continue;
            if (auto *wire = dynamic_cast<QGraphicsPathItem *>(item))
                wire->setPen(QPen(color, 2));
        }
    }

    void updateHover(const QPoint &position) {
        if (!scene())
            return;
        auto *owner = schematicItemOwner(itemAt(position));
        ice::ObjectId instanceId = 0;
        if (owner && owner->data(0).toString() == QStringLiteral("symbol"))
            instanceId = owner->data(1).toULongLong();
        if (instanceId != hoveredInstanceId_) {
            if (auto *previous = itemById(hoveredInstanceId_))
                applySymbolHoverStyle(previous, false);
            hoveredInstanceId_ = instanceId;
            if (auto *current = itemById(hoveredInstanceId_))
                applySymbolHoverStyle(current, true);
        }

        const auto netId = netIdAtPosition(position);
        if (netId != hoveredNetId_) {
            setNetHovered(hoveredNetId_, false);
            hoveredNetId_ = netId;
            setNetHovered(hoveredNetId_, true);
        }
    }

    void clearHover() {
        if (auto *previous = itemById(hoveredInstanceId_))
            applySymbolHoverStyle(previous, false);
        hoveredInstanceId_ = 0;
        setNetHovered(hoveredNetId_, false);
        hoveredNetId_ = 0;
    }

    // Selection is view state expressed over stable domain ids, so it survives
    // a scene rebuild and can be mirrored by the properties panel.
    void selectSymbol(QGraphicsItem *symbol, bool additive) {
        if (!symbol)
            return;
        const auto id = symbol->data(1).toULongLong();
        if (additive) {
            if (selectedInstances_.contains(id)) {
                selectedInstances_.erase(id);
                applySymbolSelectedStyle(symbol, false);
            } else {
                selectedInstances_.insert(id);
                applySymbolSelectedStyle(symbol, true);
            }
        } else {
            selectedInstances_.clear();
            selectedInstances_.insert(id);
            for (auto *item : scene()->items()) {
                if (isSelectableSchematicItem(item))
                    applySymbolSelectedStyle(item, item == symbol);
            }
        }
        if (symbolSelected_)
            symbolSelected_(selectedInstanceId());
    }

    void applySelection(const std::set<ice::ObjectId> &instances) {
        updatingSelection_ = true;
        for (auto *item : scene()->items()) {
            if (isSelectableSchematicItem(item)) {
                const auto id = item->data(1).toULongLong();
                applySymbolSelectedStyle(item, instances.contains(id));
            }
        }
        updatingSelection_ = false;
    }

    void syncSelectionFromScene() {
        if (updatingSelection_ || !scene())
            return;
        updatingSelection_ = true;
        selectedInstances_.clear();
        for (auto *item : scene()->items()) {
            if (isSelectableSchematicItem(item)) {
                const bool sel = item->isSelected();
                applySymbolSelectedStyle(item, sel);
                if (sel)
                    selectedInstances_.insert(item->data(1).toULongLong());
            }
        }
        if (symbolSelected_)
            symbolSelected_(selectedInstanceId());
        updatingSelection_ = false;
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

    struct NetLabelProjection {
        QPointF position;
        ice::NetLabelAnchor anchor;
    };

    // Net labels stay beside the wire they name, on whichever side the drag
    // requests. The requested position is projected onto the nearest branch
    // segment, then offset perpendicular to the line by half the label size so
    // it never covers the wire. The branch endpoints, normalized branch
    // distance, and side are returned so the caller can store a route-relative
    // anchor instead of an absolute position.
    std::optional<NetLabelProjection> projectNetLabel(ice::ObjectId netId, const QGraphicsItem *label,
                                                      const QPointF &candidate) const {
        const auto bounds = label ? label->boundingRect() : QRectF(0.0, 0.0, 80.0, 18.0);
        const qreal width = bounds.width() > 0.0 ? bounds.width() : 80.0;
        const qreal height = bounds.height() > 0.0 ? bounds.height() : 18.0;
        const QPointF labelCenter = bounds.center();
        const QPointF candidateCenter = candidate + labelCenter;
        constexpr qreal margin = 2.0;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        std::optional<NetLabelProjection> best;
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("wire") || item->data(1).toULongLong() != netId)
                continue;
            auto *wire = dynamic_cast<QGraphicsPathItem *>(item);
            if (!wire)
                continue;
            const auto fromPin = item->data(4).toULongLong();
            const auto toPin = item->data(5).toULongLong();
            if (fromPin == 0 || toPin == 0 || fromPin == toPin)
                continue;
            const auto path = wire->path();
            std::vector<QPointF> points;
            points.reserve(static_cast<std::size_t>(path.elementCount()));
            for (int index = 0; index < path.elementCount(); ++index) {
                const auto element = path.elementAt(index);
                points.push_back(wire->mapToScene(QPointF(element.x, element.y)));
            }
            if (points.size() < 2)
                continue;
            std::vector<qreal> cumulative(points.size(), 0.0);
            for (std::size_t index = 1; index < points.size(); ++index)
                cumulative[index] = cumulative[index - 1] + QLineF(points[index - 1], points[index]).length();
            const auto totalLength = cumulative.back();
            if (totalLength <= 0.0)
                continue;
            for (std::size_t index = 1; index < points.size(); ++index) {
                const auto start = points[index - 1];
                const auto end = points[index];
                const auto delta = end - start;
                const auto lengthSquared = QPointF::dotProduct(delta, delta);
                if (lengthSquared <= 0.0)
                    continue;
                const auto length = std::sqrt(lengthSquared);
                const auto segmentFraction = std::clamp(
                    QPointF::dotProduct(candidateCenter - start, delta) / lengthSquared, qreal(0.0), qreal(1.0));
                const auto point = start + delta * segmentFraction;
                const QPointF normal(-delta.y() / length, delta.x() / length);
                const qreal side = QPointF::dotProduct(candidateCenter - point, normal) < 0.0 ? -1.0 : 1.0;
                const qreal clearance =
                    std::abs(normal.x()) * width / 2.0 + std::abs(normal.y()) * height / 2.0 + margin;
                const auto anchored = point + normal * side * clearance - labelCenter;
                const auto distance = QLineF(candidate, anchored).length();
                if (distance >= bestDistance)
                    continue;
                bestDistance = distance;
                best = NetLabelProjection{
                    anchored,
                    ice::NetLabelAnchor{static_cast<ice::ObjectId>(fromPin), static_cast<ice::ObjectId>(toPin),
                                        std::clamp((cumulative[index - 1] + length * segmentFraction) / totalLength,
                                                   qreal(0.0), qreal(1.0)),
                                        side > 0.0}};
            }
        }
        return best;
    }

    QPointF clampNetLabelPosition(ice::ObjectId netId, const QGraphicsItem *label, const QPointF &candidate) const {
        if (const auto projection = projectNetLabel(netId, label, candidate))
            return projection->position;
        return candidate;
    }

    void updatePinVisibility() {
        const bool selectTool = !placeSymbolMode_ && !commentMode_;
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
    QGraphicsItem *netLabelDrag_ = nullptr;
    ice::ObjectId netLabelDragNetId_ = 0;
    QPointF netLabelDragOriginScene_;
    QPointF netLabelDragOriginPosition_;
    QPointF netLabelDragCandidate_;
    std::map<ice::ObjectId, QPointF> dragOriginPositions_;
    QPointF dragOriginScene_;
    bool dragged_ = false;
    std::function<void(ice::ObjectId, QPointF)> moved_;
    std::function<void(ice::ObjectId)> rotated_;
    std::function<void(ice::ObjectId)> flipped_;
    std::function<void(ice::ObjectId, ice::ObjectId)> connected_;
    std::function<void(ice::ObjectId)> disconnected_;
    std::function<void(ice::ObjectId, const ice::NetLabelAnchor &, QPointF)> netLabelMoved_;
    std::function<void(ice::ObjectId)> netLabelActivated_;
    std::function<void(QPointF)> commentPlaced_;
    std::function<void()> changed_;
    std::function<void(ice::ObjectId)> symbolSelected_;
    std::function<void(ice::ObjectId)> symbolActivated_;
    std::function<void(ice::ObjectId)> signalPicked_;
    std::function<void()> signalPickCancelled_;
    bool commentMode_ = false;
    bool placeSymbolMode_ = false;
    bool signalPickMode_ = false;
    bool gridVisible_ = true;
    bool snapToGrid_ = true;
    QByteArray placeSymbolArtwork_;
    QPointF cursorScenePosition_;
    std::function<void(QPointF)> symbolPlacementRequested_;
    std::function<void(int)> placeSymbolCycleRequested_;
    std::function<void()> toolEscapeRequested_;
    std::set<ice::ObjectId> selectedInstances_;
    ice::ObjectId hoveredInstanceId_ = 0;
    ice::ObjectId hoveredNetId_ = 0;
    bool updatingSelection_ = false;
    bool viewInitialized_ = false;
    qreal zoom_ = 1.0;
    QPointF centerScene_;
};
} // namespace

SchematicCanvas *createSchematicCanvas(QWidget *parent) { return new SchematicCanvasImpl(parent); }
