#pragma once

#include "icelib/core.hpp"

#include <QDialog>

#include <filesystem>

class QCheckBox;
class QComboBox;
class QSpinBox;
class QTableWidget;

// Editor for the waveform-analysis settings of one simulation graph: view
// transform, FFT, derived expressions, stored-run overlays, and point markers.
class WaveformAnalysisDialog : public QDialog {
  public:
    WaveformAnalysisDialog(ice::SimulationGraph graph, std::filesystem::path projectRoot, ice::ObjectId simulationId,
                           QWidget *parent = nullptr);

    const ice::SimulationGraph &graph() const { return graph_; }

  protected:
    void accept() override;

  private:
    bool apply();
    void addDerivedRow(const ice::SimulationDerivedTrace &trace = {});
    void addMarkerRow(const ice::SimulationGraphMarker &marker = {});
    void loadOverlays();
    void removeSelectedRow(QTableWidget *table);

    ice::SimulationGraph graph_;
    std::filesystem::path projectRoot_;
    ice::ObjectId simulationId_ = 0;

    QComboBox *view_ = nullptr;
    QCheckBox *fft_ = nullptr;
    QComboBox *fftWindow_ = nullptr;
    QSpinBox *fftPoints_ = nullptr;
    QCheckBox *showMeasurements_ = nullptr;
    QTableWidget *derived_ = nullptr;
    QTableWidget *overlays_ = nullptr;
    QTableWidget *markers_ = nullptr;
};
