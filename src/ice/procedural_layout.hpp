#pragma once

#include "icelib/procedural_layout.hpp"

#include <QWidget>

#include <functional>
#include <string>

class PythonSyntaxHighlighter;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

// Text editor shown in the middle view when a procedural layout tab is active.
class ProceduralLayoutEditor : public QWidget {
  public:
    explicit ProceduralLayoutEditor(QWidget *parent = nullptr);

    void setScript(const QString &script);
    QString script() const;
    void setParameters(const std::vector<ice::Parameter> &parameters);
    void refreshSyntaxColors();
    void setOnScriptEdited(std::function<void(const QString &)> callback);
    void setOnParameterChanged(std::function<void(const QString &, const QString &)> callback);
    void setOnRegenerate(std::function<void()> callback);

  private:
    QLabel *parametersLabel_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QPlainTextEdit *editor_ = nullptr;
    PythonSyntaxHighlighter *pythonHighlighter_ = nullptr;
    QPushButton *regenerateButton_ = nullptr;
    bool updatingParameters_ = false;
    std::function<void(const QString &)> onScriptEdited_;
    std::function<void(const QString &, const QString &)> onParameterChanged_;
    std::function<void()> onRegenerate_;
};
