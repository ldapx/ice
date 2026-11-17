#pragma once

#include "icelib/core.hpp"
#include "icelib/simulation.hpp"

#include <QDialog>

#include <filesystem>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

// Editor for the analysis, corner, sweep, Monte Carlo, and measurement
// configuration stored on a SimulationItem. The dialog can also run the
// configured experiment and display one result row per variant.
class SimulationExperimentDialog : public QDialog {
  public:
    SimulationExperimentDialog(const ice::Project &project, const ice::SimulationItem &simulation,
                               const std::filesystem::path &projectRoot, QWidget *parent = nullptr);

    const ice::SimulationItem &simulation() const { return item_; }

  protected:
    void accept() override;

  private:
    bool applyItem();
    void runExperiment();
    void addCornerRow(const ice::SimulationCorner &corner = {});
    void loadPdkCorners();
    void addSweepRow(const ice::SimulationSweep &sweep = {});
    void addMeasurementRow(const ice::SimulationMeasurementSpec &spec = {});
    void removeSelectedRow(QTableWidget *table);
    void showResults(const std::vector<ice::ExperimentPoint> &points,
                     const std::vector<std::filesystem::path> &storedPaths);

    const ice::Project &project_;
    const std::filesystem::path projectRoot_;
    ice::SimulationItem item_;

    QDoubleSpinBox *startTime_ = nullptr;
    QDoubleSpinBox *temperature_ = nullptr;
    QLineEdit *modelSection_ = nullptr;
    QLineEdit *dcSource_ = nullptr;
    QDoubleSpinBox *dcStart_ = nullptr;
    QDoubleSpinBox *dcStop_ = nullptr;
    QDoubleSpinBox *dcStep_ = nullptr;
    QComboBox *acVariation_ = nullptr;
    QSpinBox *acPoints_ = nullptr;
    QDoubleSpinBox *acStart_ = nullptr;
    QDoubleSpinBox *acStop_ = nullptr;
    QLineEdit *noiseOutput_ = nullptr;
    QLineEdit *noiseInput_ = nullptr;
    QComboBox *noiseVariation_ = nullptr;
    QSpinBox *noisePoints_ = nullptr;
    QDoubleSpinBox *noiseStart_ = nullptr;
    QDoubleSpinBox *noiseStop_ = nullptr;
    QLineEdit *tfOutput_ = nullptr;
    QLineEdit *tfSource_ = nullptr;

    QTableWidget *corners_ = nullptr;
    QTableWidget *sweeps_ = nullptr;
    QCheckBox *monteCarloEnabled_ = nullptr;
    QSpinBox *monteCarloSamples_ = nullptr;
    QDoubleSpinBox *temperatureSigma_ = nullptr;
    QDoubleSpinBox *supplySigma_ = nullptr;
    QLineEdit *supplySource_ = nullptr;
    QDoubleSpinBox *supplyNominal_ = nullptr;
    QSpinBox *monteCarloSeed_ = nullptr;
    QTableWidget *parameterSigma_ = nullptr;
    QTableWidget *measurements_ = nullptr;
    QTableWidget *results_ = nullptr;
    QPushButton *runButton_ = nullptr;
};
