#pragma once

#include "openvlsilib/core.hpp"
#include "symbol_ui.hpp"

#include <QIcon>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class ProjectPanel : public QWidget {
  public:
    explicit ProjectPanel(bool showLibraries = true, bool showLayers = true, bool showLayoutHierarchy = true,
                          bool showActions = false, QWidget *parent = nullptr);
    void setOnAddClicked(std::function<void()> callback);
    void setOnRemoveClicked(std::function<void(openvlsi::ObjectId)> callback);
    void setOnMakeLayoutFromSchematic(std::function<void(openvlsi::ObjectId)> callback);
    void setOnLinkSchematicToLayout(std::function<void(openvlsi::ObjectId)> callback);
    void setOnActivateSchematic(std::function<void(openvlsi::ObjectId)> callback);
    void setOnActivateLayout(std::function<void(openvlsi::ObjectId)> callback);
    using InstanceCallback = std::function<void(openvlsi::ObjectId ownerCellId, openvlsi::ObjectId instanceId)>;
    void setOnInstanceSelected(InstanceCallback callback);
    // Highlights the tree row for a placed instance without re-emitting the
    // selection callback. Used to mirror canvas selections into the hierarchy.
    void selectInstance(openvlsi::ObjectId instanceId);
    using PortCallback = std::function<void(openvlsi::ObjectId cellId, openvlsi::ObjectId portId)>;
    using PortVisibilityCallback = std::function<void(openvlsi::ObjectId cellId, openvlsi::ObjectId portId, bool visible)>;
    void setOnActivatePort(PortCallback callback);
    void setOnPortVisibilityChanged(PortVisibilityCallback callback);
    void setOnRegenerateProceduralLayout(std::function<void(openvlsi::ObjectId)> callback);
    void setOnRunPex(std::function<void(openvlsi::ObjectId)> callback);
    void setOnLinkLayoutToSchematic(std::function<void(openvlsi::ObjectId)> callback);
    void setOnUnlinkLayoutFromSchematic(std::function<void(openvlsi::ObjectId)> callback);
    using RenameCallback = std::function<void(openvlsi::ObjectId id, const std::string &name)>;
    using RenameChildCallback = std::function<void(openvlsi::ObjectId parentId, openvlsi::ObjectId id, const std::string &name)>;
    void setOnRenameLayout(RenameCallback callback);
    void setOnRenameProceduralLayout(RenameCallback callback);
    void setOnRenameInstance(RenameChildCallback callback);
    void setOnRenamePlannedDevice(RenameChildCallback callback);
    void setOnRenameShape(RenameChildCallback callback);
    void setOnFlattenChanged(std::function<void()> callback);
    void setLayoutFlattenState(std::shared_ptr<std::set<openvlsi::ObjectId>> state);
    void setHideSystemVerilog(bool hide);
    void setProject(const openvlsi::Project &project);

  private:
    openvlsi::ObjectId selectedCellId() const;
    void updateRemoveButton();
    void rebuildTree();
    void showContextMenu(const QPoint &pos);
    static bool filterTreeItem(QTreeWidgetItem *item, const QString &needle);
    void applyFilter(const QString &text);

    std::optional<openvlsi::Project> project_;
    std::shared_ptr<std::set<openvlsi::ObjectId>> flattenedLayouts_;
    std::function<void()> onFlattenChanged_;
    QTreeWidget *tree_ = nullptr;
    QListWidget *packageList_ = nullptr;
    QListWidget *layerList_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    std::function<void()> onAdd_;
    std::function<void(openvlsi::ObjectId)> onRemove_;
    std::function<void(openvlsi::ObjectId)> onMakeLayoutFromSchematic_;
    std::function<void(openvlsi::ObjectId)> onLinkSchematicToLayout_;
    std::function<void(openvlsi::ObjectId)> onActivateSchematic_;
    std::function<void(openvlsi::ObjectId)> onActivateLayout_;
    InstanceCallback onInstanceSelected_;
    PortCallback onActivatePort_;
    PortVisibilityCallback onPortVisibilityChanged_;
    std::function<void(openvlsi::ObjectId)> onRegenerateProceduralLayout_;
    std::function<void(openvlsi::ObjectId)> onRunPex_;
    std::function<void(openvlsi::ObjectId)> onLinkLayoutToSchematic_;
    std::function<void(openvlsi::ObjectId)> onUnlinkLayoutFromSchematic_;
    RenameCallback onRenameLayout_;
    RenameCallback onRenameProceduralLayout_;
    RenameChildCallback onRenameInstance_;
    RenameChildCallback onRenamePlannedDevice_;
    RenameChildCallback onRenameShape_;
    bool showLayoutHierarchy_ = true;
    bool hideSystemVerilog_ = false;
    bool rebuildingTree_ = false;
};

class SimulationPanel : public QWidget {
  public:
    using SelectionCallback = std::function<void(openvlsi::ObjectId simulationId, openvlsi::ObjectId graphId)>;
    using ReportSelectionCallback = std::function<void(openvlsi::ObjectId reportId, int graphIndex)>;

    explicit SimulationPanel(QWidget *parent = nullptr);
    void setOnAddClicked(std::function<void()> cb);
    void setOnAddReport(std::function<void()> cb);
    void setOnOpenReport(std::function<void(openvlsi::ObjectId)> cb);
    void setOnDeleteClicked(std::function<void()> cb);
    void setOnAddGraph(std::function<void(openvlsi::ObjectId)> cb);
    void setOnDeleteGraph(std::function<void(openvlsi::ObjectId, openvlsi::ObjectId)> cb);
    void setOnDeleteReport(std::function<void(openvlsi::ObjectId)> cb);
    void setOnSelection(SelectionCallback cb);
    void setOnReportSelection(ReportSelectionCallback cb);
    void setProjectRoot(const QString &projectRoot);
    void setProject(const openvlsi::Project &project);
    void clear();
    openvlsi::ObjectId currentSimulationId() const;
    openvlsi::ObjectId currentGraphId() const;
    openvlsi::ObjectId currentReportId() const;
    int currentReportGraphIndex() const;
    void selectSimulation(openvlsi::ObjectId id, openvlsi::ObjectId graphId = 0);
    void selectReport(openvlsi::ObjectId id, int graphIndex = -1);

  private:
    void applyFilter(const QString &text);
    void showContextMenu(const QPoint &pos);

    QTreeWidget *tree_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    QString projectRoot_;
    SelectionCallback onSelection_;
    ReportSelectionCallback onReportSelection_;
    std::function<void()> onAdd_;
    std::function<void()> onDelete_;
    std::function<void()> onAddReport_;
    std::function<void(openvlsi::ObjectId)> onOpenReport_;
    std::function<void(openvlsi::ObjectId)> onAddGraph_;
    std::function<void(openvlsi::ObjectId, openvlsi::ObjectId)> onDeleteGraph_;
    std::function<void(openvlsi::ObjectId)> onDeleteReport_;
};

class LayersPanel : public QWidget {
  public:
    explicit LayersPanel(QWidget *parent = nullptr);
    void setProject(const openvlsi::Project &project, openvlsi::ObjectId layoutId = 0);
    void setOnCreateLayer(std::function<void()> callback);
    void setOnLayerSelected(std::function<void(openvlsi::ObjectId)> callback);
    void setSelectedLayer(openvlsi::ObjectId layerId);
    void setOnLayerVisibilityChanged(std::function<void(openvlsi::ObjectId, bool)> callback);
    void setOnLayerGroupVisibilityChanged(std::function<void(const std::vector<openvlsi::ObjectId> &, bool)> callback);
    void setOnAllLayersVisibilityChanged(std::function<void(bool)> callback);
    void setOnShowDrawingLayers(std::function<void()> callback);

  private:
    void applyFilter(const QString &text);
    void showContextMenu(const QPoint &pos);
    void updateVisibilityButton();

    QTreeWidget *tree_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *visibilityButton_ = nullptr;
    std::function<void(openvlsi::ObjectId, bool)> onLayerVisibilityChanged_;
    std::function<void(const std::vector<openvlsi::ObjectId> &, bool)> onLayerGroupVisibilityChanged_;
    std::function<void(bool)> onAllLayersVisibilityChanged_;
    std::function<void()> onShowDrawingLayers_;
    std::function<void()> onCreateLayer_;
    std::function<void(openvlsi::ObjectId)> onLayerSelected_;
};

class SymbolsPanel : public QWidget {
  public:
    explicit SymbolsPanel(QWidget *parent = nullptr);
    void setOnSelectionChanged(std::function<void()> callback);
    void setOnSymbolSelected(std::function<void()> callback);
    void clearSymbolSelection();
    void selectSymbol(const QString &name);
    void setSymbols(std::vector<SymbolDefinition> symbols);
    void clear();
    const SymbolDefinition *selectedSymbol() const;

  private:
    void rebuildCategories();
    void rebuildList(const QString &keepSelected = {});

    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    std::vector<SymbolDefinition> symbols_;
    // Row icons rendered once per symbol set; the list stays cheap to filter.
    std::vector<QIcon> icons_;
    std::function<void()> onSelectionChanged_;
    std::function<void()> onSymbolSelected_;
};

class SchematicNetlistPanel : public QWidget {
  public:
    explicit SchematicNetlistPanel(QWidget *parent = nullptr);
    void setCell(const openvlsi::Project &project, const openvlsi::SchematicCell *cell);
    void clear();

  private:
    static QString buildNetlistText(const openvlsi::Project &project, const openvlsi::SchematicCell *cell);
    void buildNetlist();
    void updateBuildButton();

    QPushButton *buildButton_ = nullptr;
    QPlainTextEdit *netlist_ = nullptr;
    openvlsi::ObjectId currentCellId_ = 0;
    openvlsi::ObjectId builtCellId_ = 0;
    QString currentNetlist_;
    QString builtNetlist_;
};

class HistoryPanel : public QWidget {
  public:
    explicit HistoryPanel(QWidget *parent = nullptr);
    void setOnHistorySelected(std::function<void(std::size_t)> callback);
    void setHistory(const QStringList &entries, const std::vector<std::size_t> &positions);

  private:
    QListWidget *list_ = nullptr;
    std::vector<std::size_t> positions_;
    std::function<void(std::size_t)> onHistorySelected_;
};
