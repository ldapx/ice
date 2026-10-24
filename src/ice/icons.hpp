#pragma once

#include <QIcon>
#include <QString>

namespace ice {

// Loads a vendored Lucide SVG icon and colorizes it for the active theme.
// Checked/On state uses the text-on-accent color so checkable toolbar buttons
// remain readable on their accent background.
QIcon lucideIcon(const QString &name, int size = 18);

} // namespace ice
