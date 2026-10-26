#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <QWidget>

class QPaintEvent;

// Plot widget for one simulation graph. Rendering is presentation-only: axis
// limits, grid visibility, and tick layout never affect simulation data.
class SimulationPlot : public QWidget {
  public:
    explicit SimulationPlot(QWidget *parent = nullptr);

    void setResult(const ice::SimulationResult &result);
    void setGraph(const ice::SimulationGraph &graph);
    void setGridVisible(bool visible);
    void clear();

    const ice::SimulationResult &result() const { return result_; }
    const ice::SimulationGraph &graph() const { return graph_; }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    const std::vector<double> *valuesForSignal(const std::string &signal) const;

    ice::SimulationResult result_;
    ice::SimulationGraph graph_;
    bool gridVisible_ = true;
};
