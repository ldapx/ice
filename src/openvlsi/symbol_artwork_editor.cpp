#include "symbol_artwork_editor.hpp"

#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPolygon>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyleOptionGraphicsItem>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

constexpr qreal kPi = 3.14159265358979323846;
constexpr int kHistoryLimit = 64;

QString formatNumber(qreal value) {
    auto text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0')))
        text.chop(1);
    if (text.endsWith(QLatin1Char('.')))
        text.chop(1);
    if (text == QStringLiteral("-0"))
        text = QStringLiteral("0");
    return text;
}

QVector<qreal> numberList(const QString &text) {
    static const QRegularExpression expression(
        QStringLiteral("[-+]?(?:[0-9]*\\.[0-9]+|[0-9]+\\.?)(?:[eE][-+]?[0-9]+)?"));
    QVector<qreal> values;
    auto iterator = expression.globalMatch(text);
    while (iterator.hasNext()) {
        bool ok = false;
        const auto value = iterator.next().captured(0).toDouble(&ok);
        if (ok)
            values.push_back(value);
    }
    return values;
}

qreal parseLength(const QString &text, qreal fallback = 0.0) {
    const auto values = numberList(text);
    return values.isEmpty() ? fallback : values.front();
}

void skipSeparators(const QString &text, qsizetype &index) {
    while (index < text.size() && (text.at(index).isSpace() || text.at(index) == QLatin1Char(',')))
        ++index;
}

bool readNumber(const QString &text, qsizetype &index, qreal &value) {
    skipSeparators(text, index);
    const auto start = index;
    if (index >= text.size())
        return false;
    if (text.at(index) == QLatin1Char('+') || text.at(index) == QLatin1Char('-'))
        ++index;
    bool digits = false;
    while (index < text.size() && text.at(index).isDigit()) {
        ++index;
        digits = true;
    }
    if (index < text.size() && text.at(index) == QLatin1Char('.')) {
        ++index;
        while (index < text.size() && text.at(index).isDigit()) {
            ++index;
            digits = true;
        }
    }
    if (!digits) {
        index = start;
        return false;
    }
    if (index < text.size() && (text.at(index) == QLatin1Char('e') || text.at(index) == QLatin1Char('E'))) {
        const auto exponent = index;
        ++index;
        if (index < text.size() && (text.at(index) == QLatin1Char('+') || text.at(index) == QLatin1Char('-')))
            ++index;
        bool exponentDigits = false;
        while (index < text.size() && text.at(index).isDigit()) {
            ++index;
            exponentDigits = true;
        }
        if (!exponentDigits)
            index = exponent;
    }
    bool ok = false;
    value = text.mid(start, index - start).toDouble(&ok);
    if (!ok)
        index = start;
    return ok;
}

void appendArc(QPainterPath &path, qreal rx, qreal ry, qreal rotation, bool largeArc, bool sweep, const QPointF &from,
               const QPointF &to) {
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx <= 0.0 || ry <= 0.0) {
        path.lineTo(to);
        return;
    }
    const auto angle = qDegreesToRadians(rotation);
    const auto cosAngle = std::cos(angle);
    const auto sinAngle = std::sin(angle);
    const auto dx = (from.x() - to.x()) / 2.0;
    const auto dy = (from.y() - to.y()) / 2.0;
    const auto x1 = cosAngle * dx + sinAngle * dy;
    const auto y1 = -sinAngle * dx + cosAngle * dy;
    const auto lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0) {
        const auto scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }
    const auto rxSquared = rx * rx;
    const auto rySquared = ry * ry;
    auto numerator = rxSquared * rySquared - rxSquared * y1 * y1 - rySquared * x1 * x1;
    numerator = std::max<qreal>(0.0, numerator);
    const auto denominator = rxSquared * y1 * y1 + rySquared * x1 * x1;
    auto factor = denominator > 0.0 ? std::sqrt(numerator / denominator) : 0.0;
    if (largeArc == sweep)
        factor = -factor;
    const auto cx1 = factor * rx * y1 / ry;
    const auto cy1 = -factor * ry * x1 / rx;
    const auto centerX = cosAngle * cx1 - sinAngle * cy1 + (from.x() + to.x()) / 2.0;
    const auto centerY = sinAngle * cx1 + cosAngle * cy1 + (from.y() + to.y()) / 2.0;

    const auto vectorAngle = [](qreal ux, qreal uy, qreal vx, qreal vy) {
        const auto dot = ux * vx + uy * vy;
        const auto lengths = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        auto result = lengths > 0.0 ? std::acos(std::clamp(dot / lengths, -1.0, 1.0)) : 0.0;
        if (ux * vy - uy * vx < 0.0)
            result = -result;
        return result;
    };

    const auto ux = (x1 - cx1) / rx;
    const auto uy = (y1 - cy1) / ry;
    const auto vx = (-x1 - cx1) / rx;
    const auto vy = (-y1 - cy1) / ry;
    const auto startAngle = vectorAngle(1.0, 0.0, ux, uy);
    auto deltaAngle = vectorAngle(ux, uy, vx, vy);
    if (!sweep && deltaAngle > 0.0)
        deltaAngle -= 2.0 * kPi;
    else if (sweep && deltaAngle < 0.0)
        deltaAngle += 2.0 * kPi;

    const auto steps = std::max(2, static_cast<int>(std::ceil(std::abs(deltaAngle) / (kPi / 16.0))));
    for (int step = 1; step <= steps; ++step) {
        const auto theta = startAngle + deltaAngle * step / steps;
        const auto x = centerX + cosAngle * rx * std::cos(theta) - sinAngle * ry * std::sin(theta);
        const auto y = centerY + sinAngle * rx * std::cos(theta) + cosAngle * ry * std::sin(theta);
        path.lineTo(x, y);
    }
}

bool parsePathData(const QString &data, QPainterPath &path, QVector<bool> &closed) {
    path = QPainterPath();
    closed.clear();
    qsizetype index = 0;
    QChar command;
    QPointF current(0.0, 0.0);
    QPointF subpathStart(0.0, 0.0);
    QPointF cubicControl;
    QPointF quadControl;
    bool lastWasCubic = false;
    bool lastWasQuad = false;

    const auto point = [&](bool relative, QPointF &out) {
        qreal x = 0.0;
        qreal y = 0.0;
        if (!readNumber(data, index, x) || !readNumber(data, index, y))
            return false;
        out = relative ? current + QPointF(x, y) : QPointF(x, y);
        return true;
    };
    const auto number = [&](qreal &out) { return readNumber(data, index, out); };

    while (true) {
        skipSeparators(data, index);
        if (index >= data.size())
            break;
        if (data.at(index).isLetter()) {
            command = data.at(index);
            ++index;
        } else if (command.isNull()) {
            return false;
        }

        switch (command.unicode()) {
        case 'M':
        case 'm': {
            QPointF next;
            if (!point(command == QLatin1Char('m'), next))
                return false;
            path.moveTo(next);
            closed.push_back(false);
            subpathStart = next;
            current = next;
            command = command == QLatin1Char('m') ? QLatin1Char('l') : QLatin1Char('L');
            lastWasCubic = false;
            lastWasQuad = false;
            break;
        }
        case 'L':
        case 'l': {
            QPointF next;
            if (!point(command == QLatin1Char('l'), next))
                return false;
            path.lineTo(next);
            current = next;
            lastWasCubic = false;
            lastWasQuad = false;
            break;
        }
        case 'H':
        case 'h': {
            qreal x = 0.0;
            if (!number(x))
                return false;
            if (command == QLatin1Char('h'))
                x += current.x();
            path.lineTo(x, current.y());
            current.setX(x);
            lastWasCubic = false;
            lastWasQuad = false;
            break;
        }
        case 'V':
        case 'v': {
            qreal y = 0.0;
            if (!number(y))
                return false;
            if (command == QLatin1Char('v'))
                y += current.y();
            path.lineTo(current.x(), y);
            current.setY(y);
            lastWasCubic = false;
            lastWasQuad = false;
            break;
        }
        case 'C':
        case 'c': {
            QPointF first;
            QPointF second;
            QPointF end;
            if (!point(command == QLatin1Char('c'), first) || !point(command == QLatin1Char('c'), second) ||
                !point(command == QLatin1Char('c'), end))
                return false;
            path.cubicTo(first, second, end);
            cubicControl = second;
            current = end;
            lastWasCubic = true;
            lastWasQuad = false;
            break;
        }
        case 'S':
        case 's': {
            QPointF second;
            QPointF end;
            if (!point(command == QLatin1Char('s'), second) || !point(command == QLatin1Char('s'), end))
                return false;
            const auto first = lastWasCubic ? current * 2.0 - cubicControl : current;
            path.cubicTo(first, second, end);
            cubicControl = second;
            current = end;
            lastWasCubic = true;
            lastWasQuad = false;
            break;
        }
        case 'Q':
        case 'q': {
            QPointF control;
            QPointF end;
            if (!point(command == QLatin1Char('q'), control) || !point(command == QLatin1Char('q'), end))
                return false;
            path.quadTo(control, end);
            quadControl = control;
            current = end;
            lastWasCubic = false;
            lastWasQuad = true;
            break;
        }
        case 'T':
        case 't': {
            QPointF end;
            if (!point(command == QLatin1Char('t'), end))
                return false;
            const auto control = lastWasQuad ? current * 2.0 - quadControl : current;
            path.quadTo(control, end);
            quadControl = control;
            current = end;
            lastWasCubic = false;
            lastWasQuad = true;
            break;
        }
        case 'A':
        case 'a': {
            qreal rx = 0.0;
            qreal ry = 0.0;
            qreal rotation = 0.0;
            qreal largeArc = 0.0;
            qreal sweep = 0.0;
            QPointF end;
            if (!number(rx) || !number(ry) || !number(rotation) || !number(largeArc) || !number(sweep) ||
                !point(command == QLatin1Char('a'), end))
                return false;
            appendArc(path, rx, ry, rotation, largeArc != 0.0, sweep != 0.0, current, end);
            current = end;
            lastWasCubic = false;
            lastWasQuad = false;
            break;
        }
        case 'Z':
        case 'z': {
            if (!closed.isEmpty())
                closed.last() = true;
            path.closeSubpath();
            current = subpathStart;
            lastWasCubic = false;
            lastWasQuad = false;
            command = QChar();
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

QColor colorFromToken(const QString &token) {
    auto text = token.trimmed();
    if (text.isEmpty() || text.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return {};
    if (text.compare(QStringLiteral("currentColor"), Qt::CaseInsensitive) == 0)
        text = QStringLiteral("#000000");
    if (text.startsWith(QStringLiteral("rgb("), Qt::CaseInsensitive) && text.endsWith(QLatin1Char(')'))) {
        const auto values = numberList(text.mid(4, text.size() - 5));
        if (values.size() >= 3) {
            const auto channel = [](qreal value) { return std::clamp(value / 255.0, 0.0, 1.0); };
            return QColor::fromRgbF(channel(values[0]), channel(values[1]), channel(values[2]));
        }
    }
    return QColor::fromString(text);
}

struct SvgStyle {
    QColor fill = QColor(Qt::black);
    QColor stroke;
    qreal strokeWidth = 1.0;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    Qt::FillRule fillRule = Qt::WindingFill;
};

void applyStyleDeclaration(SvgStyle &style, const QString &name, const QString &value) {
    const auto key = name.trimmed().toLower();
    const auto text = value.trimmed();
    if (key == QStringLiteral("fill")) {
        style.fill = colorFromToken(text);
    } else if (key == QStringLiteral("stroke")) {
        style.stroke = colorFromToken(text);
    } else if (key == QStringLiteral("stroke-width")) {
        bool ok = false;
        const auto width = text.toDouble(&ok);
        if (ok && width >= 0.0)
            style.strokeWidth = width;
    } else if (key == QStringLiteral("fill-rule")) {
        style.fillRule =
            text.compare(QStringLiteral("evenodd"), Qt::CaseInsensitive) == 0 ? Qt::OddEvenFill : Qt::WindingFill;
    } else if (key == QStringLiteral("stroke-linecap")) {
        if (text.compare(QStringLiteral("round"), Qt::CaseInsensitive) == 0)
            style.cap = Qt::RoundCap;
        else if (text.compare(QStringLiteral("square"), Qt::CaseInsensitive) == 0)
            style.cap = Qt::SquareCap;
        else
            style.cap = Qt::FlatCap;
    } else if (key == QStringLiteral("stroke-linejoin")) {
        if (text.compare(QStringLiteral("round"), Qt::CaseInsensitive) == 0)
            style.join = Qt::RoundJoin;
        else if (text.compare(QStringLiteral("bevel"), Qt::CaseInsensitive) == 0)
            style.join = Qt::BevelJoin;
        else
            style.join = Qt::MiterJoin;
    }
}

void applyStyleAttribute(SvgStyle &style, const QString &attribute) {
    for (const auto &declaration : attribute.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const auto colon = declaration.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        applyStyleDeclaration(style, declaration.left(colon), declaration.mid(colon + 1));
    }
}

QString attribute(const QXmlStreamAttributes &attributes, const char *name) {
    return attributes.value(QString::fromLatin1(name)).toString();
}

void applyStyleAttributes(SvgStyle &style, const QXmlStreamAttributes &attributes) {
    for (const auto *name : {"fill", "stroke", "stroke-width", "fill-rule", "stroke-linecap", "stroke-linejoin"}) {
        if (attributes.hasAttribute(QString::fromLatin1(name)))
            applyStyleDeclaration(style, QString::fromLatin1(name), attribute(attributes, name));
    }
    if (attributes.hasAttribute(QStringLiteral("style")))
        applyStyleAttribute(style, attribute(attributes, "style"));
}

QTransform parseTransform(const QString &text) {
    static const QRegularExpression expression(QStringLiteral("([A-Za-z]+)\\s*\\(([^)]*)\\)"));
    QTransform result;
    auto iterator = expression.globalMatch(text);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        const auto name = match.captured(1).toLower();
        const auto values = numberList(match.captured(2));
        QTransform local;
        bool known = true;
        if (name == QStringLiteral("matrix") && values.size() >= 6) {
            local = QTransform(values[0], values[1], values[2], values[3], values[4], values[5]);
        } else if (name == QStringLiteral("translate") && !values.isEmpty()) {
            local = QTransform::fromTranslate(values[0], values.value(1, 0.0));
        } else if (name == QStringLiteral("scale") && !values.isEmpty()) {
            local = QTransform::fromScale(values[0], values.value(1, values[0]));
        } else if (name == QStringLiteral("rotate") && !values.isEmpty()) {
            const auto angle = qDegreesToRadians(values[0]);
            QTransform rotation(std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0.0, 0.0);
            if (values.size() >= 3) {
                const QTransform toOrigin(1.0, 0.0, 0.0, 1.0, -values[1], -values[2]);
                const QTransform fromOrigin(1.0, 0.0, 0.0, 1.0, values[1], values[2]);
                rotation = toOrigin * rotation * fromOrigin;
            }
            local = rotation;
        } else if (name == QStringLiteral("skewx") && !values.isEmpty()) {
            local = QTransform(1.0, 0.0, std::tan(qDegreesToRadians(values[0])), 1.0, 0.0, 0.0);
        } else if (name == QStringLiteral("skewy") && !values.isEmpty()) {
            local = QTransform(1.0, std::tan(qDegreesToRadians(values[0])), 0.0, 1.0, 0.0, 0.0);
        } else {
            known = false;
        }
        if (known)
            result = local * result;
    }
    return result;
}

qreal transformScale(const QTransform &transform) {
    const auto determinant = std::abs(transform.m11() * transform.m22() - transform.m12() * transform.m21());
    return determinant > 0.0 ? std::sqrt(determinant) : 1.0;
}

bool buildShape(const QString &tag, const QXmlStreamAttributes &attributes, QPainterPath &path, QVector<bool> &closed) {
    const auto value = [&](const char *name) { return parseLength(attribute(attributes, name)); };
    if (tag == QStringLiteral("path")) {
        return parsePathData(attribute(attributes, "d"), path, closed);
    }
    if (tag == QStringLiteral("rect")) {
        const auto x = value("x");
        const auto y = value("y");
        const auto width = value("width");
        const auto height = value("height");
        if (width <= 0.0 || height <= 0.0)
            return false;
        auto rx = attribute(attributes, "rx").isEmpty() ? -1.0 : value("rx");
        auto ry = attribute(attributes, "ry").isEmpty() ? -1.0 : value("ry");
        if (rx < 0.0 && ry >= 0.0)
            rx = ry;
        if (ry < 0.0 && rx >= 0.0)
            ry = rx;
        if (rx > 0.0 || ry > 0.0) {
            path.addRoundedRect(QRectF(x, y, width, height), std::min(rx, width / 2.0), std::min(ry, height / 2.0));
        } else {
            path.addRect(QRectF(x, y, width, height));
        }
        closed = {true};
        return true;
    }
    if (tag == QStringLiteral("circle")) {
        const auto radius = value("r");
        if (radius <= 0.0)
            return false;
        path.addEllipse(QPointF(value("cx"), value("cy")), radius, radius);
        closed = {true};
        return true;
    }
    if (tag == QStringLiteral("ellipse")) {
        const auto rx = value("rx");
        const auto ry = value("ry");
        if (rx <= 0.0 || ry <= 0.0)
            return false;
        path.addEllipse(QPointF(value("cx"), value("cy")), rx, ry);
        closed = {true};
        return true;
    }
    if (tag == QStringLiteral("line")) {
        path.moveTo(value("x1"), value("y1"));
        path.lineTo(value("x2"), value("y2"));
        closed = {false};
        return true;
    }
    if (tag == QStringLiteral("polyline") || tag == QStringLiteral("polygon")) {
        const auto values = numberList(attribute(attributes, "points"));
        if (values.size() < 4)
            return false;
        path.moveTo(values[0], values[1]);
        for (qsizetype index = 2; index + 1 < values.size(); index += 2)
            path.lineTo(values[index], values[index + 1]);
        if (tag == QStringLiteral("polygon"))
            path.closeSubpath();
        closed = {tag == QStringLiteral("polygon")};
        return true;
    }
    return false;
}

bool parseViewBox(const QString &text, QRectF &viewBox) {
    const auto values = numberList(text);
    if (values.size() < 4)
        return false;
    QRectF parsed(values[0], values[1], values[2], values[3]);
    if (!parsed.isValid() || parsed.isEmpty())
        return false;
    viewBox = parsed;
    return true;
}

struct SvgContext {
    QTransform transform;
    SvgStyle style;
};

bool parseSvgDocument(const QString &svg, QRectF &viewBox, QSizeF &documentSize,
                      std::vector<SymbolArtworkShape> &shapes) {
    QXmlStreamReader reader(svg);
    QVector<SvgContext> stack;
    bool sawSvg = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::StartElement) {
            SvgContext context = stack.isEmpty() ? SvgContext{} : stack.back();
            const auto tag = reader.name().toString().toLower();
            if (tag == QStringLiteral("svg")) {
                sawSvg = true;
                if (!viewBox.isValid()) {
                    if (!parseViewBox(attribute(reader.attributes(), "viewBox"), viewBox)) {
                        const auto width = parseLength(attribute(reader.attributes(), "width"), 120.0);
                        const auto height = parseLength(attribute(reader.attributes(), "height"), 60.0);
                        viewBox = QRectF(0.0, 0.0, width > 0.0 ? width : 120.0, height > 0.0 ? height : 60.0);
                    }
                }
                const auto width = parseLength(attribute(reader.attributes(), "width"));
                const auto height = parseLength(attribute(reader.attributes(), "height"));
                documentSize = QSizeF(width > 0.0 ? width : viewBox.width(), height > 0.0 ? height : viewBox.height());
            }
            const auto local = parseTransform(attribute(reader.attributes(), "transform"));
            context.transform = local * context.transform;
            applyStyleAttributes(context.style, reader.attributes());

            QPainterPath rawPath;
            QVector<bool> closed;
            if (buildShape(tag, reader.attributes(), rawPath, closed) && rawPath.elementCount() > 0) {
                SymbolArtworkShape shape;
                shape.path = context.transform.map(rawPath);
                shape.closedSubpaths = closed;
                shape.fill = context.style.fill;
                shape.stroke = context.style.stroke;
                shape.strokeWidth = context.style.strokeWidth * transformScale(context.transform);
                shape.cap = context.style.cap;
                shape.join = context.style.join;
                shape.fillRule = context.style.fillRule;
                shapes.push_back(std::move(shape));
            }
            stack.push_back(context);
        } else if (token == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty())
                stack.pop_back();
        }
    }
    return sawSvg && !reader.hasError() && viewBox.isValid();
}

bool samePath(const QPainterPath &first, const QPainterPath &second) {
    if (first.elementCount() != second.elementCount())
        return false;
    for (int index = 0; index < first.elementCount(); ++index) {
        const auto a = first.elementAt(index);
        const auto b = second.elementAt(index);
        if (a.type != b.type || !qFuzzyCompare(a.x + 1.0, b.x + 1.0) || !qFuzzyCompare(a.y + 1.0, b.y + 1.0))
            return false;
    }
    return true;
}

bool sameShapes(const std::vector<SymbolArtworkShape> &first, const std::vector<SymbolArtworkShape> &second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const auto &a = first[index];
        const auto &b = second[index];
        if (!samePath(a.path, b.path) || a.closedSubpaths != b.closedSubpaths || a.fill != b.fill ||
            a.stroke != b.stroke || !qFuzzyCompare(a.strokeWidth + 1.0, b.strokeWidth + 1.0) || a.cap != b.cap ||
            a.join != b.join || a.fillRule != b.fillRule)
            return false;
    }
    return true;
}

QString pathDataString(const SymbolArtworkShape &shape) {
    const auto &path = shape.path;
    QString data;
    int subpath = -1;
    QPointF subpathStart;
    const auto closeSubpath = [&] {
        if (subpath >= 0 && subpath < shape.closedSubpaths.size() && shape.closedSubpaths.at(subpath))
            data += QStringLiteral("Z ");
    };
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        if (element.type == QPainterPath::MoveToElement) {
            closeSubpath();
            ++subpath;
            subpathStart = QPointF(element.x, element.y);
            data += QStringLiteral("M ") + formatNumber(element.x) + QLatin1Char(' ') + formatNumber(element.y) +
                    QLatin1Char(' ');
        } else if (element.type == QPainterPath::LineToElement) {
            // Qt paths for rectangles end with an explicit line back to the
            // start. A following Z already closes the subpath, so drop the
            // duplicate so an exported icon reloads to the same markup.
            const bool lastInSubpath =
                index + 1 >= path.elementCount() || path.elementAt(index + 1).type == QPainterPath::MoveToElement;
            const bool closesSubpath = subpath >= 0 && subpath < shape.closedSubpaths.size() &&
                                       shape.closedSubpaths.at(subpath) && lastInSubpath &&
                                       qFuzzyCompare(element.x + 1.0, subpathStart.x() + 1.0) &&
                                       qFuzzyCompare(element.y + 1.0, subpathStart.y() + 1.0);
            if (!closesSubpath)
                data += QStringLiteral("L ") + formatNumber(element.x) + QLatin1Char(' ') + formatNumber(element.y) +
                        QLatin1Char(' ');
        } else if (element.type == QPainterPath::CurveToElement) {
            if (index + 2 >= path.elementCount())
                break;
            const auto second = path.elementAt(index + 1);
            const auto end = path.elementAt(index + 2);
            data += QStringLiteral("C ") + formatNumber(element.x) + QLatin1Char(' ') + formatNumber(element.y) +
                    QLatin1Char(' ') + formatNumber(second.x) + QLatin1Char(' ') + formatNumber(second.y) +
                    QLatin1Char(' ') + formatNumber(end.x) + QLatin1Char(' ') + formatNumber(end.y) + QLatin1Char(' ');
            index += 2;
        }
    }
    closeSubpath();
    return data.trimmed();
}

QString capName(Qt::PenCapStyle cap) {
    switch (cap) {
    case Qt::RoundCap:
        return QStringLiteral("round");
    case Qt::SquareCap:
        return QStringLiteral("square");
    default:
        return QStringLiteral("butt");
    }
}

QString joinName(Qt::PenJoinStyle join) {
    switch (join) {
    case Qt::RoundJoin:
        return QStringLiteral("round");
    case Qt::BevelJoin:
        return QStringLiteral("bevel");
    default:
        return QStringLiteral("miter");
    }
}

} // namespace

SymbolArtworkItem::SymbolArtworkItem(const SymbolArtworkShape &shape) : shape_(shape) {
    setFlag(QGraphicsItem::ItemIsSelectable, true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

void SymbolArtworkItem::setShape(const SymbolArtworkShape &shape) {
    prepareGeometryChange();
    shape_ = shape;
    update();
}

QRectF SymbolArtworkItem::boundingRect() const {
    const auto strokeMargin = shape_.stroke.isValid() ? std::max<qreal>(shape_.strokeWidth, 1.0) : 1.0;
    const auto margin = strokeMargin / 2.0 + 2.0;
    return shape_.path.boundingRect().adjusted(-margin, -margin, margin, margin);
}

QPainterPath SymbolArtworkItem::shape() const {
    auto path = shape_.path;
    path.setFillRule(shape_.fillRule);
    if (!shape_.stroke.isValid())
        return path;
    QPainterPathStroker stroker;
    stroker.setWidth(std::max<qreal>(shape_.strokeWidth, 6.0));
    stroker.setCapStyle(shape_.cap);
    stroker.setJoinStyle(shape_.join);
    return stroker.createStroke(path).united(path);
}

void SymbolArtworkItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    painter->setRenderHint(QPainter::Antialiasing, true);
    auto path = shape_.path;
    path.setFillRule(shape_.fillRule);
    const QPen pen = shape_.stroke.isValid()
                         ? QPen(shape_.stroke, shape_.strokeWidth, Qt::SolidLine, shape_.cap, shape_.join)
                         : QPen(Qt::NoPen);
    painter->setPen(pen);
    painter->setBrush(shape_.fill.isValid() ? QBrush(shape_.fill) : QBrush(Qt::NoBrush));
    painter->drawPath(path);
}

void SymbolArtworkItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QGraphicsItem::mousePressEvent(event);
        return;
    }
    if (!isSelected()) {
        scene()->clearSelection();
        setSelected(true);
    }
    dragging_ = true;
    dragStart_ = event->scenePos();
    dragPath_ = shape_.path;
    event->accept();
}

void SymbolArtworkItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (!dragging_) {
        QGraphicsItem::mouseMoveEvent(event);
        return;
    }
    const auto delta = event->scenePos() - dragStart_;
    auto shape = shape_;
    shape.path = QTransform::fromTranslate(delta.x(), delta.y()).map(dragPath_);
    setShape(shape);
    event->accept();
}

void SymbolArtworkItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    dragging_ = false;
    event->accept();
}

SymbolArtworkCanvas::SymbolArtworkCanvas(QWidget *parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing, true);
    setBackgroundBrush(Qt::white);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setDragMode(QGraphicsView::RubberBandDrag);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    scene_->setSceneRect(viewBox_.adjusted(-20.0, -20.0, 20.0, 20.0));
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this] {
        viewport()->update();
        if (onSelectionChanged_)
            onSelectionChanged_();
    });
    fitDocument();
}

SymbolArtworkCanvas::~SymbolArtworkCanvas() {
    // The scene is a child of this view and is deleted while the view is
    // already partially destroyed. Clearing it emits selectionChanged, whose
    // handler touches the viewport (and the dialog that owns this canvas), so
    // detach it before teardown starts.
    onSelectionChanged_ = {};
    onHistoryChanged_ = {};
    if (scene_) {
        disconnect(scene_, nullptr, this, nullptr);
        scene_->blockSignals(true);
    }
}

void SymbolArtworkCanvas::setTool(Tool tool) {
    cancelDrawing();
    resizeItem_ = nullptr;
    resizeHandle_ = -1;
    pendingSnapshot_.reset();
    tool_ = tool;
    setDragMode(tool_ == Tool::select ? QGraphicsView::RubberBandDrag : QGraphicsView::NoDrag);
    viewport()->setCursor(tool_ == Tool::select ? Qt::ArrowCursor : Qt::CrossCursor);
    viewport()->update();
}

void SymbolArtworkCanvas::setOnSelectionChanged(std::function<void()> callback) {
    onSelectionChanged_ = std::move(callback);
}

void SymbolArtworkCanvas::setOnHistoryChanged(std::function<void()> callback) {
    onHistoryChanged_ = std::move(callback);
}

bool SymbolArtworkCanvas::loadSvg(const QString &svg) {
    QRectF box;
    QSizeF size;
    std::vector<SymbolArtworkShape> loaded;
    if (!svg.trimmed().isEmpty()) {
        if (!parseSvgDocument(svg, box, size, loaded))
            return false;
    } else {
        box = QRectF(0.0, 0.0, 120.0, 60.0);
    }
    viewBox_ = box;
    documentSize_ = size.isValid() && !size.isEmpty() ? size : box.size();
    undoStack_.clear();
    redoStack_.clear();
    pendingSnapshot_.reset();
    resizeItem_ = nullptr;
    resizeHandle_ = -1;
    restoreShapes(loaded);
    initialShapes_ = loaded;
    fitDocument();
    if (onHistoryChanged_)
        onHistoryChanged_();
    return true;
}

QString SymbolArtworkCanvas::toSvg() const {
    QStringList elements;
    for (auto *item : scene_->items(Qt::AscendingOrder)) {
        const auto *shapeItem = dynamic_cast<SymbolArtworkItem *>(item);
        if (!shapeItem)
            continue;
        const auto &shape = shapeItem->artworkShape();
        const auto data = pathDataString(shape);
        if (data.isEmpty())
            continue;
        QString element = QStringLiteral("<path d=\"") + data + QStringLiteral("\" fill=\"") +
                          (shape.fill.isValid() ? shape.fill.name(QColor::HexRgb) : QStringLiteral("none")) +
                          QStringLiteral("\" stroke=\"") +
                          (shape.stroke.isValid() ? shape.stroke.name(QColor::HexRgb) : QStringLiteral("none")) +
                          QStringLiteral("\" stroke-width=\"") + formatNumber(shape.strokeWidth) +
                          QStringLiteral("\" stroke-linecap=\"") + capName(shape.cap) +
                          QStringLiteral("\" stroke-linejoin=\"") + joinName(shape.join) +
                          QStringLiteral("\" fill-rule=\"") +
                          (shape.fillRule == Qt::OddEvenFill ? QStringLiteral("evenodd") : QStringLiteral("nonzero")) +
                          QStringLiteral("\"/>");
        elements.push_back(std::move(element));
    }
    QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"") +
                  formatNumber(documentSize_.width()) + QStringLiteral("\" height=\"") +
                  formatNumber(documentSize_.height()) + QStringLiteral("\" viewBox=\"") + formatNumber(viewBox_.x()) +
                  QLatin1Char(' ') + formatNumber(viewBox_.y()) + QLatin1Char(' ') + formatNumber(viewBox_.width()) +
                  QLatin1Char(' ') + formatNumber(viewBox_.height()) + QStringLiteral("\">");
    for (const auto &element : elements)
        svg += element;
    svg += QStringLiteral("</svg>");
    return svg;
}

std::optional<SymbolArtworkShape> SymbolArtworkCanvas::selectionShape() const {
    if (const auto *item = selectedArtworkItem())
        return item->artworkShape();
    return std::nullopt;
}

void SymbolArtworkCanvas::applyStyleToSelection(QColor stroke, QColor fill, qreal strokeWidth) {
    stroke_ = std::move(stroke);
    fill_ = std::move(fill);
    strokeWidth_ = strokeWidth;
    const auto selected = scene_->selectedItems();
    if (selected.isEmpty())
        return;
    const auto before = shapes();
    bool changed = false;
    for (auto *item : selected) {
        auto *shapeItem = dynamic_cast<SymbolArtworkItem *>(item);
        if (!shapeItem)
            continue;
        auto shape = shapeItem->artworkShape();
        if (shape.stroke == stroke_ && shape.fill == fill_ &&
            qFuzzyCompare(shape.strokeWidth + 1.0, strokeWidth_ + 1.0))
            continue;
        shape.stroke = stroke_;
        shape.fill = fill_;
        shape.strokeWidth = strokeWidth_;
        shapeItem->setShape(shape);
        changed = true;
    }
    if (changed)
        pushUndo(before);
}

void SymbolArtworkCanvas::deleteSelection() {
    const auto selected = scene_->selectedItems();
    if (selected.isEmpty())
        return;
    const auto before = shapes();
    for (auto *item : selected)
        delete item;
    pushUndo(before);
    viewport()->update();
}

void SymbolArtworkCanvas::undo() {
    if (undoStack_.empty())
        return;
    auto current = shapes();
    redoStack_.push_back(std::move(current));
    auto previous = std::move(undoStack_.back());
    undoStack_.pop_back();
    restoreShapes(previous);
    if (onHistoryChanged_)
        onHistoryChanged_();
}

void SymbolArtworkCanvas::redo() {
    if (redoStack_.empty())
        return;
    auto current = shapes();
    undoStack_.push_back(std::move(current));
    auto next = std::move(redoStack_.back());
    redoStack_.pop_back();
    restoreShapes(next);
    if (onHistoryChanged_)
        onHistoryChanged_();
}

void SymbolArtworkCanvas::showEvent(QShowEvent *event) {
    QGraphicsView::showEvent(event);
    fitDocument();
}

void SymbolArtworkCanvas::fitDocument() {
    if (!viewBox_.isValid())
        return;
    const auto margin = std::max(2.0, std::max(viewBox_.width(), viewBox_.height()) * 0.05);
    fitInView(viewBox_.adjusted(-margin, -margin, margin, margin), Qt::KeepAspectRatio);
}

void SymbolArtworkCanvas::drawBackground(QPainter *painter, const QRectF &rect) {
    painter->fillRect(rect, QColor(Qt::white));
    if (!viewBox_.isValid())
        return;
    const auto step = std::max<qreal>(
        1.0, std::pow(10.0, std::floor(std::log10(std::max(viewBox_.width(), viewBox_.height()) / 12.0))));
    painter->save();
    painter->setClipRect(viewBox_);
    painter->setPen(QPen(QColor(236, 236, 236), 0));
    for (auto x = std::floor(viewBox_.left() / step) * step; x <= viewBox_.right(); x += step)
        painter->drawLine(QPointF(x, viewBox_.top()), QPointF(x, viewBox_.bottom()));
    for (auto y = std::floor(viewBox_.top() / step) * step; y <= viewBox_.bottom(); y += step)
        painter->drawLine(QPointF(viewBox_.left(), y), QPointF(viewBox_.right(), y));
    painter->setClipping(false);
    painter->setPen(QPen(QColor(110, 110, 110), 0));
    painter->drawRect(viewBox_);
    painter->restore();
}

void SymbolArtworkCanvas::drawForeground(QPainter *painter, const QRectF &) {
    if (resizeHandle_ < 0 && tool_ != Tool::select)
        return;
    const auto box = selectionBox();
    if (!box.isValid())
        return;
    painter->save();
    painter->resetTransform();
    const auto topLeft = mapFromScene(box.topLeft());
    const auto topRight = mapFromScene(box.topRight());
    const auto bottomLeft = mapFromScene(box.bottomLeft());
    const auto bottomRight = mapFromScene(box.bottomRight());
    painter->setPen(QPen(QColor(0, 120, 215), 1, Qt::DashLine));
    painter->setBrush(Qt::NoBrush);
    QPolygon polygon;
    polygon << topLeft << topRight << bottomRight << bottomLeft;
    painter->drawPolygon(polygon);
    if (box.width() > 0.001 && box.height() > 0.001) {
        painter->setPen(QPen(QColor(0, 120, 215), 1));
        painter->setBrush(Qt::white);
        for (const auto &corner : {topLeft, topRight, bottomLeft, bottomRight})
            painter->drawRect(QRectF(corner.x() - 4, corner.y() - 4, 8, 8));
    }
    painter->restore();
}

void SymbolArtworkCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && spaceDown_)) {
        panning_ = true;
        panStart_ = event->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }
    if (tool_ == Tool::select) {
        pendingSnapshot_ = shapes();
        if (auto *item = selectedArtworkItem()) {
            const auto handle = handleAtViewport(event->pos());
            if (handle >= 0) {
                beginResize(handle, item);
                event->accept();
                return;
            }
        }
        QGraphicsView::mousePressEvent(event);
        return;
    }

    pendingSnapshot_ = shapes();
    drawing_ = true;
    drawStart_ = mapToScene(event->pos());
    pencilPoints_.clear();
    if (tool_ == Tool::pencil)
        pencilPoints_.push_back(drawStart_);
    previewItem_ = new QGraphicsPathItem;
    previewItem_->setPen(QPen(QColor(0, 120, 215), 0, Qt::DashLine));
    scene_->addItem(previewItem_);
    event->accept();
}

void SymbolArtworkCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (panning_) {
        const auto delta = event->pos() - panStart_;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        panStart_ = event->pos();
        return;
    }
    if (resizeItem_) {
        updateResize(mapToScene(event->pos()));
        return;
    }
    if (drawing_) {
        const auto scenePosition = mapToScene(event->pos());
        if (tool_ == Tool::pencil && (pencilPoints_.isEmpty() ||
                                      QLineF(pencilPoints_.back(), scenePosition).length() >
                                          std::max<qreal>(0.5, std::max(viewBox_.width(), viewBox_.height()) * 0.002)))
            pencilPoints_.push_back(scenePosition);
        if (previewItem_)
            previewItem_->setPath(drawingPath(scenePosition));
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void SymbolArtworkCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (panning_) {
        panning_ = false;
        viewport()->setCursor(spaceDown_ ? Qt::OpenHandCursor
                                         : (tool_ == Tool::select ? Qt::ArrowCursor : Qt::CrossCursor));
        return;
    }
    if (resizeItem_ && event->button() == Qt::LeftButton) {
        resizeItem_ = nullptr;
        resizeHandle_ = -1;
        if (pendingSnapshot_) {
            auto before = std::move(*pendingSnapshot_);
            pendingSnapshot_.reset();
            if (!sameShapes(before, shapes()))
                pushUndo(before);
        }
        viewport()->update();
        return;
    }
    if (drawing_ && event->button() == Qt::LeftButton) {
        const auto position = mapToScene(event->pos());
        const auto path = drawingPath(position);
        const auto kind = tool_;
        cancelDrawing();
        const auto bounds = path.boundingRect();
        if (bounds.width() + bounds.height() > 0.5) {
            QVector<bool> closed;
            if (kind == Tool::rectangle || kind == Tool::ellipse)
                closed = {true};
            else
                closed = {false};
            auto before = pendingSnapshot_ ? std::move(*pendingSnapshot_) : shapes();
            pendingSnapshot_.reset();
            addShape(currentShape(path, closed), before);
        } else {
            pendingSnapshot_.reset();
        }
        return;
    }
    if (tool_ == Tool::select) {
        QGraphicsView::mouseReleaseEvent(event);
        if (pendingSnapshot_) {
            auto before = std::move(*pendingSnapshot_);
            pendingSnapshot_.reset();
            if (!sameShapes(before, shapes()))
                pushUndo(before);
        }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void SymbolArtworkCanvas::wheelEvent(QWheelEvent *event) {
    if (event->angleDelta().y() == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    const auto factor = std::pow(1.0015, event->angleDelta().y());
    const auto current = transform().m11();
    if ((current < 0.02 && factor < 1.0) || (current > 100.0 && factor > 1.0))
        return;
    scale(factor, factor);
}

void SymbolArtworkCanvas::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        deleteSelection();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (drawing_ || resizeItem_) {
            cancelDrawing();
            resizeItem_ = nullptr;
            resizeHandle_ = -1;
            pendingSnapshot_.reset();
            viewport()->update();
        } else {
            scene_->clearSelection();
        }
        return;
    }
    if (event->key() == Qt::Key_Space) {
        spaceDown_ = true;
        if (tool_ == Tool::select)
            viewport()->setCursor(Qt::OpenHandCursor);
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        if (event->key() == Qt::Key_Z) {
            if (event->modifiers().testFlag(Qt::ShiftModifier))
                redo();
            else
                undo();
            return;
        }
        if (event->key() == Qt::Key_Y) {
            redo();
            return;
        }
    }
    QGraphicsView::keyPressEvent(event);
}

void SymbolArtworkCanvas::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space) {
        spaceDown_ = false;
        if (!panning_)
            viewport()->setCursor(tool_ == Tool::select ? Qt::ArrowCursor : Qt::CrossCursor);
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}

bool SymbolArtworkCanvas::dirty() const { return !sameShapes(initialShapes_, shapes()); }

SymbolArtworkCanvas::ShapeList SymbolArtworkCanvas::shapes() const {
    ShapeList result;
    for (auto *item : scene_->items(Qt::AscendingOrder)) {
        if (const auto *shapeItem = dynamic_cast<SymbolArtworkItem *>(item))
            result.push_back(shapeItem->artworkShape());
    }
    return result;
}

void SymbolArtworkCanvas::restoreShapes(const ShapeList &shapes) {
    previewItem_ = nullptr;
    resizeItem_ = nullptr;
    resizeHandle_ = -1;
    scene_->clear();
    for (const auto &shape : shapes) {
        auto *item = new SymbolArtworkItem(shape);
        scene_->addItem(item);
    }
    const auto margin = std::max(10.0, std::max(viewBox_.width(), viewBox_.height()) * 0.15);
    scene_->setSceneRect(viewBox_.adjusted(-margin, -margin, margin, margin));
    viewport()->update();
    if (onSelectionChanged_)
        onSelectionChanged_();
}

void SymbolArtworkCanvas::pushUndo(const ShapeList &before) {
    undoStack_.push_back(before);
    if (undoStack_.size() > kHistoryLimit)
        undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
    if (onHistoryChanged_)
        onHistoryChanged_();
}

void SymbolArtworkCanvas::addShape(SymbolArtworkShape shape, const ShapeList &before) {
    auto *item = new SymbolArtworkItem(shape);
    scene_->addItem(item);
    scene_->clearSelection();
    item->setSelected(true);
    pushUndo(before);
    viewport()->update();
}

void SymbolArtworkCanvas::cancelDrawing() {
    drawing_ = false;
    pencilPoints_.clear();
    if (previewItem_) {
        delete previewItem_;
        previewItem_ = nullptr;
    }
}

QPainterPath SymbolArtworkCanvas::drawingPath(const QPointF &scenePosition) const {
    QPainterPath path;
    if (tool_ == Tool::rectangle) {
        path.addRect(QRectF(drawStart_, scenePosition).normalized());
    } else if (tool_ == Tool::ellipse) {
        path.addEllipse(QRectF(drawStart_, scenePosition).normalized());
    } else if (tool_ == Tool::line) {
        path.moveTo(drawStart_);
        path.lineTo(scenePosition);
    } else if (tool_ == Tool::pencil) {
        if (pencilPoints_.isEmpty())
            return path;
        path.moveTo(pencilPoints_.front());
        for (qsizetype index = 1; index < pencilPoints_.size(); ++index)
            path.lineTo(pencilPoints_.at(index));
        if (QLineF(pencilPoints_.back(), scenePosition).length() > 0.01)
            path.lineTo(scenePosition);
    }
    return path;
}

SymbolArtworkShape SymbolArtworkCanvas::currentShape(const QPainterPath &path, const QVector<bool> &closed) const {
    SymbolArtworkShape shape;
    shape.path = path;
    shape.closedSubpaths = closed;
    shape.fill = fill_;
    shape.stroke = stroke_;
    shape.strokeWidth = strokeWidth_;
    shape.cap = Qt::FlatCap;
    shape.join = Qt::MiterJoin;
    shape.fillRule = Qt::WindingFill;
    return shape;
}

SymbolArtworkItem *SymbolArtworkCanvas::selectedArtworkItem() const {
    const auto selected = scene_->selectedItems();
    if (selected.size() != 1)
        return nullptr;
    return dynamic_cast<SymbolArtworkItem *>(selected.front());
}

QRectF SymbolArtworkCanvas::selectionBox() const {
    const auto *item = selectedArtworkItem();
    if (!item)
        return {};
    return item->artworkShape().path.boundingRect();
}

int SymbolArtworkCanvas::handleAtViewport(const QPoint &position) const {
    const auto box = selectionBox();
    if (!box.isValid() || box.width() <= 0.001 || box.height() <= 0.001)
        return -1;
    const QPoint corners[] = {mapFromScene(box.topLeft()), mapFromScene(box.topRight()), mapFromScene(box.bottomLeft()),
                              mapFromScene(box.bottomRight())};
    for (int index = 0; index < 4; ++index)
        if ((position - corners[index]).manhattanLength() <= 10)
            return index;
    return -1;
}

void SymbolArtworkCanvas::beginResize(int handle, SymbolArtworkItem *item) {
    resizeItem_ = item;
    resizeHandle_ = handle;
    resizeStartPath_ = item->artworkShape().path;
    resizeStartBox_ = resizeStartPath_.boundingRect();
    switch (handle) {
    case 0:
        resizeAnchor_ = resizeStartBox_.bottomRight();
        break;
    case 1:
        resizeAnchor_ = resizeStartBox_.bottomLeft();
        break;
    case 2:
        resizeAnchor_ = resizeStartBox_.topRight();
        break;
    default:
        resizeAnchor_ = resizeStartBox_.topLeft();
        break;
    }
    pendingSnapshot_ = shapes();
}

void SymbolArtworkCanvas::updateResize(const QPointF &scenePosition) {
    if (!resizeItem_ || resizeStartBox_.width() <= 0.0 || resizeStartBox_.height() <= 0.0)
        return;
    const auto box = QRectF(resizeAnchor_, scenePosition).normalized();
    if (box.width() < 0.2 || box.height() < 0.2)
        return;
    QTransform transform;
    transform.translate(box.x(), box.y());
    transform.scale(box.width() / resizeStartBox_.width(), box.height() / resizeStartBox_.height());
    transform.translate(-resizeStartBox_.x(), -resizeStartBox_.y());
    auto shape = resizeItem_->artworkShape();
    shape.path = transform.map(resizeStartPath_);
    resizeItem_->setShape(shape);
    viewport()->update();
}

SymbolArtworkEditor::SymbolArtworkEditor(QWidget *parent) : QWidget(parent) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    toolbar_ = new QToolBar(QStringLiteral("Icon tools"));
    toolbar_->setOrientation(Qt::Vertical);
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(18, 18));
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setAccessibleName(QStringLiteral("Icon editor toolbar"));
    tools_ = new QActionGroup(this);
    tools_->setExclusive(true);

    const auto addTool = [this](const QString &icon, const QString &tip, SymbolArtworkCanvas::Tool tool) {
        auto *action = toolbar_->addAction(tip);
        setLucideIcon(action, icon);
        action->setToolTip(tip);
        action->setCheckable(true);
        tools_->addAction(action);
        connect(action, &QAction::triggered, this, [this, tool] { canvas_->setTool(tool); });
        return action;
    };
    selectAction_ = addTool(QStringLiteral("mouse-pointer-2"), QStringLiteral("Select and move shapes"),
                            SymbolArtworkCanvas::Tool::select);
    selectAction_->setChecked(true);
    addTool(QStringLiteral("square"), QStringLiteral("Draw rectangle"), SymbolArtworkCanvas::Tool::rectangle);
    addTool(QStringLiteral("circle-dot"), QStringLiteral("Draw ellipse"), SymbolArtworkCanvas::Tool::ellipse);
    addTool(QStringLiteral("minus"), QStringLiteral("Draw line"), SymbolArtworkCanvas::Tool::line);
    addTool(QStringLiteral("pencil"), QStringLiteral("Draw freehand"), SymbolArtworkCanvas::Tool::pencil);
    toolbar_->addSeparator();

    undoAction_ = toolbar_->addAction(QStringLiteral("Undo"));
    undoAction_->setToolTip(QStringLiteral("Undo"));
    setLucideIcon(undoAction_, QStringLiteral("rotate-ccw"));
    redoAction_ = toolbar_->addAction(QStringLiteral("Redo"));
    redoAction_->setToolTip(QStringLiteral("Redo"));
    setLucideIcon(redoAction_, QStringLiteral("rotate-cw"));
    auto *deleteAction = toolbar_->addAction(QStringLiteral("Delete selected shapes"));
    deleteAction->setToolTip(QStringLiteral("Delete selected shapes"));
    setLucideIcon(deleteAction, QStringLiteral("trash"));
    auto *fitAction = toolbar_->addAction(QStringLiteral("Fit the icon in the view"));
    fitAction->setToolTip(QStringLiteral("Fit the icon in the view"));
    setLucideIcon(fitAction, QStringLiteral("scan"));
    // Match the schematic editor's vertical tool columns: the tools sit
    // centered in their column instead of stretching the full height.
    auto *toolColumn = new QWidget;
    auto *toolColumnLayout = new QVBoxLayout(toolColumn);
    toolColumnLayout->setContentsMargins(0, 0, 0, 0);
    toolColumnLayout->addStretch(1);
    toolColumnLayout->addWidget(toolbar_, 0, Qt::AlignHCenter);
    toolColumnLayout->addStretch(1);
    layout->addWidget(toolColumn, 0);

    canvas_ = new SymbolArtworkCanvas;
    layout->addWidget(canvas_, 1);

    auto *side = new QWidget;
    side->setMinimumWidth(220);
    side->setMaximumWidth(320);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 8, 8, 8);
    sideLayout->setSpacing(8);
    title_ = heading(QStringLiteral("Icon"));
    sideLayout->addWidget(title_);

    auto *styleGroup = new QGroupBox(QStringLiteral("Style"));
    auto *styleLayout = new QGridLayout(styleGroup);
    strokeCheck_ = new QCheckBox(QStringLiteral("Stroke"));
    strokeCheck_->setChecked(true);
    strokeButton_ = new QPushButton;
    strokeButton_->setFixedSize(48, 20);
    strokeButton_->setToolTip(QStringLiteral("Stroke color"));
    strokeButton_->setAccessibleName(QStringLiteral("Stroke color"));
    widthSpin_ = new QDoubleSpinBox;
    widthSpin_->setRange(0.05, 1000.0);
    widthSpin_->setDecimals(2);
    widthSpin_->setSingleStep(0.25);
    widthSpin_->setValue(2.0);
    widthSpin_->setKeyboardTracking(false);
    fillCheck_ = new QCheckBox(QStringLiteral("Fill"));
    fillButton_ = new QPushButton;
    fillButton_->setFixedSize(48, 20);
    fillButton_->setToolTip(QStringLiteral("Fill color"));
    fillButton_->setAccessibleName(QStringLiteral("Fill color"));
    styleLayout->addWidget(strokeCheck_, 0, 0);
    styleLayout->addWidget(strokeButton_, 0, 1);
    styleLayout->addWidget(new QLabel(QStringLiteral("Width")), 1, 0);
    styleLayout->addWidget(widthSpin_, 1, 1);
    styleLayout->addWidget(fillCheck_, 2, 0);
    styleLayout->addWidget(fillButton_, 2, 1);
    sideLayout->addWidget(styleGroup);

    auto *fileGroup = new QGroupBox(QStringLiteral("Icon file"));
    auto *fileLayout = new QVBoxLayout(fileGroup);
    auto *loadButton = new QPushButton(QStringLiteral("Load SVG…"));
    auto *saveButton = new QPushButton(QStringLiteral("Save SVG…"));
    fileLayout->addWidget(loadButton);
    fileLayout->addWidget(saveButton);
    sideLayout->addWidget(fileGroup);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(openvlsi::uiColor(openvlsi::UiColor::Text2).name()));
    sideLayout->addWidget(status_);
    sideLayout->addStretch(1);

    auto *buttonRow = new QHBoxLayout;
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"));
    applyButton_ = new QPushButton(QStringLiteral("Apply"));
    applyButton_->setDefault(true);
    applyButton_->setEnabled(false);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(applyButton_, 1);
    sideLayout->addLayout(buttonRow);
    layout->addWidget(side, 0);

    canvas_->setOnSelectionChanged([this] { refreshStyleFromSelection(); });
    canvas_->setOnHistoryChanged([this] { refreshHistoryButtons(); });
    connect(undoAction_, &QAction::triggered, this, [this] { canvas_->undo(); });
    connect(redoAction_, &QAction::triggered, this, [this] { canvas_->redo(); });
    connect(deleteAction, &QAction::triggered, this, [this] { canvas_->deleteSelection(); });
    connect(fitAction, &QAction::triggered, this, [this] { canvas_->fitDocument(); });
    connect(loadButton, &QPushButton::clicked, this, [this] { loadFromFile(); });
    connect(saveButton, &QPushButton::clicked, this, [this] { saveToFile(); });
    connect(applyButton_, &QPushButton::clicked, this, [this] {
        if (onApply_)
            onApply_();
    });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (onCancel_)
            onCancel_();
    });
    connect(strokeCheck_, &QCheckBox::toggled, this, [this] { applyStyleControls(); });
    connect(fillCheck_, &QCheckBox::toggled, this, [this] { applyStyleControls(); });
    connect(widthSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { applyStyleControls(); });
    connect(strokeButton_, &QPushButton::clicked, this, [this] {
        const auto color = QColorDialog::getColor(strokeColor_, this, QStringLiteral("Stroke Color"));
        if (!color.isValid())
            return;
        strokeColor_ = color;
        strokeCheck_->setChecked(true);
        updateSwatches();
        applyStyleControls();
    });
    connect(fillButton_, &QPushButton::clicked, this, [this] {
        const auto color = QColorDialog::getColor(fillColor_, this, QStringLiteral("Fill Color"));
        if (!color.isValid())
            return;
        fillColor_ = color;
        fillCheck_->setChecked(true);
        updateSwatches();
        applyStyleControls();
    });

    updateSwatches();
    refreshHistoryButtons();
}

void SymbolArtworkEditor::load(const QString &symbolName, const QString &currentSvg) {
    symbolName_ = symbolName;
    originalSvg_ = currentSvg;
    baselineSvg_ = currentSvg;
    title_->setText(QStringLiteral("Icon — %1").arg(symbolName));
    status_->setText(QStringLiteral(
        "Drag to draw. Select a shape to move it, or drag a corner to resize. Wheel zooms, middle button or "
        "space+drag pans."));
    canvas_->setTool(SymbolArtworkCanvas::Tool::select);
    if (selectAction_)
        selectAction_->setChecked(true);
    // Reset first so a shared editor never keeps shapes from a previous icon.
    canvas_->loadSvg(QString());
    if (!currentSvg.trimmed().isEmpty() && !canvas_->loadSvg(currentSvg)) {
        status_->setText(QStringLiteral("The current icon could not be read as SVG; starting from a blank canvas."));
    } else if (!currentSvg.trimmed().isEmpty() && canvas_->documentEmpty()) {
        status_->setText(QStringLiteral(
            "The current icon has no editable shapes. Load another SVG or draw a new one before applying."));
    }
    refreshHistoryButtons();
    refreshStyleFromSelection();
}

QString SymbolArtworkEditor::artwork() const { return canvas_->dirty() ? canvas_->toSvg() : baselineSvg_; }

bool SymbolArtworkEditor::documentEmpty() const { return canvas_->documentEmpty(); }

// Loading an SVG file makes the editor dirty relative to the project icon even
// though the canvas itself has no edits yet.
bool SymbolArtworkEditor::dirty() const { return canvas_->dirty() || baselineSvg_ != originalSvg_; }

void SymbolArtworkEditor::setOnApply(std::function<void()> callback) { onApply_ = std::move(callback); }

void SymbolArtworkEditor::setOnCancel(std::function<void()> callback) { onCancel_ = std::move(callback); }

void SymbolArtworkEditor::loadFromFile() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose SVG Icon"), QString(),
                                                   QStringLiteral("SVG images (*.svg);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Load SVG"), QStringLiteral("Could not read %1.").arg(path));
        return;
    }
    if (!canvas_->loadSvg(QString::fromUtf8(file.readAll()))) {
        QMessageBox::warning(this, QStringLiteral("Load SVG"),
                             QStringLiteral("%1 is not a readable SVG document.").arg(path));
        return;
    }
    baselineSvg_ = canvas_->toSvg();
    status_->setText(QStringLiteral("Loaded %1.").arg(QFileInfo(path).fileName()));
    refreshHistoryButtons();
}

void SymbolArtworkEditor::saveToFile() {
    auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Save SVG Icon"), QStringLiteral("icon.svg"),
                                             QStringLiteral("SVG images (*.svg)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive))
        path += QStringLiteral(".svg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Save SVG"), QStringLiteral("Could not write %1.").arg(path));
        return;
    }
    file.write(canvas_->toSvg().toUtf8());
    status_->setText(QStringLiteral("Saved %1.").arg(QFileInfo(path).fileName()));
}

void SymbolArtworkEditor::applyStyleControls() {
    if (updatingStyle_ || !canvas_)
        return;
    canvas_->applyStyleToSelection(strokeCheck_->isChecked() ? strokeColor_ : QColor(),
                                   fillCheck_->isChecked() ? fillColor_ : QColor(), widthSpin_->value());
}

void SymbolArtworkEditor::refreshStyleFromSelection() {
    const auto shape = canvas_->selectionShape();
    if (!shape)
        return;
    updatingStyle_ = true;
    if (shape->stroke.isValid())
        strokeColor_ = shape->stroke;
    if (shape->fill.isValid())
        fillColor_ = shape->fill;
    strokeCheck_->setChecked(shape->stroke.isValid());
    fillCheck_->setChecked(shape->fill.isValid());
    {
        QSignalBlocker blocker(widthSpin_);
        widthSpin_->setValue(std::clamp(shape->strokeWidth, widthSpin_->minimum(), widthSpin_->maximum()));
    }
    updateSwatches();
    updatingStyle_ = false;
}

void SymbolArtworkEditor::refreshHistoryButtons() {
    if (!canvas_)
        return;
    undoAction_->setEnabled(canvas_->canUndo());
    redoAction_->setEnabled(canvas_->canRedo());
    if (applyButton_)
        applyButton_->setEnabled(!canvas_->documentEmpty());
}

void SymbolArtworkEditor::updateSwatches() {
    const auto style = [](QPushButton *button, const QColor &color) {
        if (!color.isValid()) {
            button->setStyleSheet(QStringLiteral("background: transparent; border: 1px dashed #888;"));
            return;
        }
        button->setStyleSheet(
            QStringLiteral("background: %1; border: 1px solid #555;").arg(color.name(QColor::HexRgb)));
    };
    style(strokeButton_, strokeCheck_->isChecked() ? strokeColor_ : QColor());
    style(fillButton_, fillCheck_->isChecked() ? fillColor_ : QColor());
}
