#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

// Plot widget for one simulation graph. Rendering is presentation-only: axis
// limits, grid visibility, and tick layout never affect simulation data.
class SimulationPlot : public QWidget {
  public:
    explicit SimulationPlot(QWidget *parent = nullptr);

    void setResult(std::shared_ptr<const ice::SimulationResult> result);
    void setGraph(const ice::SimulationGraph &graph);
    void setGridVisible(bool visible);
    void clear();
    // Called when the user renames a signal's display label from the plot. An
    // empty label means "use the signal name again".
    void setLabelEditCallback(
        std::function<void(ice::ObjectId /*graphId*/, const std::string & /*signal*/, const QString & /*label*/)>
            callback);
    // Called when the user renames the Y axis title. An empty label restores
    // the default "Signals" caption.
    void setYAxisLabelEditCallback(std::function<void(ice::ObjectId /*graphId*/, const QString & /*label*/)> callback);

    const ice::SimulationResult &result() const;
    std::shared_ptr<const ice::SimulationResult> resultPtr() const { return result_; }
    const ice::SimulationGraph &graph() const { return graph_; }

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

  private:
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
        std::string xSignal;
        std::vector<double> xTicks;
        std::vector<double> yTicks;
        std::vector<QPainterPath> paths;
        double xMin = 0.0;
        double xMax = 1.0;
        double yMin = 0.0;
        double yMax = 1.0;
    };

    const std::vector<double> *valuesForSignal(const std::string &signal) const;
    void invalidateDisplayCache();
    void ensureDisplayCache() const;
    void editLabel(const std::string &signal, const QString &currentLabel);

    std::shared_ptr<const ice::SimulationResult> result_;
    ice::SimulationGraph graph_;
    bool gridVisible_ = true;
    mutable DisplayCache cache_;
    std::function<void(ice::ObjectId, const std::string &, const QString &)> labelEditCallback_;
    std::function<void(ice::ObjectId, const QString &)> yAxisLabelEditCallback_;
};
