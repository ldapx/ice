#pragma once

#include "agent_transcript.hpp"
#include "icelib/core.hpp"

#include <QAction>
#include <QColor>
#include <QDialog>
#include <QElapsedTimer>
#include <QSize>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QButtonGroup;
class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPaintEvent;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace ice {
struct LvsReport;
struct VerificationReport;
} // namespace ice

struct LayerStyle;

class DiagnosticsPanel : public QWidget {
  public:
    explicit DiagnosticsPanel(QWidget *parent = nullptr);
};

class ActivityPanel : public QWidget {
  public:
    explicit ActivityPanel(QWidget *parent = nullptr);
    ~ActivityPanel() override;

    void setProjectPath(const QString &path);
    void setProjectChangedCallback(std::function<void()> callback);
    void setDrcReport(const ice::VerificationReport &report, const QString &reportPath = {});
    void setDrcRunning(const QString &message = {});
    void setDrcError(const QString &message);
    void showDrcTab();
    void setLvsReport(const ice::LvsReport &report);
    void setLvsError(const QString &message);
    void showLvsTab();
    void showAgentTab();
    void appendMessage(const QString &message);
    void showMessagesTab();

  private:
    struct AgentBackend;
    struct AgentToolCall {
        QString title;
        QString kind;
        QString command;
        QString output;
        QString path;
        QVector<AgentQuestion> questions;
    };
    struct AgentSession {
        std::string acpSessionId;
        QString label;
        AgentTranscriptView *transcript = nullptr;
        QString configOptionsJson;
        QString modelConfigId;
        QString pendingUserText;
        std::map<std::string, AgentToolCall> toolCalls;
        QElapsedTimer turnTimer;
        int pendingReads = 0;
        qint64 used = 0;
        qint64 size = 0;
        bool modelAvailable = false;
        bool promptRunning = false;
        bool loading = false;
    };
    struct AgentSessionSummary {
        std::string acpSessionId;
        QString title;
        QString updatedAt;
    };

    void applyAgentSelection();
    void connectAgent();
    void disconnectAgent();
    void sendPrompt();
    void cancelPrompt();
    void updateAgentControls();
    void appendTranscript(const QString &text);
    void handleAgentUpdate(const std::string &acpSessionId, const std::string &params);
    std::string handleAgentPermission(const std::string &params);
    std::string handleAgentElicitation(const std::string &params);
    void updateAgentUsage(qint64 used, qint64 size);
    void setAgentStatus(const QString &text);
    void populateModelSelector();
    void applyModelSelection();
    AgentSession *activeAgentSession();
    AgentSession *findAgentSession(const std::string &acpSessionId);
    int findAgentSessionIndex(const std::string &acpSessionId) const;
    void addAgentSession(const std::string &acpSessionId, const QString &configOptionsJson);
    void removeAgentSession(const std::string &acpSessionId);
    void refreshSessionSelector();
    void switchAgentSession(int index);
    void showSessionTranscript();
    void appendSessionTranscript(const std::string &acpSessionId, const QString &text);
    void flushPendingReads(AgentSession &session);
    void handleToolCallUpdate(AgentSession &session, const AgentToolUpdate &update);
    void setSessionPromptRunning(const std::string &acpSessionId, bool running);
    QString agentModeLabel(const AgentSession &session) const;
    QString agentModelLabel(const AgentSession &session) const;
    void newAgentSession();
    void closeActiveAgentSession();
    void refreshPastAgentSessions();
    void loadAgentSession(const std::string &acpSessionId);

    QLabel *drcSummary_ = nullptr;
    QTableWidget *drcTable_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QPlainTextEdit *messages_ = nullptr;
    QLabel *lvsSummary_ = nullptr;
    QPlainTextEdit *lvsReport_ = nullptr;
    int drcTabIndex_ = -1;
    int lvsTabIndex_ = -1;
    int agentTabIndex_ = -1;
    int messagesTabIndex_ = -1;

    QComboBox *agentSelector_ = nullptr;
    QComboBox *sessionSelector_ = nullptr;
    QComboBox *modelSelector_ = nullptr;
    QLabel *agentStatus_ = nullptr;
    QLabel *tokens_ = nullptr;
    QProgressBar *contextUsage_ = nullptr;
    QStackedWidget *agentTranscriptStack_ = nullptr;
    AgentTranscriptView *agentTranscriptPlaceholder_ = nullptr;
    QLineEdit *prompt_ = nullptr;
    QPushButton *newSessionButton_ = nullptr;
    QPushButton *closeSessionButton_ = nullptr;
    QPushButton *sendButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QLabel *agentActivity_ = nullptr;
    QListWidget *mcpServers_ = nullptr;
    std::unique_ptr<AgentBackend> agentBackend_;
    std::vector<AgentSession> agentSessions_;
    std::vector<AgentSessionSummary> pastAgentSessions_;
    QString projectPath_;
    QString connectedAgentCommand_;
    int activeAgentSession_ = -1;
    int nextAgentSessionNumber_ = 1;
    bool agentConnected_ = false;
    bool agentConnecting_ = false;
    bool sessionCreating_ = false;
    bool sessionClosing_ = false;
    bool sessionLoading_ = false;
    bool sessionListLoading_ = false;
    bool loadSupported_ = false;
    bool refreshingSessionSelector_ = false;
    bool modelChanging_ = false;
    bool updatingModelSelector_ = false;
    std::function<void()> projectChangedCallback_;
};

class CommandPalette : public QDialog {
  public:
    explicit CommandPalette(const QList<QAction *> &actions, QWidget *parent = nullptr);

  private:
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
};

class StatusBar : public QStatusBar {
  public:
    explicit StatusBar(QWidget *parent = nullptr);
    void setProjectName(const QString &name);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString projectName_;
};

class StartupView : public QWidget {
  public:
    using CreateProjectCallback = std::function<void(const QString &name, const QString &folder, bool createFolder)>;
    using OpenProjectCallback = std::function<void()>;
    using OpenRecentCallback = std::function<void(const QString &path)>;
    using RemoveRecentCallback = std::function<void(const QString &path)>;

    explicit StartupView(QWidget *parent = nullptr);
    void setOnCreateProject(CreateProjectCallback callback);
    void setOnOpenProject(OpenProjectCallback callback);
    void setOnOpenRecent(OpenRecentCallback callback);
    void setOnRemoveRecent(RemoveRecentCallback callback);
    void setRecentProjects(const QStringList &paths);
    void clearCreateForm();
    void refreshAppearance();

  private:
    static QLabel *makeLink(const QString &text, const QString &url);

    QLineEdit *name_ = nullptr;
    QLineEdit *location_ = nullptr;
    QCheckBox *createFolder_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QListWidget *recent_ = nullptr;
    QLabel *subtitle_ = nullptr;
    QLabel *newsPlaceholder_ = nullptr;
    CreateProjectCallback onCreateProject_;
    OpenProjectCallback onOpenProject_;
    OpenRecentCallback onOpenRecent_;
    RemoveRecentCallback onRemoveRecent_;
};

class CreateProjectDialog : public QDialog {
  public:
    explicit CreateProjectDialog(QWidget *parent = nullptr);

    QString projectName() const;
    QString projectFolder() const;
    bool createFolder() const;

  private:
    QLineEdit *name_ = nullptr;
    QLineEdit *folder_ = nullptr;
    QCheckBox *createFolder_ = nullptr;
};

class EmptySchematicPanel : public QWidget {
  public:
    enum class Kind { SchematicDrawing, TextSpice, SystemVerilog };
    using CreateCallback = std::function<void(Kind kind, const QString &name, const std::string &pdk)>;
    using OpenCallback = std::function<void(ice::ObjectId)>;
    explicit EmptySchematicPanel(QWidget *parent = nullptr);
    void setOnCreate(CreateCallback callback);
    void setOnOpen(OpenCallback callback);
    void setOpenCandidates(const std::vector<std::pair<ice::ObjectId, QString>> &candidates);
    void refreshPdks(const std::filesystem::path &installDirectory);
    void prepareForDisplay();
    // Fills the name field with the suggested default unless the user typed
    // their own name since the panel was last shown.
    void setDefaultName(const QString &name);

  private:
    void submit();
    void selectKind(Kind kind);
    void updateKindUi();

    QLabel *description_ = nullptr;
    QWidget *openSection_ = nullptr;
    QComboBox *open_ = nullptr;
    QLabel *kindDescription_ = nullptr;
    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QButtonGroup *kindGroup_ = nullptr;
    Kind kind_ = Kind::SchematicDrawing;
    bool nameEdited_ = false;
    CreateCallback onCreate_;
    OpenCallback onOpen_;
};

class EmptyLayoutPanel : public QWidget {
  public:
    enum class Kind { StaticLayout, ProceduralLayout };
    using CreateCallback = std::function<void(Kind kind, const QString &name, const std::string &pdk)>;
    using OpenCallback = std::function<void(ice::ObjectId)>;
    explicit EmptyLayoutPanel(QWidget *parent = nullptr);
    void setOnCreate(CreateCallback callback);
    void setOnOpen(OpenCallback callback);
    void setOpenCandidates(const std::vector<std::pair<ice::ObjectId, QString>> &candidates);
    void refreshPdks(const std::filesystem::path &installDirectory);
    void prepareForDisplay();
    // Fills the name field with the suggested default unless the user typed
    // their own name since the panel was last shown.
    void setDefaultName(const QString &name);

  private:
    void submit();
    void selectKind(Kind kind);
    void updateKindUi();

    QLabel *description_ = nullptr;
    QWidget *openSection_ = nullptr;
    QComboBox *open_ = nullptr;
    QLabel *kindDescription_ = nullptr;
    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QButtonGroup *kindGroup_ = nullptr;
    Kind kind_ = Kind::StaticLayout;
    bool nameEdited_ = false;
    CreateCallback onCreate_;
    OpenCallback onOpen_;
};

class CreateLayoutDialog : public QDialog {
  public:
    explicit CreateLayoutDialog(const std::filesystem::path &installDirectory, const QString &defaultName,
                                QWidget *parent = nullptr);
    QString layoutName() const;
    std::string pdkId() const;

  private:
    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    QLabel *pdkDescription_ = nullptr;
};

class CreateLayerDialog : public QDialog {
  public:
    explicit CreateLayerDialog(const QString &defaultName, int layerNumber, const LayerStyle &suggestedStyle,
                               QWidget *parent = nullptr);
    QString layerName() const;
    QColor color() const;
    int patternIndex() const;

  private:
    void updateColorButton();
    void updatePreview();

    QLineEdit *name_ = nullptr;
    QPushButton *colorButton_ = nullptr;
    QComboBox *pattern_ = nullptr;
    QLabel *preview_ = nullptr;
    QColor color_;
    Qt::BrushStyle autoPattern_ = Qt::SolidPattern;
};

class WorkspaceStack : public QStackedWidget {
  public:
    using QStackedWidget::QStackedWidget;
    QSize minimumSizeHint() const override;
};
