#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class QTextDocument;

// Lightweight Python highlighter for project report source files.
class PythonSyntaxHighlighter : public QSyntaxHighlighter {
  public:
    explicit PythonSyntaxHighlighter(QTextDocument *document);

    // Rebuilds formats from the current Qt palette and rehighlights the document.
    void refreshColors();

  protected:
    void highlightBlock(const QString &text) override;

  private:
    void highlightCode(const QString &text, int offset);

    QTextCharFormat keywordFormat_;
    QTextCharFormat builtinFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat decoratorFormat_;
    QTextCharFormat definitionFormat_;
};
