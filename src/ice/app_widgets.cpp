#include "app_widgets.hpp"

#include "icelib/verification.hpp"

#include "icons.hpp"
#include "package_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractButton>
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
#include <QSettings>
#include <QSignalBlocker>
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
    AgentBackend() { pool.setMaxThreadCount(1); }

    QThreadPool pool;
    std::atomic<bool> alive{true};
    std::atomic<unsigned> generation{0};
#ifdef ICE_HAVE_AGENTSDK
    std::vector<agentsdk::acp::agent_entry> agents;
    std::unique_ptr<agentsdk::acp::acp_client> client;
    std::unique_ptr<agentsdk::acp::acp_session> session;
#endif
};

namespace {

QString jsonString(const nlohmann::json &object, const char *key, const QString &fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return fallback;
    return QString::fromStdString(it->get<std::string>());
}

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
    sessionPanel->setFixedWidth(260);
    auto *sessionLayout = new QVBoxLayout(sessionPanel);
    sessionLayout->setContentsMargins(10, 8, 10, 8);
    sessionLayout->setSpacing(6);
    sessionLayout->addWidget(new QLabel(QStringLiteral("Agent")));
    agentSelector_ = new QComboBox;
    agentSelector_->setAccessibleName(QStringLiteral("ACP agent"));
    sessionLayout->addWidget(agentSelector_);
    agentConnectButton_ = new QPushButton(QStringLiteral("Connect"));
    sessionLayout->addWidget(agentConnectButton_);
    agentStatus_ = new QLabel(QStringLiteral("Not connected"));
    agentStatus_->setWordWrap(true);
    agentStatus_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    sessionLayout->addWidget(agentStatus_);
    sessionLayout->addSpacing(6);
    sessionTitle_ = new QLabel(QStringLiteral("No active session"));
    sessionTitle_->setWordWrap(true);
    sessionTitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sessionLayout->addWidget(sessionTitle_);
    sessionLayout->addSpacing(6);
    tokens_ = new QLabel(QStringLiteral("0 / 0 tokens"));
    sessionLayout->addWidget(tokens_);
    contextUsage_ = new QProgressBar;
    contextUsage_->setRange(0, 100);
    contextUsage_->setValue(0);
    contextUsage_->setFormat(QStringLiteral("%p% used"));
    contextUsage_->setMaximumHeight(18);
    sessionLayout->addWidget(contextUsage_);
    sessionLayout->addSpacing(6);
    mcpServers_ = new QListWidget;
    mcpServers_->setAccessibleName(QStringLiteral("MCP servers"));
    mcpServers_->setAlternatingRowColors(true);
    auto *noServers = new QListWidgetItem(QStringLiteral("No MCP servers connected"), mcpServers_);
    noServers->setFlags(Qt::NoItemFlags);
    sessionLayout->addWidget(mcpServers_, 1);

    auto *conversationColumn = new QVBoxLayout;
    conversationColumn->addWidget(transcript_, 1);
    prompt_ = new QLineEdit;
    prompt_->setPlaceholderText(QStringLiteral("Ask the agent about the current project..."));
    prompt_->setEnabled(false);
    conversationColumn->addWidget(prompt_);
    agentLayout->addLayout(conversationColumn, 1);

    auto *sessionColumn = new QVBoxLayout;
    sessionColumn->setSpacing(8);
    sessionColumn->addWidget(sessionPanel, 1);
    sendButton_ = new QPushButton(QStringLiteral("Send"));
    sendButton_->setFixedWidth(260);
    sendButton_->setEnabled(false);
    sessionColumn->addWidget(sendButton_);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel turn"));
    cancelButton_->setFixedWidth(260);
    cancelButton_->setEnabled(false);
    sessionColumn->addWidget(cancelButton_);
    agentLayout->addLayout(sessionColumn, 0);
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
        agentStatus_->setText(QStringLiteral("Install an ACP-capable agent on PATH (for example opencode)."));
    }
#else
    agentSelector_->addItem(QStringLiteral("agentsdk-cpp not built"));
    agentSelector_->setEnabled(false);
    agentConnectButton_->setEnabled(false);
    agentStatus_->setText(QStringLiteral("Build with agentsdk-cpp available or set AGENTSDK_CPP_DIR."));
#endif

    connect(agentConnectButton_, &QPushButton::clicked, this, [this] {
        if (agentConnected_)
            disconnectAgent();
        else
            connectAgent();
    });
    connect(sendButton_, &QPushButton::clicked, this, &ActivityPanel::sendPrompt);
    connect(cancelButton_, &QPushButton::clicked, this, &ActivityPanel::cancelPrompt);
    connect(prompt_, &QLineEdit::returnPressed, this, &ActivityPanel::sendPrompt);
    updateAgentControls();

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

void ActivityPanel::appendTranscript(const QString &text) {
    if (!transcript_ || text.isEmpty())
        return;
    auto cursor = transcript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    transcript_->setTextCursor(cursor);
    transcript_->ensureCursorVisible();
}

void ActivityPanel::updateAgentUsage(qint64 used, qint64 size) {
    if (!tokens_ || !contextUsage_)
        return;
    tokens_->setText(QStringLiteral("%1 / %2 tokens").arg(used).arg(size));
    const int percentage = size > 0 ? static_cast<int>(used * 100 / size) : 0;
    contextUsage_->setValue(std::clamp(percentage, 0, 100));
}

void ActivityPanel::updateAgentControls() {
    if (!agentConnectButton_ || !agentSelector_ || !prompt_ || !sendButton_ || !cancelButton_)
        return;
    const bool hasAgent =
        agentSelector_->currentData().isValid() && !agentSelector_->currentData().toString().isEmpty();
    agentConnectButton_->setText(agentConnected_ ? QStringLiteral("Disconnect") : QStringLiteral("Connect"));
    agentConnectButton_->setEnabled(!agentConnecting_ && (agentConnected_ || hasAgent));
    agentSelector_->setEnabled(!agentConnecting_ && !agentConnected_ && !promptRunning_);
    prompt_->setEnabled(agentConnected_ && !promptRunning_);
    sendButton_->setEnabled(agentConnected_ && !promptRunning_);
    cancelButton_->setEnabled(agentConnected_ && promptRunning_);
}

void ActivityPanel::setPromptRunning(bool running) {
    promptRunning_ = running;
    updateAgentControls();
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
    agentBackend_->session = std::make_unique<agentsdk::acp::acp_session>(*agentBackend_->client);

    auto *client = agentBackend_->client.get();
    auto *session = agentBackend_->session.get();
    session->set_update_handler([this](const std::string &, const std::string &params) { handleAgentUpdate(params); });
    session->set_agent_request_handler(
        [this](const std::string &method, const std::string &params) -> agentsdk::acp::agent_response {
            agentsdk::acp::agent_response response;
            if (method != "session/request_permission")
                return response;
            response.handled = true;
            response.result_json = handleAgentPermission(params);
            return response;
        });

    const auto cwd = projectPath_.isEmpty() ? QDir::currentPath() : projectPath_;
    const auto generation = agentBackend_->generation.fetch_add(1) + 1;
    agentConnecting_ = true;
    agentStatus_->setText(QStringLiteral("Launching %1...").arg(QString::fromStdString(entry.display_name)));
    appendTranscript(QStringLiteral("\nConnecting to %1...\n").arg(QString::fromStdString(entry.display_name)));
    updateAgentControls();

    agentBackend_->pool.start(QRunnable::create([this, client, session, entry, cwd, generation] {
        QString error;
        bool ok = client->launch_agent(entry.command, entry.args);
        if (!ok)
            error = QStringLiteral("Unable to launch %1.").arg(QString::fromStdString(entry.display_name));
        if (ok) {
            agentsdk::acp::client_capabilities capabilities;
            capabilities.fs.read_text_file = true;
            capabilities.fs.write_text_file = true;
            capabilities.terminal = false;
            agentsdk::acp::implementation_info info;
            info.name = "ice";
            info.title = "ICE";
            info.version = ice::version();
            ok = session->initialize(capabilities, info);
            if (!ok)
                error = QStringLiteral("ACP initialize failed.");
        }
        std::string sessionId;
        if (ok) {
            sessionId = session->new_session(cwd.toStdString());
            if (sessionId.empty()) {
                ok = false;
                error = QStringLiteral("The agent did not create an ACP session.");
            }
        }
        if (!ok)
            client->terminate();
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, error, entry, sessionId, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                agentConnecting_ = false;
                agentConnected_ = ok;
                if (ok) {
                    agentStatus_->setText(
                        QStringLiteral("Connected to %1").arg(QString::fromStdString(entry.display_name)));
                    sessionTitle_->setText(
                        QStringLiteral("Agent: %1\nSession: %2")
                            .arg(QString::fromStdString(entry.display_name), QString::fromStdString(sessionId)));
                    appendTranscript(
                        QStringLiteral("Connected. ACP session %1.\n").arg(QString::fromStdString(sessionId)));
                } else {
                    agentStatus_->setText(error);
                    sessionTitle_->setText(QStringLiteral("No active session"));
                    appendTranscript(error + QStringLiteral("\n"));
                }
                updateAgentControls();
            },
            Qt::QueuedConnection);
    }));
#else
    agentStatus_->setText(QStringLiteral("agentsdk-cpp is not available in this build."));
#endif
}

void ActivityPanel::disconnectAgent() {
    if (!agentBackend_)
        return;
    agentBackend_->generation.fetch_add(1);
    agentConnected_ = false;
    agentConnecting_ = false;
    promptRunning_ = false;
#ifdef ICE_HAVE_AGENTSDK
    if (agentBackend_->client)
        agentBackend_->client->terminate();
    agentBackend_->pool.waitForDone();
    agentBackend_->client.reset();
    agentBackend_->session.reset();
#endif
    if (sessionTitle_)
        sessionTitle_->setText(QStringLiteral("No active session"));
    if (agentStatus_)
        agentStatus_->setText(QStringLiteral("Disconnected"));
    updateAgentUsage(0, 0);
    appendTranscript(QStringLiteral("\nDisconnected.\n"));
    updateAgentControls();
}

void ActivityPanel::sendPrompt() {
    if (!agentBackend_ || !agentConnected_ || promptRunning_ || !prompt_ || prompt_->text().trimmed().isEmpty())
        return;
#ifdef ICE_HAVE_AGENTSDK
    if (!agentBackend_->session)
        return;
    const auto text = prompt_->text().trimmed();
    prompt_->clear();
    appendTranscript(QStringLiteral("\nYou: %1\n").arg(text));
    setPromptRunning(true);
    const auto generation = agentBackend_->generation.load();
    auto *session = agentBackend_->session.get();
    agentBackend_->pool.start(QRunnable::create([this, session, text, generation] {
        const bool ok = session->prompt(text.toStdString());
        if (!agentBackend_ || !agentBackend_->alive.load())
            return;
        QMetaObject::invokeMethod(
            this,
            [this, ok, generation] {
                if (!agentBackend_ || !agentBackend_->alive.load() || agentBackend_->generation.load() != generation)
                    return;
                setPromptRunning(false);
                appendTranscript(ok ? QStringLiteral("\n[Turn complete]\n")
                                    : QStringLiteral("\n[Prompt failed or cancelled]\n"));
            },
            Qt::QueuedConnection);
    }));
#endif
}

void ActivityPanel::cancelPrompt() {
#ifdef ICE_HAVE_AGENTSDK
    if (agentBackend_ && agentBackend_->session) {
        appendTranscript(QStringLiteral("\nCancelling current turn...\n"));
        agentBackend_->session->cancel();
    }
#endif
}

void ActivityPanel::handleAgentUpdate(const std::string &params) {
    QString text;
    std::optional<qint64> used;
    std::optional<qint64> size;
    try {
        const auto root = nlohmann::json::parse(params);
        const auto updateIt = root.find("update");
        if (updateIt == root.end() || !updateIt->is_object())
            return;
        const auto &update = *updateIt;
        const auto kind = jsonString(update, "sessionUpdate");
        if (kind == QStringLiteral("agent_message_chunk") || kind == QStringLiteral("agent_thought_chunk")) {
            const auto contentIt = update.find("content");
            if (contentIt != update.end() && contentIt->is_object() &&
                jsonString(*contentIt, "type") == QStringLiteral("text"))
                text = jsonString(*contentIt, "text");
        } else if (kind == QStringLiteral("tool_call") || kind == QStringLiteral("tool_call_update")) {
            const auto title = jsonString(update, "title", QStringLiteral("Tool call"));
            const auto status = jsonString(update, "status");
            const auto toolKind = jsonString(update, "kind");
            text = QStringLiteral("\n[tool] %1").arg(title);
            if (!toolKind.isEmpty())
                text += QStringLiteral(" · %1").arg(toolKind);
            if (!status.isEmpty())
                text += QStringLiteral(" · %1").arg(status);
            text += QLatin1Char('\n');
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
    if (text.isEmpty() && !used && !size)
        return;
    QMetaObject::invokeMethod(
        this,
        [this, text, used, size] {
            if (!agentBackend_ || !agentBackend_->alive.load())
                return;
            appendTranscript(text);
            if (used && size)
                updateAgentUsage(*used, *size);
        },
        Qt::QueuedConnection);
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
