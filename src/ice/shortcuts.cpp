#include "shortcuts.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace ice {
namespace {

QString settingsKey(const QString &id) { return QStringLiteral("shortcuts/") + id; }

int scopeValue(ShortcutScope scope) { return static_cast<int>(scope); }

} // namespace

QString ShortcutRegistry::scopeLabel(ShortcutScope scope) {
    switch (scope) {
    case ShortcutScope::global:
        return QStringLiteral("Global");
    case ShortcutScope::schematic:
        return QStringLiteral("Schematic");
    case ShortcutScope::layout:
        return QStringLiteral("Layout");
    }
    return QStringLiteral("Global");
}

QString ShortcutRegistry::sequenceText(const QKeySequence &sequence) {
    return sequence.isEmpty() ? QStringLiteral("Unassigned") : sequence.toString(QKeySequence::NativeText);
}

QString ShortcutRegistry::idForText(const QString &prefix, const QString &text) {
    QString id = prefix;
    id += QLatin1Char('.');
    bool separator = false;
    for (const auto character : text) {
        if (character.isLetterOrNumber()) {
            id += character.toLower();
            separator = false;
        } else if (!separator && !id.endsWith(QLatin1Char('.'))) {
            id += QLatin1Char('-');
            separator = true;
        }
    }
    while (id.endsWith(QLatin1Char('-')))
        id.chop(1);
    return id;
}

QAction *ShortcutRegistry::registerAction(QAction *action, const QString &id, const QString &label, ShortcutScope scope,
                                          const QKeySequence &defaultSequence) {
    if (!action || id.isEmpty())
        return action;
    if (std::any_of(entries_.begin(), entries_.end(), [&](const ShortcutEntry &entry) { return entry.id == id; }))
        return action;
    QSettings settings;
    QKeySequence sequence = defaultSequence;
    const auto key = settingsKey(id);
    if (settings.contains(key))
        sequence = QKeySequence::fromString(settings.value(key).toString(), QKeySequence::PortableText);
    action->setShortcut(sequence);
    if (scope == ShortcutScope::global)
        action->setShortcutContext(Qt::WindowShortcut);
    entries_.push_back({id, label, scope, defaultSequence, sequence, action});
    if (auto *widget = scopeWidgets_.value(scopeValue(scope))) {
        if (!widget->actions().contains(action))
            widget->addAction(action);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        action->setShortcutVisibleInContextMenu(true);
    }
    return action;
}

void ShortcutRegistry::attachScope(ShortcutScope scope, QWidget *widget) {
    scopeWidgets_.insert(scopeValue(scope), widget);
    for (auto &entry : entries_) {
        if (entry.scope != scope || !entry.action || !widget)
            continue;
        if (!widget->actions().contains(entry.action))
            widget->addAction(entry.action);
        entry.action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        entry.action->setShortcutVisibleInContextMenu(true);
    }
}

bool ShortcutRegistry::setShortcut(const QString &id, const QKeySequence &sequence, QString *error) {
    auto entry = std::find_if(entries_.begin(), entries_.end(),
                              [&](const ShortcutEntry &candidate) { return candidate.id == id; });
    if (entry == entries_.end()) {
        if (error)
            *error = QStringLiteral("Unknown shortcut: %1").arg(id);
        return false;
    }
    if (!sequence.isEmpty()) {
        for (const auto &other : entries_) {
            if (other.id == id || other.sequence.isEmpty())
                continue;
            const bool sameScope = other.scope == entry->scope;
            const bool involvesGlobal = other.scope == ShortcutScope::global || entry->scope == ShortcutScope::global;
            if (!sameScope && !involvesGlobal)
                continue;
            if (other.sequence == sequence) {
                if (error)
                    *error = QStringLiteral("\"%1\" is already assigned to %2 (%3).")
                                 .arg(sequenceText(sequence), other.label, scopeLabel(other.scope));
                return false;
            }
        }
    }
    entry->sequence = sequence;
    if (entry->action)
        entry->action->setShortcut(sequence);
    QSettings settings;
    settings.setValue(settingsKey(id), sequence.toString(QKeySequence::PortableText));
    return true;
}

QVector<ShortcutEntry> ShortcutRegistry::entries() const {
    auto result = entries_;
    std::sort(result.begin(), result.end(), [](const ShortcutEntry &left, const ShortcutEntry &right) {
        if (left.scope != right.scope)
            return static_cast<int>(left.scope) < static_cast<int>(right.scope);
        return QString::compare(left.label, right.label, Qt::CaseInsensitive) < 0;
    });
    return result;
}

void showShortcutSettings(QWidget *parent, ShortcutRegistry &registry) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Keyboard Shortcuts"));
    dialog.resize(780, 560);

    auto *layout = new QVBoxLayout(&dialog);
    auto *description =
        new QLabel(QStringLiteral("Global commands work anywhere in the window. Schematic and layout tool shortcuts "
                                  "are active while that editor canvas has focus."),
                   &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *filterRow = new QHBoxLayout;
    auto *search = new QLineEdit(&dialog);
    search->setPlaceholderText(QStringLiteral("Filter commands..."));
    search->setClearButtonEnabled(true);
    auto *scopeFilter = new QComboBox(&dialog);
    scopeFilter->addItem(QStringLiteral("All contexts"), -1);
    scopeFilter->addItem(QStringLiteral("Global"), scopeValue(ShortcutScope::global));
    scopeFilter->addItem(QStringLiteral("Schematic"), scopeValue(ShortcutScope::schematic));
    scopeFilter->addItem(QStringLiteral("Layout"), scopeValue(ShortcutScope::layout));
    filterRow->addWidget(search, 1);
    filterRow->addWidget(scopeFilter);
    layout->addLayout(filterRow);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Command"), QStringLiteral("Context"), QStringLiteral("Shortcut"), QStringLiteral("Default")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);

    auto working = registry.entries();
    const auto indexForId = [&](const QString &id) {
        for (int index = 0; index < working.size(); ++index)
            if (working[index].id == id)
                return index;
        return -1;
    };

    table->setRowCount(working.size());
    for (int row = 0; row < working.size(); ++row) {
        const auto &entry = working[row];
        auto *command = new QTableWidgetItem(entry.label);
        command->setData(Qt::UserRole, entry.id);
        table->setItem(row, 0, command);
        table->setItem(row, 1, new QTableWidgetItem(ShortcutRegistry::scopeLabel(entry.scope)));
        table->setItem(row, 2, new QTableWidgetItem(ShortcutRegistry::sequenceText(entry.sequence)));
        table->setItem(row, 3, new QTableWidgetItem(ShortcutRegistry::sequenceText(entry.defaultSequence)));
    }

    auto *editorRow = new QHBoxLayout;
    editorRow->addWidget(new QLabel(QStringLiteral("Shortcut:"), &dialog));
    auto *editor = new QKeySequenceEdit(&dialog);
    editorRow->addWidget(editor, 1);
    auto *setButton = new QPushButton(QStringLiteral("Set"), &dialog);
    auto *clearButton = new QPushButton(QStringLiteral("Clear"), &dialog);
    auto *resetButton = new QPushButton(QStringLiteral("Reset Selected"), &dialog);
    auto *resetAllButton = new QPushButton(QStringLiteral("Reset All"), &dialog);
    editorRow->addWidget(setButton);
    editorRow->addWidget(clearButton);
    editorRow->addWidget(resetButton);
    editorRow->addWidget(resetAllButton);
    layout->addLayout(editorRow);

    const auto selectedId = [&] {
        const auto row = table->currentRow();
        if (row < 0 || !table->item(row, 0))
            return QString{};
        return table->item(row, 0)->data(Qt::UserRole).toString();
    };
    const auto updateRow = [&](const QString &id, const QKeySequence &sequence) {
        const auto index = indexForId(id);
        if (index < 0)
            return;
        working[index].sequence = sequence;
        if (auto *item = table->item(index, 2))
            item->setText(ShortcutRegistry::sequenceText(sequence));
    };
    const auto updateEditor = [&] {
        const auto id = selectedId();
        editor->setKeySequence(id.isEmpty() ? QKeySequence{} : working[indexForId(id)].sequence);
    };
    const auto updateSelected = [&](const QKeySequence &sequence) {
        const auto id = selectedId();
        if (id.isEmpty())
            return;
        const auto index = indexForId(id);
        if (!sequence.isEmpty()) {
            for (int other = 0; other < working.size(); ++other) {
                if (other == index || working[other].sequence.isEmpty())
                    continue;
                const bool sameScope = working[other].scope == working[index].scope;
                const bool involvesGlobal =
                    working[other].scope == ShortcutScope::global || working[index].scope == ShortcutScope::global;
                if (!sameScope && !involvesGlobal)
                    continue;
                if (working[other].sequence == sequence) {
                    QMessageBox::warning(&dialog, QStringLiteral("Keyboard Shortcuts"),
                                         QStringLiteral("\"%1\" is already assigned to %2 (%3).")
                                             .arg(ShortcutRegistry::sequenceText(sequence), working[other].label,
                                                  ShortcutRegistry::scopeLabel(working[other].scope)));
                    return;
                }
            }
        }
        updateRow(id, sequence);
    };
    const auto resetRow = [&] {
        const auto id = selectedId();
        const auto index = indexForId(id);
        if (index >= 0)
            updateRow(id, working[index].defaultSequence);
    };

    QObject::connect(table, &QTableWidget::currentCellChanged, &dialog, [&](int, int, int, int) { updateEditor(); });
    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int, int) { editor->setFocus(); });
    QObject::connect(setButton, &QPushButton::clicked, &dialog, [&] { updateSelected(editor->keySequence()); });
    QObject::connect(clearButton, &QPushButton::clicked, &dialog, [&] { updateSelected(QKeySequence{}); });
    QObject::connect(resetButton, &QPushButton::clicked, &dialog, resetRow);
    QObject::connect(resetAllButton, &QPushButton::clicked, &dialog, [&] {
        for (int row = 0; row < working.size(); ++row)
            updateRow(working[row].id, working[row].defaultSequence);
        updateEditor();
    });

    const auto applyFilter = [&] {
        const auto needle = search->text().trimmed();
        const auto scope = scopeFilter->currentData().toInt();
        for (int row = 0; row < table->rowCount(); ++row) {
            const bool scopeMatches = scope < 0 || static_cast<int>(working[row].scope) == scope;
            const bool textMatches = needle.isEmpty() ||
                                     table->item(row, 0)->text().contains(needle, Qt::CaseInsensitive) ||
                                     table->item(row, 1)->text().contains(needle, Qt::CaseInsensitive) ||
                                     table->item(row, 2)->text().contains(needle, Qt::CaseInsensitive);
            table->setRowHidden(row, !(scopeMatches && textMatches));
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [&] { applyFilter(); });
    QObject::connect(scopeFilter, &QComboBox::currentIndexChanged, &dialog, [&] { applyFilter(); });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (const auto &entry : working) {
            QString error;
            if (!registry.setShortcut(entry.id, entry.sequence, &error)) {
                QMessageBox::warning(&dialog, QStringLiteral("Keyboard Shortcuts"), error);
                return;
            }
        }
        dialog.accept();
    });

    if (!working.isEmpty()) {
        table->selectRow(0);
        updateEditor();
    }
    applyFilter();
    dialog.exec();
}

} // namespace ice
