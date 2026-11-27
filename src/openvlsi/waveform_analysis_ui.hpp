#pragma once

#include "openvlsilib/core.hpp"

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
    WaveformAnalysisDialog(openvlsi::SimulationGraph graph, std::filesystem::path projectRoot, openvlsi::ObjectId simulationId,
                           QWidget *parent = nullptr);

    const openvlsi::SimulationGraph &graph() const { return graph_; }

  protected:
    void accept() override;

  private:
    bool apply();
    void addDerivedRow(const openvlsi::SimulationDerivedTrace &trace = {});
    void addMarkerRow(const openvlsi::SimulationGraphMarker &marker = {});
    void loadOverlays();
    void removeSelectedRow(QTableWidget *table);

    openvlsi::SimulationGraph graph_;
    std::filesystem::path projectRoot_;
    openvlsi::ObjectId simulationId_ = 0;

    QComboBox *view_ = nullptr;
    QCheckBox *fft_ = nullptr;
    QComboBox *fftWindow_ = nullptr;
    QSpinBox *fftPoints_ = nullptr;
    QCheckBox *showMeasurements_ = nullptr;
    QTableWidget *derived_ = nullptr;
    QTableWidget *overlays_ = nullptr;
    QTableWidget *markers_ = nullptr;
};
