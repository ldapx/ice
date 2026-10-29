#include "python_highlighter.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QRegularExpression>
#include <QTextDocument>

#include <utility>

namespace {

QTextCharFormat makeFormat(const QColor &color, bool italic = false, bool bold = false) {
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontItalic(italic);
    if (bold)
        format.setFontWeight(QFont::DemiBold);
    return format;
}

const QRegularExpression &keywordPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(?:False|None|True|and|as|assert|async|await|break|case|class|continue|def|del|elif|else|except|"
        "finally|for|from|global|if|import|in|is|lambda|match|nonlocal|not|or|pass|raise|return|try|while|"
        "with|yield)\\b"));
    return pattern;
}

const QRegularExpression &builtinPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(?:self|cls|abs|all|any|bool|bytes|callable|dict|dir|enumerate|filter|float|format|frozenset|"
        "getattr|globals|hasattr|hash|help|hex|id|input|int|isinstance|issubclass|iter|len|list|locals|map|"
        "max|min|next|object|oct|open|ord|pow|print|property|range|repr|reversed|round|set|setattr|slice|"
        "sorted|staticmethod|str|sum|super|tuple|type|vars|zip)\\b"));
    return pattern;
}

const QRegularExpression &numberPattern() {
    static const QRegularExpression pattern(QStringLiteral("\\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|(?:\\d["
                                                           "\\d_]*)(?:\\.\\d[\\d_]*)?(?:[eE][+-]?\\d[\\d_]*)?j?)\\b"));
    return pattern;
}

const QRegularExpression &decoratorPattern() {
    static const QRegularExpression pattern(QStringLiteral("@[A-Za-z_][\\w.]*"));
    return pattern;
}

const QRegularExpression &definitionPattern() {
    static const QRegularExpression pattern(QStringLiteral("\\b(?:def|class)\\s+([A-Za-z_]\\w*)"));
    return pattern;
}

} // namespace

PythonSyntaxHighlighter::PythonSyntaxHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
    refreshColors();
}

void PythonSyntaxHighlighter::refreshColors() {
    const auto palette = QApplication::palette();
    const bool dark = palette.color(QPalette::Window).lightness() < 128;

    keywordFormat_ = makeFormat(QColor(dark ? QStringLiteral("#c678dd") : QStringLiteral("#a626a4")), false, true);
    builtinFormat_ = makeFormat(QColor(dark ? QStringLiteral("#61afef") : QStringLiteral("#005cc5")));
    numberFormat_ = makeFormat(QColor(dark ? QStringLiteral("#d19a66") : QStringLiteral("#b76b01")));
    stringFormat_ = makeFormat(QColor(dark ? QStringLiteral("#98c379") : QStringLiteral("#22863a")));
    commentFormat_ = makeFormat(QColor(dark ? QStringLiteral("#7f848e") : QStringLiteral("#6a737d")), true);
    decoratorFormat_ = makeFormat(QColor(dark ? QStringLiteral("#e5c07b") : QStringLiteral("#795e26")));
    definitionFormat_ = makeFormat(QColor(dark ? QStringLiteral("#61afef") : QStringLiteral("#6f42c1")), false, true);
    rehighlight();
}

void PythonSyntaxHighlighter::highlightCode(const QString &text, int offset) {
    const auto apply = [&](const QRegularExpression &pattern, const QTextCharFormat &format, int group = 0) {
        auto matches = pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.hasMatch())
                setFormat(offset + match.capturedStart(group), match.capturedLength(group), format);
        }
    };

    apply(decoratorPattern(), decoratorFormat_);
    apply(keywordPattern(), keywordFormat_);
    apply(builtinPattern(), builtinFormat_);
    apply(numberPattern(), numberFormat_);
    apply(definitionPattern(), definitionFormat_, 1);
}

void PythonSyntaxHighlighter::highlightBlock(const QString &text) {
    const int length = text.size();
    int index = 0;
    int state = previousBlockState();
    if (state != 1 && state != 2)
        state = 0;

    if (state != 0) {
        const QString delimiter = state == 1 ? QStringLiteral("\"\"\"") : QStringLiteral("'''");
        const auto end = text.indexOf(delimiter);
        if (end < 0) {
            setFormat(0, length, stringFormat_);
            setCurrentBlockState(state);
            return;
        }
        setFormat(0, end + delimiter.size(), stringFormat_);
        index = end + delimiter.size();
        state = 0;
    }
    setCurrentBlockState(state);

    int codeStart = index;
    const auto flushCode = [&](int end) {
        if (end > codeStart)
            highlightCode(text.mid(codeStart, end - codeStart), codeStart);
    };

    while (index < length) {
        const QChar character = text.at(index);
        if (character == QLatin1Char('#')) {
            flushCode(index);
            setFormat(index, length - index, commentFormat_);
            return;
        }
        if (character != QLatin1Char('"') && character != QLatin1Char('\'')) {
            ++index;
            continue;
        }

        flushCode(index);
        const QChar quote = character;
        const QString triple = QString(quote) + quote + quote;
        if (index + 2 < length && text.at(index + 1) == quote && text.at(index + 2) == quote) {
            const auto end = text.indexOf(triple, index + 3);
            if (end < 0) {
                setFormat(index, length - index, stringFormat_);
                setCurrentBlockState(quote == QLatin1Char('"') ? 1 : 2);
                return;
            }
            setFormat(index, end - index + triple.size(), stringFormat_);
            index = end + triple.size();
            codeStart = index;
            continue;
        }

        int end = index + 1;
        bool closed = false;
        while (end < length) {
            if (text.at(end) == QLatin1Char('\\')) {
                end += 2;
                continue;
            }
            if (text.at(end) == quote) {
                ++end;
                closed = true;
                break;
            }
            ++end;
        }
        if (!closed)
            end = length;
        setFormat(index, end - index, stringFormat_);
        index = end;
        codeStart = index;
    }

    flushCode(length);
}
