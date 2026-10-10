#include "icelib/core.hpp"
#include "icelib/operations.hpp"
#include "icelib/simulation.hpp"

#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QGraphicsPathItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsView>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QKeySequence>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QtSvgWidgets/QGraphicsSvgItem>

#include <memory>
#include <string>
#include <sstream>
#include <functional>
#include <cmath>
#include <algorithm>
#include <queue>
#include <set>
#include <tuple>
#include <limits>

namespace {

QLabel* heading(const QString& text) {
    auto* label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("font-weight: bold; padding: 4px 2px;"));
    return label;
}

QWidget* placeholder(const QString& title, const QString& description) {
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->addStretch();
    auto* titleLabel = new QLabel(title);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    layout->addWidget(titleLabel);
    auto* descriptionLabel = new QLabel(description);
    descriptionLabel->setAlignment(Qt::AlignCenter);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);
    layout->addStretch();
    return widget;
}

struct SymbolDefinition {
    QString name;
    QString category;
    QString svg;
    std::vector<ice::Pin> pins;
};

const std::vector<SymbolDefinition>& symbolLibrary() {
    static const std::vector<SymbolDefinition> definitions = {
        {"Resistor", "Passive", "<svg viewBox='0 0 120 60'><path d='M0 30h20l8-15 16 30 16-30 16 30 8-15h20' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "1", -60, 0}, {0, "2", 60, 0}}},
        {"Capacitor", "Passive", "<svg viewBox='0 0 120 60'><path d='M0 30h45M75 30h45M45 10v40M75 10v40' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "1", -60, 0}, {0, "2", 60, 0}}},
        {"Diode", "Semiconductor", "<svg viewBox='0 0 120 60'><path d='M0 30h35l25-25v50L35 30h85M85 10v50' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "A", -60, 0}, {0, "K", 60, 0}}},
        {"Voltage Source", "Sources", "<svg viewBox='0 0 120 60'><path d='M0 30h35M85 30h35M60 5v50M45 20h30M60 12v16' fill='none' stroke='black' stroke-width='4'/><circle cx='60' cy='30' r='25' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "+", -60, 0}, {0, "-", 60, 0}}},
        {"Ground", "Power", "<svg viewBox='0 0 80 60'><path d='M40 0v25M10 25h60M20 35h40M30 45h20' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "GND", 0, 0}}},
        {"Op Amp", "Analog", "<svg viewBox='0 0 120 100'><path d='M20 5v90l90-45zM0 30h20M0 70h20M110 50h10' fill='none' stroke='black' stroke-width='4'/><path d='M30 25h12M36 19v12M30 65h12' stroke='black' stroke-width='3'/></svg>", {{0, "+", -60, -20}, {0, "-", -60, 20}, {0, "out", 60, 0}}},
        {"NMOS", "Transistor", "<svg viewBox='0 0 100 120'><path d='M50 0v35M50 85v35M50 35v50M20 35v50M0 45h20M0 75h20M50 60h50' fill='none' stroke='black' stroke-width='4'/></svg>", {{0, "G", -50, 0}, {0, "S", 0, 60}, {0, "D", 0, -60}}},
    };
    return definitions;
}

const SymbolDefinition* findSymbolDefinition(const QString& name) {
    const auto& library = symbolLibrary();
    const auto it = std::find_if(library.begin(), library.end(),
                                 [&](const auto& symbol) { return symbol.name == name; });
    return it == library.end() ? nullptr : &*it;
}

QPointF rotatePoint(QPointF point, ice::Orientation orientation) {
    switch (orientation) {
    case ice::Orientation::r90: return {-point.y(), point.x()};
    case ice::Orientation::r180: return {-point.x(), -point.y()};
    case ice::Orientation::r270: return {point.y(), -point.x()};
    case ice::Orientation::mx: return {point.x(), -point.y()};
    case ice::Orientation::my: return {-point.x(), point.y()};
    case ice::Orientation::r0: return point;
    }
    return point;
}

ice::PinDirection rotateDirection(ice::PinDirection direction, ice::Orientation orientation) {
    const QPointF vector = [&] {
        switch (direction) {
        case ice::PinDirection::left: return QPointF(-1, 0);
        case ice::PinDirection::right: return QPointF(1, 0);
        case ice::PinDirection::up: return QPointF(0, 1);
        case ice::PinDirection::down: return QPointF(0, -1);
        }
        return QPointF(1, 0);
    }();
    const auto rotated = rotatePoint(vector, orientation);
    if (std::abs(rotated.x()) > std::abs(rotated.y()))
        return rotated.x() < 0 ? ice::PinDirection::left : ice::PinDirection::right;
    return rotated.y() < 0 ? ice::PinDirection::down : ice::PinDirection::up;
}

QPointF directionVector(ice::PinDirection direction) {
    switch (direction) {
    case ice::PinDirection::left: return {-1, 0};
    case ice::PinDirection::right: return {1, 0};
    case ice::PinDirection::up: return {0, -1};
    case ice::PinDirection::down: return {0, 1};
    }
    return {1, 0};
}

std::string joinNetlist(const std::vector<std::string>& lines) {
        std::ostringstream output;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (index != 0) output << '\n';
            output << lines[index];
        }
        return output.str();
    }

std::string simulationSummary(const ice::SimulationResult& result) {
        std::ostringstream output;
        output << "State: " << ice::jobStateName(result.state) << "\n"
               << "Analysis: " << result.analysis << "\n"
               << "Simulator: " << result.simulator << " " << result.simulator_version << "\n";
        if (!result.error.empty()) output << "Error: " << result.error << "\n";
        if (!result.time.empty() && !result.traces.empty()) {
            output << "Samples: " << result.time.size() << "\n"
                   << "Trace: " << result.traces.front().name << "\n"
                   << "First sample: " << result.traces.front().values.front() << "\n"
                   << "Last sample: " << result.traces.front().values.back() << "\n";
        }
        return output.str();
    }

    class LayoutCanvas : public QGraphicsView {
    public:
        enum class Tool { select, rectangle };

        explicit LayoutCanvas(QWidget* parent = nullptr) : QGraphicsView(parent) {
            setScene(new QGraphicsScene(this));
            setSceneRect(-2000, -2000, 4000, 4000);
            setRenderHint(QPainter::Antialiasing);
            setDragMode(QGraphicsView::RubberBandDrag);
            setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            setBackgroundBrush(QColor(QStringLiteral("#20252b")));
        }

        void setTool(Tool tool) { tool_ = tool; }
        void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                          std::function<void(ice::ObjectId, QPointF)> shapeMoved,
                          std::function<void(std::vector<ice::ObjectId>)> deleted,
                          std::function<void()> changed) {
            rectangleCreated_ = std::move(rectangleCreated);
            shapeMoved_ = std::move(shapeMoved);
            deleted_ = std::move(deleted);
            changed_ = std::move(changed);
        }

        void drawGrid(QPainter& painter, const QRectF& exposed, qreal scale) {
            const qreal grid = 20.0;
            painter.setPen(QPen(QColor(QStringLiteral("#343b43")), 0));
            const int left = static_cast<int>(std::floor(exposed.left() / grid));
            const int right = static_cast<int>(std::ceil(exposed.right() / grid));
            const int top = static_cast<int>(std::floor(exposed.top() / grid));
            const int bottom = static_cast<int>(std::ceil(exposed.bottom() / grid));
            for (int x = left; x <= right; ++x) painter.drawLine(QPointF(x * grid, exposed.top()), QPointF(x * grid, exposed.bottom()));
            for (int y = top; y <= bottom; ++y) painter.drawLine(QPointF(exposed.left(), y * grid), QPointF(exposed.right(), y * grid));
            Q_UNUSED(scale);
        }

        void setLayerColors(const std::map<ice::ObjectId, QColor>& colors) { layerColors_ = colors; }

    protected:
        void drawBackground(QPainter* painter, const QRectF& rect) override {
            painter->fillRect(rect, QColor(QStringLiteral("#20252b")));
            drawGrid(*painter, rect, transform().m11());
        }

        void mousePressEvent(QMouseEvent* event) override {
            if (event->button() == Qt::MiddleButton) {
                panning_ = true;
                lastPanPosition_ = event->pos();
                setCursor(Qt::ClosedHandCursor);
                event->accept();
                return;
            }
            if (tool_ == Tool::rectangle && event->button() == Qt::LeftButton) {
                drawing_ = true;
                drawStart_ = mapToScene(event->pos());
                drawPreview_ = scene()->addRect(QRectF(drawStart_, drawStart_), QPen(Qt::cyan, 1), QBrush(QColor(0, 180, 220, 50)));
                event->accept();
                return;
            }
            if (tool_ == Tool::select && event->button() == Qt::LeftButton) {
                movingItem_ = dynamic_cast<QGraphicsRectItem*>(itemAt(event->pos()));
                if (movingItem_) movingStartPosition_ = movingItem_->pos();
            }
            QGraphicsView::mousePressEvent(event);
        }

        void mouseMoveEvent(QMouseEvent* event) override {
            if (panning_) {
                const auto delta = event->pos() - lastPanPosition_;
                lastPanPosition_ = event->pos();
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
                verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
                event->accept();
                return;
            }
            if (drawing_ && drawPreview_) {
                drawPreview_->setRect(QRectF(drawStart_, mapToScene(event->pos())).normalized());
                event->accept();
                return;
            }
            QGraphicsView::mouseMoveEvent(event);
        }

        void mouseReleaseEvent(QMouseEvent* event) override {
            if (event->button() == Qt::MiddleButton) {
                panning_ = false;
                unsetCursor();
                event->accept();
                return;
            }
            if (drawing_ && event->button() == Qt::LeftButton) {
                const auto end = mapToScene(event->pos());
                const auto rectangle = QRectF(drawStart_, end).normalized();
                if (drawPreview_) {
                    scene()->removeItem(drawPreview_);
                    delete drawPreview_;
                    drawPreview_ = nullptr;
                }
                drawing_ = false;
                if (rectangle.width() >= 1.0 && rectangle.height() >= 1.0 && rectangleCreated_)
                    rectangleCreated_(drawStart_, end);
                event->accept();
                return;
            }
            if (tool_ == Tool::select && event->button() == Qt::LeftButton && movingItem_) {
                const auto delta = movingItem_->pos() - movingStartPosition_;
                if ((delta.x() != 0.0 || delta.y() != 0.0) && shapeMoved_)
                    shapeMoved_(movingItem_->data(0).toULongLong(), delta);
                movingItem_ = nullptr;
            }
            QGraphicsView::mouseReleaseEvent(event);
        }

        void keyPressEvent(QKeyEvent* event) override {
            if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && deleted_) {
                std::vector<ice::ObjectId> ids;
                for (auto* item : scene()->selectedItems())
                    if (auto* graphics = dynamic_cast<QGraphicsRectItem*>(item)) {
                        const auto id = graphics->data(0).toULongLong();
                        if (id != 0) ids.push_back(id);
                    }
                if (!ids.empty()) deleted_(std::move(ids));
                event->accept();
                return;
            }
            QGraphicsView::keyPressEvent(event);
        }

    private:
        Tool tool_ = Tool::select;
        bool panning_ = false;
        bool drawing_ = false;
        QPoint lastPanPosition_;
        QPointF drawStart_;
        QGraphicsRectItem* drawPreview_ = nullptr;
        QGraphicsRectItem* movingItem_ = nullptr;
        QPointF movingStartPosition_;
        std::function<void(QPointF, QPointF)> rectangleCreated_;
        std::function<void(ice::ObjectId, QPointF)> shapeMoved_;
        std::function<void(std::vector<ice::ObjectId>)> deleted_;
        std::function<void()> changed_;
        std::map<ice::ObjectId, QColor> layerColors_;
    };

    class SymbolLibraryDialog : public QDialog {
public:
    explicit SymbolLibraryDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Symbol Library"));
        resize(520, 420);
        auto* layout = new QVBoxLayout(this);
        list_ = new QListWidget;
        for (const auto& symbol : symbolLibrary())
            list_->addItem(symbol.category + QStringLiteral(" / ") + symbol.name);
        layout->addWidget(list_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        list_->setCurrentRow(0);
    }
    const SymbolDefinition& selected() const { return symbolLibrary().at(list_->currentRow()); }
private:
    QListWidget* list_ = nullptr;
};

class SchematicCanvas : public QGraphicsView {
public:
    explicit SchematicCanvas(QWidget* parent = nullptr) : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setSceneRect(-2000, -2000, 4000, 4000);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setBackgroundBrush(QColor(QStringLiteral("#20252b")));
    }

    void setCallbacks(std::function<void(ice::ObjectId, QPointF)> moved,
                      std::function<void(ice::ObjectId)> rotated,
                      std::function<void(ice::ObjectId, ice::ObjectId)> connected,
                      std::function<void(QPointF)> netLabelPlaced,
                      std::function<void()> changed) {
        moved_ = std::move(moved);
        rotated_ = std::move(rotated);
        connected_ = std::move(connected);
        netLabelPlaced_ = std::move(netLabelPlaced);
        changed_ = std::move(changed);
    }

    void setNetLabelMode(bool enabled) { netLabelMode_ = enabled; }

    void drawBackground(QPainter* painter, const QRectF& rect) override {
        painter->fillRect(rect, QColor(QStringLiteral("#f7f7f7")));
        const qreal step = 20.0;
        QPen minor(QColor(QStringLiteral("#e1e1e1")));
        minor.setWidthF(0.5);
        painter->setPen(minor);
        const auto left = std::floor(rect.left() / step) * step;
        const auto top = std::floor(rect.top() / step) * step;
        for (qreal x = left; x <= rect.right(); x += step) painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        for (qreal y = top; y <= rect.bottom(); y += step) painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
        QPen major(QColor(QStringLiteral("#b4b4b4")));
        major.setWidthF(1.0);
        painter->setPen(major);
        const auto majorStep = step * 5;
        const auto majorLeft = std::floor(rect.left() / majorStep) * majorStep;
        const auto majorTop = std::floor(rect.top() / majorStep) * majorStep;
        for (qreal x = majorLeft; x <= rect.right(); x += majorStep) painter->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
        for (qreal y = majorTop; y <= rect.bottom(); y += majorStep) painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            lastPan_ = event->pos();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            auto* item = itemAt(event->pos());
            if (netLabelMode_ && (!item || item->data(0).toString() != QStringLiteral("pin"))) {
                if (netLabelPlaced_) netLabelPlaced_(mapToScene(event->pos()));
                netLabelMode_ = false;
                event->accept();
                return;
            }
            if (item && item->data(0).toString() == QStringLiteral("pin")) {
                wireStart_ = item;
                setPinHighlights(item);
                event->accept();
                return;
            }
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (panning_) {
            const auto delta = event->pos() - lastPan_;
            lastPan_ = event->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
            event->accept();
            return;
        }
        if (wireStart_) {
            auto* candidate = itemAt(event->pos());
            highlightCandidate(candidate);
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (panning_ && event->button() == Qt::MiddleButton) {
            panning_ = false;
            unsetCursor();
            event->accept();
            return;
        }
        if (wireStart_ && event->button() == Qt::LeftButton) {
            auto* end = itemAt(event->pos());
            if (end && end != wireStart_ && end->data(0).toString() == QStringLiteral("pin"))
                connected_(wireStart_->data(1).toULongLong(), end->data(1).toULongLong());
            wireStart_ = nullptr;
            clearPinHighlights();
            event->accept();
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
        if (moved_ && scene()) {
            std::vector<std::pair<ice::ObjectId, QPointF>> movedItems;
            for (auto* item : scene()->items()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    movedItems.emplace_back(item->data(1).toULongLong(), item->pos());
            }
            for (const auto& [id, position] : movedItems)
                moved_(id, position);
        }
        if (changed_) changed_();
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_R && scene()) {
            for (auto* item : scene()->selectedItems())
                if (item->data(0).toString() == QStringLiteral("symbol") && rotated_)
                    rotated_(item->data(1).toULongLong());
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && scene()) {
            std::vector<ice::ObjectId> ids;
            for (auto* item : scene()->selectedItems()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    ids.push_back(item->data(1).toULongLong());
            }
            if (!ids.empty() && deleteSelected_) deleteSelected_(ids);
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

public:
    std::function<void(std::vector<ice::ObjectId>)> deleteSelected_;

private:
    void setPinHighlights(QGraphicsItem* source) {
        for (auto* item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin")) continue;
            auto* pin = dynamic_cast<QGraphicsEllipseItem*>(item);
            if (pin) pin->setBrush(item == source ? QBrush(Qt::green) : QBrush(QColor("#8cff66")));
        }
    }

    void highlightCandidate(QGraphicsItem* candidate) {
        for (auto* item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin")) continue;
            auto* pin = dynamic_cast<QGraphicsEllipseItem*>(item);
            if (!pin) continue;
            pin->setBrush(item == candidate && item != wireStart_ ? QBrush(Qt::white)
                                                                  : QBrush(QColor("#8cff66")));
        }
        if (auto* pin = dynamic_cast<QGraphicsEllipseItem*>(wireStart_))
            pin->setBrush(QBrush(Qt::green));
    }

    void clearPinHighlights() {
        for (auto* item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin")) continue;
            if (auto* pin = dynamic_cast<QGraphicsEllipseItem*>(item))
                pin->setBrush(QBrush(Qt::yellow));
        }
    }

    bool panning_ = false;
    QPoint lastPan_;
    QGraphicsItem* wireStart_ = nullptr;
    std::function<void(ice::ObjectId, QPointF)> moved_;
    std::function<void(ice::ObjectId)> rotated_;
    std::function<void(ice::ObjectId, ice::ObjectId)> connected_;
    std::function<void(QPointF)> netLabelPlaced_;
    std::function<void()> changed_;
    bool netLabelMode_ = false;
};

class SimulationPlot : public QWidget {
public:
    explicit SimulationPlot(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(260);
        setAutoFillBackground(true);
        setToolTip(QStringLiteral("Simulation traces"));
    }

    void setResult(const ice::SimulationResult& result) {
        result_ = result;
        update();
    }

    void clear() {
        result_ = {};
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#171b20")));
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = rect().adjusted(58, 18, -20, -42);
        painter.setPen(QPen(QColor("#8d98a5"), 1));
        painter.drawRect(plot);
        painter.drawText(QRectF(8, 2, width() - 16, 20), Qt::AlignCenter,
                         result_.traces.empty() ? QStringLiteral("No simulation trace") : QStringLiteral("Simulation traces"));
        if (result_.time.empty() || result_.traces.empty()) return;

        double maxY = 0.0;
        for (const auto& trace : result_.traces)
            for (double value : trace.values) maxY = std::max(maxY, std::abs(value));
        if (maxY <= 0.0) maxY = 1.0;
        const double maxX = result_.time.back() > 0.0 ? result_.time.back() : 1.0;
        painter.setPen(QPen(QColor("#65717f"), 1));
        painter.drawText(QRectF(4, plot.bottom() - 12, 48, 20), Qt::AlignRight, QStringLiteral("0"));
        painter.drawText(QRectF(4, plot.top() - 10, 48, 20), Qt::AlignRight,
                         QString::number(maxY, 'g', 4));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 8, 100, 20), Qt::AlignLeft, QStringLiteral("0"));
        painter.drawText(QRectF(plot.right() - 100, plot.bottom() + 8, 100, 20), Qt::AlignRight,
                         QString::number(maxX, 'g', 4) + QStringLiteral(" s"));

        const QColor colors[] = {QColor("#65c7f7"), QColor("#f6c85f"), QColor("#8be28b"),
                                 QColor("#e78ac3")};
        for (std::size_t traceIndex = 0; traceIndex < result_.traces.size(); ++traceIndex) {
            const auto& trace = result_.traces[traceIndex];
            if (trace.values.empty()) continue;
            QPainterPath path;
            const auto count = std::min(result_.time.size(), trace.values.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto x = plot.left() + (result_.time[index] / maxX) * plot.width();
                const auto y = plot.center().y() - (trace.values[index] / maxY) * (plot.height() / 2.0);
                if (index == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }
            painter.setPen(QPen(colors[traceIndex % 4], 2));
            painter.drawPath(path);
            painter.drawText(QRectF(plot.left() + 8 + traceIndex * 100, plot.top() + 6, 95, 20),
                             Qt::AlignLeft, QString::fromStdString(trace.name));
        }
    }

private:
    ice::SimulationResult result_;
};

class ProjectPanel : public QWidget {
public:
    explicit ProjectPanel(bool showLibraries = true, bool showLayers = true,
                          bool showLayoutHierarchy = true, QWidget* parent = nullptr)
        : QWidget(parent), showLayoutHierarchy_(showLayoutHierarchy) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Project")));
        projectName_ = new QLabel(QStringLiteral("No project open"));
        layout->addWidget(projectName_);

        tree_ = new QTreeWidget;
        tree_->setHeaderLabel(QStringLiteral("Hierarchy"));
        tree_->setAccessibleName(QStringLiteral("Project hierarchy"));
        layout->addWidget(tree_, 1);

        auto* libraries = new QGroupBox(QStringLiteral("Libraries and PDK"));
        auto* libraryLayout = new QVBoxLayout(libraries);
        libraryLayout->addWidget(new QLabel(QStringLiteral("Packages")));
        libraryLayout->addWidget(new QListWidget);
        if (showLibraries) layout->addWidget(libraries);

        auto* layers = new QGroupBox(QStringLiteral("Layers"));
        auto* layerLayout = new QVBoxLayout(layers);
        layerList_ = new QListWidget;
        layerList_->setAccessibleName(QStringLiteral("Layer visibility"));
        layerLayout->addWidget(layerList_);
        if (showLayers) layout->addWidget(layers);
    }

    void setProject(const ice::Project& project) {
        projectName_->setText(QString::fromStdString(project.name));
        tree_->clear();
        layerList_->clear();

        if (!showLayoutHierarchy_) {
            auto* schematic = new QTreeWidgetItem(tree_, {QStringLiteral("Schematic")});
            for (const auto& [id, cell] : project.schematicCells) {
                auto* cellItem = new QTreeWidgetItem(schematic, {QString::fromStdString(cell.name)});
                cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                new QTreeWidgetItem(cellItem, {QStringLiteral("Symbols (%1)").arg(cell.symbols.size())});
                new QTreeWidgetItem(cellItem, {QStringLiteral("Instances (%1)").arg(cell.instances.size())});
                new QTreeWidgetItem(cellItem, {QStringLiteral("Nets (%1)").arg(cell.nets.size())});
            }
        }

        if (!showLayoutHierarchy_) {
            tree_->expandAll();
            return;
        }
        auto* layout = new QTreeWidgetItem(tree_, {QStringLiteral("Layout")});
        for (const auto& [id, layer] : project.layers) {
            auto* item = new QListWidgetItem(QString::fromStdString(layer.name), layerList_);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
        }
        for (const auto& [id, cell] : project.layoutCells) {
            auto* cellItem = new QTreeWidgetItem(layout, {QString::fromStdString(cell.name)});
            cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            new QTreeWidgetItem(cellItem, {QStringLiteral("Shapes (%1)").arg(cell.shapes.size())});
            new QTreeWidgetItem(cellItem, {QStringLiteral("Ports (%1)").arg(cell.ports.size())});
            new QTreeWidgetItem(cellItem, {QStringLiteral("Instances (%1)").arg(cell.instances.size())});
        }
        tree_->expandAll();
    }

private:
    QLabel* projectName_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QListWidget* layerList_ = nullptr;
    bool showLayoutHierarchy_ = true;
};

class LayersPanel : public QWidget {
public:
    explicit LayersPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Layers")));
        list_ = new QListWidget;
        list_->setAccessibleName(QStringLiteral("Layer visibility"));
        layout->addWidget(list_);
        layout->addStretch();
    }

    void setProject(const ice::Project& project) {
        list_->clear();
        for (const auto& [id, layer] : project.layers) {
            auto* item = new QListWidgetItem(QString::fromStdString(layer.name), list_);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
        }
    }

private:
    QListWidget* list_ = nullptr;
};

class PropertiesPanel : public QWidget {
public:
    explicit PropertiesPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Properties")));
        auto* form = new QFormLayout;
        form->addRow(QStringLiteral("Selection"), new QLabel(QStringLiteral("Nothing selected")));
        form->addRow(QStringLiteral("Stable ID"), new QLabel(QStringLiteral("—")));
        form->addRow(QStringLiteral("Type"), new QLabel(QStringLiteral("—")));
        form->addRow(QStringLiteral("Layer / Net"), new QLabel(QStringLiteral("—")));
        form->addRow(QStringLiteral("Position"), new QLabel(QStringLiteral("—")));
        layout->addLayout(form);
        layout->addWidget(heading(QStringLiteral("Display")));
        auto* layerSelector = new QComboBox;
        layerSelector->addItem(QStringLiteral("All layers"));
        layerSelector->addItem(QStringLiteral("Visible layers"));
        layout->addWidget(layerSelector);
        layout->addStretch();
    }
};

class DiagnosticsPanel : public QWidget {
public:
    explicit DiagnosticsPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Diagnostics")));
        auto* diagnostics = new QListWidget;
        diagnostics->setAccessibleName(QStringLiteral("Diagnostics list"));
        diagnostics->addItem(QStringLiteral("No diagnostics"));
        layout->addWidget(diagnostics);
    }
};

class ActivityPanel : public QWidget {
public:
    explicit ActivityPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        auto* tabs = new QTabWidget;

        auto* jobs = new QWidget;
        auto* jobsLayout = new QVBoxLayout(jobs);
        jobsLayout->addWidget(new QLabel(QStringLiteral("No active jobs")));
        auto* progress = new QProgressBar;
        progress->setRange(0, 100);
        progress->setValue(0);
        progress->setFormat(QStringLiteral("Ready"));
        jobsLayout->addWidget(progress);
        jobsLayout->addWidget(new QPushButton(QStringLiteral("Cancel selected job")));
        jobsLayout->addStretch();
        tabs->addTab(jobs, QStringLiteral("Jobs"));

        auto* messages = new QPlainTextEdit;
        messages->setReadOnly(true);
        messages->setPlaceholderText(QStringLiteral("Structured application and external-tool messages"));
        tabs->addTab(messages, QStringLiteral("Messages"));

        auto* agent = new QWidget;
        auto* agentLayout = new QVBoxLayout(agent);
        agentLayout->addWidget(new QLabel(QStringLiteral("Agent / ACP session")));
        auto* transcript = new QPlainTextEdit;
        transcript->setReadOnly(true);
        transcript->setPlaceholderText(QStringLiteral("Agent conversation and tool-call updates"));
        agentLayout->addWidget(transcript, 1);
        auto* prompt = new QLineEdit;
        prompt->setPlaceholderText(QStringLiteral("Ask the agent about the current project..."));
        agentLayout->addWidget(prompt);
        agentLayout->addWidget(new QPushButton(QStringLiteral("Send")));
        tabs->addTab(agent, QStringLiteral("Agent"));

        layout->addWidget(tabs);
    }
};

class CommandPalette : public QDialog {
public:
    explicit CommandPalette(const QList<QAction*>& actions, QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Command Palette"));
        setModal(true);
        resize(560, 420);

        auto* layout = new QVBoxLayout(this);
        search_ = new QLineEdit;
        search_->setPlaceholderText(QStringLiteral("Type to search commands..."));
        search_->setAccessibleName(QStringLiteral("Command palette search"));
        layout->addWidget(search_);

        list_ = new QListWidget;
        list_->setAccessibleName(QStringLiteral("Command palette results"));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(list_, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(search_, &QLineEdit::textChanged, this, [this, actions](const QString& text) {
            list_->clear();
            for (auto* action : actions) {
                if (text.isEmpty() || action->text().contains(text, Qt::CaseInsensitive)) {
                    auto* item = new QListWidgetItem(action->text(), list_);
                    item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(
                                                       reinterpret_cast<quintptr>(action)));
                }
            }
            if (list_->count() > 0) list_->setCurrentRow(0);
        });
        connect(search_, &QLineEdit::returnPressed, this, [this] {
            if (list_->currentItem() == nullptr) return;
            auto* action = reinterpret_cast<QAction*>(
                list_->currentItem()->data(Qt::UserRole).value<quintptr>());
            if (action != nullptr) {
                accept();
                action->trigger();
            }
        });
        search_->setFocus();
        search_->setText(QString());
    }

private:
    QLineEdit* search_ = nullptr;
    QListWidget* list_ = nullptr;
};

class DocumentWindow : public QMainWindow {
public:
    explicit DocumentWindow() {
        setWindowTitle(QStringLiteral("ICE"));
        setDockNestingEnabled(true);
        createMenus();
        createActivityDock();
        createDocuments();
        createStatusBar();
    }

    void setProject(ice::Project project) {
        project_ = std::make_unique<ice::ProjectSession>(std::move(project));
        projectPath_.clear();
        refreshEmbeddedPanels();
        refreshSimulationSchematics();
        refreshSchematic();
        refreshLayout();
        setWindowTitle(QStringLiteral("ICE - %1").arg(QString::fromStdString(project_->project().name)));
        statusBar()->showMessage(QStringLiteral("Project loaded"));
    }

private:
    ice::Project makeNewProject(const QString& name) const {
        ice::Project project;
        project.name = name.toStdString();
        project.identity = project.name;
        project.units = {};
        project.grid = 1;
        return project;
    }

    void newProject() {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("New Project"),
                                                QStringLiteral("Project name:"), QLineEdit::Normal,
                                                QStringLiteral("untitled"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        setProject(makeNewProject(name.trimmed()));
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Choose New Project Path"),
                                                       name.trimmed() + QStringLiteral(".ice.json"),
                                                       QStringLiteral("ICE projects (*.ice.json);;All files (*)"));
        if (!path.isEmpty()) {
            try {
                project_->save(path.toStdString());
                projectPath_ = path;
            } catch (const ice::ProjectError& error) {
                QMessageBox::critical(this, QStringLiteral("New Project"),
                                      QString::fromStdString(error.what()));
            }
        }
        statusBar()->showMessage(QStringLiteral("New project created"));
    }

    void openProject() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open ICE Project"),
                                                       QString(), QStringLiteral("ICE projects (*.ice.json);;All files (*)"));
        if (path.isEmpty()) return;
        try {
            projectPath_ = path;
            setProject(ice::Project::load(path.toStdString()));
            projectPath_ = path;
        } catch (const ice::ProjectError& error) {
            QMessageBox::critical(this, QStringLiteral("Open Project"),
                                  QString::fromStdString(error.what()));
        }
    }

    void saveProject() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Save Project"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        auto path = projectPath_;
        if (path.isEmpty())
            path = QFileDialog::getSaveFileName(this, QStringLiteral("Save ICE Project"),
                                                QStringLiteral("project.ice.json"),
                                                QStringLiteral("ICE projects (*.ice.json);;All files (*)"));
        if (path.isEmpty()) return;
        try {
            project_->save(path.toStdString());
            projectPath_ = path;
            statusBar()->showMessage(QStringLiteral("Project saved"));
        } catch (const ice::ProjectError& error) {
            QMessageBox::critical(this, QStringLiteral("Save Project"),
                                  QString::fromStdString(error.what()));
        }
    }

    void closeProject() {
        project_.reset();
        projectPath_.clear();
        refreshEmbeddedPanels();
        if (schematicCanvas_) schematicCanvas_->scene()->clear();
        if (layoutCanvas_) layoutCanvas_->scene()->clear();
        simulationOutput_->setPlainText(QStringLiteral("No project open."));
        schematicSelector_->clear();
        simulationPlot_->clear();
        simulationList_->clear();
        simulationEntries_.clear();
        setWindowTitle(QStringLiteral("ICE"));
        statusBar()->showMessage(QStringLiteral("Ready — no project open"));
    }

    void createSchematic() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Create Schematic"),
                                                QStringLiteral("Schematic name:"), QLineEdit::Normal,
                                                QStringLiteral("top"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        try {
            project_->execute("create schematic", [&](ice::Project& project) {
                const auto cell = ice::createSchematicCell(project, name.trimmed().toStdString());
                const auto* sourceDefinition = findSymbolDefinition(QStringLiteral("Voltage Source"));
                const auto symbol = ice::addSymbol(project, cell, "Voltage Source",
                                                   sourceDefinition ? sourceDefinition->svg.toStdString() : std::string{});
                auto& source = project.schematicCells.at(cell).symbols.at(symbol);
                if (sourceDefinition) {
                    for (auto pin : sourceDefinition->pins) {
                        pin.id = project.nextId();
                        pin.direction = pin.x < 0 ? ice::PinDirection::left :
                                        pin.x > 0 ? ice::PinDirection::right :
                                        pin.y > 0 ? ice::PinDirection::up : ice::PinDirection::down;
                        source.pins.push_back(std::move(pin));
                    }
                }
                ice::addSchematicInstance(project, cell, symbol, "V1", 0, 0);
            });
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Schematic created"));
        } catch (const ice::ProjectError& error) {
            QMessageBox::critical(this, QStringLiteral("Create Schematic"),
                                  QString::fromStdString(error.what()));
        }
    }

    void createLayoutCell() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layout Cell"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Create Layout Cell"),
                                                QStringLiteral("Layout cell name:"), QLineEdit::Normal,
                                                QStringLiteral("top"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        project_->execute("create layout cell", [&](ice::Project& project) {
            activeLayoutId_ = ice::createLayoutCell(project, name.trimmed().toStdString());
        });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout cell created"));
    }

    void createLayer() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layer"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Create Layer"),
                                                QStringLiteral("Layer name:"), QLineEdit::Normal,
                                                QStringLiteral("metal1"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        int number = 1;
        const auto existingLayers = project_->project().layers.size();
        number = static_cast<int>(existingLayers) + 1;
        project_->execute("create layer", [&](ice::Project& project) {
            ice::addLayer(project, name.trimmed().toStdString(), number);
        });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layer created"));
    }

    void refreshLayout() {
        if (!layoutCanvas_ || !project_) return;
        layoutCanvas_->scene()->clear();
        const auto& project = project_->project();
        auto cellIt = project.layoutCells.find(activeLayoutId_);
        if (cellIt == project.layoutCells.end()) {
            if (project.layoutCells.empty()) return;
            cellIt = project.layoutCells.begin();
            activeLayoutId_ = cellIt->first;
        }
        std::map<ice::ObjectId, QColor> colors;
        const std::vector<QColor> palette = {QColor("#36a3eb"), QColor("#e58b2a"),
                                             QColor("#65c466"), QColor("#bd72d0"),
                                             QColor("#e2d34f")};
        std::size_t colorIndex = 0;
        for (const auto& [id, layer] : project.layers)
            colors[id] = layer.visible ? palette[colorIndex++ % palette.size()] : QColor(90, 90, 90, 60);
        layoutCanvas_->setLayerColors(colors);
        for (const auto& [id, shape] : cellIt->second.shapes) {
            const auto layerColor = colors.contains(shape.layer) ? colors.at(shape.layer) : QColor("#777777");
            auto* item = layoutCanvas_->scene()->addRect(
                QRectF(shape.rectangle.x, -shape.rectangle.y - shape.rectangle.height,
                       shape.rectangle.width, shape.rectangle.height),
                QPen(layerColor, 1), QBrush(QColor(layerColor.red(), layerColor.green(), layerColor.blue(), 90)));
            item->setData(0, QVariant::fromValue<qulonglong>(id));
            item->setFlag(QGraphicsItem::ItemIsSelectable);
            item->setFlag(QGraphicsItem::ItemIsMovable);
            item->setToolTip(QStringLiteral("Shape %1").arg(id));
        }
        layoutCanvas_->fitInView(layoutCanvas_->scene()->itemsBoundingRect().adjusted(-100, -100, 100, 100),
                                 Qt::KeepAspectRatio);
    }

    void placeNetLabel(QPointF position) {
        if (!project_ || project_->project().schematicCells.empty()) return;
        position = nearestWireLabelPosition(position);
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Place Net Label"),
                                                QStringLiteral("Net name:"), QLineEdit::Normal,
                                                QStringLiteral("NET1"), &accepted);
        if (!accepted || name.trimmed().isEmpty()) return;
        const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                ? activeSchematicId_
                                : project_->project().schematicCells.begin()->first;
        project_->execute("place net label", [&](ice::Project& project) {
            auto& cell = project.schematicCells.at(cellId);
            const auto id = project.nextId();
            cell.nets.emplace(id, ice::Net{id, name.trimmed().toStdString(), {},
                                           static_cast<ice::Coordinate>(position.x()),
                                           static_cast<ice::Coordinate>(-position.y())});
        });
        refreshProject();
    }

    QPointF nearestWireLabelPosition(QPointF position) const {
        if (!schematicCanvas_ || !schematicCanvas_->scene()) return position;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        QPointF bestPosition = position;
        for (auto* item : schematicCanvas_->scene()->items()) {
            auto* wire = dynamic_cast<QGraphicsPathItem*>(item);
            if (!wire || wire->data(0).toString() != QStringLiteral("wire")) continue;
            const auto path = wire->path();
            for (int index = 1; index < path.elementCount(); ++index) {
                const auto first = path.elementAt(index - 1);
                const auto second = path.elementAt(index);
                const QPointF start(first.x, first.y);
                const QPointF end(second.x, second.y);
                const auto delta = end - start;
                const auto lengthSquared = QPointF::dotProduct(delta, delta);
                const auto fraction = lengthSquared > 0.0
                                          ? std::clamp(QPointF::dotProduct(position - start, delta) / lengthSquared,
                                                       qreal(0.0), qreal(1.0))
                                          : qreal(0.0);
                const auto candidate = start + delta * fraction;
                const auto distance = QLineF(position, candidate).length();
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestPosition = candidate;
                }
            }
        }
        if (bestDistance < std::numeric_limits<qreal>::max())
            return bestPosition + QPointF(0.0, -18.0);

        // If a wire is not currently rendered, keep the label near the
        // closest visible pin rather than falling back to the canvas origin.
        for (auto* item : schematicCanvas_->scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin")) continue;
            const auto distance = QLineF(position, item->scenePos()).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPosition = item->scenePos() + QPointF(0.0, -18.0);
            }
        }
        if (bestDistance < std::numeric_limits<qreal>::max())
            return bestPosition;
        return position;
    }

    void placeSymbol() {
        if (!project_ || project_->project().schematicCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Place Symbol"),
                                     QStringLiteral("Create a project and schematic first."));
            return;
        }
        SymbolLibraryDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto& definition = dialog.selected();
        const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                ? activeSchematicId_
                                : project_->project().schematicCells.begin()->first;
        const auto scenePosition = schematicCanvas_->mapToScene(schematicCanvas_->viewport()->rect().center());
        try {
            project_->execute("place symbol", [&](ice::Project& project) {
                const auto symbolId = ice::addSymbol(project, cellId, definition.name.toStdString(), definition.svg.toStdString());
                auto& symbol = project.schematicCells.at(cellId).symbols.at(symbolId);
                for (auto pin : definition.pins) {
                    pin.id = project.nextId();
                    pin.direction = pin.x < 0 ? ice::PinDirection::left :
                                    pin.x > 0 ? ice::PinDirection::right :
                                    pin.y > 0 ? ice::PinDirection::up : ice::PinDirection::down;
                    symbol.pins.push_back(std::move(pin));
                }
                ice::addSchematicInstance(project, cellId, symbolId, definition.name.toStdString(),
                                          static_cast<ice::Coordinate>(scenePosition.x()),
                                          static_cast<ice::Coordinate>(-scenePosition.y()));
            });
            refreshProject();
        } catch (const ice::ProjectError& error) {
            QMessageBox::critical(this, QStringLiteral("Place Symbol"),
                                  QString::fromStdString(error.what()));
        }
    }

    void runSimulation() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        simulationOutput_->setPlainText(QStringLiteral("Running simulation..."));
        documents_->setCurrentWidget(simulationPage_);
        QApplication::processEvents();
        ice::SimulationRequest request;
        request.analysis = analysisSelector_->currentData().toString().toStdString();
        request.simulator = simulatorSelector_->currentData().toString().toStdString();
        request.stop = stopTime_->value();
        request.step = stepTime_->value();
        for (const auto& line : directiveEditor_->toPlainText().split('\n'))
            if (!line.trimmed().isEmpty()) request.netlist.push_back(line.toStdString());
        const auto selectedCell = schematicSelector_->currentText();
        if (request.netlist.empty())
            request.netlist.push_back("* ICE schematic simulation: " + selectedCell.toStdString());

        const auto directive = directiveEditor_->toPlainText().trimmed();
        const auto tokens = directive.split(QRegularExpression(QStringLiteral("\\s+")),
                                            Qt::SkipEmptyParts);
        if (!tokens.isEmpty() && tokens.front().compare(QStringLiteral(".tran"), Qt::CaseInsensitive) == 0 &&
            tokens.size() >= 3) {
            bool stopOk = false;
            bool stepOk = false;
            const auto stop = tokens.at(1).toDouble(&stopOk);
            const auto step = tokens.at(2).toDouble(&stepOk);
            if (stopOk && stepOk && stop > 0.0 && step > 0.0) {
                request.analysis = "transient";
                request.stop = stop;
                request.step = step;
                analysisSelector_->setCurrentIndex(analysisSelector_->findData(QStringLiteral("transient")));
                stopTime_->setValue(stop);
                stepTime_->setValue(step);
            }
        }

        const auto result = ice::SimulationRunner(std::make_shared<ice::NgspiceSimulator>())
                                .run(request, std::chrono::seconds(10));
        std::ostringstream output;
        output << "State: " << ice::jobStateName(result.state) << "\n"
               << "Analysis: " << result.analysis << "\n"
               << "Simulator: " << result.simulator << " " << result.simulator_version << "\n";
        if (!result.error.empty()) output << "Error: " << result.error << "\n";
        if (!result.time.empty()) {
            output << "Samples: " << result.time.size() << "\n";
            const auto& trace = result.traces.front();
            output << "Trace: " << trace.name << "\n";
            output << "First sample: " << trace.values.front() << "\n";
            output << "Last sample: " << trace.values.back() << "\n";
        }
        simulationOutput_->setPlainText(QString::fromStdString(output.str()));
        simulationPlot_->setResult(result);
        const auto entryName = QStringLiteral("%1 — %2").arg(schematicSelector_->currentText(),
                                                              QString::fromStdString(result.analysis));
        SimulationEntry entry{entryName, schematicSelector_->currentData().toULongLong(), request, result};
        simulationEntries_.push_back(std::move(entry));
        simulationList_->addItem(entryName);
        simulationList_->setCurrentRow(simulationList_->count() - 1);
        statusBar()->showMessage(QStringLiteral("Simulation finished"));
    }

    void loadSimulationEntry(int row) {
        if (row < 0 || row >= static_cast<int>(simulationEntries_.size())) return;
        const auto& entry = simulationEntries_[static_cast<std::size_t>(row)];
        const auto schematicIndex = schematicSelector_->findData(
            QVariant::fromValue<qulonglong>(entry.schematicId));
        if (schematicIndex >= 0) schematicSelector_->setCurrentIndex(schematicIndex);
        simulatorSelector_->setCurrentIndex(simulatorSelector_->findData(
            QString::fromStdString(entry.request.simulator)));
        analysisSelector_->setCurrentIndex(analysisSelector_->findData(
            QString::fromStdString(entry.request.analysis)));
        stopTime_->setValue(entry.request.stop);
        stepTime_->setValue(entry.request.step);
        directiveEditor_->setPlainText(QString::fromStdString(joinNetlist(entry.request.netlist)));
        simulationOutput_->setPlainText(QString::fromStdString(simulationSummary(entry.result)));
        simulationPlot_->setResult(entry.result);
    }

    void refreshProject() {
        if (!project_) return;
        refreshEmbeddedPanels();
        refreshSimulationSchematics();
        refreshSchematic();
        refreshLayout();
    }

    void refreshEmbeddedPanels() {
        const ice::Project empty;
        const auto& project = project_ ? project_->project() : empty;
        if (schematicHierarchy_) schematicHierarchy_->setProject(project);
        if (layoutHierarchy_) layoutHierarchy_->setProject(project);
        if (layersPanel_) layersPanel_->setProject(project);
        if (schematicCellTabs_) {
            const auto selected = activeSchematicId_;
            schematicCellTabs_->clear();
            for (const auto& [id, cell] : project.schematicCells) {
                schematicCellTabs_->addTab(new QWidget, QString::fromStdString(cell.name));
                schematicCellTabs_->tabBar()->setTabData(schematicCellTabs_->count() - 1,
                                                        QVariant::fromValue<qulonglong>(id));
            }
            for (int index = 0; index < schematicCellTabs_->count(); ++index)
                if (schematicCellTabs_->tabBar()->tabData(index).toULongLong() == selected)
                    schematicCellTabs_->setCurrentIndex(index);
        }
        if (layoutCellTabs_) {
            const auto selected = activeLayoutId_;
            layoutCellTabs_->clear();
            for (const auto& [id, cell] : project.layoutCells) {
                layoutCellTabs_->addTab(new QWidget, QString::fromStdString(cell.name));
                layoutCellTabs_->tabBar()->setTabData(layoutCellTabs_->count() - 1,
                                                      QVariant::fromValue<qulonglong>(id));
            }
            for (int index = 0; index < layoutCellTabs_->count(); ++index)
                if (layoutCellTabs_->tabBar()->tabData(index).toULongLong() == selected)
                    layoutCellTabs_->setCurrentIndex(index);
        }
    }

    void refreshSimulationSchematics() {
        if (!schematicSelector_) return;
        schematicSelector_->clear();
        if (!project_) return;
        for (const auto& [id, cell] : project_->project().schematicCells)
            schematicSelector_->addItem(QString::fromStdString(cell.name),
                                        QVariant::fromValue<qulonglong>(id));
    }

    static std::optional<QPointF> pinScenePosition(const ice::Project& project,
                                                   const ice::SchematicCell& cell,
                                                   ice::ObjectId pinId) {
            for (const auto& [_, instance] : cell.instances) {
                const auto symbol = cell.symbols.find(instance.symbol);
                if (symbol == cell.symbols.end()) continue;
                for (const auto& pin : symbol->second.pins) {
                    if (pin.id != pinId) continue;
                    const auto local = rotatePoint({static_cast<qreal>(pin.x), -static_cast<qreal>(pin.y)},
                                                   instance.orientation);
                    return QPointF(instance.x + local.x(), -instance.y + local.y());
                }
            }
            return std::nullopt;
        }

    static void updateAutomaticNetLabelPositions(ice::Project& project) {
            for (auto& [_, cell] : project.schematicCells) {
                for (auto& [_, net] : cell.nets) {
                    if (net.pins.empty()) continue;
                    std::vector<QPointF> positions;
                    for (const auto pinId : net.pins)
                        if (const auto position = pinScenePosition(project, cell, pinId))
                            positions.push_back(*position);
                    if (positions.empty()) continue;
                    QPointF center;
                    for (const auto position : positions) center += position;
                    center /= static_cast<qreal>(positions.size());
                    net.x = static_cast<ice::Coordinate>(center.x());
                    net.y = static_cast<ice::Coordinate>(-center.y() + 18.0);
                }
            }
    }

    void refreshSchematic() {
            if (!schematicCanvas_) return;
            auto* scene = schematicCanvas_->scene();
            scene->clear();
            if (!project_ || project_->project().schematicCells.empty()) {
                scene->addText(QStringLiteral("No schematic cell.\nUse Project → Create Schematic."));
                return;
            }
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            cellIt = project_->project().schematicCells.begin();
        if (cellIt == project_->project().schematicCells.end()) return;
        const auto& cell = cellIt->second;
        scene->addText(QStringLiteral("Schematic: %1").arg(QString::fromStdString(cell.name)))->setPos(-900, -900);
        for (const auto& [netId, net] : cell.nets) {
            if (net.name.empty()) continue;
            auto* label = scene->addText(QString::fromStdString(net.name));
            label->setDefaultTextColor(Qt::darkBlue);
            label->setData(0, QStringLiteral("net-label"));
            label->setData(1, QVariant::fromValue<qulonglong>(netId));
            label->setPos(net.x, -net.y);
        }
        for (const auto& [instanceId, instance] : cell.instances) {
            auto symbolIt = cell.symbols.find(instance.symbol);
            if (symbolIt == cell.symbols.end()) continue;
            auto* group = new QGraphicsItemGroup;
            scene->addItem(group);
            group->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
            group->setData(0, QStringLiteral("symbol"));
            group->setData(1, QVariant::fromValue<qulonglong>(instanceId));
            group->setPos(instance.x, -instance.y);
            switch (instance.orientation) {
            case ice::Orientation::r90: group->setRotation(90); break;
            case ice::Orientation::r180: group->setRotation(180); break;
            case ice::Orientation::r270: group->setRotation(270); break;
            default: group->setRotation(0); break;
            }
            group->setToolTip(QString::fromStdString(instance.name));
            const auto artworkText = symbolIt->second.artwork.empty()
                                         ? std::string("<svg viewBox='0 0 120 60'><rect x='2' y='2' width='116' height='56' fill='none' stroke='black' stroke-width='4'/></svg>")
                                         : symbolIt->second.artwork;
            auto* renderer = new QSvgRenderer(QByteArray::fromStdString(artworkText), scene);
            auto* artwork = new QGraphicsSvgItem;
            artwork->setSharedRenderer(renderer);
            artwork->setParentItem(group);
            artwork->setPos(-60, -35);
            artwork->setFlag(QGraphicsItem::ItemIsSelectable, false);
            auto* label = new QGraphicsTextItem(QString::fromStdString(instance.name), group);
            label->setDefaultTextColor(Qt::black);
            label->setPos(-45, 38);
            for (const auto& pin : symbolIt->second.pins) {
                auto* pinItem = new QGraphicsEllipseItem(QRectF(-5, -5, 10, 10), group);
                pinItem->setPen(QPen(Qt::darkYellow));
                pinItem->setBrush(QBrush(Qt::yellow));
                pinItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
                pinItem->setData(0, QStringLiteral("pin"));
                pinItem->setData(1, QVariant::fromValue<qulonglong>(pin.id));
                pinItem->setData(2, static_cast<int>(rotateDirection(pin.direction, instance.orientation)));
                pinItem->setPos(pin.x, -pin.y);
                pinItem->setToolTip(QString::fromStdString(pin.name));
            }
        }
        struct PinView {
            QPointF point;
            ice::PinDirection direction;
        };
        std::map<ice::ObjectId, PinView> pinViews;
        for (auto* item : scene->items()) {
            if (item->data(0).toString() != QStringLiteral("pin")) continue;
            pinViews[item->data(1).toULongLong()] = {
                item->scenePos(), static_cast<ice::PinDirection>(item->data(2).toInt())};
        }
        for (const auto& [netId, net] : cell.nets) {
            if (net.pins.size() < 2) continue;
            const auto first = pinViews.find(net.pins.front());
            const auto second = pinViews.find(net.pins.back());
            if (first != pinViews.end() && second != pinViews.end()) {
                auto path = routeWire(first->second.point, first->second.direction,
                                      second->second.point, second->second.direction, scene,
                                      net.pins.front(), net.pins.back());
                auto* wire = scene->addPath(path, QPen(QColor("#277da1"), 2));
                wire->setZValue(-1);
                wire->setData(0, QStringLiteral("wire"));
            }
        }
    }

    QPainterPath routeWire(QPointF start, ice::PinDirection startDirection,
                           QPointF target, ice::PinDirection targetDirection,
                           QGraphicsScene* scene, ice::ObjectId startPin,
                           ice::ObjectId targetPin) const {
        constexpr qreal minimum = 30.0;
        const auto startVector = directionVector(startDirection);
        const auto targetVector = directionVector(targetDirection);
        const QPointF startExit = start + startVector * minimum;
        // The target entry point must be outside the symbol. The final route
        // segment then travels opposite the pin's outward-facing direction.
        const QPointF targetEntry = target + targetVector * minimum;
        const auto key = [](int node, int direction) {
            return std::pair<int, int>{node, direction};
        };
        std::vector<QRectF> obstacles;
        for (auto* item : scene->items()) {
            if (item->data(0).toString() == QStringLiteral("symbol") &&
                item->data(1).toULongLong() != startPin &&
                item->data(1).toULongLong() != targetPin)
                obstacles.push_back(item->sceneBoundingRect().adjusted(-12, -12, 12, 12));
        }
        const auto blocked = [&](QPointF point) {
            return std::any_of(obstacles.begin(), obstacles.end(),
                               [&](const QRectF& obstacle) { return obstacle.contains(point); });
        };
        const auto directionIndex = [](QPointF vector) {
            if (vector.x() < 0) return 0;
            if (vector.x() > 0) return 1;
            if (vector.y() < 0) return 2;
            return 3;
        };
        std::vector<qreal> xCoordinates{startExit.x(), targetEntry.x(), start.x(), target.x()};
        std::vector<qreal> yCoordinates{startExit.y(), targetEntry.y(), start.y(), target.y()};
        for (const auto& obstacle : obstacles) {
            xCoordinates.push_back(obstacle.left());
            xCoordinates.push_back(obstacle.right());
            yCoordinates.push_back(obstacle.top());
            yCoordinates.push_back(obstacle.bottom());
        }
        const auto uniqueCoordinates = [](std::vector<qreal>& coordinates) {
            std::sort(coordinates.begin(), coordinates.end());
            coordinates.erase(std::unique(coordinates.begin(), coordinates.end(),
                                          [](qreal left, qreal right) {
                                              return qFuzzyCompare(left + 1.0, right + 1.0);
                                          }),
                              coordinates.end());
        };
        uniqueCoordinates(xCoordinates);
        uniqueCoordinates(yCoordinates);
        std::vector<QPointF> nodes;
        for (qreal x : xCoordinates)
            for (qreal y : yCoordinates)
                if (!blocked({x, y})) nodes.emplace_back(x, y);
        const auto nodeIndex = [&nodes](QPointF point) {
            for (std::size_t index = 0; index < nodes.size(); ++index)
                if (qFuzzyCompare(nodes[index].x() + 1.0, point.x() + 1.0) &&
                    qFuzzyCompare(nodes[index].y() + 1.0, point.y() + 1.0))
                    return static_cast<int>(index);
            return -1;
        };
        const int startNode = nodeIndex(startExit);
        const int targetNode = nodeIndex(targetEntry);
        std::vector<std::vector<int>> adjacency(nodes.size());
        const auto clearSegment = [&blocked](QPointF from, QPointF to) {
            const int samples = std::max(2, static_cast<int>(std::ceil(
                std::max(std::abs(to.x() - from.x()), std::abs(to.y() - from.y())) / 10.0)));
            for (int index = 1; index < samples; ++index) {
                const qreal fraction = static_cast<qreal>(index) / samples;
                if (blocked(from + (to - from) * fraction)) return false;
            }
            return true;
        };
        for (std::size_t first = 0; first < nodes.size(); ++first) {
            for (std::size_t second = first + 1; second < nodes.size(); ++second) {
                const bool sameX = qFuzzyCompare(nodes[first].x() + 1.0, nodes[second].x() + 1.0);
                const bool sameY = qFuzzyCompare(nodes[first].y() + 1.0, nodes[second].y() + 1.0);
                if ((sameX || sameY) && clearSegment(nodes[first], nodes[second])) {
                    adjacency[first].push_back(static_cast<int>(second));
                    adjacency[second].push_back(static_cast<int>(first));
                }
            }
        }
        struct QueueNode {
            int cost;
            int length;
            int node;
            int direction;
            bool operator>(const QueueNode& other) const {
                return std::tie(cost, length) > std::tie(other.cost, other.length);
            }
        };
        const int firstDirection = directionIndex(startVector);
        const int finalDirection = directionIndex(-targetVector);
        std::priority_queue<QueueNode, std::vector<QueueNode>, std::greater<>> queue;
        std::map<std::pair<int, int>, std::pair<int, int>> best;
        std::map<std::pair<int, int>, std::pair<int, int>> parent;
        queue.push({0, 0, startNode, firstDirection});
        best[key(startNode, firstDirection)] = {0, 0};
        std::pair<int, int> endState{};
        bool found = false;
        while (!queue.empty()) {
            auto current = queue.top();
            queue.pop();
            if (current.node == targetNode && current.direction == finalDirection) {
                endState = key(current.node, current.direction);
                found = true;
                break;
            }
            for (const int neighbor : adjacency[current.node]) {
                const auto delta = nodes[neighbor] - nodes[current.node];
                const int direction = directionIndex(delta);
                const int length = current.length + static_cast<int>(std::abs(delta.x()) + std::abs(delta.y()));
                const int cost = current.cost + (direction == current.direction ? 0 : 100000) +
                                 static_cast<int>(std::abs(delta.x()) + std::abs(delta.y()));
                const auto state = key(neighbor, direction);
                auto it = best.find(state);
                const auto candidate = std::make_pair(cost, length);
                if (it != best.end() && it->second <= candidate)
                    continue;
                best[state] = candidate;
                parent[state] = key(current.node, current.direction);
                queue.push({cost, length, neighbor, direction});
            }
        }
        std::vector<QPointF> points{start, startExit};
        if (found) {
            std::vector<QPointF> middle;
            auto state = endState;
            while (state != key(startNode, firstDirection)) {
                middle.push_back(nodes[state.first]);
                state = parent.at(state);
            }
            std::reverse(middle.begin(), middle.end());
            points.insert(points.end(), middle.begin(), middle.end());
        } else {
            points.push_back({targetEntry.x(), startExit.y()});
            points.push_back(targetEntry);
        }
        points.push_back(target);
        QPainterPath path(points.front());
        for (std::size_t index = 1; index < points.size(); ++index)
            if (points[index] != points[index - 1]) path.lineTo(points[index]);
        return path;
    }

    QAction* addCommand(const QString& text, const QKeySequence& shortcut = {}) {
        auto* action = new QAction(text, this);
        if (!shortcut.isEmpty()) action->setShortcut(shortcut);
        commands_.append(action);
        return action;
    }

    void createMenus() {
        auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        auto* newAction = addCommand(QStringLiteral("New Project"), QKeySequence::New);
        auto* openAction = addCommand(QStringLiteral("Open Project"), QKeySequence::Open);
        auto* saveAction = addCommand(QStringLiteral("Save Project"), QKeySequence::Save);
        fileMenu->addAction(newAction);
        fileMenu->addAction(openAction);
        fileMenu->addAction(saveAction);
        fileMenu->addSeparator();
        auto* closeAction = addCommand(QStringLiteral("Close Project"), QKeySequence::Close);
        fileMenu->addAction(closeAction);
        fileMenu->addSeparator();
        auto* exitAction = addCommand(QStringLiteral("Exit"), QKeySequence::Quit);
        fileMenu->addAction(exitAction);
        connect(exitAction, &QAction::triggered, this, [this] { close(); });

        auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        editMenu->addAction(addCommand(QStringLiteral("Undo"), QKeySequence::Undo));
        editMenu->addAction(addCommand(QStringLiteral("Redo"), QKeySequence::Redo));
        editMenu->addSeparator();
        editMenu->addAction(addCommand(QStringLiteral("Cut"), QKeySequence::Cut));
        editMenu->addAction(addCommand(QStringLiteral("Copy"), QKeySequence::Copy));
        editMenu->addAction(addCommand(QStringLiteral("Paste"), QKeySequence::Paste));
        editMenu->addAction(addCommand(QStringLiteral("Delete"), QKeySequence::Delete));

        auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        viewMenu->addAction(addCommand(QStringLiteral("Project / Hierarchy")));
        viewMenu->addAction(addCommand(QStringLiteral("Properties / Diagnostics")));
        viewMenu->addAction(addCommand(QStringLiteral("Activity / Agent")));
        viewMenu->addSeparator();
        auto* paletteAction = addCommand(QStringLiteral("Command Palette"), QKeySequence(QStringLiteral("Ctrl+K")));
        viewMenu->addAction(paletteAction);
        connect(paletteAction, &QAction::triggered, this, &DocumentWindow::showCommandPalette);

        auto* projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
        auto* createSchematicAction = addCommand(QStringLiteral("Create Schematic"));
        projectMenu->addAction(createSchematicAction);
        auto* createLayoutAction = addCommand(QStringLiteral("Create Layout Cell"));
        projectMenu->addAction(createLayoutAction);
        auto* createLayerAction = addCommand(QStringLiteral("Create Layer"));
        projectMenu->addAction(createLayerAction);
        auto* placeSymbolAction = addCommand(QStringLiteral("Place Symbol"), QKeySequence(QStringLiteral("P")));
        projectMenu->addAction(placeSymbolAction);
        projectMenu->addSeparator();
        projectMenu->addAction(addCommand(QStringLiteral("Validate Project")));
        projectMenu->addAction(addCommand(QStringLiteral("Project Settings")));
        projectMenu->addAction(addCommand(QStringLiteral("Manage Packages")));

        auto* toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
        auto* simulationAction = addCommand(QStringLiteral("Run Simulation"), QKeySequence(QStringLiteral("F6")));
        toolsMenu->addAction(simulationAction);
        toolsMenu->addAction(addCommand(QStringLiteral("Run DRC")));
        toolsMenu->addAction(addCommand(QStringLiteral("Run LVS")));
        toolsMenu->addAction(addCommand(QStringLiteral("Run PEX")));
        toolsMenu->addAction(addCommand(QStringLiteral("Open Python Console")));

        auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        helpMenu->addAction(addCommand(QStringLiteral("Keyboard Shortcuts")));
        helpMenu->addAction(addCommand(QStringLiteral("About ICE")));

        connect(newAction, &QAction::triggered, this, &DocumentWindow::newProject);
        connect(openAction, &QAction::triggered, this, &DocumentWindow::openProject);
        connect(saveAction, &QAction::triggered, this, &DocumentWindow::saveProject);
        connect(closeAction, &QAction::triggered, this, &DocumentWindow::closeProject);
        connect(createSchematicAction, &QAction::triggered, this, &DocumentWindow::createSchematic);
        connect(createLayoutAction, &QAction::triggered, this, &DocumentWindow::createLayoutCell);
        connect(createLayerAction, &QAction::triggered, this, &DocumentWindow::createLayer);
        connect(placeSymbolAction, &QAction::triggered, this, &DocumentWindow::placeSymbol);
        connect(simulationAction, &QAction::triggered, this, &DocumentWindow::runSimulation);
    }

    void showCommandPalette() {
        CommandPalette palette(commands_, this);
        palette.exec();
    }

    void createProjectDock() {
        auto* dock = new QDockWidget(QStringLiteral("Project / Hierarchy"), this);
        dock->setObjectName(QStringLiteral("projectHierarchyDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        projectPanel_ = new ProjectPanel;
        dock->setWidget(projectPanel_);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
    }

    void createInspectorDock() {
        auto* dock = new QDockWidget(QStringLiteral("Properties / Diagnostics"), this);
        dock->setObjectName(QStringLiteral("inspectorDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        auto* tabs = new QTabWidget;
        tabs->addTab(new PropertiesPanel, QStringLiteral("Properties"));
        tabs->addTab(new DiagnosticsPanel, QStringLiteral("Diagnostics"));
        dock->setWidget(tabs);
        addDockWidget(Qt::RightDockWidgetArea, dock);
    }

    void createActivityDock() {
        auto* dock = new QDockWidget(QStringLiteral("Activity / Agent"), this);
        dock->setObjectName(QStringLiteral("activityDock"));
        dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        dock->setWidget(new ActivityPanel);
        addDockWidget(Qt::BottomDockWidgetArea, dock);
    }

    void createDocuments() {
        documents_ = new QTabWidget;
        auto* documents = documents_;
        documents->setTabsClosable(false);
        auto* schematicPage = new QWidget;
        auto* schematicLayout = new QVBoxLayout(schematicPage);
        schematicLayout->setContentsMargins(0, 0, 0, 0);
        schematicLayout->setSpacing(0);
        schematicLayout->addWidget(createEditorToolbar(false));
        schematicCellTabs_ = new QTabWidget;
        schematicCellTabs_->setDocumentMode(true);
        auto* schematicContent = new QWidget;
        auto* schematicContentLayout = new QHBoxLayout(schematicContent);
        schematicHierarchy_ = new ProjectPanel(false, false, false);
        schematicContentLayout->addWidget(schematicHierarchy_, 0);
        auto* schematicEditorColumn = new QWidget;
        auto* schematicEditorLayout = new QVBoxLayout(schematicEditorColumn);
        schematicEditorLayout->setContentsMargins(0, 0, 0, 0);
        schematicEditorLayout->setSpacing(0);
        schematicEditorLayout->addWidget(schematicCellTabs_);
        schematicEditorLayout->addWidget(makeEditor(QStringLiteral("Schematic editor"),
                                                    QStringLiteral("Schematic view\nPan, zoom, grid, snapping, symbols, pins, nets, and hierarchy"),
                                                    false), 1);
        schematicContentLayout->addWidget(schematicEditorColumn, 1);
        auto* schematicProperties = new QTabWidget;
        schematicProperties->addTab(new PropertiesPanel, QStringLiteral("Properties"));
        schematicProperties->addTab(new DiagnosticsPanel, QStringLiteral("Diagnostics"));
        schematicContentLayout->addWidget(schematicProperties, 0);
        schematicLayout->addWidget(schematicContent, 1);
        documents->addTab(schematicPage, QStringLiteral("Schematic"));

        auto* layoutPage = new QWidget;
        auto* layoutLayout = new QVBoxLayout(layoutPage);
        layoutLayout->setContentsMargins(0, 0, 0, 0);
        layoutLayout->setSpacing(0);
        layoutLayout->addWidget(createEditorToolbar(true));
        layoutCellTabs_ = new QTabWidget;
        layoutCellTabs_->setDocumentMode(true);
        auto* layoutContent = new QWidget;
        auto* layoutContentLayout = new QHBoxLayout(layoutContent);
        layoutHierarchy_ = new ProjectPanel(true, false);
        layoutContentLayout->addWidget(layoutHierarchy_, 0);
        auto* layoutEditorColumn = new QWidget;
        auto* layoutEditorLayout = new QVBoxLayout(layoutEditorColumn);
        layoutEditorLayout->setContentsMargins(0, 0, 0, 0);
        layoutEditorLayout->setSpacing(0);
        layoutEditorLayout->addWidget(layoutCellTabs_);
        layoutEditorLayout->addWidget(makeEditor(QStringLiteral("Layout editor"),
                                                 QStringLiteral("Layout view\nLayers, shapes, ports, instances, snapping, and geometry"),
                                                 true), 1);
        layoutContentLayout->addWidget(layoutEditorColumn, 1);
        layersPanel_ = new LayersPanel;
        layoutContentLayout->addWidget(layersPanel_, 0);
        layoutLayout->addWidget(layoutContent, 1);
        documents->addTab(layoutPage, QStringLiteral("Layout"));

        simulationPage_ = new QWidget;
        auto* simulationLayout = new QVBoxLayout(simulationPage_);
        auto* simulationSplit = new QSplitter(Qt::Horizontal);
        simulationList_ = new QListWidget;
        simulationList_->setMinimumWidth(180);
        simulationSplit->addWidget(simulationList_);
        auto* simulationControls = new QWidget;
        auto* simulationControlsLayout = new QVBoxLayout(simulationControls);
        auto* simulationForm = new QFormLayout;
        schematicSelector_ = new QComboBox;
        simulatorSelector_ = new QComboBox;
        simulatorSelector_->addItem(QStringLiteral("ngspice"), QStringLiteral("ngspice"));
        analysisSelector_ = new QComboBox;
        analysisSelector_->addItem(QStringLiteral("Transient"), QStringLiteral("transient"));
        analysisSelector_->addItem(QStringLiteral("DC sweep"), QStringLiteral("dc"));
        analysisSelector_->addItem(QStringLiteral("AC sweep"), QStringLiteral("ac"));
        analysisSelector_->addItem(QStringLiteral("Operating point"), QStringLiteral("op"));
        stopTime_ = new QDoubleSpinBox;
        stopTime_->setRange(1e-12, 1e9);
        stopTime_->setDecimals(9);
        stopTime_->setValue(1.0);
        stepTime_ = new QDoubleSpinBox;
        stepTime_->setRange(1e-12, 1e9);
        stepTime_->setDecimals(9);
        stepTime_->setValue(0.01);
        simulationForm->addRow(QStringLiteral("Schematic"), schematicSelector_);
        simulationForm->addRow(QStringLiteral("Simulator"), simulatorSelector_);
        simulationForm->addRow(QStringLiteral("Analysis"), analysisSelector_);
        simulationForm->addRow(QStringLiteral("Stop time / end"), stopTime_);
        simulationForm->addRow(QStringLiteral("Step / increment"), stepTime_);
        simulationControlsLayout->addLayout(simulationForm);
        directiveEditor_ = new QPlainTextEdit;
        directiveEditor_->setPlainText(QStringLiteral(".tran 1.0 0.01"));
        directiveEditor_->setPlaceholderText(QStringLiteral("SPICE directives, for example: .tran 1.0 0.01"));
        directiveEditor_->setMaximumHeight(100);
        simulationControlsLayout->addWidget(new QLabel(QStringLiteral("SPICE directives")));
        simulationControlsLayout->addWidget(directiveEditor_);
        auto* runButton = new QPushButton(QStringLiteral("Run simulation"));
        simulationControlsLayout->addWidget(runButton);
        simulationPlot_ = new SimulationPlot;
        simulationControlsLayout->addWidget(simulationPlot_, 1);
        simulationOutput_ = new QPlainTextEdit;
        simulationOutput_->setReadOnly(true);
        simulationOutput_->setMaximumHeight(130);
        simulationOutput_->setPlaceholderText(QStringLiteral("Simulation status and result summary"));
        simulationControlsLayout->addWidget(simulationOutput_);
        simulationSplit->addWidget(simulationControls);
        simulationLayout->addWidget(simulationSplit, 1);
        connect(runButton, &QPushButton::clicked, this, &DocumentWindow::runSimulation);
        connect(simulationList_, &QListWidget::currentRowChanged, this, &DocumentWindow::loadSimulationEntry);
        connect(schematicCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0) return;
            activeSchematicId_ = schematicCellTabs_->tabBar()->tabData(index).toULongLong();
            refreshSchematic();
        });
        connect(layoutCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0) return;
            activeLayoutId_ = layoutCellTabs_->tabBar()->tabData(index).toULongLong();
            refreshLayout();
        });
        connect(analysisSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            const auto analysis = analysisSelector_->currentData().toString();
            if (analysis == QStringLiteral("transient"))
                directiveEditor_->setPlainText(QStringLiteral(".tran %1 %2").arg(stopTime_->value()).arg(stepTime_->value()));
            else if (analysis == QStringLiteral("dc"))
                directiveEditor_->setPlainText(QStringLiteral(".dc V1 0 1 0.01"));
            else if (analysis == QStringLiteral("ac"))
                directiveEditor_->setPlainText(QStringLiteral(".ac dec 10 1 1e6"));
            else
                directiveEditor_->setPlainText(QStringLiteral(".op"));
        });
        documents->addTab(simulationPage_, QStringLiteral("Simulation"));
        documents->addTab(placeholder(QStringLiteral("Verification results"),
                                       QStringLiteral("DRC, LVS, PEX, diagnostics, and provenance")),
                          QStringLiteral("Results"));
        setCentralWidget(documents);
    }

    QWidget* makeEditor(const QString& title, const QString& text, bool layoutEditor) {
        auto* editor = new QWidget;
        auto* editorLayout = new QVBoxLayout(editor);
        Q_UNUSED(title);
        Q_UNUSED(layoutEditor);
        auto* splitter = new QSplitter(Qt::Vertical, editor);
        auto* scene = new QGraphicsScene(splitter);
        QGraphicsView* view = nullptr;
        if (!layoutEditor) {
            schematicCanvas_ = new SchematicCanvas;
            schematicCanvas_->setCallbacks(
                [this](ice::ObjectId id, QPointF position) {
                    if (!project_) return;
                    project_->execute("move symbol", [&](ice::Project& project) {
                        for (auto& [_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                it->second.x = static_cast<ice::Coordinate>(position.x());
                                it->second.y = static_cast<ice::Coordinate>(-position.y());
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                },
                [this](ice::ObjectId id) {
                    if (!project_) return;
                    project_->execute("rotate symbol", [&](ice::Project& project) {
                            for (auto& [_, cell] : project.schematicCells)
                                if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                    switch (it->second.orientation) {
                                    case ice::Orientation::r0: it->second.orientation = ice::Orientation::r90; break;
                                    case ice::Orientation::r90: it->second.orientation = ice::Orientation::r180; break;
                                    case ice::Orientation::r180: it->second.orientation = ice::Orientation::r270; break;
                                    default: it->second.orientation = ice::Orientation::r0; break;
                                    }
                                    updateAutomaticNetLabelPositions(project);
                                    return;
                                }
                    });
                    refreshProject();
                },
                [this](ice::ObjectId firstPin, ice::ObjectId secondPin) {
                    if (!project_) return;
                    project_->execute("connect pins", [&](ice::Project& project) {
                        auto cellIt = project.schematicCells.find(activeSchematicId_);
                        if (cellIt == project.schematicCells.end()) cellIt = project.schematicCells.begin();
                        auto& cell = cellIt->second;
                        auto netIt = std::find_if(cell.nets.begin(), cell.nets.end(), [&](const auto& entry) {
                            return std::find(entry.second.pins.begin(), entry.second.pins.end(), firstPin) != entry.second.pins.end();
                        });
                        if (netIt == cell.nets.end()) {
                            auto netId = project.nextId();
                            cell.nets.emplace(netId, ice::Net{netId, "N" + std::to_string(netId), {firstPin, secondPin}});
                        } else if (std::find(netIt->second.pins.begin(), netIt->second.pins.end(), secondPin) == netIt->second.pins.end())
                            netIt->second.pins.push_back(secondPin);
                        updateAutomaticNetLabelPositions(project);
                    });
                    refreshProject();
                },
                [this](QPointF position) { placeNetLabel(position); },
                [this] {
                    if (!project_) return;
                    project_->markDirty();
                    refreshSchematic();
                });
            schematicCanvas_->deleteSelected_ = [this](std::vector<ice::ObjectId> ids) {
                if (!project_) return;
                project_->execute("delete symbols", [&](ice::Project& project) {
                    for (auto& [_, cell] : project.schematicCells) {
                        std::vector<ice::ObjectId> removedPins;
                        for (auto id : ids) {
                            auto instance = cell.instances.find(id);
                            if (instance == cell.instances.end()) continue;
                            auto symbol = cell.symbols.find(instance->second.symbol);
                            if (symbol != cell.symbols.end())
                                for (const auto& pin : symbol->second.pins) removedPins.push_back(pin.id);
                            cell.instances.erase(instance);
                        }
                        for (auto& [_, net] : cell.nets)
                            net.pins.erase(std::remove_if(net.pins.begin(), net.pins.end(), [&](ice::ObjectId pin) {
                                return std::find(removedPins.begin(), removedPins.end(), pin) != removedPins.end();
                            }), net.pins.end());
                        for (auto it = cell.nets.begin(); it != cell.nets.end();)
                            if (it->second.pins.size() < 2) it = cell.nets.erase(it);
                            else ++it;
                    }
                });
                refreshProject();
            };
            view = schematicCanvas_;
            schematicCanvas_->scene()->addText(text);
        } else {
            layoutCanvas_ = new LayoutCanvas;
            layoutCanvas_->setCallbacks(
                [this](QPointF first, QPointF second) {
                    if (!project_ || project_->project().layers.empty()) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Create a layer before drawing shapes."));
                        return;
                    }
                    const auto cellId = project_->project().layoutCells.contains(activeLayoutId_)
                                            ? activeLayoutId_
                                            : (project_->project().layoutCells.empty()
                                                   ? 0 : project_->project().layoutCells.begin()->first);
                    if (cellId == 0) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Create a layout cell before drawing shapes."));
                        return;
                    }
                    const auto rectangle = QRectF(first, second).normalized();
                    const auto layerId = project_->project().layers.begin()->first;
                    project_->execute("create layout rectangle", [&](ice::Project& project) {
                        ice::addRectangle(project, cellId, layerId,
                                          {static_cast<ice::Coordinate>(rectangle.left()),
                                           static_cast<ice::Coordinate>(-rectangle.bottom()),
                                           static_cast<ice::Coordinate>(rectangle.width()),
                                           static_cast<ice::Coordinate>(rectangle.height())});
                    });
                    refreshProject();
                },
                [this](ice::ObjectId shapeId, QPointF delta) {
                    if (!project_) return;
                    project_->execute("move layout shape", [&](ice::Project& project) {
                        if (!project.layoutCells.contains(activeLayoutId_)) return;
                        ice::moveRectangle(project, activeLayoutId_, shapeId,
                                           static_cast<ice::Coordinate>(delta.x()),
                                           static_cast<ice::Coordinate>(-delta.y()));
                    });
                    refreshProject();
                },
                [this](std::vector<ice::ObjectId> ids) {
                    if (!project_) return;
                    project_->execute("delete layout shapes", [&](ice::Project& project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end()) return;
                        for (const auto id : ids) cellIt->second.shapes.erase(id);
                    });
                    refreshProject();
                },
                [this] {
                    if (project_) project_->markDirty();
                });
            view = layoutCanvas_;
        }
        view->setBackgroundBrush(QColor(QStringLiteral("#20252b")));
        view->setAlignment(Qt::AlignCenter);
        view->setAccessibleName(title);
        splitter->addWidget(view);
        auto* editorStatus = new QLabel(QStringLiteral("Grid: 1 dbu    Snap: on    Selection: none"));
        editorStatus->setMargin(6);
        splitter->addWidget(editorStatus);
        splitter->setStretchFactor(0, 1);
        editorLayout->addWidget(splitter, 1);
        return editor;
    }

    QToolBar* createEditorToolbar(bool layoutEditor) {
        auto* toolbar = new QToolBar(layoutEditor ? QStringLiteral("Layout tools")
                                                  : QStringLiteral("Schematic tools"), this);
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setAccessibleName(layoutEditor ? QStringLiteral("Layout toolbar")
                                                 : QStringLiteral("Schematic toolbar"));

        auto addTool = [toolbar](QStyle::StandardPixmap icon, const QString& name) {
            auto* action = toolbar->addAction(toolbar->style()->standardIcon(icon), QString());
            action->setToolTip(name);
            action->setStatusTip(name);
            return action;
        };

        auto* selectTool = addTool(QStyle::SP_ArrowUp, QStringLiteral("Select"));
        if (layoutEditor) {
            connect(selectTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) layoutCanvas_->setTool(LayoutCanvas::Tool::select);
            });
        }
        addTool(QStyle::SP_DialogResetButton, QStringLiteral("Pan"));
        addTool(QStyle::SP_BrowserReload, QStringLiteral("Zoom to fit"));
        toolbar->addSeparator();
        addTool(QStyle::SP_FileDialogDetailedView, QStringLiteral("Grid and snapping"));
        addTool(QStyle::SP_DesktopIcon, QStringLiteral("Toggle visibility"));
        toolbar->addSeparator();
        if (layoutEditor) {
            auto* rectangleTool = addTool(QStyle::SP_FileDialogNewFolder, QStringLiteral("Create rectangle"));
            connect(rectangleTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) layoutCanvas_->setTool(LayoutCanvas::Tool::rectangle);
            });
            addTool(QStyle::SP_DirOpenIcon, QStringLiteral("Create polygon"));
            addTool(QStyle::SP_DialogOpenButton, QStringLiteral("Create port"));
            addTool(QStyle::SP_DialogSaveButton, QStringLiteral("Create instance"));
            toolbar->addSeparator();
            addTool(QStyle::SP_ArrowBack, QStringLiteral("Rotate counterclockwise"));
            addTool(QStyle::SP_ArrowForward, QStringLiteral("Rotate clockwise"));
        } else {
            addTool(QStyle::SP_FileDialogNewFolder, QStringLiteral("Place symbol"));
            addTool(QStyle::SP_DirOpenIcon, QStringLiteral("Draw wire"));
            auto* netLabelTool = addTool(QStyle::SP_DialogOpenButton, QStringLiteral("Place net label"));
            connect(netLabelTool, &QAction::triggered, this, [this] {
                if (!project_ || project_->project().schematicCells.empty()) return;
                schematicCanvas_->setNetLabelMode(true);
                statusBar()->showMessage(QStringLiteral("Click in the schematic to place a net label"));
            });
            addTool(QStyle::SP_DialogSaveButton, QStringLiteral("Edit properties"));
        }
        toolbar->addSeparator();
        addTool(QStyle::SP_DialogCancelButton, QStringLiteral("Delete"));
        return toolbar;
    }

    void createStatusBar() {
        statusBar()->showMessage(QStringLiteral("Ready — no project open"));
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("DBU: 1 nm")));
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Grid: 1")));
        statusBar()->addPermanentWidget(new QLabel(QStringLiteral("Selection: none")));
    }

    struct SimulationEntry {
        QString name;
        ice::ObjectId schematicId = 0;
        ice::SimulationRequest request;
        ice::SimulationResult result;
    };

    ProjectPanel* projectPanel_ = nullptr;
    ProjectPanel* schematicHierarchy_ = nullptr;
    ProjectPanel* layoutHierarchy_ = nullptr;
    LayersPanel* layersPanel_ = nullptr;
    QTabWidget* schematicCellTabs_ = nullptr;
    QTabWidget* layoutCellTabs_ = nullptr;
    ice::ObjectId activeSchematicId_ = 0;
    ice::ObjectId activeLayoutId_ = 0;
    std::unique_ptr<ice::ProjectSession> project_;
    QString projectPath_;
    QTabWidget* documents_ = nullptr;
    SchematicCanvas* schematicCanvas_ = nullptr;
    LayoutCanvas* layoutCanvas_ = nullptr;
    QPlainTextEdit* simulationOutput_ = nullptr;
    QWidget* simulationPage_ = nullptr;
    QComboBox* schematicSelector_ = nullptr;
    QComboBox* simulatorSelector_ = nullptr;
    QComboBox* analysisSelector_ = nullptr;
    QDoubleSpinBox* stopTime_ = nullptr;
    QDoubleSpinBox* stepTime_ = nullptr;
    QPlainTextEdit* directiveEditor_ = nullptr;
    SimulationPlot* simulationPlot_ = nullptr;
    QListWidget* simulationList_ = nullptr;
    std::vector<SimulationEntry> simulationEntries_;
    QList<QAction*> commands_;
};

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ICE"));
    app.setApplicationVersion(QString::fromStdString(ice::version()));

    DocumentWindow window;
    if (argc == 3 && std::string(argv[1]) == "--project") {
        try {
            window.setProject(ice::Project::load(argv[2]));
        } catch (const ice::ProjectError&) {
            window.statusBar()->showMessage(QStringLiteral("Unable to load project"));
        }
    }
    window.resize(1440, 900);
    window.show();
    return app.exec();
}
