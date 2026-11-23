#include "appearance.hpp"

#include "ui_colors.hpp"

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QProxyStyle>
#include <QSettings>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOption>

#include <algorithm>

namespace ice {
namespace {

// Closes the tab with the same restrained X that the lucide icon set uses,
// tinted from the active palette instead of the style's stock cross.
class IceStyle : public QProxyStyle {
  public:
    explicit IceStyle(const QString &baseKey) : QProxyStyle(baseKey) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element != PE_IndicatorTabClose) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        if (!option || !painter)
            return;
        const auto color = uiColor((option->state & State_MouseOver) ? UiColor::Text1 : UiColor::Text2);
        const auto side = std::min(option->rect.width(), option->rect.height());
        const auto inset = std::max(3, side / 4);
        const auto rect = option->rect.adjusted(inset, inset, -inset, -inset);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        pen.setWidthF(1.3);
        pen.setCapStyle(Qt::RoundCap);
        painter->setPen(pen);
        painter->drawLine(rect.topLeft(), rect.bottomRight());
        painter->drawLine(rect.topRight(), rect.bottomLeft());
        painter->restore();
    }
};

constexpr auto kAppearanceKey = "appearance/mode";

QPalette lightPalette() { return QApplication::style()->standardPalette(); }

QPalette darkPalette() {
    auto palette = lightPalette();
    const QColor window(QStringLiteral("#353535"));
    const QColor base(QStringLiteral("#2a2a2a"));
    const QColor button(QStringLiteral("#3c3c3c"));
    const QColor text(QStringLiteral("#f0f0f0"));
    const QColor disabled(QStringLiteral("#8a8a8a"));

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#333333")));
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#9f9f9f")));
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ff6b6b")));
    palette.setColor(QPalette::Light, QColor(QStringLiteral("#6a6a6a")));
    palette.setColor(QPalette::Midlight, QColor(QStringLiteral("#555555")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#4a4a4a")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#222222")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#151515")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#2a82da")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#4fa3ff")));
    palette.setColor(QPalette::LinkVisited, QColor(QStringLiteral("#b08cff")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#505050")));
    return palette;
}

QPalette systemPalette() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark ? darkPalette() : lightPalette();
#else
    const auto current = QGuiApplication::palette();
    return current.color(QPalette::Window).lightness() < 128 ? darkPalette() : lightPalette();
#endif
}

} // namespace

QStyle *createAppStyle() { return new IceStyle(QStringLiteral("Fusion")); }

AppearanceMode loadAppearanceMode() {
    QSettings settings;
    const auto value = settings.value(QString::fromLatin1(kAppearanceKey), QStringLiteral("system")).toString();
    if (value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0)
        return AppearanceMode::Light;
    if (value.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0)
        return AppearanceMode::Dark;
    return AppearanceMode::System;
}

void saveAppearanceMode(AppearanceMode mode) {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kAppearanceKey), appearanceModeName(mode));
}

QString appearanceModeName(AppearanceMode mode) {
    switch (mode) {
    case AppearanceMode::Light:
        return QStringLiteral("light");
    case AppearanceMode::Dark:
        return QStringLiteral("dark");
    case AppearanceMode::System:
        break;
    }
    return QStringLiteral("system");
}

QPalette appearancePalette(AppearanceMode mode) {
    switch (mode) {
    case AppearanceMode::Light:
        return lightPalette();
    case AppearanceMode::Dark:
        return darkPalette();
    case AppearanceMode::System:
        return systemPalette();
    }
    return lightPalette();
}

void applyAppearance(AppearanceMode mode) {
    if (!qApp)
        return;
    qApp->setPalette(appearancePalette(mode));
}

} // namespace ice
