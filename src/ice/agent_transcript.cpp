#include "agent_transcript.hpp"

#include "ui_colors.hpp"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace {

struct Theme {
    QColor text = ice::uiColor(ice::UiColor::Text1);
    QColor muted = ice::uiColor(ice::UiColor::Text2);
    QColor accent = ice::uiColor(ice::UiColor::Accent1);
    QColor userBackground = QApplication::palette().color(QPalette::AlternateBase);
    QColor codeBackground = QApplication::palette().color(QPalette::AlternateBase);
    QColor success;
    QColor failure;
    QColor warning;

    Theme() {
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        success = dark ? QColor(QStringLiteral("#8fd694")) : QColor(QStringLiteral("#2e7d32"));
        failure = dark ? QColor(QStringLiteral("#ff8a80")) : QColor(QStringLiteral("#c62828"));
        warning = dark ? QColor(QStringLiteral("#ffd180")) : QColor(QStringLiteral("#b26a00"));
    }
};

QString cssColor(const QColor &color) { return color.name(QColor::HexRgb); }

QString formatDuration(qint64 milliseconds) {
    if (milliseconds < 0)
        return {};
    if (milliseconds < 1000)
        return QStringLiteral("%1ms").arg(milliseconds);
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    if (seconds < 60.0)
        return QStringLiteral("%1s").arg(seconds, 0, 'f', 1);
    const auto total = static_cast<qint64>(std::llround(seconds));
    return QStringLiteral("%1m %2s").arg(total / 60).arg(total % 60);
}

QString truncateOneLine(QString text, int limit) {
    text = text.simplified();
    if (text.size() > limit) {
        int cut = text.lastIndexOf(QLatin1Char(' '), limit);
        if (cut < limit / 2)
            cut = limit - 1;
        text = text.left(cut).trimmed() + QChar(0x2026);
    }
    return text;
}

// The first reasoning line is normally a markdown heading such as
// "**Clarifying inverter schematic details**"; use it as the collapsed title.
QString thoughtTitle(const QString &markdown) {
    int position = 0;
    while (position < markdown.size()) {
        int end = markdown.indexOf(QLatin1Char('\n'), position);
        if (end < 0)
            end = markdown.size();
        QString line = markdown.mid(position, end - position).trimmed();
        position = end + 1;
        if (line.isEmpty())
            continue;
        while (line.startsWith(QLatin1Char('#')))
            line.remove(0, 1);
        line = line.trimmed();
        if (line.size() >= 4 && line.startsWith(QStringLiteral("**")) && line.endsWith(QStringLiteral("**")))
            line = line.mid(2, line.size() - 4).trimmed();
        line.remove(QStringLiteral("**"));
        line.remove(QStringLiteral("__"));
        line.remove(QLatin1Char('`'));
        line = line.simplified();
        if (line.isEmpty())
            continue;
        const int sentence = line.indexOf(QStringLiteral(". "));
        if (sentence > 12)
            line = line.left(sentence);
        return truncateOneLine(line, 88);
    }
    return QStringLiteral("Thought");
}

QString errorExcerpt(QString text, int maxLines = 6) {
    text = text.trimmed();
    if (text.isEmpty())
        return {};
    QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() > maxLines) {
        const int omitted = lines.size() - maxLines;
        lines = lines.mid(0, maxLines);
        lines.push_back(QStringLiteral("(%1 more lines)").arg(omitted));
    }
    for (auto &line : lines) {
        if (line.size() > 300)
            line = line.left(297) + QChar(0x2026);
    }
    return lines.join(QLatin1Char('\n'));
}

QString statusIcon(const QString &status) {
    if (status == QStringLiteral("completed"))
        return QStringLiteral("\u2713");
    if (status == QStringLiteral("failed") || status == QStringLiteral("error"))
        return QStringLiteral("\u2717");
    if (status == QStringLiteral("in_progress") || status == QStringLiteral("pending"))
        return QStringLiteral("\u22ef");
    return QStringLiteral("\u203a");
}

QString statusWord(const QString &status) {
    if (status == QStringLiteral("failed") || status == QStringLiteral("error"))
        return QStringLiteral("failed");
    if (status == QStringLiteral("in_progress"))
        return QStringLiteral("running");
    if (status == QStringLiteral("pending"))
        return QStringLiteral("pending");
    return {};
}

QString widgetKey(const QString &value, const QString &fallback) { return value.isEmpty() ? fallback : value; }

// A QTextBrowser that renders markdown, has no internal scrollbars, and grows
// to fit its document.  Wheel events are forwarded to the transcript scroll
// area so scrolling over a message still scrolls the conversation.
class MarkdownView : public QTextBrowser {
  public:
    explicit MarkdownView(const Theme &theme, QWidget *parent = nullptr, int heightPadding = 4)
        : QTextBrowser(parent), theme_(theme), heightPadding_(heightPadding) {
        setReadOnly(true);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setOpenExternalLinks(true);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse);
        setFocusPolicy(Qt::ClickFocus);
        setContentsMargins(0, 0, 0, 0);
        setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; border: none; }"));
        auto palette = this->palette();
        palette.setColor(QPalette::Text, theme_.text);
        palette.setColor(QPalette::Base, Qt::transparent);
        setPalette(palette);
        document()->setDocumentMargin(0);
        document()->setDefaultFont(QApplication::font());
        document()->setDefaultStyleSheet(QStringLiteral("pre { background-color: %1; color: %2; "
                                                        "white-space: pre-wrap; }"
                                                        "code { font-family: monospace; }"
                                                        "a { color: %3; }"
                                                        "blockquote { color: %4; }")
                                             .arg(cssColor(theme_.codeBackground), cssColor(theme_.text),
                                                  cssColor(theme_.accent), cssColor(theme_.muted)));
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
                [this](const QSizeF &) { updateHeight(); });
    }

    void setMarkdownText(const QString &text) {
        markdown_ = text;
        setMarkdown(text);
        updateHeight();
    }

    void setHtmlText(const QString &html) {
        markdown_.clear();
        setHtml(html);
        updateHeight();
    }

    void refreshLayout() { updateHeight(); }

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QTextBrowser::resizeEvent(event);
        updateHeight();
    }

    void wheelEvent(QWheelEvent *event) override {
        QWidget *target = parentWidget();
        while (target && !qobject_cast<QScrollArea *>(target))
            target = target->parentWidget();
        auto *scroll = qobject_cast<QScrollArea *>(target);
        if (!scroll) {
            QTextBrowser::wheelEvent(event);
            return;
        }
        auto *bar = scroll->verticalScrollBar();
        const QPoint pixel = event->pixelDelta();
        if (!pixel.isNull()) {
            bar->setValue(bar->value() - pixel.y());
        } else {
            const int delta = event->angleDelta().y();
            if (delta != 0)
                bar->setValue(bar->value() - delta / 2);
        }
        event->accept();
    }

  private:
    void updateHeight() {
        if (updating_)
            return;
        const int width = viewport()->width();
        if (width <= 0)
            return;
        updating_ = true;
        document()->setTextWidth(width);
        const int height = static_cast<int>(std::ceil(document()->size().height())) + heightPadding_;
        updating_ = false;
        if (height != this->height())
            setFixedHeight(height);
    }

    Theme theme_;
    QString markdown_;
    int heightPadding_ = 4;
    bool updating_ = false;
};

class ClickableHeader : public QFrame {
  public:
    explicit ClickableHeader(const Theme &theme, QWidget *parent = nullptr) : QFrame(parent), theme_(theme) {
        setObjectName(QStringLiteral("agentTranscriptHeader"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFocusPolicy(Qt::NoFocus);
        setStyleSheet(QStringLiteral("QFrame#agentTranscriptHeader:hover { background-color: %1; border-radius: 4px; }")
                          .arg(cssColor(theme_.codeBackground)));
        layout_ = new QHBoxLayout(this);
        layout_->setContentsMargins(0, 1, 0, 1);
        layout_->setSpacing(4);
        icon_ = new QLabel(this);
        icon_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        icon_->setAttribute(Qt::WA_TransparentForMouseEvents);
        title_ = new QLabel(this);
        title_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        title_->setWordWrap(true);
        title_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        title_->setMinimumWidth(0);
        title_->setAttribute(Qt::WA_TransparentForMouseEvents);
        meta_ = new QLabel(this);
        meta_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        meta_->setAttribute(Qt::WA_TransparentForMouseEvents);
        meta_->setStyleSheet(QStringLiteral("color: %1;").arg(cssColor(theme_.muted)));
        end_ = new QLabel(this);
        end_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        end_->setAttribute(Qt::WA_TransparentForMouseEvents);
        end_->setStyleSheet(QStringLiteral("color: %1;").arg(cssColor(theme_.muted)));
        layout_->addWidget(icon_, 0);
        titleIndex_ = layout_->count();
        layout_->addWidget(title_, 0);
        layout_->addWidget(meta_, 0);
        stretchIndex_ = layout_->count();
        layout_->addStretch(1);
        layout_->addWidget(end_, 0);
        setClickable(false);
    }

    void setIcon(const QString &text, const QColor &color) {
        icon_->setText(text);
        icon_->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(cssColor(color)));
        icon_->setMinimumWidth(12);
    }

    void setTitle(const QString &text, bool monospace = false, const QColor &color = {}) {
        title_->setText(text);
        title_->setStyleSheet(QStringLiteral("color: %1;%2")
                                  .arg(cssColor(color.isValid() ? color : theme_.text),
                                       monospace ? QStringLiteral(" font-family: monospace;") : QString{}));
    }

    void setMeta(const QString &text) { meta_->setText(text); }

    void setEndText(const QString &text) { end_->setText(text); }

    void setWideTitle(bool wide) {
        title_->setSizePolicy(wide ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Preferred);
        layout_->setStretch(titleIndex_, wide ? 1 : 0);
        layout_->setStretch(stretchIndex_, wide ? 0 : 1);
    }

    void setClickable(bool clickable) {
        clickable_ = clickable;
        setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setOnClick(std::function<void()> callback) { callback_ = std::move(callback); }

  protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (clickable_ && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()) &&
            callback_)
            callback_();
        QFrame::mouseReleaseEvent(event);
    }

  private:
    Theme theme_;
    QHBoxLayout *layout_ = nullptr;
    QLabel *icon_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *meta_ = nullptr;
    QLabel *end_ = nullptr;
    int titleIndex_ = -1;
    int stretchIndex_ = -1;
    bool clickable_ = false;
    std::function<void()> callback_;
};

class MarkdownBlock : public QFrame {
  public:
    explicit MarkdownBlock(const Theme &theme, bool card = false, QWidget *parent = nullptr)
        : QFrame(parent), theme_(theme), card_(card) {
        setFrameShape(QFrame::NoFrame);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(card ? 10 : 0, card ? 6 : 0, card ? 10 : 0, card ? 6 : 0);
        layout->setSpacing(0);
        view_ = new MarkdownView(theme, this);
        layout->addWidget(view_);
        if (card_) {
            setObjectName(QStringLiteral("agentUserMessage"));
            setAttribute(Qt::WA_StyledBackground, true);
            setStyleSheet(QStringLiteral("QFrame#agentUserMessage { background-color: %1; border-left: 3px solid %2; "
                                         "border-radius: 4px; }")
                              .arg(cssColor(theme_.userBackground), cssColor(theme_.accent)));
        }
        connect(&renderTimer_, &QTimer::timeout, this, [this] { view_->setMarkdownText(markdown_); });
        renderTimer_.setSingleShot(true);
    }

    void setText(const QString &markdown) {
        markdown_ = markdown;
        view_->setMarkdownText(markdown_);
    }

    void appendText(const QString &text) {
        if (text.isEmpty())
            return;
        markdown_ += text;
        if (!renderTimer_.isActive())
            renderTimer_.start(40);
    }

    void flush() {
        if (renderTimer_.isActive())
            renderTimer_.stop();
        view_->setMarkdownText(markdown_);
    }

    QString text() const { return markdown_; }

  private:
    Theme theme_;
    bool card_ = false;
    MarkdownView *view_ = nullptr;
    QString markdown_;
    QTimer renderTimer_;
};

class ThoughtBlock : public QWidget {
  public:
    explicit ThoughtBlock(const Theme &theme, QWidget *parent = nullptr) : QWidget(parent), theme_(theme) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 1, 0, 1);
        layout->setSpacing(2);
        header_ = new ClickableHeader(theme, this);
        header_->setOnClick([this] { toggle(); });
        body_ = new MarkdownView(theme, this);
        body_->hide();
        layout->addWidget(header_);
        layout->addWidget(body_);
        connect(&renderTimer_, &QTimer::timeout, this, [this] {
            if (expanded_)
                body_->setMarkdownText(markdown_);
        });
        renderTimer_.setSingleShot(true);
        updateHeader();
    }

    void appendChunk(const QString &text) {
        if (text.isEmpty())
            return;
        if (!started_) {
            started_ = true;
            timer_.start();
        }
        if (finalized_) {
            finalized_ = false;
            durationMilliseconds_ = -1;
        }
        markdown_ += text;
        lastMilliseconds_ = timer_.elapsed();
        title_ = thoughtTitle(markdown_);
        updateHeader();
        if (expanded_ && !renderTimer_.isActive())
            renderTimer_.start(60);
    }

    void finalize() {
        if (!started_ || finalized_)
            return;
        finalized_ = true;
        const qint64 elapsed = lastMilliseconds_ > 0 ? lastMilliseconds_ : timer_.elapsed();
        durationMilliseconds_ = elapsed >= 40 ? elapsed : -1;
        if (expanded_)
            body_->setMarkdownText(markdown_);
        updateHeader();
    }

    void toggle() {
        expanded_ = !expanded_;
        body_->setVisible(expanded_);
        if (expanded_)
            body_->setMarkdownText(markdown_);
        updateHeader();
    }

  private:
    void updateHeader() {
        header_->setIcon(expanded_ ? QStringLiteral("\u2212") : QStringLiteral("+"), theme_.muted);
        header_->setTitle(QStringLiteral("Thought: %1").arg(title_), false, theme_.muted);
        header_->setMeta(durationMilliseconds_ >= 0
                             ? QStringLiteral(" \u00b7 %1").arg(formatDuration(durationMilliseconds_))
                             : QString{});
        header_->setClickable(true);
    }

    Theme theme_;
    ClickableHeader *header_ = nullptr;
    MarkdownView *body_ = nullptr;
    QString markdown_;
    QString title_ = QStringLiteral("Thought");
    QElapsedTimer timer_;
    QTimer renderTimer_;
    qint64 lastMilliseconds_ = 0;
    qint64 durationMilliseconds_ = -1;
    bool started_ = false;
    bool finalized_ = false;
    bool expanded_ = false;
};

class ToolBlock : public QWidget {
  public:
    explicit ToolBlock(const Theme &theme, QWidget *parent = nullptr) : QWidget(parent), theme_(theme) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 1, 0, 1);
        layout->setSpacing(2);
        header_ = new ClickableHeader(theme, this);
        header_->setOnClick([this] {
            if (!hasDetails_)
                return;
            expanded_ = !expanded_;
            details_->setVisible(expanded_);
            if (expanded_)
                details_->refreshLayout();
            updateHeader();
        });
        rows_ = new QWidget(this);
        rowsLayout_ = new QVBoxLayout(rows_);
        rowsLayout_->setContentsMargins(18, 1, 0, 1);
        rowsLayout_->setSpacing(1);
        details_ = new MarkdownView(theme, this);
        details_->hide();
        details_->setContentsMargins(18, 2, 0, 0);
        layout->addWidget(header_);
        layout->addWidget(rows_);
        layout->addWidget(details_);
    }

    void update(const AgentToolUpdate &update) {
        if (!update.id.isEmpty())
            id_ = update.id;
        if (!update.title.isEmpty() && (update.title != QStringLiteral("execute") || title_.isEmpty()))
            title_ = update.title;
        if (!update.kind.isEmpty())
            kind_ = update.kind;
        if (!update.status.isEmpty())
            status_ = update.status;
        if (!update.command.isEmpty())
            command_ = update.command;
        if (!update.invocations.isEmpty())
            invocations_ = update.invocations;
        if (!update.output.isEmpty())
            output_ = update.output;
        shellMode_ = !command_.isEmpty() &&
                     (kind_ == QStringLiteral("execute") || title_ == QStringLiteral("shell") || title_ == command_);
        if (shellMode_)
            invocations_.clear();

        bool failed = status_ == QStringLiteral("failed") || status_ == QStringLiteral("error");
        for (const auto &invocation : invocations_) {
            if (invocation.status == QStringLiteral("failed") || invocation.status == QStringLiteral("error"))
                failed = true;
        }
        failed_ = failed;
        rebuildRows();
        rebuildDetails();
        updateHeader();
    }

  private:
    void updateHeader() {
        if (shellMode_) {
            header_->setWideTitle(true);
            if (failed_)
                header_->setIcon(QStringLiteral("\u2717"), theme_.failure);
            else
                header_->setIcon(QStringLiteral("$"), theme_.muted);
            header_->setTitle(command_, true, failed_ ? theme_.failure : theme_.text);
            header_->setMeta({});
        } else {
            header_->setWideTitle(false);
            header_->setIcon(statusIcon(status_),
                             failed_ ? theme_.failure
                                     : (status_ == QStringLiteral("completed") ? theme_.success : theme_.muted));
            QString label = title_.isEmpty() ? kind_ : title_;
            if (label.isEmpty())
                label = QStringLiteral("tool");
            header_->setTitle(label, true, theme_.text);
            const QString word = statusWord(status_);
            header_->setMeta(word.isEmpty() ? QString{} : QStringLiteral(" \u00b7 %1").arg(word));
        }
        header_->setEndText(hasDetails_ ? (expanded_ ? QStringLiteral("\u2304") : QStringLiteral("\u203a"))
                                        : QString{});
        header_->setClickable(hasDetails_);
    }

    void clearRows() {
        while (auto *item = rowsLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
    }

    QString invocationError(const AgentToolInvocation &invocation) const {
        if (!invocation.error.isEmpty())
            return errorExcerpt(invocation.error);
        if (invocation.status == QStringLiteral("failed") || invocation.status == QStringLiteral("error"))
            return errorExcerpt(output_);
        return {};
    }

    void addRow(const AgentToolInvocation &invocation) {
        const bool failed =
            invocation.status == QStringLiteral("failed") || invocation.status == QStringLiteral("error");
        const QString rowName = invocation.name == title_ ? QString{} : invocation.name;
        const QString error = invocationError(invocation);
        if (rowName.isEmpty() && invocation.arguments.isEmpty() && error.isEmpty())
            return;

        QString icon;
        if (!rowName.isEmpty() || !invocation.arguments.isEmpty()) {
            if (failed)
                icon = QStringLiteral("\u2717");
            else if (invocation.status == QStringLiteral("pending") ||
                     invocation.status == QStringLiteral("in_progress"))
                icon = QStringLiteral("\u22ef");
            else
                icon = QStringLiteral("\u203a");
        }
        const QColor iconColor = failed ? theme_.failure
                                        : (invocation.status == QStringLiteral("pending") ||
                                                   invocation.status == QStringLiteral("in_progress")
                                               ? theme_.warning
                                               : theme_.muted);
        QString html;
        if (!icon.isEmpty())
            html += QStringLiteral("<span style=\"color:%1; font-weight:600;\">%2</span> ")
                        .arg(cssColor(iconColor), icon.toHtmlEscaped());
        if (!rowName.isEmpty())
            html += QStringLiteral("<span style=\"font-family:monospace; color:%1;\">%2</span>")
                        .arg(cssColor(theme_.text), rowName.toHtmlEscaped());
        if (!invocation.arguments.isEmpty()) {
            if (!html.isEmpty())
                html += QLatin1Char(' ');
            html += QStringLiteral("<span style=\"font-family:monospace; color:%1;\">[%2]</span>")
                        .arg(cssColor(theme_.muted), invocation.arguments.toHtmlEscaped());
        }
        if (!error.isEmpty()) {
            QString escaped = error.toHtmlEscaped();
            escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
            if (!html.isEmpty())
                html += QStringLiteral("<br/>");
            html += QStringLiteral("<span style=\"color:%1;\">\u21b3 %2</span>")
                        .arg(cssColor(failed ? theme_.failure : theme_.muted), escaped);
        }
        auto *row = new MarkdownView(theme_, rows_, 2);
        row->setHtmlText(html);
        rowsLayout_->addWidget(row);
    }

    void rebuildRows() {
        clearRows();
        if (invocations_.isEmpty()) {
            if (failed_) {
                AgentToolInvocation failedCall;
                failedCall.status = QStringLiteral("failed");
                failedCall.error = output_;
                addRow(failedCall);
            }
            rows_->setVisible(rowsLayout_->count() > 0);
            return;
        }
        bool anyFailed = false;
        for (const auto &invocation : invocations_) {
            if (invocation.status == QStringLiteral("failed") || invocation.status == QStringLiteral("error"))
                anyFailed = true;
            addRow(invocation);
        }
        if (failed_ && !anyFailed && !output_.trimmed().isEmpty()) {
            AgentToolInvocation failedCall;
            failedCall.status = QStringLiteral("failed");
            failedCall.error = output_;
            addRow(failedCall);
        }
        rows_->setVisible(rowsLayout_->count() > 0);
    }

    void rebuildDetails() {
        QString output = output_.trimmed();
        if (output.isEmpty()) {
            hasDetails_ = false;
            details_->hide();
            return;
        }
        constexpr int maxChars = 24000;
        constexpr int maxLines = 400;
        QStringList lines = output.split(QLatin1Char('\n'));
        bool truncated = false;
        if (lines.size() > maxLines) {
            lines = lines.mid(0, maxLines);
            truncated = true;
        }
        output = lines.join(QLatin1Char('\n'));
        if (output.size() > maxChars) {
            output = output.left(maxChars);
            truncated = true;
        }
        if (truncated)
            output += QStringLiteral("\n\n(output truncated)");
        details_->setHtmlText(
            QStringLiteral("<pre style=\"background-color:%1; color:%2; margin:0; white-space: pre-wrap;\">%3</pre>")
                .arg(cssColor(theme_.codeBackground), cssColor(theme_.text), output.toHtmlEscaped()));
        hasDetails_ = true;
        details_->setVisible(expanded_);
    }

    Theme theme_;
    ClickableHeader *header_ = nullptr;
    QWidget *rows_ = nullptr;
    QVBoxLayout *rowsLayout_ = nullptr;
    MarkdownView *details_ = nullptr;
    QString id_;
    QString title_;
    QString kind_;
    QString status_;
    QString command_;
    QString output_;
    QVector<AgentToolInvocation> invocations_;
    bool shellMode_ = false;
    bool failed_ = false;
    bool hasDetails_ = false;
    bool expanded_ = false;
};

} // namespace

class AgentTranscriptView::Impl {
  public:
    explicit Impl(AgentTranscriptView *owner) : q(owner) {}

    QWidget *addWidget(QWidget *widget) {
        layout->insertWidget(std::max(0, layout->count() - 1), widget);
        blocks.push_back(widget);
        ++blockCount;
        placeholder->hide();
        requestScroll();
        return widget;
    }

    void requestScroll() {
        if (!pinned || scrollScheduled)
            return;
        scrollScheduled = true;
        QTimer::singleShot(0, q, [this] {
            scrollScheduled = false;
            if (pinned)
                q->verticalScrollBar()->setValue(q->verticalScrollBar()->maximum());
        });
    }

    void finalizeThought() {
        if (openThoughtId.isEmpty())
            return;
        const auto it = thoughts.find(openThoughtId);
        if (it != thoughts.end())
            (*it)->finalize();
        openThoughtId.clear();
    }

    void finalizeAssistant() {
        if (openAssistantId.isEmpty())
            return;
        const auto it = messages.find(openAssistantId);
        if (it != messages.end())
            (*it)->flush();
        openAssistantId.clear();
    }

    void resetOpenIds() {
        openThoughtId.clear();
        openAssistantId.clear();
        openUserId.clear();
    }

    AgentTranscriptView *q = nullptr;
    Theme theme;
    QWidget *content = nullptr;
    QVBoxLayout *layout = nullptr;
    QLabel *placeholder = nullptr;
    std::vector<QWidget *> blocks;
    QHash<QString, MarkdownBlock *> userBlocks;
    QHash<QString, MarkdownBlock *> messages;
    QHash<QString, ThoughtBlock *> thoughts;
    QHash<QString, ToolBlock *> tools;
    QString openThoughtId;
    QString openAssistantId;
    QString openUserId;
    int blockCount = 0;
    bool pinned = true;
    bool scrollScheduled = false;
};

AgentTranscriptView::AgentTranscriptView(QWidget *parent) : QScrollArea(parent), impl_(std::make_unique<Impl>(this)) {
    setAccessibleName(QStringLiteral("Agent transcript"));
    auto transcriptPalette = palette();
    const QColor baseColor = QApplication::palette().color(QPalette::Base);
    transcriptPalette.setColor(QPalette::Base, baseColor);
    transcriptPalette.setColor(QPalette::Window, baseColor);
    setPalette(transcriptPalette);
    setBackgroundRole(QPalette::Base);
    setAutoFillBackground(true);
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setPalette(transcriptPalette);
    impl_->content = new QWidget;
    impl_->content->setAutoFillBackground(true);
    impl_->content->setBackgroundRole(QPalette::Base);
    impl_->content->setPalette(transcriptPalette);
    impl_->layout = new QVBoxLayout(impl_->content);
    impl_->layout->setContentsMargins(10, 8, 10, 10);
    impl_->layout->setSpacing(8);
    impl_->placeholder = new QLabel(QStringLiteral("Agent conversation and tool-call updates"), impl_->content);
    impl_->placeholder->setWordWrap(true);
    impl_->placeholder->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    impl_->placeholder->setStyleSheet(QStringLiteral("color: %1;").arg(cssColor(impl_->theme.muted)));
    impl_->layout->addWidget(impl_->placeholder);
    impl_->layout->addStretch(1);
    setWidget(impl_->content);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) { impl_->pinned = value >= verticalScrollBar()->maximum() - 8; });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int maximum) {
        if (impl_->pinned)
            verticalScrollBar()->setValue(maximum);
    });
}

AgentTranscriptView::~AgentTranscriptView() = default;

void AgentTranscriptView::clear() {
    for (auto *block : impl_->blocks) {
        if (block) {
            impl_->layout->removeWidget(block);
            delete block;
        }
    }
    impl_->blocks.clear();
    impl_->userBlocks.clear();
    impl_->messages.clear();
    impl_->thoughts.clear();
    impl_->tools.clear();
    impl_->blockCount = 0;
    impl_->resetOpenIds();
    impl_->placeholder->show();
    impl_->pinned = true;
    scrollToBottom();
}

bool AgentTranscriptView::isEmpty() const { return impl_->blockCount == 0; }

int AgentTranscriptView::blockCount() const { return impl_->blockCount; }

void AgentTranscriptView::appendUserChunk(const QString &messageId, const QString &text) {
    if (text.isEmpty())
        return;
    finalizeOpenBlocks();
    const QString key = widgetKey(messageId, QStringLiteral("__user__"));
    MarkdownBlock *block = nullptr;
    if (impl_->userBlocks.contains(key)) {
        block = impl_->userBlocks.value(key);
        block->appendText(text);
    } else {
        block = new MarkdownBlock(impl_->theme, true, impl_->content);
        block->setText(text);
        impl_->addWidget(block);
        impl_->userBlocks.insert(key, block);
    }
    impl_->openUserId = key;
    impl_->requestScroll();
}

void AgentTranscriptView::appendAssistantChunk(const QString &messageId, const QString &text) {
    if (text.isEmpty())
        return;
    impl_->finalizeThought();
    const QString key = widgetKey(messageId, QStringLiteral("__assistant__"));
    MarkdownBlock *block = nullptr;
    if (impl_->messages.contains(key)) {
        block = impl_->messages.value(key);
        block->appendText(text);
    } else {
        block = new MarkdownBlock(impl_->theme, false, impl_->content);
        block->setText(text);
        impl_->addWidget(block);
        impl_->messages.insert(key, block);
    }
    impl_->openAssistantId = key;
    impl_->requestScroll();
}

void AgentTranscriptView::appendThoughtChunk(const QString &messageId, const QString &text) {
    if (text.isEmpty())
        return;
    const QString key = widgetKey(messageId, QStringLiteral("__thought__"));
    if (impl_->openThoughtId != key) {
        impl_->finalizeThought();
        impl_->finalizeAssistant();
        if (!impl_->thoughts.contains(key)) {
            auto *block = new ThoughtBlock(impl_->theme, impl_->content);
            impl_->addWidget(block);
            impl_->thoughts.insert(key, block);
        }
        impl_->openThoughtId = key;
    }
    impl_->thoughts.value(key)->appendChunk(text);
    impl_->requestScroll();
}

void AgentTranscriptView::appendNotice(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    auto *label = new QLabel(impl_->content);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QString escaped = trimmed.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    label->setText(QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(cssColor(impl_->theme.muted), escaped));
    impl_->addWidget(label);
}

void AgentTranscriptView::appendReadSummary(int count) {
    if (count <= 0)
        return;
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    auto *label = new QLabel(impl_->content);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setText(QStringLiteral("<span style=\"color:%1;\">\u2192Explored: %2 %3</span>")
                       .arg(cssColor(impl_->theme.accent))
                       .arg(count)
                       .arg(count == 1 ? QStringLiteral("read") : QStringLiteral("reads")));
    impl_->addWidget(label);
}

void AgentTranscriptView::appendMarkdownBlock(const QString &markdown) {
    if (markdown.trimmed().isEmpty())
        return;
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    auto *block = new MarkdownBlock(impl_->theme, false, impl_->content);
    block->setText(markdown);
    impl_->addWidget(block);
}

void AgentTranscriptView::updateToolCall(const AgentToolUpdate &update) {
    if (update.id.isEmpty())
        return;
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    ToolBlock *block = nullptr;
    if (impl_->tools.contains(update.id)) {
        block = impl_->tools.value(update.id);
    } else {
        block = new ToolBlock(impl_->theme, impl_->content);
        impl_->addWidget(block);
        impl_->tools.insert(update.id, block);
    }
    block->update(update);
    impl_->requestScroll();
}

void AgentTranscriptView::updateQuestion(const QString &id, const QVector<AgentQuestion> &questions) {
    if (id.isEmpty() || questions.isEmpty())
        return;
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    QString markdown = QStringLiteral("# Questions\n\n");
    for (const auto &question : questions) {
        QString text = question.question.trimmed();
        if (text.isEmpty())
            text = question.header.trimmed();
        if (text.isEmpty())
            continue;
        markdown += QStringLiteral("**%1**\n\n").arg(text);
        if (!question.answers.isEmpty()) {
            for (const auto &answer : question.answers)
                markdown += QStringLiteral("%1\n\n").arg(answer);
        } else if (!question.options.isEmpty()) {
            for (const auto &option : question.options)
                markdown += QStringLiteral("- %1\n").arg(option);
            markdown += QLatin1Char('\n');
        } else {
            markdown += QStringLiteral("_Waiting for answer..._\n\n");
        }
    }
    MarkdownBlock *block = nullptr;
    if (impl_->messages.contains(QStringLiteral("question:") + id)) {
        block = impl_->messages.value(QStringLiteral("question:") + id);
        block->setText(markdown);
    } else {
        block = new MarkdownBlock(impl_->theme, false, impl_->content);
        block->setText(markdown);
        impl_->addWidget(block);
        impl_->messages.insert(QStringLiteral("question:") + id, block);
    }
    impl_->requestScroll();
}

void AgentTranscriptView::finalizeOpenBlocks() {
    impl_->finalizeThought();
    impl_->finalizeAssistant();
    if (!impl_->openUserId.isEmpty()) {
        const auto it = impl_->userBlocks.find(impl_->openUserId);
        if (it != impl_->userBlocks.end())
            (*it)->flush();
        impl_->openUserId.clear();
    }
}

void AgentTranscriptView::finishTurn(const QString &mode, const QString &model, double seconds, qint64 outputTokens,
                                     qint64 thoughtTokens, bool failed, const QString &detail) {
    finalizeOpenBlocks();
    QStringList parts;
    if (!mode.isEmpty())
        parts.push_back(mode);
    if (!model.isEmpty())
        parts.push_back(model);
    parts.push_back(formatDuration(static_cast<qint64>(std::llround(std::max(0.0, seconds) * 1000.0))));
    const double tokens = static_cast<double>(outputTokens + thoughtTokens);
    if (tokens > 0 && seconds > 0.05)
        parts.push_back(QStringLiteral("%1 tok/s").arg(tokens / seconds, 0, 'f', 1));
    if (failed)
        parts.push_back(QStringLiteral("failed"));
    else if (!detail.isEmpty())
        parts.push_back(detail);

    auto *label = new QLabel(impl_->content);
    label->setTextFormat(Qt::PlainText);
    label->setText(parts.join(QStringLiteral(" \u00b7 ")));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont font = QApplication::font();
    if (font.pointSizeF() > 0)
        font.setPointSizeF(std::max(7.0, font.pointSizeF() * 0.92));
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1; margin-top: 4px;").arg(cssColor(impl_->theme.muted)));
    impl_->addWidget(label);
}

void AgentTranscriptView::scrollToBottom() {
    impl_->pinned = true;
    QTimer::singleShot(0, this, [this] {
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
        impl_->pinned = true;
    });
}
