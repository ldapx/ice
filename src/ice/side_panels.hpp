#pragma once

#include "icelib/core.hpp"
#include "symbol_ui.hpp"

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
    void setOnRemoveClicked(std::function<void(ice::ObjectId)> callback);
    void setOnMakeLayoutFromSchematic(std::function<void(ice::ObjectId)> callback);
    void setOnActivateSchematic(std::function<void(ice::ObjectId)> callback);
    void setOnActivateLayout(std::function<void(ice::ObjectId)> callback);
    void setOnRegenerateProceduralLayout(std::function<void(ice::ObjectId)> callback);
    void setOnFlattenChanged(std::function<void()> callback);
    void setLayoutFlattenState(std::shared_ptr<std::set<ice::ObjectId>> state);
    void setProject(const ice::Project &project);

  private:
    ice::ObjectId selectedCellId() const;
    void updateRemoveButton();
    void rebuildTree();
    void showContextMenu(const QPoint &pos);
    static bool filterTreeItem(QTreeWidgetItem *item, const QString &needle);
    void applyFilter(const QString &text);

    std::optional<ice::Project> project_;
    std::shared_ptr<std::set<ice::ObjectId>> flattenedLayouts_;
    std::function<void()> onFlattenChanged_;
    QTreeWidget *tree_ = nullptr;
    QListWidget *packageList_ = nullptr;
    QListWidget *layerList_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    std::function<void()> onAdd_;
    std::function<void(ice::ObjectId)> onRemove_;
    std::function<void(ice::ObjectId)> onMakeLayoutFromSchematic_;
    std::function<void(ice::ObjectId)> onActivateSchematic_;
    std::function<void(ice::ObjectId)> onActivateLayout_;
    std::function<void(ice::ObjectId)> onRegenerateProceduralLayout_;
    bool showLayoutHierarchy_ = true;
};

class SimulationPanel : public QWidget {
  public:
    using SelectionCallback = std::function<void(ice::ObjectId simulationId, ice::ObjectId graphId)>;
    using ReportSelectionCallback = std::function<void(ice::ObjectId reportId)>;

    explicit SimulationPanel(QWidget *parent = nullptr);
    void setOnAddClicked(std::function<void()> cb);
    void setOnAddReport(std::function<void()> cb);
    void setOnOpenReport(std::function<void(ice::ObjectId)> cb);
    void setOnDeleteClicked(std::function<void()> cb);
    void setOnAddGraph(std::function<void(ice::ObjectId)> cb);
    void setOnDeleteGraph(std::function<void(ice::ObjectId, ice::ObjectId)> cb);
    void setOnDeleteReport(std::function<void(ice::ObjectId)> cb);
    void setOnSelection(SelectionCallback cb);
    void setOnReportSelection(ReportSelectionCallback cb);
    void setProject(const ice::Project &project);
    void clear();
    ice::ObjectId currentSimulationId() const;
    ice::ObjectId currentGraphId() const;
    ice::ObjectId currentReportId() const;
    void selectSimulation(ice::ObjectId id, ice::ObjectId graphId = 0);
    void selectReport(ice::ObjectId id);

  private:
    void applyFilter(const QString &text);
    void showContextMenu(const QPoint &pos);

    QTreeWidget *tree_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    SelectionCallback onSelection_;
    ReportSelectionCallback onReportSelection_;
    std::function<void()> onAdd_;
    std::function<void()> onDelete_;
    std::function<void()> onAddReport_;
    std::function<void(ice::ObjectId)> onOpenReport_;
    std::function<void(ice::ObjectId)> onAddGraph_;
    std::function<void(ice::ObjectId, ice::ObjectId)> onDeleteGraph_;
    std::function<void(ice::ObjectId)> onDeleteReport_;
};

class LayersPanel : public QWidget {
  public:
    explicit LayersPanel(QWidget *parent = nullptr);
    void setProject(const ice::Project &project);
    void setOnLayerVisibilityChanged(std::function<void(ice::ObjectId, bool)> callback);
    void setOnAllLayersVisibilityChanged(std::function<void(bool)> callback);

  private:
    void applyFilter(const QString &text);

    QTreeWidget *tree_ = nullptr;
    QLineEdit *search_ = nullptr;
    std::function<void(ice::ObjectId, bool)> onLayerVisibilityChanged_;
    std::function<void(bool)> onAllLayersVisibilityChanged_;
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
    std::function<void()> onSelectionChanged_;
    std::function<void()> onSymbolSelected_;
};

class SchematicNetlistPanel : public QWidget {
  public:
    explicit SchematicNetlistPanel(QWidget *parent = nullptr);
    void setCell(const ice::Project &project, const ice::SchematicCell *cell);
    void clear();

  private:
    static QString buildNetlistText(const ice::Project &project, const ice::SchematicCell *cell);
    void buildNetlist();
    void updateBuildButton();

    QPushButton *buildButton_ = nullptr;
    QPlainTextEdit *netlist_ = nullptr;
    ice::ObjectId currentCellId_ = 0;
    ice::ObjectId builtCellId_ = 0;
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
