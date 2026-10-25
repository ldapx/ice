#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

namespace ice {

// Loads a vendored Lucide SVG icon and colorizes it for the active palette.
// Checked/On state uses the text-on-accent color so checkable toolbar buttons
// remain readable on their accent background.
QIcon lucideIcon(const QString &name, int size = 18);

// Loads the application logo. Returns an empty pixmap when the asset cannot be
// found; callers should handle that by hiding the logo widget.
QPixmap appLogo(int size = 0);

} // namespace ice
