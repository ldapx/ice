#pragma once

#include <QApplication>
#include <QColor>
#include <QPalette>

namespace ice {

// Semantic colors derived from the active Qt palette. The application uses the
// standard Fusion style, so custom-painted canvases follow the same colors as
// the surrounding widgets without a separate theme configuration.
enum class UiColor {
    Accent1 = 0,
    Accent2,
    Accent3,
    Accent4,
    Accent5,
    Accent6,
    Background1,
    Background2,
    Surface1,
    Border1,
    Text1,
    Text2,
    TextOnAccent,
    Grid1,
    Grid2,
    Axis1,
    GridAxis,
};

inline QColor uiColor(UiColor role) {
    const auto palette = QApplication::palette();
    const auto highlight = palette.color(QPalette::Highlight);
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    const auto highlightVariant = [&](int hueOffset) {
        const int hue = highlight.hslHue();
        if (hue < 0)
            return highlight;
        return QColor::fromHsl((hue + hueOffset) % 360, highlight.hslSaturation(), highlight.lightness());
    };
    const auto blend = [](const QColor &foreground, const QColor &background, qreal amount) {
        return QColor::fromRgbF(foreground.redF() * amount + background.redF() * (1.0 - amount),
                                foreground.greenF() * amount + background.greenF() * (1.0 - amount),
                                foreground.blueF() * amount + background.blueF() * (1.0 - amount));
    };
    const auto grid1 = palette.color(QPalette::Midlight);
    // The 5-unit grid is intentionally close to the 1-unit grid; separation
    // comes primarily from line width rather than large contrast.
    const auto grid2 = blend(palette.color(dark ? QPalette::Light : QPalette::Dark), grid1, 0.25);
    // Canvas axes are nearly the same tone as the 5-unit grid and rely on
    // their slightly heavier stroke for separation.
    const auto gridAxis = blend(grid2, palette.color(QPalette::WindowText), 0.85);

    switch (role) {
    case UiColor::Accent1:
        return highlight;
    case UiColor::Accent2:
        return highlightVariant(120);
    case UiColor::Accent3:
        return highlightVariant(60);
    case UiColor::Accent4:
        return highlight;
    case UiColor::Accent5:
        return highlightVariant(180);
    case UiColor::Accent6:
        return highlightVariant(240);
    case UiColor::Background1:
    case UiColor::Background2:
        return palette.color(QPalette::Base);
    case UiColor::Surface1:
        return palette.color(QPalette::Base);
    case UiColor::Border1:
        return palette.color(QPalette::Mid);
    case UiColor::Text1:
        return palette.color(QPalette::WindowText);
    case UiColor::Text2:
        return blend(palette.color(QPalette::WindowText), palette.color(QPalette::Window), 0.55);
    case UiColor::TextOnAccent:
        return palette.color(QPalette::HighlightedText);
    case UiColor::Grid1:
        return grid1;
    case UiColor::Grid2:
        return grid2;
    case UiColor::Axis1:
        return palette.color(QPalette::WindowText);
    case UiColor::GridAxis:
        return gridAxis;
    }
    return palette.color(QPalette::WindowText);
}

} // namespace ice
