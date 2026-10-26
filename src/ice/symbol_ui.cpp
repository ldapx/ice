#include "symbol_ui.hpp"

#include "icelib/operations.hpp"
#include "icelib/symbols.hpp"

#include "ui_colors.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QSvgRenderer>
#include <QSvgWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <map>
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

std::vector<SymbolDefinition> toDefinitions(std::vector<ice::FileSymbol> fileSymbols) {
    std::vector<SymbolDefinition> definitions;
    definitions.reserve(fileSymbols.size());
    for (auto &fileSymbol : fileSymbols) {
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

std::vector<SymbolDefinition> genericSymbolLibrary() {
    return toDefinitions(ice::SymbolLibrary::load(symbolLibraryDirectory()));
}

std::vector<SymbolDefinition> packageSymbolLibrary(const std::filesystem::path &packageDirectory) {
    if (packageDirectory.empty())
        return {};
    try {
        return toDefinitions(ice::SymbolLibrary::load(packageDirectory, symbolLibraryDirectory()));
    } catch (const ice::ProjectError &) {
        return {};
    }
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
    std::string prefix;
    if (symbolName == "Input") {
        prefix = "IN";
    } else if (symbolName == "Output") {
        prefix = "OUT";
    } else {
        char elementPrefix = ice::spiceElementForSymbol(symbolName);
        if (elementPrefix == 0)
            elementPrefix = 'X';
        prefix = std::string(1, elementPrefix);
    }
    std::set<std::string> used;
    for (const auto &[_, instance] : cell.instances)
        used.insert(instance.name);
    for (std::size_t index = 1;; ++index) {
        const std::string candidate = prefix + std::to_string(index);
        if (!used.contains(candidate))
            return candidate;
    }
}

void SymbolPreview::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!renderer()->isValid())
        return;
    const auto artworkSize = renderer()->defaultSize();
    if (artworkSize.isEmpty())
        return;
    const auto available = QRectF(rect()).adjusted(8, 8, -8, -8);
    const qreal scale = std::min(available.width() / artworkSize.width(), available.height() / artworkSize.height());
    const QSizeF scaledSize(artworkSize.width() * scale, artworkSize.height() * scale);
    const QRectF target(available.center().x() - scaledSize.width() / 2.0,
                        available.center().y() - scaledSize.height() / 2.0, scaledSize.width(), scaledSize.height());

    // Render the artwork offscreen, then tint it with the palette axis color.
    // This mirrors the schematic editor, where symbol strokes are colorized so
    // they remain visible against the canvas background.
    const qreal ratio = devicePixelRatioF();
    const QSize pixelSize(static_cast<int>(std::ceil(target.width() * ratio)),
                          static_cast<int>(std::ceil(target.height() * ratio)));
    QImage artwork(pixelSize, QImage::Format_ARGB32_Premultiplied);
    artwork.setDevicePixelRatio(ratio);
    artwork.fill(Qt::transparent);
    {
        QPainter artworkPainter(&artwork);
        artworkPainter.setRenderHint(QPainter::Antialiasing);
        renderer()->render(&artworkPainter, QRectF(QPointF(0.0, 0.0), QSizeF(target.size())));
    }
    {
        QPainter tintPainter(&artwork);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(artwork.rect(), ice::uiColor(ice::UiColor::Axis1));
    }
    painter.drawImage(target.topLeft(), artwork);
}

SymbolLibraryDialog::SymbolLibraryDialog(std::vector<SymbolDefinition> generic, std::vector<SymbolDefinition> pdk,
                                         QWidget *parent)
    : QDialog(parent), symbols_(std::move(generic)) {
    symbols_.insert(symbols_.end(), std::make_move_iterator(pdk.begin()), std::make_move_iterator(pdk.end()));

    setWindowTitle(QStringLiteral("Device Library"));
    resize(960, 620);
    setMinimumSize(720, 480);

    auto *layout = new QVBoxLayout(this);
    auto *searchRow = new QHBoxLayout;
    searchRow->addWidget(new QLabel(QStringLiteral("Search:")));
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Search by device or category…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Device search"));
    searchRow->addWidget(search_, 1);
    layout->addLayout(searchRow);

    auto *splitter = new QSplitter(Qt::Horizontal);
    auto *browser = new QWidget;
    auto *browserLayout = new QVBoxLayout(browser);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    tabs_->setUsesScrollButtons(true);
    tabs_->setElideMode(Qt::ElideRight);
    browserLayout->addWidget(tabs_, 1);
    noResults_ = new QLabel(QStringLiteral("No matching devices."));
    noResults_->setAlignment(Qt::AlignCenter);
    noResults_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    noResults_->setVisible(false);
    browserLayout->addWidget(noResults_);
    splitter->addWidget(browser);

    auto *previewPanel = new QWidget;
    auto *previewLayout = new QVBoxLayout(previewPanel);
    previewLayout->setContentsMargins(8, 0, 0, 0);
    symbolName_ = new QLabel;
    auto nameFont = symbolName_->font();
    nameFont.setBold(true);
    nameFont.setPointSize(nameFont.pointSize() + 4);
    symbolName_->setFont(nameFont);
    symbolName_->setWordWrap(true);
    previewLayout->addWidget(symbolName_);
    symbolCategory_ = new QLabel;
    symbolCategory_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    previewLayout->addWidget(symbolCategory_);
    symbolPins_ = new QLabel;
    previewLayout->addWidget(symbolPins_);

    auto *previewFrame = new QFrame;
    previewFrame->setObjectName(QStringLiteral("symbolPreviewFrame"));
    previewFrame->setMinimumSize(260, 220);
    previewFrame->setStyleSheet(
        QStringLiteral("#symbolPreviewFrame { background: %1; border: 1px solid %2; "
                       "border-radius: 4px; }")
            .arg(ice::uiColor(ice::UiColor::Background2).name(), ice::uiColor(ice::UiColor::Border1).name()));
    auto *previewFrameLayout = new QVBoxLayout(previewFrame);
    preview_ = new SymbolPreview;
    preview_->setMinimumSize(240, 200);
    preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewFrameLayout->addWidget(preview_);
    previewLayout->addWidget(previewFrame, 1);

    previewLayout->addWidget(new QLabel(QStringLiteral("Default parameters")));
    symbolParameters_ = new QLabel;
    symbolParameters_->setWordWrap(true);
    symbolParameters_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewLayout->addWidget(symbolParameters_);
    splitter->addWidget(previewPanel);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({540, 340});
    layout->addWidget(splitter, 1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    const auto categoryRank = [](const QString &category) {
        static const QStringList preferred = {QStringLiteral("Passive"),       QStringLiteral("Sources"),
                                              QStringLiteral("Semiconductor"), QStringLiteral("Power"),
                                              QStringLiteral("Analog"),        QStringLiteral("Transistor")};
        const auto index = preferred.indexOf(category);
        if (index >= 0)
            return static_cast<int>(index);
        return category.startsWith(QStringLiteral("IHP")) ? static_cast<int>(preferred.size())
                                                          : static_cast<int>(preferred.size()) + 1;
    };
    const auto categoryLess = [categoryRank](const QString &left, const QString &right) {
        const auto leftRank = categoryRank(left);
        const auto rightRank = categoryRank(right);
        if (leftRank != rightRank)
            return leftRank < rightRank;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    };
    std::map<QString, std::vector<std::size_t>, decltype(categoryLess)> categories(categoryLess);
    for (std::size_t index = 0; index < symbols_.size(); ++index) {
        const auto category =
            symbols_[index].category.isEmpty() ? QStringLiteral("Uncategorized") : symbols_[index].category;
        categories[category].push_back(index);
    }
    for (const auto &[category, indices] : categories) {
        auto *list = new QListWidget;
        list->setAccessibleName(QStringLiteral("%1 devices").arg(category));
        for (const auto index : indices) {
            auto *item = new QListWidgetItem(symbols_[index].name, list);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(index));
        }
        tabs_->addTab(list, category);
        connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *) {
            if (current && !current->isHidden())
                updatePreview(static_cast<std::size_t>(current->data(Qt::UserRole).toULongLong()));
        });
    }
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) { showCurrentTab(); });
    connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { applyFilter(text); });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        if (auto *ok = buttons_->button(QDialogButtonBox::Ok); ok && ok->isEnabled())
            accept();
    });
    if (tabs_->count() > 0)
        tabs_->setCurrentIndex(0);
    showCurrentTab();
    search_->setFocus();
}

const SymbolDefinition &SymbolLibraryDialog::selected() const { return symbols_.at(selectedIndex_); }

QListWidget *SymbolLibraryDialog::currentList() const { return qobject_cast<QListWidget *>(tabs_->currentWidget()); }

void SymbolLibraryDialog::showCurrentTab() {
    auto *list = currentList();
    if (!list)
        return;
    auto *current = list->currentItem();
    if (!current || current->isHidden()) {
        for (int index = 0; index < list->count(); ++index) {
            auto *candidate = list->item(index);
            if (!candidate->isHidden()) {
                list->setCurrentItem(candidate);
                return;
            }
        }
    }
    if (current && !current->isHidden())
        updatePreview(static_cast<std::size_t>(current->data(Qt::UserRole).toULongLong()));
}

void SymbolLibraryDialog::updatePreview(std::size_t index) {
    if (index >= symbols_.size())
        return;
    selectedIndex_ = index;
    const auto &symbol = symbols_[index];
    symbolName_->setText(symbol.name);
    symbolCategory_->setText(symbol.category.isEmpty() ? QStringLiteral("Uncategorized") : symbol.category);
    symbolPins_->setText(QStringLiteral("Pins: %1").arg(symbol.pins.size()));

    QString parameters;
    for (const auto &parameter : symbol.parameters) {
        if (!parameters.isEmpty())
            parameters += QLatin1Char('\n');
        parameters += QStringLiteral("%1 = %2").arg(QString::fromStdString(parameter.name),
                                                    QString::fromStdString(parameter.value));
    }
    symbolParameters_->setText(parameters.isEmpty() ? QStringLiteral("No default parameters") : parameters);
    preview_->load(symbol.svg.toUtf8());
}

void SymbolLibraryDialog::applyFilter(const QString &text) {
    const auto needle = text.trimmed();
    bool anyVisible = false;
    int firstVisibleTab = -1;
    for (int tab = 0; tab < tabs_->count(); ++tab) {
        auto *list = qobject_cast<QListWidget *>(tabs_->widget(tab));
        if (!list)
            continue;
        const bool categoryMatches = tabs_->tabText(tab).contains(needle, Qt::CaseInsensitive);
        bool tabVisible = false;
        for (int row = 0; row < list->count(); ++row) {
            auto *item = list->item(row);
            const bool visible =
                needle.isEmpty() || categoryMatches || item->text().contains(needle, Qt::CaseInsensitive);
            item->setHidden(!visible);
            tabVisible = tabVisible || visible;
        }
        tabs_->setTabVisible(tab, tabVisible);
        if (tabVisible) {
            anyVisible = true;
            if (firstVisibleTab < 0)
                firstVisibleTab = tab;
        }
    }

    tabs_->setVisible(anyVisible);
    noResults_->setVisible(!anyVisible);
    if (auto *ok = buttons_->button(QDialogButtonBox::Ok))
        ok->setEnabled(anyVisible);
    if (!anyVisible)
        return;
    if (!tabs_->isTabVisible(tabs_->currentIndex()))
        tabs_->setCurrentIndex(firstVisibleTab);
    auto *list = currentList();
    if (!list)
        return;
    auto *current = list->currentItem();
    if (current && !current->isHidden())
        return;
    for (int row = 0; row < list->count(); ++row) {
        auto *candidate = list->item(row);
        if (!candidate->isHidden()) {
            list->setCurrentItem(candidate);
            return;
        }
    }
}

SymbolPropertiesDialog::SymbolPropertiesDialog(const std::string &symbolName, const ice::SchematicInstance &instance,
                                               QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Device Properties"));
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
        {QStringLiteral("m45"), ice::Orientation::m45},   {QStringLiteral("m135"), ice::Orientation::m135},
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
