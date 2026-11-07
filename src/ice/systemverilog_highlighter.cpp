#include "systemverilog_highlighter.hpp"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QRegularExpression>
#include <QTextDocument>

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
        "\\b(?:accept_on|alias|always|always_comb|always_ff|always_latch|and|assert|assign|assume|automatic|"
        "before|begin|bind|binsof|break|buf|bufif0|bufif1|case|casex|casez|class|clocking|const|constraint|"
        "context|continue|cover|covergroup|coverpoint|cross|deassign|default|defparam|disable|dist|do|edge|else|"
        "end|endcase|endclass|endclocking|endfunction|endgenerate|endgroup|endinterface|endmodule|endpackage|"
        "endprogram|endproperty|endsequence|endtask|eventually|expect|export|extends|extern|final|first_match|"
        "for|force|foreach|forever|fork|forkjoin|function|generate|genvar|if|iff|ignore_bins|illegal_bins|"
        "implements|implies|import|initial|inout|input|inside|interface|intersect|join|join_any|join_none|let|"
        "local|localparam|macromodule|matches|modport|module|nand|negedge|new|nexttime|nor|not|null|or|output|"
        "package|parameter|posedge|priority|program|property|protected|pure|rand|randc|randcase|randsequence|ref|"
        "reject_on|release|repeat|restrict|return|s_always|s_eventually|s_nexttime|s_until|s_until_with|sequence|"
        "solve|static|struct|super|sync_accept_on|sync_reject_on|tagged|task|this|throughout|type|typedef|union|"
        "unique|unique0|until|until_with|var|virtual|void|wait|wait_order|while|wildcard|with|within)\\b"));
    return pattern;
}

const QRegularExpression &typePattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(?:bit|byte|chandle|event|int|integer|interconnect|logic|longint|mailbox|nettype|process|real|"
        "realtime|reg|scalared|semaphore|shortint|shortreal|signed|string|supply0|supply1|time|tri|tri0|tri1|"
        "triand|trior|trireg|unsigned|uwire|vectored|wand|wire|wor)\\b"));
    return pattern;
}

const QRegularExpression &numberPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("(?:\\b\\d[\\d_]*'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ?_]+)|(?<![\\w'])'[01xXzZ](?![\\w'])|"
                       "(?:\\b\\d[\\d_]*(?:\\.\\d[\\d_]*)?(?:[eE][+-]?\\d+)?(?:fs|ps|ns|us|ms|s)?\\b)"));
    return pattern;
}

const QRegularExpression &preprocessorPattern() {
    static const QRegularExpression pattern(QStringLiteral("`[A-Za-z_]\\w*"));
    return pattern;
}

const QRegularExpression &systemTaskPattern() {
    static const QRegularExpression pattern(QStringLiteral("\\$[A-Za-z_]\\w*"));
    return pattern;
}

const QRegularExpression &definitionPattern() {
    static const QRegularExpression pattern(QStringLiteral(
        "\\b(?:module|macromodule|interface|package|program|primitive|class|covergroup|property|sequence)\\s+"
        "([A-Za-z_]\\w*)"));
    return pattern;
}

const QRegularExpression &routineDefinitionPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("\\b(?:function|task)\\b[^;\\n]*?\\b([A-Za-z_]\\w*)\\s*(?=\\(|;)"));
    return pattern;
}

} // namespace

SystemVerilogSyntaxHighlighter::SystemVerilogSyntaxHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {
    refreshColors();
}

void SystemVerilogSyntaxHighlighter::refreshColors() {
    const auto palette = QApplication::palette();
    const bool dark = palette.color(QPalette::Window).lightness() < 128;

    keywordFormat_ = makeFormat(QColor(dark ? QStringLiteral("#c678dd") : QStringLiteral("#a626a4")), false, true);
    typeFormat_ = makeFormat(QColor(dark ? QStringLiteral("#56b6c2") : QStringLiteral("#0086b3")));
    numberFormat_ = makeFormat(QColor(dark ? QStringLiteral("#d19a66") : QStringLiteral("#b76b01")));
    stringFormat_ = makeFormat(QColor(dark ? QStringLiteral("#98c379") : QStringLiteral("#22863a")));
    commentFormat_ = makeFormat(QColor(dark ? QStringLiteral("#7f848e") : QStringLiteral("#6a737d")), true);
    preprocessorFormat_ = makeFormat(QColor(dark ? QStringLiteral("#e5c07b") : QStringLiteral("#795e26")));
    systemTaskFormat_ = makeFormat(QColor(dark ? QStringLiteral("#61afef") : QStringLiteral("#005cc5")));
    definitionFormat_ = makeFormat(QColor(dark ? QStringLiteral("#61afef") : QStringLiteral("#6f42c1")), false, true);
    rehighlight();
}

void SystemVerilogSyntaxHighlighter::highlightCode(const QString &text, int offset) {
    const auto apply = [&](const QRegularExpression &pattern, const QTextCharFormat &format, int group = 0) {
        auto matches = pattern.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.hasMatch())
                setFormat(offset + match.capturedStart(group), match.capturedLength(group), format);
        }
    };

    apply(preprocessorPattern(), preprocessorFormat_);
    apply(definitionPattern(), definitionFormat_, 1);
    apply(routineDefinitionPattern(), definitionFormat_, 1);
    apply(keywordPattern(), keywordFormat_);
    apply(typePattern(), typeFormat_);
    apply(systemTaskPattern(), systemTaskFormat_);
    apply(numberPattern(), numberFormat_);
}

void SystemVerilogSyntaxHighlighter::highlightBlock(const QString &text) {
    const int length = text.size();
    int index = 0;
    int state = previousBlockState();
    if (state != 0 && state != 1)
        state = 0;

    if (state == 1) {
        const auto end = text.indexOf(QStringLiteral("*/"));
        if (end < 0) {
            setFormat(0, length, commentFormat_);
            setCurrentBlockState(1);
            return;
        }
        setFormat(0, end + 2, commentFormat_);
        index = end + 2;
    }
    setCurrentBlockState(0);

    int codeStart = index;
    const auto flushCode = [&](int end) {
        if (end > codeStart)
            highlightCode(text.mid(codeStart, end - codeStart), codeStart);
    };

    while (index < length) {
        if (text.at(index) == QLatin1Char('/') && index + 1 < length) {
            const QChar next = text.at(index + 1);
            if (next == QLatin1Char('/')) {
                flushCode(index);
                setFormat(index, length - index, commentFormat_);
                return;
            }
            if (next == QLatin1Char('*')) {
                flushCode(index);
                const auto end = text.indexOf(QStringLiteral("*/"), index + 2);
                if (end < 0) {
                    setFormat(index, length - index, commentFormat_);
                    setCurrentBlockState(1);
                    return;
                }
                setFormat(index, end + 2 - index, commentFormat_);
                index = end + 2;
                codeStart = index;
                continue;
            }
        }
        if (text.at(index) == QLatin1Char('"')) {
            flushCode(index);
            int end = index + 1;
            bool closed = false;
            while (end < length) {
                if (text.at(end) == QLatin1Char('\\')) {
                    end += 2;
                    continue;
                }
                if (text.at(end) == QLatin1Char('"')) {
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
            continue;
        }
        ++index;
    }

    flushCode(length);
}
