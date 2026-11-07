#pragma once

#include <QWidget>

#include <functional>

class QPlainTextEdit;
class SystemVerilogSyntaxHighlighter;

enum class TextDocumentLanguage { PlainText, Spice, SystemVerilog };

// Minimal source-text editor used by document tabs that do not have a canvas.
class TextDocumentEditor : public QWidget {
  public:
    explicit TextDocumentEditor(QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;
    void setLanguage(TextDocumentLanguage language);
    void refreshSyntaxColors();
    void setOnTextEdited(std::function<void(const QString &)> callback);

  private:
    QPlainTextEdit *editor_ = nullptr;
    TextDocumentLanguage language_ = TextDocumentLanguage::PlainText;
    SystemVerilogSyntaxHighlighter *systemVerilogHighlighter_ = nullptr;
    std::function<void(const QString &)> onTextEdited_;
};
