#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/implementation.hpp"

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

namespace openvlsi {
struct LayoutPreviewModel;
struct VerificationReport;
} // namespace openvlsi

// Shared state for the RTL-to-GDS phase tabs. It intentionally lives in the
// GUI layer: run mutation and stage execution stay in openvlsilib / openvlsi-cli.
class ImplementationUiController {
  public:
    struct StagePreview {
        std::shared_ptr<openvlsi::Project> project;
        openvlsi::ObjectId cell = 0;
        std::shared_ptr<openvlsi::LayoutPreviewModel> model;
        QString message;
    };

    explicit ImplementationUiController(QObject *parent);
    ~ImplementationUiController();

    void setProject(const openvlsi::Project *project, const QString &projectPath);
    const openvlsi::Project *project() const noexcept { return project_; }
    QString projectPath() const { return projectPath_; }
    QString projectRoot() const;
    bool busy() const;
    openvlsi::ImplementationStage runningStage() const { return runningStage_; }
    openvlsi::ObjectId activeRunId() const { return activeRunId_; }
    const openvlsi::ImplementationRun *activeRun() const;

    void setActiveRunId(openvlsi::ObjectId id);
    void run(openvlsi::ImplementationStage from, openvlsi::ImplementationStage to);
    void exportMapped(openvlsi::ObjectId runId, const QString &destination);
    void cancel();

    // Loads the stage preview from its ODB checkpoint, falling back to the
    // legacy DEF import. Results are cached by run/stage/file timestamp.
    StagePreview stagePreview(openvlsi::ImplementationStage stage);

    // Asynchronous variant used by the physical panes.
    using StagePreviewCallback = std::function<void(const StagePreview &)>;
    void requestStagePreview(openvlsi::ImplementationStage stage, StagePreviewCallback callback);

    // Writes a native OpenROAD heat-map CSV for the active run/stage.
    QString exportHeatMap(openvlsi::ImplementationStage stage, const QString &kind, QString &error);

    using HeatMapCallback = std::function<void(const QString &path, const QString &error)>;
    void requestExportHeatMap(openvlsi::ImplementationStage stage, const QString &kind, HeatMapCallback callback);

    // Opens the active stage ODB in the standalone OpenROAD GUI.
    bool openInOpenRoadGui(openvlsi::ImplementationStage stage, QString &error);

    // Runs OpenROAD STA on the active stage ODB and returns its path report.
    QString timingReport(openvlsi::ImplementationStage stage, QString &error);

    // Imports the newest available DEF checkpoint as a layout cell.
    bool importLayout(openvlsi::ObjectId runId, QString &error);

    void refresh();
    void addLogListener(std::function<void(openvlsi::ImplementationStage, const QString &)> listener);
    void setOnChanged(std::function<void()> callback) { onChanged_ = std::move(callback); }
    void setOnProjectChanged(std::function<void()> callback) { onProjectChanged_ = std::move(callback); }
    void setOnRunFinished(std::function<void(int)> callback) { onRunFinished_ = std::move(callback); }
    void setOnHdlTextEdited(std::function<void(openvlsi::ObjectId, const std::string &)> callback) {
        onHdlTextEdited_ = std::move(callback);
    }
    void editHdlText(openvlsi::ObjectId cell, const std::string &text) {
        if (onHdlTextEdited_)
            onHdlTextEdited_(cell, text);
    }
    // Invoked immediately before openvlsi-cli starts. Returning false aborts the
    // run, which lets the GUI flush unsaved project state first.
    void setBeforeRun(std::function<bool()> callback) { beforeRun_ = std::move(callback); }

  private:
    struct PreviewCacheEntry {
        QString key;
        std::shared_ptr<openvlsi::Project> project;
        openvlsi::ObjectId cell = 0;
        std::shared_ptr<openvlsi::LayoutPreviewModel> model;
        QString message;
    };

    void appendProcessText(const QString &text);
    void finishRun(int exitCode, int exitStatus);
    void startCli(const QString &executable, const QStringList &arguments, openvlsi::ImplementationStage stage);
    QString cliExecutable() const;
    QString helperExecutable() const;

    QObject *parent_ = nullptr;
    const openvlsi::Project *project_ = nullptr;
    QString projectPath_;
    openvlsi::ObjectId activeRunId_ = 0;
    QProcess *process_ = nullptr;
    QString processBuffer_;
    openvlsi::ImplementationStage runningStage_ = openvlsi::ImplementationStage::created;
    QProcess *previewProcess_ = nullptr;
    QProcess *heatMapProcess_ = nullptr;
    std::function<void()> onChanged_;
    std::function<void()> onProjectChanged_;
    std::function<void(int)> onRunFinished_;
    std::function<void(openvlsi::ObjectId, const std::string &)> onHdlTextEdited_;
    std::function<bool()> beforeRun_;
    std::vector<std::function<void(openvlsi::ImplementationStage, const QString &)>> logListeners_;
    std::map<int, PreviewCacheEntry> previewCache_;
};

class ImplementationPage : public QWidget {
  public:
    ImplementationPage(openvlsi::ImplementationStage stage, ImplementationUiController *controller,
                       QWidget *parent = nullptr, bool embedded = false);

    openvlsi::ImplementationStage stage() const noexcept { return stage_; }
    bool stageReady() const noexcept { return stageReady_; }
    const QString &stageHint() const noexcept { return stageHint_; }
    void refresh();
    void setCreateHdlBlockCallback(std::function<void()> callback) { createHdlBlockCallback_ = std::move(callback); }
    void setDeleteHdlBlockCallback(std::function<void(openvlsi::ObjectId)> callback) {
        deleteHdlBlockCallback_ = std::move(callback);
    }
    // Creates the project block backing the scratch editor as soon as the user
    // types into it, and returns the new cell id (0 on failure).
    void setCreateTemporaryHdlBlockCallback(std::function<openvlsi::ObjectId(const std::string &)> callback) {
        createTemporaryHdlBlockCallback_ = std::move(callback);
    }
    void setRenameHdlBlockCallback(std::function<bool(openvlsi::ObjectId, const std::string &)> callback) {
        renameHdlBlockCallback_ = std::move(callback);
    }
    // Name the scratch editor will give its block, so its tab label already
    // matches the block created on the first keystroke.
    void setHdlDefaultBlockNameCallback(std::function<QString()> callback) {
        hdlDefaultBlockNameCallback_ = std::move(callback);
    }
    openvlsi::ObjectId activeHdlCellId() const noexcept { return activeHdlCellId_; }
    // Flushes pending edits and, when the open block is still the unnamed
    // scratch block, asks the user for a name. Returns false when there is
    // nothing to save or the prompt was cancelled.
    bool prepareHdlFileSave(openvlsi::ObjectId &cellId);
    void setElaborateDesignCallback(std::function<void(openvlsi::ObjectId, const std::string &)> callback) {
        elaborateDesignCallback_ = std::move(callback);
    }
    void setSynthesizeRunCallback(std::function<void(openvlsi::ObjectId, const std::string &)> callback) {
        synthesizeRunCallback_ = std::move(callback);
    }
    void setExportVerilogCallback(std::function<void(openvlsi::ObjectId)> callback) {
        exportVerilogCallback_ = std::move(callback);
    }
    void setRemoveRunCallback(std::function<void(openvlsi::ObjectId)> callback) { removeRunCallback_ = std::move(callback); }
    void setApplyConfigCallback(std::function<void(openvlsi::ObjectId, const openvlsi::ImplementationConfig &)> callback) {
        applyConfigCallback_ = std::move(callback);
    }
    void setRunSelectionCallback(std::function<void(openvlsi::ObjectId, std::optional<openvlsi::ImplementationStage>)> callback) {
        runSelectionCallback_ = std::move(callback);
    }
    void
    setDrcReportCallback(std::function<void(const openvlsi::VerificationReport &, const QString &stageLabel)> callback) {
        drcReportCallback_ = std::move(callback);
    }
    void setTimingMetricsCallback(std::function<void(const std::vector<std::pair<QString, QString>> &)> callback) {
        timingMetricsCallback_ = std::move(callback);
    }
    // Selects a run (and optionally one of its stages) in the Runs tree
    // without emitting the user-selection signal.
    void selectRunStage(openvlsi::ObjectId runId, std::optional<openvlsi::ImplementationStage> stage);
    bool selectDrcMarker(int index);
    void selectHdlCell(openvlsi::ObjectId id);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void buildUi();
    void refreshRunTree();
    void updateStageRunButtons();
    void refreshRunCombo();
    void refreshExplorer();
    void applyHdlFilter(const QString &text);
    void openHdlFile(openvlsi::ObjectId id);
    void closeHdlFile(openvlsi::ObjectId id);
    void ensureHdlScratchEditor();
    void promoteHdlScratchEditor(openvlsi::ObjectId id);
    void closeHdlScratchEditor();
    void flushHdlEdit();
    TextDocumentEditor *hdlEditorFor(openvlsi::ObjectId id) const;
    void refreshRunsTable();
    void applyRunFilter(const QString &text);
    void updateRunActions();
    openvlsi::ObjectId selectedRunId() const;
    openvlsi::ObjectId actionRunId() const;
    // First physical stage in floorplan..target that has not succeeded yet, or
    // target when every stage up to it already succeeded.
    static openvlsi::ImplementationStage runStartStage(const openvlsi::ImplementationRun &run, openvlsi::ImplementationStage target);
    // True when the run's RTL sources no longer match the snapshot prepared for
    // its latest stage run, so a previous synthesis result is stale.
    bool sourcesChangedSincePrepare(const openvlsi::ImplementationRun &run) const;
    void showRunGraph(openvlsi::ObjectId runId);
    QString runStageLabel(const openvlsi::ImplementationRun &run) const;
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
    bool hdlMode() const { return stage_ == openvlsi::ImplementationStage::created; }
    bool synthesisMode() const { return stage_ == openvlsi::ImplementationStage::synthesis; }
    bool floorplanMode() const { return stage_ == openvlsi::ImplementationStage::floorplan; }
    bool physicalStageMode() const {
        return stage_ == openvlsi::ImplementationStage::floorplan || stage_ == openvlsi::ImplementationStage::placement ||
               stage_ == openvlsi::ImplementationStage::cts || stage_ == openvlsi::ImplementationStage::routing ||
               stage_ == openvlsi::ImplementationStage::final;
    }

    openvlsi::ImplementationStage stage_;
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
    TextDocumentEditor *hdlScratchEditor_ = nullptr;
    std::map<openvlsi::ObjectId, TextDocumentEditor *> hdlEditors_;
    QTimer *hdlEditTimer_ = nullptr;
    openvlsi::ObjectId activeHdlCellId_ = 0;
    openvlsi::ObjectId hdlEditCellId_ = 0;
    // Block created from the scratch editor; its first save asks for a name.
    openvlsi::ObjectId hdlTempCellId_ = 0;
    bool hdlScratchCreating_ = false;
    bool hdlScratchDismissed_ = false;
    openvlsi::ObjectId lastRunTreeActiveRunId_ = 0;
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
    QPushButton *cancelStageButton_ = nullptr;
    QPushButton *runAllButton_ = nullptr;
    QComboBox *runCombo_ = nullptr;
    QLabel *stageHintLabel_ = nullptr;
    QComboBox *heatMapCombo_ = nullptr;
    ImplementationPreviewCanvas *previewCanvas_ = nullptr;
    std::map<std::string, QWidget *> artifactTabs_;
    QWidget *middlePlaceholder_ = nullptr;
    std::function<void()> createHdlBlockCallback_;
    std::function<void(openvlsi::ObjectId)> deleteHdlBlockCallback_;
    std::function<openvlsi::ObjectId(const std::string &)> createTemporaryHdlBlockCallback_;
    std::function<bool(openvlsi::ObjectId, const std::string &)> renameHdlBlockCallback_;
    std::function<QString()> hdlDefaultBlockNameCallback_;
    std::function<void(openvlsi::ObjectId, const std::string &)> elaborateDesignCallback_;
    std::function<void(openvlsi::ObjectId, const std::string &)> synthesizeRunCallback_;
    std::function<void(openvlsi::ObjectId)> exportVerilogCallback_;
    std::function<void(openvlsi::ObjectId)> removeRunCallback_;
    std::function<void(openvlsi::ObjectId, const openvlsi::ImplementationConfig &)> applyConfigCallback_;
    std::function<void(openvlsi::ObjectId, std::optional<openvlsi::ImplementationStage>)> runSelectionCallback_;
    std::function<void(const openvlsi::VerificationReport &, const QString &stageLabel)> drcReportCallback_;
    std::function<void(const std::vector<std::pair<QString, QString>> &)> timingMetricsCallback_;
};

// One top-level workspace that hosts the four physical stages. Stage
// selection lives in each pane's Runs tree; the page itself only swaps panes.
class PhysicalDesignPage : public QWidget {
  public:
    explicit PhysicalDesignPage(ImplementationUiController *controller, QWidget *parent = nullptr);

    void setCurrentStage(openvlsi::ImplementationStage stage);
    openvlsi::ImplementationStage currentStage() const;
    std::vector<ImplementationPage *> pages() const;
    ImplementationPage *pageForStage(openvlsi::ImplementationStage stage) const;

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    int stageIndex(openvlsi::ImplementationStage stage) const;
    openvlsi::ImplementationStage stageAt(int index) const;
    void selectRunStage(openvlsi::ObjectId runId, std::optional<openvlsi::ImplementationStage> stage);

    QStackedWidget *stageStack_ = nullptr;
    std::vector<ImplementationPage *> pages_;
    std::vector<openvlsi::ImplementationStage> stages_;
};
