#pragma once

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QTextCharFormat>

inline bool darkSyntaxPalette() { return QApplication::palette().color(QPalette::Window).lightness() < 128; }

inline QTextCharFormat makeSyntaxFormat(const QColor &color, bool italic = false, bool bold = false) {
    QTextCharFormat format;
    format.setForeground(color);
    format.setFontItalic(italic);
    if (bold)
        format.setFontWeight(QFont::DemiBold);
    return format;
}
