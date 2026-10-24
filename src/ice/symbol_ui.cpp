#include "symbol_ui.hpp"

#include "icelib/operations.hpp"
#include "icelib/symbols.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>

std::filesystem::path symbolLibraryDirectory() {
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "symbols",
        std::filesystem::path(appDir) / ".." / "symbols",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "symbols",
#endif
        std::filesystem::path("symbols"),
        std::filesystem::path("/usr/share/ice/symbols"),
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_directory(candidate, status) && !status)
            return candidate;
    }
    throw ice::ProjectError("symbol.library", "Symbol library directory not found (searched app dir, ./symbols, "
                                              "/usr/share/ice/symbols)");
}

std::vector<SymbolDefinition> genericSymbolLibrary() {
    std::vector<SymbolDefinition> definitions;
    for (auto &fileSymbol : ice::SymbolLibrary::load(symbolLibraryDirectory())) {
        SymbolDefinition definition;
        definition.name = QString::fromStdString(fileSymbol.name);
        definition.category = QString::fromStdString(fileSymbol.category);
        definition.svg = QString::fromStdString(fileSymbol.svg);
        definition.pins = std::move(fileSymbol.pins);
        definition.parameters = std::move(fileSymbol.defaultParameters);
        definitions.push_back(std::move(definition));
    }
    return definitions;
}

const std::vector<SymbolDefinition> &ihpSymbolLibrary() {
    static const std::vector<SymbolDefinition> definitions = {
        {"IHP NMOS 1.2V",
         "IHP SG13G2 / MOS",
         "<svg viewBox='0 0 120 120'><path d='M45 20v80M60 35v50M75 35v50M20 35h25M20 85h25M75 60h35M60 20v15M60 "
         "85v15' fill='none' stroke='black' stroke-width='4'/></svg>",
         {{0, "G", -60, -25}, {0, "S", 0, 60}, {0, "D", 0, -60}}},
        {"IHP PMOS 1.2V",
         "IHP SG13G2 / MOS",
         "<svg viewBox='0 0 120 120'><path d='M45 20v80M60 35v50M75 35v50M20 35h25M20 85h25M75 60h35M60 20v15M60 "
         "85v15' fill='none' stroke='black' stroke-width='4'/><circle cx='35' cy='35' r='6' fill='none' stroke='black' "
         "stroke-width='3'/></svg>",
         {{0, "G", -60, -25}, {0, "S", 0, 60}, {0, "D", 0, -60}}},
        {"IHP NPN HBT",
         "IHP SG13G2 / Bipolar",
         "<svg viewBox='0 0 120 120'><path d='M55 20v80M70 35v50M20 45h35M20 75h35M70 60h35M70 20v15M70 85v15' "
         "fill='none' stroke='black' stroke-width='4'/><path d='M82 52l10 8-10 8' fill='none' stroke='black' "
         "stroke-width='3'/></svg>",
         {{0, "B", -60, -25}, {0, "E", 0, 60}, {0, "C", 0, -60}}},
        {"IHP MIM Capacitor",
         "IHP SG13G2 / Passive",
         "<svg viewBox='0 0 120 60'><path d='M0 30h45M75 30h45M45 8v44M75 8v44' fill='none' stroke='black' "
         "stroke-width='4'/></svg>",
         {{0, "1", -60, 0}, {0, "2", 60, 0}}},
    };
    return definitions;
}

std::vector<ice::Parameter> instanceDefaults(const SymbolDefinition &definition) {
    auto parameters = definition.parameters;
    if (definition.name == QStringLiteral("Voltage Source")) {
        const bool hasVoltage = std::any_of(parameters.begin(), parameters.end(), [](const ice::Parameter &parameter) {
            return QString::fromStdString(parameter.name)
                       .trimmed()
                       .compare(QStringLiteral("voltage"), Qt::CaseInsensitive) == 0;
        });
        if (!hasVoltage)
            parameters.push_back({0, "voltage", "1"});
    }
    return parameters;
}

// Assigns the next free SPICE-style reference designator (V1, V2, I1, R1, ...)
// for a newly placed instance. Using the symbol name directly would create
// duplicate names when the same source or passive is placed more than once.
std::string nextInstanceName(const ice::SchematicCell &cell, const std::string &symbolName) {
    char prefix = ice::spiceElementForSymbol(symbolName);
    if (prefix == 0)
        prefix = 'X';
    std::set<std::string> used;
    for (const auto &[_, instance] : cell.instances)
        used.insert(instance.name);
    for (std::size_t index = 1;; ++index) {
        const std::string candidate = std::string(1, prefix) + std::to_string(index);
        if (!used.contains(candidate))
            return candidate;
    }
}

SymbolLibraryDialog::SymbolLibraryDialog(std::vector<SymbolDefinition> generic, bool ihpEnabled, QWidget *parent)
    : QDialog(parent), generic_(std::move(generic)) {
    setWindowTitle(QStringLiteral("Symbol Library"));
    resize(520, 420);
    auto *layout = new QVBoxLayout(this);
    list_ = new QListWidget;
    for (const auto &symbol : generic_)
        list_->addItem(symbol.category + QStringLiteral(" / ") + symbol.name);
    if (ihpEnabled)
        for (const auto &symbol : ihpSymbolLibrary())
            list_->addItem(symbol.category + QStringLiteral(" / ") + symbol.name);
    layout->addWidget(list_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    list_->setCurrentRow(0);
}

const SymbolDefinition &SymbolLibraryDialog::selected() const {
    if (list_->currentRow() < static_cast<int>(generic_.size()))
        return generic_.at(list_->currentRow());
    return ihpSymbolLibrary().at(list_->currentRow() - static_cast<int>(generic_.size()));
}

SymbolPropertiesDialog::SymbolPropertiesDialog(const std::string &symbolName, const ice::SchematicInstance &instance,
                                               QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Symbol Properties"));
    setModal(true);
    resize(430, 420);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    name_ = new QLineEdit(QString::fromStdString(instance.name));
    form->addRow(QStringLiteral("Instance name"), name_);
    auto *symbol = new QLabel(QString::fromStdString(symbolName));
    symbol->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(QStringLiteral("Symbol"), symbol);
    x_ = new QSpinBox;
    y_ = new QSpinBox;
    for (auto *spin : {x_, y_}) {
        spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        spin->setSingleStep(10);
    }
    x_->setValue(static_cast<int>(
        std::clamp<ice::Coordinate>(instance.x, std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
    y_->setValue(static_cast<int>(
        std::clamp<ice::Coordinate>(instance.y, std::numeric_limits<int>::min(), std::numeric_limits<int>::max())));
    form->addRow(QStringLiteral("Position X"), x_);
    form->addRow(QStringLiteral("Position Y"), y_);
    orientation_ = new QComboBox;
    for (const auto &entry : orientations())
        orientation_->addItem(entry.label, static_cast<int>(entry.orientation));
    orientation_->setCurrentIndex(orientation_->findData(static_cast<int>(instance.orientation)));
    form->addRow(QStringLiteral("Orientation"), orientation_);
    layout->addLayout(form);

    auto *parameterHeading = new QLabel(QStringLiteral("Parameters"));
    parameterHeading->setStyleSheet(QStringLiteral("font-weight: bold; padding-top: 6px;"));
    layout->addWidget(parameterHeading);
    parameters_ = new QTableWidget(0, 2);
    parameters_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
    parameters_->setAccessibleName(QStringLiteral("Instance parameters"));
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(parameters_, 1);
    for (const auto &parameter : instance.parameters)
        addParameterRow(
            {parameter.id, QString::fromStdString(parameter.name), QString::fromStdString(parameter.value)});

    auto *buttons = new QWidget;
    auto *buttonRow = new QHBoxLayout(buttons);
    buttonRow->setContentsMargins(0, 0, 0, 0);
    auto *add = new QPushButton(QStringLiteral("Add parameter"));
    auto *remove = new QPushButton(QStringLiteral("Remove parameter"));
    buttonRow->addWidget(add);
    buttonRow->addWidget(remove);
    buttonRow->addStretch();
    layout->addWidget(buttons);

    auto *close = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(close);

    connect(add, &QPushButton::clicked, this, [this] { addParameterRow({}); });
    connect(remove, &QPushButton::clicked, this, [this] {
        for (auto *item : parameters_->selectedItems())
            parameters_->removeRow(item->row());
        if (parameters_->rowCount() > 0)
            parameters_->selectRow(parameters_->rowCount() - 1);
    });
    connect(close, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString SymbolPropertiesDialog::instanceName() const { return name_->text().trimmed(); }
ice::Coordinate SymbolPropertiesDialog::positionX() const { return x_->value(); }
ice::Coordinate SymbolPropertiesDialog::positionY() const { return y_->value(); }
ice::Orientation SymbolPropertiesDialog::orientation() const {
    return static_cast<ice::Orientation>(orientation_->currentData().toInt());
}

std::vector<ParameterEdit> SymbolPropertiesDialog::parameters() const {
    std::vector<ParameterEdit> result;
    result.reserve(parameters_->rowCount());
    for (int row = 0; row < parameters_->rowCount(); ++row) {
        const auto *name = parameters_->item(row, 0);
        const auto *value = parameters_->item(row, 1);
        result.push_back({name->data(Qt::UserRole).toULongLong(), name->text(), value ? value->text() : QString()});
    }
    return result;
}

const std::vector<SymbolPropertiesDialog::OrientationEntry> &SymbolPropertiesDialog::orientations() {
    static const std::vector<OrientationEntry> entries = {
        {QStringLiteral("r0"), ice::Orientation::r0},     {QStringLiteral("r90"), ice::Orientation::r90},
        {QStringLiteral("r180"), ice::Orientation::r180}, {QStringLiteral("r270"), ice::Orientation::r270},
        {QStringLiteral("mx"), ice::Orientation::mx},     {QStringLiteral("my"), ice::Orientation::my},
    };
    return entries;
}

void SymbolPropertiesDialog::addParameterRow(ParameterEdit parameter) {
    const int row = parameters_->rowCount();
    parameters_->insertRow(row);
    auto *name = new QTableWidgetItem(parameter.name);
    name->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(parameter.id));
    parameters_->setItem(row, 0, name);
    parameters_->setItem(row, 1, new QTableWidgetItem(parameter.value));
    parameters_->setCurrentCell(row, 0);
}
