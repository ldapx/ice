#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <QPainterPath>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

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

    const ice::SimulationResult &result() const;
    std::shared_ptr<const ice::SimulationResult> resultPtr() const { return result_; }
    const ice::SimulationGraph &graph() const { return graph_; }

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;

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

    std::shared_ptr<const ice::SimulationResult> result_;
    ice::SimulationGraph graph_;
    bool gridVisible_ = true;
    mutable DisplayCache cache_;
};
