#include "text_document_editor.hpp"

#include "systemverilog_highlighter.hpp"

#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QTextCursor>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

TextDocumentEditor::TextDocumentEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    editor_ = new QPlainTextEdit;
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(std::max(font.pointSizeF(), 10.0));
    editor_->setFont(font);
    editor_->setTabStopDistance(editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    layout->addWidget(editor_, 1);

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (onTextEdited_)
            onTextEdited_(editor_->toPlainText());
    });
}

void TextDocumentEditor::setText(const QString &text) {
    const QSignalBlocker blocker(editor_);
    editor_->setPlainText(text);
}

QString TextDocumentEditor::text() const { return editor_->toPlainText(); }

void TextDocumentEditor::setLanguage(TextDocumentLanguage language) {
    if (language_ == language)
        return;
    language_ = language;
    if (systemVerilogHighlighter_) {
        delete systemVerilogHighlighter_;
        systemVerilogHighlighter_ = nullptr;
        // Deleting a highlighter leaves its last formats on the text. Replace
        // the document contents to clear them while preserving the selection.
        const auto contents = editor_->toPlainText();
        const auto selection = editor_->textCursor();
        const int anchor = selection.anchor();
        const int position = selection.position();
        {
            const QSignalBlocker blocker(editor_);
            editor_->setPlainText(contents);
        }
        QTextCursor cursor = editor_->textCursor();
        cursor.setPosition(anchor);
        cursor.setPosition(position, QTextCursor::KeepAnchor);
        editor_->setTextCursor(cursor);
    }
    if (language_ == TextDocumentLanguage::SystemVerilog)
        systemVerilogHighlighter_ = new SystemVerilogSyntaxHighlighter(editor_->document());
}

void TextDocumentEditor::refreshSyntaxColors() {
    if (systemVerilogHighlighter_)
        systemVerilogHighlighter_->refreshColors();
}

void TextDocumentEditor::setOnTextEdited(std::function<void(const QString &)> callback) {
    onTextEdited_ = std::move(callback);
}
