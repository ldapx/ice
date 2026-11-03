#pragma once

#include "icelib/core.hpp"

#include <QColor>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <map>
#include <vector>

class QAbstractButton;
class QAction;
class QLabel;
class QPainter;
class QPixmap;
class QRectF;

void setLucideIcon(QAction *action, const QString &name);
void setLucideIcon(QAbstractButton *button, const QString &name, int size = 18);
QLabel *heading(const QString &text);

// Suggests the next unused name for a new cell: the prefix followed by the
// smallest two-digit counter that is not in existingNames, e.g. "Schematic01",
// "Schematic02", ... Used as the default text of the create dialogs.
QString nextDefaultCellName(const QString &prefix, const QStringList &existingNames);

bool isAnalysisControlDirective(const QString &line);
QStringList parseSignalList(const QString &text);

QPointF rotatePoint(QPointF point, ice::Orientation orientation);

struct OrientationMatrix {
    int a = 1;
    int b = 0;
    int c = 0;
    int d = 1;
};

OrientationMatrix orientationMatrix(ice::Orientation orientation);
ice::Orientation composeOrientation(ice::Orientation current, ice::Orientation step);
ice::PinDirection rotateDirection(ice::PinDirection direction, ice::Orientation orientation);
QPointF directionVector(ice::PinDirection direction);

struct LayerStyle {
    QColor color;
    Qt::BrushStyle pattern = Qt::SolidPattern;
};

const std::vector<QColor> &layerPalette();
std::map<ice::ObjectId, LayerStyle> layerStylesFor(const ice::Project &project);
QPixmap layerPreviewIcon(const LayerStyle &style);
void drawCanvasAxes(QPainter &painter, const QRectF &exposed);

class WaitCursorGuard {
  public:
    WaitCursorGuard();
    ~WaitCursorGuard();
    WaitCursorGuard(const WaitCursorGuard &) = delete;
    WaitCursorGuard &operator=(const WaitCursorGuard &) = delete;
};
