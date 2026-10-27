#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "ui_helpers.hpp"

#include <QGraphicsView>
#include <QPointF>

#include <functional>
#include <map>
#include <vector>

class QPainter;
class QRectF;
class QLineEdit;

class LayoutCanvas : public QGraphicsView {
  public:
    enum class Tool { select, rectangle, placeDevice };

    explicit LayoutCanvas(QWidget *parent = nullptr);
    ~LayoutCanvas() override;

    virtual void setTool(Tool tool) = 0;
    virtual void setReadOnly(bool readOnly) = 0;
    virtual bool isReadOnly() const = 0;
    virtual Tool currentTool() const = 0;
    virtual void setGridVisible(bool visible) = 0;
    virtual void setSnapToGrid(bool enabled) = 0;
    virtual void setGridSpacing(qreal spacing) = 0;
    virtual void setDevicePreview(const ice::LayoutDeviceCell *cell) = 0;
    virtual void restoreLayoutInstanceSelection() = 0;
    virtual void setDevicePlacementCallback(std::function<void(QPointF)> callback) = 0;
    virtual void setInstanceActivatedCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual void setInstanceSelectionCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual ice::ObjectId selectedLayoutInstance() const = 0;
    virtual void setToolEscapeCallback(std::function<void()> callback) = 0;
    virtual void deleteSelectedShapes() = 0;
    virtual void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                              std::function<void(ice::ObjectId, QPointF)> shapeMoved,
                              std::function<void(std::vector<ice::ObjectId>)> deleted,
                              std::function<void()> changed) = 0;
    virtual void setLayerStyles(const std::map<ice::ObjectId, LayerStyle> &styles) = 0;
    virtual void fitToContent() = 0;
};

LayoutCanvas *createLayoutCanvas(QWidget *parent = nullptr);
