#include "simulation_plot.hpp"

#include "openvlsilib/waveform.hpp"
#include "ui_colors.hpp"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QString>
#include <QWheelEvent>

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

bool sameSignal(const std::string &left, const std::string &right) {
    const auto normalizedLeft = normalizedSignalName(left);
    const auto normalizedRight = normalizedSignalName(right);
    if (normalizedLeft == normalizedRight)
        return true;
    if (normalizedLeft.size() > 2 && normalizedLeft.back() == ')' &&
        (normalizedLeft.rfind("v(", 0) == 0 || normalizedLeft.rfind("i(", 0) == 0))
        return normalizedLeft.substr(2, normalizedLeft.size() - 3) == normalizedRight;
    if (normalizedRight.size() > 2 && normalizedRight.back() == ')' &&
        (normalizedRight.rfind("v(", 0) == 0 || normalizedRight.rfind("i(", 0) == 0))
        return normalizedRight.substr(2, normalizedRight.size() - 3) == normalizedLeft;
    return false;
}

const std::vector<double> *valuesForSignalInResult(const openvlsi::SimulationResult &result, const std::string &signal) {
    if (signal.empty())
        return nullptr;
    if (isTimeSignal(normalizedSignalName(signal)) ||
        (!result.xAxis.empty() && normalizedSignalName(signal) == normalizedSignalName(result.xAxis)))
        return &result.time;
    for (const auto &trace : result.traces) {
        if (sameSignal(trace.name, signal))
            return &trace.values;
    }
    return nullptr;
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
    auto cyan = openvlsi::uiColor(openvlsi::UiColor::Accent3);
    cyan = cyan.darker(135);
    const QColor colors[] = {openvlsi::uiColor(openvlsi::UiColor::Accent2), openvlsi::uiColor(openvlsi::UiColor::Accent4),
                             openvlsi::uiColor(openvlsi::UiColor::Accent5), openvlsi::uiColor(openvlsi::UiColor::Accent6), cyan};
    return colors[index % 5];
}

bool sameDerivedTraces(const std::vector<openvlsi::SimulationDerivedTrace> &left,
                       const std::vector<openvlsi::SimulationDerivedTrace> &right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name || left[index].expression != right[index].expression)
            return false;
    }
    return true;
}

bool graphDataChanged(const openvlsi::SimulationGraph &left, const openvlsi::SimulationGraph &right) {
    return left.xAxis != right.xAxis || left.ySignals != right.ySignals || left.signalLabels != right.signalLabels ||
           left.view != right.view || left.fft != right.fft || left.fftWindow != right.fftWindow ||
           left.fftPoints != right.fftPoints || !sameDerivedTraces(left.derivedTraces, right.derivedTraces);
}

std::optional<double> interpolateValue(const std::vector<double> &x, const std::vector<double> &y, double target,
                                       bool sorted, const std::vector<std::size_t> *nearestOrder = nullptr) {
    const auto count = std::min(x.size(), y.size());
    if (count == 0)
        return std::nullopt;
    if (count == 1)
        return std::isfinite(y.front()) ? std::optional<double>(y.front()) : std::nullopt;
    if (sorted) {
        const auto lower = std::lower_bound(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(count), target);
        if (lower == x.begin())
            return std::isfinite(y.front()) ? std::optional<double>(y.front()) : std::nullopt;
        if (lower == x.begin() + static_cast<std::ptrdiff_t>(count))
            return std::isfinite(y.back()) ? std::optional<double>(y.back()) : std::nullopt;
        const auto upperIndex = static_cast<std::size_t>(lower - x.begin());
        const auto lowerIndex = upperIndex - 1;
        const auto span = x[upperIndex] - x[lowerIndex];
        if (std::abs(span) <= 1e-30)
            return y[upperIndex];
        const auto fraction = (target - x[lowerIndex]) / span;
        const auto value = y[lowerIndex] + (y[upperIndex] - y[lowerIndex]) * fraction;
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }
    // Large non-monotonic axes (for example a hysteresis loop) use a sorted
    // index. Small curves still use the nearest sample scan.
    if (nearestOrder && !nearestOrder->empty()) {
        const auto lower = std::lower_bound(
            nearestOrder->begin(), nearestOrder->end(), target,
            [&](std::size_t index, double value) { return x[index] < value; });
        std::size_t nearest = *nearestOrder->begin();
        double best = std::numeric_limits<double>::max();
        const auto consider = [&](std::size_t index) {
            if (index >= count)
                return;
            const auto distance = std::abs(x[index] - target);
            if (distance < best) {
                best = distance;
                nearest = index;
            }
        };
        if (lower != nearestOrder->begin())
            consider(*(lower - 1));
        if (lower != nearestOrder->end())
            consider(*lower);
        return std::isfinite(y[nearest]) ? std::optional<double>(y[nearest]) : std::nullopt;
    }
    // Unsorted axes without an index use the nearest sample.
    std::size_t nearest = 0;
    double best = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < count; ++index) {
        const auto distance = std::abs(x[index] - target);
        if (distance < best) {
            best = distance;
            nearest = index;
        }
    }
    return std::isfinite(y[nearest]) ? std::optional<double>(y[nearest]) : std::nullopt;
}

double decibelToLinear(double decibels) { return std::pow(10.0, decibels / 20.0); }

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
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setToolTip(QStringLiteral("Simulation traces. Drag to pan, Shift+drag to box zoom, wheel to zoom, "
                              "click for cursor A, Shift+click for cursor B. Double-click an axis title or "
                              "legend label to rename it; right-click for markers and view actions."));
}

void SimulationPlot::setResult(std::shared_ptr<const openvlsi::SimulationResult> result) {
    result_ = std::move(result);
    // A new run invalidates any previously zoomed window. Cursors are kept so
    // they can be used to compare the new result against the same x values.
    viewXMin_.reset();
    viewXMax_.reset();
    viewYMin_.reset();
    viewYMax_.reset();
    invalidatePreparedCache();
    invalidateDisplayCache();
    update();
}

const openvlsi::SimulationResult &SimulationPlot::result() const {
    static const openvlsi::SimulationResult empty;
    return result_ ? *result_ : empty;
}

void SimulationPlot::setOverlays(std::vector<Overlay> overlays) {
    overlays_ = std::move(overlays);
    invalidatePreparedCache();
    invalidateDisplayCache();
    update();
}

void SimulationPlot::setGraph(const openvlsi::SimulationGraph &graph) {
    // Changing the transform invalidates a zoom window expressed on the old
    // axis (time zoom cannot be reused for a frequency view).
    if (graph.view != graph_.view || graph.fft != graph_.fft || graph.xAxis != graph_.xAxis) {
        viewXMin_.reset();
        viewXMax_.reset();
        viewYMin_.reset();
        viewYMax_.reset();
        boxZooming_ = false;
        boxZoomRect_ = {};
    }
    if (graphDataChanged(graph, graph_))
        invalidatePreparedCache();
    graph_ = graph;
    invalidateDisplayCache();
    update();
}

void SimulationPlot::setGridVisible(bool visible) {
    gridVisible_ = visible;
    update();
}

void SimulationPlot::setTool(Tool tool) {
    tool_ = tool;
    QWidget::setCursor(tool == Tool::marker ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void SimulationPlot::setLabelEditCallback(
    std::function<void(openvlsi::ObjectId, const std::string &, const QString &)> callback) {
    labelEditCallback_ = std::move(callback);
}

void SimulationPlot::setYAxisLabelEditCallback(std::function<void(openvlsi::ObjectId, const QString &)> callback) {
    yAxisLabelEditCallback_ = std::move(callback);
}

void SimulationPlot::setMarkerCallback(std::function<void(const openvlsi::SimulationGraphMarker &)> callback) {
    markerCallback_ = std::move(callback);
}

void SimulationPlot::setClearMarkersCallback(std::function<void()> callback) {
    clearMarkersCallback_ = std::move(callback);
}

void SimulationPlot::setStatusCallback(std::function<void(const QString &)> callback) {
    statusCallback_ = std::move(callback);
}

void SimulationPlot::clear() {
    result_.reset();
    overlays_.clear();
    invalidatePreparedCache();
    invalidateDisplayCache();
    update();
}

bool SimulationPlot::hasZoom() const {
    return viewXMin_.has_value() && viewXMax_.has_value() && viewYMin_.has_value() && viewYMax_.has_value();
}

void SimulationPlot::resetView() {
    viewXMin_.reset();
    viewXMax_.reset();
    viewYMin_.reset();
    viewYMax_.reset();
    boxZooming_ = false;
    boxZoomRect_ = {};
    dragging_ = false;
    invalidateDisplayCache();
    update();
    emitStatus(QStringLiteral("Zoom reset"));
}

void SimulationPlot::setCursorPosition(std::size_t which, std::optional<double> x) {
    if (which == 0)
        cursorA_ = x;
    else
        cursorB_ = x;
    update();
}

std::optional<double> SimulationPlot::cursor(std::size_t which) const { return which == 0 ? cursorA_ : cursorB_; }

void SimulationPlot::clearCursors() {
    cursorA_.reset();
    cursorB_.reset();
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
    if (inTraceArea(position)) {
        resetView();
        return;
    }
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

void SimulationPlot::mousePressEvent(QMouseEvent *event) {
    ensureDisplayCache();
    if (event->button() == Qt::MiddleButton && inTraceArea(event->position())) {
        resetView();
        return;
    }
    if (event->button() != Qt::LeftButton || !inTraceArea(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto position = event->position();
    // Grab a cursor if the press starts on its line.
    for (std::size_t index = 0; index < 2; ++index) {
        const auto cursorValue = cursor(index);
        if (!cursorValue)
            continue;
        const auto cursorX = positionAt(*cursorValue, cache_.yMin).x();
        if (std::abs(cursorX - position.x()) <= 5.0) {
            movingCursor_ = true;
            movingCursorIndex_ = index;
            return;
        }
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        boxZooming_ = true;
        boxZoomRect_ = QRectF(position, position);
        update();
        return;
    }
    if (tool_ == Tool::marker) {
        placeMarker(position);
        return;
    }
    dragging_ = true;
    dragStart_ = position;
    dragStartXMin_ = cache_.xMin;
    dragStartXMax_ = cache_.xMax;
    dragStartYMin_ = cache_.yMin;
    dragStartYMax_ = cache_.yMax;
}

void SimulationPlot::mouseMoveEvent(QMouseEvent *event) {
    ensureDisplayCache();
    const auto position = event->position();
    hover_ = position;
    if (movingCursor_) {
        const auto value = xAtPosition(position.x());
        if (movingCursorIndex_ == 0)
            cursorA_ = value;
        else
            cursorB_ = value;
        update();
        return;
    }
    if (boxZooming_) {
        boxZoomRect_ = QRectF(dragStart_, position).normalized();
        update();
        return;
    }
    if (dragging_) {
        if (!dragStartXMin_ || !dragStartXMax_ || !dragStartYMin_ || !dragStartYMax_)
            return;
        const auto width = std::max<qreal>(1.0, cache_.traceArea.width());
        const auto height = std::max<qreal>(1.0, cache_.traceArea.height());
        const auto dx = (position.x() - dragStart_.x()) / width;
        const auto dy = (position.y() - dragStart_.y()) / height;
        const auto xRange = *dragStartXMax_ - *dragStartXMin_;
        const auto yRange = *dragStartYMax_ - *dragStartYMin_;
        viewXMin_ = *dragStartXMin_ - dx * xRange;
        viewXMax_ = *dragStartXMax_ - dx * xRange;
        viewYMin_ = *dragStartYMin_ + dy * yRange;
        viewYMax_ = *dragStartYMax_ + dy * yRange;
        invalidateDisplayCache();
        update();
        return;
    }
    update();
}

void SimulationPlot::mouseReleaseEvent(QMouseEvent *event) {
    ensureDisplayCache();
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (movingCursor_) {
        movingCursor_ = false;
        emitStatus(readoutText());
        return;
    }
    if (boxZooming_) {
        applyBoxZoom();
        return;
    }
    if (dragging_) {
        const auto moved = (event->position() - dragStart_).manhattanLength();
        dragging_ = false;
        if (moved < 3.0 && inTraceArea(event->position())) {
            const auto value = xAtPosition(event->position().x());
            if (event->modifiers().testFlag(Qt::ShiftModifier))
                cursorB_ = value;
            else
                cursorA_ = value;
            update();
            emitStatus(readoutText());
        }
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void SimulationPlot::leaveEvent(QEvent *event) {
    hover_.reset();
    update();
    QWidget::leaveEvent(event);
}

void SimulationPlot::wheelEvent(QWheelEvent *event) {
    ensureDisplayCache();
    if (!inTraceArea(event->position()) || cache_.xMax <= cache_.xMin) {
        QWidget::wheelEvent(event);
        return;
    }
    const auto steps = event->angleDelta().y();
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const auto factor = steps > 0 ? 0.8 : 1.25;
    const auto anchorX = xAtPosition(event->position().x());
    const auto anchorY = yAtPosition(event->position().y());
    viewXMin_ = anchorX - (anchorX - cache_.xMin) * factor;
    viewXMax_ = anchorX + (cache_.xMax - anchorX) * factor;
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
        viewYMin_ = anchorY - (anchorY - cache_.yMin) * factor;
        viewYMax_ = anchorY + (cache_.yMax - anchorY) * factor;
    } else {
        viewYMin_ = cache_.yMin;
        viewYMax_ = cache_.yMax;
    }
    if (std::abs(*viewXMax_ - *viewXMin_) <= 1e-15 || std::abs(*viewYMax_ - *viewYMin_) <= 1e-15)
        resetView();
    else {
        invalidateDisplayCache();
        update();
    }
    event->accept();
}

void SimulationPlot::contextMenuEvent(QContextMenuEvent *event) {
    ensureDisplayCache();
    showContextMenu(event->pos());
}

void SimulationPlot::showContextMenu(const QPointF &position) {
    QMenu menu(this);
    auto *reset = menu.addAction(QStringLiteral("Reset Zoom"));
    reset->setEnabled(hasZoom());
    connect(reset, &QAction::triggered, this, [this] { resetView(); });
    menu.addSeparator();
    const auto inPlot = inTraceArea(position);
    auto *setA = menu.addAction(QStringLiteral("Set Cursor A Here"));
    setA->setEnabled(inPlot);
    connect(setA, &QAction::triggered, this, [this, position] {
        cursorA_ = xAtPosition(position.x());
        update();
        emitStatus(readoutText());
    });
    auto *setB = menu.addAction(QStringLiteral("Set Cursor B Here"));
    setB->setEnabled(inPlot);
    connect(setB, &QAction::triggered, this, [this, position] {
        cursorB_ = xAtPosition(position.x());
        update();
        emitStatus(readoutText());
    });
    auto *clearCursorsAction = menu.addAction(QStringLiteral("Clear Cursors"));
    clearCursorsAction->setEnabled(cursorA_.has_value() || cursorB_.has_value());
    connect(clearCursorsAction, &QAction::triggered, this, [this] {
        clearCursors();
        emitStatus(QString());
    });
    menu.addSeparator();
    auto *markerMode = menu.addAction(QStringLiteral("Marker Mode"));
    markerMode->setCheckable(true);
    markerMode->setChecked(tool_ == Tool::marker);
    connect(markerMode, &QAction::triggered, this,
            [this](bool checked) { setTool(checked ? Tool::marker : Tool::pan); });
    auto *addMarker = menu.addAction(QStringLiteral("Add Marker Here"));
    addMarker->setEnabled(inPlot);
    connect(addMarker, &QAction::triggered, this, [this, position] { placeMarker(position); });
    auto *clearMarkersAction = menu.addAction(QStringLiteral("Clear Markers"));
    clearMarkersAction->setEnabled(!graph_.markers.empty());
    connect(clearMarkersAction, &QAction::triggered, this, [this] {
        graph_.markers.clear();
        invalidateDisplayCache();
        update();
        if (clearMarkersCallback_)
            clearMarkersCallback_();
    });
    menu.addSeparator();
    auto *copy = menu.addAction(QStringLiteral("Copy Cursor Readout"));
    copy->setEnabled(cursorA_.has_value() || cursorB_.has_value());
    connect(copy, &QAction::triggered, this, [this] {
        const auto text = readoutText();
        if (!text.isEmpty())
            QGuiApplication::clipboard()->setText(text);
        emitStatus(QStringLiteral("Cursor readout copied"));
    });
    menu.exec(mapToGlobal(position.toPoint()));
}

void SimulationPlot::placeMarker(const QPointF &position) {
    ensureDisplayCache();
    if (!inTraceArea(position))
        return;
    openvlsi::SimulationGraphMarker marker;
    marker.label = "M" + std::to_string(graph_.markers.size() + 1);
    marker.x = xAtPosition(position.x());
    const auto view = openvlsi::parseWaveformView(graph_.view).value_or(openvlsi::WaveformView::linear);
    for (const auto &trace : prepared_.traces) {
        if (trace.y.empty())
            continue;
        const auto value = interpolateValue(trace.x, trace.y, marker.x, trace.xSorted,
                                trace.nearestOrder.empty() ? nullptr : &trace.nearestOrder);
        if (!value)
            continue;
        marker.signal = trace.signal;
        marker.y = view == openvlsi::WaveformView::decibel ? decibelToLinear(*value) : *value;
        marker.hasY = true;
        break;
    }
    graph_.markers.push_back(marker);
    invalidateDisplayCache();
    update();
    if (markerCallback_)
        markerCallback_(marker);
    emitStatus(
        QStringLiteral("Marker '%1' added at %2").arg(QString::fromStdString(marker.label), formatValue(marker.x)));
}

void SimulationPlot::applyBoxZoom() {
    boxZooming_ = false;
    const auto rect = boxZoomRect_.normalized();
    boxZoomRect_ = {};
    if (rect.width() < 5.0 || rect.height() < 5.0) {
        update();
        return;
    }
    viewXMin_ = xAtPosition(rect.left());
    viewXMax_ = xAtPosition(rect.right());
    // Y increases upward, so the top edge is the maximum.
    viewYMax_ = yAtPosition(rect.top());
    viewYMin_ = yAtPosition(rect.bottom());
    if (std::abs(*viewXMax_ - *viewXMin_) <= 1e-15 || std::abs(*viewYMax_ - *viewYMin_) <= 1e-15)
        resetView();
    else {
        invalidateDisplayCache();
        update();
        emitStatus(QStringLiteral("Zoomed to selection"));
    }
}

bool SimulationPlot::inTraceArea(const QPointF &position) const { return cache_.traceArea.contains(position); }

double SimulationPlot::xAtPosition(double x) const {
    if (cache_.traceArea.width() <= 1.0)
        return cache_.xMin;
    const auto fraction = (x - cache_.traceArea.left()) / cache_.traceArea.width();
    return cache_.xMin + fraction * (cache_.xMax - cache_.xMin);
}

double SimulationPlot::yAtPosition(double y) const {
    if (cache_.traceArea.height() <= 1.0)
        return cache_.yMin;
    const auto fraction = (cache_.traceArea.bottom() - y) / cache_.traceArea.height();
    return cache_.yMin + fraction * (cache_.yMax - cache_.yMin);
}

QPointF SimulationPlot::positionAt(double x, double y) const {
    const auto xSpan = std::max(1e-300, cache_.xMax - cache_.xMin);
    const auto ySpan = std::max(1e-300, cache_.yMax - cache_.yMin);
    return QPointF(cache_.traceArea.left() + (x - cache_.xMin) / xSpan * cache_.traceArea.width(),
                   cache_.traceArea.bottom() - (y - cache_.yMin) / ySpan * cache_.traceArea.height());
}

void SimulationPlot::emitStatus(const QString &message) {
    if (statusCallback_)
        statusCallback_(message);
}

QString SimulationPlot::formatValue(double value, const QString &unit) const {
    if (!std::isfinite(value))
        return QStringLiteral("—");
    auto text = QString::number(value, 'g', 6);
    if (!unit.isEmpty())
        text += QLatin1Char(' ') + unit;
    return text;
}

void SimulationPlot::invalidateDisplayCache() { cache_.valid = false; }

const std::vector<double> *SimulationPlot::valuesForSignal(const std::string &signal) const {
    if (!result_)
        return nullptr;
    return valuesForSignalInResult(*result_, signal);
}

void SimulationPlot::invalidatePreparedCache() { prepared_.valid = false; }

void SimulationPlot::ensurePreparedCache() const {
    if (prepared_.valid)
        return;
    prepared_ = {};
    static const openvlsi::SimulationResult emptyResult;
    const auto &base = result_ ? *result_ : emptyResult;
    const auto view = openvlsi::parseWaveformView(graph_.view).value_or(openvlsi::WaveformView::linear);
    openvlsi::WaveformFftOptions fftOptions;
    fftOptions.window = graph_.fftWindow;
    fftOptions.points = graph_.fftPoints > 0 ? static_cast<std::size_t>(graph_.fftPoints) : 0;

    const auto labelFor = [this, &base](const std::string &signal) -> QString {
        const auto override = graph_.signalLabels.find(signal);
        if (override != graph_.signalLabels.end() && !override->second.empty())
            return QString::fromStdString(override->second);
        if (isTimeSignal(normalizedSignalName(signal)) ||
            (!base.xAxis.empty() && normalizedSignalName(signal) == normalizedSignalName(base.xAxis))) {
            if (base.xAxis.empty() || base.xAxis == "time")
                return QStringLiteral("Time (s)");
            if (base.xUnit.empty())
                return QString::fromStdString(base.xAxis);
            return QStringLiteral("%1 (%2)").arg(QString::fromStdString(base.xAxis),
                                                 QString::fromStdString(base.xUnit));
        }
        return QString::fromStdString(signal);
    };

    struct RawTrace {
        std::string signal;
        QString label;
        QString xUnit;
        QString yUnit;
        std::vector<double> x;
        std::vector<double> y;
        bool dashed = false;
        int colorIndex = 0;
    };
    std::vector<RawTrace> rawTraces;
    const auto appendRun = [&](const openvlsi::SimulationResult &run, const QString &overlayLabel, bool baseRun) {
        std::vector<std::string> runWarnings;
        auto series = openvlsi::buildWaveformSeries(run, graph_.ySignals, graph_.derivedTraces, view, graph_.fft, fftOptions,
                                               &runWarnings);
        if (series.empty() && !run.traces.empty()) {
            // Older projects that selected a signal missing from this run still
            // show all returned traces instead of an empty plot.
            runWarnings.clear();
            series =
                openvlsi::buildWaveformSeries(run, {}, graph_.derivedTraces, view, graph_.fft, fftOptions, &runWarnings);
        }
        for (const auto &warning : runWarnings) {
            const auto text = overlayLabel.isEmpty()
                                  ? QString::fromStdString(warning)
                                  : QStringLiteral("[%1] %2").arg(overlayLabel, QString::fromStdString(warning));
            if (std::find(prepared_.warnings.begin(), prepared_.warnings.end(), text) == prepared_.warnings.end())
                prepared_.warnings.push_back(text);
        }
        // A graph can plot one captured signal against another, preserving the
        // X/Y curve workflow from the original widget.
        const std::vector<double> *customX = nullptr;
        if (!graph_.fft && !graph_.xAxis.empty() && !isTimeSignal(normalizedSignalName(graph_.xAxis)) &&
            normalizedSignalName(graph_.xAxis) != normalizedSignalName(run.xAxis))
            customX = valuesForSignalInResult(run, graph_.xAxis);
        for (std::size_t index = 0; index < series.size(); ++index) {
            RawTrace trace;
            trace.signal = series[index].name;
            trace.x = customX ? *customX : std::move(series[index].x);
            trace.y = std::move(series[index].y);
            const auto count = std::min(trace.x.size(), trace.y.size());
            if (count == 0)
                continue;
            trace.x.resize(count);
            trace.y.resize(count);
            auto label = labelFor(series[index].name);
            if (label == QString::fromStdString(series[index].name) && !series[index].label.empty() && graph_.fft)
                label = QString::fromStdString(series[index].label);
            if (!overlayLabel.isEmpty())
                label += QStringLiteral(" [%1]").arg(overlayLabel);
            trace.label = label;
            trace.xUnit = QString::fromStdString(series[index].xUnit);
            trace.yUnit = QString::fromStdString(series[index].yUnit);
            trace.dashed = !baseRun;
            trace.colorIndex = static_cast<int>(index);
            rawTraces.push_back(std::move(trace));
        }
    };

    if (result_)
        appendRun(base, {}, true);
    for (const auto &overlay : overlays_) {
        if (!overlay.result)
            continue;
        QString label = QString::fromStdString(overlay.label);
        if (label.isEmpty())
            label = QStringLiteral("overlay");
        appendRun(*overlay.result, label, false);
    }

    prepared_.haveData = false;
    prepared_.dataXMin = std::numeric_limits<double>::max();
    prepared_.dataXMax = std::numeric_limits<double>::lowest();
    prepared_.dataYMin = std::numeric_limits<double>::max();
    prepared_.dataYMax = std::numeric_limits<double>::lowest();
    prepared_.traces.reserve(rawTraces.size());
    for (auto &raw : rawTraces) {
        PreparedTrace trace;
        trace.signal = std::move(raw.signal);
        trace.label = std::move(raw.label);
        trace.xUnit = std::move(raw.xUnit);
        trace.yUnit = std::move(raw.yUnit);
        trace.x = std::move(raw.x);
        trace.y = std::move(raw.y);
        trace.dashed = raw.dashed;
        trace.colorIndex = raw.colorIndex;
        const auto count = std::min(trace.x.size(), trace.y.size());
        trace.x.resize(count);
        trace.y.resize(count);

        bool sorted = count > 0;
        double previousX = 0.0;
        bool havePrevious = false;
        double xMin = std::numeric_limits<double>::max();
        double xMax = std::numeric_limits<double>::lowest();
        double yMin = std::numeric_limits<double>::max();
        double yMax = std::numeric_limits<double>::lowest();
        for (std::size_t index = 0; index < count; ++index) {
            const auto x = trace.x[index];
            const auto y = trace.y[index];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                sorted = false;
                continue;
            }
            if (havePrevious && x < previousX)
                sorted = false;
            previousX = x;
            havePrevious = true;
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
        }
        trace.xSorted = sorted;
        if (!trace.xSorted && count > 4096) {
            trace.nearestOrder.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                if (std::isfinite(trace.x[index]))
                    trace.nearestOrder.push_back(index);
            }
            std::sort(trace.nearestOrder.begin(), trace.nearestOrder.end(),
                      [&](std::size_t left, std::size_t right) { return trace.x[left] < trace.x[right]; });
        }
        if (xMin <= xMax) {
            prepared_.haveData = true;
            prepared_.dataXMin = std::min(prepared_.dataXMin, xMin);
            prepared_.dataXMax = std::max(prepared_.dataXMax, xMax);
            prepared_.dataYMin = std::min(prepared_.dataYMin, yMin);
            prepared_.dataYMax = std::max(prepared_.dataYMax, yMax);
        }
        if (count > 4096 && xMax > xMin) {
            trace.envelopeMinX = xMin;
            trace.envelopeMaxX = xMax;
            trace.envelope.assign(envelopeBuckets, {});
            const auto span = xMax - xMin;
            for (std::size_t index = 0; index < count; ++index) {
                const auto x = trace.x[index];
                const auto y = trace.y[index];
                if (!std::isfinite(x) || !std::isfinite(y))
                    continue;
                const auto bucket =
                    std::clamp(static_cast<std::size_t>((x - xMin) / span * static_cast<double>(envelopeBuckets - 1)),
                               std::size_t{0}, envelopeBuckets - 1);
                auto &entry = trace.envelope[bucket];
                if (!entry.valid) {
                    entry.minY = y;
                    entry.maxY = y;
                    entry.valid = true;
                } else {
                    entry.minY = std::min(entry.minY, y);
                    entry.maxY = std::max(entry.maxY, y);
                }
            }
        }
        prepared_.traces.push_back(std::move(trace));
    }
    if (!prepared_.haveData) {
        prepared_.dataXMin = 0.0;
        prepared_.dataXMax = 1.0;
        prepared_.dataYMin = 0.0;
        prepared_.dataYMax = 1.0;
    }
    prepared_.valid = true;
}

QPainterPath SimulationPlot::buildTracePath(const PreparedTrace &trace, double xMin, double xMax, int columns) const {
    QPainterPath path;
    const auto &area = cache_.traceArea;
    if (area.width() <= 1.0 || columns <= 0)
        return path;
    const auto count = std::min(trace.x.size(), trace.y.size());
    if (count == 0)
        return path;

    std::vector<double> columnMin(static_cast<std::size_t>(columns), std::numeric_limits<double>::max());
    std::vector<double> columnMax(static_cast<std::size_t>(columns), std::numeric_limits<double>::lowest());
    const auto addPoint = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
        const auto screenX = area.left() + ((x - xMin) / (xMax - xMin)) * area.width();
        auto column = static_cast<int>((screenX - area.left()) / area.width() * columns);
        column = std::clamp(column, 0, columns - 1);
        auto &minimum = columnMin[static_cast<std::size_t>(column)];
        auto &maximum = columnMax[static_cast<std::size_t>(column)];
        minimum = std::min(minimum, y);
        maximum = std::max(maximum, y);
    };

    auto begin = trace.x.begin();
    auto end = trace.x.begin() + static_cast<std::ptrdiff_t>(count);
    if (trace.xSorted) {
        begin = std::lower_bound(begin, end, xMin);
        end = std::upper_bound(begin, end, xMax);
    }
    const auto visible = static_cast<std::size_t>(end - begin);
    const auto envelopeThreshold = std::max<std::size_t>(131072, static_cast<std::size_t>(columns) * 32);
    const auto visibleCount = trace.xSorted ? visible : count;
    if (!trace.envelope.empty() && visibleCount > envelopeThreshold) {
        const auto envelopeSpan = trace.envelopeMaxX - trace.envelopeMinX;
        const auto span = xMax - xMin;
        for (int column = 0; column < columns; ++column) {
            const auto left = xMin + (span * column) / columns;
            const auto right = xMin + (span * (column + 1)) / columns;
            if (right < trace.envelopeMinX || left > trace.envelopeMaxX)
                continue;
            const auto clampedLeft = std::max(left, trace.envelopeMinX);
            const auto clampedRight = std::min(right, trace.envelopeMaxX);
            auto firstBucket = static_cast<std::size_t>(
                std::clamp((clampedLeft - trace.envelopeMinX) / envelopeSpan *
                               static_cast<double>(envelopeBuckets - 1),
                           0.0, static_cast<double>(envelopeBuckets - 1)));
            auto lastBucket = static_cast<std::size_t>(
                std::clamp((clampedRight - trace.envelopeMinX) / envelopeSpan *
                               static_cast<double>(envelopeBuckets - 1),
                           0.0, static_cast<double>(envelopeBuckets - 1)));
            if (lastBucket < firstBucket)
                std::swap(firstBucket, lastBucket);
            bool valid = false;
            double minimum = std::numeric_limits<double>::max();
            double maximum = std::numeric_limits<double>::lowest();
            for (std::size_t bucket = firstBucket; bucket <= lastBucket; ++bucket) {
                const auto &entry = trace.envelope[bucket];
                if (!entry.valid)
                    continue;
                valid = true;
                minimum = std::min(minimum, entry.minY);
                maximum = std::max(maximum, entry.maxY);
            }
            if (!valid)
                continue;
            columnMin[static_cast<std::size_t>(column)] = minimum;
            columnMax[static_cast<std::size_t>(column)] = maximum;
        }
    } else {
        // For sorted traces, only scan the visible slice. For non-monotonic
        // signal-vs-signal curves, preserve sample order and scan all samples.
        for (auto iterator = begin; iterator != end; ++iterator) {
            const auto index = static_cast<std::size_t>(iterator - trace.x.begin());
            addPoint(trace.x[index], trace.y[index]);
        }
    }

    double previousY = 0.0;
    bool havePrevious = false;
    for (int column = 0; column < columns; ++column) {
        const auto minimum = columnMin[static_cast<std::size_t>(column)];
        const auto maximum = columnMax[static_cast<std::size_t>(column)];
        if (minimum > maximum)
            continue;
        const auto x = area.left() + (column + 0.5) * area.width() / columns;
        auto first = area.bottom() - ((minimum - cache_.yMin) / (cache_.yMax - cache_.yMin)) * area.height();
        auto second = area.bottom() - ((maximum - cache_.yMin) / (cache_.yMax - cache_.yMin)) * area.height();
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
    return path;
}

void SimulationPlot::ensureDisplayCache() const {
    if (cache_.valid && cache_.size == size())
        return;
    ensurePreparedCache();
    cache_ = {};
    cache_.size = size();
    cache_.warnings = prepared_.warnings;

    const QFontMetricsF metrics(font());
    const qreal legendWidth = std::max<qreal>(40.0, rect().width() - plotLeft - plotRightMargin);
    qreal cursorX = plotLeft;
    qreal cursorY = titleBottom;
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
    for (const auto &trace : prepared_.traces) {
        cache_.traceLabels.push_back(trace.label);
        cache_.legendSignals.push_back(trace.signal);
        placeChip(trace.label);
    }

    std::string axisSignal = graph_.xAxis;
    if (!graph_.fft) {
        if (axisSignal.empty() || axisSignal == "time" ||
            (!result_ || result_->xAxis.empty() ||
             normalizedSignalName(axisSignal) == normalizedSignalName(result_->xAxis)))
            axisSignal = !result_ || result_->xAxis.empty() ? "time" : result_->xAxis;
    }
    cache_.xSignal = axisSignal;
    const auto xLabelFor = [this](const std::string &signal) -> QString {
        const auto override = graph_.signalLabels.find(signal);
        if (override != graph_.signalLabels.end() && !override->second.empty())
            return QString::fromStdString(override->second);
        if (signal.empty() || signal == "time")
            return QStringLiteral("Time (s)");
        if (result_ && !result_->xUnit.empty() && !result_->xAxis.empty() &&
            normalizedSignalName(signal) == normalizedSignalName(result_->xAxis))
            return QStringLiteral("%1 (%2)").arg(QString::fromStdString(signal),
                                                 QString::fromStdString(result_->xUnit));
        return QString::fromStdString(signal);
    };
    cache_.xLabel = graph_.fft ? QStringLiteral("Frequency (Hz)") : xLabelFor(axisSignal);
    const auto view = openvlsi::parseWaveformView(graph_.view).value_or(openvlsi::WaveformView::linear);
    cache_.yLabel = graph_.yAxisLabel.empty()
                        ? (graph_.fft ? (view == openvlsi::WaveformView::decibel
                                             ? QStringLiteral("Magnitude (dB)")
                                             : (view == openvlsi::WaveformView::phase ? QStringLiteral("Phase (deg)")
                                                                                 : QStringLiteral("Magnitude")))
                                      : (view == openvlsi::WaveformView::decibel
                                             ? QStringLiteral("Magnitude (dB)")
                                             : (view == openvlsi::WaveformView::phase ? QStringLiteral("Phase (deg)")
                                                                                 : QStringLiteral("Signals"))))
                        : QString::fromStdString(graph_.yAxisLabel);
    cache_.title = graph_.name.empty() ? QStringLiteral("Simulation traces") : QString::fromStdString(graph_.name);
    if (graph_.fft)
        cache_.title += QStringLiteral(" · FFT");
    if (view == openvlsi::WaveformView::decibel)
        cache_.title += QStringLiteral(" · dB");
    else if (view == openvlsi::WaveformView::phase)
        cache_.title += QStringLiteral(" · Phase");

    const qreal plotTop = cursorY + legendChipHeight + legendRowGap + 4.0;
    const qreal plotBottom = std::max(plotTop + 2.0, rect().bottom() - plotBottomMargin);
    const QRectF plot = QRectF(QPointF(plotLeft, plotTop), QPointF(rect().width() - plotRightMargin, plotBottom));
    const QRectF traceArea = plot.adjusted(2, 2, -2, -2);
    cache_.plot = plot;
    cache_.traceArea = traceArea;
    cache_.xLabelRect = QRectF(plot.left(), plot.bottom() + 26, plot.width(), 16);
    cache_.yTickLabelRect = QRectF(plot.left() - 58, 0, 52, 0);
    cache_.yLabelRect = QRectF(4, plot.center().y() - plot.height() / 2.0, 16, plot.height());

    if (prepared_.traces.empty() || traceArea.width() <= 1.0 || traceArea.height() <= 1.0) {
        cache_.valid = true;
        return;
    }

    const auto dataXMin = prepared_.haveData ? prepared_.dataXMin : 0.0;
    const auto dataXMax = prepared_.haveData ? prepared_.dataXMax : 1.0;
    auto dataYMin = prepared_.haveData ? prepared_.dataYMin : 0.0;
    auto dataYMax = prepared_.haveData ? prepared_.dataYMax : 1.0;
    if (view == openvlsi::WaveformView::linear && !graph_.fft && isTimeSignal(normalizedSignalName(cache_.xSignal))) {
        dataYMin = std::min(dataYMin, 0.0);
        dataYMax = std::max(dataYMax, 0.0);
    }
    const auto resolveRange = [](const std::optional<double> &viewMin, const std::optional<double> &viewMax,
                                 const std::optional<double> &fixedMin, const std::optional<double> &fixedMax,
                                 double autoMin, double autoMax) {
        if (!std::isfinite(autoMin))
            autoMin = 0.0;
        if (!std::isfinite(autoMax) || autoMax <= autoMin)
            autoMax = autoMin + 1.0;
        const bool zoomed = viewMin && viewMax;
        const bool hasMin = zoomed ? true : fixedMin && std::isfinite(*fixedMin);
        const bool hasMax = zoomed ? true : fixedMax && std::isfinite(*fixedMax);
        double minimum = zoomed ? *viewMin : (hasMin ? *fixedMin : autoMin);
        double maximum = zoomed ? *viewMax : (hasMax ? *fixedMax : autoMax);
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
    const auto [xMin, xMax] = resolveRange(viewXMin_, viewXMax_, graph_.xMin, graph_.xMax, dataXMin, dataXMax);
    const auto [yMin, yMax] = resolveRange(viewYMin_, viewYMax_, graph_.yMin, graph_.yMax, dataYMin, dataYMax);
    cache_.xMin = xMin;
    cache_.xMax = xMax;
    cache_.yMin = yMin;
    cache_.yMax = yMax;

    const int xIntervals = std::clamp(static_cast<int>(plot.width() / 110.0), 3, 8);
    const int yIntervals = std::clamp(static_cast<int>(plot.height() / 55.0), 3, 7);
    cache_.xTicks = axisTicks(xMin, xMax, xIntervals);
    cache_.yTicks = axisTicks(yMin, yMax, yIntervals);

    qreal widestYTick = 0.0;
    for (const double value : cache_.yTicks)
        widestYTick = std::max(widestYTick, metrics.horizontalAdvance(formatTick(value)));
    const qreal yTickWidth = std::max<qreal>(24.0, widestYTick + 6.0);
    const qreal yTickRight = plot.left() - 6.0;
    cache_.yTickLabelRect = QRectF(yTickRight - yTickWidth, 0, yTickWidth, 0);
    const qreal yLabelWidth = std::max<qreal>(12.0, metrics.height());
    const qreal yLabelLeft = std::max<qreal>(4.0, cache_.yTickLabelRect.left() - 8.0 - yLabelWidth);
    cache_.yLabelRect = QRectF(yLabelLeft, plot.center().y() - plot.height() / 2.0, yLabelWidth, plot.height());

    const int columns = std::clamp(static_cast<int>(std::ceil(traceArea.width())), 64, 4096);
    cache_.traces.reserve(prepared_.traces.size());
    for (std::size_t index = 0; index < prepared_.traces.size(); ++index) {
        const auto &source = prepared_.traces[index];
        DisplayTrace trace;
        trace.signal = source.signal;
        trace.label = source.label;
        trace.xUnit = source.xUnit;
        trace.yUnit = source.yUnit;
        trace.preparedIndex = index;
        trace.dashed = source.dashed;
        trace.colorIndex = source.colorIndex;
        if (xMax > xMin)
            trace.path = buildTracePath(source, xMin, xMax, columns);
        cache_.traces.push_back(std::move(trace));
    }
    cache_.valid = true;
}

void SimulationPlot::paintEvent(QPaintEvent *) {
    ensureDisplayCache();

    QPainter painter(this);
    // Match the schematic/layout canvas instead of using a dark plot.
    const auto canvasBackground = openvlsi::uiColor(openvlsi::UiColor::Background2);
    const auto plotFrame = openvlsi::uiColor(openvlsi::UiColor::Axis1);
    const auto axisText = openvlsi::uiColor(openvlsi::UiColor::Axis1);
    const auto tickText = openvlsi::uiColor(openvlsi::UiColor::Axis1);
    auto zeroLine = openvlsi::uiColor(openvlsi::UiColor::Axis1);
    zeroLine.setAlpha(110);
    painter.fillRect(rect(), canvasBackground);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto &plot = cache_.plot;
    const auto &traceArea = cache_.traceArea;
    painter.setPen(QPen(plotFrame, 1));
    painter.drawRect(plot);
    painter.setPen(QPen(axisText, 1));
    painter.drawText(QRectF(plot.left(), 6, plot.width(), 20), Qt::AlignCenter, cache_.title);

    // Legend entries sit above the plot frame, left-aligned with it, so the
    // labels never overlap the traces. Each entry is a white-outlined box
    // holding a short line in the signal colour followed by the signal name.
    const auto legendBorder = QColor(Qt::white);
    const QFontMetricsF metrics(font());
    for (std::size_t index = 0; index < cache_.legendChips.size() && index < cache_.traceLabels.size(); ++index) {
        const auto &chip = cache_.legendChips[index];
        painter.setPen(QPen(legendBorder, 1));
        painter.setBrush(canvasBackground);
        painter.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        const auto lineY = chip.center().y();
        const auto lineLeft = chip.left() + legendChipPadding;
        const auto colorIndex =
            index < cache_.traces.size() ? cache_.traces[index].colorIndex : static_cast<int>(index);
        const bool dashed = index < cache_.traces.size() && cache_.traces[index].dashed;
        auto pen = QPen(traceColor(static_cast<std::size_t>(colorIndex)), dashed ? 1.5 : 2);
        if (dashed)
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
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
    painter.translate(cache_.yLabelRect.center().x(), cache_.yLabelRect.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-cache_.yLabelRect.height() / 2.0, -cache_.yLabelRect.width() / 2.0,
                            cache_.yLabelRect.height(), cache_.yLabelRect.width()),
                     Qt::AlignCenter, cache_.yLabel);
    painter.restore();
    painter.drawText(cache_.xLabelRect, Qt::AlignHCenter, cache_.xLabel);

    const auto mapX = [&](double value) {
        return traceArea.left() + ((value - cache_.xMin) / (cache_.xMax - cache_.xMin)) * traceArea.width();
    };
    const auto mapY = [&](double value) {
        return traceArea.bottom() - ((value - cache_.yMin) / (cache_.yMax - cache_.yMin)) * traceArea.height();
    };

    if (gridVisible_) {
        painter.setPen(QPen(openvlsi::uiColor(openvlsi::UiColor::Grid1), 1));
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
        painter.drawText(QRectF(cache_.yTickLabelRect.left(), y - 8, cache_.yTickLabelRect.width(), 16),
                         Qt::AlignRight | Qt::AlignVCenter, formatTick(value));
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

    // Shade the region between cursors A and B before drawing traces.
    if (cursorA_ && cursorB_) {
        auto shade = openvlsi::uiColor(openvlsi::UiColor::Accent3);
        shade.setAlpha(28);
        const auto left = mapX(std::min(*cursorA_, *cursorB_));
        const auto right = mapX(std::max(*cursorA_, *cursorB_));
        painter.fillRect(QRectF(QPointF(left, plot.top()), QPointF(right, plot.bottom())), shade);
    }

    painter.save();
    painter.setClipRect(plot.adjusted(1, 1, -1, -1));
    for (const auto &trace : cache_.traces) {
        auto pen = QPen(traceColor(static_cast<std::size_t>(trace.colorIndex)), trace.dashed ? 1.5 : 2);
        if (trace.dashed)
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawPath(trace.path);
    }
    painter.restore();

    // Persisted point markers. Their Y is stored in linear units so changing
    // the graph view does not move them.
    const auto view = openvlsi::parseWaveformView(graph_.view).value_or(openvlsi::WaveformView::linear);
    auto markerPen = openvlsi::uiColor(openvlsi::UiColor::Accent2);
    markerPen.setAlpha(200);
    qreal markerTextY = plot.top() + 8.0;
    for (const auto &marker : graph_.markers) {
        if (marker.x < cache_.xMin || marker.x > cache_.xMax)
            continue;
        const auto x = mapX(marker.x);
        painter.setPen(QPen(markerPen, 1, Qt::DotLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        if (marker.hasY) {
            auto displayY = marker.y;
            if (view == openvlsi::WaveformView::decibel)
                displayY = 20.0 * std::log10(std::max(std::abs(marker.y), 1e-30));
            if (displayY >= cache_.yMin && displayY <= cache_.yMax) {
                const auto y = mapY(displayY);
                painter.setBrush(markerPen);
                painter.setPen(QPen(markerPen, 1));
                painter.drawEllipse(QPointF(x, y), 3.0, 3.0);
                painter.setBrush(Qt::NoBrush);
                const auto xUnit = cache_.traces.empty() ? QString() : cache_.traces.front().xUnit;
                const auto yUnit = cache_.traces.empty() ? QString() : cache_.traces.front().yUnit;
                const auto label = QStringLiteral("%1  %2, %3")
                                       .arg(QString::fromStdString(marker.label), formatValue(marker.x, xUnit),
                                            formatValue(displayY, yUnit));
                const auto width = metrics.horizontalAdvance(label) + 8.0;
                const auto labelX = std::min(x + 6.0, plot.right() - width - 2.0);
                painter.setPen(QPen(axisText, 1));
                painter.drawText(QRectF(labelX, markerTextY, width, 14.0), Qt::AlignLeft | Qt::AlignVCenter, label);
            }
        }
        markerTextY += 14.0;
    }

    // Measurement annotations: declared metrics are printed inside the frame
    // so the plot is self-describing when exported or screenshotted. Overlay
    // results contribute their own measurements with the overlay label.
    if (graph_.showMeasurements) {
        std::vector<QString> lines;
        const auto appendMeasurements = [&](const openvlsi::SimulationResult &run, const QString &prefix) {
            for (const auto &measurement : run.measurements) {
                const auto name = QString::fromStdString(measurement.name);
                const auto label = prefix.isEmpty() ? name : QStringLiteral("%1: %2").arg(prefix, name);
                if (measurement.valid) {
                    lines.push_back(QStringLiteral("%1 = %2").arg(
                        label, formatValue(measurement.value, QString::fromStdString(measurement.unit))));
                } else {
                    lines.push_back(QStringLiteral("%1 = —").arg(label));
                }
            }
        };
        if (result_)
            appendMeasurements(*result_, {});
        for (const auto &overlay : overlays_) {
            if (!overlay.result)
                continue;
            const auto label =
                overlay.label.empty() ? QStringLiteral("overlay") : QString::fromStdString(overlay.label);
            appendMeasurements(*overlay.result, label);
        }
        if (!lines.empty()) {
            QStringList rows;
            rows << QStringLiteral("Measurements");
            constexpr std::size_t maximum = 8;
            for (std::size_t index = 0; index < std::min(maximum, lines.size()); ++index)
                rows << lines[index];
            if (lines.size() > maximum)
                rows << QStringLiteral("+%1 more").arg(lines.size() - maximum);
            qreal width = 0.0;
            for (const auto &line : rows)
                width = std::max(width, metrics.horizontalAdvance(line));
            width += 16.0;
            const auto height = static_cast<qreal>(rows.size()) * 14.0 + 10.0;
            const QRectF box(plot.right() - width - 8.0, plot.top() + 8.0, width, height);
            auto background = canvasBackground;
            background.setAlpha(225);
            painter.setPen(QPen(openvlsi::uiColor(openvlsi::UiColor::Grid1), 1));
            painter.setBrush(background);
            painter.drawRoundedRect(box, 3, 3);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(axisText, 1));
            qreal lineY = box.top() + 5.0;
            for (const auto &line : rows) {
                painter.drawText(QRectF(box.left() + 8.0, lineY, box.width() - 16.0, 14.0),
                                 Qt::AlignLeft | Qt::AlignVCenter, line);
                lineY += 14.0;
            }
        }
    }

    // Cursors and their readout.
    const auto drawCursor = [&](double value, const QString &name, const QColor &color) {
        const auto x = mapX(value);
        if (x < plot.left() - 1.0 || x > plot.right() + 1.0)
            return;
        painter.setPen(QPen(color, 1, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRectF(x - 8.0, plot.top() - 1.0, 16.0, 12.0), 2, 2);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(canvasBackground, 1));
        painter.drawText(QRectF(x - 8.0, plot.top() - 1.0, 16.0, 12.0), Qt::AlignCenter, name);
    };
    auto cursorAColor = openvlsi::uiColor(openvlsi::UiColor::Accent2);
    auto cursorBColor = openvlsi::uiColor(openvlsi::UiColor::Accent4);
    if (cursorA_)
        drawCursor(*cursorA_, QStringLiteral("A"), cursorAColor);
    if (cursorB_)
        drawCursor(*cursorB_, QStringLiteral("B"), cursorBColor);

    const auto readout = readoutText();
    if (!readout.isEmpty()) {
        const auto lines = readout.split(QLatin1Char('\n'));
        qreal width = 0.0;
        for (const auto &line : lines)
            width = std::max(width, metrics.horizontalAdvance(line));
        width += 16.0;
        const auto height = static_cast<qreal>(lines.size()) * 14.0 + 10.0;
        QRectF box(traceArea.left() + 8.0, traceArea.bottom() - height - 8.0, width, height);
        if (box.right() > traceArea.right() - 8.0)
            box.moveRight(traceArea.right() - 8.0);
        auto background = canvasBackground;
        background.setAlpha(225);
        painter.setPen(QPen(openvlsi::uiColor(openvlsi::UiColor::Grid1), 1));
        painter.setBrush(background);
        painter.drawRoundedRect(box, 3, 3);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(axisText, 1));
        qreal lineY = box.top() + 5.0;
        for (const auto &line : lines) {
            painter.drawText(QRectF(box.left() + 8.0, lineY, box.width() - 16.0, 14.0),
                             Qt::AlignLeft | Qt::AlignVCenter, line);
            lineY += 14.0;
        }
    }

    if (!cache_.warnings.empty()) {
        auto warningColor = QColor(190, 132, 0);
        warningColor.setAlpha(220);
        painter.setPen(QPen(warningColor, 1));
        auto text = QStringLiteral("⚠ %1 warning%2")
                        .arg(cache_.warnings.size())
                        .arg(cache_.warnings.size() == 1 ? QString() : QStringLiteral("s"));
        const auto width = std::min<qreal>(plot.width() / 2.0, metrics.horizontalAdvance(text) + 12.0);
        painter.drawText(QRectF(plot.right() - width - 8.0, plot.bottom() - 18.0, width, 14.0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         metrics.elidedText(text, Qt::ElideRight, static_cast<int>(width - 4.0)));
        setToolTip(QStringList(cache_.warnings.begin(), cache_.warnings.end()).join(QLatin1Char('\n')));
    }

    // Box zoom selection overlay.
    if (boxZooming_ && boxZoomRect_.width() > 0.0 && boxZoomRect_.height() > 0.0) {
        auto selection = openvlsi::uiColor(openvlsi::UiColor::Accent3);
        selection.setAlpha(35);
        painter.fillRect(boxZoomRect_, selection);
        painter.setPen(QPen(selection.darker(120), 1, Qt::DashLine));
        painter.drawRect(boxZoomRect_);
    }
}

QString SimulationPlot::readoutText() const {
    ensureDisplayCache();
    std::optional<double> primary;
    if (cursorA_)
        primary = cursorA_;
    else if (cursorB_)
        primary = cursorB_;
    else if (hover_ && inTraceArea(*hover_))
        primary = xAtPosition(hover_->x());
    if (!primary)
        return {};
    const auto xUnit = cache_.traces.empty() ? QString() : cache_.traces.front().xUnit;
    QStringList lines;
    if (cursorA_ && cursorB_) {
        lines << QStringLiteral("A %1   B %2   Δ %3")
                     .arg(formatValue(*cursorA_, xUnit), formatValue(*cursorB_, xUnit),
                          formatValue(std::abs(*cursorB_ - *cursorA_), xUnit));
    } else {
        lines << QStringLiteral("%1 = %2").arg(cache_.xLabel, formatValue(*primary, xUnit));
    }
    const auto maximum = cursorA_ && cursorB_ ? std::size_t{4} : std::size_t{6};
    std::size_t shown = 0;
    for (const auto &display : cache_.traces) {
        if (shown >= maximum)
            break;
        if (display.preparedIndex >= prepared_.traces.size())
            continue;
        const auto &trace = prepared_.traces[display.preparedIndex];
        const auto first = interpolateValue(trace.x, trace.y, *primary, trace.xSorted,
                                  trace.nearestOrder.empty() ? nullptr : &trace.nearestOrder);
        if (!first)
            continue;
        if (cursorA_ && cursorB_) {
            const auto second = interpolateValue(trace.x, trace.y, *cursorB_, trace.xSorted,
                                       trace.nearestOrder.empty() ? nullptr : &trace.nearestOrder);
            if (!second)
                continue;
            lines << QStringLiteral("%1: %2 / %3  (Δ %4)")
                         .arg(display.label, formatValue(*first, display.yUnit), formatValue(*second, display.yUnit),
                              formatValue(*second - *first, display.yUnit));
        } else {
            lines << QStringLiteral("%1: %2").arg(display.label, formatValue(*first, display.yUnit));
        }
        ++shown;
    }
    if (cache_.traces.size() > shown)
        lines << QStringLiteral("+%1 more traces").arg(cache_.traces.size() - shown);
    return lines.join(QLatin1Char('\n'));
}

QString SimulationPlot::cursorReadout() const { return readoutText(); }
