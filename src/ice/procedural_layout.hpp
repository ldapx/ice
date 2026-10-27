#pragma once

#include "icelib/core.hpp"

#include <QString>
#include <QWidget>

#include <functional>
#include <string>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace ice {

// A small starter script that demonstrates the layout builder API.
std::string defaultProceduralLayoutScript();

// Runs the procedural layout's Python script and replaces the contents of its
// generated static layout cell. Returns false and sets error on failure; the
// project is left unchanged when generation fails.
bool regenerateProceduralLayout(Project &project, ObjectId proceduralLayoutId, QString *error);

} // namespace ice

// Text editor shown in the middle view when a procedural layout tab is active.
class ProceduralLayoutEditor : public QWidget {
  public:
    explicit ProceduralLayoutEditor(QWidget *parent = nullptr);

    void setScript(const QString &script);
    QString script() const;
    void setOnScriptEdited(std::function<void(const QString &)> callback);
    void setOnRegenerate(std::function<void()> callback);
    void showMessage(const QString &message, bool error = false);

  private:
    QPlainTextEdit *editor_ = nullptr;
    QPushButton *regenerateButton_ = nullptr;
    QLabel *status_ = nullptr;
    std::function<void(const QString &)> onScriptEdited_;
    std::function<void()> onRegenerate_;
};
