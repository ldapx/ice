#include "text_document_editor.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

TextDocumentEditor::TextDocumentEditor(const QString &title, const QString &hint, QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);

    layout->addWidget(heading(title));

    auto *hintLabel = new QLabel(hint);
    hintLabel->setWordWrap(true);
    hintLabel->setStyleSheet(QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
    layout->addWidget(hintLabel);

    editor_ = new QPlainTextEdit;
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(std::max(font.pointSizeF(), 10.0));
    editor_->setFont(font);
    editor_->setTabStopDistance(editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    layout->addWidget(editor_, 1);

    status_ = new QLabel;
    status_->setWordWrap(true);
    layout->addWidget(status_);

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

void TextDocumentEditor::showMessage(const QString &message, bool error) {
    status_->setText(message);
    status_->setStyleSheet(error ? QStringLiteral("color: #d9534f; font-weight: bold;")
                                 : QStringLiteral("color: %1;").arg(ice::uiColor(ice::UiColor::Text2).name()));
}
