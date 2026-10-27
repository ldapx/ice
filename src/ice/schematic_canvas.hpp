#pragma once

#include "icelib/core.hpp"

#include <QByteArray>
#include <QGraphicsView>
#include <QPointF>

#include <functional>
#include <vector>

class SchematicCanvas : public QGraphicsView {
  public:
    explicit SchematicCanvas(QWidget *parent = nullptr);
    ~SchematicCanvas() override;

    virtual void setCallbacks(std::function<void(ice::ObjectId, QPointF)> moved,
                              std::function<void(ice::ObjectId)> rotated,
                              std::function<void(ice::ObjectId, ice::ObjectId)> connected,
                              std::function<void(QPointF)> netLabelPlaced, std::function<void()> changed,
                              std::function<void(ice::ObjectId)> symbolSelected = {},
                              std::function<void(ice::ObjectId)> symbolActivated = {}) = 0;
    virtual void setNetLabelMode(bool enabled) = 0;
    virtual void setPlaceSymbolMode(bool enabled) = 0;
    virtual bool isSignalPickMode() const = 0;
    virtual void setSignalPickMode(bool enabled) = 0;
    virtual void setSignalPickedCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual void setSignalPickCancelledCallback(std::function<void()> callback) = 0;
    virtual void setFlipCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual void setGridVisible(bool visible) = 0;
    virtual void setSnapToGrid(bool enabled) = 0;
    virtual void setSymbolPlacementCallback(std::function<void(QPointF)> callback) = 0;
    virtual void setToolEscapeCallback(std::function<void()> callback) = 0;
    virtual void setDeleteCallback(std::function<void(std::vector<ice::ObjectId>)> callback) = 0;
    virtual void setDisconnectCallback(std::function<void(ice::ObjectId)> callback) = 0;
    virtual void setNetLabelMovedCallback(std::function<void(ice::ObjectId, QPointF)> callback) = 0;
    virtual void setPlaceSymbolPreview(const QByteArray &artwork) = 0;
    virtual void refreshPinVisibility() = 0;
    virtual ice::ObjectId selectedInstanceId() const = 0;
    virtual void restoreSelection() = 0;
    virtual void clearSelection() = 0;
    virtual void deleteSelectedSymbols() = 0;
    virtual void rotateSelectedSymbols() = 0;
    virtual void flipSelectedSymbols() = 0;
    virtual void fitToContent() = 0;
};

SchematicCanvas *createSchematicCanvas(QWidget *parent = nullptr);
