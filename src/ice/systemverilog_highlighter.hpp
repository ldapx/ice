#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QTextDocument;

// Lightweight SystemVerilog highlighter for source cells and testbenches.
class SystemVerilogSyntaxHighlighter : public QSyntaxHighlighter {
  public:
    explicit SystemVerilogSyntaxHighlighter(QTextDocument *document);

    // Rebuilds formats from the current Qt palette and rehighlights the document.
    void refreshColors();

  protected:
    void highlightBlock(const QString &text) override;

  private:
    void highlightCode(const QString &text, int offset);

    QTextCharFormat keywordFormat_;
    QTextCharFormat typeFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat preprocessorFormat_;
    QTextCharFormat systemTaskFormat_;
    QTextCharFormat definitionFormat_;
};
