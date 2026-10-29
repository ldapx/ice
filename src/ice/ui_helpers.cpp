#include "ui_helpers.hpp"

#include "icons.hpp"
#include "ui_colors.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <cstddef>

void setLucideIcon(QAction *action, const QString &name) {
    action->setProperty("lucideIcon", name);
    action->setIcon(ice::lucideIcon(name));
}

void setLucideIcon(QAbstractButton *button, const QString &name, int size) {
    button->setProperty("lucideIcon", name);
    button->setIcon(ice::lucideIcon(name, size));
    button->setIconSize(QSize(size, size));
}

QLabel *heading(const QString &text) {
    auto *label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("font-weight: bold; padding: 4px 2px;"));
    return label;
}

bool isAnalysisControlDirective(const QString &line) {
    return line.startsWith(QLatin1Char('.')) && (line.startsWith(QStringLiteral(".tran"), Qt::CaseInsensitive) ||
                                                 line.startsWith(QStringLiteral(".dc"), Qt::CaseInsensitive) ||
                                                 line.startsWith(QStringLiteral(".ac"), Qt::CaseInsensitive) ||
                                                 line.startsWith(QStringLiteral(".op"), Qt::CaseInsensitive) ||
                                                 line.startsWith(QStringLiteral(".end"), Qt::CaseInsensitive));
}

QStringList parseSignalList(const QString &text) {
    QStringList signalNames;
    for (const auto &part : text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts)) {
        const auto signal = part.trimmed();
        if (signal.isEmpty() || signalNames.contains(signal, Qt::CaseInsensitive))
            continue;
        signalNames << signal;
    }
    return signalNames;
}

QPointF rotatePoint(QPointF point, ice::Orientation orientation) {
    switch (orientation) {
    case ice::Orientation::r90:
        return {-point.y(), point.x()};
    case ice::Orientation::r180:
        return {-point.x(), -point.y()};
    case ice::Orientation::r270:
        return {point.y(), -point.x()};
    case ice::Orientation::mx:
        return {point.x(), -point.y()};
    case ice::Orientation::my:
        return {-point.x(), point.y()};
    case ice::Orientation::m45:
        return {point.y(), point.x()};
    case ice::Orientation::m135:
        return {-point.y(), -point.x()};
    case ice::Orientation::r0:
        return point;
    }
    return point;
}

OrientationMatrix orientationMatrix(ice::Orientation orientation) {
    switch (orientation) {
    case ice::Orientation::r0:
        return {1, 0, 0, 1};
    case ice::Orientation::r90:
        return {0, -1, 1, 0};
    case ice::Orientation::r180:
        return {-1, 0, 0, -1};
    case ice::Orientation::r270:
        return {0, 1, -1, 0};
    case ice::Orientation::mx:
        return {1, 0, 0, -1};
    case ice::Orientation::my:
        return {-1, 0, 0, 1};
    case ice::Orientation::m45:
        return {0, 1, 1, 0};
    case ice::Orientation::m135:
        return {0, -1, -1, 0};
    }
    return {1, 0, 0, 1};
}

ice::Orientation composeOrientation(ice::Orientation current, ice::Orientation step) {
    const auto lhs = orientationMatrix(step);
    const auto rhs = orientationMatrix(current);
    const auto product = OrientationMatrix{lhs.a * rhs.a + lhs.b * rhs.c, lhs.a * rhs.b + lhs.b * rhs.d,
                                           lhs.c * rhs.a + lhs.d * rhs.c, lhs.c * rhs.b + lhs.d * rhs.d};
    const ice::Orientation orientations[] = {ice::Orientation::r0,   ice::Orientation::r90, ice::Orientation::r180,
                                             ice::Orientation::r270, ice::Orientation::mx,  ice::Orientation::my,
                                             ice::Orientation::m45,  ice::Orientation::m135};
    for (const auto orientation : orientations) {
        const auto candidate = orientationMatrix(orientation);
        if (candidate.a == product.a && candidate.b == product.b && candidate.c == product.c &&
            candidate.d == product.d)
            return orientation;
    }
    return ice::Orientation::r0;
}

ice::PinDirection rotateDirection(ice::PinDirection direction, ice::Orientation orientation) {
    // Pin positions are projected into Qt scene coordinates (positive Y down)
    // before the symbol transform is applied. Direction vectors must follow
    // the same path or rotated symbols report the opposite vertical side.
    const auto rotated = rotatePoint(directionVector(direction), orientation);
    if (std::abs(rotated.x()) > std::abs(rotated.y()))
        return rotated.x() < 0 ? ice::PinDirection::left : ice::PinDirection::right;
    return rotated.y() < 0 ? ice::PinDirection::up : ice::PinDirection::down;
}

QPointF directionVector(ice::PinDirection direction) {
    switch (direction) {
    case ice::PinDirection::left:
        return {-1, 0};
    case ice::PinDirection::right:
        return {1, 0};
    case ice::PinDirection::up:
        return {0, -1};
    case ice::PinDirection::down:
        return {0, 1};
    }
    return {1, 0};
}

const std::vector<QColor> &layerPalette() {
    static const std::vector<QColor> palette = {
        QColor("#3366cc"), QColor("#cc0000"), QColor("#109618"), QColor("#ff9900"),
        QColor("#990099"), QColor("#0099c6"), QColor("#dd4477"), QColor("#66aa00"),
        QColor("#b82e2e"), QColor("#316395"), QColor("#994499"), QColor("#22aa99"),
    };
    return palette;
}

std::map<ice::ObjectId, LayerStyle> layerStylesFor(const ice::Project &project) {
    static const std::vector<Qt::BrushStyle> patterns = {
        Qt::SolidPattern, Qt::BDiagPattern, Qt::FDiagPattern,  Qt::DiagCrossPattern, Qt::HorPattern,
        Qt::VerPattern,   Qt::CrossPattern, Qt::Dense4Pattern, Qt::Dense5Pattern,
    };
    std::map<ice::ObjectId, LayerStyle> styles;
    std::size_t index = 0;
    for (const auto &[id, layer] : project.layers) {
        if (!layer.visible) {
            styles.emplace(id, LayerStyle{QColor(130, 130, 130), Qt::NoBrush});
        } else {
            const auto &palette = layerPalette();
            const QColor color = palette[index % palette.size()];
            const Qt::BrushStyle pattern = patterns[(index / palette.size()) % patterns.size()];
            styles.emplace(id, LayerStyle{color, pattern});
        }
        ++index;
    }
    return styles;
}

QPixmap layerPreviewIcon(const LayerStyle &style) {
    QPixmap pixmap(36, 18);
    pixmap.fill(ice::uiColor(ice::UiColor::Background2));
    QPainter painter(&pixmap);
    painter.setPen(QPen(style.color, 1));
    if (style.pattern != Qt::NoBrush)
        painter.setBrush(QBrush(style.color, style.pattern));
    else
        painter.setBrush(Qt::NoBrush);
    painter.drawRect(1, 1, 33, 15);
    return pixmap;
}

void drawCanvasAxes(QPainter &painter, const QRectF &exposed) {
    QPen axisPen(ice::uiColor(ice::UiColor::GridAxis));
    axisPen.setCosmetic(true);
    axisPen.setWidthF(1.2);
    painter.setPen(axisPen);
    painter.drawLine(QPointF(exposed.left(), 0.0), QPointF(exposed.right(), 0.0));
    painter.drawLine(QPointF(0.0, exposed.top()), QPointF(0.0, exposed.bottom()));
}

WaitCursorGuard::WaitCursorGuard() { QApplication::setOverrideCursor(Qt::WaitCursor); }
WaitCursorGuard::~WaitCursorGuard() { QApplication::restoreOverrideCursor(); }
