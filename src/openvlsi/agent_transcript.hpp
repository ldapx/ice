#pragma once

#include <QScrollArea>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

class QWidget;

// A nested tool invocation reported by an agent (for example the individual
// MCP calls made by an ACP "execute" call).
struct AgentToolInvocation {
    QString name;
    QString status;
    QString arguments;
    QString error;
};

// A question/answer exchange produced by an agent's question tool.
struct AgentQuestion {
    QString header;
    QString question;
    QStringList options;
    QStringList answers;
};

// One ACP tool_call/tool_call_update reduced to the pieces the transcript
// renders.  The widget never sees raw JSON or ACP payloads.
struct AgentToolUpdate {
    QString id;
    QString title;
    QString kind;
    QString status;
    QString command;
    QString output;
    QVector<AgentToolInvocation> invocations;
    QVector<AgentQuestion> questions;
    QVector<QStringList> answers;
};

// Structured, scrollable agent transcript.
//
// The view renders markdown, collapses reasoning and tool output, groups
// streamed chunks by message/tool-call id, and keeps the transcript pinned to
// the bottom while the user has not scrolled away.  A view instance owns the
// transcript for one ACP session.
class AgentTranscriptView : public QScrollArea {
  public:
    explicit AgentTranscriptView(QWidget *parent = nullptr);
    ~AgentTranscriptView() override;

    void clear();
    bool isEmpty() const;
    int blockCount() const;

    // Streamed content.  Chunks with the same id update one block.
    void appendUserChunk(const QString &messageId, const QString &text);
    void appendAssistantChunk(const QString &messageId, const QString &text);
    void appendThoughtChunk(const QString &messageId, const QString &text);

    void appendNotice(const QString &text);
    void appendReadSummary(int count);
    void appendMarkdownBlock(const QString &markdown);

    void updateToolCall(const AgentToolUpdate &update);
    void updateQuestion(const QString &id, const QVector<AgentQuestion> &questions);

    // Close any block that is still streaming.  Called when the next kind of
    // event arrives and at the end of a turn.
    void finalizeOpenBlocks();

    // Add the opencode-style turn footer:
    //   Build · Iceman Contributor Free · 2m 43s · 50.6 tok/s
    void finishTurn(const QString &mode, const QString &model, double seconds, qint64 outputTokens,
                    qint64 thoughtTokens, bool failed, const QString &detail = {});

    void scrollToBottom();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
