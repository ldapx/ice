#include "simulation_plot.hpp"

#include "ui_colors.hpp"

#include <QInputDialog>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
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

// Legend chips and the axis titles use the same palette as the traces.
QColor traceColor(std::size_t index) {
    auto cyan = ice::uiColor(ice::UiColor::Accent3);
    cyan = cyan.darker(135);
    const QColor colors[] = {ice::uiColor(ice::UiColor::Accent2), ice::uiColor(ice::UiColor::Accent4),
                             ice::uiColor(ice::UiColor::Accent5), ice::uiColor(ice::UiColor::Accent6), cyan};
    return colors[index % 5];
}

constexpr qreal plotLeft = 82.0;
constexpr qreal plotRightMargin = 20.0;
constexpr qreal plotBottomMargin = 56.0;
constexpr qreal titleBottom = 28.0;
constexpr qreal legendChipHeight = 18.0;
constexpr qreal legendChipPadding = 7.0;
constexpr qreal legendChipGap = 6.0;
constexpr qreal legendRowGap = 4.0;
// Each legend entry draws a short line in the signal colour before its name.
constexpr qreal legendSwatchWidth = 16.0;
constexpr qreal legendSwatchGap = 6.0;

} // namespace

SimulationPlot::SimulationPlot(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(260);
    setAutoFillBackground(true);
    setToolTip(QStringLiteral("Simulation traces. Double-click an axis title or legend label to rename it."));
}

void SimulationPlot::setResult(std::shared_ptr<const ice::SimulationResult> result) {
    result_ = std::move(result);
    invalidateDisplayCache();
    update();
}

const ice::SimulationResult &SimulationPlot::result() const {
    static const ice::SimulationResult empty;
    return result_ ? *result_ : empty;
}

void SimulationPlot::setGraph(const ice::SimulationGraph &graph) {
    graph_ = graph;
    invalidateDisplayCache();
    update();
}

void SimulationPlot::setGridVisible(bool visible) {
    gridVisible_ = visible;
    update();
}

void SimulationPlot::setLabelEditCallback(
    std::function<void(ice::ObjectId, const std::string &, const QString &)> callback) {
    labelEditCallback_ = std::move(callback);
}

void SimulationPlot::setYAxisLabelEditCallback(std::function<void(ice::ObjectId, const QString &)> callback) {
    yAxisLabelEditCallback_ = std::move(callback);
}

void SimulationPlot::clear() {
    result_.reset();
    invalidateDisplayCache();
    update();
}

void SimulationPlot::resizeEvent(QResizeEvent *event) {
    invalidateDisplayCache();
    QWidget::resizeEvent(event);
}

void SimulationPlot::editLabel(const std::string &signal, const QString &currentLabel) {
    if (!labelEditCallback_ || signal.empty())
        return;
    bool ok = false;
    const auto label = QInputDialog::getText(this, tr("Rename Signal"),
                                             tr("Display label for %1:").arg(QString::fromStdString(signal)),
                                             QLineEdit::Normal, currentLabel, &ok);
    if (!ok)
        return;
    labelEditCallback_(graph_.id, signal, label.trimmed());
}

void SimulationPlot::mouseDoubleClickEvent(QMouseEvent *event) {
    ensureDisplayCache();
    const auto position = event->position();
    if (labelEditCallback_) {
        for (std::size_t index = 0; index < cache_.legendChips.size() && index < cache_.legendSignals.size(); ++index) {
            if (!cache_.legendChips[index].contains(position))
                continue;
            editLabel(cache_.legendSignals[index], cache_.traceLabels[index]);
            return;
        }
        if (cache_.xLabelRect.contains(position)) {
            editLabel(cache_.xSignal, cache_.xLabel);
            return;
        }
    }
    if (yAxisLabelEditCallback_ && cache_.yLabelRect.contains(position)) {
        bool ok = false;
        const auto label = QInputDialog::getText(this, tr("Rename Y Axis"), tr("Y axis title:"), QLineEdit::Normal,
                                                 cache_.yLabel, &ok);
        if (ok)
            yAxisLabelEditCallback_(graph_.id, label.trimmed());
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void SimulationPlot::invalidateDisplayCache() { cache_.valid = false; }

const std::vector<double> *SimulationPlot::valuesForSignal(const std::string &signal) const {
    if (!result_)
        return nullptr;
    const auto &result = *result_;
    const auto normalized = normalizedSignalName(signal);
    if (normalized.empty())
        return nullptr;
    if (isTimeSignal(normalized))
        return &result.time;
    for (const auto &trace : result.traces) {
        if (normalizedSignalName(trace.name) == normalized)
            return &trace.values;
    }
    return nullptr;
}

void SimulationPlot::ensureDisplayCache() const {
    if (cache_.valid && cache_.size == size())
        return;
    cache_ = {};
    cache_.size = size();
    static const ice::SimulationResult emptyResult;
    const auto &result = result_ ? *result_ : emptyResult;

    // Display labels fall back to the signal name. They are stored per signal
    // on the graph, so a renamed axis or legend entry survives project reloads.
    const auto labelFor = [this](const std::string &signal) {
        const auto override = graph_.signalLabels.find(signal);
        if (override != graph_.signalLabels.end() && !override->second.empty())
            return QString::fromStdString(override->second);
        return isTimeSignal(normalizedSignalName(signal)) ? QStringLiteral("Time (s)") : QString::fromStdString(signal);
    };
    const QFontMetricsF metrics(font());
    const qreal legendWidth = std::max<qreal>(40.0, rect().width() - plotLeft - plotRightMargin);
    qreal cursorX = plotLeft;
    qreal cursorY = titleBottom;
    // Lay the legend chips out in rows above the plot frame, in trace order.
    const auto placeChip = [&](const QString &label) {
        const qreal width =
            legendSwatchWidth + legendSwatchGap + metrics.horizontalAdvance(label) + 2 * legendChipPadding;
        if (cursorX > plotLeft && cursorX + width > plotLeft + legendWidth) {
            cursorX = plotLeft;
            cursorY += legendChipHeight + legendRowGap;
        }
        cache_.legendChips.emplace_back(cursorX, cursorY, width, legendChipHeight);
        cursorX += width + legendChipGap;
    };

    // Resolve the selected axes to result vectors. Time is always available;
    // missing signals fall back to time on X and the first returned trace on Y,
    // which keeps older projects readable after signals are renamed.
    std::string effectiveX = graph_.xAxis;
    const auto *xValues = valuesForSignal(effectiveX);
    if (!xValues || xValues->empty()) {
        effectiveX = "time";
        xValues = &result.time;
    }

    struct YTrace {
        std::string signal;
        const std::vector<double> *values = nullptr;
    };
    std::vector<YTrace> yTraces;
    for (const auto &signal : graph_.ySignals) {
        const auto *values = valuesForSignal(signal);
        if (values && !values->empty())
            yTraces.push_back({signal, values});
    }
    if (yTraces.empty()) {
        if (!result.traces.empty())
            yTraces.push_back({result.traces.front().name, &result.traces.front().values});
        else
            yTraces.push_back({"time", &result.time});
    }

    cache_.xLabel = labelFor(effectiveX);
    cache_.xSignal = effectiveX;
    for (const auto &trace : yTraces) {
        const auto label = labelFor(trace.signal);
        cache_.traceLabels.push_back(label);
        cache_.legendSignals.push_back(trace.signal);
        placeChip(label);
    }
    // The Y axis shows the unit-less caption "Signals" unless the graph was
    // given an explicit title; individual signal names live in the legend.
    cache_.yLabel = graph_.yAxisLabel.empty() ? QStringLiteral("Signals") : QString::fromStdString(graph_.yAxisLabel);
    cache_.title = graph_.name.empty() ? (result.traces.empty() ? QStringLiteral("No simulation trace")
                                                                : QStringLiteral("Simulation traces"))
                                       : QString::fromStdString(graph_.name);

    // The plot starts below the legend band so the chips sit outside the frame.
    const qreal plotTop = cursorY + legendChipHeight + legendRowGap + 4.0;
    const qreal plotBottom = std::max(plotTop + 2.0, rect().bottom() - plotBottomMargin);
    const QRectF plot = QRectF(QPointF(plotLeft, plotTop), QPointF(rect().width() - plotRightMargin, plotBottom));
    const QRectF traceArea = plot.adjusted(2, 2, -2, -2);
    cache_.plot = plot;
    cache_.traceArea = traceArea;
    cache_.xLabelRect = QRectF(plot.left(), plot.bottom() + 26, plot.width(), 16);
    cache_.yLabelRect = QRectF(4, plot.center().y() - plot.height() / 2.0, 16, plot.height());

    std::size_t maxCount = 0;
    for (const auto &trace : yTraces)
        maxCount = std::max(maxCount, std::min(xValues->size(), trace.values->size()));
    if (maxCount == 0 || traceArea.width() <= 1.0 || traceArea.height() <= 1.0) {
        cache_.valid = true;
        return;
    }

    double dataXMin = std::numeric_limits<double>::max();
    double dataXMax = std::numeric_limits<double>::lowest();
    for (std::size_t index = 0; index < maxCount; ++index) {
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
    for (const auto &trace : yTraces) {
        const auto count = std::min(xValues->size(), trace.values->size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = (*trace.values)[index];
            if (!std::isfinite(value))
                continue;
            dataYMin = std::min(dataYMin, value);
            dataYMax = std::max(dataYMax, value);
        }
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
    cache_.xMin = xMin;
    cache_.xMax = xMax;
    cache_.yMin = yMin;
    cache_.yMax = yMax;

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
    cache_.xTicks = axisTicks(xMin, xMax, xIntervals);
    cache_.yTicks = axisTicks(yMin, yMax, yIntervals);

    // Keep at most two display points per horizontal pixel column. The
    // min/max envelope preserves short spikes while making the painter path
    // independent of the number of stored simulation samples.
    const int columns = std::clamp(static_cast<int>(std::ceil(traceArea.width())), 64, 4096);
    for (const auto &trace : yTraces) {
        std::vector<double> columnMin(static_cast<std::size_t>(columns), std::numeric_limits<double>::max());
        std::vector<double> columnMax(static_cast<std::size_t>(columns), std::numeric_limits<double>::lowest());
        bool havePoint = false;
        const auto count = std::min(xValues->size(), trace.values->size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto x = (*xValues)[index];
            const auto y = (*trace.values)[index];
            if (!std::isfinite(x) || !std::isfinite(y))
                continue;
            const auto screenX = mapX(x);
            auto column = static_cast<int>((screenX - traceArea.left()) / traceArea.width() * columns);
            column = std::clamp(column, 0, columns - 1);
            columnMin[static_cast<std::size_t>(column)] = std::min(columnMin[static_cast<std::size_t>(column)], y);
            columnMax[static_cast<std::size_t>(column)] = std::max(columnMax[static_cast<std::size_t>(column)], y);
            havePoint = true;
        }
        QPainterPath path;
        if (havePoint) {
            double previousY = 0.0;
            bool havePrevious = false;
            for (int column = 0; column < columns; ++column) {
                const auto minimum = columnMin[static_cast<std::size_t>(column)];
                const auto maximum = columnMax[static_cast<std::size_t>(column)];
                if (minimum > maximum)
                    continue;
                const auto x = traceArea.left() + (column + 0.5) * traceArea.width() / columns;
                auto first = mapY(minimum);
                auto second = mapY(maximum);
                if (havePrevious && std::abs(second - previousY) < std::abs(first - previousY))
                    std::swap(first, second);
                if (path.elementCount() == 0)
                    path.moveTo(x, first);
                else
                    path.lineTo(x, first);
                if (second != first)
                    path.lineTo(x, second);
                previousY = second;
                havePrevious = true;
            }
        }
        cache_.paths.push_back(std::move(path));
    }
    cache_.valid = true;
}

void SimulationPlot::paintEvent(QPaintEvent *) {
    ensureDisplayCache();

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

    const auto &plot = cache_.plot;
    const auto &traceArea = cache_.traceArea;
    painter.setPen(QPen(plotFrame, 1));
    painter.drawRect(plot);
    painter.setPen(QPen(axisText, 1));
    painter.drawText(QRectF(8, 6, width() - 16, 20), Qt::AlignCenter, cache_.title);

    // Legend entries sit above the plot frame, left-aligned with it, so the
    // labels never overlap the traces. Each entry is a white-outlined box
    // holding a short line in the signal colour followed by the signal name.
    const auto legendBorder = QColor(Qt::white);
    for (std::size_t index = 0; index < cache_.legendChips.size() && index < cache_.traceLabels.size(); ++index) {
        const auto &chip = cache_.legendChips[index];
        painter.setPen(QPen(legendBorder, 1));
        painter.setBrush(canvasBackground);
        painter.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        const auto lineY = chip.center().y();
        const auto lineLeft = chip.left() + legendChipPadding;
        painter.setPen(QPen(traceColor(index), 2));
        painter.drawLine(QPointF(lineLeft, lineY), QPointF(lineLeft + legendSwatchWidth, lineY));
        painter.setPen(QPen(axisText, 1));
        painter.drawText(QRectF(lineLeft + legendSwatchWidth + legendSwatchGap, chip.top(),
                                chip.width() - (legendSwatchWidth + legendSwatchGap + 2 * legendChipPadding),
                                chip.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, cache_.traceLabels[index]);
    }
    painter.setBrush(Qt::NoBrush);

    painter.setPen(QPen(axisText, 1));
    painter.save();
    painter.translate(12, plot.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-plot.height() / 2.0, -8, plot.height(), 16), Qt::AlignCenter, cache_.yLabel);
    painter.restore();
    painter.drawText(cache_.xLabelRect, Qt::AlignHCenter, cache_.xLabel);

    if (cache_.paths.empty())
        return;

    const auto mapX = [&](double value) {
        return traceArea.left() + ((value - cache_.xMin) / (cache_.xMax - cache_.xMin)) * traceArea.width();
    };
    const auto mapY = [&](double value) {
        return traceArea.bottom() - ((value - cache_.yMin) / (cache_.yMax - cache_.yMin)) * traceArea.height();
    };

    if (gridVisible_) {
        painter.setPen(QPen(ice::uiColor(ice::UiColor::Grid1), 1));
        for (const double value : cache_.xTicks) {
            const auto x = mapX(value);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        for (const double value : cache_.yTicks) {
            const auto y = mapY(value);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
    }

    // Tick marks and labels. Y labels sit left of the frame, X labels below.
    painter.setPen(QPen(plotFrame, 1));
    for (const double value : cache_.yTicks) {
        const auto y = mapY(value);
        painter.drawLine(QPointF(plot.left() - 4.0, y), QPointF(plot.left(), y));
    }
    for (const double value : cache_.xTicks) {
        const auto x = mapX(value);
        painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + 4.0));
    }
    painter.setPen(QPen(tickText, 1));
    for (const double value : cache_.yTicks) {
        const auto y = mapY(value);
        painter.drawText(QRectF(24, y - 8, 52, 16), Qt::AlignRight | Qt::AlignVCenter, formatTick(value));
    }
    for (const double value : cache_.xTicks) {
        const auto x = mapX(value);
        painter.drawText(QRectF(x - 45, plot.bottom() + 6, 90, 16), Qt::AlignHCenter | Qt::AlignTop, formatTick(value));
    }

    // Zero reference line (only when the axis straddles zero). The value
    // itself is already labelled by the Y tick marks.
    if (cache_.yMin < 0.0 && cache_.yMax > 0.0) {
        const auto zeroY = mapY(0.0);
        painter.setPen(QPen(zeroLine, 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), zeroY), QPointF(plot.right(), zeroY));
    }

    painter.save();
    painter.setClipRect(plot.adjusted(1, 1, -1, -1));
    for (std::size_t traceIndex = 0; traceIndex < cache_.paths.size(); ++traceIndex) {
        painter.setPen(QPen(traceColor(traceIndex), 2));
        painter.drawPath(cache_.paths[traceIndex]);
    }
    painter.restore();
}
