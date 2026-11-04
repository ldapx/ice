#include "side_panels.hpp"

#include "icons.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include "icelib/operations.hpp"
#include "icelib/pdk.hpp"

#include <QAction>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>
#include <utility>

namespace {
constexpr int kItemKindRole = Qt::UserRole + 1;
constexpr int kReportIdRole = Qt::UserRole + 2;
constexpr int kInstanceIdRole = Qt::UserRole + 3;
constexpr int kOwnerCellRole = Qt::UserRole + 4;
constexpr int kLayerGroupIdsRole = Qt::UserRole + 10;

enum class HierarchyItemKind : int {
    layout = 1,
    device = 2,
    plannedDevice = 4,
    proceduralLayout = 5,
    textSchematic = 6,
    rectangle = 7,
    polygon = 8,
    path = 9,
    text = 10,
    pin = 11,
    textPolygon = 12
};

enum class ShapeEntryKind : int { rectangle = 0, polygon = 1, path = 2, text = 3, textPolygon = 4 };

constexpr int kDeviceIconSize = 18;

// The schematic devices list puts the symbol artwork on the far right of the
// row, so the name reads first and the shape it draws sits beside it.
class TrailingDeviceIconDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.decorationPosition = QStyleOptionViewItem::Right;
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

ShapeEntryKind shapeEntryKind(const ice::Shape &shape) {
    if (shape.textGroup != 0)
        return ShapeEntryKind::textPolygon;
    if (shape.isText())
        return ShapeEntryKind::text;
    if (shape.isPath())
        return ShapeEntryKind::path;
    if (!shape.polygon.empty())
        return ShapeEntryKind::polygon;
    return ShapeEntryKind::rectangle;
}

const char *shapeEntryIcon(ShapeEntryKind kind) {
    switch (kind) {
    case ShapeEntryKind::rectangle:
        return "square";
    case ShapeEntryKind::polygon:
        return "pentagon";
    case ShapeEntryKind::path:
        return "route";
    case ShapeEntryKind::text:
        return "type";
    case ShapeEntryKind::textPolygon:
        return "type";
    }
    return "square";
}

const char *shapeEntryName(ShapeEntryKind kind) {
    switch (kind) {
    case ShapeEntryKind::rectangle:
        return "Rectangle";
    case ShapeEntryKind::polygon:
        return "Polygon";
    case ShapeEntryKind::path:
        return "Path";
    case ShapeEntryKind::text:
        return "Text";
    case ShapeEntryKind::textPolygon:
        return "Text Polygon";
    }
    return "Shape";
}

HierarchyItemKind shapeEntryHierarchyKind(ShapeEntryKind kind) {
    switch (kind) {
    case ShapeEntryKind::rectangle:
        return HierarchyItemKind::rectangle;
    case ShapeEntryKind::polygon:
        return HierarchyItemKind::polygon;
    case ShapeEntryKind::path:
        return HierarchyItemKind::path;
    case ShapeEntryKind::text:
        return HierarchyItemKind::text;
    case ShapeEntryKind::textPolygon:
        return HierarchyItemKind::textPolygon;
    }
    return HierarchyItemKind::rectangle;
}

QString shapeEntryDisplayName(ice::ObjectId id, const ice::Shape &shape) {
    if (!shape.name.empty())
        return QString::fromStdString(shape.name);
    return QStringLiteral("%1 #%2").arg(QString::fromLatin1(shapeEntryName(shapeEntryKind(shape)))).arg(id);
}

QIcon hierarchyIcon(const char *name) { return ice::lucideIcon(QString::fromLatin1(name), 16); }

QTreeWidgetItem *findInstanceItem(QTreeWidgetItem *item, ice::ObjectId instanceId) {
    if (!item)
        return nullptr;
    if (item->data(0, kInstanceIdRole).toULongLong() == static_cast<qulonglong>(instanceId))
        return item;
    for (int index = 0; index < item->childCount(); ++index)
        if (auto *found = findInstanceItem(item->child(index), instanceId))
            return found;
    return nullptr;
}

QString layerBaseName(const ice::Layer &layer) {
    const auto name = QString::fromStdString(layer.name);
    const auto dot = name.indexOf('.');
    return dot > 0 ? name.left(dot) : name;
}

QString layerPurposeLabel(const ice::Layer &layer) {
    const auto name = QString::fromStdString(layer.name);
    const auto dot = name.indexOf('.');
    if (dot >= 0 && dot + 1 < name.size())
        return name.mid(dot + 1);
    if (layer.datatype == 0)
        return QStringLiteral("drawing");
    return QStringLiteral("datatype");
}

QString layerVariantLabel(const ice::Layer &layer) {
    return QStringLiteral("%1 (dt %2)").arg(layerPurposeLabel(layer)).arg(layer.datatype);
}
} // namespace

ProjectPanel::ProjectPanel(bool showLibraries, bool showLayers, bool showLayoutHierarchy, bool showActions,
                           QWidget *parent)
    : QWidget(parent), flattenedLayouts_(std::make_shared<std::set<ice::ObjectId>>()),
      showLayoutHierarchy_(showLayoutHierarchy) {
    auto *layout = new QVBoxLayout(this);
    if (showActions) {
        auto *searchRow = new QHBoxLayout;
        search_ = new QLineEdit;
        search_->setPlaceholderText(showLayoutHierarchy_ ? QStringLiteral("Search layouts…")
                                                         : QStringLiteral("Search schematics…"));
        search_->setClearButtonEnabled(true);
        search_->setAccessibleName(showLayoutHierarchy_ ? QStringLiteral("Search layouts")
                                                        : QStringLiteral("Search schematics"));
        addButton_ = new QPushButton;
        addButton_->setFixedWidth(28);
        setLucideIcon(addButton_, QStringLiteral("plus"), 16);
        removeButton_ = new QPushButton;
        removeButton_->setFixedWidth(28);
        setLucideIcon(removeButton_, QStringLiteral("trash"), 16);
        removeButton_->setEnabled(false);
        if (showLayoutHierarchy_) {
            addButton_->setToolTip(QStringLiteral("Add layout"));
            addButton_->setAccessibleName(QStringLiteral("Add layout"));
            removeButton_->setToolTip(QStringLiteral("Remove selected layout"));
            removeButton_->setAccessibleName(QStringLiteral("Remove selected layout"));
        } else {
            addButton_->setToolTip(QStringLiteral("Add schematic"));
            addButton_->setAccessibleName(QStringLiteral("Add schematic"));
            removeButton_->setToolTip(QStringLiteral("Remove selected schematic"));
            removeButton_->setAccessibleName(QStringLiteral("Remove selected schematic"));
        }
        searchRow->addWidget(search_, 1);
        searchRow->addWidget(addButton_);
        searchRow->addWidget(removeButton_);
        layout->addLayout(searchRow);
    }
    tree_ = new QTreeWidget;
    tree_->setHeaderLabel(showLayoutHierarchy_ ? QStringLiteral("Layouts") : QStringLiteral("Schematics"));
    tree_->setAccessibleName(showLayoutHierarchy_ ? QStringLiteral("Layout hierarchy")
                                                  : QStringLiteral("Schematic hierarchy"));
    tree_->setIconSize(QSize(16, 16));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showContextMenu(pos); });
    layout->addWidget(tree_, 1);

    packageList_ = new QListWidget;
    packageList_->setAccessibleName(QStringLiteral("Project package dependencies"));
    if (showLibraries) {
        layout->addWidget(heading(QStringLiteral("Packages")));
        layout->addWidget(packageList_);
    }

    auto *layers = new QGroupBox(QStringLiteral("Layers"));
    auto *layerLayout = new QVBoxLayout(layers);
    layerList_ = new QListWidget;
    layerList_->setAccessibleName(QStringLiteral("Layer visibility"));
    layerLayout->addWidget(layerList_);
    if (showLayers)
        layout->addWidget(layers);

    if (search_)
        connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { applyFilter(text); });
    if (addButton_)
        connect(addButton_, &QPushButton::clicked, this, [this] {
            if (onAdd_)
                onAdd_();
        });
    if (removeButton_)
        connect(removeButton_, &QPushButton::clicked, this, [this] {
            const auto cellId = selectedCellId();
            if (cellId != 0 && onRemove_)
                onRemove_(cellId);
        });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        updateRemoveButton();
        if (rebuildingTree_ || !showLayoutHierarchy_ || !onInstanceSelected_ || !current)
            return;
        const auto instanceId = current->data(0, kInstanceIdRole).toULongLong();
        if (instanceId == 0)
            return;
        const auto *parent = current->parent();
        const auto ownerCellId = parent ? parent->data(0, Qt::UserRole).toULongLong() : 0;
        if (ownerCellId != 0)
            onInstanceSelected_(static_cast<ice::ObjectId>(ownerCellId), static_cast<ice::ObjectId>(instanceId));
    });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (!item || !item->data(0, Qt::UserRole).isValid())
            return;
        const auto id = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
        if (!showLayoutHierarchy_) {
            if (onActivateSchematic_)
                onActivateSchematic_(id);
            return;
        }
        const auto kind = item->data(0, kItemKindRole).toInt();
        if (kind == static_cast<int>(HierarchyItemKind::pin)) {
            const auto cellId = static_cast<ice::ObjectId>(item->data(0, kOwnerCellRole).toULongLong());
            if (onActivatePin_)
                onActivatePin_(cellId, id);
            return;
        }
        if (kind != static_cast<int>(HierarchyItemKind::layout) &&
            kind != static_cast<int>(HierarchyItemKind::proceduralLayout))
            return;
        if (onActivateLayout_)
            onActivateLayout_(id);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (rebuildingTree_ || !item || column != 0)
            return;
        if (item->data(0, kItemKindRole).toInt() != static_cast<int>(HierarchyItemKind::pin))
            return;
        const auto pinId = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
        const auto cellId = static_cast<ice::ObjectId>(item->data(0, kOwnerCellRole).toULongLong());
        if (pinId != 0 && cellId != 0 && onPinVisibilityChanged_)
            onPinVisibilityChanged_(cellId, pinId, item->checkState(0) == Qt::Checked);
    });
}

void ProjectPanel::setOnAddClicked(std::function<void()> callback) { onAdd_ = std::move(callback); }

void ProjectPanel::setOnRemoveClicked(std::function<void(ice::ObjectId)> callback) { onRemove_ = std::move(callback); }

void ProjectPanel::setOnMakeLayoutFromSchematic(std::function<void(ice::ObjectId)> callback) {
    onMakeLayoutFromSchematic_ = std::move(callback);
}

void ProjectPanel::setOnActivateSchematic(std::function<void(ice::ObjectId)> callback) {
    onActivateSchematic_ = std::move(callback);
}

void ProjectPanel::setOnActivateLayout(std::function<void(ice::ObjectId)> callback) {
    onActivateLayout_ = std::move(callback);
}

void ProjectPanel::setOnInstanceSelected(InstanceCallback callback) { onInstanceSelected_ = std::move(callback); }

void ProjectPanel::selectInstance(ice::ObjectId instanceId) {
    if (!tree_ || rebuildingTree_)
        return;
    {
        // Mirroring a canvas selection must not look like a tree click.
        const QSignalBlocker blocker(tree_);
        if (instanceId == 0) {
            if (const auto *current = tree_->currentItem(); current && current->data(0, kInstanceIdRole).isValid()) {
                tree_->setCurrentItem(nullptr);
                tree_->clearSelection();
            }
        } else {
            for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
                auto *item = findInstanceItem(tree_->topLevelItem(index), instanceId);
                if (!item)
                    continue;
                for (auto *parent = item->parent(); parent; parent = parent->parent())
                    parent->setExpanded(true);
                tree_->setCurrentItem(item);
                tree_->scrollToItem(item);
                break;
            }
        }
    }
    updateRemoveButton();
}

void ProjectPanel::setOnActivatePin(PinCallback callback) { onActivatePin_ = std::move(callback); }

void ProjectPanel::setOnPinVisibilityChanged(PinVisibilityCallback callback) {
    onPinVisibilityChanged_ = std::move(callback);
}

void ProjectPanel::setOnRegenerateProceduralLayout(std::function<void(ice::ObjectId)> callback) {
    onRegenerateProceduralLayout_ = std::move(callback);
}

void ProjectPanel::setOnRunPex(std::function<void(ice::ObjectId)> callback) { onRunPex_ = std::move(callback); }

void ProjectPanel::setOnRenameLayout(RenameCallback callback) { onRenameLayout_ = std::move(callback); }

void ProjectPanel::setOnRenameProceduralLayout(RenameCallback callback) {
    onRenameProceduralLayout_ = std::move(callback);
}

void ProjectPanel::setOnRenameInstance(RenameChildCallback callback) { onRenameInstance_ = std::move(callback); }

void ProjectPanel::setOnRenamePlannedDevice(RenameChildCallback callback) {
    onRenamePlannedDevice_ = std::move(callback);
}

void ProjectPanel::setOnRenameShape(RenameChildCallback callback) { onRenameShape_ = std::move(callback); }

void ProjectPanel::setOnFlattenChanged(std::function<void()> callback) { onFlattenChanged_ = std::move(callback); }

void ProjectPanel::setLayoutFlattenState(std::shared_ptr<std::set<ice::ObjectId>> state) {
    if (state)
        flattenedLayouts_ = std::move(state);
}

void ProjectPanel::setProject(const ice::Project &project) {
    project_ = project;
    std::erase_if(*flattenedLayouts_, [&](ice::ObjectId id) { return !project.layoutCells.contains(id); });
    rebuildTree();
}

void ProjectPanel::rebuildTree() {
    rebuildingTree_ = true;
    tree_->clear();
    layerList_->clear();
    packageList_->clear();
    if (!project_) {
        rebuildingTree_ = false;
        return;
    }
    const auto &project = *project_;
    if (project.packageDependencies.empty()) {
        auto *item = new QListWidgetItem(QStringLiteral("No package dependencies"), packageList_);
        item->setFlags(Qt::NoItemFlags);
    } else {
        for (const auto &dependency : project.packageDependencies)
            new QListWidgetItem(QString::fromStdString(dependency), packageList_);
    }

    if (!showLayoutHierarchy_) {
        std::map<ice::ObjectId, std::vector<ice::ObjectId>> children;
        for (const auto &[cellId, cell] : project.schematicCells) {
            if (cell.isTextSource())
                continue;
            auto &childCells = children[cellId];
            for (const auto &[_, instance] : cell.instances) {
                const auto symbol = cell.symbols.find(instance.symbol);
                if (symbol == cell.symbols.end())
                    continue;
                const auto childId = symbol->second.sourceCell;
                if (childId == 0 || childId == cellId || !project.schematicCells.contains(childId))
                    continue;
                if (std::find(childCells.begin(), childCells.end(), childId) == childCells.end())
                    childCells.push_back(childId);
            }
            std::sort(childCells.begin(), childCells.end(), [&](ice::ObjectId left, ice::ObjectId right) {
                return project.schematicCells.at(left).name < project.schematicCells.at(right).name;
            });
        }
        std::function<void(QTreeWidgetItem *, ice::ObjectId, std::vector<ice::ObjectId> &)> addChildren;
        addChildren = [&](QTreeWidgetItem *parentItem, ice::ObjectId parentId, std::vector<ice::ObjectId> &ancestors) {
            const auto childIt = children.find(parentId);
            if (childIt == children.end())
                return;
            for (const auto childId : childIt->second) {
                const auto cellIt = project.schematicCells.find(childId);
                if (cellIt == project.schematicCells.end())
                    continue;
                auto *childItem = new QTreeWidgetItem(parentItem, {QString::fromStdString(cellIt->second.name)});
                childItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(childId));
                childItem->setIcon(0, hierarchyIcon("puzzle"));
                childItem->setToolTip(0, QStringLiteral("Cell: %1").arg(QString::fromStdString(cellIt->second.name)));
                if (std::find(ancestors.begin(), ancestors.end(), childId) != ancestors.end())
                    continue;
                ancestors.push_back(childId);
                addChildren(childItem, childId, ancestors);
                ancestors.pop_back();
            }
        };
        for (const auto &[id, cell] : project.schematicCells) {
            auto *cellItem = new QTreeWidgetItem(tree_, {QString::fromStdString(cell.name)});
            cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            if (cell.isTextSource()) {
                cellItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::textSchematic));
                cellItem->setIcon(0, hierarchyIcon("notebook-text"));
                cellItem->setToolTip(0, (cell.systemVerilogSchematic ? QStringLiteral("SystemVerilog source: %1")
                                                                     : QStringLiteral("Text SPICE schematic: %1"))
                                            .arg(QString::fromStdString(cell.name)));
                continue;
            }
            cellItem->setIcon(0, hierarchyIcon("microchip"));
            cellItem->setToolTip(0, QStringLiteral("Schematic: %1").arg(QString::fromStdString(cell.name)));
            std::vector<ice::ObjectId> ancestors{id};
            addChildren(cellItem, id, ancestors);
        }
        tree_->expandAll();
        if (search_)
            applyFilter(search_->text());
        updateRemoveButton();
        rebuildingTree_ = false;
        return;
    }

    for (const auto &[id, layer] : project.layers) {
        auto *item = new QListWidgetItem(QString::fromStdString(layer.name), layerList_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
    }
    struct LayoutChild {
        ice::ObjectId instanceId = 0;
        std::string instanceName;
        ice::ObjectId cellId = 0;
    };
    std::map<ice::ObjectId, std::vector<LayoutChild>> children;
    for (const auto &[cellId, cell] : project.layoutCells) {
        auto &childInstances = children[cellId];
        for (const auto &[instanceId, instance] : cell.instances) {
            if (instance.cell == 0 || instance.cell == cellId || !project.layoutCells.contains(instance.cell))
                continue;
            childInstances.push_back({instanceId, instance.name, instance.cell});
        }
        std::sort(childInstances.begin(), childInstances.end(), [&](const auto &left, const auto &right) {
            if (left.instanceName != right.instanceName)
                return left.instanceName < right.instanceName;
            return project.layoutCells.at(left.cellId).name < project.layoutCells.at(right.cellId).name;
        });
    }
    std::function<void(QTreeWidgetItem *, ice::ObjectId, std::vector<ice::ObjectId> &)> addContents;
    addContents = [&](QTreeWidgetItem *layoutItem, ice::ObjectId layoutId, std::vector<ice::ObjectId> &ancestors) {
        const auto layoutIt = project.layoutCells.find(layoutId);
        QTreeWidgetItem *parentItem = layoutItem;
        const auto childIt = children.find(layoutId);
        if (childIt != children.end()) {
            for (const auto &child : childIt->second) {
                const auto cellIt = project.layoutCells.find(child.cellId);
                if (cellIt == project.layoutCells.end())
                    continue;
                const bool device = cellIt->second.library || !cellIt->second.device.empty();
                auto *childItem = new QTreeWidgetItem(parentItem, {QString::fromStdString(child.instanceName)});
                childItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(child.cellId));
                childItem->setData(0, kInstanceIdRole, QVariant::fromValue<qulonglong>(child.instanceId));
                childItem->setData(0, kItemKindRole,
                                   static_cast<int>(device ? HierarchyItemKind::device : HierarchyItemKind::layout));
                childItem->setIcon(0, hierarchyIcon(device ? "puzzle" : "notebook-pen"));
                childItem->setToolTip(0, QString::fromStdString("Cell: " + cellIt->second.name));
                if (std::find(ancestors.begin(), ancestors.end(), child.cellId) != ancestors.end())
                    continue;
                ancestors.push_back(child.cellId);
                addContents(childItem, child.cellId, ancestors);
                ancestors.pop_back();
            }
        }
        if (layoutIt == project.layoutCells.end())
            return;
        std::vector<std::pair<ice::ObjectId, const ice::Port *>> pinEntries;
        pinEntries.reserve(layoutIt->second.ports.size());
        for (const auto &[pinId, pin] : layoutIt->second.ports)
            pinEntries.emplace_back(pinId, &pin);
        std::sort(pinEntries.begin(), pinEntries.end(), [](const auto &left, const auto &right) {
            if (left.second->name != right.second->name)
                return left.second->name < right.second->name;
            return left.first < right.first;
        });
        if (!pinEntries.empty()) {
            auto *pinsItem = new QTreeWidgetItem(parentItem, {QStringLiteral("Pins")});
            pinsItem->setIcon(0, hierarchyIcon("circle-dot"));
            pinsItem->setToolTip(0, QStringLiteral("Layout pins"));
            for (const auto &[pinId, pin] : pinEntries) {
                auto *pinItem = new QTreeWidgetItem(pinsItem, {QString::fromStdString(pin->name)});
                pinItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(pinId));
                pinItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::pin));
                pinItem->setData(0, kOwnerCellRole, QVariant::fromValue<qulonglong>(layoutId));
                pinItem->setFlags(pinItem->flags() | Qt::ItemIsUserCheckable);
                pinItem->setCheckState(0, pin->visible ? Qt::Checked : Qt::Unchecked);
                pinItem->setIcon(0, hierarchyIcon("circle-dot"));
                QString tooltip =
                    QStringLiteral("%1 pin: %2 (%3, %4)")
                        .arg(QString::fromStdString(layoutIt->second.name), QString::fromStdString(pin->name),
                             QString::fromStdString(ice::portDirectionName(pin->direction)),
                             QString::fromStdString(ice::portSideName(pin->side)));
                if (pin->net != 0) {
                    if (const auto net = layoutIt->second.nets.find(pin->net); net != layoutIt->second.nets.end())
                        tooltip += QStringLiteral(" · net %1").arg(QString::fromStdString(net->second.name));
                }
                pinItem->setToolTip(0, tooltip);
            }
        }
        for (const auto &[deviceId, device] : layoutIt->second.plannedDevices) {
            auto *deviceItem = new QTreeWidgetItem(parentItem, {QString::fromStdString(device.name)});
            deviceItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(deviceId));
            deviceItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::plannedDevice));
            deviceItem->setIcon(0, hierarchyIcon("box"));
            deviceItem->setToolTip(0, QStringLiteral("Planned device: %1").arg(QString::fromStdString(device.name)));
        }
        if (layoutIt->second.library || !layoutIt->second.device.empty())
            return;
        std::vector<std::pair<ice::ObjectId, const ice::Shape *>> shapeEntries;
        shapeEntries.reserve(layoutIt->second.shapes.size());
        std::map<ice::ObjectId, const ice::Shape *> textPolygonEntries;
        for (const auto &[shapeId, shape] : layoutIt->second.shapes) {
            if (shape.textGroup != 0) {
                textPolygonEntries.emplace(shape.textGroup, &shape);
                continue;
            }
            shapeEntries.emplace_back(shapeId, &shape);
        }
        for (const auto &[groupId, shape] : textPolygonEntries)
            shapeEntries.emplace_back(groupId, shape);
        std::sort(shapeEntries.begin(), shapeEntries.end(), [](const auto &left, const auto &right) {
            const auto leftKind = shapeEntryKind(*left.second);
            const auto rightKind = shapeEntryKind(*right.second);
            if (leftKind != rightKind)
                return static_cast<int>(leftKind) < static_cast<int>(rightKind);
            return left.first < right.first;
        });
        for (const auto &[shapeId, shape] : shapeEntries) {
            const auto kind = shapeEntryKind(*shape);
            const auto displayName = shapeEntryDisplayName(shapeId, *shape);
            auto *shapeItem = new QTreeWidgetItem(parentItem, {displayName});
            shapeItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(shapeId));
            shapeItem->setData(0, kItemKindRole, static_cast<int>(shapeEntryHierarchyKind(kind)));
            shapeItem->setIcon(0, hierarchyIcon(shapeEntryIcon(kind)));
            auto tooltip = QStringLiteral("%1 shape: %2").arg(QString::fromLatin1(shapeEntryName(kind)), displayName);
            if (kind == ShapeEntryKind::textPolygon && !shape->textSource.empty())
                tooltip += QStringLiteral(" · %1").arg(QString::fromStdString(shape->textSource));
            shapeItem->setToolTip(0, tooltip);
        }
    };
    std::set<ice::ObjectId> generatedCells;
    for (const auto &[id, procedural] : project.proceduralLayouts) {
        if (procedural.generatedCell != 0)
            generatedCells.insert(procedural.generatedCell);
        auto *proceduralItem = new QTreeWidgetItem(tree_, {QString::fromStdString(procedural.name)});
        proceduralItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        proceduralItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::proceduralLayout));
        proceduralItem->setIcon(0, hierarchyIcon("notebook-text"));
        proceduralItem->setToolTip(
            0, QStringLiteral("Procedural layout (Python script): %1").arg(QString::fromStdString(procedural.name)));
        const auto generatedIt = project.layoutCells.find(procedural.generatedCell);
        if (generatedIt == project.layoutCells.end())
            continue;
        auto *generatedItem = new QTreeWidgetItem(proceduralItem, {QString::fromStdString(generatedIt->second.name)});
        generatedItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(generatedIt->first));
        generatedItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::layout));
        generatedItem->setIcon(0, hierarchyIcon("notebook-pen"));
        generatedItem->setToolTip(
            0, QStringLiteral("Generated static layout: %1").arg(QString::fromStdString(generatedIt->second.name)));
        std::vector<ice::ObjectId> ancestors{generatedIt->first};
        addContents(generatedItem, generatedIt->first, ancestors);
    }
    for (const auto &[id, cell] : project.layoutCells) {
        if (cell.library || generatedCells.contains(id))
            continue;
        auto *cellItem = new QTreeWidgetItem(tree_, {QString::fromStdString(cell.name)});
        cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        cellItem->setData(0, kItemKindRole, static_cast<int>(HierarchyItemKind::layout));
        cellItem->setIcon(0, hierarchyIcon("notebook-pen"));
        cellItem->setToolTip(0, QStringLiteral("Layout: %1").arg(QString::fromStdString(cell.name)));
        std::vector<ice::ObjectId> ancestors{id};
        addContents(cellItem, id, ancestors);
    }
    tree_->expandAll();
    if (search_)
        applyFilter(search_->text());
    updateRemoveButton();
    rebuildingTree_ = false;
}

void ProjectPanel::showContextMenu(const QPoint &pos) {
    if (!project_)
        return;
    auto *item = tree_->itemAt(pos);
    if (!item)
        return;
    if (!showLayoutHierarchy_) {
        if (!onMakeLayoutFromSchematic_ || !item->data(0, Qt::UserRole).isValid())
            return;
        const auto cellId = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
        const auto cell = project_->schematicCells.find(cellId);
        if (cellId == 0 || cell == project_->schematicCells.end() || cell->second.isTextSource())
            return;
        QMenu menu(this);
        auto *makeLayoutAction = menu.addAction(QStringLiteral("Make Layout from Schematic"));
        setLucideIcon(makeLayoutAction, QStringLiteral("notebook-pen"));
        if (menu.exec(tree_->viewport()->mapToGlobal(pos)) == makeLayoutAction)
            onMakeLayoutFromSchematic_(cellId);
        return;
    }
    const auto kind = item->data(0, kItemKindRole).toInt();
    const auto id = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
    QMenu menu(this);
    QAction *regenerateAction = nullptr;
    QAction *openAction = nullptr;
    QAction *flattenAction = nullptr;
    QAction *pexAction = nullptr;
    QAction *renameAction = nullptr;
    std::function<void(const std::string &)> renameHandler;

    const auto addRename = [&](const QString &title, std::function<void(const std::string &)> handler) {
        renameAction = menu.addAction(title);
        setLucideIcon(renameAction, QStringLiteral("pencil"));
        renameHandler = std::move(handler);
    };

    if (kind == static_cast<int>(HierarchyItemKind::proceduralLayout)) {
        if (id == 0 || !project_->proceduralLayouts.contains(id))
            return;
        regenerateAction = menu.addAction(QStringLiteral("Regenerate layout"));
        setLucideIcon(regenerateAction, QStringLiteral("refresh-cw"));
        openAction = menu.addAction(QStringLiteral("Open script"));
        setLucideIcon(openAction, QStringLiteral("notebook-text"));
        if (onRenameProceduralLayout_)
            addRename(QStringLiteral("Rename Script…"),
                      [this, id](const std::string &name) { onRenameProceduralLayout_(id, name); });
    } else if (kind == static_cast<int>(HierarchyItemKind::layout)) {
        const auto *parent = item->parent();
        const auto parentKind = parent ? parent->data(0, kItemKindRole).toInt() : -1;
        const bool isCell = !parent || parentKind == static_cast<int>(HierarchyItemKind::proceduralLayout);
        if (isCell) {
            if (id == 0 || !project_->layoutCells.contains(id))
                return;
            if (onRenameLayout_)
                addRename(QStringLiteral("Rename Layout…"),
                          [this, id](const std::string &name) { onRenameLayout_(id, name); });
        } else {
            const auto parentId = parent->data(0, Qt::UserRole).toULongLong();
            const auto instanceId = item->data(0, kInstanceIdRole).toULongLong();
            if (parentId != 0 && instanceId != 0 && onRenameInstance_)
                addRename(QStringLiteral("Rename Instance…"), [this, parentId, instanceId](const std::string &name) {
                    onRenameInstance_(parentId, instanceId, name);
                });
        }
        if (id != 0 && project_->layoutCells.contains(id)) {
            flattenAction = menu.addAction(QStringLiteral("Flatten Layout for DRC"));
            flattenAction->setCheckable(true);
            flattenAction->setChecked(flattenedLayouts_->contains(id));
            if (isCell && !project_->layoutCells.at(id).library && onRunPex_)
                pexAction = menu.addAction(QStringLiteral("Extract Parasitics (PEX)"));
        }
    } else if (kind == static_cast<int>(HierarchyItemKind::device)) {
        const auto *parent = item->parent();
        const auto parentId = parent ? parent->data(0, Qt::UserRole).toULongLong() : 0;
        const auto instanceId = item->data(0, kInstanceIdRole).toULongLong();
        if (parentId != 0 && instanceId != 0 && onRenameInstance_)
            addRename(QStringLiteral("Rename Instance…"), [this, parentId, instanceId](const std::string &name) {
                onRenameInstance_(parentId, instanceId, name);
            });
    } else if (kind == static_cast<int>(HierarchyItemKind::plannedDevice)) {
        const auto *parent = item->parent();
        const auto parentId = parent ? parent->data(0, Qt::UserRole).toULongLong() : 0;
        if (parentId != 0 && id != 0 && onRenamePlannedDevice_)
            addRename(QStringLiteral("Rename Device…"),
                      [this, parentId, id](const std::string &name) { onRenamePlannedDevice_(parentId, id, name); });
    } else if (kind == static_cast<int>(HierarchyItemKind::rectangle) ||
               kind == static_cast<int>(HierarchyItemKind::polygon) ||
               kind == static_cast<int>(HierarchyItemKind::path) || kind == static_cast<int>(HierarchyItemKind::text) ||
               kind == static_cast<int>(HierarchyItemKind::textPolygon)) {
        const auto *parent = item->parent();
        const auto parentId = parent ? parent->data(0, Qt::UserRole).toULongLong() : 0;
        if (parentId != 0 && id != 0 && onRenameShape_)
            addRename(QStringLiteral("Rename Shape…"),
                      [this, parentId, id](const std::string &name) { onRenameShape_(parentId, id, name); });
    } else {
        return;
    }

    if (menu.actions().isEmpty())
        return;
    auto *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == regenerateAction && onRegenerateProceduralLayout_)
        onRegenerateProceduralLayout_(id);
    else if (chosen == openAction && onActivateLayout_)
        onActivateLayout_(id);
    else if (chosen == flattenAction) {
        if (flattenedLayouts_->contains(id))
            flattenedLayouts_->erase(id);
        else
            flattenedLayouts_->insert(id);
        rebuildTree();
    } else if (chosen == pexAction && onRunPex_) {
        onRunPex_(id);
    } else if (chosen == renameAction) {
        bool ok = false;
        const auto current = item->text(0);
        const auto name =
            QInputDialog::getText(this, renameAction->text(), QStringLiteral("Name:"), QLineEdit::Normal, current, &ok)
                .trimmed();
        if (ok && !name.isEmpty() && name != current && renameHandler)
            renameHandler(name.toStdString());
    }
}

ice::ObjectId ProjectPanel::selectedCellId() const {
    const auto *item = tree_->currentItem();
    if (!item || item->parent() != nullptr || item->isHidden() || !item->data(0, Qt::UserRole).isValid())
        return 0;
    return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
}

void ProjectPanel::updateRemoveButton() {
    if (removeButton_)
        removeButton_->setEnabled(selectedCellId() != 0);
}

bool ProjectPanel::filterTreeItem(QTreeWidgetItem *item, const QString &needle) {
    bool childMatches = false;
    for (int index = 0; index < item->childCount(); ++index)
        childMatches |= filterTreeItem(item->child(index), needle);
    const bool selfMatches = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
    const bool visible = selfMatches || childMatches;
    item->setHidden(!visible);
    return visible;
}

void ProjectPanel::applyFilter(const QString &text) {
    const auto needle = text.trimmed();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index)
        filterTreeItem(tree_->topLevelItem(index), needle);
    if (!needle.isEmpty())
        tree_->expandAll();
    updateRemoveButton();
}

SimulationPanel::SimulationPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *toolbarRow = new QHBoxLayout;
    toolbarRow->setContentsMargins(4, 0, 4, 4);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search simulations and simulation scripts…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Search simulations and simulation scripts"));
    addButton_ = new QPushButton;
    addButton_->setToolTip(QStringLiteral("Add simulation, SystemVerilog testbench, or simulation script"));
    addButton_->setAccessibleName(QStringLiteral("Add simulation, SystemVerilog testbench, or simulation script"));
    addButton_->setFixedWidth(28);
    setLucideIcon(addButton_, QStringLiteral("plus"), 16);
    deleteButton_ = new QPushButton;
    deleteButton_->setToolTip(QStringLiteral("Delete selected simulation or simulation script"));
    deleteButton_->setAccessibleName(QStringLiteral("Delete selected simulation or simulation script"));
    deleteButton_->setFixedWidth(28);
    setLucideIcon(deleteButton_, QStringLiteral("trash"), 16);
    deleteButton_->setEnabled(false);
    toolbarRow->addWidget(search_, 1);
    toolbarRow->addWidget(addButton_);
    toolbarRow->addWidget(deleteButton_);
    layout->addLayout(toolbarRow);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabel(QStringLiteral("Hierarchy"));
    tree_->setAccessibleName(QStringLiteral("Simulation hierarchy"));
    tree_->setIconSize(QSize(16, 16));
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { applyFilter(text); });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *) {
        const auto reportId = currentReportId();
        deleteButton_->setEnabled(currentSimulationId() != 0 || reportId != 0);
        if (reportId != 0) {
            if (onReportSelection_)
                onReportSelection_(reportId);
            return;
        }
        if (onSelection_)
            onSelection_(currentSimulationId(), currentGraphId());
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showContextMenu(pos); });
    connect(addButton_, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        auto *simulationAction = menu.addAction(QStringLiteral("Add Simulation…"));
        setLucideIcon(simulationAction, QStringLiteral("microscope"));
        auto *testbenchAction = menu.addAction(QStringLiteral("Add SystemVerilog Testbench…"));
        setLucideIcon(testbenchAction, QStringLiteral("notebook-text"));
        auto *reportAction = menu.addAction(QStringLiteral("Add Simulation Script…"));
        setLucideIcon(reportAction, QStringLiteral("notebook-text"));
        const auto *chosen = menu.exec(addButton_->mapToGlobal(QPoint(0, addButton_->height())));
        if (chosen == simulationAction && onAdd_) {
            onAdd_();
        } else if (chosen == testbenchAction && onAddSystemVerilogTestbench_) {
            onAddSystemVerilogTestbench_();
        } else if (chosen == reportAction && onAddReport_) {
            onAddReport_();
        }
    });
    connect(deleteButton_, &QPushButton::clicked, this, [this] {
        const auto reportId = currentReportId();
        if (reportId != 0 && onDeleteReport_)
            onDeleteReport_(reportId);
        else if (onDelete_)
            onDelete_();
    });
}

void SimulationPanel::setOnAddClicked(std::function<void()> cb) { onAdd_ = std::move(cb); }

void SimulationPanel::setOnAddSystemVerilogTestbench(std::function<void()> cb) {
    onAddSystemVerilogTestbench_ = std::move(cb);
}

void SimulationPanel::setOnAddReport(std::function<void()> cb) { onAddReport_ = std::move(cb); }
void SimulationPanel::setOnOpenReport(std::function<void(ice::ObjectId)> cb) { onOpenReport_ = std::move(cb); }

void SimulationPanel::setOnDeleteClicked(std::function<void()> cb) { onDelete_ = std::move(cb); }

void SimulationPanel::setOnAddGraph(std::function<void(ice::ObjectId)> cb) { onAddGraph_ = std::move(cb); }
void SimulationPanel::setOnDeleteGraph(std::function<void(ice::ObjectId, ice::ObjectId)> cb) {
    onDeleteGraph_ = std::move(cb);
}
void SimulationPanel::setOnDeleteReport(std::function<void(ice::ObjectId)> cb) { onDeleteReport_ = std::move(cb); }
void SimulationPanel::setOnSelection(SelectionCallback cb) { onSelection_ = std::move(cb); }
void SimulationPanel::setOnReportSelection(ReportSelectionCallback cb) { onReportSelection_ = std::move(cb); }

void SimulationPanel::setProject(const ice::Project &project) {
    const ice::ObjectId prevSimId = currentSimulationId();
    const ice::ObjectId prevGraphId = currentGraphId();
    const ice::ObjectId prevReportId = currentReportId();
    {
        QSignalBlocker blocker(tree_);
        tree_->clear();
        for (const auto &[id, sim] : project.simulations) {
            auto *item = new QTreeWidgetItem(tree_, {QString::fromStdString(sim.name)});
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setIcon(0, hierarchyIcon("microscope"));
            for (const auto &graph : sim.graphs) {
                auto *graphItem = new QTreeWidgetItem(item, {QString::fromStdString(graph.name)});
                graphItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                graphItem->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(graph.id));
                graphItem->setIcon(0, hierarchyIcon("chart-spline"));
                const auto xAxis = graph.xAxis.empty() ? QStringLiteral("time") : QString::fromStdString(graph.xAxis);
                QStringList ySignals;
                for (const auto &signal : graph.ySignals)
                    ySignals << QString::fromStdString(signal);
                const auto yAxis = ySignals.isEmpty() ? QStringLiteral("auto") : ySignals.join(QStringLiteral(", "));
                graphItem->setToolTip(0, QStringLiteral("X: %1  Y: %2").arg(xAxis, yAxis));
            }
        }
        for (const auto &[id, report] : project.reports) {
            auto *reportItem = new QTreeWidgetItem(tree_, {QString::fromStdString(report.name)});
            reportItem->setData(0, kReportIdRole, QVariant::fromValue<qulonglong>(id));
            reportItem->setIcon(0, hierarchyIcon("notebook-text"));
            reportItem->setToolTip(0, QString::fromStdString(report.path));
        }
        tree_->expandAll();
        if (prevReportId != 0)
            selectReport(prevReportId);
        else if (prevSimId != 0)
            selectSimulation(prevSimId, prevGraphId);
        if (search_)
            applyFilter(search_->text());
    }
    const auto reportId = currentReportId();
    const auto simId = currentSimulationId();
    const auto graphId = currentGraphId();
    deleteButton_->setEnabled(currentSimulationId() != 0 || reportId != 0);
    if (reportId != 0)
        return;
    // Rebuilding the panel is part of ordinary project refreshes. Do not
    // treat the restored selection as a new user selection, otherwise the
    // main document tab jumps to Simulation after unrelated edits.
    if (onSelection_ && (simId != prevSimId || graphId != prevGraphId))
        onSelection_(simId, graphId);
}

void SimulationPanel::clear() {
    tree_->clear();
    if (search_)
        search_->clear();
    deleteButton_->setEnabled(false);
    if (onSelection_)
        onSelection_(0, 0);
}

ice::ObjectId SimulationPanel::currentSimulationId() const {
    auto *item = tree_->currentItem();
    if (!item)
        return 0;
    if (item->data(0, Qt::UserRole).isValid())
        return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
    return 0;
}

ice::ObjectId SimulationPanel::currentGraphId() const {
    auto *item = tree_->currentItem();
    if (!item || !item->data(0, Qt::UserRole + 1).isValid())
        return 0;
    return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole + 1).toULongLong());
}

ice::ObjectId SimulationPanel::currentReportId() const {
    auto *item = tree_->currentItem();
    if (!item || !item->data(0, kReportIdRole).isValid())
        return 0;
    return static_cast<ice::ObjectId>(item->data(0, kReportIdRole).toULongLong());
}

void SimulationPanel::selectSimulation(ice::ObjectId id, ice::ObjectId graphId) {
    if (id == 0) {
        tree_->setCurrentItem(nullptr);
        return;
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *item = tree_->topLevelItem(i);
        if (static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong()) != id)
            continue;
        QTreeWidgetItem *target = item;
        if (graphId != 0) {
            for (int g = 0; g < item->childCount(); ++g) {
                auto *graphItem = item->child(g);
                if (static_cast<ice::ObjectId>(graphItem->data(0, Qt::UserRole + 1).toULongLong()) == graphId) {
                    target = graphItem;
                    break;
                }
            }
        }
        tree_->setCurrentItem(target);
        return;
    }
    tree_->setCurrentItem(nullptr);
}

void SimulationPanel::selectReport(ice::ObjectId id) {
    if (id == 0) {
        tree_->setCurrentItem(nullptr);
        return;
    }
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto *item = tree_->topLevelItem(i);
        if (static_cast<ice::ObjectId>(item->data(0, kReportIdRole).toULongLong()) == id) {
            tree_->setCurrentItem(item);
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
}

void SimulationPanel::applyFilter(const QString &text) {
    const auto needle = text.trimmed();
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto *simItem = tree_->topLevelItem(index);
        const bool simMatches = needle.isEmpty() || simItem->text(0).contains(needle, Qt::CaseInsensitive);
        bool anyGraphVisible = false;
        for (int childIndex = 0; childIndex < simItem->childCount(); ++childIndex) {
            auto *graphItem = simItem->child(childIndex);
            const bool graphVisible = simMatches || graphItem->text(0).contains(needle, Qt::CaseInsensitive);
            graphItem->setHidden(!graphVisible);
            anyGraphVisible |= graphVisible;
        }
        simItem->setHidden(!simMatches && !anyGraphVisible);
    }
    if (!needle.isEmpty())
        tree_->expandAll();
    deleteButton_->setEnabled(currentSimulationId() != 0 || currentReportId() != 0);
}

void SimulationPanel::showContextMenu(const QPoint &pos) {
    auto *clicked = tree_->itemAt(pos);
    if (!clicked)
        return;
    if (clicked->data(0, kReportIdRole).isValid()) {
        const auto reportId = static_cast<ice::ObjectId>(clicked->data(0, kReportIdRole).toULongLong());
        QMenu menu(this);
        auto *openAction = menu.addAction(QStringLiteral("Open simulation script"));
        auto *deleteAction = menu.addAction(QStringLiteral("Delete simulation script"));
        const auto *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
        if (chosen == openAction && onOpenReport_)
            onOpenReport_(reportId);
        else if (chosen == deleteAction && onDeleteReport_)
            onDeleteReport_(reportId);
        return;
    }
    QTreeWidgetItem *simItem = clicked;
    while (simItem && !simItem->data(0, Qt::UserRole).isValid())
        simItem = simItem->parent();
    if (!simItem)
        return;
    const auto simId = static_cast<ice::ObjectId>(simItem->data(0, Qt::UserRole).toULongLong());
    const ice::ObjectId graphId = clicked->data(0, Qt::UserRole + 1).isValid()
                                      ? static_cast<ice::ObjectId>(clicked->data(0, Qt::UserRole + 1).toULongLong())
                                      : 0;
    QMenu menu(this);
    auto *addGraphAction = menu.addAction(QStringLiteral("Add graph…"));
    QAction *deleteGraphAction = nullptr;
    if (graphId != 0)
        deleteGraphAction = menu.addAction(QStringLiteral("Delete this graph"));
    const auto *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (chosen == addGraphAction && onAddGraph_)
        onAddGraph_(simId);
    else if (chosen == deleteGraphAction && onDeleteGraph_ && graphId != 0)
        onDeleteGraph_(simId, graphId);
}

LayersPanel::LayersPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search layers…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Search layers"));
    controls->addWidget(search_, 1);
    auto *drawingButton = new QPushButton;
    drawingButton->setFixedWidth(28);
    drawingButton->setToolTip(QStringLiteral("Show only drawing layers"));
    drawingButton->setAccessibleName(QStringLiteral("Show only drawing layers"));
    setLucideIcon(drawingButton, QStringLiteral("square"), 16);
    controls->addWidget(drawingButton);
    auto *pinButton = new QPushButton;
    pinButton->setFixedWidth(28);
    pinButton->setToolTip(QStringLiteral("Show only pin layers"));
    pinButton->setAccessibleName(QStringLiteral("Show only pin layers"));
    setLucideIcon(pinButton, QStringLiteral("circle-dot"), 16);
    controls->addWidget(pinButton);
    visibilityButton_ = new QPushButton;
    visibilityButton_->setFixedWidth(28);
    setLucideIcon(visibilityButton_, QStringLiteral("eye"), 16);
    controls->addWidget(visibilityButton_);
    layout->addLayout(controls);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabel(QStringLiteral("Layers"));
    tree_->setAccessibleName(QStringLiteral("Layer visibility"));
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(36, 18));
    tree_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(tree_, 1);
    setMinimumWidth(190);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    connect(drawingButton, &QPushButton::clicked, this, [this] {
        if (onLayerVisibilityFilter_)
            onLayerVisibilityFilter_(false);
    });
    connect(pinButton, &QPushButton::clicked, this, [this] {
        if (onLayerVisibilityFilter_)
            onLayerVisibilityFilter_(true);
    });
    connect(visibilityButton_, &QPushButton::clicked, this, [this] {
        if (!onAllLayersVisibilityChanged_)
            return;
        bool allVisible = true;
        bool anyLayer = false;
        for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
            const auto *item = tree_->topLevelItem(index);
            const auto include = [&](const QTreeWidgetItem *layerItem) {
                if (layerItem->data(0, Qt::UserRole).toULongLong() == 0)
                    return;
                anyLayer = true;
                if (layerItem->checkState(0) != Qt::Checked)
                    allVisible = false;
            };
            include(item);
            for (int child = 0; child < item->childCount(); ++child)
                include(item->child(child));
        }
        allVisible = anyLayer && allVisible;
        onAllLayersVisibilityChanged_(!allVisible);
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (!item || column != 0)
            return;
        const bool visible = item->checkState(0) == Qt::Checked;
        const auto groupIds = item->data(0, kLayerGroupIdsRole).toStringList();
        if (!groupIds.isEmpty()) {
            {
                QSignalBlocker blocker(tree_);
                for (int index = 0; index < item->childCount(); ++index)
                    item->child(index)->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
                item->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
            }
            std::vector<ice::ObjectId> ids;
            ids.reserve(groupIds.size());
            for (const auto &text : groupIds)
                ids.push_back(static_cast<ice::ObjectId>(text.toULongLong()));
            updateVisibilityButton();
            // The callback usually rebuilds this tree. Queue it so Qt is done
            // with the QTreeWidgetItem before the panel is repopulated.
            QTimer::singleShot(0, this, [this, ids = std::move(ids), visible] {
                if (onLayerGroupVisibilityChanged_)
                    onLayerGroupVisibilityChanged_(ids, visible);
            });
            return;
        }
        const auto layerId = static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
        if (layerId == 0 || !onLayerVisibilityChanged_)
            return;
        if (auto *parent = item->parent()) {
            QSignalBlocker blocker(tree_);
            int checked = 0;
            int unchecked = 0;
            for (int index = 0; index < parent->childCount(); ++index) {
                const auto state = parent->child(index)->checkState(0);
                checked += state == Qt::Checked ? 1 : 0;
                unchecked += state == Qt::Unchecked ? 1 : 0;
            }
            parent->setCheckState(0, checked == parent->childCount()
                                         ? Qt::Checked
                                         : (unchecked == parent->childCount() ? Qt::Unchecked : Qt::PartiallyChecked));
        }
        updateVisibilityButton();
        QTimer::singleShot(0, this, [this, layerId, visible] {
            if (onLayerVisibilityChanged_)
                onLayerVisibilityChanged_(layerId, visible);
        });
    });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { applyFilter(text); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showContextMenu(pos); });
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (!onLayerSelected_ || !tree_->currentItem())
            return;
        const auto id = static_cast<ice::ObjectId>(tree_->currentItem()->data(0, Qt::UserRole).toULongLong());
        if (id != 0)
            onLayerSelected_(id);
    });
    updateVisibilityButton();
}

void LayersPanel::setOnCreateLayer(std::function<void()> callback) { onCreateLayer_ = std::move(callback); }

void LayersPanel::setOnLayerSelected(std::function<void(ice::ObjectId)> callback) {
    onLayerSelected_ = std::move(callback);
}

void LayersPanel::setSelectedLayer(ice::ObjectId layerId) {
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        auto *item = tree_->topLevelItem(index);
        if (item->data(0, Qt::UserRole).toULongLong() == layerId) {
            tree_->setCurrentItem(item);
            return;
        }
        for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
            auto *child = item->child(childIndex);
            if (child->data(0, Qt::UserRole).toULongLong() != layerId)
                continue;
            item->setExpanded(true);
            tree_->setCurrentItem(child);
            return;
        }
    }
    tree_->setCurrentItem(nullptr);
}

void LayersPanel::setOnLayerVisibilityChanged(std::function<void(ice::ObjectId, bool)> callback) {
    onLayerVisibilityChanged_ = std::move(callback);
}

void LayersPanel::setOnLayerGroupVisibilityChanged(
    std::function<void(const std::vector<ice::ObjectId> &, bool)> callback) {
    onLayerGroupVisibilityChanged_ = std::move(callback);
}

void LayersPanel::setOnAllLayersVisibilityChanged(std::function<void(bool)> callback) {
    onAllLayersVisibilityChanged_ = std::move(callback);
}

void LayersPanel::setOnLayerVisibilityFilter(std::function<void(bool pinOnly)> callback) {
    onLayerVisibilityFilter_ = std::move(callback);
}

void LayersPanel::updateVisibilityButton() {
    if (!visibilityButton_)
        return;
    bool anyLayer = false;
    bool allVisible = true;
    const auto include = [&](const QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole).toULongLong() == 0)
            return;
        anyLayer = true;
        if (item->checkState(0) != Qt::Checked)
            allVisible = false;
    };
    for (int index = 0; index < tree_->topLevelItemCount(); ++index) {
        const auto *item = tree_->topLevelItem(index);
        include(item);
        for (int child = 0; child < item->childCount(); ++child)
            include(item->child(child));
    }
    visibilityButton_->setEnabled(anyLayer);
    const bool hideAll = anyLayer && allVisible;
    visibilityButton_->setToolTip(hideAll ? QStringLiteral("Hide all layers") : QStringLiteral("Show all layers"));
    visibilityButton_->setAccessibleName(visibilityButton_->toolTip());
    setLucideIcon(visibilityButton_, hideAll ? QStringLiteral("eye-off") : QStringLiteral("eye"), 16);
}

void LayersPanel::applyFilter(const QString &text) {
    QSignalBlocker blocker(tree_);
    const auto needle = text.trimmed();
    std::function<bool(QTreeWidgetItem *)> filter = [&](QTreeWidgetItem *item) {
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index)
            childMatches |= filter(item->child(index));
        const bool selfMatches = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        if (selfMatches && !needle.isEmpty()) {
            for (int index = 0; index < item->childCount(); ++index)
                item->child(index)->setHidden(false);
        }
        const bool visible = selfMatches || childMatches;
        item->setHidden(!visible);
        return visible;
    };
    for (int index = 0; index < tree_->topLevelItemCount(); ++index)
        filter(tree_->topLevelItem(index));
    if (!needle.isEmpty()) {
        for (int index = 0; index < tree_->topLevelItemCount(); ++index)
            tree_->topLevelItem(index)->setExpanded(true);
    }
}

void LayersPanel::showContextMenu(const QPoint &pos) {
    // The layer viewport deliberately provides a menu even when the click is
    // not over an item, matching the Project panel's empty-space behavior.
    QMenu menu(this);
    auto *createAction = menu.addAction(QStringLiteral("Create Layer"));
    menu.addSeparator();
    auto *showAllAction = menu.addAction(QStringLiteral("Show all layers"));
    showAllAction->setCheckable(true);
    bool allVisible = true;
    for (int index = 0; index < tree_->topLevelItemCount(); ++index)
        allVisible &= tree_->topLevelItem(index)->checkState(0) == Qt::Checked;
    showAllAction->setChecked(allVisible && tree_->topLevelItemCount() > 0);
    const auto *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
    if (chosen == createAction && onCreateLayer_)
        onCreateLayer_();
    else if (chosen == showAllAction && onAllLayersVisibilityChanged_)
        onAllLayersVisibilityChanged_(showAllAction->isChecked());
}

void LayersPanel::setProject(const ice::Project &project, ice::ObjectId layoutId) {
    QSignalBlocker blocker(tree_);
    tree_->clear();
    const auto styles = layerStylesFor(project);
    const auto styleFor = [&](ice::ObjectId id) {
        return styles.contains(id) ? styles.at(id) : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
    };

    // Purpose variants (pin, nofill, noqrc, mask, …) are separate stream
    // layers because they carry different GDS datatypes. Group them under the
    // base layer number so the panel reads like a technology stack.
    std::map<int, std::vector<std::pair<ice::ObjectId, const ice::Layer *>>> groups;
    for (const auto &[id, layer] : project.layers) {
        if (layoutId != 0 && layer.layout != 0 && layer.layout != layoutId)
            continue;
        if (layoutId != 0 && layer.layout == 0 && !layer.pdk.empty()) {
            const auto cell = project.layoutCells.find(layoutId);
            if (cell == project.layoutCells.end() || cell->second.pdk != layer.pdk)
                continue;
        }
        groups[layer.number].emplace_back(id, &layer);
    }

    for (auto &[number, entries] : groups) {
        std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
            if (left.second->datatype != right.second->datatype)
                return left.second->datatype < right.second->datatype;
            return left.second->name < right.second->name;
        });
        if (entries.size() == 1) {
            const auto &[id, layer] = entries.front();
            auto *item = new QTreeWidgetItem(
                tree_, {QStringLiteral("%1 (%2)").arg(QString::fromStdString(layer->name)).arg(number)});
            item->setIcon(0, QIcon(layerPreviewIcon(styleFor(id))));
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, layer->visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, QStringLiteral("%1 · GDS %2/%3")
                                    .arg(QString::fromStdString(layer->name))
                                    .arg(number)
                                    .arg(layer->datatype));
            continue;
        }

        const auto *primary = entries.front().second;
        const auto baseName = layerBaseName(*primary);
        const auto title = baseName.isEmpty() ? QStringLiteral("Layer %1").arg(number) : baseName;
        auto *groupItem = new QTreeWidgetItem(tree_, {QStringLiteral("%1 (%2)").arg(title).arg(number)});
        groupItem->setIcon(0, QIcon(layerPreviewIcon(styleFor(entries.front().first))));
        groupItem->setFlags(groupItem->flags() | Qt::ItemIsUserCheckable);
        groupItem->setToolTip(0, QStringLiteral("GDS layer %1 · %2 purpose variants").arg(number).arg(entries.size()));
        int visibleCount = 0;
        QStringList groupIds;
        for (const auto &[id, layer] : entries) {
            auto *item = new QTreeWidgetItem(groupItem, {layerVariantLabel(*layer)});
            item->setIcon(0, QIcon(layerPreviewIcon(styleFor(id))));
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, layer->visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, QStringLiteral("%1 · GDS %2/%3")
                                    .arg(QString::fromStdString(layer->name))
                                    .arg(number)
                                    .arg(layer->datatype));
            visibleCount += layer->visible ? 1 : 0;
            groupIds << QString::number(static_cast<qulonglong>(id));
        }
        groupItem->setData(0, kLayerGroupIdsRole, groupIds);
        groupItem->setCheckState(
            0, visibleCount == 0
                   ? Qt::Unchecked
                   : (visibleCount == static_cast<int>(entries.size()) ? Qt::Checked : Qt::PartiallyChecked));
        groupItem->setExpanded(true);
    }
    if (search_)
        applyFilter(search_->text());
    updateVisibilityButton();
}

SymbolsPanel::SymbolsPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search devices…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Search devices"));
    layout->addWidget(search_);
    category_ = new QComboBox;
    category_->setAccessibleName(QStringLiteral("Device category"));
    category_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    category_->setMinimumContentsLength(12);
    layout->addWidget(category_);
    list_ = new QListWidget;
    list_->setAccessibleName(QStringLiteral("Available devices"));
    // Device rows show the symbol artwork on the far right of the name.
    list_->setIconSize(QSize(kDeviceIconSize, kDeviceIconSize));
    list_->setItemDelegate(new TrailingDeviceIconDelegate(list_));
    layout->addWidget(list_, 1);

    connect(search_, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(category_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { rebuildList(); });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *) {
        if (onSelectionChanged_)
            onSelectionChanged_();
        if (onSymbolSelected_ && current && current->data(Qt::UserRole).isValid())
            onSymbolSelected_();
    });
    rebuildCategories();
    rebuildList();
}

void SymbolsPanel::setOnSelectionChanged(std::function<void()> callback) { onSelectionChanged_ = std::move(callback); }

void SymbolsPanel::setOnSymbolSelected(std::function<void()> callback) { onSymbolSelected_ = std::move(callback); }

void SymbolsPanel::clearSymbolSelection() {
    QSignalBlocker blocker(list_);
    list_->setCurrentRow(-1);
    list_->clearSelection();
    if (onSelectionChanged_)
        onSelectionChanged_();
}

void SymbolsPanel::selectSymbol(const QString &name) {
    // A popup selection should be visible in the panel even when the panel
    // was filtered to a different device or category.
    search_->clear();
    category_->setCurrentIndex(0);
    for (int row = 0; row < list_->count(); ++row) {
        if (list_->item(row)->text() == name) {
            list_->setCurrentItem(list_->item(row));
            return;
        }
    }
}

void SymbolsPanel::setSymbols(std::vector<SymbolDefinition> symbols) {
    QString selectedName;
    if (const auto *item = list_->currentItem(); item && item->data(Qt::UserRole).isValid()) {
        const int index = item->data(Qt::UserRole).toInt();
        if (index >= 0 && index < static_cast<int>(symbols_.size()))
            selectedName = symbols_[index].name;
    }
    symbols_ = std::move(symbols);
    icons_.clear();
    icons_.reserve(symbols_.size());
    for (const auto &symbol : symbols_)
        icons_.push_back(symbol.svg.isEmpty() ? QIcon() : ice::symbolArtworkIcon(symbol.svg.toUtf8(), kDeviceIconSize));
    rebuildCategories();
    rebuildList(selectedName);
}

void SymbolsPanel::clear() {
    symbols_.clear();
    icons_.clear();
    rebuildCategories();
    rebuildList();
}

const SymbolDefinition *SymbolsPanel::selectedSymbol() const {
    const auto *item = list_->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid())
        return nullptr;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(symbols_.size()))
        return nullptr;
    return &symbols_[index];
}

void SymbolsPanel::rebuildCategories() {
    const auto current = category_->currentData().toString();
    QSignalBlocker blocker(category_);
    category_->clear();
    category_->addItem(QStringLiteral("All categories"), QString());
    QStringList categories;
    for (const auto &symbol : symbols_)
        if (!symbol.category.isEmpty() && !categories.contains(symbol.category))
            categories << symbol.category;
    categories.sort(Qt::CaseInsensitive);
    for (const auto &category : categories)
        category_->addItem(category, category);
    const auto index = category_->findData(current);
    category_->setCurrentIndex(index >= 0 ? index : 0);
}

void SymbolsPanel::rebuildList(const QString &keepSelected) {
    QSignalBlocker blocker(list_);
    list_->clear();
    const auto needle = search_->text().trimmed();
    const auto category = category_->currentData().toString();
    for (std::size_t index = 0; index < symbols_.size(); ++index) {
        const auto &symbol = symbols_[index];
        if (!category.isEmpty() && symbol.category != category)
            continue;
        if (!needle.isEmpty() && !symbol.name.contains(needle, Qt::CaseInsensitive) &&
            !symbol.category.contains(needle, Qt::CaseInsensitive))
            continue;
        auto *item = new QListWidgetItem(symbol.name, list_);
        item->setData(Qt::UserRole, static_cast<int>(index));
        if (index < icons_.size())
            item->setIcon(icons_[index]);
        item->setToolTip(symbol.category.isEmpty() ? QStringLiteral("Uncategorized") : symbol.category);
    }
    if (list_->count() == 0) {
        auto *item = new QListWidgetItem(
            symbols_.empty() ? QStringLiteral("No devices available") : QStringLiteral("No matching devices"), list_);
        item->setFlags(Qt::NoItemFlags);
        if (onSelectionChanged_)
            onSelectionChanged_();
        return;
    }
    int row = -1;
    if (!keepSelected.isEmpty()) {
        for (int index = 0; index < list_->count(); ++index) {
            const int symbolIndex = list_->item(index)->data(Qt::UserRole).toInt();
            if (symbolIndex >= 0 && symbolIndex < static_cast<int>(symbols_.size()) &&
                symbols_[symbolIndex].name == keepSelected) {
                row = index;
                break;
            }
        }
    }
    list_->setCurrentRow(row);
    list_->clearSelection();
    if (row >= 0)
        list_->setCurrentRow(row);
    if (onSelectionChanged_)
        onSelectionChanged_();
}

SchematicNetlistPanel::SchematicNetlistPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *buttonRow = new QHBoxLayout;
    buildButton_ = new QPushButton(QStringLiteral("Build Netlist"));
    setLucideIcon(buildButton_, QStringLiteral("refresh-cw"), 16);
    buildButton_->setToolTip(QStringLiteral("Generate the netlist for the active schematic"));
    buildButton_->setAccessibleName(QStringLiteral("Build netlist"));
    buttonRow->addWidget(buildButton_);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);
    netlist_ = new QPlainTextEdit;
    netlist_->setReadOnly(true);
    netlist_->setLineWrapMode(QPlainTextEdit::NoWrap);
    netlist_->setPlaceholderText(QStringLiteral("Build the netlist to see it here."));
    layout->addWidget(netlist_, 1);
    connect(buildButton_, &QPushButton::clicked, this, [this] { buildNetlist(); });
    updateBuildButton();
}

void SchematicNetlistPanel::setCell(const ice::Project &project, const ice::SchematicCell *cell) {
    currentCellId_ = cell ? cell->id : 0;
    currentNetlist_ = cell ? buildNetlistText(project, cell) : QString();
    updateBuildButton();
}

void SchematicNetlistPanel::clear() {
    currentCellId_ = 0;
    currentNetlist_.clear();
    builtCellId_ = 0;
    builtNetlist_.clear();
    netlist_->clear();
    updateBuildButton();
}

QString SchematicNetlistPanel::buildNetlistText(const ice::Project &project, const ice::SchematicCell *cell) {
    if (!cell)
        return {};
    std::vector<std::string> notes;
    try {
        auto lines = ice::buildSimulationNetlist(project, cell->id, &notes);
        const auto setup = ice::pdkSimulationSetupFor(cell->pdk);
        lines.insert(lines.begin(), setup.netlistLines.begin(), setup.netlistLines.end());
        QString text;
        for (const auto &line : lines)
            text += QString::fromStdString(line) + QLatin1Char('\n');
        if (!notes.empty()) {
            if (!text.isEmpty())
                text += QLatin1Char('\n');
            text += QStringLiteral("* Notes\n");
            for (const auto &note : notes)
                text += QStringLiteral("* %1\n").arg(QString::fromStdString(note));
        }
        return text.isEmpty() ? QStringLiteral("* Empty netlist") : text;
    } catch (const ice::ProjectError &error) {
        return QString::fromStdString(error.what());
    }
}

void SchematicNetlistPanel::buildNetlist() {
    if (currentCellId_ == 0)
        return;
    netlist_->setPlainText(currentNetlist_);
    builtCellId_ = currentCellId_;
    builtNetlist_ = currentNetlist_;
    updateBuildButton();
}

void SchematicNetlistPanel::updateBuildButton() {
    const bool hasCell = currentCellId_ != 0;
    const bool upToDate = builtCellId_ == currentCellId_ && builtNetlist_ == currentNetlist_ && builtCellId_ != 0;
    buildButton_->setEnabled(hasCell && !upToDate);
}

HistoryPanel::HistoryPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    list_ = new QListWidget;
    list_->setAccessibleName(QStringLiteral("Operation history"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(list_);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        const auto row = list_->row(item);
        if (onHistorySelected_ && row >= 0 && row < static_cast<int>(positions_.size()))
            onHistorySelected_(positions_[row]);
    });
}

void HistoryPanel::setOnHistorySelected(std::function<void(std::size_t)> callback) {
    onHistorySelected_ = std::move(callback);
}

void HistoryPanel::setHistory(const QStringList &entries, const std::vector<std::size_t> &positions) {
    list_->clear();
    positions_.clear();
    for (int index = entries.size() - 1; index >= 0; --index) {
        list_->addItem(entries[index]);
        positions_.push_back(positions[static_cast<std::size_t>(index)]);
    }
}
