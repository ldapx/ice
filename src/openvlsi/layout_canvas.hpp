#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/layout_library.hpp"
#include "layout_render.hpp"
#include "ui_helpers.hpp"

#include <QColor>
#include <QGraphicsView>
#include <QPointF>
#include <QPolygonF>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class QGraphicsItem;
class QLineF;
class QPainter;
class QRectF;
class QLineEdit;

struct LayoutPathLayer {
    openvlsi::ObjectId layer = 0;
    openvlsi::ObjectId viaToNext = 0;
    std::string name;
    // PDK minimum drawn width in database units. Zero falls back to the path
    // tool's previous/default width.
    int minimumWidth = 0;
};

struct LayoutPathConfig {
    std::vector<LayoutPathLayer> layers;
    int width = 200;
    openvlsi::ObjectId cell = 0;
};

// Complete path produced by the path tool. Widths, segment layers, and point
// vias are indexed like a layout path: one width/layer per segment and one via
// list per point.
struct LayoutPathData {
    std::vector<QPointF> points;
    std::vector<int> widths;
    std::vector<openvlsi::ObjectId> segmentLayers;
    std::vector<std::vector<openvlsi::ObjectId>> pointVias;
};
using LayoutPathCreatedCallback = std::function<void(const LayoutPathData &path)>;
using LayoutPathLayerChangedCallback = std::function<void(openvlsi::ObjectId layer, const std::string &name)>;
using LayoutPathPointSelectionChangedCallback = std::function<void()>;
using LayoutPathPointsMovedCallback =
    std::function<void(openvlsi::ObjectId id, const std::vector<int> &indices, const std::vector<QPointF> &positions)>;
using LayoutPathPointInsertedCallback = std::function<void(openvlsi::ObjectId id, int index, QPointF position)>;
using LayoutPathPointsDeletedCallback = std::function<void(openvlsi::ObjectId id, const std::vector<int> &indices)>;
using LayoutTextPlacedCallback = std::function<void(QPointF)>;
using LayoutTextPolygonPlacedCallback = std::function<void(QPointF)>;
using LayoutPortPlacedCallback = std::function<void(QPointF, QPointF)>;
using LayoutInstancePortActivatedCallback = std::function<void(openvlsi::ObjectId instance, openvlsi::ObjectId port)>;
using LayoutPlannedDeviceActivatedCallback = std::function<void(openvlsi::ObjectId device)>;
using LayoutPlannedConnectionActivatedCallback = std::function<void(openvlsi::ObjectId connection)>;
using LayoutPolygonCreatedCallback = std::function<void(const std::vector<QPointF> &points)>;
using LayoutPolygonPointMovedCallback = std::function<void(openvlsi::ObjectId id, int index, QPointF position)>;

// Selection and view state that survives a full scene rebuild. Figure ids are
// stable project ids; instance ids are tracked separately because instance
// groups are not scene-selected the same way as shapes and ports.
struct LayoutSelectionState {
    std::vector<openvlsi::ObjectId> figureIds;
    openvlsi::ObjectId instance = 0;
    openvlsi::ObjectId pathShape = 0;
    std::vector<int> pathPoints;
    QPointF center;
    qreal zoom = 1.0;
};

class LayoutCanvas : public QGraphicsView {
  public:
    enum class Tool { select, move, path, rectangle, polygon, port, placeDevice, ruler, text, textPolygon };
    // Entries of the right-click menu shown on a selected layout figure.
    enum class FigureContextAction {
        traceNet,
        rotate,
        flip,
        flattenPath,
        locateSchematicCounterpart,
        bindToSchematicDevice,
        clearSchematicBinding,
        duplicate,
        createArray,
        alignLeft,
        alignRight,
        alignTop,
        alignBottom,
        alignCenterX,
        alignCenterY,
        distributeX,
        distributeY,
        copy,
        cut,
        paste,
        deleteShape
    };

    explicit LayoutCanvas(QWidget *parent = nullptr);
    ~LayoutCanvas() override;

    virtual void setTool(Tool tool) = 0;
    virtual void setReadOnly(bool readOnly) = 0;
    virtual bool isReadOnly() const = 0;
    virtual Tool currentTool() const = 0;
    virtual void setGridVisible(bool visible) = 0;
    virtual void setSnapToGrid(bool enabled) = 0;
    virtual void setGridSpacing(qreal spacing) = 0;
    virtual void setDevicePreview(const openvlsi::LayoutDeviceCell *cell) = 0;
    virtual void setDevicePreviewModel(const openvlsi::LayoutPreviewModel &model) = 0;
    virtual void setDeviceLayerStyles(const std::map<std::pair<int, int>, LayerStyle> &styles) = 0;
    virtual void restoreLayoutInstanceSelection() = 0;
    virtual void selectLayoutInstance(openvlsi::ObjectId instanceId) = 0;
    virtual void setDevicePlacementCallback(std::function<void(QPointF)> callback) = 0;
    virtual void setInstanceActivatedCallback(std::function<void(openvlsi::ObjectId)> callback) = 0;
    virtual void setInstanceSelectionCallback(std::function<void(openvlsi::ObjectId)> callback) = 0;
    virtual openvlsi::ObjectId selectedLayoutInstance() const = 0;
    virtual LayoutSelectionState selectionState() const = 0;
    virtual void restoreSelectionState(const LayoutSelectionState &state, bool restoreView) = 0;
    virtual void setToolEscapeCallback(std::function<void()> callback) = 0;
    virtual void deleteSelectedShapes() = 0;
    // Drops any in-progress interaction (rectangle/port preview, polygon, ruler,
    // path drag) and then clears the scene. Use this instead of
    // scene()->clear(): the canvas keeps raw pointers to scene items used by
    // the current interaction, and clearing the scene behind its back left
    // those pointers dangling (crash after e.g. creating a layer mid-draw).
    virtual void clearScene() = 0;
    virtual void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                              std::function<void(openvlsi::ObjectId, QPointF)> shapeMoved,
                              std::function<void(openvlsi::ObjectId, QRectF)> shapeResized,
                              std::function<void(std::vector<openvlsi::ObjectId>)> deleted,
                              std::function<void()> changed) = 0;
    virtual void setLayerStyles(const std::map<openvlsi::ObjectId, LayerStyle> &styles) = 0;
    virtual void setRectangleStyle(const LayerStyle &style) = 0;
    virtual void fitToContent() = 0;
    // Centers the view on a layout-space rectangle and marks it with a
    // temporary DRC highlight. Zero-size rectangles mark a single point.
    virtual void focusOnLayoutRect(const openvlsi::Rect &rect) = 0;

    virtual void setPathConfig(const LayoutPathConfig &config) = 0;
    virtual void setPathWidth(int width) = 0;
    virtual int pathWidth() const = 0;
    virtual bool pathAvailable() const = 0;
    virtual std::string pathLayerName() const = 0;
    virtual void setPathCreatedCallback(LayoutPathCreatedCallback callback) = 0;
    virtual void setPathLayerChangedCallback(LayoutPathLayerChangedCallback callback) = 0;
    virtual void setPathPointSelectionChangedCallback(LayoutPathPointSelectionChangedCallback callback) = 0;
    virtual void setTextPlacedCallback(LayoutTextPlacedCallback callback) = 0;
    virtual void setTextPolygonPlacedCallback(LayoutTextPolygonPlacedCallback callback) = 0;
    virtual void setPortPlacedCallback(LayoutPortPlacedCallback callback) = 0;
    virtual void setInstancePortActivatedCallback(LayoutInstancePortActivatedCallback callback) = 0;
    virtual void setPlannedDeviceActivatedCallback(LayoutPlannedDeviceActivatedCallback callback) = 0;
    virtual void setPlannedConnectionActivatedCallback(LayoutPlannedConnectionActivatedCallback callback) = 0;
    virtual void setPolygonCreatedCallback(LayoutPolygonCreatedCallback callback) = 0;
    virtual void setPolygonPointMovedCallback(LayoutPolygonPointMovedCallback callback) = 0;
    virtual void setShapeActivatedCallback(std::function<void(openvlsi::ObjectId)> callback) = 0;
    // Invoked when the user picks an entry of the figure context menu. The id
    // is the selected figure the menu was opened on.
    virtual void setFigureContextActionCallback(std::function<void(openvlsi::ObjectId, FigureContextAction)> callback) = 0;
    // Invoked when the user right-clicks empty canvas space. The host owns the
    // menu contents (editor tools plus clipboard entries).
    virtual void setEmptyContextMenuCallback(std::function<void(const QPoint &globalPos)> callback) = 0;
    // Cursor position in scene coordinates (positive Y downwards), updated on
    // the latest mouse move or press. Used to anchor pasted figures.
    virtual QPointF cursorScenePosition() const = 0;
    // Highlights the layout shapes and planned connections that belong to one
    // logical net. Overlays are presentation-only and are cleared on the next
    // canvas interaction.
    virtual void highlightLayoutNet(const std::vector<openvlsi::ObjectId> &shapeIds,
                                    const std::vector<openvlsi::ObjectId> &connectionIds,
                                    const std::vector<openvlsi::ObjectId> &plannedDeviceIds) = 0;
    virtual void clearLayoutNetHighlight() = 0;
    virtual void refreshInteractionState() = 0;
};

// Builds the outline of an open polyline using KLayout-style corner joins.
// The miter is used only while both offset-line intersections stay within
// width/2 of the corner; sharper corners are cut off at width/2 instead of
// producing long elbows. Points are in scene coordinates. A width vector may
// supply one width per segment.
QPolygonF layoutPathOutline(const std::vector<QPointF> &points, qreal width);
QPolygonF layoutPathOutline(const std::vector<QPointF> &points, const std::vector<qreal> &widths);

// Creates the single selectable item used for a routed path. Points are in
// scene coordinates. When pointEditable is true, handles become visible while
// selected; dragging a handle calls pointMoved, double-clicking a segment calls
// pointInserted, and right-clicking a handle offers point deletion. When
// endpointHandlesOnly is true, only the first and last points expose handles
// and point insertion/deletion are disabled.
QGraphicsItem *createLayoutPathItem(openvlsi::ObjectId id, const std::vector<QPointF> &points, qreal width,
                                    const std::vector<qreal> &segmentWidths, const std::vector<bool> &segmentVisible,
                                    const std::vector<QColor> &segmentColors,
                                    const std::vector<std::vector<QColor>> &pointViaColors, bool readOnly,
                                    bool pointEditable, bool endpointHandlesOnly,
                                    LayoutPathPointsMovedCallback pointsMoved,
                                    LayoutPathPointInsertedCallback pointInserted,
                                    LayoutPathPointsDeletedCallback pointsDeleted);

// Creates the selectable/movable canvas box used for a planned device. The
// rectangle is given in scene coordinates and the item is positioned at its
// top-left corner so drag deltas map directly onto layout coordinates.
QGraphicsItem *createLayoutPlannedDeviceItem(openvlsi::ObjectId id, const QString &name, const QRectF &rect, bool readOnly);

// Creates the selectable line used for a planned connection. The line is given
// in scene coordinates and has a wide invisible hit area.
QGraphicsItem *createLayoutPlannedConnectionItem(openvlsi::ObjectId id, const QLineF &line, const QColor &color);

// Rectangle resize handles use the same square point style as routed paths.
// Handle 0..3 are the corners; 4..7 are the left, right, top, and bottom
// midpoints.
QPointF layoutResizeHandlePosition(const QRectF &rect, int handle);
QGraphicsItem *createLayoutResizeHandleItem(openvlsi::ObjectId shapeId, int handle);
QGraphicsItem *createLayoutPolygonHandleItem(openvlsi::ObjectId shapeId, int index);

// Paints a prepared preview model as a non-interactive child item. Used for
// instance contents so the canvas and the Properties preview share one
// renderer.
QGraphicsItem *
createLayoutPreviewItem(const openvlsi::LayoutPreviewModel &model, const openvlsi::LayoutRenderer::LayerStyleMap &styles,
                        const openvlsi::LayoutRenderer::DeviceStyleMap &deviceStyles,
                        const openvlsi::LayoutRenderer::PaintOptions &options = openvlsi::LayoutRenderer::PaintOptions{});
QGraphicsItem *
createLayoutPreviewItem(const openvlsi::LayoutPreviewModel *model, const openvlsi::LayoutRenderer::LayerStyleMap &styles,
                        const openvlsi::LayoutRenderer::DeviceStyleMap &deviceStyles,
                        const openvlsi::LayoutRenderer::PaintOptions &options = openvlsi::LayoutRenderer::PaintOptions{});

LayoutCanvas *createLayoutCanvas(QWidget *parent = nullptr);
