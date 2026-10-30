#include "app_widgets.hpp"

#include "icelib/verification.hpp"

#include "icons.hpp"
#include "package_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRunnable>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#ifdef ICE_HAVE_AGENTSDK
#include <agentsdk/acp/acp_agent_manager.hpp>
#include <agentsdk/acp/acp_client.hpp>
#include <agentsdk/acp/acp_session.hpp>
#include <agentsdk/acp/acp_types.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

DiagnosticsPanel::DiagnosticsPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(heading(QStringLiteral("Diagnostics")));
    auto *diagnostics = new QListWidget;
    diagnostics->setAccessibleName(QStringLiteral("Diagnostics list"));
    diagnostics->addItem(QStringLiteral("No diagnostics"));
    layout->addWidget(diagnostics);
}

struct ActivityPanel::AgentBackend {
    AgentBackend() { pool.setMaxThreadCount(4); }

    QThreadPool pool;
    std::atomic<bool> alive{true};
    std::atomic<unsigned> generation{0};
#ifdef ICE_HAVE_AGENTSDK
    std::vector<agentsdk::acp::agent_entry> agents;
    std::unique_ptr<agentsdk::acp::acp_client> client;
    // Owns initialize and routes notifications/permissions for every session
    // created through the shared client.
    std::unique_ptr<agentsdk::acp::acp_session> protocolSession;
#endif
};

namespace {

QString jsonString(const nlohmann::json &object, const char *key, const QString &fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return fallback;
    return QString::fromStdString(it->get<std::string>());
}

struct AgentToolEvent {
    QString id;
    QString title;
    QString kind;
    QString status;
    QString command;
    QString output;
    QString path;
};

QString truncatedToolOutput(const QString &text) {
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    auto lines = trimmed.split(QLatin1Char('\n'));
    constexpr int tailLines = 12;
    if (lines.size() <= tailLines + 1)
        return trimmed;
    const auto omitted = lines.size() - tailLines;
    return QStringLiteral("(%1 earlier lines)\n%2").arg(omitted).arg(lines.mid(omitted).join(QLatin1Char('\n')));
}

QString configOptionsFromResponse(const std::string &response) {
    try {
        const auto doc = nlohmann::json::parse(response);
        const auto it = doc.find("configOptions");
        if (it != doc.end() && it->is_array())
            return QString::fromStdString(it->dump());
    } catch (const std::exception &) {
    }
    return QStringLiteral("[]");
}

#ifdef ICE_HAVE_AGENTSDK
// ACP allows multiple session IDs over one initialized connection.  The
// acp_session wrapper only tracks one ID, so session operations use the shared
// client directly while acp_session remains the protocol/notification hub.
bool createAcpSession(agentsdk::acp::acp_client &client, const QString &cwd, std::string &sessionId,
                      QString &configOptionsJson, QString &error) {
    nlohmann::json params;
    params["cwd"] = cwd.toStdString();
    params["mcpServers"] = nlohmann::json::array();
    const auto result = client.send_request("session/new", params.dump());
    if (result.empty()) {
        error = QStringLiteral("The agent did not create an ACP session.");
        return false;
    }
    try {
        const auto doc = nlohmann::json::parse(result);
        const auto sessionIt = doc.find("sessionId");
        if (sessionIt == doc.end() || !sessionIt->is_string() || sessionIt->get<std::string>().empty()) {
            error = QStringLiteral("The agent returned an invalid ACP session.");
            return false;
        }
        sessionId = sessionIt->get<std::string>();
        configOptionsJson = configOptionsFromResponse(result);
        return true;
    } catch (const std::exception &) {
        error = QStringLiteral("The agent returned an invalid ACP session response.");
        return false;
    }
}
#endif

} // namespace

ActivityPanel::ActivityPanel(QWidget *parent) : QWidget(parent), agentBackend_(std::make_unique<AgentBackend>()) {
    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;

    auto *agent = new QWidget;
    auto *agentLayout = new QHBoxLayout(agent);
    transcript_ = new QPlainTextEdit;
    transcript_->setReadOnly(true);
    transcript_->setPlaceholderText(QStringLiteral("Agent conversation and tool-call updates"));

    auto *sessionPanel = new QFrame;
    sessionPanel->setFrameShape(QFrame::StyledPanel);
    sessionPanel->setFixedWidth(240);
    auto *panelLayout = new QVBoxLayout(sessionPanel);
    panelLayout->setContentsMargins(0, 0, 0, 8);
    panelLayout->setSpacing(4);

    agentStack_ = new QStackedWidget;

    auto *setupPage = new QWidget;
    auto *setupLayout = new QVBoxLayout(setupPage);
    setupLayout->setContentsMargins(8, 8, 8, 0);
    setupLayout->setSpacing(6);
    auto *agentRow = new QHBoxLayout;
    agentRow->setSpacing(6);
    auto *agentLabel = new QLabel(QStringLiteral("Agent"));
    agentLabel->setFixedWidth(40);
    agentRow->addWidget(agentLabel);
    agentSelector_ = new QComboBox;
    agentSelector_->setAccessibleName(QStringLiteral("ACP agent"));
    agentRow->addWidget(agentSelector_, 1);
    setupLayout->addLayout(agentRow);
    auto *setupButtons = new QHBoxLayout;
    setupButtons->setSpacing(6);
    agentConnectButton_ = new QPushButton(QStringLiteral("Connect"));
    agentSessionButton_ = new QPushButton(QStringLiteral("Session"));
    agentSessionButton_->setToolTip(QStringLiteral("Sessions, model, token usage, and MCP servers"));
    agentSessionButton_->setEnabled(false);
    setupButtons->addWidget(agentConnectButton_, 1);
    setupButtons->addWidget(agentSessionButton_, 1);
    setupLayout->addLayout(setupButtons);
    setupLayout->addStretch();
    agentStack_->addWidget(setupPage);

    auto *sessionPage = new QWidget;
    auto *sessionLayout = new QVBoxLayout(sessionPage);
    sessionLayout->setContentsMargins(8, 8, 8, 0);
    sessionLayout->setSpacing(6);
    auto *sessionRow = new QHBoxLayout;
    sessionRow->setSpacing(4);
    auto *sessionLabel = new QLabel(QStringLiteral("Session"));
    sessionLabel->setFixedWidth(40);
    sessionRow->addWidget(sessionLabel);
    sessionSelector_ = new QComboBox;
    sessionSelector_->setAccessibleName(QStringLiteral("ACP session"));
    sessionSelector_->view()->setMinimumWidth(320);
    sessionRow->addWidget(sessionSelector_, 1);
    newSessionButton_ = new QPushButton(QStringLiteral("+"));
    newSessionButton_->setAccessibleName(QStringLiteral("New session"));
    newSessionButton_->setToolTip(QStringLiteral("New session"));
    newSessionButton_->setFixedWidth(24);
    sessionRow->addWidget(newSessionButton_);
    closeSessionButton_ = new QPushButton(QStringLiteral("x"));
    closeSessionButton_->setAccessibleName(QStringLiteral("Close session"));
    closeSessionButton_->setToolTip(QStringLiteral("Close current session"));
    closeSessionButton_->setFixedWidth(24);
    sessionRow->addWidget(closeSessionButton_);
    sessionLayout->addLayout(sessionRow);
    auto *modelRow = new QHBoxLayout;
    modelRow->setSpacing(6);
    auto *modelLabel = new QLabel(QStringLiteral("Model"));
    modelLabel->setFixedWidth(40);
    modelRow->addWidget(modelLabel);
    modelSelector_ = new QComboBox;
    modelSelector_->setAccessibleName(QStringLiteral("Agent model"));
    modelSelector_->setEnabled(false);
    modelRow->addWidget(modelSelector_, 1);
    sessionLayout->addLayout(modelRow);
    tokens_ = new QLabel(QStringLiteral("0 / 0 tokens"));
    tokens_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    contextUsage_ = new QProgressBar;
    contextUsage_->setRange(0, 100);
    contextUsage_->setValue(0);
    contextUsage_->setFormat(QStringLiteral("%p%"));
    contextUsage_->setMaximumHeight(16);
    auto *usageRow = new QHBoxLayout;
    usageRow->setSpacing(6);
    usageRow->addWidget(tokens_);
    usageRow->addWidget(contextUsage_, 1);
    sessionLayout->addLayout(usageRow);
    auto *mcpLabel = new QLabel(QStringLiteral("MCP servers"));
    mcpLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    sessionLayout->addWidget(mcpLabel);
    mcpServers_ = new QListWidget;
    mcpServers_->setAccessibleName(QStringLiteral("MCP servers"));
    mcpServers_->setAlternatingRowColors(true);
    auto *noServers = new QListWidgetItem(QStringLiteral("None"), mcpServers_);
    noServers->setFlags(Qt::NoItemFlags);
    sessionLayout->addWidget(mcpServers_, 1);
    auto *sessionButtons = new QHBoxLayout;
    sessionButtons->setSpacing(6);
    agentBackButton_ = new QPushButton(QStringLiteral("Agent"));
    agentBackButton_->setToolTip(QStringLiteral("Change the connected agent"));
    agentDisconnectButton_ = new QPushButton(QStringLiteral("Disconnect"));
    sessionButtons->addWidget(agentBackButton_, 1);
    sessionButtons->addWidget(agentDisconnectButton_, 1);
    sessionLayout->addLayout(sessionButtons);
    agentStack_->addWidget(sessionPage);

    agentStatus_ = new QLabel;
    agentStatus_->setWordWrap(true);
    agentStatus_->setContentsMargins(8, 6, 8, 0);
    agentStatus_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    agentStatus_->hide();
    panelLayout->addWidget(agentStatus_);
    panelLayout->addWidget(agentStack_, 1);

    auto *conversationColumn = new QVBoxLayout;
    conversationColumn->addWidget(transcript_, 1);
    auto *promptRow = new QHBoxLayout;
    promptRow->setSpacing(6);
    prompt_ = new QLineEdit;
    prompt_->setPlaceholderText(QStringLiteral("Ask the agent about the current project..."));
    prompt_->setEnabled(false);
    promptRow->addWidget(prompt_, 1);
    sendButton_ = new QPushButton(QStringLiteral("Send"));
    sendButton_->setEnabled(false);
    promptRow->addWidget(sendButton_);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"));
    cancelButton_->setEnabled(false);
    promptRow->addWidget(cancelButton_);
    conversationColumn->addLayout(promptRow);
    agentLayout->addLayout(conversationColumn, 1);
    agentLayout->addWidget(sessionPanel, 0);
    agentTabIndex_ = tabs->addTab(agent, QStringLiteral("Agent"));

#ifdef ICE_HAVE_AGENTSDK
    agentsdk::acp::acp_agent_manager manager;
    agentBackend_->agents = manager.discover_agents();
    for (const auto &entry : agentBackend_->agents) {
        const auto label = entry.display_name.empty() ? QString::fromStdString(entry.name)
                                                      : QString::fromStdString(entry.display_name);
        agentSelector_->addItem(label, QString::fromStdString(entry.command));
        agentSelector_->setItemData(agentSelector_->count() - 1, QString::fromStdString(entry.command),
                                    Qt::ToolTipRole);
    }
    if (agentBackend_->agents.empty()) {
        agentSelector_->addItem(QStringLiteral("No ACP agents found"));
        agentSelector_->setEnabled(false);
        agentConnectButton_->setEnabled(false);
        setAgentStatus(QStringLiteral("Install an ACP-capable agent on PATH (for example opencode)."));
    }
#else
    agentSelector_->addItem(QStringLiteral("Agent support unavailable"));
    agentSelector_->setEnabled(false);
    agentConnectButton_->setEnabled(false);
    setAgentStatus(QStringLiteral("Agent support is unavailable. Initialize the bundled agentsdk-cpp submodule with "
                                  "`git submodule update --init --recursive` and rebuild."));
#endif

    connect(agentConnectButton_, &QPushButton::clicked, this, [this] {
        if (agentConnected_)
            disconnectAgent();
        else
            connectAgent();
    });
    connect(agentSessionButton_, &QPushButton::clicked, this, &ActivityPanel::showAgentSessionPage);
    connect(agentBackButton_, &QPushButton::clicked, this, &ActivityPanel::showAgentSetupPage);
    connect(agentDisconnectButton_, &QPushButton::clicked, this, &ActivityPanel::disconnectAgent);
    connect(sendButton_, &QPushButton::clicked, this, &ActivityPanel::sendPrompt);
    connect(cancelButton_, &QPushButton::clicked, this, &ActivityPanel::cancelPrompt);
    connect(prompt_, &QLineEdit::returnPressed, this, &ActivityPanel::sendPrompt);
    connect(sessionSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index) { switchAgentSession(index); });
    connect(newSessionButton_, &QPushButton::clicked, this, &ActivityPanel::newAgentSession);
    connect(closeSessionButton_, &QPushButton::clicked, this, &ActivityPanel::closeActiveAgentSession);
    connect(modelSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyModelSelection(); });
    updateAgentControls();

    messages_ = new QPlainTextEdit;
    messages_->setReadOnly(true);
    messages_->setPlaceholderText(QStringLiteral("Structured application and external-tool messages"));
    messagesTabIndex_ = tabs->addTab(messages_, QStringLiteral("Messages"));

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

ActivityPanel::~ActivityPanel() {
    if (!agentBackend_)
        return;
    agentBackend_->alive.store(false);
    agentBackend_->generation.fetch_add(1);
#ifdef ICE_HAVE_AGENTSDK
    if (agentBackend_->client)
        agentBackend_->client->terminate();
#endif
    agentBackend_->pool.waitForDone();
}

void ActivityPanel::setProjectPath(const QString &path) { projectPath_ = path; }

ActivityPanel::AgentSession *ActivityPanel::activeAgentSession() {
    if (activeAgentSession_ < 0 || activeAgentSession_ >= static_cast<int>(agentSessions_.size()))
        return nullptr;
    return &agentSessions_[static_cast<std::size_t>(activeAgentSession_)];
}

ActivityPanel::AgentSession *ActivityPanel::findAgentSession(const std::string &acpSessionId) {
    const auto index = findAgentSessionIndex(acpSessionId);
    return index < 0 ? nullptr : &agentSessions_[static_cast<std::size_t>(index)];
}

int ActivityPanel::findAgentSessionIndex(const std::string &acpSessionId) const {
    for (std::size_t index = 0; index < agentSessions_.size(); ++index) {
        if (agentSessions_[index].acpSessionId == acpSessionId)
            return static_cast<int>(index);
    }
    return -1;
}

void ActivityPanel::appendSessionTranscript(const std::string &acpSessionId, const QString &text) {
    const auto index = findAgentSessionIndex(acpSessionId);
    if (index < 0 || text.isEmpty())
        return;
    auto &session = agentSessions_[static_cast<std::size_t>(index)];
    session.transcript += text;
    if (index != activeAgentSession_ || !transcript_ || session.loading)
        return;
    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void ActivityPanel::flushPendingReads(AgentSession &session) {
    if (session.pendingReads <= 0)
        return;
    const auto count = session.pendingReads;
    session.pendingReads = 0;
    appendSessionTranscript(session.acpSessionId,
                            QStringLiteral("→Explored: %1 %2\n").arg(count).arg(count == 1 ? "read" : "reads"));
}

void ActivityPanel::handleToolCallUpdate(AgentSession &session, const std::string &toolCallId, const QString &title,
                                         const QString &kind, const QString &status, const QString &command,
                                         const QString &output, const QString &path) {
    if (toolCallId.empty())
        return;
    auto &call = session.toolCalls[toolCallId];
    if (!title.isEmpty())
        call.title = title;
    if (!kind.isEmpty())
        call.kind = kind;
    if (!command.isEmpty())
        call.command = command;
    if (!output.isEmpty())
        call.output = output;
    if (!path.isEmpty())
        call.path = path;

    if (status.isEmpty() || status == QStringLiteral("pending") || status == QStringLiteral("in_progress")) {
        if (call.kind != QStringLiteral("read"))
            flushPendingReads(session);
        return;
    }

    const bool failed = status == QStringLiteral("failed");
    if (!failed && call.kind == QStringLiteral("read")) {
        ++session.pendingReads;
        session.toolCalls.erase(toolCallId);
        return;
    }

    flushPendingReads(session);

    auto label = call.title;
    if (label.isEmpty() || label == call.kind)
        label = call.path;
    if (label.isEmpty())
        label = call.kind;
    if (failed) {
        appendSessionTranscript(
            session.acpSessionId,
            QStringLiteral("✗ %1 failed\n").arg(label.isEmpty() ? QStringLiteral("Tool call") : label));
        const auto detail = truncatedToolOutput(call.output);
        if (!detail.isEmpty())
            appendSessionTranscript(session.acpSessionId, detail + QLatin1Char('\n'));
        session.toolCalls.erase(toolCallId);
        return;
    }

    if (call.kind == QStringLiteral("execute")) {
        const auto shellCommand = !call.command.isEmpty() ? call.command : call.title;
        QString block = QStringLiteral("$ %1\n").arg(shellCommand.isEmpty() ? QStringLiteral("command") : shellCommand);
        const auto detail = truncatedToolOutput(call.output);
        if (!detail.isEmpty())
            block += detail + QLatin1Char('\n');
        appendSessionTranscript(session.acpSessionId, block);
    } else if (call.title == QStringLiteral("question") && !call.output.isEmpty()) {
        appendSessionTranscript(session.acpSessionId, QStringLiteral("→ %1\n").arg(truncatedToolOutput(call.output)));
    } else {
        appendSessionTranscript(session.acpSessionId,
                                QStringLiteral("→ %1\n").arg(label.isEmpty() ? QStringLiteral("Tool call") : label));
    }
    session.toolCalls.erase(toolCallId);
}

void ActivityPanel::appendTranscript(const QString &text) {
    if (auto *session = activeAgentSession()) {
        appendSessionTranscript(session->acpSessionId, text);
        return;
    }
    if (!transcript_ || text.isEmpty())
        return;
    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void ActivityPanel::showSessionTranscript() {
    if (!transcript_)
        return;
    const auto *session = activeAgentSession();
    transcript_->setPlainText(session ? session->transcript : QString{});
    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void ActivityPanel::refreshSessionSelector() {
    if (!sessionSelector_)
        return;
    const QSignalBlocker blocker(sessionSelector_);
    refreshingSessionSelector_ = true;
    sessionSelector_->clear();
    for (const auto &session : agentSessions_) {
        sessionSelector_->addItem(session.label, QString::fromStdString(session.acpSessionId));
        sessionSelector_->setItemData(sessionSelector_->count() - 1, QString::fromStdString(session.acpSessionId),
                                      Qt::ToolTipRole);
    }
    bool hasPast = false;
    for (const auto &summary : pastAgentSessions_) {
        if (findAgentSessionIndex(summary.acpSessionId) < 0) {
            hasPast = true;
            break;
        }
    }
    if (hasPast && !agentSessions_.empty())
        sessionSelector_->insertSeparator(sessionSelector_->count());
    for (const auto &summary : pastAgentSessions_) {
        if (findAgentSessionIndex(summary.acpSessionId) >= 0)
            continue;
        const auto title = summary.title.isEmpty() ? QStringLiteral("Past session") : summary.title;
        sessionSelector_->addItem(QStringLiteral("↺ %1").arg(title), QString::fromStdString(summary.acpSessionId));
        QString tooltip = title;
        if (!summary.updatedAt.isEmpty())
            tooltip += QStringLiteral("\nUpdated: %1").arg(summary.updatedAt);
        tooltip += QStringLiteral("\n%1").arg(QString::fromStdString(summary.acpSessionId));
        sessionSelector_->setItemData(sessionSelector_->count() - 1, tooltip, Qt::ToolTipRole);
    }
    if (activeAgentSession_ >= 0 && activeAgentSession_ < sessionSelector_->count())
        sessionSelector_->setCurrentIndex(activeAgentSession_);
    refreshingSessionSelector_ = false;
}

void ActivityPanel::switchAgentSession(int index) {
    if (refreshingSessionSelector_ || modelChanging_ || sessionLoading_ || index < 0 ||
        index >= sessionSelector_->count())
        return;
    const auto acpSessionId = sessionSelector_->itemData(index).toString().toStdString();
    if (acpSessionId.empty())
        return;
    if (auto *current = activeAgentSession())
        flushPendingReads(*current);
    const auto sessionIndex = findAgentSessionIndex(acpSessionId);
    if (sessionIndex < 0) {
        const auto previousActive = activeAgentSession_;
        loadAgentSession(acpSessionId);
        if (activeAgentSession_ == previousActive) {
            const QSignalBlocker blocker(sessionSelector_);
            sessionSelector_->setCurrentIndex(previousActive);
        }
        return;
    }
    activeAgentSession_ = sessionIndex;
    if (prompt_)
        prompt_->clear();
    showSessionTranscript();
    populateModelSelector();
    if (const auto *session = activeAgentSession())
        updateAgentUsage(session->used, session->size);
    updateAgentControls();
}

void ActivityPanel::addAgentSession(const std::string &acpSessionId, const QString &configOptionsJson,
                                    const QString &initialTranscript) {
    AgentSession session;
    session.acpSessionId = acpSessionId;
    session.label = QStringLiteral("Session %1").arg(nextAgentSessionNumber_++);
    session.transcript = initialTranscript;
    session.configOptionsJson = configOptionsJson.isEmpty() ? QStringLiteral("[]") : configOptionsJson;
    agentSessions_.push_back(std::move(session));
    activeAgentSession_ = static_cast<int>(agentSessions_.size()) - 1;
    if (prompt_)
        prompt_->clear();
    refreshSessionSelector();
    showSessionTranscript();
    populateModelSelector();
    updateAgentUsage(0, 0);
    updateAgentControls();
}

void ActivityPanel::removeAgentSession(const std::string &acpSessionId) {
    const auto index = findAgentSessionIndex(acpSessionId);
    if (index < 0)
        return;
    flushPendingReads(agentSessions_[static_cast<std::size_t>(index)]);
    const bool wasActive = index == activeAgentSession_;
    agentSessions_.erase(agentSessions_.begin() + index);
    if (agentSessions_.empty()) {
        activeAgentSession_ = -1;
    } else if (wasActive) {
        activeAgentSession_ = std::min(index, static_cast<int>(agentSessions_.size()) - 1);
    } else if (index < activeAgentSession_) {
        --activeAgentSession_;
    }
    if (prompt_)
        prompt_->clear();
    refreshSessionSelector();
    showSessionTranscript();
    populateModelSelector();
    if (const auto *session = activeAgentSession())
        updateAgentUsage(session->used, session->size);
    else
        updateAgentUsage(0, 0);
    updateAgentControls();
}

void ActivityPanel::setSessionPromptRunning(const std::string &acpSessionId, bool running) {
    auto *session = findAgentSession(acpSessionId);
    if (!session)
        return;
    session->promptRunning = running;
    if (session == activeAgentSession())
        updateAgentControls();
}

void ActivityPanel::updateAgentUsage(qint64 used, qint64 size) {
    if (!tokens_ || !contextUsage_)
        return;
    tokens_->setText(QStringLiteral("%1 / %2 tokens").arg(used).arg(size));
    const int percentage = size > 0 ? static_cast<int>(used * 100 / size) : 0;
    contextUsage_->setValue(std::clamp(percentage, 0, 100));
}

void ActivityPanel::setAgentStatus(const QString &text) {
    if (!agentStatus_)
        return;
    agentStatus_->setText(text);
    agentStatus_->setVisible(!text.isEmpty());
}

void ActivityPanel::populateModelSelector() {
    if (!modelSelector_)
        return;
    auto *session = activeAgentSession();
    if (session) {
        session->modelAvailable = false;
        session->modelConfigId.clear();
    }
    const QSignalBlocker blocker(modelSelector_);
    updatingModelSelector_ = true;
    modelSelector_->clear();
    if (!session) {
        modelSelector_->addItem(QStringLiteral("Agent default"));
        updatingModelSelector_ = false;
        updateAgentControls();
        return;
    }

    const nlohmann::json *model = nullptr;
    nlohmann::json configs;
    try {
        configs = nlohmann::json::parse(session->configOptionsJson.toStdString());
        if (configs.is_array()) {
            for (const auto &option : configs) {
                if (!option.is_object())
                    continue;
                const auto id = option.value("id", std::string{});
                const auto category = option.value("category", std::string{});
                if (id == "model" || category == "model") {
                    model = &option;
                    break;
                }
            }
        }
    } catch (const std::exception &) {
        configs = nlohmann::json::array();
    }

    if (!model) {
        modelSelector_->addItem(QStringLiteral("Agent default"));
        updatingModelSelector_ = false;
        updateAgentControls();
        return;
    }

    session->modelConfigId = QString::fromStdString(model->value("id", std::string{}));
    const auto optionsIt = model->find("options");
    if (optionsIt != model->end() && optionsIt->is_array() && !optionsIt->empty()) {
        for (const auto &value : *optionsIt) {
            if (!value.is_object())
                continue;
            const auto id = value.value("value", std::string{});
            const auto name = value.value("name", std::string{});
            const auto label = name.empty() ? QString::fromStdString(id) : QString::fromStdString(name);
            modelSelector_->addItem(label, QString::fromStdString(id));
            modelSelector_->setItemData(modelSelector_->count() - 1,
                                        QString::fromStdString(value.value("description", std::string{})),
                                        Qt::ToolTipRole);
        }
        const auto current =
            modelSelector_->findData(QString::fromStdString(model->value("currentValue", std::string{})));
        if (current >= 0)
            modelSelector_->setCurrentIndex(current);
        session->modelAvailable = true;
    } else {
        modelSelector_->addItem(QStringLiteral("Agent default"));
    }
    updatingModelSelector_ = false;
    updateAgentControls();
}

void ActivityPanel::applyModelSelection() {
    if (updatingModelSelector_ || !agentConnected_ || modelChanging_ || !modelSelector_)
        return;
    auto *session = activeAgentSession();
    if (!session || session->promptRunning || session->modelConfigId.isEmpty())
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_ || !agentBackend_->client)
        return;
    const auto value = modelSelector_->currentData().toString();
    if (value.isEmpty())
        return;
    const auto label = modelSelector_->currentText();
    const auto configId = session->modelConfigId;
    const auto acpSessionId = session->acpSessionId;
    const auto generation = agentBackend_->generation.load();
    auto *client = agentBackend_->client.get();
    modelChanging_ = true;
    updateAgentControls();
    agentBackend_->pool.start(QRunnable::create([this, client, acpSessionId, configId, value, label, generation] {
        nlohmann::json params;
        params["sessionId"] = acpSessionId;
        params["configId"] = configId.toStdString();
        params["value"] = value.toStdString();
        const auto result = client->send_request("session/set_config_option", params.dump());
        const bool ok = !result.empty();
        const auto optionsJson = ok ? configOptionsFromResponse(result) : QString{};
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, optionsJson, acpSessionId, label, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                modelChanging_ = false;
                auto *changedSession = findAgentSession(acpSessionId);
                if (ok && changedSession)
                    changedSession->configOptionsJson = optionsJson;
                if (changedSession == activeAgentSession())
                    populateModelSelector();
                if (ok) {
                    setAgentStatus(QStringLiteral("Connected to %1").arg(agentSelector_->currentText()));
                    appendSessionTranscript(acpSessionId, QStringLiteral("\n[model] %1\n").arg(label));
                } else {
                    setAgentStatus(QStringLiteral("Unable to change the model."));
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::newAgentSession() {
    if (!agentConnected_ || sessionCreating_ || sessionClosing_ || !agentBackend_)
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_->client)
        return;
    auto *client = agentBackend_->client.get();
    const auto cwd = projectPath_.isEmpty() ? QDir::currentPath() : projectPath_;
    const auto generation = agentBackend_->generation.load();
    sessionCreating_ = true;
    setAgentStatus(QStringLiteral("Creating session..."));
    updateAgentControls();
    agentBackend_->pool.start(QRunnable::create([this, client, cwd, generation] {
        std::string acpSessionId;
        QString configOptionsJson;
        QString error;
        const bool ok = createAcpSession(*client, cwd, acpSessionId, configOptionsJson, error);
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, error, acpSessionId, configOptionsJson, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                sessionCreating_ = false;
                if (ok) {
                    setAgentStatus(QStringLiteral("Connected to %1").arg(agentSelector_->currentText()));
                    addAgentSession(acpSessionId, configOptionsJson);
                    showAgentSessionPage();
                } else {
                    setAgentStatus(error);
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::closeActiveAgentSession() {
    auto *session = activeAgentSession();
    if (!session || agentSessions_.size() <= 1 || sessionClosing_ || session->promptRunning)
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_ || !agentBackend_->client)
        return;
    const auto acpSessionId = session->acpSessionId;
    const auto generation = agentBackend_->generation.load();
    auto *client = agentBackend_->client.get();
    sessionClosing_ = true;
    updateAgentControls();
    agentBackend_->pool.start(QRunnable::create([this, client, acpSessionId, generation] {
        nlohmann::json params;
        params["sessionId"] = acpSessionId;
        client->send_request("session/close", params.dump(), std::chrono::milliseconds{5000});
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, acpSessionId, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                sessionClosing_ = false;
                removeAgentSession(acpSessionId);
                refreshPastAgentSessions();
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::refreshPastAgentSessions() {
    if (!agentConnected_ || !loadSupported_ || sessionListLoading_ || !agentBackend_)
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_->client)
        return;
    auto *client = agentBackend_->client.get();
    const auto cwd = projectPath_.isEmpty() ? QDir::currentPath() : projectPath_;
    const auto generation = agentBackend_->generation.load();
    sessionListLoading_ = true;
    agentBackend_->pool.start(QRunnable::create([this, client, cwd, generation] {
        std::vector<AgentSessionSummary> summaries;
        bool ok = false;
        const auto normalizePath = [](const QString &path) {
            return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        };
        const auto normalizedCwd = normalizePath(cwd);
        std::string cursor;
        for (int page = 0; page < 8; ++page) {
            nlohmann::json params;
            params["cwd"] = cwd.toStdString();
            if (!cursor.empty())
                params["cursor"] = cursor;
            const auto result = client->send_request("session/list", params.dump(), std::chrono::milliseconds{5000});
            if (result.empty()) {
                if (page == 0)
                    ok = false;
                break;
            }
            std::string nextCursor;
            try {
                const auto doc = nlohmann::json::parse(result);
                const auto sessionsIt = doc.find("sessions");
                if (sessionsIt == doc.end() || !sessionsIt->is_array())
                    break;
                ok = true;
                for (const auto &entry : *sessionsIt) {
                    if (!entry.is_object())
                        continue;
                    const auto idIt = entry.find("sessionId");
                    if (idIt == entry.end() || !idIt->is_string())
                        continue;
                    const auto sessionId = idIt->get<std::string>();
                    if (sessionId.empty())
                        continue;
                    QString entryCwd;
                    const auto cwdIt = entry.find("cwd");
                    if (cwdIt != entry.end() && cwdIt->is_string())
                        entryCwd = QString::fromStdString(cwdIt->get<std::string>());
                    if (!entryCwd.isEmpty() && normalizePath(entryCwd) != normalizedCwd)
                        continue;
                    bool duplicate = false;
                    for (const auto &summary : summaries) {
                        if (summary.acpSessionId == sessionId) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (duplicate)
                        continue;
                    AgentSessionSummary summary;
                    summary.acpSessionId = sessionId;
                    const auto titleIt = entry.find("title");
                    if (titleIt != entry.end() && titleIt->is_string())
                        summary.title = QString::fromStdString(titleIt->get<std::string>());
                    const auto updatedIt = entry.find("updatedAt");
                    if (updatedIt != entry.end() && updatedIt->is_string())
                        summary.updatedAt = QString::fromStdString(updatedIt->get<std::string>());
                    summaries.push_back(std::move(summary));
                }
                const auto cursorIt = doc.find("nextCursor");
                if (cursorIt != doc.end() && cursorIt->is_string())
                    nextCursor = cursorIt->get<std::string>();
            } catch (const std::exception &) {
                break;
            }
            if (nextCursor.empty() || nextCursor == cursor)
                break;
            cursor = std::move(nextCursor);
        }
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, summaries = std::move(summaries), generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                sessionListLoading_ = false;
                if (ok) {
                    pastAgentSessions_ = summaries;
                    refreshSessionSelector();
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#else
    sessionListLoading_ = false;
#endif
}

void ActivityPanel::loadAgentSession(const std::string &acpSessionId) {
    if (!agentConnected_ || !loadSupported_ || sessionLoading_ || sessionCreating_ || sessionClosing_ ||
        acpSessionId.empty())
        return;
    if (findAgentSessionIndex(acpSessionId) >= 0)
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_ || !agentBackend_->client)
        return;
    QString title;
    for (const auto &summary : pastAgentSessions_) {
        if (summary.acpSessionId == acpSessionId) {
            title = summary.title;
            break;
        }
    }
    AgentSession session;
    session.acpSessionId = acpSessionId;
    session.label = title.isEmpty() ? QStringLiteral("Past session") : title;
    session.configOptionsJson = QStringLiteral("[]");
    session.loading = true;
    agentSessions_.push_back(std::move(session));
    activeAgentSession_ = static_cast<int>(agentSessions_.size()) - 1;
    sessionLoading_ = true;
    if (prompt_)
        prompt_->clear();
    refreshSessionSelector();
    showSessionTranscript();
    populateModelSelector();
    updateAgentUsage(0, 0);
    setAgentStatus(QStringLiteral("Loading session..."));
    updateAgentControls();

    auto *client = agentBackend_->client.get();
    const auto cwd = projectPath_.isEmpty() ? QDir::currentPath() : projectPath_;
    const auto generation = agentBackend_->generation.load();
    agentBackend_->pool.start(QRunnable::create([this, client, acpSessionId, cwd, generation] {
        nlohmann::json params;
        params["sessionId"] = acpSessionId;
        params["cwd"] = cwd.toStdString();
        params["mcpServers"] = nlohmann::json::array();
        const auto result = client->send_request("session/load", params.dump(), std::chrono::milliseconds{30000});
        const bool ok = !result.empty();
        const auto configOptionsJson = ok ? configOptionsFromResponse(result) : QString{};
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, configOptionsJson, acpSessionId, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                sessionLoading_ = false;
                auto *session = findAgentSession(acpSessionId);
                if (!session) {
                    updateAgentControls();
                    return;
                }
                session->loading = false;
                if (ok) {
                    session->configOptionsJson = configOptionsJson;
                    if (session == activeAgentSession()) {
                        populateModelSelector();
                        if (session->transcript.isEmpty())
                            appendSessionTranscript(acpSessionId, QStringLiteral("[Session loaded]\n"));
                        showSessionTranscript();
                    }
                    setAgentStatus(QStringLiteral("Connected to %1").arg(agentSelector_->currentText()));
                } else {
                    setAgentStatus(QStringLiteral("Unable to load the past session."));
                    removeAgentSession(acpSessionId);
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::updateAgentControls() {
    if (!agentConnectButton_ || !agentSelector_ || !prompt_ || !sendButton_ || !cancelButton_ || !sessionSelector_ ||
        !newSessionButton_ || !closeSessionButton_)
        return;
    const bool hasAgent =
        agentSelector_->currentData().isValid() && !agentSelector_->currentData().toString().isEmpty();
    const auto *session = activeAgentSession();
    const bool hasSession = agentConnected_ && session;
    agentConnectButton_->setText(agentConnected_ ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    agentConnectButton_->setEnabled(!agentConnecting_ && (agentConnected_ || hasAgent));
    agentSessionButton_->setEnabled(agentConnected_ && !agentSessions_.empty() && !sessionCreating_);
    agentDisconnectButton_->setEnabled(agentConnected_ && !agentConnecting_);
    newSessionButton_->setEnabled(agentConnected_ && !sessionCreating_ && !sessionClosing_ && !sessionLoading_ &&
                                  !modelChanging_);
    closeSessionButton_->setEnabled(hasSession && agentSessions_.size() > 1 && !session->promptRunning &&
                                    !session->loading && !sessionCreating_ && !sessionClosing_ && !sessionLoading_ &&
                                    !modelChanging_);
    sessionSelector_->setEnabled(agentConnected_ && !agentSessions_.empty() && !sessionCreating_ && !sessionClosing_ &&
                                 !sessionLoading_ && !modelChanging_);
    agentSelector_->setEnabled(!agentConnecting_ && !agentConnected_ && hasAgent);
    prompt_->setEnabled(hasSession && !session->promptRunning && !session->loading && !sessionLoading_ &&
                        !modelChanging_);
    sendButton_->setEnabled(hasSession && !session->promptRunning && !session->loading && !sessionLoading_ &&
                            !modelChanging_);
    cancelButton_->setEnabled(hasSession && session->promptRunning && !session->loading && !sessionLoading_);
    if (modelSelector_)
        modelSelector_->setEnabled(hasSession && session->modelAvailable && !session->promptRunning &&
                                   !session->loading && !sessionLoading_ && !modelChanging_);
}

void ActivityPanel::showAgentSetupPage() {
    if (agentStack_)
        agentStack_->setCurrentIndex(0);
}

void ActivityPanel::showAgentSessionPage() {
    if (agentStack_ && agentConnected_ && activeAgentSession()) {
        agentStack_->setCurrentIndex(1);
        refreshPastAgentSessions();
    }
}

void ActivityPanel::connectAgent() {
    if (!agentBackend_ || agentConnected_ || agentConnecting_)
        return;
#ifdef ICE_HAVE_AGENTSDK
    const int index = agentSelector_ ? agentSelector_->currentIndex() : -1;
    if (index < 0 || index >= static_cast<int>(agentBackend_->agents.size()))
        return;
    const auto entry = agentBackend_->agents[static_cast<std::size_t>(index)];

    if (agentBackend_->client)
        agentBackend_->client->terminate();
    agentBackend_->pool.waitForDone();
    agentBackend_->client = std::make_unique<agentsdk::acp::acp_client>();
    agentBackend_->protocolSession = std::make_unique<agentsdk::acp::acp_session>(*agentBackend_->client);

    auto *client = agentBackend_->client.get();
    auto *protocolSession = agentBackend_->protocolSession.get();
    protocolSession->set_update_handler([this](const std::string &method, const std::string &params) {
        if (method != "session/update")
            return;
        try {
            const auto root = nlohmann::json::parse(params);
            const auto id = root.find("sessionId");
            if (id != root.end() && id->is_string())
                handleAgentUpdate(id->get<std::string>(), params);
        } catch (const std::exception &) {
        }
    });
    protocolSession->set_agent_request_handler(
        [this](const std::string &method, const std::string &params) -> agentsdk::acp::agent_response {
            agentsdk::acp::agent_response response;
            if (method == "session/request_permission") {
                response.handled = true;
                response.result_json = handleAgentPermission(params);
                return response;
            }
            if (method == "elicitation/create") {
                response.handled = true;
                response.result_json = handleAgentElicitation(params);
                return response;
            }
            return response;
        });

    const auto cwd = projectPath_.isEmpty() ? QDir::currentPath() : projectPath_;
    const auto generation = agentBackend_->generation.fetch_add(1) + 1;
    agentConnecting_ = true;
    agentSessions_.clear();
    pastAgentSessions_.clear();
    activeAgentSession_ = -1;
    nextAgentSessionNumber_ = 1;
    loadSupported_ = false;
    sessionLoading_ = false;
    sessionListLoading_ = false;
    refreshSessionSelector();
    showSessionTranscript();
    setAgentStatus(QStringLiteral("Launching %1...").arg(QString::fromStdString(entry.display_name)));
    appendTranscript(QStringLiteral("\nConnecting to %1...\n").arg(QString::fromStdString(entry.display_name)));
    updateAgentControls();

    agentBackend_->pool.start(QRunnable::create([this, client, protocolSession, entry, cwd, generation] {
        QString error;
        bool ok = client->launch_agent(entry.command, entry.args);
        bool canLoadSessions = false;
        if (!ok)
            error = QStringLiteral("Unable to launch %1.").arg(QString::fromStdString(entry.display_name));
        if (ok) {
            agentsdk::acp::client_capabilities capabilities;
            capabilities.fs.read_text_file = true;
            capabilities.fs.write_text_file = true;
            capabilities.terminal = false;
            capabilities.elicitation.form = true;
            agentsdk::acp::implementation_info info;
            info.name = "ice";
            info.title = "ICE";
            info.version = ice::version();
            ok = protocolSession->initialize(capabilities, info);
            if (!ok)
                error = QStringLiteral("ACP initialize failed.");
            else
                canLoadSessions = protocolSession->agent_caps().load_session;
        }
        std::string sessionId;
        QString configOptionsJson;
        if (ok)
            ok = createAcpSession(*client, cwd, sessionId, configOptionsJson, error);
        if (!ok)
            client->terminate();
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, error, entry, sessionId, configOptionsJson, canLoadSessions, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                agentConnecting_ = false;
                agentConnected_ = ok;
                loadSupported_ = canLoadSessions;
                if (ok) {
                    setAgentStatus(QStringLiteral("Connected to %1").arg(QString::fromStdString(entry.display_name)));
                    agentStatus_->setToolTip({});
                    addAgentSession(sessionId, configOptionsJson);
                    refreshPastAgentSessions();
                } else {
                    setAgentStatus(error);
                    agentStatus_->setToolTip({});
                    appendTranscript(error + QStringLiteral("\n"));
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#else
    setAgentStatus(QStringLiteral("Agent support is unavailable in this build. Initialize the agentsdk-cpp submodule "
                                  "and rebuild."));
#endif
}

void ActivityPanel::disconnectAgent() {
    if (!agentBackend_)
        return;
    agentBackend_->generation.fetch_add(1);
    agentConnected_ = false;
    agentConnecting_ = false;
    sessionCreating_ = false;
    sessionClosing_ = false;
    sessionLoading_ = false;
    sessionListLoading_ = false;
    loadSupported_ = false;
    modelChanging_ = false;
#ifdef ICE_HAVE_AGENTSDK
    if (agentBackend_->client)
        agentBackend_->client->terminate();
    agentBackend_->pool.waitForDone();
    agentBackend_->client.reset();
    agentBackend_->protocolSession.reset();
#endif
    if (agentStatus_) {
        setAgentStatus({});
        agentStatus_->setToolTip({});
    }
    agentSessions_.clear();
    pastAgentSessions_.clear();
    activeAgentSession_ = -1;
    nextAgentSessionNumber_ = 1;
    refreshSessionSelector();
    showSessionTranscript();
    populateModelSelector();
    updateAgentUsage(0, 0);
    showAgentSetupPage();
    updateAgentControls();
}

void ActivityPanel::sendPrompt() {
    if (!agentBackend_ || !agentConnected_ || !prompt_ || prompt_->text().trimmed().isEmpty())
        return;
    auto *session = activeAgentSession();
    if (!session || session->promptRunning)
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_->client)
        return;
    const auto text = prompt_->text().trimmed();
    const auto acpSessionId = session->acpSessionId;
    prompt_->clear();
    appendSessionTranscript(acpSessionId, QStringLiteral("\nYou: %1\n").arg(text));
    setSessionPromptRunning(acpSessionId, true);
    const auto generation = agentBackend_->generation.load();
    auto *client = agentBackend_->client.get();
    agentBackend_->pool.start(QRunnable::create([this, client, acpSessionId, text, generation] {
        nlohmann::json params;
        params["sessionId"] = acpSessionId;
        params["prompt"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text.toStdString()}}});
        const bool ok = !client->send_request("session/prompt", params.dump(), std::chrono::milliseconds{0}).empty();
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, acpSessionId, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                auto *session = findAgentSession(acpSessionId);
                if (!session)
                    return;
                session->promptRunning = false;
                flushPendingReads(*session);
                if (!ok)
                    appendSessionTranscript(acpSessionId, QStringLiteral("\n[Turn failed or cancelled]\n"));
                if (session == activeAgentSession())
                    updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::cancelPrompt() {
#ifdef ICE_HAVE_AGENTSDK
    auto *session = activeAgentSession();
    if (agentBackend_ && agentBackend_->client && session) {
        flushPendingReads(*session);
        appendSessionTranscript(session->acpSessionId, QStringLiteral("\nCancelling current turn...\n"));
        nlohmann::json params;
        params["sessionId"] = session->acpSessionId;
        agentBackend_->client->send_notification("session/cancel", params.dump());
    }
#endif
}

void ActivityPanel::handleAgentUpdate(const std::string &acpSessionId, const std::string &params) {
    QString text;
    QString messageId;
    bool userMessage = false;
    std::optional<AgentToolEvent> toolEvent;
    std::optional<qint64> used;
    std::optional<qint64> size;
    try {
        const auto root = nlohmann::json::parse(params);
        const auto updateIt = root.find("update");
        if (updateIt == root.end() || !updateIt->is_object())
            return;
        const auto &update = *updateIt;
        const auto kind = jsonString(update, "sessionUpdate");
        if (kind == QStringLiteral("user_message_chunk")) {
            const auto contentIt = update.find("content");
            if (contentIt != update.end() && contentIt->is_object() &&
                jsonString(*contentIt, "type") == QStringLiteral("text")) {
                text = jsonString(*contentIt, "text");
                messageId = jsonString(update, "messageId");
                userMessage = true;
            }
        } else if (kind == QStringLiteral("agent_message_chunk") || kind == QStringLiteral("agent_thought_chunk")) {
            const auto contentIt = update.find("content");
            if (contentIt != update.end() && contentIt->is_object() &&
                jsonString(*contentIt, "type") == QStringLiteral("text"))
                text = jsonString(*contentIt, "text");
        } else if (kind == QStringLiteral("session_info_update") ||
                   kind == QStringLiteral("available_commands_update") ||
                   kind == QStringLiteral("current_mode_update") || kind == QStringLiteral("config_option_update")) {
            return;
        } else if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
            AgentToolEvent event;
            event.id = jsonString(update, "toolCallId");
            event.title = jsonString(update, "title");
            event.kind = jsonString(update, "kind");
            event.status = jsonString(update, "status");
            if (const auto inputIt = update.find("rawInput"); inputIt != update.end() && inputIt->is_object())
                event.command = jsonString(*inputIt, "command");
            if (const auto locationsIt = update.find("locations");
                locationsIt != update.end() && locationsIt->is_array() && !locationsIt->empty()) {
                const auto &location = locationsIt->front();
                if (location.is_object())
                    event.path = jsonString(location, "path");
            }
            if (const auto contentIt = update.find("content"); contentIt != update.end() && contentIt->is_array()) {
                for (const auto &item : *contentIt) {
                    if (!item.is_object())
                        continue;
                    const auto itemType = jsonString(item, "type");
                    if (itemType == QStringLiteral("content") || itemType == QStringLiteral("text")) {
                        const auto inner =
                            itemType == QStringLiteral("content") ? item.find("content") : item.find("text");
                        QString chunk;
                        if (itemType == QStringLiteral("text") && inner != item.end() && inner->is_string())
                            chunk = QString::fromStdString(inner->get<std::string>());
                        else if (inner != item.end() && inner->is_object() &&
                                 jsonString(*inner, "type") == QStringLiteral("text"))
                            chunk = jsonString(*inner, "text");
                        if (!chunk.isEmpty()) {
                            if (!event.output.isEmpty())
                                event.output += QLatin1Char('\n');
                            event.output += chunk;
                        }
                    } else if (itemType == QStringLiteral("diff")) {
                        if (event.path.isEmpty())
                            event.path = jsonString(item, "path");
                    }
                }
            }
            toolEvent = std::move(event);
        } else if (kind == QStringLiteral("plan")) {
            text = QStringLiteral("\n[plan]\n");
            const auto entriesIt = update.find("entries");
            if (entriesIt != update.end() && entriesIt->is_array()) {
                for (const auto &entry : *entriesIt) {
                    if (!entry.is_object())
                        continue;
                    text += QStringLiteral("- %1").arg(jsonString(entry, "content"));
                    const auto status = jsonString(entry, "status");
                    if (!status.isEmpty())
                        text += QStringLiteral(" (%1)").arg(status);
                    text += QLatin1Char('\n');
                }
            }
        } else if (kind == QStringLiteral("usage_update")) {
            used = update.value("used", qint64{0});
            size = update.value("size", qint64{0});
        } else if (!kind.isEmpty()) {
            text = QStringLiteral("\n[%1]\n").arg(kind);
        }
    } catch (const std::exception &) {
        return;
    }
    if (text.isEmpty() && !toolEvent && !used && !size)
        return;
    QMetaObject::invokeMethod(
        this,
        [this, acpSessionId, text, messageId, userMessage, toolEvent, used, size] {
            if (!agentBackend_ || !agentBackend_->alive.load())
                return;
            auto *session = findAgentSession(acpSessionId);
            if (!session)
                return;
            if (toolEvent) {
                handleToolCallUpdate(*session, toolEvent->id.toStdString(), toolEvent->title, toolEvent->kind,
                                     toolEvent->status, toolEvent->command, toolEvent->output, toolEvent->path);
            }
            if (!text.isEmpty()) {
                flushPendingReads(*session);
                auto formatted = text;
                if (userMessage && (messageId.isEmpty() || session->lastUserMessageId != messageId)) {
                    formatted = QStringLiteral("\nYou: %1\n").arg(text);
                    session->lastUserMessageId = messageId;
                }
                appendSessionTranscript(acpSessionId, formatted);
            }
            if (used && size) {
                session->used = *used;
                session->size = *size;
                if (session == activeAgentSession())
                    updateAgentUsage(*used, *size);
            }
        },
        Qt::QueuedConnection);
}

std::string ActivityPanel::handleAgentElicitation(const std::string &params) {
    auto promise = std::make_shared<std::promise<QString>>();
    auto future = promise->get_future();
    QPointer<ActivityPanel> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, promise, params] {
            const auto decline = [&] { promise->set_value(QStringLiteral("{\"action\":\"decline\"}")); };
            if (!self) {
                decline();
                return;
            }
            nlohmann::json root;
            try {
                root = nlohmann::json::parse(params);
            } catch (const std::exception &) {
                decline();
                return;
            }
            const auto schemaIt = root.find("requestedSchema");
            if (schemaIt == root.end() || !schemaIt->is_object()) {
                decline();
                return;
            }
            const auto propertiesIt = schemaIt->find("properties");
            if (propertiesIt == schemaIt->end() || !propertiesIt->is_object()) {
                decline();
                return;
            }
            std::set<std::string> required;
            if (const auto requiredIt = schemaIt->find("required");
                requiredIt != schemaIt->end() && requiredIt->is_array()) {
                for (const auto &entry : *requiredIt) {
                    if (entry.is_string())
                        required.insert(entry.get<std::string>());
                }
            }

            struct Option {
                nlohmann::json value;
                QString label;
                QString description;
            };
            const auto optionsFor = [](const nlohmann::json &schema) {
                std::vector<Option> options;
                if (const auto oneOf = schema.find("oneOf"); oneOf != schema.end() && oneOf->is_array()) {
                    for (const auto &item : *oneOf) {
                        if (!item.is_object())
                            continue;
                        Option option;
                        option.value =
                            item.contains("const") ? item.at("const") : item.value("title", nlohmann::json{});
                        const auto title = item.value("title", std::string{});
                        option.label = QString::fromStdString(
                            title.empty() && option.value.is_string() ? option.value.get<std::string>() : title);
                        option.description = QString::fromStdString(item.value("description", std::string{}));
                        options.push_back(std::move(option));
                    }
                } else if (const auto enumIt = schema.find("enum"); enumIt != schema.end() && enumIt->is_array()) {
                    for (const auto &item : *enumIt) {
                        Option option;
                        option.value = item;
                        option.label = QString::fromStdString(item.is_string() ? item.get<std::string>() : item.dump());
                        options.push_back(std::move(option));
                    }
                }
                return options;
            };

            struct Binding {
                std::string key;
                bool required = false;
                std::function<nlohmann::json()> read;
            };
            std::vector<Binding> bindings;

            QDialog dialog(self.data());
            dialog.setWindowTitle(QStringLiteral("Agent Question"));
            auto *layout = new QVBoxLayout(&dialog);
            const auto message = QString::fromStdString(root.value("message", std::string{}));
            if (!message.isEmpty()) {
                auto *messageLabel = new QLabel(message);
                auto font = messageLabel->font();
                font.setBold(true);
                messageLabel->setFont(font);
                messageLabel->setWordWrap(true);
                layout->addWidget(messageLabel);
            }

            auto *scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            auto *formWidget = new QWidget;
            auto *form = new QFormLayout(formWidget);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

            const auto addField = [&](const std::string &key, const nlohmann::json &property, QWidget *field,
                                      std::function<nlohmann::json()> read) {
                auto *box = new QWidget;
                auto *boxLayout = new QVBoxLayout(box);
                boxLayout->setContentsMargins(0, 0, 0, 0);
                boxLayout->setSpacing(2);
                auto *title = new QLabel(QString::fromStdString(property.value("title", key)));
                auto titleFont = title->font();
                titleFont.setBold(true);
                title->setFont(titleFont);
                title->setWordWrap(true);
                boxLayout->addWidget(title);
                const auto description = QString::fromStdString(property.value("description", std::string{}));
                if (!description.isEmpty()) {
                    auto *hint = new QLabel(description);
                    hint->setWordWrap(true);
                    hint->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
                    boxLayout->addWidget(hint);
                }
                boxLayout->addWidget(field);
                form->addRow(box);
                bindings.push_back({key, required.contains(key), std::move(read)});
            };

            for (auto it = propertiesIt->begin(); it != propertiesIt->end(); ++it) {
                if (!it->is_object())
                    continue;
                const auto &key = it.key();
                const auto &property = *it;
                const auto type = property.value("type", std::string{"string"});
                const auto options = optionsFor(property);
                if (type == "array") {
                    auto *list = new QListWidget;
                    list->setSelectionMode(QAbstractItemView::NoSelection);
                    list->setMinimumHeight(110);
                    const auto itemSchema = property.contains("items") && property.at("items").is_object()
                                                ? property.at("items")
                                                : nlohmann::json::object();
                    for (const auto &option : optionsFor(itemSchema)) {
                        auto *item = new QListWidgetItem(option.label, list);
                        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                        item->setCheckState(Qt::Unchecked);
                        item->setData(Qt::UserRole, QString::fromStdString(option.value.dump()));
                        if (!option.description.isEmpty())
                            item->setToolTip(option.description);
                    }
                    addField(key, property, list, [list] {
                        nlohmann::json values = nlohmann::json::array();
                        for (int index = 0; index < list->count(); ++index) {
                            const auto *item = list->item(index);
                            if (item->checkState() != Qt::Checked)
                                continue;
                            try {
                                values.push_back(
                                    nlohmann::json::parse(item->data(Qt::UserRole).toString().toStdString()));
                            } catch (const std::exception &) {
                            }
                        }
                        return values.empty() ? nlohmann::json{} : values;
                    });
                    continue;
                }
                if (type == "boolean") {
                    auto *check = new QCheckBox;
                    addField(key, property, check, [check] { return nlohmann::json(check->isChecked()); });
                    continue;
                }
                if (type == "integer") {
                    auto *spin = new QSpinBox;
                    spin->setRange(property.value("minimum", -1000000000), property.value("maximum", 1000000000));
                    addField(key, property, spin, [spin] { return nlohmann::json(spin->value()); });
                    continue;
                }
                if (type == "number") {
                    auto *spin = new QDoubleSpinBox;
                    spin->setDecimals(6);
                    spin->setRange(property.value("minimum", -1.0e12), property.value("maximum", 1.0e12));
                    addField(key, property, spin, [spin] { return nlohmann::json(spin->value()); });
                    continue;
                }
                if (!options.empty()) {
                    auto *combo = new QComboBox;
                    combo->addItem(QStringLiteral("Select…"), QString{});
                    for (const auto &option : options) {
                        combo->addItem(option.label, QString::fromStdString(option.value.dump()));
                        if (!option.description.isEmpty())
                            combo->setItemData(combo->count() - 1, option.description, Qt::ToolTipRole);
                    }
                    addField(key, property, combo, [combo] {
                        const auto data = combo->currentData().toString();
                        if (data.isEmpty())
                            return nlohmann::json{};
                        try {
                            return nlohmann::json::parse(data.toStdString());
                        } catch (const std::exception &) {
                            return nlohmann::json{};
                        }
                    });
                    continue;
                }
                auto *edit = new QLineEdit;
                addField(key, property, edit, [edit] {
                    const auto text = edit->text().trimmed();
                    return text.isEmpty() ? nlohmann::json{} : nlohmann::json(text.toStdString());
                });
            }

            scroll->setWidget(formWidget);
            layout->addWidget(scroll, 1);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Answer"));
            buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Decline"));
            layout->addWidget(buttons);
            bool accepted = false;
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
                for (const auto &binding : bindings) {
                    if (binding.required && binding.read().is_null()) {
                        QMessageBox::warning(&dialog, QStringLiteral("Agent Question"),
                                             QStringLiteral("Please answer all required questions."));
                        return;
                    }
                }
                accepted = true;
                dialog.accept();
            });
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            dialog.resize(620, 460);
            if (dialog.exec() != QDialog::Accepted || !accepted) {
                decline();
                return;
            }
            nlohmann::json content = nlohmann::json::object();
            for (const auto &binding : bindings) {
                auto value = binding.read();
                if (!value.is_null())
                    content[binding.key] = std::move(value);
            }
            promise->set_value(
                QString::fromStdString(nlohmann::json{{"action", "accept"}, {"content", std::move(content)}}.dump()));
        },
        Qt::QueuedConnection);

    while (agentBackend_ && agentBackend_->alive.load()) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
            return future.get().toStdString();
    }
    return R"({"action":"cancel"})";
}

std::string ActivityPanel::handleAgentPermission(const std::string &params) {
    QString title = QStringLiteral("Tool call");
    std::vector<std::tuple<QString, QString, QString>> options;
    try {
        const auto root = nlohmann::json::parse(params);
        const auto toolIt = root.find("toolCall");
        if (toolIt != root.end() && toolIt->is_object())
            title = jsonString(*toolIt, "title", title);
        const auto optionsIt = root.find("options");
        if (optionsIt != root.end() && optionsIt->is_array()) {
            for (const auto &option : *optionsIt) {
                if (!option.is_object())
                    continue;
                options.emplace_back(jsonString(option, "name", QStringLiteral("Allow")),
                                     jsonString(option, "optionId"), jsonString(option, "kind"));
            }
        }
    } catch (const std::exception &) {
        options.clear();
    }

    auto promise = std::make_shared<std::promise<QString>>();
    auto future = promise->get_future();
    QPointer<ActivityPanel> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, promise, title, options] {
            if (!self) {
                promise->set_value({});
                return;
            }
            QMessageBox box(self);
            box.setWindowTitle(QStringLiteral("Agent Permission"));
            box.setIcon(QMessageBox::Question);
            box.setText(QStringLiteral("The agent requests permission:\n\n%1").arg(title));
            box.setInformativeText(QStringLiteral("Choose an option for this tool call."));
            QHash<QAbstractButton *, QString> ids;
            for (const auto &[name, id, kind] : options) {
                const auto role =
                    kind.contains(QStringLiteral("reject")) ? QMessageBox::RejectRole : QMessageBox::AcceptRole;
                auto *button = box.addButton(name, role);
                ids.insert(button, id);
            }
            if (options.empty())
                box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
            box.exec();
            const auto *clicked = box.clickedButton();
            promise->set_value(clicked && ids.contains(const_cast<QAbstractButton *>(clicked))
                                   ? ids.value(const_cast<QAbstractButton *>(clicked))
                                   : QString{});
        },
        Qt::QueuedConnection);

    while (agentBackend_ && agentBackend_->alive.load()) {
        if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
            const auto optionId = future.get();
            nlohmann::json response;
            if (optionId.isEmpty()) {
                response["outcome"]["outcome"] = "cancelled";
            } else {
                response["outcome"]["outcome"] = "selected";
                response["outcome"]["optionId"] = optionId.toStdString();
            }
            return response.dump();
        }
    }
    return R"({"outcome":{"outcome":"cancelled"}})";
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

void ActivityPanel::showAgentTab() {
    if (tabs_ && agentTabIndex_ >= 0)
        tabs_->setCurrentIndex(agentTabIndex_);
}

void ActivityPanel::appendMessage(const QString &message) {
    if (!messages_)
        return;
    messages_->appendPlainText(message);
    messages_->moveCursor(QTextCursor::End);
}

void ActivityPanel::showMessagesTab() {
    if (tabs_ && messagesTabIndex_ >= 0)
        tabs_->setCurrentIndex(messagesTabIndex_);
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

QString defaultProjectFolder() {
    const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return documents.isEmpty() ? QDir::homePath() : documents;
}
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
    location_ = new QLineEdit(defaultProjectFolder());
    auto *browseButton = new QPushButton(QStringLiteral("…"));
    browseButton->setFixedWidth(32);
    browseButton->setToolTip(QStringLiteral("Choose project folder"));
    auto *locationRow = new QWidget;
    auto *locationLayout = new QHBoxLayout(locationRow);
    locationLayout->setContentsMargins(0, 0, 0, 0);
    locationLayout->addWidget(location_, 1);
    locationLayout->addWidget(browseButton);
    createFolder_ = new QCheckBox(QStringLiteral("Create project folder"));
    createFolder_->setChecked(true);
    form->addRow(QStringLiteral("Project name"), name_);
    form->addRow(QStringLiteral("Project folder"), locationRow);
    form->addRow(QString(), createFolder_);
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
            onCreateProject_(name_->text().trimmed(), location_->text().trimmed(), createFolder_->isChecked());
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

void StartupView::setOnRemoveRecent(RemoveRecentCallback callback) { onRemoveRecent_ = std::move(callback); }

void StartupView::setRecentProjects(const QStringList &paths) {
    recent_->clear();
    if (paths.isEmpty()) {
        auto *item = new QListWidgetItem(QStringLiteral("No recent projects"), recent_);
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const auto &path : paths) {
        const QFileInfo info(path);
        const auto label = info.fileName() == QStringLiteral("project.icepro") ? info.dir().dirName() : info.fileName();
        auto *item = new QListWidgetItem(recent_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);

        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 0, 4, 0);
        rowLayout->setSpacing(4);
        auto *openButton = new QPushButton(label);
        openButton->setFlat(true);
        openButton->setCursor(Qt::PointingHandCursor);
        openButton->setStyleSheet(
            QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; padding: 2px 0; }"
                           "QPushButton:hover { text-decoration: underline; }"));
        openButton->setToolTip(path);
        rowLayout->addWidget(openButton, 1);
        auto *removeButton = new QPushButton;
        removeButton->setFlat(true);
        removeButton->setFixedWidth(24);
        removeButton->setToolTip(QStringLiteral("Remove from recent projects"));
        removeButton->setAccessibleName(QStringLiteral("Remove %1 from recent projects").arg(label));
        setLucideIcon(removeButton, QStringLiteral("trash"), 14);
        rowLayout->addWidget(removeButton);
        item->setSizeHint(row->sizeHint());
        recent_->setItemWidget(item, row);
        connect(openButton, &QPushButton::clicked, this, [this, path] {
            if (onOpenRecent_)
                onOpenRecent_(path);
        });
        connect(removeButton, &QPushButton::clicked, this, [this, path] {
            // The callback rebuilds the list, which deletes this button. Defer
            // it until the click signal has fully returned.
            QTimer::singleShot(0, this, [this, path] {
                if (onRemoveRecent_)
                    onRemoveRecent_(path);
            });
        });
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

CreateProjectDialog::CreateProjectDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Project"));
    setModal(true);
    resize(560, 230);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    name_ = new QLineEdit(QStringLiteral("untitled"));
    name_->setClearButtonEnabled(true);
    folder_ = new QLineEdit(defaultProjectFolder());
    auto *browse = new QPushButton(QStringLiteral("…"));
    browse->setFixedWidth(32);
    browse->setToolTip(QStringLiteral("Choose project folder"));
    auto *folderRow = new QWidget;
    auto *folderLayout = new QHBoxLayout(folderRow);
    folderLayout->setContentsMargins(0, 0, 0, 0);
    folderLayout->addWidget(folder_, 1);
    folderLayout->addWidget(browse);
    createFolder_ = new QCheckBox(QStringLiteral("Create project folder"));
    createFolder_->setChecked(true);
    form->addRow(QStringLiteral("Project name"), name_);
    form->addRow(QStringLiteral("Project folder"), folderRow);
    form->addRow(QString(), createFolder_);
    layout->addLayout(form);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    layout->addWidget(buttons);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto directory =
            QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Project Folder"), folder_->text());
        if (!directory.isEmpty())
            folder_->setText(directory);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (name_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Create Project"), QStringLiteral("Enter a project name."));
            name_->setFocus();
            return;
        }
        if (folder_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Create Project"), QStringLiteral("Choose a project folder."));
            folder_->setFocus();
            return;
        }
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    name_->selectAll();
    name_->setFocus();
}

QString CreateProjectDialog::projectName() const { return name_->text().trimmed(); }
QString CreateProjectDialog::projectFolder() const { return folder_->text().trimmed(); }
bool CreateProjectDialog::createFolder() const { return createFolder_->isChecked(); }

EmptySchematicPanel::EmptySchematicPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->addStretch(1);
    auto *card = new QGroupBox(QStringLiteral("Create a schematic"));
    card->setMaximumWidth(520);
    auto *cardLayout = new QVBoxLayout(card);
    description_ = new QLabel(QStringLiteral("This project has no schematics yet. Create one to start drawing."));
    description_->setWordWrap(true);
    cardLayout->addWidget(description_);

    openSection_ = new QWidget;
    auto *openSectionLayout = new QHBoxLayout(openSection_);
    openSectionLayout->setContentsMargins(0, 0, 0, 0);
    open_ = new QComboBox;
    open_->setAccessibleName(QStringLiteral("Open schematic"));
    openSectionLayout->addWidget(open_, 1);
    auto *openButton = new QPushButton(QStringLiteral("Open"));
    openSectionLayout->addWidget(openButton);
    cardLayout->addWidget(openSection_);

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
    setOpenCandidates({});
    connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
    connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
    connect(openButton, &QPushButton::clicked, this, [this] {
        const auto id = static_cast<ice::ObjectId>(open_->currentData().toULongLong());
        if (id != 0 && onOpen_)
            onOpen_(id);
    });
}

void EmptySchematicPanel::setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

void EmptySchematicPanel::setOnOpen(OpenCallback callback) { onOpen_ = std::move(callback); }

void EmptySchematicPanel::setOpenCandidates(const std::vector<std::pair<ice::ObjectId, QString>> &candidates) {
    const QSignalBlocker blocker(open_);
    open_->clear();
    for (const auto &[id, name] : candidates)
        open_->addItem(name, QVariant::fromValue<qulonglong>(id));
    const bool hasCandidates = !candidates.empty();
    openSection_->setVisible(hasCandidates);
    if (description_)
        description_->setText(hasCandidates
                                  ? QStringLiteral("All schematics are closed. Open an existing file or create a "
                                                   "new schematic.")
                                  : QStringLiteral("This project has no schematics yet. Create one to start drawing."));
}

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
    description_ = new QLabel(QStringLiteral("This project has no layout cells yet. Create one to start drawing."));
    description_->setWordWrap(true);
    cardLayout->addWidget(description_);

    openSection_ = new QWidget;
    auto *openSectionLayout = new QHBoxLayout(openSection_);
    openSectionLayout->setContentsMargins(0, 0, 0, 0);
    open_ = new QComboBox;
    open_->setAccessibleName(QStringLiteral("Open layout"));
    openSectionLayout->addWidget(open_, 1);
    auto *openButton = new QPushButton(QStringLiteral("Open"));
    openSectionLayout->addWidget(openButton);
    cardLayout->addWidget(openSection_);

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
    setOpenCandidates({});
    connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
    connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
    connect(openButton, &QPushButton::clicked, this, [this] {
        const auto id = static_cast<ice::ObjectId>(open_->currentData().toULongLong());
        if (id != 0 && onOpen_)
            onOpen_(id);
    });
}

void EmptyLayoutPanel::setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

void EmptyLayoutPanel::setOnOpen(OpenCallback callback) { onOpen_ = std::move(callback); }

void EmptyLayoutPanel::setOpenCandidates(const std::vector<std::pair<ice::ObjectId, QString>> &candidates) {
    const QSignalBlocker blocker(open_);
    open_->clear();
    for (const auto &[id, name] : candidates)
        open_->addItem(name, QVariant::fromValue<qulonglong>(id));
    const bool hasCandidates = !candidates.empty();
    openSection_->setVisible(hasCandidates);
    if (description_)
        description_->setText(
            hasCandidates ? QStringLiteral("All layout files are closed. Open an existing file or create a "
                                           "new layout.")
                          : QStringLiteral("This project has no layout cells yet. Create one to start drawing."));
}

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
