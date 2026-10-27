#include "app_widgets.hpp"

#include "icelib/verification.hpp"

#include "icons.hpp"
#include "package_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

DiagnosticsPanel::DiagnosticsPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(heading(QStringLiteral("Diagnostics")));
    auto *diagnostics = new QListWidget;
    diagnostics->setAccessibleName(QStringLiteral("Diagnostics list"));
    diagnostics->addItem(QStringLiteral("No diagnostics"));
    layout->addWidget(diagnostics);
}

ActivityPanel::ActivityPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    auto *agent = new QWidget;
    auto *agentLayout = new QHBoxLayout(agent);
    auto *transcript = new QPlainTextEdit;
    transcript->setReadOnly(true);
    transcript->setPlaceholderText(QStringLiteral("Agent conversation and tool-call updates"));

    auto *sessionPanel = new QFrame;
    sessionPanel->setFrameShape(QFrame::StyledPanel);
    sessionPanel->setFixedWidth(240);
    auto *sessionLayout = new QVBoxLayout(sessionPanel);
    sessionLayout->setContentsMargins(10, 8, 10, 8);
    sessionLayout->setSpacing(6);
    auto *sessionTitle = new QLabel(QStringLiteral("No active session"));
    sessionTitle->setWordWrap(true);
    sessionTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sessionLayout->addWidget(sessionTitle);
    sessionLayout->addSpacing(6);
    auto *tokens = new QLabel(QStringLiteral("0 / 128,000 tokens"));
    sessionLayout->addWidget(tokens);
    auto *contextUsage = new QProgressBar;
    contextUsage->setRange(0, 100);
    contextUsage->setValue(0);
    contextUsage->setFormat(QStringLiteral("%p% used"));
    contextUsage->setMaximumHeight(18);
    sessionLayout->addWidget(contextUsage);
    sessionLayout->addSpacing(6);
    auto *mcpServers = new QListWidget;
    mcpServers->setAccessibleName(QStringLiteral("MCP servers"));
    mcpServers->setAlternatingRowColors(true);
    auto *noServers = new QListWidgetItem(QStringLiteral("No MCP servers connected"), mcpServers);
    noServers->setFlags(Qt::NoItemFlags);
    sessionLayout->addWidget(mcpServers, 1);

    auto *conversationColumn = new QVBoxLayout;
    conversationColumn->addWidget(transcript, 1);
    auto *prompt = new QLineEdit;
    prompt->setPlaceholderText(QStringLiteral("Ask the agent about the current project..."));
    conversationColumn->addWidget(prompt);
    agentLayout->addLayout(conversationColumn, 1);

    auto *sessionColumn = new QVBoxLayout;
    sessionColumn->setSpacing(8);
    sessionColumn->addWidget(sessionPanel, 1);
    auto *sendButton = new QPushButton(QStringLiteral("Send"));
    sendButton->setFixedWidth(240);
    sessionColumn->addWidget(sendButton);
    agentLayout->addLayout(sessionColumn, 0);
    tabs->addTab(agent, QStringLiteral("Agent"));

    auto *messages = new QPlainTextEdit;
    messages->setReadOnly(true);
    messages->setPlaceholderText(QStringLiteral("Structured application and external-tool messages"));
    tabs->addTab(messages, QStringLiteral("Messages"));

    auto *jobs = new QWidget;
    auto *jobsLayout = new QVBoxLayout(jobs);
    jobsLayout->addWidget(new QLabel(QStringLiteral("No active jobs")));
    auto *progress = new QProgressBar;
    progress->setRange(0, 100);
    progress->setValue(0);
    progress->setFormat(QStringLiteral("Ready"));
    jobsLayout->addWidget(progress);
    jobsLayout->addWidget(new QPushButton(QStringLiteral("Cancel selected job")));
    jobsLayout->addStretch();
    tabs->addTab(jobs, QStringLiteral("Jobs"));

    auto *drcPage = new QWidget;
    auto *drcLayout = new QVBoxLayout(drcPage);
    drcSummary_ = new QLabel(QStringLiteral("No DRC report yet. Use Tools > Run DRC to run gdscheck."));
    drcSummary_->setWordWrap(true);
    drcSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    drcLayout->addWidget(drcSummary_);
    drcTable_ = new QTableWidget(0, 5);
    drcTable_->setAccessibleName(QStringLiteral("DRC report"));
    drcTable_->setHorizontalHeaderLabels({QStringLiteral("Severity"), QStringLiteral("Rule"), QStringLiteral("Message"),
                                          QStringLiteral("Cell"), QStringLiteral("Location")});
    drcTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    drcTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    drcTable_->setAlternatingRowColors(true);
    drcTable_->setSortingEnabled(true);
    drcTable_->verticalHeader()->setVisible(false);
    drcTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    drcTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    drcTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    drcTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    drcTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    drcLayout->addWidget(drcTable_, 1);
    drcTabIndex_ = tabs->addTab(drcPage, QStringLiteral("DRC Report"));

    auto *lvsReport = new QPlainTextEdit;
    lvsReport->setReadOnly(true);
    lvsReport->setAccessibleName(QStringLiteral("LVS report"));
    lvsReport->setPlaceholderText(QStringLiteral("LVS report will appear here after a layout-versus-schematic check."));
    tabs->addTab(lvsReport, QStringLiteral("LVS Report"));

    layout->addWidget(tabs);
    tabs_ = tabs;
}

namespace {
QString drcSeverityLabel(ice::VerificationSeverity severity) {
    switch (severity) {
    case ice::VerificationSeverity::error:
        return QStringLiteral("Error");
    case ice::VerificationSeverity::warning:
        return QStringLiteral("Warning");
    case ice::VerificationSeverity::info:
        return QStringLiteral("Info");
    }
    return QStringLiteral("Unknown");
}

QString drcLocationLabel(const ice::Rect &rect) {
    if (rect.width == 0 && rect.height == 0)
        return {};
    const auto format = [](ice::Coordinate value) { return QString::number(value / 1000.0, 'f', 3); };
    return QStringLiteral("%1, %2 (%3 x %4) um")
        .arg(format(rect.x), format(rect.y), format(rect.width), format(rect.height));
}
} // namespace

void ActivityPanel::setDrcReport(const ice::VerificationReport &report, const QString &reportPath) {
    if (!drcTable_ || !drcSummary_)
        return;
    drcTable_->setSortingEnabled(false);
    drcTable_->setRowCount(0);
    int errors = 0;
    int warnings = 0;
    int infos = 0;
    for (const auto &violation : report.drc) {
        switch (violation.severity) {
        case ice::VerificationSeverity::error:
            ++errors;
            break;
        case ice::VerificationSeverity::warning:
            ++warnings;
            break;
        case ice::VerificationSeverity::info:
            ++infos;
            break;
        }
        const int row = drcTable_->rowCount();
        drcTable_->insertRow(row);
        auto *severity = new QTableWidgetItem(drcSeverityLabel(violation.severity));
        auto *rule = new QTableWidgetItem(QString::fromStdString(violation.rule));
        auto *message = new QTableWidgetItem(QString::fromStdString(violation.message));
        auto *cell = new QTableWidgetItem(QString::fromStdString(violation.cell));
        auto *location = new QTableWidgetItem(drcLocationLabel(violation.location));
        severity->setData(Qt::UserRole, static_cast<int>(violation.severity));
        drcTable_->setItem(row, 0, severity);
        drcTable_->setItem(row, 1, rule);
        drcTable_->setItem(row, 2, message);
        drcTable_->setItem(row, 3, cell);
        drcTable_->setItem(row, 4, location);
    }
    drcTable_->setSortingEnabled(true);
    QString summary;
    if (report.drc.empty()) {
        summary = QStringLiteral("DRC completed: no violations found.");
    } else {
        summary = QStringLiteral("DRC completed: %1 error(s), %2 warning(s), %3 info item(s).")
                      .arg(errors)
                      .arg(warnings)
                      .arg(infos);
    }
    if (!reportPath.isEmpty())
        summary += QStringLiteral(" Report: %1").arg(reportPath);
    drcSummary_->setText(summary);
    showDrcTab();
}

void ActivityPanel::setDrcRunning(const QString &message) {
    if (!drcSummary_ || !drcTable_)
        return;
    drcTable_->setRowCount(0);
    drcSummary_->setText(message.isEmpty() ? QStringLiteral("Running gdscheck...") : message);
    showDrcTab();
}

void ActivityPanel::setDrcError(const QString &message) {
    if (!drcSummary_ || !drcTable_)
        return;
    drcTable_->setRowCount(0);
    drcSummary_->setText(message);
    showDrcTab();
}

void ActivityPanel::showDrcTab() {
    if (tabs_ && drcTabIndex_ >= 0)
        tabs_->setCurrentIndex(drcTabIndex_);
}

CommandPalette::CommandPalette(const QList<QAction *> &actions, QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Command Palette"));
    setModal(true);
    resize(560, 420);

    auto *layout = new QVBoxLayout(this);
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("Type to search commands..."));
    search_->setAccessibleName(QStringLiteral("Command palette search"));
    layout->addWidget(search_);

    list_ = new QListWidget;
    list_->setAccessibleName(QStringLiteral("Command palette results"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this, actions](const QString &text) {
        list_->clear();
        for (auto *action : actions) {
            if (text.isEmpty() || action->text().contains(text, Qt::CaseInsensitive)) {
                auto *item = new QListWidgetItem(action->text(), list_);
                item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(action)));
            }
        }
        if (list_->count() > 0)
            list_->setCurrentRow(0);
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        if (list_->currentItem() == nullptr)
            return;
        auto *action = reinterpret_cast<QAction *>(list_->currentItem()->data(Qt::UserRole).value<quintptr>());
        if (action != nullptr) {
            accept();
            action->trigger();
        }
    });
    search_->setFocus();
    search_->setText(QString());
}

StatusBar::StatusBar(QWidget *parent) : QStatusBar(parent) {}

void StatusBar::setProjectName(const QString &name) {
    if (projectName_ == name)
        return;
    projectName_ = name;
    update();
}

void StatusBar::paintEvent(QPaintEvent *event) {
    QStatusBar::paintEvent(event);
    if (projectName_.isEmpty())
        return;
    QPainter painter(this);
    painter.setPen(palette().windowText().color());
    const auto text = fontMetrics().elidedText(projectName_, Qt::ElideMiddle, qMax(0, width() - 320));
    painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text);
}

namespace {
constexpr auto kManualUrl = "https://github.com/ldapx/ice/tree/main/docs";
constexpr auto kDonationUrl = "https://github.com/sponsors/ldapx";
constexpr auto kSourceUrl = "https://github.com/ldapx/ice";
constexpr auto kWebsiteUrl = "https://github.com/ldapx/ice";
} // namespace

StartupView::StartupView(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(40, 28, 40, 28);
    layout->setSpacing(16);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);
    auto *logo = new QLabel;
    const auto logoPixmap = ice::appLogo(96);
    logo->setPixmap(logoPixmap);
    logo->setFixedSize(96, 96);
    logo->setAlignment(Qt::AlignCenter);
    logo->setVisible(!logoPixmap.isNull());
    header->addWidget(logo, 0, Qt::AlignTop);

    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("ICE"));
    auto titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    titleColumn->addWidget(title);

    subtitle_ = new QLabel(QStringLiteral("Integrated Circuits Electronics Design Automation"));
    auto subtitleFont = subtitle_->font();
    subtitleFont.setPointSize(subtitleFont.pointSize() + 2);
    subtitle_->setFont(subtitleFont);
    titleColumn->addWidget(subtitle_);
    titleColumn->addStretch();
    header->addLayout(titleColumn, 1);
    layout->addLayout(header);

    auto *content = new QHBoxLayout;
    content->setSpacing(20);

    auto *left = new QVBoxLayout;
    left->setSpacing(16);
    auto *createGroup = new QGroupBox(QStringLiteral("Create Project"));
    auto *form = new QFormLayout(createGroup);
    name_ = new QLineEdit;
    name_->setPlaceholderText(QStringLiteral("My design"));
    name_->setClearButtonEnabled(true);
    location_ = new QLineEdit(QDir::homePath());
    auto *browseButton = new QPushButton(QStringLiteral("Browse…"));
    auto *locationRow = new QWidget;
    auto *locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->addWidget(location_, 1);
    locationLayout->addWidget(browseButton);
    form->addRow(QStringLiteral("Project name"), name_);
    form->addRow(QStringLiteral("Location"), locationRow);
    auto *buttonRow = new QHBoxLayout;
    auto *openButton = new QPushButton(QStringLiteral("Open Project…"));
    createButton_ = new QPushButton(QStringLiteral("Create Project"));
    createButton_->setDefault(true);
    buttonRow->addWidget(openButton);
    buttonRow->addStretch();
    buttonRow->addWidget(createButton_);
    form->addRow(buttonRow);
    left->addWidget(createGroup);

    auto *recentGroup = new QGroupBox(QStringLiteral("Recent Projects"));
    auto *recentLayout = new QVBoxLayout(recentGroup);
    recent_ = new QListWidget;
    recent_->setAlternatingRowColors(true);
    recent_->setAccessibleName(QStringLiteral("Recent projects"));
    recent_->setMinimumHeight(200);
    recentLayout->addWidget(recent_, 1);
    left->addWidget(recentGroup, 1);
    content->addLayout(left, 3);

    auto *right = new QVBoxLayout;
    right->setSpacing(16);
    auto *communityGroup = new QGroupBox(QStringLiteral("Community"));
    auto *communityLayout = new QVBoxLayout(communityGroup);
    communityLayout->addWidget(makeLink(QStringLiteral("Manual"), QString::fromLatin1(kManualUrl)));
    communityLayout->addWidget(makeLink(QStringLiteral("Donate"), QString::fromLatin1(kDonationUrl)));
    communityLayout->addWidget(makeLink(QStringLiteral("Source code"), QString::fromLatin1(kSourceUrl)));
    communityLayout->addWidget(makeLink(QStringLiteral("Website"), QString::fromLatin1(kWebsiteUrl)));
    communityLayout->addStretch();
    right->addWidget(communityGroup);

    auto *newsGroup = new QGroupBox(QStringLiteral("News"));
    auto *newsLayout = new QVBoxLayout(newsGroup);
    newsPlaceholder_ = new QLabel(QStringLiteral("No news yet."));
    newsPlaceholder_->setAlignment(Qt::AlignCenter);
    newsLayout->addWidget(newsPlaceholder_);
    newsLayout->addStretch();
    right->addWidget(newsGroup, 1);
    content->addLayout(right, 2);

    layout->addLayout(content, 1);

    connect(browseButton, &QPushButton::clicked, this, [this] {
        const auto directory =
            QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Project Location"), location_->text());
        if (!directory.isEmpty())
            location_->setText(directory);
    });
    connect(openButton, &QPushButton::clicked, this, [this] {
        if (onOpenProject_)
            onOpenProject_();
    });
    connect(createButton_, &QPushButton::clicked, this, [this] {
        if (onCreateProject_)
            onCreateProject_(name_->text().trimmed(), location_->text().trimmed());
    });
    auto submit = [this] {
        if (createButton_->isEnabled())
            createButton_->click();
    };
    connect(name_, &QLineEdit::returnPressed, this, submit);
    connect(location_, &QLineEdit::returnPressed, this, submit);
    auto updateCreateButton = [this] {
        createButton_->setEnabled(!name_->text().trimmed().isEmpty() && !location_->text().trimmed().isEmpty());
    };
    connect(name_, &QLineEdit::textChanged, this, updateCreateButton);
    connect(location_, &QLineEdit::textChanged, this, updateCreateButton);
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        const auto path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty() && onOpenRecent_)
            onOpenRecent_(path);
    });
    updateCreateButton();
    refreshAppearance();
}

void StartupView::setOnCreateProject(CreateProjectCallback callback) { onCreateProject_ = std::move(callback); }
void StartupView::setOnOpenProject(OpenProjectCallback callback) { onOpenProject_ = std::move(callback); }
void StartupView::setOnOpenRecent(OpenRecentCallback callback) { onOpenRecent_ = std::move(callback); }

void StartupView::setRecentProjects(const QStringList &paths) {
    recent_->clear();
    if (paths.isEmpty()) {
        auto *item = new QListWidgetItem(QStringLiteral("No recent projects"), recent_);
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const auto &path : paths) {
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), recent_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    }
}

void StartupView::clearCreateForm() {
    name_->clear();
    name_->setFocus();
}

void StartupView::refreshAppearance() {
    const auto muted = ice::uiColor(ice::UiColor::Text2).name();
    subtitle_->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
    newsPlaceholder_->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
}

QLabel *StartupView::makeLink(const QString &text, const QString &url) {
    auto *label = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), text.toHtmlEscaped()));
    label->setOpenExternalLinks(true);
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    return label;
}

EmptySchematicPanel::EmptySchematicPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addStretch(1);
    auto *card = new QGroupBox(QStringLiteral("Create a schematic"));
    card->setMaximumWidth(520);
    auto *cardLayout = new QVBoxLayout(card);
    auto *description = new QLabel(QStringLiteral("This project has no schematics yet. Create one to start drawing."));
    description->setWordWrap(true);
    cardLayout->addWidget(description);
    auto *form = new QFormLayout;
    name_ = new QLineEdit(QStringLiteral("top"));
    name_->setClearButtonEnabled(true);
    pdk_ = new QComboBox;
    pdk_->setAccessibleName(QStringLiteral("Schematic PDK"));
    form->addRow(QStringLiteral("Schematic name"), name_);
    form->addRow(QStringLiteral("PDK"), pdk_);
    cardLayout->addLayout(form);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    auto *createButton = new QPushButton(QStringLiteral("Create Schematic"));
    createButton->setDefault(true);
    buttonRow->addWidget(createButton);
    cardLayout->addLayout(buttonRow);
    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    refreshPdks(configuredPackageInstallDirectory());
    connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
    connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
}

void EmptySchematicPanel::setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

void EmptySchematicPanel::refreshPdks(const std::filesystem::path &installDirectory) {
    const auto current = pdk_->currentData().toString();
    QSignalBlocker blocker(pdk_);
    pdk_->clear();
    pdk_->addItem(QStringLiteral("Generic PDK"), QString::fromLatin1(ice::genericPdkId));
    for (const auto &entry : installedPdkPackages(installDirectory)) {
        const auto packageName = QString::fromStdString(entry.spec.name);
        auto label = packagePdkDisplayName(entry.spec);
        if (label != packageName)
            label += QStringLiteral("  (%1)").arg(packageName);
        pdk_->addItem(label, packageName);
    }
    const auto index = pdk_->findData(current);
    if (index >= 0)
        pdk_->setCurrentIndex(index);
}

void EmptySchematicPanel::prepareForDisplay() {
    name_->selectAll();
    name_->setFocus();
}

void EmptySchematicPanel::submit() {
    const auto name = name_->text().trimmed();
    if (name.isEmpty()) {
        name_->setFocus();
        return;
    }
    if (onCreate_)
        onCreate_(name, pdk_->currentData().toString().toStdString());
}

EmptyLayoutPanel::EmptyLayoutPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addStretch(1);
    auto *card = new QGroupBox(QStringLiteral("Create a layout cell"));
    card->setMaximumWidth(520);
    auto *cardLayout = new QVBoxLayout(card);
    auto *description =
        new QLabel(QStringLiteral("This project has no layout cells yet. Create one to start drawing."));
    description->setWordWrap(true);
    cardLayout->addWidget(description);
    auto *form = new QFormLayout;
    name_ = new QLineEdit(QStringLiteral("top"));
    name_->setClearButtonEnabled(true);
    pdk_ = new QComboBox;
    pdk_->setAccessibleName(QStringLiteral("Layout PDK"));
    form->addRow(QStringLiteral("Layout cell name"), name_);
    form->addRow(QStringLiteral("PDK"), pdk_);
    cardLayout->addLayout(form);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    auto *createButton = new QPushButton(QStringLiteral("Create Layout Cell"));
    createButton->setDefault(true);
    buttonRow->addWidget(createButton);
    cardLayout->addLayout(buttonRow);
    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    refreshPdks(configuredPackageInstallDirectory());
    connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
    connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
}

void EmptyLayoutPanel::setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

void EmptyLayoutPanel::refreshPdks(const std::filesystem::path &installDirectory) {
    const auto current = pdk_->currentData().toString();
    QSignalBlocker blocker(pdk_);
    pdk_->clear();
    pdk_->addItem(QStringLiteral("Generic PDK"), QString::fromLatin1(ice::genericPdkId));
    for (const auto &entry : installedPdkPackages(installDirectory)) {
        const auto packageName = QString::fromStdString(entry.spec.name);
        auto label = packagePdkDisplayName(entry.spec);
        if (label != packageName)
            label += QStringLiteral("  (%1)").arg(packageName);
        pdk_->addItem(label, packageName);
    }
    const auto index = pdk_->findData(current);
    if (index >= 0)
        pdk_->setCurrentIndex(index);
}

void EmptyLayoutPanel::prepareForDisplay() {
    name_->selectAll();
    name_->setFocus();
}

void EmptyLayoutPanel::submit() {
    const auto name = name_->text().trimmed();
    if (name.isEmpty()) {
        name_->setFocus();
        return;
    }
    if (onCreate_)
        onCreate_(name, pdk_->currentData().toString().toStdString());
}

CreateLayoutDialog::CreateLayoutDialog(const std::filesystem::path &installDirectory, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Layout Cell"));
    setModal(true);
    resize(600, 300);

    auto *layout = new QVBoxLayout(this);
    auto *description =
        new QLabel(QStringLiteral("Choose the PDK used by this layout cell. Generic PDKs use the built-in layer set; "
                                  "package PDKs add their technology metadata."));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout;
    name_ = new QLineEdit(QStringLiteral("top"));
    name_->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("Layout cell name"), name_);
    pdk_ = new QComboBox;
    pdk_->setAccessibleName(QStringLiteral("Layout PDK"));
    pdk_->addItem(QStringLiteral("Generic PDK"), QString::fromLatin1(ice::genericPdkId));
    pdk_->setItemData(0, QStringLiteral("Built-in generic layer set."), Qt::ToolTipRole);
    for (const auto &entry : installedPdkPackages(installDirectory)) {
        const auto displayName = packagePdkDisplayName(entry.spec);
        const auto packageName = QString::fromStdString(entry.spec.name);
        auto label = displayName;
        if (displayName != packageName)
            label += QStringLiteral("  (%1)").arg(packageName);
        label += QStringLiteral("  %1").arg(QString::fromStdString(entry.spec.version));
        pdk_->addItem(label, packageName);
        auto detail = QStringLiteral("Package PDK.");
        if (!entry.spec.description.empty())
            detail = QString::fromStdString(entry.spec.description) + QStringLiteral("  ") + detail;
        pdk_->setItemData(pdk_->count() - 1, detail, Qt::ToolTipRole);
    }
    form->addRow(QStringLiteral("PDK"), pdk_);
    layout->addLayout(form);

    pdkDescription_ = new QLabel;
    pdkDescription_->setWordWrap(true);
    pdkDescription_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    layout->addWidget(pdkDescription_);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    layout->addWidget(buttons);
    connect(pdk_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { pdkDescription_->setText(pdk_->currentData(Qt::ToolTipRole).toString()); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (name_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Create Layout Cell"),
                                 QStringLiteral("Enter a layout cell name."));
            name_->setFocus();
            return;
        }
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    pdkDescription_->setText(pdk_->currentData(Qt::ToolTipRole).toString());
    name_->selectAll();
    name_->setFocus();
}

QString CreateLayoutDialog::layoutName() const { return name_->text().trimmed(); }
std::string CreateLayoutDialog::pdkId() const { return pdk_->currentData().toString().toStdString(); }

QSize WorkspaceStack::minimumSizeHint() const { return QSize(QStackedWidget::minimumSizeHint().width(), 0); }
