#include "properties_panel.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

PropertiesPanel::PropertiesPanel(bool showDisplay, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(heading(QStringLiteral("Properties")));
    previewFrame_ = new QFrame;
    previewFrame_->setObjectName(QStringLiteral("propertiesSymbolPreview"));
    previewFrame_->setFixedHeight(180);
    auto *previewLayout = new QVBoxLayout(previewFrame_);
    previewStack_ = new QStackedWidget;
    symbolPreview_ = new SymbolPreview;
    symbolPreview_->setMinimumSize(160, 120);
    symbolPreview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layoutDevicePreview_ = new LayoutDevicePreview;
    previewStack_->addWidget(symbolPreview_);
    previewStack_->addWidget(layoutDevicePreview_);
    previewLayout->addWidget(previewStack_);
    previewFrame_->setVisible(false);
    layout->addWidget(previewFrame_);
    auto *form = new QFormLayout;
    selection_ = addRow(form, QStringLiteral("Selection"));
    layout->addLayout(form);
    auto *parametersHeader = new QWidget;
    auto *parametersHeaderLayout = new QHBoxLayout(parametersHeader);
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
    layout->addWidget(parametersHeader);
    parameters_ = new QTableWidget(0, 2);
    parameters_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
    parameters_->setAccessibleName(QStringLiteral("Parameters"));
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    parameters_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    parameters_->verticalHeader()->setVisible(false);
    layout->addWidget(parameters_, 1);
    if (showDisplay) {
        layout->addWidget(heading(QStringLiteral("Display")));
        auto *layerSelector = new QComboBox;
        layerSelector->addItem(QStringLiteral("All layers"));
        layerSelector->addItem(QStringLiteral("Visible layers"));
        layout->addWidget(layerSelector);
    }
    layout->addStretch();
    connect(parameters_, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) { handleParameterEdit(item); });
    connect(parameters_, &QTableWidget::itemSelectionChanged, this, [this] { updateParameterButtons(); });
    connect(addParameterButton_, &QPushButton::clicked, this, [this] { addParameter(); });
    connect(removeParameterButton_, &QPushButton::clicked, this, [this] { removeParameters(); });
    updateParameterButtons();
    refreshAppearance();
}

void PropertiesPanel::setOnParameterChanged(ParameterChanged callback) { onParameterChanged_ = std::move(callback); }

void PropertiesPanel::setOnCellParameterChanged(CellParameterChanged callback) {
    onCellParameterChanged_ = std::move(callback);
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

void PropertiesPanel::setOnPathWidthChanged(PathWidthChanged callback) { onPathWidthChanged_ = std::move(callback); }

void PropertiesPanel::refreshAppearance() {
    previewFrame_->setStyleSheet(
        QStringLiteral("#propertiesSymbolPreview { background: %1; border: 1px solid %2; border-radius: 4px; }")
            .arg(ice::uiColor(ice::UiColor::Background2).name(), ice::uiColor(ice::UiColor::Border1).name()));
}

void PropertiesPanel::clearSelection() {
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    selection_->setText(QStringLiteral("Nothing selected"));
    previewFrame_->setVisible(false);
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    updateParameterButtons();
}

void PropertiesPanel::setInstance(const ice::SchematicInstance &instance, const std::string &symbolName,
                                  const std::string &symbolSvg) {
    instanceId_ = instance.id;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    const auto type = symbolName.empty() ? QStringLiteral("Schematic") : QString::fromStdString(symbolName);
    selection_->setText(QStringLiteral("%1 (%2)").arg(QString::fromStdString(instance.name), type));
    previewStack_->setCurrentWidget(symbolPreview_);
    symbolPreview_->load(QByteArray::fromStdString(symbolSvg));
    previewFrame_->setVisible(!symbolSvg.empty());
    populateParameters(instance.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setSchematic(const ice::SchematicCell &cell, const QString &symbolSvg) {
    instanceId_ = 0;
    cellId_ = cell.id;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    selection_->setText(QStringLiteral("%1 (Schematic)").arg(QString::fromStdString(cell.name)));
    previewStack_->setCurrentWidget(symbolPreview_);
    symbolPreview_->load(symbolSvg.toUtf8());
    previewFrame_->setVisible(!symbolSvg.isEmpty());
    populateParameters(cell.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutCell(const ice::LayoutCell &cell, const ice::LayoutDeviceCell *previewCell) {
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = cell.id;
    layoutDeviceName_.clear();
    pathMode_ = false;
    const auto pdk = cell.pdk.empty() || cell.pdk == ice::genericPdkId ? QStringLiteral("Generic PDK")
                                                                       : QString::fromStdString(cell.pdk);
    const auto kind = cell.device.empty() ? QStringLiteral("Layout") : QStringLiteral("Device");
    auto text = QStringLiteral("%1 (%2 · %3)").arg(QString::fromStdString(cell.name), kind, pdk);
    if (cell.library)
        text += QStringLiteral(" · Read-only geometry");
    selection_->setText(text);
    if (previewCell && !previewCell->shapes.empty()) {
        layoutCellPreview_ = *previewCell;
        layoutDevicePreview_->setCell(&layoutCellPreview_);
        previewStack_->setCurrentWidget(layoutDevicePreview_);
        previewFrame_->setVisible(true);
    } else {
        previewFrame_->setVisible(false);
    }
    populateParameters(cell.parameters);
    updateParameterButtons();
}

void PropertiesPanel::setLayoutDevice(const ice::LayoutDeviceCell *cell) {
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = false;
    if (!cell) {
        selection_->setText(QStringLiteral("No device selected"));
        previewFrame_->setVisible(false);
        populateParameters({});
        updateParameterButtons();
        return;
    }
    layoutDeviceName_ = cell->name;
    selection_->setText(QStringLiteral("%1 (Device)").arg(QString::fromStdString(cell->name)));
    layoutDevicePreview_->setCell(cell);
    previewStack_->setCurrentWidget(layoutDevicePreview_);
    previewFrame_->setVisible(true);
    populateLayoutDeviceParameters(cell->parameters);
    updateParameterButtons();
}

void PropertiesPanel::setPathTool(int width, const std::string &layerName) {
    instanceId_ = 0;
    cellId_ = 0;
    layoutCellId_ = 0;
    layoutDeviceName_.clear();
    pathMode_ = true;
    pathWidth_ = std::max(1, width);
    pathLayerName_ = layerName;
    selection_->setText(QStringLiteral("Path tool"));
    previewFrame_->setVisible(false);
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

    const int widthRow = parameters_->rowCount();
    parameters_->insertRow(widthRow);
    auto *widthName = new QTableWidgetItem(QStringLiteral("width (nm)"));
    widthName->setFlags(widthName->flags() & ~Qt::ItemIsEditable);
    auto *widthValue = new QTableWidgetItem(QString::number(pathWidth_));
    widthValue->setToolTip(QStringLiteral("Path width in database units (nanometers)"));
    parameters_->setItem(widthRow, 0, widthName);
    parameters_->setItem(widthRow, 1, widthValue);
    updateParameterButtons();
}

QLabel *PropertiesPanel::addRow(QFormLayout *form, const QString &label) {
    auto *value = new QLabel(QStringLiteral("—"));
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value);
    return value;
}

void PropertiesPanel::populateParameters(const std::vector<ice::Parameter> &parameters) {
    QSignalBlocker blocker(parameters_);
    parameters_->setRowCount(0);
    for (const auto &parameter : parameters) {
        const int row = parameters_->rowCount();
        parameters_->insertRow(row);
        auto *name = new QTableWidgetItem(QString::fromStdString(parameter.name));
        name->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(parameter.id));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, name);
        parameters_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(parameter.value)));
    }
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
    if (!item || item->column() != 1)
        return;
    const auto *name = parameters_->item(item->row(), 0);
    if (!name)
        return;
    if (pathMode_) {
        if (name->text() != QStringLiteral("width (nm)"))
            return;
        bool accepted = false;
        const auto width = item->text().toInt(&accepted);
        if (!accepted || width <= 0) {
            QSignalBlocker blocker(parameters_);
            item->setText(QString::number(pathWidth_));
            return;
        }
        const int clamped = std::clamp(width, 1, 1000000);
        if (clamped != width) {
            QSignalBlocker blocker(parameters_);
            item->setText(QString::number(clamped));
        }
        pathWidth_ = clamped;
        if (onPathWidthChanged_)
            onPathWidthChanged_(clamped);
        return;
    }
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
