#include "properties_panel.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// Original parameter name stored on the name cell so a rejected rename can be
// restored without rebuilding the table.
constexpr int kOriginalParameterNameRole = Qt::UserRole + 1;
// Identifies which path-point field an editable Properties cell belongs to.
constexpr int kPathPointFieldRole = Qt::UserRole + 2;
} // namespace

PropertiesPanel::PropertiesPanel(bool showDisplay, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    title_ = heading(QStringLiteral("Properties"));
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerRow->addWidget(title_, 1);
    headerRow->addStretch();
    editButton_ = new QPushButton;
    editButton_->setFixedWidth(24);
    editButton_->setToolTip(QStringLiteral("Change schematic icon"));
    editButton_->setAccessibleName(QStringLiteral("Change schematic icon"));
    setLucideIcon(editButton_, QStringLiteral("pencil"), 14);
    editButton_->setVisible(false);
    headerRow->addWidget(editButton_);
    layout->addLayout(headerRow);
    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName(QStringLiteral("propertiesPreviewSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    previewFrame_ = new QFrame;
    previewFrame_->setObjectName(QStringLiteral("propertiesSymbolPreview"));
    previewFrame_->setMinimumHeight(75);
    previewFrame_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *previewLayout = new QVBoxLayout(previewFrame_);
    previewLayout->setContentsMargins(4, 4, 4, 4);
    previewStack_ = new QStackedWidget;
    previewStack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    symbolPreview_ = new SymbolPreview;
    symbolPreview_->setMinimumSize(0, 0);
    symbolPreview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layoutDevicePreview_ = new LayoutDevicePreview;
    previewStack_->addWidget(symbolPreview_);
    previewStack_->addWidget(layoutDevicePreview_);
    previewLayout->addWidget(previewStack_);
    previewFrame_->setVisible(true);
    splitter->addWidget(previewFrame_);

    auto *details = new QWidget;
    auto *detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(layout->spacing());

    gridOptions_ = new QGroupBox(QStringLiteral("Grid"));
    auto *gridForm = new QFormLayout(gridOptions_);
    gridSpacingSpin_ = new QSpinBox;
    gridSpacingSpin_->setRange(1, 1000000000);
    gridSpacingSpin_->setSuffix(QStringLiteral(" nm"));
    gridSpacingSpin_->setKeyboardTracking(false);
    gridForm->addRow(QStringLiteral("Spacing"), gridSpacingSpin_);
    gridOptions_->setVisible(false);
    detailsLayout->addWidget(gridOptions_);

    parametersHeader_ = new QWidget;
    auto *parametersHeaderLayout = new QHBoxLayout(parametersHeader_);
    parametersHeaderLayout->setContentsMargins(0, 0, 0, 0);
    parametersHeaderLayout->addWidget(heading(QStringLiteral("Parameters")));
    parametersHeaderLayout->addStretch();
    addParameterButton_ = new QPushButton;
    addParameterButton_->setFixedWidth(24);
    addParameterButton_->setToolTip(QStringLiteral("Add parameter"));
    addParameterButton_->setAccessibleName(QStringLiteral("Add parameter"));
    setLucideIcon(addParameterButton_, QStringLiteral("plus"), 14);
    removeParameterButton_ = new QPushButton;
    removeParameterButton_->setFixedWidth(24);
    removeParameterButton_->setToolTip(QStringLiteral("Remove selected parameters"));
    removeParameterButton_->setAccessibleName(QStringLiteral("Remove selected parameters"));
    setLucideIcon(removeParameterButton_, QStringLiteral("trash"), 14);
    parametersHeaderLayout->addWidget(addParameterButton_);
    parametersHeaderLayout->addWidget(removeParameterButton_);
    detailsLayout->addWidget(parametersHeader_);
    parameters_ = new QTableWidget(0, 2);
    parameters_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
    parameters_->setAccessibleName(QStringLiteral("Parameters"));
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    parameters_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    parameters_->verticalHeader()->setVisible(false);
    detailsLayout->addWidget(parameters_, 1);
    if (showDisplay) {
        detailsLayout->addWidget(heading(QStringLiteral("Display")));
        auto *layerSelector = new QComboBox;
        layerSelector->addItem(QStringLiteral("All layers"));
        layerSelector->addItem(QStringLiteral("Visible layers"));
        detailsLayout->addWidget(layerSelector);
    }
    detailsLayout->addStretch();
    splitter->addWidget(details);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({75, 500});
    contentSplitter_ = splitter;
    emptyHint_ = new QLabel(QStringLiteral("Select a device, net, or the schematic background to inspect its "
                                           "properties."));
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setWordWrap(true);
    emptyHint_->setContentsMargins(16, 16, 16, 16);
    emptyHint_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    emptyHint_->setVisible(false);
    layout->addWidget(emptyHint_, 1);
    layout->addWidget(splitter, 1);
    connect(parameters_, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) { handleParameterEdit(item); });
    connect(parameters_, &QTableWidget::itemSelectionChanged, this, [this] { updateParameterButtons(); });
    connect(addParameterButton_, &QPushButton::clicked, this, [this] { addParameter(); });
    connect(removeParameterButton_, &QPushButton::clicked, this, [this] { removeParameters(); });
    connect(editButton_, &QPushButton::clicked, this, [this] {
        if (instanceId_ != 0 && onSymbolIconEditRequested_)
            onSymbolIconEditRequested_(instanceId_);
    });
    connect(gridSpacingSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int spacing) {
        if (onGridSpacingChanged_)
            onGridSpacingChanged_(spacing);
    });
    updateParameterButtons();
    refreshAppearance();
    setEmptyState(true);
}

void PropertiesPanel::setEditButtonVisible(bool visible) {
    editButtonVisible_ = visible;
    if (editButton_)
        editButton_->setVisible(visible && !emptyState_);
}

void PropertiesPanel::setEmptyState(bool empty) {
    emptyState_ = empty;
    if (emptyHint_)
        emptyHint_->setVisible(empty);
    if (contentSplitter_)
        contentSplitter_->setVisible(!empty);
    if (title_)
        title_->setVisible(!empty);
    if (editButton_)
        editButton_->setVisible(!empty && editButtonVisible_);
}

void PropertiesPanel::setOnParameterChanged(ParameterChanged callback) { onParameterChanged_ = std::move(callback); }

void PropertiesPanel::setOnCellParameterChanged(CellParameterChanged callback) {
    onCellParameterChanged_ = std::move(callback);
}

void PropertiesPanel::setOnParameterRenamed(ParameterRenamed callback) { onParameterRenamed_ = std::move(callback); }

void PropertiesPanel::setOnCellParameterRenamed(CellParameterRenamed callback) {
    onCellParameterRenamed_ = std::move(callback);
}

void PropertiesPanel::setOnCellParameterAdded(CellParameterAdded callback) {
    onCellParameterAdded_ = std::move(callback);
}

void PropertiesPanel::setOnCellParametersRemoved(CellParametersRemoved callback) {
    onCellParametersRemoved_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutCellParameterChanged(LayoutCellParameterChanged callback) {
    onLayoutCellParameterChanged_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutDeviceParameterChanged(LayoutDeviceParameterChanged callback) {
    onLayoutDeviceParameterChanged_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutPortChanged(LayoutPortChanged callback) { onLayoutPortChanged_ = std::move(callback); }

void PropertiesPanel::setOnGridSpacingChanged(GridSpacingChanged callback) {
    onGridSpacingChanged_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutShapeSizeChanged(LayoutShapeSizeChanged callback) {
    onLayoutShapeSizeChanged_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutPathPointChanged(LayoutPathPointChanged callback) {
    onLayoutPathPointChanged_ = std::move(callback);
}

void PropertiesPanel::setOnLayoutPathSegmentChanged(LayoutPathSegmentChanged callback) {
    onLayoutPathSegmentChanged_ = std::move(callback);
}

void PropertiesPanel::setOnSymbolIconEditRequested(SymbolIconEditRequested callback) {
    onSymbolIconEditRequested_ = std::move(callback);
    updateParameterButtons();
}

void PropertiesPanel::refreshAppearance() {
    previewFrame_->setStyleSheet(
        QStringLiteral("#propertiesSymbolPreview { background: %1; border: 1px solid %2; border-radius: 4px; }")
            .arg(ice::uiColor(ice::UiColor::Background2).name(), ice::uiColor(ice::UiColor::Border1).name()));
}

void PropertiesPanel::clearSelection() {
    instanceId_ = 0;
    symbolIconEditable_ = false;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(false);
    setTitle(QStringLiteral("Properties"));
    setEmptyState(true);
    previewFrame_->setVisible(true);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    updateParameterButtons();
}

void PropertiesPanel::setInstance(const ice::SchematicInstance &instance, const std::string &symbolName,
                                  const std::string &symbolSvg, bool iconEditable) {
    setEmptyState(false);
    parameterNamesEditable_ = true;
    (void)symbolName;
    instanceId_ = instance.id;
    symbolIconEditable_ = iconEditable;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(QString::fromStdString(instance.name));
    previewStack_->setCurrentWidget(symbolPreview_);
    symbolPreview_->load(QByteArray::fromStdString(symbolSvg));
    previewFrame_->setVisible(true);
    populateParameters(instance.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setSchematic(const ice::SchematicCell &cell, const QString &symbolSvg) {
    setEmptyState(false);
    parameterNamesEditable_ = true;
    instanceId_ = 0;
    cellId_ = cell.id;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(QString::fromStdString(cell.name));
    previewStack_->setCurrentWidget(symbolPreview_);
    symbolPreview_->load(symbolSvg.toUtf8());
    previewFrame_->setVisible(true);
    populateParameters(cell.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutPreviewStyles(const ice::LayoutRenderer::LayerStyleMap &styles,
                                             const ice::LayoutRenderer::DeviceStyleMap &deviceStyles) {
    layoutPreviewStyles_ = styles;
    layoutPreviewDeviceStyles_ = deviceStyles;
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
}

void PropertiesPanel::setLayoutCell(const ice::LayoutCell &cell, const ice::LayoutPreviewModel &model) {
    setEmptyState(false);
    parameterNamesEditable_ = false;
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = cell.id;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(QString::fromStdString(cell.name));
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
    layoutDevicePreview_->setModel(model);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    populateParameters(cell.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutDevice(const ice::LayoutDeviceCell *cell) {
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(cell != nullptr);
    if (!cell) {
        setTitle(QStringLiteral("Properties"));
        setEmptyState(true);
        previewFrame_->setVisible(true);
        populateParameters({});
        updateParameterButtons();
        return;
    }
    setEmptyState(false);
    layoutDeviceName_ = cell->name;
    setTitle(QString::fromStdString(cell->name));
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
    layoutDevicePreview_->setCell(cell);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    populateLayoutDeviceParameters(cell->parameters);
    updateParameterButtons();
}

void PropertiesPanel::setPathTool(const std::string &layerName) {
    setEmptyState(false);
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = true;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    pathLayerName_ = layerName;
    setTitle(QStringLiteral("Path tool"));
    previewFrame_->setVisible(true);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    const int layerRow = parameters_->rowCount();
    parameters_->insertRow(layerRow);
    auto *layerNameItem = new QTableWidgetItem(QStringLiteral("layer"));
    layerNameItem->setFlags(layerNameItem->flags() & ~Qt::ItemIsEditable);
    auto *layerValue = new QTableWidgetItem(QString::fromStdString(pathLayerName_));
    layerValue->setFlags(layerValue->flags() & ~Qt::ItemIsEditable);
    parameters_->setItem(layerRow, 0, layerNameItem);
    parameters_->setItem(layerRow, 1, layerValue);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutShape(const ice::Shape &shape, const std::string &layerName,
                                     const std::string &displayName, const std::string &typeName,
                                     const ice::LayoutPreviewModel &model) {
    setEmptyState(false);
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = true;
    portMode_ = false;
    shapeIsRectangle_ = !shape.isPath() && !shape.isText() && shape.polygon.empty();
    layoutShapeId_ = shape.id;
    layoutShapeWidth_ = static_cast<int>(std::clamp<ice::Coordinate>(shape.rectangle.width, 1, 1000000000));
    layoutShapeHeight_ = static_cast<int>(std::clamp<ice::Coordinate>(shape.rectangle.height, 1, 1000000000));
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(QString::fromStdString(displayName));
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
    layoutDevicePreview_->setModel(model);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    const auto addReadOnlyRow = [&](const QString &name, const QString &value) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        auto *valueItem = new QTableWidgetItem(value);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, nameItem);
        parameters_->setItem(row, 1, valueItem);
    };
    addReadOnlyRow(QStringLiteral("name"), QString::fromStdString(displayName));
    addReadOnlyRow(QStringLiteral("type"), QString::fromStdString(typeName));
    addReadOnlyRow(QStringLiteral("layer"), QString::fromStdString(layerName));
    if (shape.textGroup != 0 && !shape.textSource.empty())
        addReadOnlyRow(QStringLiteral("text"), QString::fromStdString(shape.textSource));
    if (shapeIsRectangle_) {
        const int widthRow = parameters_->rowCount();
        parameters_->insertRow(widthRow);
        auto *widthName = new QTableWidgetItem(QStringLiteral("width (nm)"));
        widthName->setFlags(widthName->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(widthRow, 0, widthName);
        parameters_->setItem(widthRow, 1, new QTableWidgetItem(QString::number(layoutShapeWidth_)));
        const int heightRow = parameters_->rowCount();
        parameters_->insertRow(heightRow);
        auto *heightName = new QTableWidgetItem(QStringLiteral("height (nm)"));
        heightName->setFlags(heightName->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(heightRow, 0, heightName);
        parameters_->setItem(heightRow, 1, new QTableWidgetItem(QString::number(layoutShapeHeight_)));
    }
    updateParameterButtons();
}

void PropertiesPanel::setLayoutPathPoints(const ice::Shape &shape, const std::vector<int> &pointIndices,
                                          const std::string &layerName, const std::vector<LayoutPortLayer> &layers,
                                          const ice::LayoutPreviewModel &model) {
    std::vector<int> points;
    for (const auto index : pointIndices)
        if (index >= 0 && index < static_cast<int>(shape.centerline.size()))
            points.push_back(index);
    if (points.empty())
        return;
    setEmptyState(false);
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = true;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = shape.id;
    layoutPathPointIndices_ = points;
    layoutPathPointIndex_ = points.front();
    layoutPathSegmentIndex_ = -1;
    layoutPathPointX_ = 0;
    layoutPathPointY_ = 0;
    layoutPathSegmentWidth_ = 0;
    layoutPathSegmentLayer_ = 0;
    layoutPathLayers_ = layers;
    if (points.size() == 1) {
        const auto pointIndex = points.front();
        layoutPathPointX_ = shape.centerline[static_cast<std::size_t>(pointIndex)].x;
        layoutPathPointY_ = shape.centerline[static_cast<std::size_t>(pointIndex)].y;
        if (pointIndex + 1 < static_cast<int>(shape.centerline.size()))
            layoutPathSegmentIndex_ = pointIndex;
        else if (pointIndex > 0)
            layoutPathSegmentIndex_ = pointIndex - 1;
        if (layoutPathSegmentIndex_ >= 0) {
            layoutPathSegmentWidth_ = shape.widthForSegment(static_cast<std::size_t>(layoutPathSegmentIndex_));
            layoutPathSegmentLayer_ = shape.layerForSegment(static_cast<std::size_t>(layoutPathSegmentIndex_));
        }
    }
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(points.size() == 1 ? QStringLiteral("Path point %1").arg(points.front())
                                : QStringLiteral("Path points (%1)").arg(points.size()));
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
    layoutDevicePreview_->setModel(model);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    const auto addReadOnlyRow = [&](const QString &name, const QString &value) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        auto *valueItem = new QTableWidgetItem(value);
        valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, nameItem);
        parameters_->setItem(row, 1, valueItem);
    };
    const auto addEditableRow = [&](const QString &name, const QString &value, const QString &field) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        nameItem->setData(kPathPointFieldRole, field);
        auto *valueItem = new QTableWidgetItem(value);
        valueItem->setData(kPathPointFieldRole, field);
        parameters_->setItem(row, 0, nameItem);
        parameters_->setItem(row, 1, valueItem);
    };
    addReadOnlyRow(QStringLiteral("name"),
                   QString::fromStdString(shape.name.empty() ? std::string{"Path"} : shape.name));
    addReadOnlyRow(QStringLiteral("type"), QStringLiteral("Path point"));
    addReadOnlyRow(QStringLiteral("layer"), QString::fromStdString(layerName));
    if (points.size() != 1) {
        QStringList indices;
        for (const auto index : points)
            indices.push_back(QString::number(index));
        addReadOnlyRow(QStringLiteral("points"), indices.join(QStringLiteral(", ")));
        updateParameterButtons();
        return;
    }
    addReadOnlyRow(QStringLiteral("point"), QString::number(points.front()));
    addEditableRow(QStringLiteral("x (nm)"), QString::number(layoutPathPointX_), QStringLiteral("x"));
    addEditableRow(QStringLiteral("y (nm)"), QString::number(layoutPathPointY_), QStringLiteral("y"));
    if (layoutPathSegmentIndex_ >= 0) {
        addEditableRow(QStringLiteral("segment width (nm)"), QString::number(layoutPathSegmentWidth_),
                       QStringLiteral("segment-width"));
        if (!layers.empty()) {
            const int row = parameters_->rowCount();
            parameters_->insertRow(row);
            auto *nameItem = new QTableWidgetItem(QStringLiteral("segment layer"));
            nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
            parameters_->setItem(row, 0, nameItem);
            auto *combo = new QComboBox;
            for (const auto &[layerId, name] : layers)
                combo->addItem(QString::fromStdString(name), QVariant::fromValue<qulonglong>(layerId));
            int index = combo->findData(QVariant::fromValue<qulonglong>(layoutPathSegmentLayer_));
            if (index < 0 && layoutPathSegmentLayer_ != 0) {
                combo->addItem(QString::number(layoutPathSegmentLayer_),
                               QVariant::fromValue<qulonglong>(layoutPathSegmentLayer_));
                index = combo->count() - 1;
            }
            if (index >= 0)
                combo->setCurrentIndex(index);
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, combo](int index) {
                if (index < 0)
                    return;
                const auto layer = static_cast<ice::ObjectId>(combo->itemData(index).toULongLong());
                if (layer == layoutPathSegmentLayer_)
                    return;
                layoutPathSegmentLayer_ = layer;
                if (onLayoutPathSegmentChanged_)
                    onLayoutPathSegmentChanged_(layoutShapeId_, layoutPathSegmentIndex_, layoutPathSegmentWidth_,
                                                layoutPathSegmentLayer_);
            });
            parameters_->setCellWidget(row, 1, combo);
        } else if (layoutPathSegmentLayer_ != 0) {
            addReadOnlyRow(QStringLiteral("segment layer"), QString::number(layoutPathSegmentLayer_));
        }
    }
    updateParameterButtons();
}

void PropertiesPanel::setLayoutPort(const ice::Port &port, const std::string &layerName, const std::string &netName,
                                    const std::vector<LayoutPortLayer> &layers) {
    setEmptyState(false);
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    portMode_ = true;
    layoutShapeId_ = 0;
    layoutPort_ = port;
    layoutPortLayerName_ = layerName;
    layoutPortNetName_ = netName;
    if (gridOptions_)
        gridOptions_->setVisible(false);
    setParametersVisible(true);
    setTitle(QString::fromStdString(port.name));
    previewFrame_->setVisible(true);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    const auto addRow = [&](const QString &name, const QString &value, bool editable) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        auto *valueItem = new QTableWidgetItem(value);
        if (!editable)
            valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, nameItem);
        parameters_->setItem(row, 1, valueItem);
    };
    addRow(QStringLiteral("name"), QString::fromStdString(port.name), true);
    addRow(QStringLiteral("label"), QString::fromStdString(port.label), true);
    addRow(QStringLiteral("direction"), QString::fromStdString(ice::portDirectionName(port.direction)), true);
    addRow(QStringLiteral("side"), QString::fromStdString(ice::portSideName(port.side)), true);
    if (!layers.empty()) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *nameItem = new QTableWidgetItem(QStringLiteral("layer"));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, nameItem);
        auto *combo = new QComboBox;
        for (const auto &[layerId, name] : layers)
            combo->addItem(QString::fromStdString(name), QVariant::fromValue<qulonglong>(layerId));
        int index = combo->findData(QVariant::fromValue<qulonglong>(port.layer));
        if (index < 0 && port.layer != 0) {
            combo->addItem(QString::fromStdString(layerName), QVariant::fromValue<qulonglong>(port.layer));
            index = combo->count() - 1;
        }
        if (index >= 0)
            combo->setCurrentIndex(index);
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, combo](int index) {
            if (index < 0)
                return;
            layoutPort_.layer = static_cast<ice::ObjectId>(combo->itemData(index).toULongLong());
            if (onLayoutPortChanged_)
                onLayoutPortChanged_(layoutPort_, layoutPortNetName_);
        });
        parameters_->setCellWidget(row, 1, combo);
    } else if (!layerName.empty()) {
        addRow(QStringLiteral("layer"), QString::fromStdString(layerName), false);
    }
    addRow(QStringLiteral("x (nm)"), QString::number(port.rectangle.x), false);
    addRow(QStringLiteral("y (nm)"), QString::number(port.rectangle.y), false);
    addRow(QStringLiteral("width (nm)"), QString::number(port.rectangle.width), true);
    addRow(QStringLiteral("height (nm)"), QString::number(port.rectangle.height), true);
    addRow(QStringLiteral("net"), QString::fromStdString(netName), true);
    addRow(QStringLiteral("visible"), port.visible ? QStringLiteral("true") : QStringLiteral("false"), true);
    addRow(QStringLiteral("locked"), port.locked ? QStringLiteral("true") : QStringLiteral("false"), true);
    if (port.sourceInstance != 0)
        addRow(QStringLiteral("source_instance"), QString::number(port.sourceInstance), false);
    if (port.sourcePort != 0)
        addRow(QStringLiteral("source_port"), QString::number(port.sourcePort), false);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutOverview(const ice::LayoutCell &cell, const ice::LayoutPreviewModel &model,
                                        int spacing) {
    setEmptyState(false);
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    pathPointMode_ = false;
    shapeMode_ = false;
    portMode_ = false;
    layoutShapeId_ = 0;
    setParametersVisible(false);
    layoutDevicePreview_->setStyles(layoutPreviewStyles_, layoutPreviewDeviceStyles_);
    layoutDevicePreview_->setModel(model);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    setTitle(QString::fromStdString(cell.name));
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    if (gridOptions_) {
        QSignalBlocker spacingBlocker(gridSpacingSpin_);
        gridSpacingSpin_->setValue(std::clamp(spacing, 1, 1000000000));
        gridOptions_->setVisible(true);
    }
    updateParameterButtons();
}

void PropertiesPanel::setTitle(const QString &title) {
    if (!title_)
        return;
    const auto text = title.trimmed().isEmpty() ? QStringLiteral("Properties") : title;
    title_->setText(text);
    title_->setToolTip(text);
}

void PropertiesPanel::populateParameters(const std::vector<ice::Parameter> &parameters) {
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    for (const auto &parameter : parameters) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        const auto nameText = QString::fromStdString(parameter.name);
        auto *name = new QTableWidgetItem(nameText);
        name->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(parameter.id));
        // Kept so an invalid rename can be rolled back without a model refresh.
        name->setData(kOriginalParameterNameRole, nameText);
        if (!parameterNamesEditable_)
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, name);
        parameters_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(parameter.value)));
    }
}

void PropertiesPanel::setParametersVisible(bool visible) {
    if (parametersHeader_)
        parametersHeader_->setVisible(visible);
    if (parameters_)
        parameters_->setVisible(visible);
}

void PropertiesPanel::populateLayoutDeviceParameters(const std::vector<ice::LayoutDeviceParameter> &parameters) {
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    for (const auto &parameter : parameters) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        const auto description = QString::fromStdString(parameter.description);
        auto *name = new QTableWidgetItem(QString::fromStdString(parameter.name));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        if (!description.isEmpty())
            name->setToolTip(description);
        parameters_->setItem(row, 0, name);
        auto *value = new QTableWidgetItem(QString::fromStdString(parameter.value));
        if (!description.isEmpty())
            value->setToolTip(description);
        parameters_->setItem(row, 1, value);
    }
}

void PropertiesPanel::handleParameterEdit(QTableWidgetItem *item) {
    if (!item)
        return;
    if (item->column() == 0) {
        const auto original = item->data(kOriginalParameterNameRole).toString();
        const auto newName = item->text().trimmed();
        const auto revert = [&] {
            QSignalBlocker blocker(parameters_);
            item->setText(original);
        };
        if (!parameterNamesEditable_ || newName == original)
            return;
        if (newName.isEmpty()) {
            revert();
            return;
        }
        for (int row = 0; row < parameters_->rowCount(); ++row) {
            const auto *other = parameters_->item(row, 0);
            if (other && other != item && other->text().compare(newName, Qt::CaseInsensitive) == 0) {
                revert();
                return;
            }
        }
        const auto parameterId = static_cast<ice::ObjectId>(item->data(Qt::UserRole).toULongLong());
        if (instanceId_ != 0 && onParameterRenamed_)
            onParameterRenamed_(instanceId_, parameterId, newName.toStdString());
        else if (cellId_ != 0 && onCellParameterRenamed_)
            onCellParameterRenamed_(cellId_, parameterId, newName.toStdString());
        else {
            revert();
            return;
        }
        item->setData(kOriginalParameterNameRole, newName);
        return;
    }
    if (item->column() != 1)
        return;
    const auto *name = parameters_->item(item->row(), 0);
    if (!name)
        return;
    if (pathPointMode_) {
        const auto field = name->data(kPathPointFieldRole).toString();
        bool accepted = false;
        const auto value = item->text().trimmed().toInt(&accepted);
        if (field == QStringLiteral("x") || field == QStringLiteral("y")) {
            const bool xField = field == QStringLiteral("x");
            const auto current = xField ? layoutPathPointX_ : layoutPathPointY_;
            if (!accepted) {
                QSignalBlocker blocker(parameters_);
                item->setText(QString::number(current));
                return;
            }
            const auto clamped = std::clamp(value, -1000000000, 1000000000);
            if (clamped != value) {
                QSignalBlocker blocker(parameters_);
                item->setText(QString::number(clamped));
            }
            if (xField)
                layoutPathPointX_ = clamped;
            else
                layoutPathPointY_ = clamped;
            if (onLayoutPathPointChanged_)
                onLayoutPathPointChanged_(layoutShapeId_, layoutPathPointIndex_, layoutPathPointX_, layoutPathPointY_);
            return;
        }
        if (field == QStringLiteral("segment-width")) {
            if (!accepted || value <= 0) {
                QSignalBlocker blocker(parameters_);
                item->setText(QString::number(layoutPathSegmentWidth_));
                return;
            }
            const auto clamped = std::clamp(value, 1, 1000000000);
            if (clamped != value) {
                QSignalBlocker blocker(parameters_);
                item->setText(QString::number(clamped));
            }
            if (clamped == layoutPathSegmentWidth_)
                return;
            layoutPathSegmentWidth_ = clamped;
            if (onLayoutPathSegmentChanged_)
                onLayoutPathSegmentChanged_(layoutShapeId_, layoutPathSegmentIndex_, layoutPathSegmentWidth_,
                                            layoutPathSegmentLayer_);
            return;
        }
        return;
    }
    if (portMode_) {
        const auto field = name->text();
        const auto value = item->text().trimmed();
        const auto revert = [&](const QString &text) {
            QSignalBlocker blocker(parameters_);
            item->setText(text);
        };
        if (field == QStringLiteral("name") || field == QStringLiteral("label")) {
            if (value.isEmpty()) {
                revert(field == QStringLiteral("name") ? QString::fromStdString(layoutPort_.name)
                                                       : QString::fromStdString(layoutPort_.label));
                return;
            }
            if (field == QStringLiteral("name"))
                layoutPort_.name = value.toStdString();
            else
                layoutPort_.label = value.toStdString();
        } else if (field == QStringLiteral("direction")) {
            const auto direction = ice::parsePortDirection(value.toStdString());
            if (!direction) {
                revert(QString::fromStdString(ice::portDirectionName(layoutPort_.direction)));
                return;
            }
            layoutPort_.direction = *direction;
        } else if (field == QStringLiteral("side")) {
            const auto side = ice::parsePortSide(value.toStdString());
            if (!side) {
                revert(QString::fromStdString(ice::portSideName(layoutPort_.side)));
                return;
            }
            layoutPort_.side = *side;
        } else if (field == QStringLiteral("width (nm)") || field == QStringLiteral("height (nm)")) {
            bool accepted = false;
            const auto number = value.toInt(&accepted);
            if (!accepted || number <= 0) {
                revert(QString::number(field == QStringLiteral("width (nm)") ? layoutPort_.rectangle.width
                                                                             : layoutPort_.rectangle.height));
                return;
            }
            if (field == QStringLiteral("width (nm)"))
                layoutPort_.rectangle.width = number;
            else
                layoutPort_.rectangle.height = number;
        } else if (field == QStringLiteral("net")) {
            layoutPortNetName_ = value.toStdString();
        } else if (field == QStringLiteral("visible") || field == QStringLiteral("locked")) {
            const auto lowered = value.toLower();
            if (lowered != QStringLiteral("true") && lowered != QStringLiteral("false")) {
                const bool current = field == QStringLiteral("visible") ? layoutPort_.visible : layoutPort_.locked;
                revert(current ? QStringLiteral("true") : QStringLiteral("false"));
                return;
            }
            const bool enabled = lowered == QStringLiteral("true");
            if (field == QStringLiteral("visible"))
                layoutPort_.visible = enabled;
            else
                layoutPort_.locked = enabled;
        } else {
            return;
        }
        setTitle(QString::fromStdString(layoutPort_.name));
        if (onLayoutPortChanged_)
            onLayoutPortChanged_(layoutPort_, layoutPortNetName_);
        return;
    }
    if (shapeMode_) {
        if (name->text() != QStringLiteral("width (nm)") && name->text() != QStringLiteral("height (nm)"))
            return;
        bool accepted = false;
        const auto value = item->text().toInt(&accepted);
        const bool width = name->text() == QStringLiteral("width (nm)");
        if (!accepted || value <= 0) {
            QSignalBlocker blocker(parameters_);
            item->setText(QString::number(width ? layoutShapeWidth_ : layoutShapeHeight_));
            return;
        }
        const int clamped = std::clamp(value, 1, 1000000000);
        const int previous = width ? layoutShapeWidth_ : layoutShapeHeight_;
        if (clamped == previous) {
            QSignalBlocker blocker(parameters_);
            item->setText(QString::number(previous));
            return;
        }
        if (clamped != value) {
            QSignalBlocker blocker(parameters_);
            item->setText(QString::number(clamped));
        }
        if (width)
            layoutShapeWidth_ = clamped;
        else
            layoutShapeHeight_ = clamped;
        if (onLayoutShapeSizeChanged_)
            onLayoutShapeSizeChanged_(layoutShapeId_, layoutShapeWidth_, layoutShapeHeight_);
        return;
    }
    if (pathMode_)
        return;
    const auto parameterId = static_cast<ice::ObjectId>(name->data(Qt::UserRole).toULongLong());
    if (instanceId_ != 0 && onParameterChanged_)
        onParameterChanged_(instanceId_, parameterId, name->text().toStdString(), item->text().toStdString());
    else if (cellId_ != 0 && onCellParameterChanged_)
        onCellParameterChanged_(cellId_, parameterId, name->text().toStdString(), item->text().toStdString());
    else if (layoutCellId_ != 0 && onLayoutCellParameterChanged_)
        onLayoutCellParameterChanged_(layoutCellId_, parameterId, name->text().toStdString(),
                                      item->text().toStdString());
    else if (!layoutDeviceName_.empty() && onLayoutDeviceParameterChanged_)
        onLayoutDeviceParameterChanged_(layoutDeviceName_, name->text().toStdString(), item->text().toStdString());
}

void PropertiesPanel::updateParameterButtons() {
    // The pencil only edits the icon of a user-created schematic device. It has
    // no meaning for primitive devices, cells, layout objects, ports, or an
    // empty selection.
    if (editButton_) {
        editButton_->setVisible(editButtonVisible_);
        const bool editable = instanceId_ != 0 && symbolIconEditable_ && onSymbolIconEditRequested_;
        editButton_->setEnabled(editable);
        if (editable)
            editButton_->setToolTip(QStringLiteral("Change schematic icon"));
        else if (instanceId_ != 0)
            editButton_->setToolTip(QStringLiteral("Only user-created schematics can have a custom icon"));
        else
            editButton_->setToolTip(QStringLiteral("Select a schematic device to change its icon"));
    }
    const bool schematicMode = cellId_ != 0 && instanceId_ == 0;
    if (addParameterButton_)
        addParameterButton_->setEnabled(schematicMode);
    if (removeParameterButton_) {
        const bool hasSelection = parameters_->selectionModel() && parameters_->selectionModel()->hasSelection();
        removeParameterButton_->setEnabled(schematicMode && hasSelection);
    }
}

void PropertiesPanel::addParameter() {
    if (instanceId_ != 0 || cellId_ == 0 || !onCellParameterAdded_)
        return;
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Add Parameter"), QStringLiteral("Parameter name:"),
                                            QLineEdit::Normal,
                                            QStringLiteral("parameter%1").arg(parameters_->rowCount() + 1), &accepted);
    if (!accepted || name.trimmed().isEmpty())
        return;
    onCellParameterAdded_(cellId_, name.trimmed().toStdString());
}

void PropertiesPanel::removeParameters() {
    if (instanceId_ != 0 || cellId_ == 0 || !onCellParametersRemoved_)
        return;
    std::vector<ice::ObjectId> parameterIds;
    const auto rows = parameters_->selectionModel()->selectedRows();
    for (const auto &index : rows) {
        if (const auto *name = parameters_->item(index.row(), 0))
            parameterIds.push_back(static_cast<ice::ObjectId>(name->data(Qt::UserRole).toULongLong()));
    }
    if (!parameterIds.empty())
        onCellParametersRemoved_(cellId_, parameterIds);
}
