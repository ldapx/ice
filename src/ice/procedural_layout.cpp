#include "procedural_layout.hpp"

#include "python_highlighter.hpp"
#include "ui_helpers.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>

ProceduralLayoutEditor::ProceduralLayoutEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(6, 6, 6, 6);
    buttonRow->addStretch(1);
    regenerateButton_ = new QPushButton(QStringLiteral("Regenerate layout"));
    setLucideIcon(regenerateButton_, QStringLiteral("refresh-cw"), 16);
    buttonRow->addWidget(regenerateButton_);
    layout->addLayout(buttonRow);

    editor_ = new QPlainTextEdit;
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(std::max(font.pointSizeF(), 10.0));
    editor_->setFont(font);
    editor_->setTabStopDistance(editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    editor_->setAccessibleName(QStringLiteral("Procedural layout Python script"));
    editor_->setPlaceholderText(QStringLiteral("Write Python that builds the layout through the `layout` object."));
    pythonHighlighter_ = new PythonSyntaxHighlighter(editor_->document());
    layout->addWidget(editor_, 1);

    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        if (onScriptEdited_)
            onScriptEdited_(editor_->toPlainText());
    });
    connect(regenerateButton_, &QPushButton::clicked, this, [this] {
        if (onRegenerate_)
            onRegenerate_();
    });
}

void ProceduralLayoutEditor::setScript(const QString &script) {
    const QSignalBlocker blocker(editor_);
    editor_->setPlainText(script);
}

QString ProceduralLayoutEditor::script() const { return editor_->toPlainText(); }

void ProceduralLayoutEditor::refreshSyntaxColors() {
    if (pythonHighlighter_)
        pythonHighlighter_->refreshColors();
}

void ProceduralLayoutEditor::setOnScriptEdited(std::function<void(const QString &)> callback) {
    onScriptEdited_ = std::move(callback);
}

void ProceduralLayoutEditor::setOnRegenerate(std::function<void()> callback) { onRegenerate_ = std::move(callback); }
