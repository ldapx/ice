#pragma once

#include "icelib/core.hpp"
#include "icelib/implementation.hpp"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QListWidget;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QShowEvent;
class QTimer;
class TextDocumentEditor;
class ImplementationPreviewCanvas;
class SvgPreviewCanvas;

namespace ice {
struct LayoutPreviewModel;
struct VerificationReport;
} // namespace ice

// Shared state for the RTL-to-GDS phase tabs. It intentionally lives in the
// GUI layer: run mutation and stage execution stay in icelib / ice-cli.
class ImplementationUiController {
  public:
    struct StagePreview {
        std::shared_ptr<ice::Project> project;
        ice::ObjectId cell = 0;
        std::shared_ptr<ice::LayoutPreviewModel> model;
        QString message;
    };

    explicit ImplementationUiController(QObject *parent);
    ~ImplementationUiController();

    void setProject(const ice::Project *project, const QString &projectPath);
    const ice::Project *project() const noexcept { return project_; }
    QString projectPath() const { return projectPath_; }
    QString projectRoot() const;
    bool busy() const;
    ice::ImplementationStage runningStage() const { return runningStage_; }
    ice::ObjectId activeRunId() const { return activeRunId_; }
    const ice::ImplementationRun *activeRun() const;

    void setActiveRunId(ice::ObjectId id);
    void run(ice::ImplementationStage from, ice::ImplementationStage to);
    void exportMapped(ice::ObjectId runId, const QString &destination);
    void cancel();

    // Loads the stage preview from its ODB checkpoint, falling back to the
    // legacy DEF import. Results are cached by run/stage/file timestamp.
    StagePreview stagePreview(ice::ImplementationStage stage);

    // Asynchronous variant used by the physical panes.
    using StagePreviewCallback = std::function<void(const StagePreview &)>;
    void requestStagePreview(ice::ImplementationStage stage, StagePreviewCallback callback);

    // Writes a native OpenROAD heat-map CSV for the active run/stage.
    QString exportHeatMap(ice::ImplementationStage stage, const QString &kind, QString &error);

    using HeatMapCallback = std::function<void(const QString &path, const QString &error)>;
    void requestExportHeatMap(ice::ImplementationStage stage, const QString &kind, HeatMapCallback callback);

    // Opens the active stage ODB in the standalone OpenROAD GUI.
    bool openInOpenRoadGui(ice::ImplementationStage stage, QString &error);

    // Runs OpenROAD STA on the active stage ODB and returns its path report.
    QString timingReport(ice::ImplementationStage stage, QString &error);

    // Imports the newest available DEF checkpoint as a layout cell.
    bool importLayout(ice::ObjectId runId, QString &error);

    void refresh();
    void addLogListener(std::function<void(ice::ImplementationStage, const QString &)> listener);
    void setOnChanged(std::function<void()> callback) { onChanged_ = std::move(callback); }
    void setOnProjectChanged(std::function<void()> callback) { onProjectChanged_ = std::move(callback); }
    void setOnRunFinished(std::function<void(int)> callback) { onRunFinished_ = std::move(callback); }
    void setOnHdlTextEdited(std::function<void(ice::ObjectId, const std::string &)> callback) {
        onHdlTextEdited_ = std::move(callback);
    }
    void editHdlText(ice::ObjectId cell, const std::string &text) {
        if (onHdlTextEdited_)
            onHdlTextEdited_(cell, text);
    }
    // Invoked immediately before ice-cli starts. Returning false aborts the
    // run, which lets the GUI flush unsaved project state first.
    void setBeforeRun(std::function<bool()> callback) { beforeRun_ = std::move(callback); }

  private:
    struct PreviewCacheEntry {
        QString key;
        std::shared_ptr<ice::Project> project;
        ice::ObjectId cell = 0;
        std::shared_ptr<ice::LayoutPreviewModel> model;
        QString message;
    };

    void appendProcessText(const QString &text);
    void finishRun(int exitCode, int exitStatus);
    void startCli(const QString &executable, const QStringList &arguments, ice::ImplementationStage stage);
    QString cliExecutable() const;
    QString helperExecutable() const;

    QObject *parent_ = nullptr;
    const ice::Project *project_ = nullptr;
    QString projectPath_;
    ice::ObjectId activeRunId_ = 0;
    QProcess *process_ = nullptr;
    QString processBuffer_;
    ice::ImplementationStage runningStage_ = ice::ImplementationStage::created;
    QProcess *previewProcess_ = nullptr;
    QProcess *heatMapProcess_ = nullptr;
    std::function<void()> onChanged_;
    std::function<void()> onProjectChanged_;
    std::function<void(int)> onRunFinished_;
    std::function<void(ice::ObjectId, const std::string &)> onHdlTextEdited_;
    std::function<bool()> beforeRun_;
    std::vector<std::function<void(ice::ImplementationStage, const QString &)>> logListeners_;
    std::map<int, PreviewCacheEntry> previewCache_;
};

class ImplementationPage : public QWidget {
  public:
    ImplementationPage(ice::ImplementationStage stage, ImplementationUiController *controller,
                       QWidget *parent = nullptr, bool embedded = false);

    ice::ImplementationStage stage() const noexcept { return stage_; }
    bool stageReady() const noexcept { return stageReady_; }
    const QString &stageHint() const noexcept { return stageHint_; }
    void refresh();
    void setCreateHdlBlockCallback(std::function<void()> callback) { createHdlBlockCallback_ = std::move(callback); }
    void setDeleteHdlBlockCallback(std::function<void(ice::ObjectId)> callback) {
        deleteHdlBlockCallback_ = std::move(callback);
    }
    void setElaborateDesignCallback(std::function<void(ice::ObjectId, const std::string &)> callback) {
        elaborateDesignCallback_ = std::move(callback);
    }
    void setSynthesizeRunCallback(std::function<void(ice::ObjectId, const std::string &)> callback) {
        synthesizeRunCallback_ = std::move(callback);
    }
    void setExportVerilogCallback(std::function<void(ice::ObjectId)> callback) {
        exportVerilogCallback_ = std::move(callback);
    }
    void setRemoveRunCallback(std::function<void(ice::ObjectId)> callback) { removeRunCallback_ = std::move(callback); }
    void setApplyConfigCallback(std::function<void(ice::ObjectId, const ice::ImplementationConfig &)> callback) {
        applyConfigCallback_ = std::move(callback);
    }
    void setRunSelectionCallback(std::function<void(ice::ObjectId, std::optional<ice::ImplementationStage>)> callback) {
        runSelectionCallback_ = std::move(callback);
    }
    void
    setDrcReportCallback(std::function<void(const ice::VerificationReport &, const QString &stageLabel)> callback) {
        drcReportCallback_ = std::move(callback);
    }
    void setTimingMetricsCallback(std::function<void(const std::vector<std::pair<QString, QString>> &)> callback) {
        timingMetricsCallback_ = std::move(callback);
    }
    // Selects a run (and optionally one of its stages) in the Runs tree
    // without emitting the user-selection signal.
    void selectRunStage(ice::ObjectId runId, std::optional<ice::ImplementationStage> stage);
    bool selectDrcMarker(int index);
    void selectHdlCell(ice::ObjectId id);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void buildUi();
    void refreshRunTree();
    void refreshRunCombo();
    void refreshExplorer();
    void applyHdlFilter(const QString &text);
    void openHdlFile(ice::ObjectId id);
    void closeHdlFile(ice::ObjectId id);
    void flushHdlEdit();
    TextDocumentEditor *hdlEditorFor(ice::ObjectId id) const;
    void refreshRunsTable();
    void applyRunFilter(const QString &text);
    void updateRunActions();
    ice::ObjectId selectedRunId() const;
    ice::ObjectId actionRunId() const;
    void showRunGraph(ice::ObjectId runId);
    QString runStageLabel(const ice::ImplementationRun &run) const;
    std::optional<std::string> promptPdk(const QString &title, const std::string &current);
    void refreshSynthesisExplorer();
    void openArtifact(const QString &path);
    void closeArtifactTab(QWidget *widget);
    void ensureMiddlePlaceholder();
    void refreshMetrics();
    void refreshArtifacts();
    void refreshStageForm();
    void updateStageActions();
    void applyStageForm();
    void refreshProperties();
    void refreshOutline();
    void refreshIpLibraries();
    void refreshPreview();
    void refreshDisplayControls();
    void refreshInspector();
    void refreshDrc();
    void refreshTiming();
    bool hdlMode() const { return stage_ == ice::ImplementationStage::created; }
    bool synthesisMode() const { return stage_ == ice::ImplementationStage::synthesis; }
    bool floorplanMode() const { return stage_ == ice::ImplementationStage::floorplan; }
    bool physicalStageMode() const {
        return stage_ == ice::ImplementationStage::floorplan || stage_ == ice::ImplementationStage::placement ||
               stage_ == ice::ImplementationStage::cts || stage_ == ice::ImplementationStage::routing ||
               stage_ == ice::ImplementationStage::final;
    }

    ice::ImplementationStage stage_;
    ImplementationUiController *controller_ = nullptr;
    bool embedded_ = false;
    bool stageReady_ = false;
    QString stageHint_;
    QTabWidget *leftTabs_ = nullptr;
    QTreeWidget *runTree_ = nullptr;
    QTreeWidget *displayTree_ = nullptr;
    QLabel *previewStatusLabel_ = nullptr;
    QLabel *previewCoordinateLabel_ = nullptr;
    QTreeWidget *inspectorTree_ = nullptr;
    QListWidget *hdlFileList_ = nullptr;
    QLineEdit *hdlSearch_ = nullptr;
    QPushButton *newHdlButton_ = nullptr;
    QPushButton *deleteHdlButton_ = nullptr;
    QPushButton *elaborateButton_ = nullptr;
    QTabWidget *runsTabs_ = nullptr;
    QLineEdit *runSearch_ = nullptr;
    QTableWidget *runsTable_ = nullptr;
    QPushButton *synthesizeButton_ = nullptr;
    QPushButton *showGraphButton_ = nullptr;
    QPushButton *exportVerilogButton_ = nullptr;
    TextDocumentEditor *hdlEditor_ = nullptr;
    std::map<ice::ObjectId, TextDocumentEditor *> hdlEditors_;
    QTimer *hdlEditTimer_ = nullptr;
    ice::ObjectId activeHdlCellId_ = 0;
    ice::ObjectId hdlEditCellId_ = 0;
    ice::ObjectId lastRunTreeActiveRunId_ = 0;
    bool loadingHdlEditor_ = false;
    bool hdlTabsInitialized_ = false;
    QString hdlProjectKey_;
    QTabWidget *middleTabs_ = nullptr;
    QTabWidget *rightTabs_ = nullptr;
    QStackedWidget *rightInspectorStack_ = nullptr;
    QTreeWidget *outlineTree_ = nullptr;
    QTreeWidget *ipLibrariesTree_ = nullptr;
    QTableWidget *metricsTable_ = nullptr;
    QListWidget *artifactsList_ = nullptr;
    QDoubleSpinBox *utilizationSpin_ = nullptr;
    QDoubleSpinBox *aspectRatioSpin_ = nullptr;
    QCheckBox *dieAreaCheck_ = nullptr;
    QDoubleSpinBox *dieWidthSpin_ = nullptr;
    QDoubleSpinBox *dieHeightSpin_ = nullptr;
    QDoubleSpinBox *dieXSpin_ = nullptr;
    QDoubleSpinBox *dieYSpin_ = nullptr;
    QCheckBox *coreAreaCheck_ = nullptr;
    QDoubleSpinBox *coreWidthSpin_ = nullptr;
    QDoubleSpinBox *coreHeightSpin_ = nullptr;
    QDoubleSpinBox *coreXSpin_ = nullptr;
    QDoubleSpinBox *coreYSpin_ = nullptr;
    QLineEdit *siteEdit_ = nullptr;
    QLineEdit *ioHorizontalEdit_ = nullptr;
    QLineEdit *ioVerticalEdit_ = nullptr;
    QDoubleSpinBox *placementDensitySpin_ = nullptr;
    QCheckBox *timingDrivenCheck_ = nullptr;
    QCheckBox *routabilityDrivenCheck_ = nullptr;
    QCheckBox *ctsEnabledCheck_ = nullptr;
    QLineEdit *ctsRootBufferEdit_ = nullptr;
    QLineEdit *ctsBuffersEdit_ = nullptr;
    QLineEdit *routingMinLayerEdit_ = nullptr;
    QLineEdit *routingMaxLayerEdit_ = nullptr;
    QCheckBox *antennaRepairCheck_ = nullptr;
    QSpinBox *threadsSpin_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QPushButton *applyStageButton_ = nullptr;
    QPushButton *runStageButton_ = nullptr;
    QPushButton *cancelStageButton_ = nullptr;
    QPushButton *runFromHereButton_ = nullptr;
    QPushButton *runAllButton_ = nullptr;
    QComboBox *runCombo_ = nullptr;
    QLabel *stageHintLabel_ = nullptr;
    QComboBox *heatMapCombo_ = nullptr;
    ImplementationPreviewCanvas *previewCanvas_ = nullptr;
    std::map<std::string, QWidget *> artifactTabs_;
    QWidget *middlePlaceholder_ = nullptr;
    std::function<void()> createHdlBlockCallback_;
    std::function<void(ice::ObjectId)> deleteHdlBlockCallback_;
    std::function<void(ice::ObjectId, const std::string &)> elaborateDesignCallback_;
    std::function<void(ice::ObjectId, const std::string &)> synthesizeRunCallback_;
    std::function<void(ice::ObjectId)> exportVerilogCallback_;
    std::function<void(ice::ObjectId)> removeRunCallback_;
    std::function<void(ice::ObjectId, const ice::ImplementationConfig &)> applyConfigCallback_;
    std::function<void(ice::ObjectId, std::optional<ice::ImplementationStage>)> runSelectionCallback_;
    std::function<void(const ice::VerificationReport &, const QString &stageLabel)> drcReportCallback_;
    std::function<void(const std::vector<std::pair<QString, QString>> &)> timingMetricsCallback_;
};

// One top-level workspace that hosts the four physical stages. Stage
// selection lives in each pane's Runs tree; the page itself only swaps panes.
class PhysicalDesignPage : public QWidget {
  public:
    explicit PhysicalDesignPage(ImplementationUiController *controller, QWidget *parent = nullptr);

    void setCurrentStage(ice::ImplementationStage stage);
    ice::ImplementationStage currentStage() const;
    std::vector<ImplementationPage *> pages() const;
    ImplementationPage *pageForStage(ice::ImplementationStage stage) const;

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    int stageIndex(ice::ImplementationStage stage) const;
    ice::ImplementationStage stageAt(int index) const;
    void selectRunStage(ice::ObjectId runId, std::optional<ice::ImplementationStage> stage);

    QStackedWidget *stageStack_ = nullptr;
    std::vector<ImplementationPage *> pages_;
    std::vector<ice::ImplementationStage> stages_;
};
