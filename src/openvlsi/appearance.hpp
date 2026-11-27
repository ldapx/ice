#pragma once

#include <QPalette>
#include <QString>

class QStyle;

namespace openvlsi {

enum class AppearanceMode { System, Light, Dark };

// Application style: Fusion plus an OpenVLSI-drawn tab close button, so document
// tabs do not fall back to the platform's oversized close cross.
QStyle *createAppStyle();

AppearanceMode loadAppearanceMode();
void saveAppearanceMode(AppearanceMode mode);
QString appearanceModeName(AppearanceMode mode);
QPalette appearancePalette(AppearanceMode mode);
void applyAppearance(AppearanceMode mode);

} // namespace openvlsi
