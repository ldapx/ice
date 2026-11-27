#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/simulation.hpp"

#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QWheelEvent;

// Plot widget for one simulation graph. Rendering is presentation-only: axis
// limits, zoom, cursors, transforms, overlays, and markers never affect
// simulation data.
class SimulationPlot : public QWidget {
  public:
    // Number of min/max buckets used for the view-independent trace envelope.
    // The display path is rebuilt from this envelope during pan/zoom instead of
    // rescanning every stored sample.
    static constexpr std::size_t envelopeBuckets = 8192;
    // One additional run drawn in the same graph. Labels identify corners or
    // experiment variants in the legend.
    struct Overlay {
        std::string label;
        std::shared_ptr<const openvlsi::SimulationResult> result;
    };

    enum class Tool { pan, cursor, marker };

    explicit SimulationPlot(QWidget *parent = nullptr);

    void setResult(std::shared_ptr<const openvlsi::SimulationResult> result);
    void setOverlays(std::vector<Overlay> overlays);
    void setGraph(const openvlsi::SimulationGraph &graph);
    void setGridVisible(bool visible);
    void clear();
    // Called when the user renames a signal's display label from the plot. An
    // empty label means "use the signal name again".
    void setLabelEditCallback(
        std::function<void(openvlsi::ObjectId /*graphId*/, const std::string & /*signal*/, const QString & /*label*/)>
            callback);
    // Called when the user renames the Y axis title. An empty label restores
    // the default "Signals" caption.
    void setYAxisLabelEditCallback(std::function<void(openvlsi::ObjectId /*graphId*/, const QString & /*label*/)> callback);
    // Called when the user places a marker on the plot.
    void setMarkerCallback(std::function<void(const openvlsi::SimulationGraphMarker &)> callback);
    // Called when the user clears persisted markers from the plot.
    void setClearMarkersCallback(std::function<void()> callback);

    const openvlsi::SimulationResult &result() const;
    std::shared_ptr<const openvlsi::SimulationResult> resultPtr() const { return result_; }
    const openvlsi::SimulationGraph &graph() const { return graph_; }
    const std::vector<Overlay> &overlays() const { return overlays_; }

    // Interaction mode. Plain clicks set cursor A, Shift+click sets cursor B,
    // and dragging pans the view in every mode. Marker mode turns a click into
    // a persisted point annotation.
    void setTool(Tool tool);
    Tool tool() const { return tool_; }

    // Clears wheel/drag zoom and returns to graph limits or auto-scaling.
    void resetView();
    bool hasZoom() const;
    // Places or clears a measurement cursor (0 = A, 1 = B) in data units.
    void setCursorPosition(std::size_t which, std::optional<double> x);
    std::optional<double> cursor(std::size_t which) const;
    void clearCursors();
    // Copies the current cursor readout as text, used by the context menu and
    // automation hooks.
    QString cursorReadout() const;

    // Optional callback for status-bar messages.
    void setStatusCallback(std::function<void(const QString &)> callback);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

  private:
    struct EnvelopeBucket {
        double minY = 0.0;
        double maxY = 0.0;
        bool valid = false;
    };

    struct PreparedTrace {
        std::string signal;
        QString label;
        QString xUnit;
        QString yUnit;
        std::vector<double> x;
        std::vector<double> y;
        bool xSorted = false;
        // Sorted sample indices for large non-monotonic X/Y curves so cursor
        // lookup does not scan the whole trace on every mouse move.
        std::vector<std::size_t> nearestOrder;
        bool dashed = false;
        int colorIndex = 0;
        // View-independent min/max envelope over the full X range.
        std::vector<EnvelopeBucket> envelope;
        double envelopeMinX = 0.0;
        double envelopeMaxX = 0.0;
    };

    struct PreparedCache {
        bool valid = false;
        std::vector<PreparedTrace> traces;
        std::vector<QString> warnings;
        bool haveData = false;
        double dataXMin = 0.0;
        double dataXMax = 1.0;
        double dataYMin = 0.0;
        double dataYMax = 1.0;
    };

    struct DisplayTrace {
        std::string signal;
        QString label;
        QString xUnit;
        QString yUnit;
        std::size_t preparedIndex = 0;
        QPainterPath path;
        bool dashed = false;
        int colorIndex = 0;
    };

    struct DisplayCache {
        bool valid = false;
        QSize size;
        QRectF plot;
        QRectF traceArea;
        QString title;
        QString xLabel;
        QString yLabel;
        std::vector<QString> traceLabels;
        // Legend chips and axis titles are drawn outside the plot rectangle so
        // they never cover the traces. The rectangles are used for hit testing
        // the double-click-to-rename interaction.
        std::vector<QRectF> legendChips;
        std::vector<std::string> legendSignals;
        QRectF xLabelRect;
        QRectF yLabelRect;
        QRectF yTickLabelRect;
        std::string xSignal;
        std::vector<double> xTicks;
        std::vector<double> yTicks;
        std::vector<DisplayTrace> traces;
        std::vector<QString> warnings;
        double xMin = 0.0;
        double xMax = 1.0;
        double yMin = 0.0;
        double yMax = 1.0;
    };

    const std::vector<double> *valuesForSignal(const std::string &signal) const;
    void invalidateDisplayCache();
    void invalidatePreparedCache();
    void ensurePreparedCache() const;
    void ensureDisplayCache() const;
    QPainterPath buildTracePath(const PreparedTrace &trace, double xMin, double xMax, int columns) const;
    void editLabel(const std::string &signal, const QString &currentLabel);
    bool inTraceArea(const QPointF &position) const;
    double xAtPosition(double x) const;
    double yAtPosition(double y) const;
    QPointF positionAt(double x, double y) const;
    void applyBoxZoom();
    void showContextMenu(const QPointF &position);
    void placeMarker(const QPointF &position);
    void emitStatus(const QString &message);
    QString formatValue(double value, const QString &unit = {}) const;
    QString readoutText() const;

    std::shared_ptr<const openvlsi::SimulationResult> result_;
    std::vector<Overlay> overlays_;
    openvlsi::SimulationGraph graph_;
    bool gridVisible_ = true;
    Tool tool_ = Tool::pan;
    mutable PreparedCache prepared_;
    mutable DisplayCache cache_;
    std::optional<double> viewXMin_;
    std::optional<double> viewXMax_;
    std::optional<double> viewYMin_;
    std::optional<double> viewYMax_;
    std::optional<double> cursorA_;
    std::optional<double> cursorB_;
    std::optional<QPointF> hover_;
    bool dragging_ = false;
    bool movingCursor_ = false;
    std::size_t movingCursorIndex_ = 0;
    QPointF dragStart_;
    QRectF boxZoomRect_;
    bool boxZooming_ = false;
    std::optional<double> dragStartXMin_;
    std::optional<double> dragStartXMax_;
    std::optional<double> dragStartYMin_;
    std::optional<double> dragStartYMax_;
    std::function<void(openvlsi::ObjectId, const std::string &, const QString &)> labelEditCallback_;
    std::function<void(openvlsi::ObjectId, const QString &)> yAxisLabelEditCallback_;
    std::function<void(const openvlsi::SimulationGraphMarker &)> markerCallback_;
    std::function<void()> clearMarkersCallback_;
    std::function<void(const QString &)> statusCallback_;
};
