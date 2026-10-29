#include "text_document_editor.hpp"

#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QSignalBlocker>
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

void TextDocumentEditor::setOnTextEdited(std::function<void(const QString &)> callback) {
    onTextEdited_ = std::move(callback);
}
