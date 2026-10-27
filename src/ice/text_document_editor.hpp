#pragma once

#include <QWidget>

#include <functional>

class QLabel;
class QPlainTextEdit;

// Simple source-text editor used by document tabs that do not have a canvas.
class TextDocumentEditor : public QWidget {
  public:
    explicit TextDocumentEditor(const QString &title, const QString &hint, QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;
    void setOnTextEdited(std::function<void(const QString &)> callback);
    void showMessage(const QString &message, bool error = false);

  private:
    QPlainTextEdit *editor_ = nullptr;
    QLabel *status_ = nullptr;
    std::function<void(const QString &)> onTextEdited_;
};
