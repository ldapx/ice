#pragma once

#include <QPalette>
#include <QString>

namespace ice {

enum class AppearanceMode { System, Light, Dark };

AppearanceMode loadAppearanceMode();
void saveAppearanceMode(AppearanceMode mode);
QString appearanceModeName(AppearanceMode mode);
QPalette appearancePalette(AppearanceMode mode);
void applyAppearance(AppearanceMode mode);

} // namespace ice
