#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

#include <array>
#include <cstddef>

namespace ice {

enum class ThemeRole {
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
};

enum class ThemePreset { Dark, Light };

struct ThemeEntry {
    ThemeRole role;
    const char *name;
    const char *label;
    const char *darkHex;
    const char *lightHex;
};

// Global application palette. Canvas widgets and custom-painted views query the
// theme while painting so changes apply immediately without recreating widgets.
class Theme {
  public:
    static constexpr std::size_t roleCount = 16;

    static Theme &instance();

    const std::array<ThemeEntry, roleCount> &entries() const;
    QColor color(ThemeRole role) const;
    QColor defaultColor(ThemeRole role) const;
    void setColor(ThemeRole role, const QColor &color);
    void resetToDefaults();

    ThemePreset preset() const { return preset_; }
    void applyPreset(ThemePreset preset);

    void load();
    void save() const;

    QPalette palette() const;
    QString styleSheet() const;
    void applyToApplication() const;

  private:
    Theme();

    ThemePreset preset_ = ThemePreset::Dark;
    std::array<QColor, roleCount> colors_{};
};

inline QColor themeColor(ThemeRole role) { return Theme::instance().color(role); }

} // namespace ice
