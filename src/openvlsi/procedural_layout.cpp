#include "procedural_layout.hpp"

#include "python_highlighter.hpp"
#include "ui_helpers.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
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

    parametersLabel_ = new QLabel(QStringLiteral("Parameters · declare them with layout.parameter(name, default)"));
    parametersLabel_->setContentsMargins(6, 0, 6, 0);
    layout->addWidget(parametersLabel_);
    parameters_ = new QTableWidget(0, 2);
    parameters_->setHorizontalHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Value")});
    parameters_->horizontalHeader()->setStretchLastSection(true);
    parameters_->verticalHeader()->setVisible(false);
    parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
    parameters_->setSelectionMode(QAbstractItemView::SingleSelection);
    parameters_->setMaximumHeight(150);
    layout->addWidget(parameters_);
    connect(parameters_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (updatingParameters_ || !item || item->column() != 1)
            return;
        auto *name = parameters_->item(item->row(), 0);
        if (name && onParameterChanged_)
            onParameterChanged_(name->text(), item->text());
    });

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

void ProceduralLayoutEditor::setParameters(const std::vector<openvlsi::Parameter> &parameters) {
    if (!parameters_)
        return;
    updatingParameters_ = true;
    const QSignalBlocker blocker(parameters_);
    parameters_->clearContents();
    parameters_->setRowCount(static_cast<int>(parameters.size()));
    for (int row = 0; row < static_cast<int>(parameters.size()); ++row) {
        const auto &parameter = parameters[static_cast<std::size_t>(row)];
        auto *name = new QTableWidgetItem(QString::fromStdString(parameter.name));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        parameters_->setItem(row, 0, name);
        parameters_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(parameter.value)));
    }
    parameters_->resizeColumnToContents(0);
    const bool visible = !parameters.empty();
    if (parametersLabel_)
        parametersLabel_->setVisible(visible);
    parameters_->setVisible(visible);
    updatingParameters_ = false;
}

void ProceduralLayoutEditor::refreshSyntaxColors() {
    if (pythonHighlighter_)
        pythonHighlighter_->refreshColors();
}

void ProceduralLayoutEditor::setOnScriptEdited(std::function<void(const QString &)> callback) {
    onScriptEdited_ = std::move(callback);
}

void ProceduralLayoutEditor::setOnParameterChanged(std::function<void(const QString &, const QString &)> callback) {
    onParameterChanged_ = std::move(callback);
}

void ProceduralLayoutEditor::setOnRegenerate(std::function<void()> callback) { onRegenerate_ = std::move(callback); }
