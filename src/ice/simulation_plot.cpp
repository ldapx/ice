#include "simulation_plot.hpp"

#include "ui_colors.hpp"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QString>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {

std::string normalizedSignalName(const std::string &signal) {
    std::string name;
    name.reserve(signal.size());
    for (const auto character : signal) {
        if (!std::isspace(static_cast<unsigned char>(character)))
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (name.size() > 3 && name.rfind("v(", 0) == 0 && name.back() == ')')
        name = name.substr(2, name.size() - 3);
    return name;
}

bool isTimeSignal(const std::string &normalized) {
    return normalized.empty() || normalized == "time" || normalized == "time(s)" || normalized == "t";
}

// Chooses a 1/2/5 x 10^n step so tick labels stay readable.
double niceTickStep(double range, int intervals) {
    if (!(range > 0.0) || !std::isfinite(range) || intervals <= 0)
        return 1.0;
    const double rough = range / static_cast<double>(intervals);
    const double magnitude = std::pow(10.0, std::floor(std::log10(rough)));
    const double normalized = rough / magnitude;
    double nice = 10.0;
    if (normalized <= 1.0)
        nice = 1.0;
    else if (normalized <= 2.0)
        nice = 2.0;
    else if (normalized <= 5.0)
        nice = 5.0;
    return nice * magnitude;
}

std::vector<double> axisTicks(double minimum, double maximum, int intervals) {
    std::vector<double> ticks;
    if (!(maximum > minimum) || !std::isfinite(minimum) || !std::isfinite(maximum) || intervals <= 0)
        return ticks;
    const double step = niceTickStep(maximum - minimum, intervals);
    const auto firstIndex = static_cast<long long>(std::ceil(minimum / step));
    for (long long index = firstIndex;; ++index) {
        const double value = static_cast<double>(index) * step;
        if (value > maximum + step * 1e-6)
            break;
        ticks.push_back(std::abs(value) < step * 1e-9 ? 0.0 : value);
        if (ticks.size() > 64)
            break;
    }
    return ticks;
}

QString formatTick(double value) { return QString::number(value, 'g', 5); }

} // namespace

SimulationPlot::SimulationPlot(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(260);
    setAutoFillBackground(true);
    setToolTip(QStringLiteral("Simulation traces"));
}

void SimulationPlot::setResult(const ice::SimulationResult &result) {
    result_ = result;
    update();
}

void SimulationPlot::setGraph(const ice::SimulationGraph &graph) {
    graph_ = graph;
    update();
}

void SimulationPlot::setGridVisible(bool visible) {
    gridVisible_ = visible;
    update();
}

void SimulationPlot::clear() {
    result_ = {};
    update();
}

const std::vector<double> *SimulationPlot::valuesForSignal(const std::string &signal) const {
    const auto normalized = normalizedSignalName(signal);
    if (normalized.empty())
        return nullptr;
    if (isTimeSignal(normalized))
        return &result_.time;
    for (const auto &trace : result_.traces) {
        if (normalizedSignalName(trace.name) == normalized)
            return &trace.values;
    }
    return nullptr;
}

void SimulationPlot::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    // Match the schematic/layout canvas instead of using a dark plot.
    const auto canvasBackground = ice::uiColor(ice::UiColor::Background2);
    const auto plotFrame = ice::uiColor(ice::UiColor::Axis1);
    const auto axisText = ice::uiColor(ice::UiColor::Axis1);
    const auto tickText = ice::uiColor(ice::UiColor::Axis1);
    auto zeroLine = ice::uiColor(ice::UiColor::Axis1);
    zeroLine.setAlpha(110);
    painter.fillRect(rect(), canvasBackground);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = rect().adjusted(58, 18, -20, -42);
    // Inset the trace area so a constant trace exactly at the minimum or
    // maximum is fully visible instead of being clipped by the plot border.
    const QRectF traceArea = plot.adjusted(2, 2, -2, -2);
    painter.setPen(QPen(plotFrame, 1));
    painter.drawRect(plot);
    const QString title = graph_.name.empty() ? (result_.traces.empty() ? QStringLiteral("No simulation trace")
                                                                        : QStringLiteral("Simulation traces"))
                                              : QString::fromStdString(graph_.name);

    // Resolve the selected axes to result vectors. Time is always available;
    // a missing signal falls back to time on X and the first returned trace on
    // Y, which keeps older projects readable after signals are renamed.
    std::string effectiveX = graph_.xAxis;
    const auto *xValues = valuesForSignal(effectiveX);
    if (!xValues || xValues->empty()) {
        effectiveX = "time";
        xValues = &result_.time;
    }
    std::string effectiveY = graph_.yAxis;
    const auto *yValues = valuesForSignal(effectiveY);
    if (!yValues || yValues->empty()) {
        if (!result_.traces.empty()) {
            effectiveY = result_.traces.front().name;
            yValues = &result_.traces.front().values;
        } else {
            effectiveY = "time";
            yValues = &result_.time;
        }
    }
    const auto axisLabel = [](const std::string &signal) {
        return isTimeSignal(normalizedSignalName(signal)) ? QStringLiteral("Time (s)") : QString::fromStdString(signal);
    };
    const QString xLabel = axisLabel(effectiveX);
    const QString yLabel = axisLabel(effectiveY);

    painter.setPen(QPen(axisText, 1));
    painter.drawText(QRectF(8, 2, width() - 16, 20), Qt::AlignCenter, title);
    painter.drawText(QRectF(58, plot.top() - 16, 200, 14), Qt::AlignLeft, yLabel);
    painter.drawText(QRectF(plot.right() - 200, plot.bottom() + 24, 200, 14), Qt::AlignRight, xLabel);
    painter.setPen(QPen(tickText, 1));
    const auto count = xValues && yValues ? std::min(xValues->size(), yValues->size()) : std::size_t{0};
    if (count == 0)
        return;

    double dataXMin = std::numeric_limits<double>::max();
    double dataXMax = std::numeric_limits<double>::lowest();
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = (*xValues)[index];
        if (!std::isfinite(value))
            continue;
        dataXMin = std::min(dataXMin, value);
        dataXMax = std::max(dataXMax, value);
    }
    if (dataXMin > dataXMax) {
        dataXMin = 0.0;
        dataXMax = 1.0;
    }
    if (dataXMax <= dataXMin)
        dataXMax = dataXMin + 1.0;
    double dataYMin = std::numeric_limits<double>::max();
    double dataYMax = std::numeric_limits<double>::lowest();
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = (*yValues)[index];
        if (!std::isfinite(value))
            continue;
        dataYMin = std::min(dataYMin, value);
        dataYMax = std::max(dataYMax, value);
    }
    if (dataYMin > dataYMax) {
        dataYMin = 0.0;
        dataYMax = 1.0;
    }
    // Keep zero visible for the common time-vs-voltage view. Signal-vs-signal
    // plots auto-scale tightly instead of wasting space around the origin.
    if (isTimeSignal(normalizedSignalName(effectiveX))) {
        dataYMin = std::min(dataYMin, 0.0);
        dataYMax = std::max(dataYMax, 0.0);
    }

    const auto resolveRange = [](const std::optional<double> &fixedMin, const std::optional<double> &fixedMax,
                                 double autoMin, double autoMax) {
        if (!std::isfinite(autoMin))
            autoMin = 0.0;
        if (!std::isfinite(autoMax) || autoMax <= autoMin)
            autoMax = autoMin + 1.0;
        const bool hasMin = fixedMin && std::isfinite(*fixedMin);
        const bool hasMax = fixedMax && std::isfinite(*fixedMax);
        double minimum = hasMin ? *fixedMin : autoMin;
        double maximum = hasMax ? *fixedMax : autoMax;
        if (maximum <= minimum) {
            if (hasMin && hasMax)
                std::swap(minimum, maximum);
            if (maximum <= minimum) {
                if (hasMin)
                    maximum = minimum + 1.0;
                else
                    minimum = maximum - 1.0;
            }
        }
        return std::pair<double, double>{minimum, maximum};
    };
    const auto [xMin, xMax] = resolveRange(graph_.xMin, graph_.xMax, dataXMin, dataXMax);
    const auto [yMin, yMax] = resolveRange(graph_.yMin, graph_.yMax, dataYMin, dataYMax);

    const auto mapX = [&](double value) {
        return traceArea.left() + ((value - xMin) / (xMax - xMin)) * traceArea.width();
    };
    // Y grows upwards and zero sits at the bottom of the plot for the
    // common all-positive transient case.
    const auto mapY = [&](double value) {
        return traceArea.bottom() - ((value - yMin) / (yMax - yMin)) * traceArea.height();
    };

    const int xIntervals = std::clamp(static_cast<int>(plot.width() / 110.0), 3, 8);
    const int yIntervals = std::clamp(static_cast<int>(plot.height() / 55.0), 3, 7);
    const auto xTicks = axisTicks(xMin, xMax, xIntervals);
    const auto yTicks = axisTicks(yMin, yMax, yIntervals);

    if (gridVisible_) {
        painter.setPen(QPen(ice::uiColor(ice::UiColor::Grid1), 1));
        for (double value : xTicks) {
            const auto x = mapX(value);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        for (double value : yTicks) {
            const auto y = mapY(value);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
    }

    // Tick marks and labels. Y labels sit left of the frame, X labels below.
    painter.setPen(QPen(plotFrame, 1));
    for (double value : yTicks) {
        const auto y = mapY(value);
        painter.drawLine(QPointF(plot.left() - 4.0, y), QPointF(plot.left(), y));
    }
    for (double value : xTicks) {
        const auto x = mapX(value);
        painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 4.0));
    }
    painter.setPen(QPen(tickText, 1));
    for (double value : yTicks) {
        const auto y = mapY(value);
        painter.drawText(QRectF(4, y - 8, 48, 16), Qt::AlignRight | Qt::AlignVCenter, formatTick(value));
    }
    for (double value : xTicks) {
        const auto x = mapX(value);
        painter.drawText(QRectF(x - 45, plot.bottom() + 6, 90, 16), Qt::AlignHCenter | Qt::AlignTop, formatTick(value));
    }

    // Zero reference line (only when the axis straddles zero). The value
    // itself is already labelled by the Y tick marks.
    if (yMin < 0.0 && yMax > 0.0) {
        const auto zeroY = mapY(0.0);
        painter.setPen(QPen(zeroLine, 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), zeroY), QPointF(plot.right(), zeroY));
    }

    QPainterPath path;
    painter.save();
    painter.setClipRect(plot.adjusted(1, 1, -1, -1));
    for (std::size_t index = 0; index < count; ++index) {
        const auto x = (*xValues)[index];
        const auto y = (*yValues)[index];
        if (!std::isfinite(x) || !std::isfinite(y))
            continue;
        const auto point = QPointF(mapX(x), mapY(y));
        if (path.elementCount() == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    painter.setPen(QPen(ice::uiColor(ice::UiColor::Accent2), 2));
    painter.drawPath(path);
    painter.restore();

    painter.setPen(QPen(ice::uiColor(ice::UiColor::Accent2), 1));
    painter.drawText(QRectF(plot.left() + 8, plot.top() + 6, plot.width() - 16, 20), Qt::AlignLeft,
                     isTimeSignal(normalizedSignalName(effectiveY)) ? xLabel : yLabel);
}
