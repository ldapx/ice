#pragma once

#include "icelib/procedural_layout.hpp"

#include <QWidget>

#include <functional>
#include <string>

class PythonSyntaxHighlighter;
class QPlainTextEdit;
class QPushButton;

// Text editor shown in the middle view when a procedural layout tab is active.
class ProceduralLayoutEditor : public QWidget {
  public:
    explicit ProceduralLayoutEditor(QWidget *parent = nullptr);

    void setScript(const QString &script);
    QString script() const;
    void refreshSyntaxColors();
    void setOnScriptEdited(std::function<void(const QString &)> callback);
    void setOnRegenerate(std::function<void()> callback);

  private:
    QPlainTextEdit *editor_ = nullptr;
    PythonSyntaxHighlighter *pythonHighlighter_ = nullptr;
    QPushButton *regenerateButton_ = nullptr;
    std::function<void(const QString &)> onScriptEdited_;
    std::function<void()> onRegenerate_;
};
