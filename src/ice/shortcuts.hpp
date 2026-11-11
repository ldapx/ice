#pragma once

#include <QAction>
#include <QHash>
#include <QKeySequence>
#include <QString>
#include <QVector>

class QWidget;

namespace ice {

enum class ShortcutScope {
    global,
    schematic,
    layout,
};

struct ShortcutEntry {
    QString id;
    QString label;
    ShortcutScope scope = ShortcutScope::global;
    QKeySequence defaultSequence;
    QKeySequence sequence;
    QAction *action = nullptr;
};

// Central registry for user-editable keyboard shortcuts. Global commands use
// window shortcuts; editor tools attach to their canvas so the same key can be
// used for different tools in the schematic and layout editors.
class ShortcutRegistry {
  public:
    QAction *registerAction(QAction *action, const QString &id, const QString &label, ShortcutScope scope,
                            const QKeySequence &defaultSequence = {});
    void attachScope(ShortcutScope scope, QWidget *widget);
    bool setShortcut(const QString &id, const QKeySequence &sequence, QString *error = nullptr);
    QVector<ShortcutEntry> entries() const;

    static QString scopeLabel(ShortcutScope scope);
    static QString sequenceText(const QKeySequence &sequence);
    static QString idForText(const QString &prefix, const QString &text);

  private:
    QVector<ShortcutEntry> entries_;
    QHash<int, QWidget *> scopeWidgets_;
};

void showShortcutSettings(QWidget *parent, ShortcutRegistry &registry);

} // namespace ice
