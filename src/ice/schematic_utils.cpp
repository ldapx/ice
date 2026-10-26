#include "schematic_utils.hpp"

#include "symbol_ui.hpp"

#include <QGraphicsItem>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <vector>

SymbolDefinition generatedSchematicSymbol(const ice::SchematicCell &cell) {
    struct Port {
        QString name;
        ice::Coordinate y = 0;
    };
    std::vector<Port> inputs;
    std::vector<Port> outputs;
    for (const auto &[_, instance] : cell.instances) {
        const auto symbol = cell.symbols.find(instance.symbol);
        if (symbol == cell.symbols.end())
            continue;
        const auto type = QString::fromStdString(symbol->second.name);
        if (type.compare(QStringLiteral("Input"), Qt::CaseInsensitive) == 0)
            inputs.push_back({QString::fromStdString(instance.name), instance.y});
        else if (type.compare(QStringLiteral("Output"), Qt::CaseInsensitive) == 0)
            outputs.push_back({QString::fromStdString(instance.name), instance.y});
    }
    const auto sortPorts = [](std::vector<Port> &ports) {
        std::sort(ports.begin(), ports.end(), [](const Port &left, const Port &right) {
            if (left.y != right.y)
                return left.y > right.y;
            return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
        });
    };
    sortPorts(inputs);
    sortPorts(outputs);

    const int portCount = static_cast<int>(std::max(inputs.size(), outputs.size()));
    const int width = 220;
    const int height = std::max(100, 70 + 30 * std::max(0, portCount - 1));
    const int boxLeft = 30;
    const int boxRight = width - 30;
    const int boxTop = 20;
    const int boxBottom = height - 20;
    const int inputPinX = 10;
    const int outputPinX = width - 10;
    const qreal centerX = width / 2.0;
    const qreal centerY = height / 2.0;
    const auto portY = [&](std::size_t index, std::size_t count) {
        if (count <= 1)
            return centerY;
        const auto spacing = (boxBottom - boxTop) / static_cast<qreal>(count + 1);
        return boxTop + spacing * static_cast<qreal>(index + 1);
    };

    QStringList svg;
    svg << QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' width='%1' height='%2' viewBox='0 0 %1 %2'>")
               .arg(width)
               .arg(height);
    svg << QStringLiteral("<rect x='%1' y='%2' width='%3' height='%4' fill='none' stroke='black' stroke-width='3'/>")
               .arg(boxLeft)
               .arg(boxTop)
               .arg(boxRight - boxLeft)
               .arg(boxBottom - boxTop);
    const auto name = QString::fromStdString(cell.name);
    const int fontSize = name.size() > 24 ? 10 : name.size() > 16 ? 12 : 14;
    svg << QStringLiteral("<text x='%1' y='%2' text-anchor='middle' font-family='sans-serif' font-size='%3' "
                          "fill='black'>%4</text>")
               .arg(centerX)
               .arg(centerY + 5)
               .arg(fontSize)
               .arg(name.toHtmlEscaped());

    SymbolDefinition definition;
    definition.name = name;
    definition.category = QStringLiteral("Schematics");
    definition.cell = cell.id;
    definition.label = ice::SymbolLabel{0, static_cast<ice::Coordinate>(height / 2 + 12)};
    definition.parameters = cell.parameters;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto y = portY(index, inputs.size());
        svg << QStringLiteral("<path d='M%1 %2 L%3 %2' fill='none' stroke='black' stroke-width='2'/>")
                   .arg(boxLeft)
                   .arg(y)
                   .arg(inputPinX);
        svg << QStringLiteral("<circle cx='%1' cy='%2' r='4' fill='black'/>").arg(inputPinX).arg(y);
        svg << QStringLiteral("<text x='%1' y='%2' text-anchor='start' font-family='sans-serif' font-size='10' "
                              "fill='black'>%3</text>")
                   .arg(boxLeft + 5)
                   .arg(y + 4)
                   .arg(inputs[index].name.toHtmlEscaped());
        ice::Pin pin;
        pin.name = inputs[index].name.toStdString();
        pin.x = static_cast<ice::Coordinate>(std::lround(inputPinX - centerX));
        pin.y = static_cast<ice::Coordinate>(std::lround(centerY - y));
        pin.direction = ice::PinDirection::left;
        definition.pins.push_back(std::move(pin));
    }
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        const auto y = portY(index, outputs.size());
        svg << QStringLiteral("<path d='M%1 %2 L%3 %2' fill='none' stroke='black' stroke-width='2'/>")
                   .arg(boxRight)
                   .arg(y)
                   .arg(outputPinX);
        svg << QStringLiteral("<circle cx='%1' cy='%2' r='4' fill='black'/>").arg(outputPinX).arg(y);
        svg << QStringLiteral("<text x='%1' y='%2' text-anchor='end' font-family='sans-serif' font-size='10' "
                              "fill='black'>%3</text>")
                   .arg(boxRight - 5)
                   .arg(y + 4)
                   .arg(outputs[index].name.toHtmlEscaped());
        ice::Pin pin;
        pin.name = outputs[index].name.toStdString();
        pin.x = static_cast<ice::Coordinate>(std::lround(outputPinX - centerX));
        pin.y = static_cast<ice::Coordinate>(std::lround(centerY - y));
        pin.direction = ice::PinDirection::right;
        definition.pins.push_back(std::move(pin));
    }
    svg << QStringLiteral("</svg>");
    definition.svg = svg.join(QLatin1Char('\n'));
    return definition;
}

QRectF symbolHighlightRect(QGraphicsItem *symbol) {
    QRectF bounds;
    for (auto *child : symbol->childItems()) {
        const auto kind = child->data(0).toString();
        if (kind != QStringLiteral("symbol-artwork") && kind != QStringLiteral("symbol-label") &&
            kind != QStringLiteral("pin"))
            continue;
        bounds |= child->mapRectToParent(child->boundingRect());
    }
    if (bounds.isNull())
        return bounds;
    return bounds.adjusted(-8, -8, 8, 8);
}

// The single properties dialog used for schematic instances. The dialog is a
// projection of a domain object: it reads and writes a plain instance struct
// and never touches the project database directly.
