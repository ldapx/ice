#include "icelib/core.hpp"
#include "icelib/interchange.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/simulation.hpp"

#include "appearance.hpp"
#include "icons.hpp"
#include "package_ui.hpp"
#include "simulation_plot.hpp"
#include "symbol_ui.hpp"
#include "ui_colors.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsColorizeEffect>
#include <QGraphicsEllipseItem>
#include <QGraphicsItemGroup>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <QtSvgWidgets/QGraphicsSvgItem>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <utility>

namespace {

void setLucideIcon(QAction *action, const QString &name) {
    action->setProperty("lucideIcon", name);
    action->setIcon(ice::lucideIcon(name));
}

void setLucideIcon(QAbstractButton *button, const QString &name, int size = 18) {
    button->setProperty("lucideIcon", name);
    button->setIcon(ice::lucideIcon(name, size));
    button->setIconSize(QSize(size, size));
}

QLabel *heading(const QString &text) {
    auto *label = new QLabel(text);
    label->setStyleSheet(QStringLiteral("font-weight: bold; padding: 4px 2px;"));
    return label;
}

QWidget *placeholder(const QString &title, const QString &description) {
    auto *widget = new QWidget;
    auto *layout = new QVBoxLayout(widget);
    layout->addStretch();
    auto *titleLabel = new QLabel(title);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    layout->addWidget(titleLabel);
    auto *descriptionLabel = new QLabel(description);
    descriptionLabel->setAlignment(Qt::AlignCenter);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);
    layout->addStretch();
    return widget;
}

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
    case ice::Orientation::r0:
        return point;
    }
    return point;
}

ice::PinDirection rotateDirection(ice::PinDirection direction, ice::Orientation orientation) {
    const QPointF vector = [&] {
        switch (direction) {
        case ice::PinDirection::left:
            return QPointF(-1, 0);
        case ice::PinDirection::right:
            return QPointF(1, 0);
        case ice::PinDirection::up:
            return QPointF(0, 1);
        case ice::PinDirection::down:
            return QPointF(0, -1);
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

// KLayout-style layer appearance: distinct outline color plus a fill pattern
// so overlapping layers stay distinguishable on a white canvas.
struct LayerStyle {
    QColor color;
    Qt::BrushStyle pattern = Qt::SolidPattern;
};

const std::vector<QColor> &layerPalette() {
    static const std::vector<QColor> palette = {
        QColor("#3366cc"), QColor("#cc0000"), QColor("#109618"), QColor("#ff9900"),
        QColor("#990099"), QColor("#0099c6"), QColor("#dd4477"), QColor("#66aa00"),
        QColor("#b82e2e"), QColor("#316395"), QColor("#994499"), QColor("#22aa99"),
    };
    return palette;
}

const std::vector<Qt::BrushStyle> &layerPatterns() {
    static const std::vector<Qt::BrushStyle> patterns = {
        Qt::SolidPattern, Qt::BDiagPattern, Qt::FDiagPattern,  Qt::DiagCrossPattern, Qt::HorPattern,
        Qt::VerPattern,   Qt::CrossPattern, Qt::Dense4Pattern, Qt::Dense5Pattern,
    };
    return patterns;
}

LayerStyle layerStyleForIndex(std::size_t index, bool visible) {
    if (!visible)
        return {QColor(130, 130, 130), Qt::NoBrush};
    const auto &palette = layerPalette();
    const auto &patterns = layerPatterns();
    const QColor color = palette[index % palette.size()];
    const Qt::BrushStyle pattern = patterns[(index / palette.size()) % patterns.size()];
    return {color, pattern};
}

std::map<ice::ObjectId, LayerStyle> layerStylesFor(const ice::Project &project) {
    std::map<ice::ObjectId, LayerStyle> styles;
    std::size_t index = 0;
    for (const auto &[id, layer] : project.layers)
        styles.emplace(id, layerStyleForIndex(index++, layer.visible));
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
    QPen axisPen(ice::uiColor(ice::UiColor::Axis1));
    axisPen.setCosmetic(true);
    axisPen.setWidthF(1.2);
    painter.setPen(axisPen);
    painter.drawLine(QPointF(exposed.left(), 0.0), QPointF(exposed.right(), 0.0));
    painter.drawLine(QPointF(0.0, exposed.top()), QPointF(0.0, exposed.bottom()));
}

class LayoutCanvas : public QGraphicsView {
  public:
    enum class Tool { select, rectangle };

    explicit LayoutCanvas(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setSceneRect(-1.0e9, -1.0e9, 2.0e9, 2.0e9);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        applyNavigationTransform();
    }

    void setTool(Tool tool) { tool_ = tool; }
    void setCallbacks(std::function<void(QPointF, QPointF)> rectangleCreated,
                      std::function<void(ice::ObjectId, QPointF)> shapeMoved,
                      std::function<void(std::vector<ice::ObjectId>)> deleted, std::function<void()> changed) {
        rectangleCreated_ = std::move(rectangleCreated);
        shapeMoved_ = std::move(shapeMoved);
        deleted_ = std::move(deleted);
        changed_ = std::move(changed);
    }

    void drawGrid(QPainter &painter, const QRectF &exposed, qreal scale) {
        if (scale <= 0.0)
            return;
        const qreal targetPixels = 24.0;
        const qreal rawGrid = targetPixels / scale;
        const qreal magnitude = std::pow(10.0, std::floor(std::log10(rawGrid)));
        const qreal normalized = rawGrid / magnitude;
        const qreal factor = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
        const qreal major = magnitude * factor;
        const int left = static_cast<int>(std::floor(exposed.left() / major));
        const int right = static_cast<int>(std::ceil(exposed.right() / major));
        const int top = static_cast<int>(std::floor(exposed.top() / major));
        const int bottom = static_cast<int>(std::ceil(exposed.bottom() / major));
        if (right - left > 500 || bottom - top > 500)
            return;
        // Minor subdivisions like the schematic grid (5x per major step).
        // Cosmetic 1px pens so lines stay visible at any zoom; scene-unit
        // widths would shrink to sub-pixel sizes when zoomed out.
        const qreal minor = major / 5.0;
        const bool drawMinor = (right - left) * 5 + (bottom - top) * 5 <= 5000;
        if (drawMinor) {
            QPen minorPen(ice::uiColor(ice::UiColor::Grid1));
            minorPen.setCosmetic(true);
            minorPen.setWidth(1);
            painter.setPen(minorPen);
            const int minorLeft = static_cast<int>(std::floor(exposed.left() / minor));
            const int minorRight = static_cast<int>(std::ceil(exposed.right() / minor));
            const int minorTop = static_cast<int>(std::floor(exposed.top() / minor));
            const int minorBottom = static_cast<int>(std::ceil(exposed.bottom() / minor));
            for (int x = minorLeft; x <= minorRight; ++x)
                painter.drawLine(QPointF(x * minor, exposed.top()), QPointF(x * minor, exposed.bottom()));
            for (int y = minorTop; y <= minorBottom; ++y)
                painter.drawLine(QPointF(exposed.left(), y * minor), QPointF(exposed.right(), y * minor));
        }
        QPen majorPen(ice::uiColor(ice::UiColor::Grid2));
        majorPen.setCosmetic(true);
        majorPen.setWidth(1);
        painter.setPen(majorPen);
        for (int x = left; x <= right; ++x)
            painter.drawLine(QPointF(x * major, exposed.top()), QPointF(x * major, exposed.bottom()));
        for (int y = top; y <= bottom; ++y)
            painter.drawLine(QPointF(exposed.left(), y * major), QPointF(exposed.right(), y * major));
    }

    void setLayerStyles(const std::map<ice::ObjectId, LayerStyle> &styles) { layerStyles_ = styles; }
    void setInitialView(const QRectF &bounds) {
        if (!bounds.isValid() || bounds.isNull())
            return;
        const auto available = viewport()->size();
        const qreal width = std::max<qreal>(1.0, bounds.width());
        const qreal height = std::max<qreal>(1.0, bounds.height());
        zoom_ = std::clamp(std::min(available.width() / width, available.height() / height) * 0.85, qreal(1.0e-6),
                           qreal(1.0e6));
        centerScene_ = bounds.center();
        applyNavigationTransform();
    }

    void fitToContent() {
        const auto bounds = scene()->itemsBoundingRect();
        if (!bounds.isValid() || bounds.isNull()) {
            zoom_ = 1.0;
            centerScene_ = QPointF(0.0, 0.0);
            applyNavigationTransform();
            return;
        }
        setInitialView(bounds.adjusted(-20, -20, 20, 20));
    }

  protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override {
        painter->fillRect(rect, ice::uiColor(ice::UiColor::Background2));
        drawGrid(*painter, rect, transform().m11());
        drawCanvasAxes(*painter, rect);
    }

    void drawScaleIndicator(QPainter &painter) {
        if (zoom_ <= 0.0)
            return;

        const qreal targetPixels = 100.0;
        const qreal rawLength = targetPixels / zoom_;
        const qreal magnitude = std::pow(10.0, std::floor(std::log10(rawLength)));
        const qreal normalized = rawLength / magnitude;
        const qreal factor = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
        const qreal length = magnitude * factor;
        const qreal pixels = length * zoom_;

        const qreal left = 14.0;
        const qreal bottom = viewport()->height() - 16.0;
        const auto color = ice::uiColor(ice::UiColor::Axis1);
        QPen pen(color);
        pen.setWidthF(2.0);
        painter.setPen(pen);
        painter.drawLine(QPointF(left, bottom), QPointF(left + pixels, bottom));
        painter.drawLine(QPointF(left, bottom - 5.0), QPointF(left, bottom + 1.0));
        painter.drawLine(QPointF(left + pixels, bottom - 5.0), QPointF(left + pixels, bottom + 1.0));
        painter.setPen(color);
        painter.drawText(QPointF(left, bottom - 8.0), formatScaleLength(length));
    }

    void paintEvent(QPaintEvent *event) override {
        QGraphicsView::paintEvent(event);
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        drawScaleIndicator(painter);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            panStartPosition_ = event->pos();
            panStartCenterScene_ = centerScene_;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (tool_ == Tool::rectangle && event->button() == Qt::LeftButton) {
            drawing_ = true;
            drawStart_ = mapToScene(event->pos());
            const auto previewColor = ice::uiColor(ice::UiColor::Accent3);
            drawPreview_ =
                scene()->addRect(QRectF(drawStart_, drawStart_), QPen(previewColor, 1),
                                 QBrush(QColor(previewColor.red(), previewColor.green(), previewColor.blue(), 50)));
            event->accept();
            return;
        }
        if (tool_ == Tool::select && event->button() == Qt::LeftButton) {
            movingItem_ = itemAt(event->pos());
            if (movingItem_)
                movingStartPosition_ = movingItem_->pos();
        }
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (panning_) {
            const auto delta = event->pos() - panStartPosition_;
            centerScene_ = panStartCenterScene_ - QPointF(delta.x() / zoom_, delta.y() / zoom_);
            applyNavigationTransform();
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

    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() == 0) {
            event->ignore();
            return;
        }
        const auto scenePosition = mapToScene(event->position().toPoint());
        const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        zoom_ = std::clamp(zoom_ * factor, qreal(1.0e-6), qreal(1.0e6));
        const auto viewportCenter = QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5);
        const auto cursor = event->position();
        centerScene_ = scenePosition -
                       QPointF((cursor.x() - viewportCenter.x()) / zoom_, (cursor.y() - viewportCenter.y()) / zoom_);
        applyNavigationTransform();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
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
        QGraphicsView::mouseReleaseEvent(event);
        if (tool_ == Tool::select && event->button() == Qt::LeftButton && movingItem_) {
            const auto delta = movingItem_->pos() - movingStartPosition_;
            if ((delta.x() != 0.0 || delta.y() != 0.0) && shapeMoved_)
                shapeMoved_(movingItem_->data(0).toULongLong(), delta);
            movingItem_ = nullptr;
        }
    }

    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && deleted_) {
            std::vector<ice::ObjectId> ids;
            for (auto *item : scene()->selectedItems())
                if (item->data(0).isValid()) {
                    const auto id = item->data(0).toULongLong();
                    if (id != 0)
                        ids.push_back(id);
                }
            if (!ids.empty())
                deleted_(std::move(ids));
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

  private:
    void applyNavigationTransform() {
        setTransform(QTransform(zoom_, 0.0, 0.0, zoom_, 0.0, 0.0), false);
        centerOn(centerScene_);
        viewport()->update();
    }

    QString formatScaleLength(qreal length) const {
        if (length >= 1000000.0)
            return QStringLiteral("%1 mm").arg(length / 1000000.0, 0, 'g', 4);
        if (length >= 1000.0)
            return QStringLiteral("%1 um").arg(length / 1000.0, 0, 'g', 4);
        if (length >= 1.0)
            return QStringLiteral("%1 nm").arg(length, 0, 'g', 4);
        return QStringLiteral("%1 pm").arg(length * 1000.0, 0, 'g', 4);
    }

    Tool tool_ = Tool::select;
    bool panning_ = false;
    bool drawing_ = false;
    QPoint panStartPosition_;
    QPointF panStartCenterScene_;
    QPointF drawStart_;
    QGraphicsRectItem *drawPreview_ = nullptr;
    QGraphicsItem *movingItem_ = nullptr;
    QPointF movingStartPosition_;
    std::function<void(QPointF, QPointF)> rectangleCreated_;
    std::function<void(ice::ObjectId, QPointF)> shapeMoved_;
    std::function<void(std::vector<ice::ObjectId>)> deleted_;
    std::function<void()> changed_;
    std::map<ice::ObjectId, LayerStyle> layerStyles_;
    qreal zoom_ = 1.0;
    QPointF centerScene_;
};

// Item data roles used to identify schematic canvas items.
//
// role 0: item kind ("symbol", "symbol-artwork", "symbol-label",
//        "symbol-highlight", "pin", "wire", "net-label")
// role 1: the stable domain id of the object the item projects

QGraphicsItem *symbolOwner(QGraphicsItem *item) {
    for (auto *cursor = item; cursor; cursor = cursor->parentItem())
        if (cursor->data(0).toString() == QStringLiteral("symbol"))
            return cursor;
    return nullptr;
}

QGraphicsItem *symbolChild(QGraphicsItem *symbol, const QString &kind) {
    for (auto *child : symbol->childItems())
        if (child->data(0).toString() == kind)
            return child;
    return nullptr;
}

QGraphicsColorizeEffect *symbolArtworkEffect(QGraphicsItem *symbol) {
    auto *artwork = symbolChild(symbol, QStringLiteral("symbol-artwork"));
    if (!artwork)
        return nullptr;
    const auto handle = artwork->data(3).toULongLong();
    return reinterpret_cast<QGraphicsColorizeEffect *>(handle);
}

void applySymbolSelectedStyle(QGraphicsItem *symbol, bool selected) {
    if (!symbol)
        return;
    symbol->setSelected(selected);
    if (auto *highlight = symbolChild(symbol, QStringLiteral("symbol-highlight")))
        highlight->setVisible(selected);
    if (auto *label = symbolChild(symbol, QStringLiteral("symbol-label")))
        static_cast<QGraphicsTextItem *>(label)->setDefaultTextColor(selected ? ice::uiColor(ice::UiColor::Accent4)
                                                                              : ice::uiColor(ice::UiColor::Axis1));
    if (auto *effect = symbolArtworkEffect(symbol)) {
        effect->setColor(selected ? ice::uiColor(ice::UiColor::Accent4) : ice::uiColor(ice::UiColor::Axis1));
        effect->setEnabled(true);
    }
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
class SchematicCanvas : public QGraphicsView {
  public:
    explicit SchematicCanvas(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setScene(new QGraphicsScene(this));
        setSceneRect(-1.0e9, -1.0e9, 2.0e9, 2.0e9);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        applyNavigationTransform();
    }

    void setCallbacks(std::function<void(ice::ObjectId, QPointF)> moved, std::function<void(ice::ObjectId)> rotated,
                      std::function<void(ice::ObjectId, ice::ObjectId)> connected,
                      std::function<void(QPointF)> netLabelPlaced, std::function<void()> changed,
                      std::function<void(ice::ObjectId)> symbolSelected = {},
                      std::function<void(ice::ObjectId)> symbolActivated = {}) {
        moved_ = std::move(moved);
        rotated_ = std::move(rotated);
        connected_ = std::move(connected);
        netLabelPlaced_ = std::move(netLabelPlaced);
        changed_ = std::move(changed);
        symbolSelected_ = std::move(symbolSelected);
        symbolActivated_ = std::move(symbolActivated);
    }

    void setNetLabelMode(bool enabled) {
        netLabelMode_ = enabled;
        if (enabled)
            setCursor(Qt::CrossCursor);
        else if (!placeSymbolMode_)
            unsetCursor();
        updatePinVisibility();
    }

    void setPlaceSymbolMode(bool enabled) {
        placeSymbolMode_ = enabled;
        if (enabled)
            setCursor(Qt::CrossCursor);
        else if (!netLabelMode_)
            unsetCursor();
        updatePinVisibility();
    }

    void setGridVisible(bool visible) {
        gridVisible_ = visible;
        viewport()->update();
    }

    void setSnapToGrid(bool enabled) { snapToGrid_ = enabled; }

    void setSymbolPlacementCallback(std::function<void(QPointF)> callback) {
        symbolPlacementRequested_ = std::move(callback);
    }

    void setToolEscapeCallback(std::function<void()> callback) { toolEscapeRequested_ = std::move(callback); }

    void setPlaceSymbolPreview(const QByteArray &artwork) {
        placeSymbolArtwork_ = artwork;
        viewport()->update();
    }

    void refreshPinVisibility() { updatePinVisibility(); }

    ice::ObjectId selectedInstanceId() const { return selectedInstance_; }

    // Re-applies the selection highlight after the scene has been rebuilt.
    void restoreSelection() {
        if (selectedInstance_ != 0 && !sceneContains(selectedInstance_))
            selectedInstance_ = 0;
        applySelection(selectedInstance_);
    }

    void clearSelection() {
        selectedInstance_ = 0;
        applySelection(0);
        if (symbolSelected_)
            symbolSelected_(0);
    }

    void deleteSelectedSymbols() {
        std::vector<ice::ObjectId> ids;
        for (auto *item : scene()->selectedItems())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                ids.push_back(item->data(1).toULongLong());
        if (!ids.empty() && deleteSelected_)
            deleteSelected_(ids);
    }

    void fitToContent() {
        QRectF bounds;
        for (auto *item : scene()->items()) {
            const auto kind = item->data(0).toString();
            if (!item->parentItem() && kind != QStringLiteral("canvas-title"))
                bounds |= item->sceneBoundingRect();
        }
        if (!bounds.isValid() || bounds.isNull()) {
            zoom_ = 1.0;
            centerScene_ = QPointF(0.0, 0.0);
            applyNavigationTransform();
            return;
        }
        const qreal width = std::max<qreal>(200.0, bounds.width());
        const qreal height = std::max<qreal>(200.0, bounds.height());
        const auto available = viewport()->size();
        zoom_ = std::clamp(std::min(available.width() / width, available.height() / height) * 0.85, qreal(0.05),
                           qreal(100.0));
        centerScene_ = bounds.center();
        applyNavigationTransform();
    }

    void drawBackground(QPainter *painter, const QRectF &rect) override {
        painter->fillRect(rect, ice::uiColor(ice::UiColor::Background2));
        if (gridVisible_) {
            const qreal step = 20.0;
            QPen minor(ice::uiColor(ice::UiColor::Grid1));
            minor.setWidthF(0.5);
            painter->setPen(minor);
            const int left = static_cast<int>(std::floor(rect.left() / step));
            const int right = static_cast<int>(std::ceil(rect.right() / step));
            const int top = static_cast<int>(std::floor(rect.top() / step));
            const int bottom = static_cast<int>(std::ceil(rect.bottom() / step));
            for (int x = left; x <= right; ++x)
                painter->drawLine(QPointF(x * step, rect.top()), QPointF(x * step, rect.bottom()));
            for (int y = top; y <= bottom; ++y)
                painter->drawLine(QPointF(rect.left(), y * step), QPointF(rect.right(), y * step));
            QPen major(ice::uiColor(ice::UiColor::Grid2));
            major.setWidthF(1.0);
            painter->setPen(major);
            const auto majorStep = step * 5;
            const int majorLeft = static_cast<int>(std::floor(rect.left() / majorStep));
            const int majorRight = static_cast<int>(std::ceil(rect.right() / majorStep));
            const int majorTop = static_cast<int>(std::floor(rect.top() / majorStep));
            const int majorBottom = static_cast<int>(std::ceil(rect.bottom() / majorStep));
            for (int x = majorLeft; x <= majorRight; ++x)
                painter->drawLine(QPointF(x * majorStep, rect.top()), QPointF(x * majorStep, rect.bottom()));
            for (int y = majorTop; y <= majorBottom; ++y)
                painter->drawLine(QPointF(rect.left(), y * majorStep), QPointF(rect.right(), y * majorStep));
        }
        drawCanvasAxes(*painter, rect);
    }

    void drawForeground(QPainter *painter, const QRectF &) override {
        if (!placeSymbolMode_ || placeSymbolArtwork_.isEmpty() || !viewport()->underMouse())
            return;
        QSvgRenderer renderer(placeSymbolArtwork_);
        if (!renderer.isValid())
            return;
        const auto artworkSize = renderer.defaultSize();
        if (artworkSize.isEmpty())
            return;
        const auto center = snapScenePosition(cursorScenePosition_);
        const QRectF target(center.x() - artworkSize.width() / 2.0, center.y() - artworkSize.height() / 2.0,
                            artworkSize.width(), artworkSize.height());
        const qreal ratio = devicePixelRatioF();
        const QSize pixelSize(static_cast<int>(std::ceil(target.width() * ratio)),
                              static_cast<int>(std::ceil(target.height() * ratio)));
        QImage artwork(pixelSize, QImage::Format_ARGB32_Premultiplied);
        artwork.setDevicePixelRatio(ratio);
        artwork.fill(Qt::transparent);
        {
            QPainter artworkPainter(&artwork);
            artworkPainter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&artworkPainter, QRectF(QPointF(0.0, 0.0), target.size()));
        }
        {
            QPainter tintPainter(&artwork);
            tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tintPainter.fillRect(artwork.rect(), ice::uiColor(ice::UiColor::Axis1));
        }
        painter->save();
        painter->setOpacity(0.45);
        painter->drawImage(target.topLeft(), artwork);
        painter->restore();
    }

  protected:
    void showEvent(QShowEvent *event) override {
        QGraphicsView::showEvent(event);
        if (viewInitialized_)
            return;
        // Defer one event-loop turn: the tab/widget layout is finalized after
        // showEvent, and centering earlier leaves the origin slightly offset.
        QTimer::singleShot(0, this, [this] {
            if (viewInitialized_)
                return;
            viewInitialized_ = true;
            applyNavigationTransform();
        });
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::MiddleButton) {
            panning_ = true;
            panStartPosition_ = event->pos();
            panStartCenterScene_ = centerScene_;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (placeSymbolMode_ && event->button() == Qt::LeftButton) {
            if (symbolPlacementRequested_)
                symbolPlacementRequested_(snapScenePosition(mapToScene(event->pos())));
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            auto *item = itemAt(event->pos());
            if (netLabelMode_ && (!item || item->data(0).toString() != QStringLiteral("pin"))) {
                if (netLabelPlaced_)
                    netLabelPlaced_(snapScenePosition(mapToScene(event->pos())));
                event->accept();
                return;
            }
            if (item && item->data(0).toString() == QStringLiteral("pin")) {
                wireStart_ = item;
                setPinHighlights(item);
                updatePinVisibility();
                event->accept();
                return;
            }
            if (auto *symbol = symbolOwner(item)) {
                selectSymbol(symbol, event->modifiers().testFlag(Qt::ControlModifier));
                if (symbol->isSelected()) {
                    // The drag is tracked by the stable domain id, so a scene
                    // rebuild during the interaction cannot leave a stale
                    // graphics item behind.
                    dragInstance_ = symbol->data(1).toULongLong();
                    dragOriginScene_ = mapToScene(event->pos());
                    dragOriginPosition_ = symbol->pos();
                    dragged_ = false;
                }
                event->accept();
                return;
            }
            // A press on empty background clears the selection and still
            // starts a rubber-band drag for the view.
            if (selectedInstance_ != 0)
                clearSelection();
        }
        dragInstance_ = 0;
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        cursorScenePosition_ = mapToScene(event->pos());
        updatePinVisibility();
        if (placeSymbolMode_)
            viewport()->update();
        if (panning_) {
            const auto delta = event->pos() - panStartPosition_;
            centerScene_ = panStartCenterScene_ - QPointF(delta.x() / zoom_, delta.y() / zoom_);
            applyNavigationTransform();
            event->accept();
            return;
        }
        if (wireStart_) {
            auto *candidate = itemAt(event->pos());
            highlightCandidate(candidate);
        }
        // A drag follows the symbol item that currently projects the pressed
        // instance; the scene may be rebuilt while dragging.
        QGraphicsItem *dragged = dragInstance_ != 0 ? symbolById(dragInstance_) : nullptr;
        if (dragInstance_ != 0 && !dragged) {
            dragInstance_ = 0;
        } else if (dragged) {
            const auto delta = mapToScene(event->pos()) - dragOriginScene_;
            if (!dragged_ && (std::abs(delta.x()) > 2.0 || std::abs(delta.y()) > 2.0))
                dragged_ = true;
            if (dragged_)
                dragged->setPos(snapScenePosition(dragOriginPosition_ + delta));
            event->accept();
            return;
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            if (auto *symbol = symbolOwner(itemAt(event->pos()))) {
                if (symbolActivated_)
                    symbolActivated_(symbol->data(1).toULongLong());
                event->accept();
                return;
            }
        }
        QGraphicsView::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        if (event->angleDelta().y() == 0) {
            event->ignore();
            return;
        }
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            const auto scenePosition = mapToScene(event->position().toPoint());
            const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
            zoom_ = std::clamp(zoom_ * factor, qreal(0.05), qreal(100.0));
            const auto viewportCenter = QPointF(viewport()->width() * 0.5, viewport()->height() * 0.5);
            const auto cursor = event->position();
            centerScene_ = scenePosition - QPointF((cursor.x() - viewportCenter.x()) / zoom_,
                                                   (cursor.y() - viewportCenter.y()) / zoom_);
        } else {
            const auto delta = event->angleDelta();
            centerScene_ -= QPointF(-delta.x() / zoom_, delta.y() / zoom_);
        }
        applyNavigationTransform();
        cursorScenePosition_ = mapToScene(event->position().toPoint());
        updatePinVisibility();
        if (placeSymbolMode_)
            viewport()->update();
        event->accept();
    }

    void leaveEvent(QEvent *event) override {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("pin"))
                item->setVisible(false);
        QGraphicsView::leaveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (panning_ && event->button() == Qt::MiddleButton) {
            panning_ = false;
            if (placeSymbolMode_)
                setCursor(Qt::CrossCursor);
            else
                unsetCursor();
            event->accept();
            return;
        }
        if (wireStart_) {
            if (event->button() == Qt::LeftButton && itemIsInScene(wireStart_)) {
                auto *end = itemAt(event->pos());
                if (end && end != wireStart_ && end->data(0).toString() == QStringLiteral("pin"))
                    connected_(wireStart_->data(1).toULongLong(), end->data(1).toULongLong());
            }
            wireStart_ = nullptr;
            clearPinHighlights();
            updatePinVisibility();
            event->accept();
            return;
        }
        // The drag ends on any release so the canvas never keeps drag state
        // across interactions that rebuild the scene.
        dragInstance_ = 0;
        QGraphicsView::mouseReleaseEvent(event);
        if (moved_ && scene()) {
            std::vector<std::pair<ice::ObjectId, QPointF>> movedItems;
            for (auto *item : scene()->items()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    movedItems.emplace_back(item->data(1).toULongLong(), item->pos());
            }
            for (const auto &[id, position] : movedItems)
                moved_(id, position);
        }
        if (changed_)
            changed_();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape && (placeSymbolMode_ || netLabelMode_)) {
            if (toolEscapeRequested_)
                toolEscapeRequested_();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_R && scene()) {
            for (auto *item : scene()->selectedItems())
                if (item->data(0).toString() == QStringLiteral("symbol") && rotated_)
                    rotated_(item->data(1).toULongLong());
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && scene()) {
            std::vector<ice::ObjectId> ids;
            for (auto *item : scene()->selectedItems()) {
                if (item->data(0).toString() == QStringLiteral("symbol"))
                    ids.push_back(item->data(1).toULongLong());
            }
            if (!ids.empty() && deleteSelected_)
                deleteSelected_(ids);
            event->accept();
            return;
        }
        QGraphicsView::keyPressEvent(event);
    }

  public:
    std::function<void(std::vector<ice::ObjectId>)> deleteSelected_;

  private:
    void applyNavigationTransform() {
        setTransform(QTransform(zoom_, 0.0, 0.0, zoom_, 0.0, 0.0), false);
        centerOn(centerScene_);
        viewport()->update();
    }

    void setPinHighlights(QGraphicsItem *source) {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item);
            if (pin)
                pin->setBrush(item == source ? QBrush(ice::uiColor(ice::UiColor::Accent5))
                                             : QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
    }

    void highlightCandidate(QGraphicsItem *candidate) {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item);
            if (!pin)
                continue;
            pin->setBrush(item == candidate && item != wireStart_ ? QBrush(ice::uiColor(ice::UiColor::Accent3))
                                                                  : QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
        if (auto *pin = dynamic_cast<QGraphicsEllipseItem *>(wireStart_))
            pin->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent5)));
    }

    void clearPinHighlights() {
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            if (auto *pin = dynamic_cast<QGraphicsEllipseItem *>(item))
                pin->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent1)));
        }
    }

    bool sceneContains(ice::ObjectId instanceId) const {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol") && item->data(1).toULongLong() == instanceId)
                return true;
        return false;
    }

    QGraphicsItem *symbolById(ice::ObjectId instanceId) {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol") && item->data(1).toULongLong() == instanceId)
                return item;
        return nullptr;
    }

    // Selection is view state expressed over stable domain ids, so it survives
    // a scene rebuild and can be mirrored by the properties panel.
    void selectSymbol(QGraphicsItem *symbol, bool additive) {
        if (additive && symbol->isSelected()) {
            applySymbolSelectedStyle(symbol, false);
            selectedInstance_ = 0;
            if (symbolSelected_)
                symbolSelected_(0);
            return;
        }
        if (!additive) {
            for (auto *item : scene()->items())
                if (item != symbol && item->data(0).toString() == QStringLiteral("symbol"))
                    applySymbolSelectedStyle(item, false);
        }
        applySymbolSelectedStyle(symbol, true);
        selectedInstance_ = symbol->data(1).toULongLong();
        if (symbolSelected_)
            symbolSelected_(selectedInstance_);
    }

    void applySelection(ice::ObjectId instanceId) {
        for (auto *item : scene()->items())
            if (item->data(0).toString() == QStringLiteral("symbol"))
                applySymbolSelectedStyle(item, item->data(1).toULongLong() == instanceId);
    }

    // Graphics items are rebuilt whenever the scene is refreshed, so every
    // item captured by an in-flight interaction is validated before use.
    bool itemIsInScene(QGraphicsItem *item) const {
        if (!item)
            return false;
        for (auto *candidate : scene()->items())
            if (candidate == item)
                return true;
        return false;
    }

    QPointF snapScenePosition(const QPointF &position) const {
        if (!snapToGrid_)
            return position;
        constexpr qreal step = 20.0;
        return {std::round(position.x() / step) * step, std::round(position.y() / step) * step};
    }

    void updatePinVisibility() {
        const bool selectTool = !placeSymbolMode_ && !netLabelMode_;
        const bool showAll = selectTool && wireStart_ != nullptr;
        const bool cursorOver = viewport()->underMouse();
        const qreal radius = 12.0 / std::max(zoom_, qreal(0.0001));
        for (auto *item : scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            const bool visible =
                selectTool &&
                (showAll || (cursorOver && QLineF(cursorScenePosition_, item->scenePos()).length() <= radius));
            item->setVisible(visible);
        }
    }

    bool panning_ = false;
    QPoint panStartPosition_;
    QPointF panStartCenterScene_;
    QGraphicsItem *wireStart_ = nullptr;
    ice::ObjectId dragInstance_ = 0;
    QPointF dragOriginScene_;
    QPointF dragOriginPosition_;
    bool dragged_ = false;
    std::function<void(ice::ObjectId, QPointF)> moved_;
    std::function<void(ice::ObjectId)> rotated_;
    std::function<void(ice::ObjectId, ice::ObjectId)> connected_;
    std::function<void(QPointF)> netLabelPlaced_;
    std::function<void()> changed_;
    std::function<void(ice::ObjectId)> symbolSelected_;
    std::function<void(ice::ObjectId)> symbolActivated_;
    bool netLabelMode_ = false;
    bool placeSymbolMode_ = false;
    bool gridVisible_ = true;
    bool snapToGrid_ = true;
    QByteArray placeSymbolArtwork_;
    QPointF cursorScenePosition_;
    std::function<void(QPointF)> symbolPlacementRequested_;
    std::function<void()> toolEscapeRequested_;
    ice::ObjectId selectedInstance_ = 0;
    bool viewInitialized_ = false;
    qreal zoom_ = 1.0;
    QPointF centerScene_;
};

class ProjectPanel : public QWidget {
  public:
    explicit ProjectPanel(bool showLibraries = true, bool showLayers = true, bool showLayoutHierarchy = true,
                          bool showActions = false, QWidget *parent = nullptr)
        : QWidget(parent), showLayoutHierarchy_(showLayoutHierarchy) {
        auto *layout = new QVBoxLayout(this);
        if (showActions) {
            auto *searchRow = new QHBoxLayout;
            search_ = new QLineEdit;
            search_->setPlaceholderText(showLayoutHierarchy_ ? QStringLiteral("Search layouts…")
                                                             : QStringLiteral("Search schematics…"));
            search_->setClearButtonEnabled(true);
            search_->setAccessibleName(showLayoutHierarchy_ ? QStringLiteral("Search layouts")
                                                            : QStringLiteral("Search schematics"));
            addButton_ = new QPushButton;
            addButton_->setFixedWidth(28);
            setLucideIcon(addButton_, QStringLiteral("plus"), 16);
            removeButton_ = new QPushButton;
            removeButton_->setFixedWidth(28);
            setLucideIcon(removeButton_, QStringLiteral("trash"), 16);
            removeButton_->setEnabled(false);
            if (showLayoutHierarchy_) {
                addButton_->setToolTip(QStringLiteral("Add layout cell"));
                addButton_->setAccessibleName(QStringLiteral("Add layout cell"));
                removeButton_->setToolTip(QStringLiteral("Remove selected layout cell"));
                removeButton_->setAccessibleName(QStringLiteral("Remove selected layout cell"));
            } else {
                addButton_->setToolTip(QStringLiteral("Add schematic"));
                addButton_->setAccessibleName(QStringLiteral("Add schematic"));
                removeButton_->setToolTip(QStringLiteral("Remove selected schematic"));
                removeButton_->setAccessibleName(QStringLiteral("Remove selected schematic"));
            }
            searchRow->addWidget(search_, 1);
            searchRow->addWidget(addButton_);
            searchRow->addWidget(removeButton_);
            layout->addLayout(searchRow);
        }
        tree_ = new QTreeWidget;
        tree_->setHeaderLabel(showLayoutHierarchy_ ? QStringLiteral("Layouts") : QStringLiteral("Schematics"));
        tree_->setAccessibleName(showLayoutHierarchy_ ? QStringLiteral("Layout hierarchy")
                                                      : QStringLiteral("Schematic hierarchy"));
        layout->addWidget(tree_, 1);

        packageList_ = new QListWidget;
        packageList_->setAccessibleName(QStringLiteral("Project package dependencies"));
        if (showLibraries) {
            layout->addWidget(heading(QStringLiteral("Packages")));
            layout->addWidget(packageList_);
        }

        auto *layers = new QGroupBox(QStringLiteral("Layers"));
        auto *layerLayout = new QVBoxLayout(layers);
        layerList_ = new QListWidget;
        layerList_->setAccessibleName(QStringLiteral("Layer visibility"));
        layerLayout->addWidget(layerList_);
        if (showLayers)
            layout->addWidget(layers);

        if (search_)
            connect(search_, &QLineEdit::textChanged, this, [this](const QString &text) { applyFilter(text); });
        if (addButton_)
            connect(addButton_, &QPushButton::clicked, this, [this] {
                if (onAdd_)
                    onAdd_();
            });
        if (removeButton_)
            connect(removeButton_, &QPushButton::clicked, this, [this] {
                const auto cellId = selectedCellId();
                if (cellId != 0 && onRemove_)
                    onRemove_(cellId);
            });
        connect(tree_, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *, QTreeWidgetItem *) { updateRemoveButton(); });
    }

    void setOnAddClicked(std::function<void()> callback) { onAdd_ = std::move(callback); }
    void setOnRemoveClicked(std::function<void(ice::ObjectId)> callback) { onRemove_ = std::move(callback); }

    void setProject(const ice::Project &project) {
        tree_->clear();
        layerList_->clear();
        packageList_->clear();
        if (project.packageDependencies.empty()) {
            auto *item = new QListWidgetItem(QStringLiteral("No package dependencies"), packageList_);
            item->setFlags(Qt::NoItemFlags);
        } else {
            for (const auto &dependency : project.packageDependencies)
                new QListWidgetItem(QString::fromStdString(dependency), packageList_);
        }

        if (!showLayoutHierarchy_) {
            for (const auto &[id, cell] : project.schematicCells) {
                auto *cellItem = new QTreeWidgetItem(tree_, {QString::fromStdString(cell.name)});
                cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                const auto pdk = cell.pdk.empty() || cell.pdk == ice::genericPdkId ? QStringLiteral("Generic PDK")
                                                                                   : QString::fromStdString(cell.pdk);
                new QTreeWidgetItem(cellItem, {QStringLiteral("PDK: %1").arg(pdk)});
                new QTreeWidgetItem(cellItem, {QStringLiteral("Symbols (%1)").arg(cell.symbols.size())});
                new QTreeWidgetItem(cellItem, {QStringLiteral("Instances (%1)").arg(cell.instances.size())});
                new QTreeWidgetItem(cellItem, {QStringLiteral("Nets (%1)").arg(cell.nets.size())});
            }
        }

        if (!showLayoutHierarchy_) {
            tree_->expandAll();
            if (search_)
                applyFilter(search_->text());
            updateRemoveButton();
            return;
        }
        for (const auto &[id, layer] : project.layers) {
            auto *item = new QListWidgetItem(QString::fromStdString(layer.name), layerList_);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
        }
        for (const auto &[id, cell] : project.layoutCells) {
            auto *cellItem = new QTreeWidgetItem(tree_, {QString::fromStdString(cell.name)});
            cellItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            new QTreeWidgetItem(cellItem, {QStringLiteral("Shapes (%1)").arg(cell.shapes.size())});
            new QTreeWidgetItem(cellItem, {QStringLiteral("Ports (%1)").arg(cell.ports.size())});
            new QTreeWidgetItem(cellItem, {QStringLiteral("Instances (%1)").arg(cell.instances.size())});
        }
        tree_->expandAll();
        if (search_)
            applyFilter(search_->text());
        updateRemoveButton();
    }

  private:
    ice::ObjectId selectedCellId() const {
        const auto *item = tree_->currentItem();
        if (!item || item->parent() != nullptr || item->isHidden() || !item->data(0, Qt::UserRole).isValid())
            return 0;
        return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
    }

    void updateRemoveButton() {
        if (removeButton_)
            removeButton_->setEnabled(selectedCellId() != 0);
    }

    static bool filterTreeItem(QTreeWidgetItem *item, const QString &needle) {
        bool childMatches = false;
        for (int index = 0; index < item->childCount(); ++index)
            childMatches |= filterTreeItem(item->child(index), needle);
        const bool selfMatches = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        const bool visible = selfMatches || childMatches;
        item->setHidden(!visible);
        return visible;
    }

    void applyFilter(const QString &text) {
        const auto needle = text.trimmed();
        for (int index = 0; index < tree_->topLevelItemCount(); ++index)
            filterTreeItem(tree_->topLevelItem(index), needle);
        if (!needle.isEmpty())
            tree_->expandAll();
        updateRemoveButton();
    }

    QTreeWidget *tree_ = nullptr;
    QListWidget *packageList_ = nullptr;
    QListWidget *layerList_ = nullptr;
    QLineEdit *search_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    std::function<void()> onAdd_;
    std::function<void(ice::ObjectId)> onRemove_;
    bool showLayoutHierarchy_ = true;
};

// ---------------------------------------------------------------------------
// SimulationPanel – left-panel tree of all simulations (mirrors ProjectPanel)
// ---------------------------------------------------------------------------
class SimulationPanel : public QWidget {
  public:
    // Emitted when the user selects a simulation or one of its graphs.
    // simulationId == 0 means no selection; graphId == 0 means no specific graph.
    using SelectionCallback = std::function<void(ice::ObjectId simulationId, ice::ObjectId graphId)>;

    explicit SimulationPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        tree_ = new QTreeWidget;
        tree_->setHeaderLabel(QStringLiteral("Hierarchy"));
        tree_->setAccessibleName(QStringLiteral("Simulation hierarchy"));
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(tree_, 1);

        auto *buttonRow = new QWidget;
        auto *buttonLayout = new QHBoxLayout(buttonRow);
        buttonLayout->setContentsMargins(4, 0, 4, 4);
        addButton_ = new QPushButton;
        addButton_->setToolTip(QStringLiteral("Add simulation"));
        addButton_->setAccessibleName(QStringLiteral("Add simulation"));
        addButton_->setFixedWidth(32);
        setLucideIcon(addButton_, QStringLiteral("plus"), 16);
        deleteButton_ = new QPushButton;
        deleteButton_->setToolTip(QStringLiteral("Delete selected simulation"));
        deleteButton_->setAccessibleName(QStringLiteral("Delete selected simulation"));
        deleteButton_->setFixedWidth(32);
        setLucideIcon(deleteButton_, QStringLiteral("trash"), 16);
        deleteButton_->setEnabled(false);
        buttonLayout->addWidget(addButton_);
        buttonLayout->addWidget(deleteButton_);
        buttonLayout->addStretch();
        layout->addWidget(buttonRow);

        connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
            const bool hasSelection = current != nullptr;
            deleteButton_->setEnabled(hasSelection);
            if (onSelection_)
                onSelection_(currentSimulationId(), currentGraphId());
        });
        connect(tree_, &QTreeWidget::customContextMenuRequested, this,
                [this](const QPoint &pos) { showContextMenu(pos); });
    }

    void setOnAddClicked(std::function<void()> cb) {
        connect(addButton_, &QPushButton::clicked, this, [cb = std::move(cb)] { cb(); });
    }
    void setOnDeleteClicked(std::function<void()> cb) {
        connect(deleteButton_, &QPushButton::clicked, this, [cb = std::move(cb)] { cb(); });
    }
    void setOnAddGraph(std::function<void(ice::ObjectId)> cb) { onAddGraph_ = std::move(cb); }
    void setOnDeleteGraph(std::function<void(ice::ObjectId, ice::ObjectId)> cb) { onDeleteGraph_ = std::move(cb); }
    void setOnSelection(SelectionCallback cb) { onSelection_ = std::move(cb); }

    // Rebuild the tree from the project's simulations map.
    void setProject(const ice::Project &project) {
        const ice::ObjectId prevSimId = currentSimulationId();
        const ice::ObjectId prevGraphId = currentGraphId();
        {
            // Block signals so a rebuild never re-fires the selection callback
            // for every intermediate state; it fires once at the end instead.
            QSignalBlocker blocker(tree_);
            tree_->clear();
            for (const auto &[id, sim] : project.simulations) {
                auto *item = new QTreeWidgetItem(tree_, {QString::fromStdString(sim.name)});
                item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                // Properties, mirroring the hierarchy panels' style.
                std::string schematicName = "(none)";
                if (sim.schematic != 0) {
                    if (const auto cell = project.schematicCells.find(sim.schematic);
                        cell != project.schematicCells.end())
                        schematicName = cell->second.name;
                }
                // Property rows keep the simulation id so selecting one keeps
                // the simulation active in the editor.
                auto addPropertyRow = [&](const QString &text) {
                    auto *row = new QTreeWidgetItem(item, {text});
                    row->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                    row->setFlags(Qt::ItemIsEnabled);
                };
                addPropertyRow(QStringLiteral("Schematic: %1").arg(QString::fromStdString(schematicName)));
                addPropertyRow(QStringLiteral("Simulator: %1").arg(QString::fromStdString(sim.simulator)));
                addPropertyRow(QStringLiteral("Analysis: %1").arg(QString::fromStdString(sim.analysis)));
                addPropertyRow(QStringLiteral("Stop time / end: %1").arg(QString::number(sim.stop, 'g', 6)));
                addPropertyRow(QStringLiteral("Step / increment: %1").arg(QString::number(sim.step, 'g', 6)));
                // Graphs sub-tree.
                auto *graphsItem = new QTreeWidgetItem(item, {QStringLiteral("Graphs (%1)").arg(sim.graphs.size())});
                graphsItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                graphsItem->setFlags(Qt::ItemIsEnabled);
                for (const auto &graph : sim.graphs) {
                    auto *graphItem = new QTreeWidgetItem(graphsItem, {QString::fromStdString(graph.name)});
                    graphItem->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                    graphItem->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(graph.id));
                    graphItem->setToolTip(
                        0, QStringLiteral("X: %1  Y: %2")
                               .arg(QString::fromStdString(graph.xAxis), QString::fromStdString(graph.yAxis)));
                }
            }
            tree_->expandAll();
            // Restore previous selection if still present.
            if (prevSimId != 0)
                selectSimulation(prevSimId, prevGraphId);
        }
        deleteButton_->setEnabled(tree_->currentItem() != nullptr);
        if (onSelection_)
            onSelection_(currentSimulationId(), currentGraphId());
    }

    void clear() {
        tree_->clear();
        deleteButton_->setEnabled(false);
        if (onSelection_)
            onSelection_(0, 0);
    }

    ice::ObjectId currentSimulationId() const {
        auto *item = tree_->currentItem();
        if (!item)
            return 0;
        // Graph items and property items live under a simulation item; the
        // simulation id is stored on every selectable row.
        if (item->data(0, Qt::UserRole).isValid())
            return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong());
        return 0;
    }

    ice::ObjectId currentGraphId() const {
        auto *item = tree_->currentItem();
        if (!item || !item->data(0, Qt::UserRole + 1).isValid())
            return 0;
        return static_cast<ice::ObjectId>(item->data(0, Qt::UserRole + 1).toULongLong());
    }

    // Select the item matching the given simulation/graph id (0 = deselect).
    void selectSimulation(ice::ObjectId id, ice::ObjectId graphId = 0) {
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto *item = tree_->topLevelItem(i);
            if (static_cast<ice::ObjectId>(item->data(0, Qt::UserRole).toULongLong()) != id)
                continue;
            QTreeWidgetItem *target = item;
            if (graphId != 0) {
                for (int g = 0; g < item->childCount(); ++g) {
                    auto *graphsNode = item->child(g);
                    for (int c = 0; graphsNode && c < graphsNode->childCount(); ++c) {
                        auto *graphItem = graphsNode->child(c);
                        if (static_cast<ice::ObjectId>(graphItem->data(0, Qt::UserRole + 1).toULongLong()) == graphId) {
                            target = graphItem;
                            break;
                        }
                    }
                }
            }
            tree_->setCurrentItem(target);
            return;
        }
        tree_->setCurrentItem(nullptr);
    }

  private:
    void showContextMenu(const QPoint &pos) {
        auto *clicked = tree_->itemAt(pos);
        if (!clicked)
            return;
        // Find the simulation item owning the clicked row (property and graph
        // rows carry the simulation id too, but walk up as a safety net).
        QTreeWidgetItem *simItem = clicked;
        while (simItem && !simItem->data(0, Qt::UserRole).isValid())
            simItem = simItem->parent();
        if (!simItem)
            return;
        const auto simId = static_cast<ice::ObjectId>(simItem->data(0, Qt::UserRole).toULongLong());
        // A graph row additionally carries its graph id.
        const ice::ObjectId graphId = clicked->data(0, Qt::UserRole + 1).isValid()
                                          ? static_cast<ice::ObjectId>(clicked->data(0, Qt::UserRole + 1).toULongLong())
                                          : 0;
        QMenu menu(this);
        auto *addGraphAction = menu.addAction(QStringLiteral("Add graph…"));
        QAction *deleteGraphAction = nullptr;
        if (graphId != 0)
            deleteGraphAction = menu.addAction(QStringLiteral("Delete this graph"));
        const auto *chosen = menu.exec(tree_->viewport()->mapToGlobal(pos));
        if (chosen == addGraphAction && onAddGraph_)
            onAddGraph_(simId);
        else if (chosen == deleteGraphAction && onDeleteGraph_ && graphId != 0)
            onDeleteGraph_(simId, graphId);
    }

    QTreeWidget *tree_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    SelectionCallback onSelection_;
    std::function<void(ice::ObjectId)> onAddGraph_;
    std::function<void(ice::ObjectId, ice::ObjectId)> onDeleteGraph_;
};

class LayersPanel : public QWidget {
  public:
    explicit LayersPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        tree_ = new QTreeWidget;
        tree_->setHeaderLabel(QStringLiteral("Layers"));
        tree_->setAccessibleName(QStringLiteral("Layer visibility"));
        tree_->setRootIsDecorated(false);
        tree_->setUniformRowHeights(true);
        tree_->setIconSize(QSize(36, 18));
        tree_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        layout->addWidget(tree_, 1);
        setMinimumWidth(190);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }

    void setProject(const ice::Project &project) {
        tree_->clear();
        const auto styles = layerStylesFor(project);
        for (const auto &[id, layer] : project.layers) {
            const auto style =
                styles.contains(id) ? styles.at(id) : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
            auto *item = new QTreeWidgetItem(
                tree_, {QStringLiteral("%1 (%2)").arg(QString::fromStdString(layer.name)).arg(layer.number)});
            item->setIcon(0, QIcon(layerPreviewIcon(style)));
            item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
        }
    }

  private:
    QTreeWidget *tree_ = nullptr;
};

class PropertiesPanel : public QWidget {
  public:
    using ParameterChanged = std::function<void(ice::ObjectId instanceId, ice::ObjectId parameterId,
                                                const std::string &name, const std::string &value)>;
    using CellParameterChanged = std::function<void(ice::ObjectId cellId, ice::ObjectId parameterId,
                                                    const std::string &name, const std::string &value)>;
    using CellParameterAdded = std::function<void(ice::ObjectId cellId, const std::string &name)>;
    using CellParametersRemoved =
        std::function<void(ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds)>;

    explicit PropertiesPanel(bool showDisplay = true, QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Properties")));
        previewFrame_ = new QFrame;
        previewFrame_->setObjectName(QStringLiteral("propertiesSymbolPreview"));
        previewFrame_->setFixedHeight(160);
        auto *previewLayout = new QVBoxLayout(previewFrame_);
        preview_ = new SymbolPreview;
        preview_->setMinimumSize(160, 120);
        preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        previewLayout->addWidget(preview_);
        previewFrame_->setVisible(false);
        layout->addWidget(previewFrame_);
        auto *form = new QFormLayout;
        selection_ = addRow(form, QStringLiteral("Selection"));
        layout->addLayout(form);
        auto *parametersHeader = new QWidget;
        auto *parametersHeaderLayout = new QHBoxLayout(parametersHeader);
        parametersHeaderLayout->setContentsMargins(0, 0, 0, 0);
        parametersHeaderLayout->addWidget(heading(QStringLiteral("Parameters")));
        parametersHeaderLayout->addStretch();
        addParameterButton_ = new QPushButton;
        addParameterButton_->setFixedWidth(24);
        addParameterButton_->setToolTip(QStringLiteral("Add parameter"));
        addParameterButton_->setAccessibleName(QStringLiteral("Add parameter"));
        setLucideIcon(addParameterButton_, QStringLiteral("plus"), 14);
        removeParameterButton_ = new QPushButton;
        removeParameterButton_->setFixedWidth(24);
        removeParameterButton_->setToolTip(QStringLiteral("Remove selected parameters"));
        removeParameterButton_->setAccessibleName(QStringLiteral("Remove selected parameters"));
        setLucideIcon(removeParameterButton_, QStringLiteral("trash"), 14);
        parametersHeaderLayout->addWidget(addParameterButton_);
        parametersHeaderLayout->addWidget(removeParameterButton_);
        layout->addWidget(parametersHeader);
        parameters_ = new QTableWidget(0, 2);
        parameters_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
        parameters_->setAccessibleName(QStringLiteral("Parameters"));
        parameters_->setSelectionBehavior(QAbstractItemView::SelectRows);
        parameters_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        parameters_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        parameters_->verticalHeader()->setVisible(false);
        layout->addWidget(parameters_, 1);
        if (showDisplay) {
            layout->addWidget(heading(QStringLiteral("Display")));
            auto *layerSelector = new QComboBox;
            layerSelector->addItem(QStringLiteral("All layers"));
            layerSelector->addItem(QStringLiteral("Visible layers"));
            layout->addWidget(layerSelector);
        }
        layout->addStretch();
        connect(parameters_, &QTableWidget::itemChanged, this,
                [this](QTableWidgetItem *item) { handleParameterEdit(item); });
        connect(parameters_, &QTableWidget::itemSelectionChanged, this, [this] { updateParameterButtons(); });
        connect(addParameterButton_, &QPushButton::clicked, this, [this] { addParameter(); });
        connect(removeParameterButton_, &QPushButton::clicked, this, [this] { removeParameters(); });
        updateParameterButtons();
        refreshAppearance();
    }

    void setOnParameterChanged(ParameterChanged callback) { onParameterChanged_ = std::move(callback); }
    void setOnCellParameterChanged(CellParameterChanged callback) { onCellParameterChanged_ = std::move(callback); }
    void setOnCellParameterAdded(CellParameterAdded callback) { onCellParameterAdded_ = std::move(callback); }
    void setOnCellParametersRemoved(CellParametersRemoved callback) { onCellParametersRemoved_ = std::move(callback); }

    void refreshAppearance() {
        previewFrame_->setStyleSheet(
            QStringLiteral("#propertiesSymbolPreview { background: %1; border: 1px solid %2; border-radius: 4px; }")
                .arg(ice::uiColor(ice::UiColor::Background2).name(), ice::uiColor(ice::UiColor::Border1).name()));
    }

    void clearSelection() {
        instanceId_ = 0;
        cellId_ = 0;
        selection_->setText(QStringLiteral("Nothing selected"));
        previewFrame_->setVisible(false);
        QSignalBlocker blocker(parameters_);
        parameters_->setRowCount(0);
        updateParameterButtons();
    }

    void setInstance(const ice::SchematicInstance &instance, const std::string &symbolName,
                     const std::string &symbolSvg) {
        instanceId_ = instance.id;
        cellId_ = 0;
        const auto type = symbolName.empty() ? QStringLiteral("Schematic") : QString::fromStdString(symbolName);
        selection_->setText(QStringLiteral("%1 (%2)").arg(QString::fromStdString(instance.name), type));
        preview_->load(QByteArray::fromStdString(symbolSvg));
        previewFrame_->setVisible(!symbolSvg.empty());
        populateParameters(instance.parameters);
        updateParameterButtons();
    }

    void setSchematic(const ice::SchematicCell &cell, const QString &symbolSvg) {
        instanceId_ = 0;
        cellId_ = cell.id;
        selection_->setText(QStringLiteral("%1 (Schematic)").arg(QString::fromStdString(cell.name)));
        preview_->load(symbolSvg.toUtf8());
        previewFrame_->setVisible(!symbolSvg.isEmpty());
        populateParameters(cell.parameters);
        updateParameterButtons();
    }

  private:
    static QLabel *addRow(QFormLayout *form, const QString &label) {
        auto *value = new QLabel(QStringLiteral("—"));
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(label, value);
        return value;
    }

    void populateParameters(const std::vector<ice::Parameter> &parameters) {
        QSignalBlocker blocker(parameters_);
        parameters_->setRowCount(0);
        for (const auto &parameter : parameters) {
            const int row = parameters_->rowCount();
            parameters_->insertRow(row);
            auto *name = new QTableWidgetItem(QString::fromStdString(parameter.name));
            name->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(parameter.id));
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            parameters_->setItem(row, 0, name);
            parameters_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(parameter.value)));
        }
    }

    void handleParameterEdit(QTableWidgetItem *item) {
        if (!item || item->column() != 1)
            return;
        const auto *name = parameters_->item(item->row(), 0);
        if (!name)
            return;
        const auto parameterId = static_cast<ice::ObjectId>(name->data(Qt::UserRole).toULongLong());
        if (instanceId_ != 0 && onParameterChanged_)
            onParameterChanged_(instanceId_, parameterId, name->text().toStdString(), item->text().toStdString());
        else if (cellId_ != 0 && onCellParameterChanged_)
            onCellParameterChanged_(cellId_, parameterId, name->text().toStdString(), item->text().toStdString());
    }

    void updateParameterButtons() {
        const bool schematicMode = cellId_ != 0 && instanceId_ == 0;
        if (addParameterButton_)
            addParameterButton_->setEnabled(schematicMode);
        if (removeParameterButton_) {
            const bool hasSelection = parameters_->selectionModel() && parameters_->selectionModel()->hasSelection();
            removeParameterButton_->setEnabled(schematicMode && hasSelection);
        }
    }

    void addParameter() {
        if (instanceId_ != 0 || cellId_ == 0 || !onCellParameterAdded_)
            return;
        bool accepted = false;
        const auto name = QInputDialog::getText(
            this, QStringLiteral("Add Parameter"), QStringLiteral("Parameter name:"), QLineEdit::Normal,
            QStringLiteral("parameter%1").arg(parameters_->rowCount() + 1), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        onCellParameterAdded_(cellId_, name.trimmed().toStdString());
    }

    void removeParameters() {
        if (instanceId_ != 0 || cellId_ == 0 || !onCellParametersRemoved_)
            return;
        std::vector<ice::ObjectId> parameterIds;
        const auto rows = parameters_->selectionModel()->selectedRows();
        for (const auto &index : rows) {
            if (const auto *name = parameters_->item(index.row(), 0))
                parameterIds.push_back(static_cast<ice::ObjectId>(name->data(Qt::UserRole).toULongLong()));
        }
        if (!parameterIds.empty())
            onCellParametersRemoved_(cellId_, parameterIds);
    }

    QLabel *selection_ = nullptr;
    SymbolPreview *preview_ = nullptr;
    QFrame *previewFrame_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QPushButton *addParameterButton_ = nullptr;
    QPushButton *removeParameterButton_ = nullptr;
    ice::ObjectId instanceId_ = 0;
    ice::ObjectId cellId_ = 0;
    ParameterChanged onParameterChanged_;
    CellParameterChanged onCellParameterChanged_;
    CellParameterAdded onCellParameterAdded_;
    CellParametersRemoved onCellParametersRemoved_;
};

class SymbolsPanel : public QWidget {
  public:
    explicit SymbolsPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        search_ = new QLineEdit;
        search_->setPlaceholderText(QStringLiteral("Search symbols…"));
        search_->setClearButtonEnabled(true);
        search_->setAccessibleName(QStringLiteral("Search symbols"));
        layout->addWidget(search_);
        category_ = new QComboBox;
        category_->setAccessibleName(QStringLiteral("Symbol category"));
        category_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        category_->setMinimumContentsLength(12);
        layout->addWidget(category_);
        list_ = new QListWidget;
        list_->setAccessibleName(QStringLiteral("Available symbols"));
        layout->addWidget(list_, 1);

        connect(search_, &QLineEdit::textChanged, this, [this] { rebuildList(); });
        connect(category_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { rebuildList(); });
        connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *, QListWidgetItem *) {
            if (onSelectionChanged_)
                onSelectionChanged_();
        });
        rebuildCategories();
        rebuildList();
    }

    void setOnSelectionChanged(std::function<void()> callback) { onSelectionChanged_ = std::move(callback); }

    void setSymbols(std::vector<SymbolDefinition> symbols) {
        QString selectedName;
        if (const auto *item = list_->currentItem(); item && item->data(Qt::UserRole).isValid()) {
            const int index = item->data(Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(symbols_.size()))
                selectedName = symbols_[index].name;
        }
        symbols_ = std::move(symbols);
        rebuildCategories();
        rebuildList(selectedName);
    }

    void clear() {
        symbols_.clear();
        rebuildCategories();
        rebuildList();
    }

    const SymbolDefinition *selectedSymbol() const {
        const auto *item = list_->currentItem();
        if (!item || !item->data(Qt::UserRole).isValid())
            return nullptr;
        const int index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(symbols_.size()))
            return nullptr;
        return &symbols_[index];
    }

  private:
    void rebuildCategories() {
        const auto current = category_->currentData().toString();
        QSignalBlocker blocker(category_);
        category_->clear();
        category_->addItem(QStringLiteral("All categories"), QString());
        QStringList categories;
        for (const auto &symbol : symbols_)
            if (!symbol.category.isEmpty() && !categories.contains(symbol.category))
                categories << symbol.category;
        categories.sort(Qt::CaseInsensitive);
        for (const auto &category : categories)
            category_->addItem(category, category);
        const auto index = category_->findData(current);
        category_->setCurrentIndex(index >= 0 ? index : 0);
    }

    void rebuildList(const QString &keepSelected = {}) {
        list_->clear();
        const auto needle = search_->text().trimmed();
        const auto category = category_->currentData().toString();
        for (std::size_t index = 0; index < symbols_.size(); ++index) {
            const auto &symbol = symbols_[index];
            if (!category.isEmpty() && symbol.category != category)
                continue;
            if (!needle.isEmpty() && !symbol.name.contains(needle, Qt::CaseInsensitive) &&
                !symbol.category.contains(needle, Qt::CaseInsensitive))
                continue;
            auto *item = new QListWidgetItem(symbol.name, list_);
            item->setData(Qt::UserRole, static_cast<int>(index));
            item->setToolTip(symbol.category.isEmpty() ? QStringLiteral("Uncategorized") : symbol.category);
        }
        if (list_->count() == 0) {
            auto *item = new QListWidgetItem(symbols_.empty() ? QStringLiteral("No symbols available")
                                                              : QStringLiteral("No matching symbols"),
                                             list_);
            item->setFlags(Qt::NoItemFlags);
            return;
        }
        int row = 0;
        if (!keepSelected.isEmpty()) {
            for (int index = 0; index < list_->count(); ++index) {
                const int symbolIndex = list_->item(index)->data(Qt::UserRole).toInt();
                if (symbolIndex >= 0 && symbolIndex < static_cast<int>(symbols_.size()) &&
                    symbols_[symbolIndex].name == keepSelected) {
                    row = index;
                    break;
                }
            }
        }
        list_->setCurrentRow(row);
    }

    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    std::vector<SymbolDefinition> symbols_;
    std::function<void()> onSelectionChanged_;
};

class SchematicNetlistPanel : public QWidget {
  public:
    explicit SchematicNetlistPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Netlist")));
        netlist_ = new QPlainTextEdit;
        netlist_->setReadOnly(true);
        netlist_->setLineWrapMode(QPlainTextEdit::NoWrap);
        netlist_->setPlaceholderText(QStringLiteral("No schematic open."));
        layout->addWidget(netlist_, 1);
    }

    void setCell(const ice::Project &project, const ice::SchematicCell *cell) {
        if (!cell) {
            netlist_->clear();
            return;
        }
        std::vector<std::string> notes;
        try {
            const auto lines = ice::buildSchematicNetlist(project, cell->id, &notes);
            QString text;
            for (const auto &line : lines)
                text += QString::fromStdString(line) + QLatin1Char('\n');
            if (!notes.empty()) {
                if (!text.isEmpty())
                    text += QLatin1Char('\n');
                text += QStringLiteral("* Notes\n");
                for (const auto &note : notes)
                    text += QStringLiteral("* %1\n").arg(QString::fromStdString(note));
            }
            netlist_->setPlainText(text.isEmpty() ? QStringLiteral("* Empty netlist") : text);
        } catch (const ice::ProjectError &error) {
            netlist_->setPlainText(QString::fromStdString(error.what()));
        }
    }

    void clear() { netlist_->clear(); }

  private:
    QPlainTextEdit *netlist_ = nullptr;
};

class DiagnosticsPanel : public QWidget {
  public:
    explicit DiagnosticsPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(heading(QStringLiteral("Diagnostics")));
        auto *diagnostics = new QListWidget;
        diagnostics->setAccessibleName(QStringLiteral("Diagnostics list"));
        diagnostics->addItem(QStringLiteral("No diagnostics"));
        layout->addWidget(diagnostics);
    }
};

class ActivityPanel : public QWidget {
  public:
    explicit ActivityPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        auto *tabs = new QTabWidget;

        auto *agent = new QWidget;
        auto *agentLayout = new QHBoxLayout(agent);
        auto *transcript = new QPlainTextEdit;
        transcript->setReadOnly(true);
        transcript->setPlaceholderText(QStringLiteral("Agent conversation and tool-call updates"));

        auto *sessionPanel = new QFrame;
        sessionPanel->setFrameShape(QFrame::StyledPanel);
        sessionPanel->setFixedWidth(240);
        auto *sessionLayout = new QVBoxLayout(sessionPanel);
        sessionLayout->setContentsMargins(10, 8, 10, 8);
        sessionLayout->setSpacing(6);
        auto *sessionTitle = new QLabel(QStringLiteral("No active session"));
        sessionTitle->setWordWrap(true);
        sessionTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sessionLayout->addWidget(sessionTitle);
        sessionLayout->addSpacing(6);
        auto *tokens = new QLabel(QStringLiteral("0 / 128,000 tokens"));
        sessionLayout->addWidget(tokens);
        auto *contextUsage = new QProgressBar;
        contextUsage->setRange(0, 100);
        contextUsage->setValue(0);
        contextUsage->setFormat(QStringLiteral("%p% used"));
        contextUsage->setMaximumHeight(18);
        sessionLayout->addWidget(contextUsage);
        sessionLayout->addSpacing(6);
        auto *mcpServers = new QListWidget;
        mcpServers->setAccessibleName(QStringLiteral("MCP servers"));
        mcpServers->setAlternatingRowColors(true);
        auto *noServers = new QListWidgetItem(QStringLiteral("No MCP servers connected"), mcpServers);
        noServers->setFlags(Qt::NoItemFlags);
        sessionLayout->addWidget(mcpServers, 1);

        auto *conversationColumn = new QVBoxLayout;
        conversationColumn->addWidget(transcript, 1);
        auto *prompt = new QLineEdit;
        prompt->setPlaceholderText(QStringLiteral("Ask the agent about the current project..."));
        conversationColumn->addWidget(prompt);
        agentLayout->addLayout(conversationColumn, 1);

        auto *sessionColumn = new QVBoxLayout;
        sessionColumn->setSpacing(8);
        sessionColumn->addWidget(sessionPanel, 1);
        auto *sendButton = new QPushButton(QStringLiteral("Send"));
        sendButton->setFixedWidth(240);
        sessionColumn->addWidget(sendButton);
        agentLayout->addLayout(sessionColumn, 0);
        tabs->addTab(agent, QStringLiteral("Agent"));

        auto *messages = new QPlainTextEdit;
        messages->setReadOnly(true);
        messages->setPlaceholderText(QStringLiteral("Structured application and external-tool messages"));
        tabs->addTab(messages, QStringLiteral("Messages"));

        auto *jobs = new QWidget;
        auto *jobsLayout = new QVBoxLayout(jobs);
        jobsLayout->addWidget(new QLabel(QStringLiteral("No active jobs")));
        auto *progress = new QProgressBar;
        progress->setRange(0, 100);
        progress->setValue(0);
        progress->setFormat(QStringLiteral("Ready"));
        jobsLayout->addWidget(progress);
        jobsLayout->addWidget(new QPushButton(QStringLiteral("Cancel selected job")));
        jobsLayout->addStretch();
        tabs->addTab(jobs, QStringLiteral("Jobs"));

        layout->addWidget(tabs);
    }
};

class CommandPalette : public QDialog {
  public:
    explicit CommandPalette(const QList<QAction *> &actions, QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Command Palette"));
        setModal(true);
        resize(560, 420);

        auto *layout = new QVBoxLayout(this);
        search_ = new QLineEdit;
        search_->setPlaceholderText(QStringLiteral("Type to search commands..."));
        search_->setAccessibleName(QStringLiteral("Command palette search"));
        layout->addWidget(search_);

        list_ = new QListWidget;
        list_->setAccessibleName(QStringLiteral("Command palette results"));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        layout->addWidget(list_, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(search_, &QLineEdit::textChanged, this, [this, actions](const QString &text) {
            list_->clear();
            for (auto *action : actions) {
                if (text.isEmpty() || action->text().contains(text, Qt::CaseInsensitive)) {
                    auto *item = new QListWidgetItem(action->text(), list_);
                    item->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(action)));
                }
            }
            if (list_->count() > 0)
                list_->setCurrentRow(0);
        });
        connect(search_, &QLineEdit::returnPressed, this, [this] {
            if (list_->currentItem() == nullptr)
                return;
            auto *action = reinterpret_cast<QAction *>(list_->currentItem()->data(Qt::UserRole).value<quintptr>());
            if (action != nullptr) {
                accept();
                action->trigger();
            }
        });
        search_->setFocus();
        search_->setText(QString());
    }

  private:
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
};

class StatusBar : public QStatusBar {
  public:
    explicit StatusBar(QWidget *parent = nullptr) : QStatusBar(parent) {}

    void setProjectName(const QString &name) {
        if (projectName_ == name)
            return;
        projectName_ = name;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QStatusBar::paintEvent(event);
        if (projectName_.isEmpty())
            return;
        QPainter painter(this);
        painter.setPen(palette().windowText().color());
        const auto text = fontMetrics().elidedText(projectName_, Qt::ElideMiddle, qMax(0, width() - 320));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text);
    }

  private:
    QString projectName_;
};

constexpr auto kManualUrl = "https://github.com/ldapx/ice/tree/main/docs";
constexpr auto kDonationUrl = "https://github.com/sponsors/ldapx";
constexpr auto kSourceUrl = "https://github.com/ldapx/ice";
// The project currently has no dedicated website; the repository page is the
// canonical home until one exists.
constexpr auto kWebsiteUrl = "https://github.com/ldapx/ice";

class StartupView : public QWidget {
  public:
    using CreateProjectCallback = std::function<void(const QString &name, const QString &location)>;
    using OpenProjectCallback = std::function<void()>;
    using OpenRecentCallback = std::function<void(const QString &path)>;

    explicit StartupView(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(40, 28, 40, 28);
        layout->setSpacing(16);

        auto *header = new QHBoxLayout;
        header->setSpacing(16);
        auto *logo = new QLabel;
        const auto logoPixmap = ice::appLogo(96);
        logo->setPixmap(logoPixmap);
        logo->setFixedSize(96, 96);
        logo->setAlignment(Qt::AlignCenter);
        logo->setVisible(!logoPixmap.isNull());
        header->addWidget(logo, 0, Qt::AlignTop);

        auto *titleColumn = new QVBoxLayout;
        titleColumn->setSpacing(4);
        auto *title = new QLabel(QStringLiteral("ICE"));
        auto titleFont = title->font();
        titleFont.setPointSize(titleFont.pointSize() + 18);
        titleFont.setBold(true);
        title->setFont(titleFont);
        titleColumn->addWidget(title);

        subtitle_ = new QLabel(QStringLiteral("Integrated Circuits Electronics Design Automation"));
        auto subtitleFont = subtitle_->font();
        subtitleFont.setPointSize(subtitleFont.pointSize() + 2);
        subtitle_->setFont(subtitleFont);
        titleColumn->addWidget(subtitle_);
        titleColumn->addStretch();
        header->addLayout(titleColumn, 1);
        layout->addLayout(header);

        auto *content = new QHBoxLayout;
        content->setSpacing(20);

        auto *left = new QVBoxLayout;
        left->setSpacing(16);
        auto *createGroup = new QGroupBox(QStringLiteral("Create Project"));
        auto *form = new QFormLayout(createGroup);
        name_ = new QLineEdit;
        name_->setPlaceholderText(QStringLiteral("My design"));
        name_->setClearButtonEnabled(true);
        location_ = new QLineEdit(QDir::homePath());
        auto *browseButton = new QPushButton(QStringLiteral("Browse…"));
        auto *locationRow = new QWidget;
        auto *locationLayout = new QHBoxLayout(locationRow);
        locationLayout->setContentsMargins(0, 0, 0, 0);
        locationLayout->addWidget(location_, 1);
        locationLayout->addWidget(browseButton);
        form->addRow(QStringLiteral("Project name"), name_);
        form->addRow(QStringLiteral("Location"), locationRow);
        auto *buttonRow = new QHBoxLayout;
        auto *openButton = new QPushButton(QStringLiteral("Open Project…"));
        createButton_ = new QPushButton(QStringLiteral("Create Project"));
        createButton_->setDefault(true);
        buttonRow->addWidget(openButton);
        buttonRow->addStretch();
        buttonRow->addWidget(createButton_);
        form->addRow(buttonRow);
        left->addWidget(createGroup);

        auto *recentGroup = new QGroupBox(QStringLiteral("Recent Projects"));
        auto *recentLayout = new QVBoxLayout(recentGroup);
        recent_ = new QListWidget;
        recent_->setAlternatingRowColors(true);
        recent_->setAccessibleName(QStringLiteral("Recent projects"));
        recent_->setMinimumHeight(200);
        recentLayout->addWidget(recent_, 1);
        left->addWidget(recentGroup, 1);
        content->addLayout(left, 3);

        auto *right = new QVBoxLayout;
        right->setSpacing(16);
        auto *communityGroup = new QGroupBox(QStringLiteral("Community"));
        auto *communityLayout = new QVBoxLayout(communityGroup);
        communityLayout->addWidget(makeLink(QStringLiteral("Manual"), QString::fromLatin1(kManualUrl)));
        communityLayout->addWidget(makeLink(QStringLiteral("Donate"), QString::fromLatin1(kDonationUrl)));
        communityLayout->addWidget(makeLink(QStringLiteral("Source code"), QString::fromLatin1(kSourceUrl)));
        communityLayout->addWidget(makeLink(QStringLiteral("Website"), QString::fromLatin1(kWebsiteUrl)));
        communityLayout->addStretch();
        right->addWidget(communityGroup);

        auto *newsGroup = new QGroupBox(QStringLiteral("News"));
        auto *newsLayout = new QVBoxLayout(newsGroup);
        newsPlaceholder_ = new QLabel(QStringLiteral("No news yet."));
        newsPlaceholder_->setAlignment(Qt::AlignCenter);
        newsLayout->addWidget(newsPlaceholder_);
        newsLayout->addStretch();
        right->addWidget(newsGroup, 1);
        content->addLayout(right, 2);

        layout->addLayout(content, 1);

        connect(browseButton, &QPushButton::clicked, this, [this] {
            const auto directory =
                QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Project Location"), location_->text());
            if (!directory.isEmpty())
                location_->setText(directory);
        });
        connect(openButton, &QPushButton::clicked, this, [this] {
            if (onOpenProject_)
                onOpenProject_();
        });
        connect(createButton_, &QPushButton::clicked, this, [this] {
            if (onCreateProject_)
                onCreateProject_(name_->text().trimmed(), location_->text().trimmed());
        });
        auto submit = [this] {
            if (createButton_->isEnabled())
                createButton_->click();
        };
        connect(name_, &QLineEdit::returnPressed, this, submit);
        connect(location_, &QLineEdit::returnPressed, this, submit);
        auto updateCreateButton = [this] {
            createButton_->setEnabled(!name_->text().trimmed().isEmpty() && !location_->text().trimmed().isEmpty());
        };
        connect(name_, &QLineEdit::textChanged, this, updateCreateButton);
        connect(location_, &QLineEdit::textChanged, this, updateCreateButton);
        connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
            if (!item)
                return;
            const auto path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty() && onOpenRecent_)
                onOpenRecent_(path);
        });
        updateCreateButton();
        refreshAppearance();
    }

    void setOnCreateProject(CreateProjectCallback callback) { onCreateProject_ = std::move(callback); }
    void setOnOpenProject(OpenProjectCallback callback) { onOpenProject_ = std::move(callback); }
    void setOnOpenRecent(OpenRecentCallback callback) { onOpenRecent_ = std::move(callback); }

    void setRecentProjects(const QStringList &paths) {
        recent_->clear();
        if (paths.isEmpty()) {
            auto *item = new QListWidgetItem(QStringLiteral("No recent projects"), recent_);
            item->setFlags(Qt::NoItemFlags);
            return;
        }
        for (const auto &path : paths) {
            auto *item = new QListWidgetItem(QFileInfo(path).fileName(), recent_);
            item->setData(Qt::UserRole, path);
            item->setToolTip(path);
        }
    }

    void clearCreateForm() {
        name_->clear();
        name_->setFocus();
    }

    void refreshAppearance() {
        const auto muted = ice::uiColor(ice::UiColor::Text2).name();
        subtitle_->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
        newsPlaceholder_->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
    }

  private:
    static QLabel *makeLink(const QString &text, const QString &url) {
        auto *label =
            new QLabel(QStringLiteral("<a href=\"%1\">%2</a>").arg(url.toHtmlEscaped(), text.toHtmlEscaped()));
        label->setOpenExternalLinks(true);
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        return label;
    }

    QLineEdit *name_ = nullptr;
    QLineEdit *location_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QListWidget *recent_ = nullptr;
    QLabel *subtitle_ = nullptr;
    QLabel *newsPlaceholder_ = nullptr;
    CreateProjectCallback onCreateProject_;
    OpenProjectCallback onOpenProject_;
    OpenRecentCallback onOpenRecent_;
};

class EmptySchematicPanel : public QWidget {
  public:
    using CreateCallback = std::function<void(const QString &name, const std::string &pdk)>;

    explicit EmptySchematicPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->addStretch(1);
        auto *card = new QGroupBox(QStringLiteral("Create a schematic"));
        card->setMaximumWidth(520);
        auto *cardLayout = new QVBoxLayout(card);
        auto *description =
            new QLabel(QStringLiteral("This project has no schematics yet. Create one to start drawing."));
        description->setWordWrap(true);
        cardLayout->addWidget(description);
        auto *form = new QFormLayout;
        name_ = new QLineEdit(QStringLiteral("top"));
        name_->setClearButtonEnabled(true);
        pdk_ = new QComboBox;
        pdk_->setAccessibleName(QStringLiteral("Schematic PDK"));
        form->addRow(QStringLiteral("Schematic name"), name_);
        form->addRow(QStringLiteral("PDK"), pdk_);
        cardLayout->addLayout(form);
        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch();
        auto *createButton = new QPushButton(QStringLiteral("Create Schematic"));
        createButton->setDefault(true);
        buttonRow->addWidget(createButton);
        cardLayout->addLayout(buttonRow);
        layout->addWidget(card, 0, Qt::AlignHCenter);
        layout->addStretch(1);

        refreshPdks(configuredPackageInstallDirectory());
        connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
        connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
    }

    void setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

    void refreshPdks(const std::filesystem::path &installDirectory) {
        const auto current = pdk_->currentData().toString();
        QSignalBlocker blocker(pdk_);
        pdk_->clear();
        pdk_->addItem(QStringLiteral("Generic PDK"), QString::fromLatin1(ice::genericPdkId));
        for (const auto &entry : installedPdkPackages(installDirectory)) {
            const auto packageName = QString::fromStdString(entry.spec.name);
            auto label = packagePdkDisplayName(entry.spec);
            if (label != packageName)
                label += QStringLiteral("  (%1)").arg(packageName);
            pdk_->addItem(label, packageName);
        }
        const auto index = pdk_->findData(current);
        if (index >= 0)
            pdk_->setCurrentIndex(index);
    }

    void prepareForDisplay() {
        name_->selectAll();
        name_->setFocus();
    }

  private:
    void submit() {
        const auto name = name_->text().trimmed();
        if (name.isEmpty()) {
            name_->setFocus();
            return;
        }
        if (onCreate_)
            onCreate_(name, pdk_->currentData().toString().toStdString());
    }

    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    CreateCallback onCreate_;
};

class EmptyLayoutPanel : public QWidget {
  public:
    using CreateCallback = std::function<void(const QString &name)>;

    explicit EmptyLayoutPanel(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->addStretch(1);
        auto *card = new QGroupBox(QStringLiteral("Create a layout cell"));
        card->setMaximumWidth(520);
        auto *cardLayout = new QVBoxLayout(card);
        auto *description =
            new QLabel(QStringLiteral("This project has no layout cells yet. Create one to start drawing."));
        description->setWordWrap(true);
        cardLayout->addWidget(description);
        auto *form = new QFormLayout;
        name_ = new QLineEdit(QStringLiteral("top"));
        name_->setClearButtonEnabled(true);
        form->addRow(QStringLiteral("Layout cell name"), name_);
        cardLayout->addLayout(form);
        auto *buttonRow = new QHBoxLayout;
        buttonRow->addStretch();
        auto *createButton = new QPushButton(QStringLiteral("Create Layout Cell"));
        createButton->setDefault(true);
        buttonRow->addWidget(createButton);
        cardLayout->addLayout(buttonRow);
        layout->addWidget(card, 0, Qt::AlignHCenter);
        layout->addStretch(1);

        connect(createButton, &QPushButton::clicked, this, [this] { submit(); });
        connect(name_, &QLineEdit::returnPressed, this, [this] { submit(); });
    }

    void setOnCreate(CreateCallback callback) { onCreate_ = std::move(callback); }

    void prepareForDisplay() {
        name_->selectAll();
        name_->setFocus();
    }

  private:
    void submit() {
        const auto name = name_->text().trimmed();
        if (name.isEmpty()) {
            name_->setFocus();
            return;
        }
        if (onCreate_)
            onCreate_(name);
    }

    QLineEdit *name_ = nullptr;
    CreateCallback onCreate_;
};

class WorkspaceStack : public QStackedWidget {
  public:
    using QStackedWidget::QStackedWidget;

    // Keep the horizontal minimum from the pages, but do not let the pages'
    // vertical minimum stop the user from enlarging bottom docks.
    QSize minimumSizeHint() const override { return QSize(QStackedWidget::minimumSizeHint().width(), 0); }
};

class DocumentWindow : public QMainWindow {
  public:
    explicit DocumentWindow(ice::AppearanceMode appearanceMode = ice::AppearanceMode::System)
        : appearanceMode_(appearanceMode) {
        setWindowTitle(QStringLiteral("ICE"));
        if (const auto logo = ice::appLogo(); !logo.isNull())
            setWindowIcon(QIcon(logo));
        setDockNestingEnabled(true);
        createMenus();
        createActivityDock();
        createDocuments();
        createStatusBar();
        toolEscapeShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        toolEscapeShortcut_->setContext(Qt::WindowShortcut);
        toolEscapeShortcut_->setEnabled(false);
        connect(toolEscapeShortcut_, &QShortcut::activated, this, [this] { selectSchematicTool(); });
        showStartup();
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (appearanceMode_ != ice::AppearanceMode::System)
                return;
            ice::applyAppearance(appearanceMode_);
            refreshAppearance();
        });
    }

    static bool directoryHasSymbols(const std::filesystem::path &directory) {
        std::error_code status;
        if (!std::filesystem::is_directory(directory, status) || status)
            return false;
        for (const auto &entry : std::filesystem::directory_iterator(directory, status)) {
            if (status)
                return false;
            if (entry.is_regular_file() && entry.path().extension() == ".toml")
                return true;
        }
        return false;
    }

    std::vector<SymbolDefinition> symbolLibraryForPdk(const std::string &pdk) const {
        auto symbols = genericSymbolLibrary();
        if (pdk != ice::genericPdkId) {
            const auto symbolDirectory = configuredPackageInstallDirectory() / pdk / "symbols";
            if (directoryHasSymbols(symbolDirectory)) {
                const auto pdkSymbols = packageSymbolLibrary(symbolDirectory);
                symbols.insert(symbols.end(), pdkSymbols.begin(), pdkSymbols.end());
            }
        }
        return symbols;
    }

    bool setProject(ice::Project project) {
        if (!ensureProjectPackages(project))
            return false;
        project_ = std::make_unique<ice::ProjectSession>(std::move(project));
        layoutViewInitialized_ = false;
        projectPath_.clear();
        schematicSymbolsLoaded_ = false;
        schematicSymbolsPdk_.clear();
        refreshEmbeddedPanels();
        refreshSimulationSchematics();
        refreshSchematic();
        refreshLayout();
        setWindowTitle(QStringLiteral("ICE - %1").arg(QString::fromStdString(project_->project().name)));
        statusBar_->setProjectName(QString::fromStdString(project_->project().name));
        statusBar()->showMessage(QStringLiteral("Project loaded"));
        showWorkspace();
        return true;
    }

  private:
    bool ensureProjectPackages(const ice::Project &project) {
        if (project.packageDependencies.empty())
            return true;
        const auto installDirectory = configuredPackageInstallDirectory();
        auto installed = installedPackages(installDirectory);
        const auto isInstalled = [&](const std::string &name) {
            return std::any_of(installed.begin(), installed.end(),
                               [&](const auto &entry) { return entry.spec.name == name; });
        };
        std::vector<std::string> missing;
        for (const auto &dependency : project.packageDependencies)
            if (!isInstalled(dependency))
                missing.push_back(dependency);
        if (missing.empty())
            return true;
        if (!installMissingPackages(this, missing, configuredPackageIndexDirectory(), installDirectory))
            return false;
        installed = installedPackages(installDirectory);
        for (const auto &dependency : project.packageDependencies) {
            if (!isInstalled(dependency)) {
                QMessageBox::critical(
                    this, QStringLiteral("Missing Packages"),
                    QStringLiteral("The project cannot be opened because package %1 is still not installed.")
                        .arg(QString::fromStdString(dependency)));
                return false;
            }
        }
        return true;
    }

    ice::Project makeNewProject(const QString &name) const {
        ice::Project project;
        project.name = name.toStdString();
        project.identity = project.name;
        project.units = {};
        project.grid = 1;
        return project;
    }

    QStringList loadRecentProjects() const {
        QSettings settings;
        auto projects = settings.value(QStringLiteral("recentProjects")).toStringList();
        projects.removeAll(QString());
        return projects;
    }

    void storeRecentProjects(const QStringList &projects) {
        QSettings settings;
        settings.setValue(QStringLiteral("recentProjects"), projects);
    }

    void addRecentProject(const QString &path) {
        if (path.isEmpty())
            return;
        const auto normalized = QFileInfo(path).absoluteFilePath();
        auto projects = loadRecentProjects();
        projects.removeAll(normalized);
        projects.prepend(normalized);
        while (projects.size() > 10)
            projects.removeLast();
        storeRecentProjects(projects);
        if (startupView_)
            startupView_->setRecentProjects(projects);
    }

    void removeRecentProject(const QString &path) {
        auto projects = loadRecentProjects();
        projects.removeAll(QFileInfo(path).absoluteFilePath());
        storeRecentProjects(projects);
        if (startupView_)
            startupView_->setRecentProjects(projects);
    }

    void createProject(const QString &rawName, const QString &rawLocation) {
        const auto name = rawName.trimmed();
        const auto location = rawLocation.trimmed();
        if (name.isEmpty() || location.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Create Project"),
                                     QStringLiteral("A project name and location are required."));
            return;
        }
        if (std::any_of(name.begin(), name.end(),
                        [](QChar character) { return QStringLiteral("<>:\"/\\|?*").contains(character); })) {
            QMessageBox::information(this, QStringLiteral("Create Project"),
                                     QStringLiteral("The project name contains characters that are not valid in a "
                                                    "file name."));
            return;
        }
        QDir directory(location);
        if (!directory.exists() && !QDir().mkpath(location)) {
            QMessageBox::critical(this, QStringLiteral("Create Project"),
                                  QStringLiteral("The project location could not be created:\n%1").arg(location));
            return;
        }
        const auto path = directory.filePath(name + QStringLiteral(".ice.json"));
        if (QFileInfo::exists(path) &&
            QMessageBox::question(this, QStringLiteral("Create Project"),
                                  QStringLiteral("A project file already exists at:\n%1\n\nOverwrite it?").arg(path),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        auto project = makeNewProject(name);
        try {
            project.save(path.toStdString());
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Project"), QString::fromStdString(error.what()));
            return;
        }
        if (!setProject(std::move(project)))
            return;
        projectPath_ = path;
        addRecentProject(path);
        if (startupView_)
            startupView_->clearCreateForm();
        statusBar()->showMessage(QStringLiteral("New project created"));
    }

    void openProjectPath(const QString &path) {
        if (path.isEmpty())
            return;
        if (!QFileInfo::exists(path)) {
            QMessageBox::warning(this, QStringLiteral("Open Project"),
                                 QStringLiteral("The project file no longer exists:\n%1").arg(path));
            removeRecentProject(path);
            return;
        }
        try {
            auto project = ice::Project::load(path.toStdString());
            if (!setProject(std::move(project)))
                return;
            projectPath_ = path;
            addRecentProject(path);
            statusBar()->showMessage(QStringLiteral("Project opened"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Open Project"), QString::fromStdString(error.what()));
        }
    }

    void newProject() {
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("New Project"), QStringLiteral("Project name:"),
                                                QLineEdit::Normal, QStringLiteral("untitled"), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        setProject(makeNewProject(name.trimmed()));
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Choose New Project Path"),
                                                       name.trimmed() + QStringLiteral(".ice.json"),
                                                       QStringLiteral("ICE projects (*.ice.json);;All files (*)"));
        if (!path.isEmpty()) {
            try {
                project_->save(path.toStdString());
                projectPath_ = path;
                addRecentProject(path);
            } catch (const ice::ProjectError &error) {
                QMessageBox::critical(this, QStringLiteral("New Project"), QString::fromStdString(error.what()));
            }
        }
        statusBar()->showMessage(QStringLiteral("New project created"));
    }

    void openProject() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open ICE Project"), QString(),
                                                       QStringLiteral("ICE projects (*.ice.json);;All files (*)"));
        if (!path.isEmpty())
            openProjectPath(path);
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
        if (path.isEmpty())
            return;
        try {
            project_->save(path.toStdString());
            projectPath_ = path;
            addRecentProject(path);
            statusBar()->showMessage(QStringLiteral("Project saved"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Save Project"), QString::fromStdString(error.what()));
        }
    }

    void openLayoutInterchange() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Open GDS/OASIS Layout"), QString(),
                                                       QStringLiteral("GDSII Stream (*.gds *.gdsii);;OASIS "
                                                                      "(unsupported) (*.oas *.oasis);;All files (*)"));
        if (path.isEmpty())
            return;
        try {
            const auto suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("oas") || suffix == QStringLiteral("oasis"))
                throw ice::ProjectError("interchange.oasis", "Binary OASIS import is not supported yet");
            if (suffix != QStringLiteral("gds") && suffix != QStringLiteral("gdsii"))
                throw ice::ProjectError("interchange.format", "Unsupported layout format (expected .gds/.gdsii)");
            setProject(ice::LayoutInterchange::importGds(path.toStdString()));
            projectPath_.clear();
            statusBar()->showMessage(QStringLiteral("Layout interchange opened"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Open GDS/OASIS Layout"), QString::fromStdString(error.what()));
        } catch (const std::exception &error) {
            QMessageBox::critical(this, QStringLiteral("Open GDS/OASIS Layout"), QString::fromUtf8(error.what()));
        }
    }

    void saveLayoutInterchange() {
        if (!project_ || project_->project().layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Save GDS/OASIS Layout"),
                                     QStringLiteral("Create or open a project with a layout cell first."));
            return;
        }
        auto cellId = activeLayoutId_;
        if (!project_->project().layoutCells.contains(cellId))
            cellId = project_->project().layoutCells.begin()->first;
        const auto path =
            QFileDialog::getSaveFileName(this, QStringLiteral("Save GDS/OASIS Layout"),
                                         QString::fromStdString(project_->project().layoutCells.at(cellId).name),
                                         QStringLiteral("GDSII Stream (*.gds *.gdsii);;OASIS "
                                                        "(unsupported) (*.oas *.oasis);;All files (*)"));
        if (path.isEmpty())
            return;
        try {
            const auto suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("oas") || suffix == QStringLiteral("oasis"))
                throw ice::ProjectError("interchange.oasis", "Binary OASIS export is not supported yet");
            if (suffix != QStringLiteral("gds") && suffix != QStringLiteral("gdsii"))
                throw ice::ProjectError("interchange.format", "Unsupported layout format (expected .gds/.gdsii)");
            ice::LayoutInterchange::exportGds(project_->project(), cellId, path.toStdString());
            statusBar()->showMessage(QStringLiteral("Layout interchange saved"));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Save GDS/OASIS Layout"), QString::fromStdString(error.what()));
        }
    }

    void closeProject() {
        project_.reset();
        projectPath_.clear();
        refreshEmbeddedPanels();
        if (schematicCanvas_) {
            schematicCanvas_->scene()->clear();
            schematicCanvas_->clearSelection();
        }
        if (schematicSymbolsPanel_)
            schematicSymbolsPanel_->clear();
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->clear();
        schematicSymbolsLoaded_ = false;
        schematicSymbolsPdk_.clear();
        if (layoutCanvas_)
            layoutCanvas_->scene()->clear();
        simulationOutput_->setPlainText(QStringLiteral("No project open."));
        schematicSelector_->clear();
        simulationPlot_->clear();
        if (simulationPanel_)
            simulationPanel_->clear();
        activeSimulationId_ = 0;
        rebuildGraphTabs({});
        setWindowTitle(QStringLiteral("ICE"));
        statusBar_->setProjectName(QStringLiteral("No project open"));
        updateSelectionStatus();
        statusBar()->showMessage(QStringLiteral("Ready — no project open"));
        showStartup();
    }

    void createSchematic() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateSchematicDialog dialog(installDirectory, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createSchematic(dialog.schematicName(), dialog.pdkId());
    }

    void createSchematic(const QString &name, const std::string &pdk) {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Schematic"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (name.trimmed().isEmpty())
            return;
        const auto installDirectory = configuredPackageInstallDirectory();
        auto pdkLabel = QStringLiteral("Generic PDK");
        bool addDependency = false;
        if (pdk != ice::genericPdkId) {
            const auto packages = installedPackages(installDirectory);
            const auto package = std::find_if(packages.begin(), packages.end(),
                                              [&](const auto &entry) { return entry.spec.name == pdk; });
            if (package == packages.end()) {
                QMessageBox::critical(
                    this, QStringLiteral("Create Schematic"),
                    QStringLiteral("The selected PDK package %1 is not installed.").arg(QString::fromStdString(pdk)));
                return;
            }
            if (!packageProvidesPdk(package->spec)) {
                QMessageBox::critical(
                    this, QStringLiteral("Create Schematic"),
                    QStringLiteral("Package %1 does not provide a PDK.").arg(QString::fromStdString(pdk)));
                return;
            }
            pdkLabel = packagePdkDisplayName(package->spec);
            const auto &dependencies = project_->project().packageDependencies;
            if (std::find(dependencies.begin(), dependencies.end(), pdk) == dependencies.end()) {
                const auto displayName = packagePdkDisplayName(package->spec);
                const auto response = QMessageBox::question(
                    this, QStringLiteral("Add Package Dependency"),
                    QStringLiteral("This schematic uses the %1 PDK from package %2. Add %2 to the project's "
                                   "package dependency list?")
                        .arg(displayName, QString::fromStdString(pdk)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (response != QMessageBox::Yes)
                    return;
                addDependency = true;
            }
        }
        try {
            project_->execute("create schematic", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                activeSchematicId_ = ice::createSchematicCell(project, name.trimmed().toStdString(), pdk);
            });
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Schematic created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Schematic"), QString::fromStdString(error.what()));
        }
    }

    void createLayoutCell() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layout Cell"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Create Layout Cell"), QStringLiteral("Layout cell name:"),
                                  QLineEdit::Normal, QStringLiteral("top"), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        createLayoutCell(name.trimmed());
    }

    void createLayoutCell(const QString &name) {
        if (!project_ || name.trimmed().isEmpty())
            return;
        project_->execute("create layout cell", [&](ice::Project &project) {
            activeLayoutId_ = ice::createLayoutCell(project, name.trimmed().toStdString());
        });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout cell created"));
    }

    void removeSchematic(ice::ObjectId cellId) {
        if (!project_ || cellId == 0)
            return;
        const auto &cells = project_->project().schematicCells;
        const auto cell = cells.find(cellId);
        if (cell == cells.end())
            return;
        QStringList simulations;
        for (const auto &[_, simulation] : project_->project().simulations)
            if (simulation.schematic == cellId)
                simulations << QString::fromStdString(simulation.name);
        if (!simulations.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Remove Schematic"),
                QStringLiteral("Schematic '%1' is used by these simulations:\n%2\n\nDelete or reassign them first.")
                    .arg(QString::fromStdString(cell->second.name), simulations.join(QStringLiteral("\n"))));
            return;
        }
        const auto name = QString::fromStdString(cell->second.name);
        if (QMessageBox::question(this, QStringLiteral("Remove Schematic"),
                                  QStringLiteral("Remove schematic '%1'?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("remove schematic", [&](ice::Project &project) { project.schematicCells.erase(cellId); });
        if (activeSchematicId_ == cellId)
            activeSchematicId_ = 0;
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Schematic '%1' removed").arg(name));
    }

    void removeLayoutCell(ice::ObjectId cellId) {
        if (!project_ || cellId == 0)
            return;
        const auto &cells = project_->project().layoutCells;
        const auto cell = cells.find(cellId);
        if (cell == cells.end())
            return;
        QStringList references;
        for (const auto &[otherId, otherCell] : cells) {
            if (otherId == cellId)
                continue;
            for (const auto &[_, instance] : otherCell.instances)
                if (instance.cell == cellId)
                    references << QStringLiteral("%1 in %2")
                                      .arg(QString::fromStdString(instance.name),
                                           QString::fromStdString(otherCell.name));
        }
        if (!references.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Remove Layout Cell"),
                QStringLiteral("Layout cell '%1' is used as an instance in:\n%2\n\nRemove those instances first.")
                    .arg(QString::fromStdString(cell->second.name), references.join(QStringLiteral("\n"))));
            return;
        }
        const auto name = QString::fromStdString(cell->second.name);
        if (QMessageBox::question(this, QStringLiteral("Remove Layout Cell"),
                                  QStringLiteral("Remove layout cell '%1'?").arg(name),
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        project_->execute("remove layout cell", [&](ice::Project &project) { project.layoutCells.erase(cellId); });
        if (activeLayoutId_ == cellId)
            activeLayoutId_ = 0;
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout cell '%1' removed").arg(name));
    }

    void createLayer() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Create Layer"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Create Layer"), QStringLiteral("Layer name:"),
                                                QLineEdit::Normal, QStringLiteral("metal1"), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        int number = 1;
        const auto existingLayers = project_->project().layers.size();
        number = static_cast<int>(existingLayers) + 1;
        project_->execute("create layer",
                          [&](ice::Project &project) { ice::addLayer(project, name.trimmed().toStdString(), number); });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layer created"));
    }

    void managePackages() {
        QSettings settings;
        PackageManagerOptions options;
        options.indexDirectory = configuredPackageIndexDirectory();
        options.installDirectory = configuredPackageInstallDirectory();
        options.projectPath = std::filesystem::path(projectPath_.toStdString());
        options.hasProject = project_ != nullptr;
        options.activatePackage = [this](const ice::PackageSpec &spec) { activatePdkTechnology(spec); };

        PackageManagerDialog dialog(std::move(options), this);
        dialog.exec();
        settings.setValue(QStringLiteral("packages/indexDirectory"),
                          QString::fromStdString(dialog.indexDirectory().string()));
        settings.setValue(QStringLiteral("packages/localRepository"),
                          QString::fromStdString(dialog.installDirectory().string()));
    }

    void activatePdkTechnology(const ice::PackageSpec &spec) {
        if (!project_ || !packageProvidesPdk(spec))
            return;
        const auto displayName = packagePdkDisplayName(spec);
        project_->execute("activate PDK technology", [&](ice::Project &project) {
            if (std::find(project.packageDependencies.begin(), project.packageDependencies.end(), spec.name) ==
                project.packageDependencies.end())
                project.packageDependencies.push_back(spec.name);
            if (spec.pdk) {
                for (const auto &layer : spec.pdk->layers) {
                    const auto exists =
                        std::find_if(project.layers.begin(), project.layers.end(),
                                     [&](const auto &entry) { return entry.second.name == layer.name; });
                    if (exists == project.layers.end())
                        ice::addLayer(project, layer.name, layer.number);
                }
            }
            if (project.schematicCells.empty()) {
                const auto schematic =
                    spec.pdk && !spec.pdk->defaultSchematic.empty() ? spec.pdk->defaultSchematic : spec.name + "_top";
                ice::createSchematicCell(project, schematic, spec.name);
            }
        });
        refreshProject();
        statusBar()->showMessage(QStringLiteral("%1 technology activated").arg(displayName));
    }

    void refreshLayout() {
        if (!layoutCanvas_ || !project_)
            return;
        layoutCanvas_->scene()->clear();
        const auto &project = project_->project();
        auto cellIt = project.layoutCells.find(activeLayoutId_);
        if (cellIt == project.layoutCells.end()) {
            if (project.layoutCells.empty())
                return;
            cellIt = project.layoutCells.begin();
            activeLayoutId_ = cellIt->first;
        }
        const auto styles = layerStylesFor(project);
        layoutCanvas_->setLayerStyles(styles);
        for (const auto &[id, shape] : cellIt->second.shapes) {
            const auto style = styles.contains(shape.layer)
                                   ? styles.at(shape.layer)
                                   : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
            const QPen pen(style.color, 1);
            QBrush brush(QColor(style.color.red(), style.color.green(), style.color.blue(), 110), style.pattern);
            if (style.pattern == Qt::NoBrush)
                brush = QBrush(Qt::NoBrush);
            QGraphicsItem *item = nullptr;
            if (shape.polygon.size() >= 3) {
                QPolygonF polygon;
                for (const auto &point : shape.polygon)
                    polygon << QPointF(point.x, -point.y);
                item = layoutCanvas_->scene()->addPolygon(polygon, pen, brush);
            } else {
                item = layoutCanvas_->scene()->addRect(QRectF(shape.rectangle.x,
                                                              -shape.rectangle.y - shape.rectangle.height,
                                                              shape.rectangle.width, shape.rectangle.height),
                                                       pen, brush);
            }
            item->setData(0, QVariant::fromValue<qulonglong>(id));
            item->setFlag(QGraphicsItem::ItemIsSelectable);
            item->setFlag(QGraphicsItem::ItemIsMovable);
            item->setToolTip(QStringLiteral("Shape %1").arg(id));
        }
        if (!layoutViewInitialized_) {
            layoutCanvas_->fitToContent();
            layoutViewInitialized_ = true;
        }
    }

    void placeNetLabel(QPointF position) {
        if (!project_ || project_->project().schematicCells.empty())
            return;
        position = nearestWireLabelPosition(position);
        bool accepted = false;
        const auto name = QInputDialog::getText(this, QStringLiteral("Place Net Label"), QStringLiteral("Net name:"),
                                                QLineEdit::Normal, QStringLiteral("NET1"), &accepted);
        if (!accepted || name.trimmed().isEmpty())
            return;
        const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                ? activeSchematicId_
                                : project_->project().schematicCells.begin()->first;
        project_->execute("place net label", [&](ice::Project &project) {
            auto &cell = project.schematicCells.at(cellId);
            const auto id = project.nextId();
            cell.nets.emplace(id, ice::Net{id,
                                           name.trimmed().toStdString(),
                                           {},
                                           static_cast<ice::Coordinate>(position.x()),
                                           static_cast<ice::Coordinate>(-position.y())});
        });
        refreshProject();
    }

    QPointF nearestWireLabelPosition(QPointF position) const {
        if (!schematicCanvas_ || !schematicCanvas_->scene())
            return position;
        qreal bestDistance = std::numeric_limits<qreal>::max();
        QPointF bestPosition = position;
        for (auto *item : schematicCanvas_->scene()->items()) {
            auto *wire = dynamic_cast<QGraphicsPathItem *>(item);
            if (!wire || wire->data(0).toString() != QStringLiteral("wire"))
                continue;
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
        for (auto *item : schematicCanvas_->scene()->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
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
        try {
            const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                    ? activeSchematicId_
                                    : project_->project().schematicCells.begin()->first;
            const auto &cell = project_->project().schematicCells.at(cellId);
            const auto pdk = cell.pdk.empty() ? std::string(ice::genericPdkId) : cell.pdk;
            std::vector<SymbolDefinition> pdkSymbols;
            if (pdk != ice::genericPdkId) {
                const auto symbolDirectory = configuredPackageInstallDirectory() / pdk / "symbols";
                if (!directoryHasSymbols(symbolDirectory)) {
                    QMessageBox::warning(
                        this, QStringLiteral("Package PDK symbols"),
                        QStringLiteral("The PDK package %1 is not installed or does not provide schematic symbols. "
                                       "Install or update it from Manage Packages before placing symbols.")
                            .arg(QString::fromStdString(pdk)));
                    return;
                }
                pdkSymbols = packageSymbolLibrary(symbolDirectory);
                if (pdkSymbols.empty()) {
                    QMessageBox::warning(
                        this, QStringLiteral("Package PDK symbols"),
                        QStringLiteral("The PDK package %1 does not provide a valid schematic symbol library.")
                            .arg(QString::fromStdString(pdk)));
                    return;
                }
            }
            SymbolLibraryDialog dialog(genericSymbolLibrary(), std::move(pdkSymbols), this);
            if (dialog.exec() != QDialog::Accepted)
                return;
            const auto definition = dialog.selected();
            placeSymbolDefinition(definition);
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Symbol"), QString::fromStdString(error.what()));
        }
    }

    bool placeSymbolDefinition(const SymbolDefinition &definition, std::optional<QPointF> position = std::nullopt) {
        if (!project_ || !schematicCanvas_ || project_->project().schematicCells.empty())
            return false;
        try {
            const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                    ? activeSchematicId_
                                    : project_->project().schematicCells.begin()->first;
            const auto scenePosition =
                position.value_or(schematicCanvas_->mapToScene(schematicCanvas_->viewport()->rect().center()));
            project_->execute("place symbol", [&](ice::Project &project) {
                const auto symbolId =
                    ice::addSymbol(project, cellId, definition.name.toStdString(), definition.svg.toStdString());
                auto &symbol = project.schematicCells.at(cellId).symbols.at(symbolId);
                for (auto pin : definition.pins) {
                    pin.id = project.nextId();
                    pin.direction = pin.x < 0   ? ice::PinDirection::left
                                    : pin.x > 0 ? ice::PinDirection::right
                                    : pin.y > 0 ? ice::PinDirection::up
                                                : ice::PinDirection::down;
                    symbol.pins.push_back(std::move(pin));
                }
                const auto instanceName =
                    nextInstanceName(project.schematicCells.at(cellId), definition.name.toStdString());
                const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, instanceName,
                                                                  static_cast<ice::Coordinate>(scenePosition.x()),
                                                                  static_cast<ice::Coordinate>(-scenePosition.y()));
                // Seed the symbol's default parameters (resistance, capacitance,
                // ...) so they are editable in the properties panel right away.
                auto &placedInstance = project.schematicCells.at(cellId).instances.at(instanceId);
                ice::seedDefaultParameters(project, placedInstance, instanceDefaults(definition));
            });
            refreshProject();
            return true;
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Symbol"), QString::fromStdString(error.what()));
            return false;
        }
    }

    // Selection is view state, but it is discussed with the domain model so the
    // status bar and properties panel always describe the selected instance.
    void updateSelectionStatus() {
        const auto instanceId = schematicCanvas_ ? schematicCanvas_->selectedInstanceId() : 0;
        if (!selectionStatus_)
            return;
        if (instanceId == 0) {
            selectionStatus_->setText(QStringLiteral("Selection: none"));
            if (!schematicPropertiesPanel_)
                return;
            if (!project_ || project_->project().schematicCells.empty()) {
                schematicPropertiesPanel_->clearSelection();
                return;
            }
            const auto &cells = project_->project().schematicCells;
            auto cell = cells.find(activeSchematicId_);
            if (cell == cells.end())
                cell = cells.begin();
            if (cell == cells.end()) {
                schematicPropertiesPanel_->clearSelection();
                return;
            }
            schematicPropertiesPanel_->setSchematic(cell->second, generatedSchematicSymbol(cell->second).svg);
            return;
        }
        if (!project_ || !schematicPropertiesPanel_) {
            selectionStatus_->setText(QStringLiteral("Selection: none"));
            return;
        }
        for (const auto &[_, cell] : project_->project().schematicCells) {
            const auto instance = cell.instances.find(instanceId);
            if (instance == cell.instances.end())
                continue;
            const auto symbol = cell.symbols.find(instance->second.symbol);
            const auto symbolName = symbol == cell.symbols.end() ? std::string() : symbol->second.name;
            const auto symbolSvg = symbol == cell.symbols.end() ? std::string() : symbol->second.artwork;
            const auto type = symbolName.empty() ? QStringLiteral("Schematic") : QString::fromStdString(symbolName);
            selectionStatus_->setText(
                QStringLiteral("Selection: %1 (%2)").arg(QString::fromStdString(instance->second.name), type));
            schematicPropertiesPanel_->setInstance(instance->second, symbolName, symbolSvg);
            return;
        }
        selectionStatus_->setText(QStringLiteral("Selection: none"));
        schematicPropertiesPanel_->clearSelection();
    }

    void updateInstanceParameter(ice::ObjectId instanceId, ice::ObjectId parameterId, const std::string &name,
                                 const std::string &value) {
        if (!project_)
            return;
        try {
            project_->execute("edit parameter", [&](ice::Project &project) {
                for (auto &[_, cell] : project.schematicCells) {
                    const auto instance = cell.instances.find(instanceId);
                    if (instance == cell.instances.end())
                        continue;
                    for (auto &parameter : instance->second.parameters) {
                        if ((parameterId != 0 && parameter.id == parameterId) ||
                            (parameterId == 0 && parameter.name == name)) {
                            parameter.value = value;
                            return;
                        }
                    }
                    return;
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Edit Parameter"), QString::fromStdString(error.what()));
            updateSelectionStatus();
        }
    }

    void updateSchematicParameter(ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name,
                                  const std::string &value) {
        if (!project_)
            return;
        try {
            project_->execute("edit schematic parameter", [&](ice::Project &project) {
                const auto cell = project.schematicCells.find(cellId);
                if (cell == project.schematicCells.end())
                    return;
                for (auto &parameter : cell->second.parameters) {
                    if ((parameterId != 0 && parameter.id == parameterId) ||
                        (parameterId == 0 && parameter.name == name)) {
                        parameter.value = value;
                        return;
                    }
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Edit Parameter"), QString::fromStdString(error.what()));
            updateSelectionStatus();
        }
    }

    void addSchematicParameter(ice::ObjectId cellId, const std::string &name) {
        if (!project_)
            return;
        project_->execute("add schematic parameter", [&](ice::Project &project) {
            const auto cell = project.schematicCells.find(cellId);
            if (cell == project.schematicCells.end())
                return;
            cell->second.parameters.push_back({project.nextId(), name, ""});
        });
        updateSelectionStatus();
    }

    void removeSchematicParameters(ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds) {
        if (!project_)
            return;
        project_->execute("remove schematic parameters", [&](ice::Project &project) {
            const auto cell = project.schematicCells.find(cellId);
            if (cell == project.schematicCells.end())
                return;
            auto &parameters = cell->second.parameters;
            parameters.erase(std::remove_if(parameters.begin(), parameters.end(),
                                            [&](const ice::Parameter &parameter) {
                                                return std::find(parameterIds.begin(), parameterIds.end(),
                                                                 parameter.id) != parameterIds.end();
                                            }),
                             parameters.end());
        });
        updateSelectionStatus();
    }

    void editSelectedSymbol() {
        if (!schematicCanvas_)
            return;
        openSymbolProperties(schematicCanvas_->selectedInstanceId());
    }

    // Double-clicking, or the properties toolbar button, opens the instance
    // property editor for the symbol under the cursor.
    void openSymbolProperties(ice::ObjectId instanceId) {
        if (!project_ || instanceId == 0)
            return;
        for (const auto &[_, cell] : project_->project().schematicCells) {
            const auto instance = cell.instances.find(instanceId);
            if (instance == cell.instances.end())
                continue;
            const auto symbol = cell.symbols.find(instance->second.symbol);
            const std::string symbolName = symbol == cell.symbols.end() ? std::string() : symbol->second.name;
            SymbolPropertiesDialog dialog(symbolName, instance->second, this);
            if (dialog.exec() != QDialog::Accepted)
                return;
            const auto name = dialog.instanceName();
            const auto orientation = dialog.orientation();
            const auto parameters = dialog.parameters();
            const auto x = dialog.positionX();
            const auto y = dialog.positionY();
            if (name.isEmpty()) {
                QMessageBox::information(this, QStringLiteral("Symbol Properties"),
                                         QStringLiteral("An instance name is required."));
                return;
            }
            project_->execute("edit symbol properties", [&](ice::Project &project) {
                for (auto &[_, cell] : project.schematicCells) {
                    const auto target = cell.instances.find(instanceId);
                    if (target == cell.instances.end())
                        continue;
                    target->second.name = name.toStdString();
                    target->second.x = x;
                    target->second.y = y;
                    target->second.orientation = orientation;
                    std::vector<ice::Parameter> updated;
                    for (const auto &edit : parameters) {
                        if (edit.name.trimmed().isEmpty())
                            continue;
                        const auto id = edit.id != 0 ? edit.id : project.nextId();
                        updated.push_back(ice::Parameter{id, edit.name.toStdString(), edit.value.toStdString()});
                    }
                    target->second.parameters = std::move(updated);
                    updateAutomaticNetLabelPositions(project);
                    break;
                }
            });
            refreshProject();
            return;
        }
    }

    // ── Simulation management ────────────────────────────────────────────────

    // Build (or rebuild) graphTabs_ from a SimulationItem's graphs vector.
    void rebuildGraphTabs(const std::vector<ice::SimulationGraph> &graphs) {
        // Preserve the current tab index so we can restore it.
        const int prevIndex = graphTabs_ ? graphTabs_->currentIndex() : 0;
        if (!graphTabs_)
            return;
        // Keep existing results by graph id so changing display settings
        // (axis limits, labels, ...) redraws the current data instead of
        // discarding it and forcing another simulation run. Results are only
        // restored when the probe list is unchanged, so stale traces are never
        // shown for a graph that now requests different signals.
        struct PreviousPlot {
            ice::SimulationGraph graph;
            ice::SimulationResult result;
        };
        std::map<ice::ObjectId, PreviousPlot> previousPlots;
        for (int i = 0; i < graphTabs_->count(); ++i) {
            const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong());
            const auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(i));
            if (graphId != 0 && plot && !plot->result().time.empty())
                previousPlots.emplace(graphId, PreviousPlot{plot->graph(), plot->result()});
        }

        // Remove all tabs except the first (which holds simulationPlot_).
        // We always keep simulationPlot_ alive; additional tabs use fresh plots.
        while (graphTabs_->count() > 0) {
            const int last = graphTabs_->count() - 1;
            QWidget *widget = graphTabs_->widget(last);
            graphTabs_->removeTab(last);
            if (widget != simulationPlot_)
                widget->deleteLater();
        }

        if (graphs.empty()) {
            // Show a single empty plot labelled "Graph 1".
            simulationPlot_->clear();
            simulationPlot_->setGraph({});
            simulationPlot_->setGridVisible(graphGridVisible_);
            graphTabs_->addTab(simulationPlot_, QStringLiteral("Graph 1"));
            return;
        }
        for (std::size_t i = 0; i < graphs.size(); ++i) {
            SimulationPlot *plot = (i == 0) ? simulationPlot_ : new SimulationPlot;
            plot->clear();
            plot->setGraph(graphs[i]);
            plot->setGridVisible(graphGridVisible_);
            if (const auto previous = previousPlots.find(graphs[i].id);
                previous != previousPlots.end() && previous->second.graph.nodes == graphs[i].nodes)
                plot->setResult(previous->second.result);
            const auto tabName = QString::fromStdString(graphs[i].name);
            const auto idx = graphTabs_->addTab(plot, tabName);
            graphTabs_->tabBar()->setTabData(idx, QVariant::fromValue<qulonglong>(graphs[i].id));
        }
        const int restored = (prevIndex >= 0 && prevIndex < graphTabs_->count()) ? prevIndex : 0;
        graphTabs_->setCurrentIndex(restored);
    }

    // Called when the SimulationPanel selection changes.
    void loadSimulationById(ice::ObjectId id, ice::ObjectId graphId = 0) {
        const bool sameSimulation = (id == activeSimulationId_);
        activeSimulationId_ = id;
        if (!project_ || id == 0) {
            rebuildGraphTabs({});
            refreshSimulationNetlist();
            return;
        }
        const auto &sims = project_->project().simulations;
        auto it = sims.find(id);
        if (it == sims.end()) {
            rebuildGraphTabs({});
            refreshSimulationNetlist();
            return;
        }
        const auto &sim = it->second;

        // Block signals so we don't re-save while populating.
        QSignalBlocker b1(schematicSelector_), b2(simulatorSelector_), b3(analysisSelector_), b4(stopTime_),
            b5(stepTime_), b6(directiveEditor_);

        const auto schIdx = schematicSelector_->findData(QVariant::fromValue<qulonglong>(sim.schematic));
        if (schIdx >= 0)
            schematicSelector_->setCurrentIndex(schIdx);
        simulatorSelector_->setCurrentIndex(simulatorSelector_->findData(QString::fromStdString(sim.simulator)));
        analysisSelector_->setCurrentIndex(analysisSelector_->findData(QString::fromStdString(sim.analysis)));
        stopTime_->setValue(sim.stop);
        stepTime_->setValue(sim.step);
        directiveEditor_->setPlainText(QString::fromStdString(sim.directives));

        // Avoid rebuilding the graph tabs (and losing plot results) when the
        // selection did not actually change, e.g. after a property commit.
        if (!sameSimulation)
            rebuildGraphTabs(sim.graphs);
        if (graphId != 0)
            selectGraphTab(graphId);
        refreshSimulationNetlist();
    }

    // Switch the graph tab widget to the tab holding the given graph id.
    void selectGraphTab(ice::ObjectId graphId) {
        if (!graphTabs_ || graphId == 0)
            return;
        for (int i = 0; i < graphTabs_->count(); ++i) {
            if (static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong()) == graphId) {
                graphTabs_->setCurrentIndex(i);
                return;
            }
        }
    }

    // Write form fields back into the active SimulationItem.
    void commitSimulationProperties() {
        if (!project_ || activeSimulationId_ == 0)
            return;
        project_->execute("edit simulation properties", [this](ice::Project &project) {
            auto it = project.simulations.find(activeSimulationId_);
            if (it == project.simulations.end())
                return;
            auto &sim = it->second;
            sim.schematic = static_cast<ice::ObjectId>(schematicSelector_->currentData().toULongLong());
            sim.simulator = simulatorSelector_->currentData().toString().toStdString();
            sim.analysis = analysisSelector_->currentData().toString().toStdString();
            sim.stop = stopTime_->value();
            sim.step = stepTime_->value();
            sim.directives = directiveEditor_->toPlainText().toStdString();
        });
        refreshSimulationNetlist();
    }

    // Add a new SimulationItem to the project.
    void addSimulation() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Add Simulation"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("Add Simulation"), QStringLiteral("Simulation name:"),
                                  QLineEdit::Normal, QStringLiteral("sim1"), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        ice::ObjectId newId = 0;
        project_->execute("add simulation", [&](ice::Project &project) {
            newId = project.nextId();
            ice::SimulationItem sim;
            sim.id = newId;
            sim.name = name.trimmed().toStdString();
            // Default to first schematic if available.
            if (!project.schematicCells.empty())
                sim.schematic = project.schematicCells.begin()->first;
            // Add one default graph. nextId() still returns newId here because
            // the simulation is not in the map yet, so allocate newId + 1.
            ice::SimulationGraph graph;
            graph.id = newId + 1;
            graph.name = "Graph 1";
            sim.graphs.push_back(std::move(graph));
            project.simulations.emplace(newId, std::move(sim));
        });
        simulationPanel_->setProject(project_->project());
        simulationPanel_->selectSimulation(newId);
        statusBar()->showMessage(QStringLiteral("Simulation added"));
    }

    // Delete the currently selected SimulationItem.
    void deleteSimulation() {
        if (!project_ || activeSimulationId_ == 0)
            return;
        const auto id = activeSimulationId_;
        const auto res = QMessageBox::question(this, QStringLiteral("Delete Simulation"),
                                               QStringLiteral("Delete this simulation and all its graphs?"),
                                               QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes)
            return;
        project_->execute("delete simulation", [id](ice::Project &project) { project.simulations.erase(id); });
        activeSimulationId_ = 0;
        simulationPanel_->setProject(project_->project());
        rebuildGraphTabs({});
        refreshSimulationNetlist();
        statusBar()->showMessage(QStringLiteral("Simulation deleted"));
    }

    // Add a new graph to the given SimulationItem.
    void addGraph(ice::ObjectId simId) {
        if (!project_ || simId == 0)
            return;
        bool ok = false;
        std::size_t graphCount = 0;
        if (const auto it = project_->project().simulations.find(simId); it != project_->project().simulations.end())
            graphCount = it->second.graphs.size();
        const auto name = QInputDialog::getText(this, QStringLiteral("Add Graph"), QStringLiteral("Graph name:"),
                                                QLineEdit::Normal, QStringLiteral("Graph %1").arg(graphCount + 1), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        ice::ObjectId newGraphId = 0;
        project_->execute("add graph", [&](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            ice::SimulationGraph graph;
            graph.id = project.nextId();
            newGraphId = graph.id;
            graph.name = name.trimmed().toStdString();
            it->second.graphs.push_back(std::move(graph));
        });
        if (newGraphId == 0)
            return;
        // Refresh the panel tree, select the simulation and the new graph tab.
        simulationPanel_->setProject(project_->project());
        activeSimulationId_ = 0; // force rebuild on next load
        loadSimulationById(simId, newGraphId);
        statusBar()->showMessage(QStringLiteral("Graph added"));
    }

    // Delete the given graph from the given SimulationItem.
    void deleteGraph(ice::ObjectId simId, ice::ObjectId graphId) {
        if (!project_ || simId == 0 || graphId == 0)
            return;
        const auto simIt = project_->project().simulations.find(simId);
        if (simIt == project_->project().simulations.end())
            return;
        // Don't delete the last graph.
        if (simIt->second.graphs.size() <= 1) {
            QMessageBox::information(this, QStringLiteral("Delete Graph"),
                                     QStringLiteral("A simulation must have at least one graph."));
            return;
        }
        const auto res =
            QMessageBox::question(this, QStringLiteral("Delete Graph"), QStringLiteral("Delete this graph?"),
                                  QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes)
            return;
        project_->execute("delete graph", [simId, graphId](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            auto &graphs = it->second.graphs;
            graphs.erase(std::remove_if(graphs.begin(), graphs.end(),
                                        [graphId](const ice::SimulationGraph &g) { return g.id == graphId; }),
                         graphs.end());
        });
        // Rebuild tabs from the surviving graphs and refresh the panel tree.
        const auto &updatedSim = project_->project().simulations.at(simId);
        rebuildGraphTabs(updatedSim.graphs);
        simulationPanel_->setProject(project_->project());
        statusBar()->showMessage(QStringLiteral("Graph deleted"));
    }

    // Delete the currently visible graph tab.
    void deleteCurrentGraph() {
        if (!project_ || activeSimulationId_ == 0 || !graphTabs_)
            return;
        const int tabIdx = graphTabs_->currentIndex();
        if (tabIdx < 0)
            return;
        const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(tabIdx).toULongLong());
        deleteGraph(activeSimulationId_, graphId);
    }

    // Edit the current graph's name, axis labels, limits, and node list.
    void editGraph() {
        if (!project_ || activeSimulationId_ == 0 || !graphTabs_)
            return;
        const int tabIdx = graphTabs_->currentIndex();
        if (tabIdx < 0)
            return;
        const auto graphId = static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(tabIdx).toULongLong());
        const auto simId = activeSimulationId_;
        const auto &sim = project_->project().simulations.at(simId);
        const auto git = std::find_if(sim.graphs.begin(), sim.graphs.end(),
                                      [graphId](const ice::SimulationGraph &g) { return g.id == graphId; });
        if (git == sim.graphs.end())
            return;
        const auto &currentGraph = *git;

        // Build a small dialog for editing name/axes/limits/nodes.
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Edit Graph"));
        dlg.resize(460, 340);
        auto *dlgLayout = new QVBoxLayout(&dlg);
        auto *form = new QFormLayout;
        auto *nameEdit = new QLineEdit(QString::fromStdString(currentGraph.name));
        auto *xAxisEdit = new QLineEdit(QString::fromStdString(currentGraph.xAxis));
        auto *yAxisEdit = new QLineEdit(QString::fromStdString(currentGraph.yAxis));
        auto *nodesEdit = new QLineEdit(QString::fromStdString([&] {
            std::string joined;
            for (std::size_t i = 0; i < currentGraph.nodes.size(); ++i) {
                if (i)
                    joined += ", ";
                joined += currentGraph.nodes[i];
            }
            return joined;
        }()));
        nodesEdit->setPlaceholderText(QStringLiteral("e.g. v(out), v(in)  (comma-separated)"));
        const auto makeLimitEdit = [](const std::optional<double> &limit) {
            auto *edit = new QLineEdit;
            edit->setPlaceholderText(QStringLiteral("auto"));
            if (limit)
                edit->setText(QString::number(*limit, 'g', 12));
            return edit;
        };
        auto *xMinEdit = makeLimitEdit(currentGraph.xMin);
        auto *xMaxEdit = makeLimitEdit(currentGraph.xMax);
        auto *yMinEdit = makeLimitEdit(currentGraph.yMin);
        auto *yMaxEdit = makeLimitEdit(currentGraph.yMax);
        const auto makeLimitRow = [](QLineEdit *minimum, QLineEdit *maximum) {
            auto *row = new QWidget;
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(minimum);
            rowLayout->addWidget(new QLabel(QStringLiteral("to")));
            rowLayout->addWidget(maximum);
            return row;
        };
        form->addRow(QStringLiteral("Graph name:"), nameEdit);
        form->addRow(QStringLiteral("X axis label:"), xAxisEdit);
        form->addRow(QStringLiteral("Y axis label:"), yAxisEdit);
        form->addRow(QStringLiteral("X limits:"), makeLimitRow(xMinEdit, xMaxEdit));
        form->addRow(QStringLiteral("Y limits:"), makeLimitRow(yMinEdit, yMaxEdit));
        form->addRow(QStringLiteral("Nodes / signals:"), nodesEdit);
        dlgLayout->addLayout(form);
        auto *limitsHint = new QLabel(
            QStringLiteral("Leave a limit blank to auto-scale that axis. Limits only change how the graph is drawn; "
                           "they do not affect simulation parameters or data acquisition."));
        limitsHint->setWordWrap(true);
        dlgLayout->addWidget(limitsHint);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        dlgLayout->addWidget(buttons);

        std::optional<double> newXMin;
        std::optional<double> newXMax;
        std::optional<double> newYMin;
        std::optional<double> newYMax;
        const auto parseLimit = [](const QLineEdit *edit, std::optional<double> &limit) {
            limit.reset();
            const auto text = edit->text().trimmed();
            if (text.isEmpty())
                return true;
            bool ok = false;
            const auto value = text.toDouble(&ok);
            if (!ok || !std::isfinite(value))
                return false;
            limit = value;
            return true;
        };
        connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
            if (!parseLimit(xMinEdit, newXMin) || !parseLimit(xMaxEdit, newXMax) || !parseLimit(yMinEdit, newYMin) ||
                !parseLimit(yMaxEdit, newYMax)) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("Enter numeric axis limits or leave a field blank for automatic "
                                                    "scaling."));
                return;
            }
            if (newXMin && newXMax && *newXMax <= *newXMin) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("The X maximum must be greater than the X minimum."));
                return;
            }
            if (newYMin && newYMax && *newYMax <= *newYMin) {
                QMessageBox::warning(&dlg, QStringLiteral("Axis limits"),
                                     QStringLiteral("The Y maximum must be greater than the Y minimum."));
                return;
            }
            dlg.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted)
            return;

        const auto newName = nameEdit->text().trimmed();
        const auto newXAxis = xAxisEdit->text().trimmed();
        const auto newYAxis = yAxisEdit->text().trimmed();
        // Parse nodes: split by commas, trim each entry.
        std::vector<std::string> newNodes;
        for (const auto &part : nodesEdit->text().split(',', Qt::SkipEmptyParts))
            if (const auto trimmed = part.trimmed(); !trimmed.isEmpty())
                newNodes.push_back(trimmed.toStdString());

        project_->execute("edit graph", [simId, graphId, newName, newXAxis, newYAxis, newNodes, newXMin, newXMax,
                                         newYMin, newYMax](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            for (auto &g : it->second.graphs) {
                if (g.id != graphId)
                    continue;
                g.name = newName.toStdString();
                g.xAxis = newXAxis.toStdString();
                g.yAxis = newYAxis.toStdString();
                g.nodes = newNodes;
                g.xMin = newXMin;
                g.xMax = newXMax;
                g.yMin = newYMin;
                g.yMax = newYMax;
                break;
            }
        });
        // Rebuild tabs to reflect new name and refresh the panel tree.
        const auto &updatedSim = project_->project().simulations.at(simId);
        rebuildGraphTabs(updatedSim.graphs);
        simulationPanel_->setProject(project_->project());
        // Restore to the same graph (by id).
        for (int i = 0; i < graphTabs_->count(); ++i) {
            if (static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(i).toULongLong()) == graphId) {
                graphTabs_->setCurrentIndex(i);
                break;
            }
        }
        statusBar()->showMessage(QStringLiteral("Graph updated"));
    }

    // Refresh the active graph's plot (called when switching tabs).
    void refreshGraphPlot() {
        // Result display is driven by runSimulation; here just ensure the plot widget is visible.
        if (graphTabs_ && graphTabs_->currentWidget())
            graphTabs_->currentWidget()->update();
    }

    ice::SimulationRequest buildSimulationRequest(const ice::SimulationItem &sim, QStringList &netlistNotes) const {
        ice::SimulationRequest request;
        request.analysis = sim.analysis;
        request.simulator = sim.simulator;
        request.stop = sim.stop;
        request.step = sim.step;
        // The circuit comes from the schematic the simulation points at, so
        // Run simulates what you drew instead of hand-typed directives. The
        // directives box is reserved for model cards / .include lines.
        const auto &project = project_->project();
        const auto cellId = sim.schematic != 0 && project.schematicCells.contains(sim.schematic)
                                ? sim.schematic
                                : (project.schematicCells.empty() ? 0 : project.schematicCells.begin()->first);
        if (cellId != 0) {
            std::vector<std::string> notes;
            const auto lines = ice::buildSchematicNetlist(project, cellId, &notes);
            request.netlist.insert(request.netlist.end(), lines.begin(), lines.end());
            for (const auto &noteText : notes)
                netlistNotes << QString::fromStdString(noteText);
        }
        for (const auto &line : directiveEditor_->toPlainText().split('\n')) {
            const auto trimmed = line.trimmed();
            // Skip analysis/control directives the runner already handles from
            // the simulation properties; keep model cards and includes.
            const bool isControlDirective =
                trimmed.startsWith('.') && (trimmed.startsWith(QStringLiteral(".tran"), Qt::CaseInsensitive) ||
                                            trimmed.startsWith(QStringLiteral(".dc"), Qt::CaseInsensitive) ||
                                            trimmed.startsWith(QStringLiteral(".ac"), Qt::CaseInsensitive) ||
                                            trimmed.startsWith(QStringLiteral(".op"), Qt::CaseInsensitive) ||
                                            trimmed.startsWith(QStringLiteral(".end"), Qt::CaseInsensitive));
            if (trimmed.isEmpty() || isControlDirective)
                continue;
            request.netlist.push_back(trimmed.toStdString());
        }
        // The active graph's Nodes/signals tell the adapter which vectors to
        // fetch back from ngspice, so editing a graph's Nodes changes what the
        // run returns and plots.
        ice::ObjectId activeGraphId = 0;
        if (graphTabs_ && graphTabs_->currentIndex() >= 0)
            activeGraphId =
                static_cast<ice::ObjectId>(graphTabs_->tabBar()->tabData(graphTabs_->currentIndex()).toULongLong());
        const auto graphIt =
            std::find_if(sim.graphs.begin(), sim.graphs.end(),
                         [activeGraphId](const ice::SimulationGraph &graph) { return graph.id == activeGraphId; });
        if (graphIt != sim.graphs.end())
            request.probeSignals = graphIt->nodes;

        // Parse .tran from directives for convenience.
        const auto directive = directiveEditor_->toPlainText().trimmed();
        const auto tokens = directive.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!tokens.isEmpty() && tokens.front().compare(QStringLiteral(".tran"), Qt::CaseInsensitive) == 0 &&
            tokens.size() >= 3) {
            bool stopOk = false, stepOk = false;
            const auto stop = tokens.at(1).toDouble(&stopOk);
            const auto step = tokens.at(2).toDouble(&stepOk);
            if (stopOk && stepOk && stop > 0.0 && step > 0.0) {
                request.analysis = "transient";
                request.stop = stop;
                request.step = step;
            }
        }
        return request;
    }

    void refreshSimulationNetlist() {
        if (!simulationNetlist_)
            return;
        if (!project_ || activeSimulationId_ == 0) {
            simulationNetlist_->clear();
            return;
        }
        const auto simulation = project_->project().simulations.find(activeSimulationId_);
        if (simulation == project_->project().simulations.end()) {
            simulationNetlist_->clear();
            return;
        }
        QStringList notes;
        try {
            const auto request = buildSimulationRequest(simulation->second, notes);
            QString text;
            for (const auto &line : request.netlist)
                text += QString::fromStdString(line) + QLatin1Char('\n');
            if (!notes.isEmpty()) {
                if (!text.isEmpty())
                    text += QLatin1Char('\n');
                text += QStringLiteral("* Notes\n");
                for (const auto &note : notes)
                    text += QStringLiteral("* %1\n").arg(note);
            }
            simulationNetlist_->setPlainText(text.isEmpty() ? QStringLiteral("* Empty netlist") : text);
        } catch (const ice::ProjectError &error) {
            simulationNetlist_->setPlainText(QString::fromStdString(error.what()));
        }
    }

    static QString formatSimulationReport(const ice::SimulationRequest &request, const ice::SimulationResult &result,
                                          const QStringList &netlistNotes) {
        std::ostringstream output;
        output << "State: " << ice::jobStateName(result.state) << "\n"
               << "Analysis: " << result.analysis << "\n"
               << "Simulator: " << result.simulator << " " << result.simulator_version << "\n";
        if (!request.netlist.empty()) {
            output << "Netlist:\n";
            for (const auto &line : request.netlist)
                output << "  " << line << "\n";
        }
        for (const auto &note : netlistNotes)
            output << "Note: " << note.toStdString() << "\n";
        if (!result.error.empty())
            output << "Error: " << result.error << "\n";
        if (!result.time.empty() && !result.traces.empty()) {
            output << "Samples: " << result.time.size() << "\n"
                   << "Trace: " << result.traces.front().name << "\n"
                   << "First sample: " << result.traces.front().values.front() << "\n"
                   << "Last sample: " << result.traces.front().values.back() << "\n";
        }
        return QString::fromStdString(output.str());
    }

    void runSimulation() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        // If no simulation is selected, offer to create one.
        if (activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select or add a simulation first."));
            return;
        }
        simulationOutput_->setPlainText(QStringLiteral("Running simulation..."));
        documents_->setCurrentWidget(simulationPage_);
        QApplication::processEvents();

        const auto &sim = project_->project().simulations.at(activeSimulationId_);
        QStringList netlistNotes;
        const auto request = buildSimulationRequest(sim, netlistNotes);
        const auto result =
            ice::SimulationRunner(std::make_shared<ice::NgspiceSimulator>()).run(request, std::chrono::seconds(10));
        simulationOutput_->setPlainText(formatSimulationReport(request, result, netlistNotes));

        // Show result in the currently active graph tab.
        if (graphTabs_) {
            auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->currentWidget());
            if (plot)
                plot->setResult(result);
        }
        statusBar()->showMessage(QStringLiteral("Simulation finished"));
    }

    void refreshProject() {
        if (!project_)
            return;
        refreshEmbeddedPanels();
        refreshSimulationSchematics();
        refreshSchematic();
        refreshLayout();
        refreshSimulationNetlist();
    }

    void refreshEmbeddedPanels() {
        const ice::Project empty;
        const auto &project = project_ ? project_->project() : empty;
        if (schematicHierarchy_)
            schematicHierarchy_->setProject(project);
        if (layoutHierarchy_)
            layoutHierarchy_->setProject(project);
        if (layersPanel_)
            layersPanel_->setProject(project);
        if (schematicCellTabs_) {
            const auto selected = activeSchematicId_;
            schematicCellTabs_->clear();
            for (const auto &[id, cell] : project.schematicCells) {
                const int tab = schematicCellTabs_->count();
                schematicCellTabs_->addTab(new QWidget, QString::fromStdString(cell.name));
                schematicCellTabs_->tabBar()->setTabData(tab, QVariant::fromValue<qulonglong>(id));
                const auto pdk = cell.pdk.empty() || cell.pdk == ice::genericPdkId ? QStringLiteral("Generic PDK")
                                                                                   : QString::fromStdString(cell.pdk);
                schematicCellTabs_->setTabToolTip(tab, QStringLiteral("PDK: %1").arg(pdk));
            }
            for (int index = 0; index < schematicCellTabs_->count(); ++index)
                if (schematicCellTabs_->tabBar()->tabData(index).toULongLong() == selected)
                    schematicCellTabs_->setCurrentIndex(index);
        }
        if (layoutCellTabs_) {
            const auto selected = activeLayoutId_;
            layoutCellTabs_->clear();
            for (const auto &[id, cell] : project.layoutCells) {
                layoutCellTabs_->addTab(new QWidget, QString::fromStdString(cell.name));
                layoutCellTabs_->tabBar()->setTabData(layoutCellTabs_->count() - 1,
                                                      QVariant::fromValue<qulonglong>(id));
            }
            for (int index = 0; index < layoutCellTabs_->count(); ++index)
                if (layoutCellTabs_->tabBar()->tabData(index).toULongLong() == selected)
                    layoutCellTabs_->setCurrentIndex(index);
        }
        // Refresh simulation panel.
        if (simulationPanel_)
            simulationPanel_->setProject(project);
        updateEmptyWorkspaceState();
    }

    void refreshSimulationSchematics() {
        if (!schematicSelector_)
            return;
        schematicSelector_->clear();
        if (!project_)
            return;
        for (const auto &[id, cell] : project_->project().schematicCells)
            schematicSelector_->addItem(QString::fromStdString(cell.name), QVariant::fromValue<qulonglong>(id));
    }

    static std::optional<QPointF> pinScenePosition(const ice::SchematicCell &cell, ice::ObjectId pinId) {
        for (const auto &[_, instance] : cell.instances) {
            const auto symbol = cell.symbols.find(instance.symbol);
            if (symbol == cell.symbols.end())
                continue;
            for (const auto &pin : symbol->second.pins) {
                if (pin.id != pinId)
                    continue;
                const auto local =
                    rotatePoint({static_cast<qreal>(pin.x), -static_cast<qreal>(pin.y)}, instance.orientation);
                return QPointF(instance.x + local.x(), -instance.y + local.y());
            }
        }
        return std::nullopt;
    }

    static void updateAutomaticNetLabelPositions(ice::Project &project) {
        for (auto &[_, cell] : project.schematicCells) {
            for (auto &[_, net] : cell.nets) {
                if (net.pins.empty())
                    continue;
                std::vector<QPointF> positions;
                for (const auto pinId : net.pins)
                    if (const auto position = pinScenePosition(cell, pinId))
                        positions.push_back(*position);
                if (positions.empty())
                    continue;
                QPointF center;
                for (const auto position : positions)
                    center += position;
                center /= static_cast<qreal>(positions.size());
                net.x = static_cast<ice::Coordinate>(center.x());
                net.y = static_cast<ice::Coordinate>(-center.y() + 18.0);
            }
        }
    }

    void refreshSchematic() {
        if (!schematicCanvas_)
            return;
        auto *scene = schematicCanvas_->scene();
        scene->clear();
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->clear();
        if (!project_ || project_->project().schematicCells.empty()) {
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clear();
            schematicSymbolsLoaded_ = false;
            schematicSymbolsPdk_.clear();
            return;
        }
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            cellIt = project_->project().schematicCells.begin();
        if (cellIt == project_->project().schematicCells.end())
            return;
        const auto &cell = cellIt->second;
        const auto pdk = cell.pdk.empty() ? std::string(ice::genericPdkId) : cell.pdk;
        if (schematicSymbolsPanel_) {
            if (!schematicSymbolsLoaded_ || schematicSymbolsPdk_ != pdk) {
                schematicSymbolsLoaded_ = true;
                schematicSymbolsPdk_ = pdk;
                schematicBaseSymbols_ = symbolLibraryForPdk(pdk);
            }
            auto symbols = schematicBaseSymbols_;
            for (const auto &[id, other] : project_->project().schematicCells) {
                if (id == cell.id)
                    continue;
                symbols.push_back(generatedSchematicSymbol(other));
            }
            schematicSymbolsPanel_->setSymbols(std::move(symbols));
        }
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->setCell(project_->project(), &cell);
        const auto pdkLabel = cell.pdk.empty() || cell.pdk == ice::genericPdkId ? QStringLiteral("Generic PDK")
                                                                                : QString::fromStdString(cell.pdk);
        auto *cellTitle =
            scene->addText(QStringLiteral("Schematic: %1  ·  %2").arg(QString::fromStdString(cell.name), pdkLabel));
        cellTitle->setDefaultTextColor(ice::uiColor(ice::UiColor::Axis1));
        cellTitle->setPos(-900, -900);
        cellTitle->setData(0, QStringLiteral("canvas-title"));
        for (const auto &[netId, net] : cell.nets) {
            if (net.name.empty())
                continue;
            auto *label = scene->addText(QString::fromStdString(net.name));
            label->setDefaultTextColor(ice::uiColor(ice::UiColor::Accent5));
            label->setData(0, QStringLiteral("net-label"));
            label->setData(1, QVariant::fromValue<qulonglong>(netId));
            label->setPos(net.x, -net.y);
        }
        for (const auto &[instanceId, instance] : cell.instances) {
            auto symbolIt = cell.symbols.find(instance.symbol);
            if (symbolIt == cell.symbols.end())
                continue;
            auto *group = new QGraphicsItemGroup;
            scene->addItem(group);
            group->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
            group->setData(0, QStringLiteral("symbol"));
            group->setData(1, QVariant::fromValue<qulonglong>(instanceId));
            group->setPos(instance.x, -instance.y);
            switch (instance.orientation) {
            case ice::Orientation::r90:
                group->setRotation(90);
                break;
            case ice::Orientation::r180:
                group->setRotation(180);
                break;
            case ice::Orientation::r270:
                group->setRotation(270);
                break;
            default:
                group->setRotation(0);
                break;
            }
            group->setToolTip(QString::fromStdString(instance.name));
            const auto artworkText =
                symbolIt->second.artwork.empty()
                    ? std::string("<svg viewBox='0 0 120 60'><rect x='2' y='2' width='116' height='56' fill='none' "
                                  "stroke='black' stroke-width='4'/></svg>")
                    : symbolIt->second.artwork;
            auto *renderer = new QSvgRenderer(QByteArray::fromStdString(artworkText), scene);
            auto *artwork = new QGraphicsSvgItem;
            artwork->setSharedRenderer(renderer);
            artwork->setParentItem(group);
            // Center artwork of any size on the symbol origin; pins are
            // defined relative to the artwork center (see symbols/*.toml).
            const auto artworkSize = renderer->defaultSize();
            artwork->setPos(-artworkSize.width() / 2.0, -artworkSize.height() / 2.0);
            artwork->setFlag(QGraphicsItem::ItemIsSelectable, false);
            artwork->setData(0, QStringLiteral("symbol-artwork"));
            auto *tint = new QGraphicsColorizeEffect(artwork);
            tint->setColor(ice::uiColor(ice::UiColor::Axis1));
            tint->setStrength(1.0);
            tint->setEnabled(true);
            artwork->setGraphicsEffect(tint);
            artwork->setData(3, QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(tint)));
            auto *label = new QGraphicsTextItem(QString::fromStdString(instance.name), group);
            label->setDefaultTextColor(ice::uiColor(ice::UiColor::Axis1));
            label->setPos(-artworkSize.width() / 2.0, artworkSize.height() / 2.0 + 6);
            label->setData(0, QStringLiteral("symbol-label"));
            for (const auto &pin : symbolIt->second.pins) {
                auto *pinItem = new QGraphicsEllipseItem(QRectF(-5, -5, 10, 10), group);
                pinItem->setPen(QPen(ice::uiColor(ice::UiColor::Axis1)));
                pinItem->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent1)));
                pinItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
                pinItem->setData(0, QStringLiteral("pin"));
                pinItem->setData(1, QVariant::fromValue<qulonglong>(pin.id));
                pinItem->setData(2, static_cast<int>(rotateDirection(pin.direction, instance.orientation)));
                pinItem->setPos(pin.x, -pin.y);
                pinItem->setToolTip(QString::fromStdString(pin.name));
                pinItem->setVisible(false);
            }
            // Selection feedback is drawn as a child of the symbol group so it
            // follows the symbol and its transform.
            if (const auto bounds = symbolHighlightRect(group); !bounds.isNull()) {
                auto *highlight = new QGraphicsRectItem(bounds, group);
                highlight->setPen(QPen(ice::uiColor(ice::UiColor::Accent4), 2));
                QColor fill(ice::uiColor(ice::UiColor::Accent4));
                fill.setAlpha(45);
                highlight->setBrush(fill);
                highlight->setZValue(-1);
                highlight->setData(0, QStringLiteral("symbol-highlight"));
                highlight->setVisible(false);
            }
        }
        struct PinView {
            QPointF point;
            ice::PinDirection direction;
        };
        std::map<ice::ObjectId, PinView> pinViews;
        for (auto *item : scene->items()) {
            if (item->data(0).toString() != QStringLiteral("pin"))
                continue;
            pinViews[item->data(1).toULongLong()] = {item->scenePos(),
                                                     static_cast<ice::PinDirection>(item->data(2).toInt())};
        }
        for (const auto &[netId, net] : cell.nets) {
            if (net.pins.size() < 2)
                continue;
            const auto first = pinViews.find(net.pins.front());
            const auto second = pinViews.find(net.pins.back());
            if (first != pinViews.end() && second != pinViews.end()) {
                auto path = routeWire(first->second.point, first->second.direction, second->second.point,
                                      second->second.direction, scene, net.pins.front(), net.pins.back());
                auto *wire = scene->addPath(path, QPen(ice::uiColor(ice::UiColor::Accent4), 2));
                wire->setZValue(-1);
                wire->setData(0, QStringLiteral("wire"));
            }
        }
        // Selection is view state over the domain model, so re-apply it after
        // the scene has been rebuilt from the database.
        schematicCanvas_->restoreSelection();
        updateSelectionStatus();
        schematicCanvas_->refreshPinVisibility();
    }

    QPainterPath routeWire(QPointF start, ice::PinDirection startDirection, QPointF target,
                           ice::PinDirection targetDirection, QGraphicsScene *scene, ice::ObjectId startPin,
                           ice::ObjectId targetPin) const {
        constexpr qreal minimum = 30.0;
        const auto startVector = directionVector(startDirection);
        const auto targetVector = directionVector(targetDirection);
        const QPointF startExit = start + startVector * minimum;
        // The target entry point must be outside the symbol. The final route
        // segment then travels opposite the pin's outward-facing direction.
        const QPointF targetEntry = target + targetVector * minimum;
        const auto key = [](int node, int direction) { return std::pair<int, int>{node, direction}; };
        std::vector<QRectF> obstacles;
        for (auto *item : scene->items()) {
            if (item->data(0).toString() == QStringLiteral("symbol") && item->data(1).toULongLong() != startPin &&
                item->data(1).toULongLong() != targetPin)
                obstacles.push_back(item->sceneBoundingRect().adjusted(-12, -12, 12, 12));
        }
        const auto blocked = [&](QPointF point) {
            return std::any_of(obstacles.begin(), obstacles.end(),
                               [&](const QRectF &obstacle) { return obstacle.contains(point); });
        };
        const auto directionIndex = [](QPointF vector) {
            if (vector.x() < 0)
                return 0;
            if (vector.x() > 0)
                return 1;
            if (vector.y() < 0)
                return 2;
            return 3;
        };
        std::vector<qreal> xCoordinates{startExit.x(), targetEntry.x(), start.x(), target.x()};
        std::vector<qreal> yCoordinates{startExit.y(), targetEntry.y(), start.y(), target.y()};
        for (const auto &obstacle : obstacles) {
            xCoordinates.push_back(obstacle.left());
            xCoordinates.push_back(obstacle.right());
            yCoordinates.push_back(obstacle.top());
            yCoordinates.push_back(obstacle.bottom());
        }
        const auto uniqueCoordinates = [](std::vector<qreal> &coordinates) {
            std::sort(coordinates.begin(), coordinates.end());
            coordinates.erase(
                std::unique(coordinates.begin(), coordinates.end(),
                            [](qreal left, qreal right) { return qFuzzyCompare(left + 1.0, right + 1.0); }),
                coordinates.end());
        };
        uniqueCoordinates(xCoordinates);
        uniqueCoordinates(yCoordinates);
        std::vector<QPointF> nodes;
        for (qreal x : xCoordinates)
            for (qreal y : yCoordinates)
                if (!blocked({x, y}))
                    nodes.emplace_back(x, y);
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
            const int samples = std::max(
                2,
                static_cast<int>(std::ceil(std::max(std::abs(to.x() - from.x()), std::abs(to.y() - from.y())) / 10.0)));
            for (int index = 1; index < samples; ++index) {
                const qreal fraction = static_cast<qreal>(index) / samples;
                if (blocked(from + (to - from) * fraction))
                    return false;
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
            bool operator>(const QueueNode &other) const {
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
            if (points[index] != points[index - 1])
                path.lineTo(points[index]);
        return path;
    }

    QAction *addCommand(const QString &text, const QKeySequence &shortcut = {}) {
        auto *action = new QAction(text, this);
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        commands_.append(action);
        return action;
    }

    void setAppearanceMode(ice::AppearanceMode mode) {
        appearanceMode_ = mode;
        ice::saveAppearanceMode(mode);
        ice::applyAppearance(mode);
        refreshAppearance();
        statusBar()->showMessage(mode == ice::AppearanceMode::System
                                     ? QStringLiteral("Following the system appearance")
                                     : (mode == ice::AppearanceMode::Dark
                                            ? QStringLiteral("Dark appearance applied")
                                            : QStringLiteral("Light appearance applied")));
    }

    void refreshAppearance() {
        const auto canvas = ice::uiColor(ice::UiColor::Background2);
        if (schematicCanvas_)
            schematicCanvas_->setBackgroundBrush(canvas);
        if (layoutCanvas_)
            layoutCanvas_->setBackgroundBrush(canvas);
        // Scene items cache their colors, so rebuild both editors after an
        // appearance change; the canvas transform and selection are preserved.
        refreshSchematic();
        refreshLayout();
        if (layersPanel_ && project_)
            layersPanel_->setProject(project_->project());
        if (schematicPropertiesPanel_)
            schematicPropertiesPanel_->refreshAppearance();
        if (startupView_)
            startupView_->refreshAppearance();
        if (graphTabs_) {
            for (int index = 0; index < graphTabs_->count(); ++index)
                if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                    plot->update();
        }
        for (auto *action : findChildren<QAction *>()) {
            const auto name = action->property("lucideIcon").toString();
            if (!name.isEmpty())
                action->setIcon(ice::lucideIcon(name));
        }
        for (auto *button : findChildren<QAbstractButton *>()) {
            const auto name = button->property("lucideIcon").toString();
            if (!name.isEmpty())
                button->setIcon(ice::lucideIcon(name, button->iconSize().width()));
        }
        update();
    }

    void createMenus() {
        auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
        auto *newAction = addCommand(QStringLiteral("New Project"), QKeySequence::New);
        auto *openAction = addCommand(QStringLiteral("Open Project"), QKeySequence::Open);
        auto *saveAction = addCommand(QStringLiteral("Save Project"), QKeySequence::Save);
        fileMenu->addAction(newAction);
        fileMenu->addAction(openAction);
        fileMenu->addAction(saveAction);
        fileMenu->addSeparator();
        auto *openLayoutAction = addCommand(QStringLiteral("Open GDS/OASIS Layout"));
        auto *saveLayoutAction = addCommand(QStringLiteral("Save GDS/OASIS Layout"));
        fileMenu->addAction(openLayoutAction);
        fileMenu->addAction(saveLayoutAction);
        fileMenu->addSeparator();
        auto *closeAction = addCommand(QStringLiteral("Close Project"), QKeySequence::Close);
        fileMenu->addAction(closeAction);
        fileMenu->addSeparator();
        auto *exitAction = addCommand(QStringLiteral("Exit"), QKeySequence::Quit);
        fileMenu->addAction(exitAction);
        connect(exitAction, &QAction::triggered, this, [this] { close(); });

        auto *editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
        editMenu->addAction(addCommand(QStringLiteral("Undo"), QKeySequence::Undo));
        editMenu->addAction(addCommand(QStringLiteral("Redo"), QKeySequence::Redo));
        editMenu->addSeparator();
        editMenu->addAction(addCommand(QStringLiteral("Cut"), QKeySequence::Cut));
        editMenu->addAction(addCommand(QStringLiteral("Copy"), QKeySequence::Copy));
        editMenu->addAction(addCommand(QStringLiteral("Paste"), QKeySequence::Paste));
        // A shortcut is deliberately omitted here: the properties editor is a
        // modal dialog, so a window level shortcut would steal Return from the
        // line edits inside it.
        auto *propertiesAction = addCommand(QStringLiteral("Symbol Properties"));
        editMenu->addAction(propertiesAction);
        editMenu->addAction(addCommand(QStringLiteral("Delete"), QKeySequence::Delete));
        connect(propertiesAction, &QAction::triggered, this, [this] { editSelectedSymbol(); });

        auto *viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
        viewMenu->addAction(addCommand(QStringLiteral("Project / Hierarchy")));
        viewMenu->addAction(addCommand(QStringLiteral("Properties / Diagnostics")));
        viewMenu->addAction(addCommand(QStringLiteral("Activity / Agent")));
        viewMenu->addSeparator();
        auto *paletteAction = addCommand(QStringLiteral("Command Palette"), QKeySequence(QStringLiteral("Ctrl+K")));
        viewMenu->addAction(paletteAction);
        connect(paletteAction, &QAction::triggered, this, &DocumentWindow::showCommandPalette);

        auto *projectMenu = menuBar()->addMenu(QStringLiteral("&Project"));
        auto *createSchematicAction = addCommand(QStringLiteral("Create Schematic"));
        projectMenu->addAction(createSchematicAction);
        auto *createLayoutAction = addCommand(QStringLiteral("Create Layout Cell"));
        projectMenu->addAction(createLayoutAction);
        auto *createLayerAction = addCommand(QStringLiteral("Create Layer"));
        projectMenu->addAction(createLayerAction);
        auto *placeSymbolAction = addCommand(QStringLiteral("Place Symbol"), QKeySequence(QStringLiteral("P")));
        projectMenu->addAction(placeSymbolAction);
        projectMenu->addSeparator();
        projectMenu->addAction(addCommand(QStringLiteral("Validate Project")));
        projectMenu->addAction(addCommand(QStringLiteral("Project Settings")));
        auto *managePackagesAction = addCommand(QStringLiteral("Manage Packages"));
        setLucideIcon(managePackagesAction, QStringLiteral("package"));
        projectMenu->addAction(managePackagesAction);

        auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
        auto *simulationAction = addCommand(QStringLiteral("Run Simulation"), QKeySequence(QStringLiteral("F6")));
        toolsMenu->addAction(simulationAction);
        toolsMenu->addAction(addCommand(QStringLiteral("Run DRC")));
        toolsMenu->addAction(addCommand(QStringLiteral("Run LVS")));
        toolsMenu->addAction(addCommand(QStringLiteral("Run PEX")));
        toolsMenu->addAction(addCommand(QStringLiteral("Open Python Console")));

        auto *settingsMenu = menuBar()->addMenu(QStringLiteral("&Settings"));
        auto *appearanceMenu = settingsMenu->addMenu(QStringLiteral("Appearance"));
        auto *systemAppearanceAction = addCommand(QStringLiteral("System Theme"));
        auto *lightAppearanceAction = addCommand(QStringLiteral("Light Theme"));
        auto *darkAppearanceAction = addCommand(QStringLiteral("Dark Theme"));
        auto *appearanceGroup = new QActionGroup(this);
        appearanceGroup->setExclusive(true);
        for (auto *action : {systemAppearanceAction, lightAppearanceAction, darkAppearanceAction}) {
            action->setCheckable(true);
            appearanceGroup->addAction(action);
            appearanceMenu->addAction(action);
        }
        switch (appearanceMode_) {
        case ice::AppearanceMode::Light:
            lightAppearanceAction->setChecked(true);
            break;
        case ice::AppearanceMode::Dark:
            darkAppearanceAction->setChecked(true);
            break;
        case ice::AppearanceMode::System:
            systemAppearanceAction->setChecked(true);
            break;
        }

        auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
        helpMenu->addAction(addCommand(QStringLiteral("Keyboard Shortcuts")));
        helpMenu->addAction(addCommand(QStringLiteral("About ICE")));

        connect(newAction, &QAction::triggered, this, &DocumentWindow::newProject);
        connect(openAction, &QAction::triggered, this, &DocumentWindow::openProject);
        connect(saveAction, &QAction::triggered, this, &DocumentWindow::saveProject);
        connect(openLayoutAction, &QAction::triggered, this, &DocumentWindow::openLayoutInterchange);
        connect(saveLayoutAction, &QAction::triggered, this, &DocumentWindow::saveLayoutInterchange);
        connect(closeAction, &QAction::triggered, this, &DocumentWindow::closeProject);
        connect(createSchematicAction, &QAction::triggered, this, [this] { createSchematic(); });
        connect(createLayoutAction, &QAction::triggered, this, [this] { createLayoutCell(); });
        connect(createLayerAction, &QAction::triggered, this, &DocumentWindow::createLayer);
        connect(managePackagesAction, &QAction::triggered, this, &DocumentWindow::managePackages);
        connect(placeSymbolAction, &QAction::triggered, this, &DocumentWindow::placeSymbol);
        connect(simulationAction, &QAction::triggered, this, &DocumentWindow::runSimulation);
        connect(systemAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::System); });
        connect(lightAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::Light); });
        connect(darkAppearanceAction, &QAction::triggered, this,
                [this] { setAppearanceMode(ice::AppearanceMode::Dark); });
    }

    void showCommandPalette() {
        CommandPalette palette(commands_, this);
        palette.exec();
    }

    void createProjectDock() {
        auto *dock = new QDockWidget(QStringLiteral("Project / Hierarchy"), this);
        dock->setObjectName(QStringLiteral("projectHierarchyDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        projectPanel_ = new ProjectPanel;
        dock->setWidget(projectPanel_);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
    }

    void createInspectorDock() {
        auto *dock = new QDockWidget(QStringLiteral("Properties / Diagnostics"), this);
        dock->setObjectName(QStringLiteral("inspectorDock"));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        auto *tabs = new QTabWidget;
        tabs->addTab(new PropertiesPanel, QStringLiteral("Properties"));
        tabs->addTab(new DiagnosticsPanel, QStringLiteral("Diagnostics"));
        dock->setWidget(tabs);
        addDockWidget(Qt::RightDockWidgetArea, dock);
    }

    void createActivityDock() {
        activityDock_ = new QDockWidget(this);
        activityDock_->setObjectName(QStringLiteral("activityDock"));
        activityDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        auto *titleBar = new QWidget(activityDock_);
        titleBar->setFixedHeight(0);
        activityDock_->setTitleBarWidget(titleBar);
        activityDock_->setWidget(new ActivityPanel);
        addDockWidget(Qt::BottomDockWidgetArea, activityDock_);
    }

    void showStartup() {
        if (workspaceStack_ && startupView_)
            workspaceStack_->setCurrentWidget(startupView_);
        if (activityDock_)
            activityDock_->hide();
        if (startupView_)
            startupView_->setRecentProjects(loadRecentProjects());
    }

    void showWorkspace() {
        if (workspaceStack_ && documents_)
            workspaceStack_->setCurrentWidget(documents_);
        if (activityDock_)
            activityDock_->show();
    }

    void selectSchematicTool() {
        if (schematicSelectTool_)
            schematicSelectTool_->setChecked(true);
        if (schematicCanvas_) {
            schematicCanvas_->setPlaceSymbolMode(false);
            schematicCanvas_->setNetLabelMode(false);
        }
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(false);
        statusBar()->showMessage(QStringLiteral("Select tool"));
    }

    void updateEmptyWorkspaceState() {
        const bool hasSchematics = project_ && !project_->project().schematicCells.empty();
        const bool hasLayouts = project_ && !project_->project().layoutCells.empty();
        if (schematicToolbarColumn_)
            schematicToolbarColumn_->setVisible(hasSchematics);
        if (layoutToolbarColumn_)
            layoutToolbarColumn_->setVisible(hasLayouts);

        if (schematicEditorStack_ && schematicEditorPage_ && schematicEmptyPanel_) {
            const bool showingEmpty = schematicEditorStack_->currentWidget() == schematicEmptyPanel_;
            if (hasSchematics) {
                schematicEditorStack_->setCurrentWidget(schematicEditorPage_);
            } else {
                schematicEmptyPanel_->refreshPdks(configuredPackageInstallDirectory());
                schematicEditorStack_->setCurrentWidget(schematicEmptyPanel_);
                if (!showingEmpty)
                    schematicEmptyPanel_->prepareForDisplay();
            }
        }
        if (layoutEditorStack_ && layoutEditorPage_ && layoutEmptyPanel_) {
            const bool showingEmpty = layoutEditorStack_->currentWidget() == layoutEmptyPanel_;
            layoutEditorStack_->setCurrentWidget(hasLayouts ? layoutEditorPage_ : layoutEmptyPanel_);
            if (!hasLayouts && !showingEmpty)
                layoutEmptyPanel_->prepareForDisplay();
        }
    }

    void createDocuments() {
        documents_ = new QTabWidget;
        auto *documents = documents_;
        documents->setTabsClosable(false);
        auto *schematicPage = new QWidget;
        auto *schematicLayout = new QVBoxLayout(schematicPage);
        schematicLayout->setContentsMargins(0, 0, 0, 0);
        schematicLayout->setSpacing(0);
        schematicCellTabs_ = new QTabWidget;
        schematicCellTabs_->setDocumentMode(true);
        auto *schematicContent = new QWidget;
        auto *schematicContentLayout = new QHBoxLayout(schematicContent);
        schematicHierarchy_ = new ProjectPanel(false, false, false, true);
        schematicHierarchy_->setOnAddClicked([this] { createSchematic(); });
        schematicHierarchy_->setOnRemoveClicked([this](ice::ObjectId id) { removeSchematic(id); });
        schematicSymbolsPanel_ = new SymbolsPanel;
        schematicSymbolsPanel_->setOnSelectionChanged([this] {
            if (!schematicCanvas_)
                return;
            const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
            schematicCanvas_->setPlaceSymbolPreview(symbol ? symbol->svg.toUtf8() : QByteArray());
        });
        schematicLeftTabs_ = new QTabWidget;
        schematicLeftTabs_->addTab(schematicHierarchy_, QStringLiteral("Hierarchy"));
        schematicLeftTabs_->addTab(schematicSymbolsPanel_, QStringLiteral("Symbols"));
        schematicContentLayout->addWidget(schematicLeftTabs_, 0);
        auto *schematicToolbar = createEditorToolbar(false);
        schematicToolbar->setOrientation(Qt::Vertical);
        schematicToolbarColumn_ = new QWidget;
        auto *schematicToolbarLayout = new QVBoxLayout(schematicToolbarColumn_);
        schematicToolbarLayout->setContentsMargins(0, 0, 0, 0);
        schematicToolbarLayout->addStretch(1);
        schematicToolbarLayout->addWidget(schematicToolbar, 0, Qt::AlignHCenter);
        schematicToolbarLayout->addStretch(1);
        schematicContentLayout->addWidget(schematicToolbarColumn_, 0);
        schematicEditorPage_ = new QWidget;
        auto *schematicEditorLayout = new QVBoxLayout(schematicEditorPage_);
        schematicEditorLayout->setContentsMargins(0, 0, 0, 0);
        schematicEditorLayout->setSpacing(0);
        schematicEditorLayout->addWidget(schematicCellTabs_);
        schematicEditorLayout->addWidget(makeEditor(QStringLiteral("Schematic editor"), false), 1);
        schematicEmptyPanel_ = new EmptySchematicPanel;
        schematicEmptyPanel_->setOnCreate(
            [this](const QString &name, const std::string &pdk) { createSchematic(name, pdk); });
        schematicEditorStack_ = new QStackedWidget;
        schematicEditorStack_->addWidget(schematicEmptyPanel_);
        schematicEditorStack_->addWidget(schematicEditorPage_);
        schematicContentLayout->addWidget(schematicEditorStack_, 1);
        auto *schematicProperties = new QTabWidget;
        schematicPropertiesPanel_ = new PropertiesPanel(false);
        schematicPropertiesPanel_->setOnParameterChanged(
            [this](ice::ObjectId instanceId, ice::ObjectId parameterId, const std::string &name,
                   const std::string &value) { updateInstanceParameter(instanceId, parameterId, name, value); });
        schematicPropertiesPanel_->setOnCellParameterChanged(
            [this](ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name, const std::string &value) {
                updateSchematicParameter(cellId, parameterId, name, value);
            });
        schematicPropertiesPanel_->setOnCellParameterAdded(
            [this](ice::ObjectId cellId, const std::string &name) { addSchematicParameter(cellId, name); });
        schematicPropertiesPanel_->setOnCellParametersRemoved(
            [this](ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds) {
                removeSchematicParameters(cellId, parameterIds);
            });
        schematicProperties->addTab(schematicPropertiesPanel_, QStringLiteral("Properties"));
        schematicNetlistPanel_ = new SchematicNetlistPanel;
        schematicProperties->addTab(schematicNetlistPanel_, QStringLiteral("Netlist"));
        schematicContentLayout->addWidget(schematicProperties, 0);
        schematicLayout->addWidget(schematicContent, 1);
        documents->addTab(schematicPage, QStringLiteral("Schematic"));

        auto *layoutPage = new QWidget;
        auto *layoutLayout = new QVBoxLayout(layoutPage);
        layoutLayout->setContentsMargins(0, 0, 0, 0);
        layoutLayout->setSpacing(0);
        layoutCellTabs_ = new QTabWidget;
        layoutCellTabs_->setDocumentMode(true);
        auto *layoutContent = new QWidget;
        auto *layoutContentLayout = new QHBoxLayout(layoutContent);
        layoutHierarchy_ = new ProjectPanel(true, false, true, true);
        layoutHierarchy_->setOnAddClicked([this] { createLayoutCell(); });
        layoutHierarchy_->setOnRemoveClicked([this](ice::ObjectId id) { removeLayoutCell(id); });
        layoutContentLayout->addWidget(layoutHierarchy_, 0);
        auto *layoutToolbar = createEditorToolbar(true);
        layoutToolbar->setOrientation(Qt::Vertical);
        layoutToolbarColumn_ = new QWidget;
        auto *layoutToolbarLayout = new QVBoxLayout(layoutToolbarColumn_);
        layoutToolbarLayout->setContentsMargins(0, 0, 0, 0);
        layoutToolbarLayout->addStretch(1);
        layoutToolbarLayout->addWidget(layoutToolbar, 0, Qt::AlignHCenter);
        layoutToolbarLayout->addStretch(1);
        layoutContentLayout->addWidget(layoutToolbarColumn_, 0);
        layoutEditorPage_ = new QWidget;
        auto *layoutEditorLayout = new QVBoxLayout(layoutEditorPage_);
        layoutEditorLayout->setContentsMargins(0, 0, 0, 0);
        layoutEditorLayout->setSpacing(0);
        layoutEditorLayout->addWidget(layoutCellTabs_);
        layoutEditorLayout->addWidget(makeEditor(QStringLiteral("Layout editor"), true), 1);
        layoutEmptyPanel_ = new EmptyLayoutPanel;
        layoutEmptyPanel_->setOnCreate([this](const QString &name) { createLayoutCell(name); });
        layoutEditorStack_ = new QStackedWidget;
        layoutEditorStack_->addWidget(layoutEmptyPanel_);
        layoutEditorStack_->addWidget(layoutEditorPage_);
        layoutContentLayout->addWidget(layoutEditorStack_, 1);
        layersPanel_ = new LayersPanel;
        layoutContentLayout->addWidget(layersPanel_, 0);
        layoutLayout->addWidget(layoutContent, 1);
        documents->addTab(layoutPage, QStringLiteral("Layout"));

        simulationPage_ = new QWidget;
        auto *simulationLayout = new QVBoxLayout(simulationPage_);
        simulationLayout->setContentsMargins(0, 0, 0, 0);
        simulationLayout->setSpacing(0);
        auto *simulationSplit = new QSplitter(Qt::Horizontal);

        // ── Left panel: SimulationPanel (hierarchy-like tree) ────────────
        simulationPanel_ = new SimulationPanel;
        simulationPanel_->setMinimumWidth(180);
        simulationPanel_->setMaximumWidth(280);
        simulationSplit->addWidget(simulationPanel_);

        // ── Right: simulation editor (properties, graphs, netlist tabs) ──
        auto *simEditor = new QWidget;
        auto *simEditorLayout = new QVBoxLayout(simEditor);
        simEditorLayout->setContentsMargins(4, 4, 4, 4);
        simulationTabs_ = new QTabWidget;

        // Properties form – links to the selected SimulationItem fields
        auto *propertiesPage = new QWidget;
        auto *propertiesLayout = new QVBoxLayout(propertiesPage);
        auto *simPropsGroup = new QGroupBox(QStringLiteral("Simulation Properties"));
        auto *simulationForm = new QFormLayout(simPropsGroup);
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
        directiveEditor_ = new QPlainTextEdit;
        directiveEditor_->setPlainText(QStringLiteral(".tran 1.0 0.01"));
        directiveEditor_->setPlaceholderText(QStringLiteral("SPICE directives, for example: .tran 1.0 0.01"));
        directiveEditor_->setMaximumHeight(80);
        simulationForm->addRow(QStringLiteral("Schematic"), schematicSelector_);
        simulationForm->addRow(QStringLiteral("Simulator"), simulatorSelector_);
        simulationForm->addRow(QStringLiteral("Analysis"), analysisSelector_);
        simulationForm->addRow(QStringLiteral("Stop time / end"), stopTime_);
        simulationForm->addRow(QStringLiteral("Step / increment"), stepTime_);
        simulationForm->addRow(QStringLiteral("SPICE directives"), directiveEditor_);
        propertiesLayout->addWidget(simPropsGroup);

        auto *runButton = new QPushButton;
        setLucideIcon(runButton, QStringLiteral("play"), 16);
        runButton->setToolTip(QStringLiteral("Run simulation"));
        runButton->setAccessibleName(QStringLiteral("Run simulation"));
        runButton->setFixedWidth(32);
        auto *runRow = new QHBoxLayout;
        runRow->addStretch();
        runRow->addWidget(runButton);
        propertiesLayout->addLayout(runRow);

        simulationOutput_ = new QPlainTextEdit;
        simulationOutput_->setReadOnly(true);
        simulationOutput_->setMaximumHeight(110);
        simulationOutput_->setPlaceholderText(QStringLiteral("Simulation status and result summary"));
        propertiesLayout->addWidget(simulationOutput_);
        propertiesLayout->addStretch();
        simulationTabs_->addTab(propertiesPage, QStringLiteral("Properties"));

        // Graph tabs area
        auto *graphsPage = new QWidget;
        auto *graphsLayout = new QVBoxLayout(graphsPage);
        graphsLayout->setContentsMargins(0, 0, 0, 0);
        auto *graphHeader = new QWidget;
        auto *graphHeaderLayout = new QHBoxLayout(graphHeader);
        graphHeaderLayout->setContentsMargins(0, 4, 0, 0);
        graphHeaderLayout->addWidget(heading(QStringLiteral("Graphs")));
        auto *addGraphButton_ = new QPushButton;
        addGraphButton_->setToolTip(QStringLiteral("Add graph to this simulation"));
        addGraphButton_->setAccessibleName(QStringLiteral("Add graph"));
        addGraphButton_->setFixedWidth(32);
        setLucideIcon(addGraphButton_, QStringLiteral("plus"), 16);
        auto *deleteGraphButton_ = new QPushButton;
        deleteGraphButton_->setToolTip(QStringLiteral("Delete current graph"));
        deleteGraphButton_->setAccessibleName(QStringLiteral("Delete current graph"));
        deleteGraphButton_->setFixedWidth(32);
        setLucideIcon(deleteGraphButton_, QStringLiteral("trash"), 16);
        auto *editGraphButton_ = new QPushButton;
        editGraphButton_->setToolTip(QStringLiteral("Edit graph name, axes, limits, and nodes"));
        editGraphButton_->setAccessibleName(QStringLiteral("Edit graph"));
        editGraphButton_->setFixedWidth(32);
        setLucideIcon(editGraphButton_, QStringLiteral("pencil"), 16);
        auto *gridButton = new QPushButton;
        gridButton->setCheckable(true);
        gridButton->setChecked(true);
        gridButton->setToolTip(QStringLiteral("Show or hide the graph grid"));
        gridButton->setAccessibleName(QStringLiteral("Toggle graph grid"));
        gridButton->setFixedWidth(32);
        setLucideIcon(gridButton, QStringLiteral("grid-3x3"), 16);
        graphHeaderLayout->addWidget(addGraphButton_);
        graphHeaderLayout->addWidget(deleteGraphButton_);
        graphHeaderLayout->addWidget(editGraphButton_);
        graphHeaderLayout->addWidget(gridButton);
        graphHeaderLayout->addStretch();
        graphsLayout->addWidget(graphHeader);

        graphTabs_ = new QTabWidget;
        graphTabs_->setDocumentMode(true);
        graphTabs_->setTabsClosable(false);
        simulationPlot_ = new SimulationPlot;
        graphTabs_->addTab(simulationPlot_, QStringLiteral("Graph 1"));
        graphsLayout->addWidget(graphTabs_, 1);
        simulationTabs_->addTab(graphsPage, QStringLiteral("Graphs"));

        // Netlist viewer
        auto *netlistPage = new QWidget;
        auto *netlistLayout = new QVBoxLayout(netlistPage);
        netlistLayout->setContentsMargins(0, 0, 0, 0);
        simulationNetlist_ = new QPlainTextEdit;
        simulationNetlist_->setReadOnly(true);
        simulationNetlist_->setLineWrapMode(QPlainTextEdit::NoWrap);
        simulationNetlist_->setPlaceholderText(QStringLiteral("Select a simulation to see its netlist."));
        netlistLayout->addWidget(simulationNetlist_);
        simulationTabs_->addTab(netlistPage, QStringLiteral("Netlist"));

        simEditorLayout->addWidget(simulationTabs_, 1);

        simulationSplit->addWidget(simEditor);
        simulationLayout->addWidget(simulationSplit, 1);

        // ── Wire SimulationPanel ─────────────────────────────────────────
        simulationPanel_->setOnAddClicked([this] { addSimulation(); });
        simulationPanel_->setOnDeleteClicked([this] { deleteSimulation(); });
        simulationPanel_->setOnSelection(
            [this](ice::ObjectId simId, ice::ObjectId graphId) { loadSimulationById(simId, graphId); });
        simulationPanel_->setOnAddGraph([this](ice::ObjectId simId) { addGraph(simId); });
        simulationPanel_->setOnDeleteGraph(
            [this](ice::ObjectId simId, ice::ObjectId graphId) { deleteGraph(simId, graphId); });

        // ── Wire properties form → save back to project ──────────────────
        auto saveSimProps = [this] { commitSimulationProperties(); };
        connect(schematicSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [saveSimProps] { saveSimProps(); });
        connect(simulatorSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [saveSimProps] { saveSimProps(); });
        connect(analysisSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, saveSimProps] {
            const auto analysis = analysisSelector_->currentData().toString();
            if (analysis == QStringLiteral("transient"))
                directiveEditor_->setPlainText(
                    QStringLiteral(".tran %1 %2").arg(stopTime_->value()).arg(stepTime_->value()));
            else if (analysis == QStringLiteral("dc"))
                directiveEditor_->setPlainText(QStringLiteral(".dc V1 0 1 0.01"));
            else if (analysis == QStringLiteral("ac"))
                directiveEditor_->setPlainText(QStringLiteral(".ac dec 10 1 1e6"));
            else
                directiveEditor_->setPlainText(QStringLiteral(".op"));
            saveSimProps();
        });
        connect(stopTime_, &QDoubleSpinBox::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(stepTime_, &QDoubleSpinBox::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(directiveEditor_, &QPlainTextEdit::textChanged, this, [saveSimProps] { saveSimProps(); });

        // ── Wire graph buttons ────────────────────────────────────────────
        connect(addGraphButton_, &QPushButton::clicked, this, [this] { addGraph(activeSimulationId_); });
        connect(deleteGraphButton_, &QPushButton::clicked, this, [this] { deleteCurrentGraph(); });
        connect(editGraphButton_, &QPushButton::clicked, this, [this] { editGraph(); });
        connect(gridButton, &QPushButton::toggled, this, [this](bool visible) {
            graphGridVisible_ = visible;
            if (!graphTabs_)
                return;
            for (int index = 0; index < graphTabs_->count(); ++index)
                if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                    plot->setGridVisible(visible);
        });
        connect(graphTabs_, &QTabWidget::currentChanged, this, [this](int) { refreshGraphPlot(); });
        connect(simulationTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index >= 0 && simulationTabs_->tabText(index) == QStringLiteral("Netlist"))
                refreshSimulationNetlist();
        });

        // ── Wire schematic / layout tabs ─────────────────────────────────
        connect(schematicCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0)
                return;
            activeSchematicId_ = schematicCellTabs_->tabBar()->tabData(index).toULongLong();
            refreshSchematic();
        });
        connect(layoutCellTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index < 0)
                return;
            activeLayoutId_ = layoutCellTabs_->tabBar()->tabData(index).toULongLong();
            refreshLayout();
        });

        connect(runButton, &QPushButton::clicked, this, &DocumentWindow::runSimulation);
        documents->addTab(simulationPage_, QStringLiteral("Simulation"));
        documents->addTab(placeholder(QStringLiteral("Verification results"),
                                      QStringLiteral("DRC, LVS, PEX, diagnostics, and provenance")),
                          QStringLiteral("Results"));
        workspaceStack_ = new WorkspaceStack;
        startupView_ = new StartupView;
        startupView_->setOnCreateProject(
            [this](const QString &name, const QString &location) { createProject(name, location); });
        startupView_->setOnOpenProject([this] { openProject(); });
        startupView_->setOnOpenRecent([this](const QString &path) { openProjectPath(path); });
        startupView_->setRecentProjects(loadRecentProjects());
        workspaceStack_->addWidget(startupView_);
        workspaceStack_->addWidget(documents);
        setCentralWidget(workspaceStack_);
        updateEmptyWorkspaceState();
    }

    QWidget *makeEditor(const QString &title, bool layoutEditor) {
        auto *editor = new QWidget;
        auto *editorLayout = new QVBoxLayout(editor);
        QGraphicsView *view = nullptr;
        if (!layoutEditor) {
            schematicCanvas_ = new SchematicCanvas;
            schematicCanvas_->setCallbacks(
                [this](ice::ObjectId id, QPointF position) {
                    if (!project_)
                        return;
                    project_->execute("move symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                it->second.x = static_cast<ice::Coordinate>(position.x());
                                it->second.y = static_cast<ice::Coordinate>(-position.y());
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                },
                [this](ice::ObjectId id) {
                    if (!project_)
                        return;
                    project_->execute("rotate symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                switch (it->second.orientation) {
                                case ice::Orientation::r0:
                                    it->second.orientation = ice::Orientation::r90;
                                    break;
                                case ice::Orientation::r90:
                                    it->second.orientation = ice::Orientation::r180;
                                    break;
                                case ice::Orientation::r180:
                                    it->second.orientation = ice::Orientation::r270;
                                    break;
                                default:
                                    it->second.orientation = ice::Orientation::r0;
                                    break;
                                }
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                    refreshProject();
                },
                [this](ice::ObjectId firstPin, ice::ObjectId secondPin) {
                    if (!project_)
                        return;
                    project_->execute("connect pins", [&](ice::Project &project) {
                        auto cellIt = project.schematicCells.find(activeSchematicId_);
                        if (cellIt == project.schematicCells.end())
                            cellIt = project.schematicCells.begin();
                        auto &cell = cellIt->second;
                        auto netIt = std::find_if(cell.nets.begin(), cell.nets.end(), [&](const auto &entry) {
                            return std::find(entry.second.pins.begin(), entry.second.pins.end(), firstPin) !=
                                   entry.second.pins.end();
                        });
                        if (netIt == cell.nets.end()) {
                            auto netId = project.nextId();
                            cell.nets.emplace(netId,
                                              ice::Net{netId, "N" + std::to_string(netId), {firstPin, secondPin}});
                        } else if (std::find(netIt->second.pins.begin(), netIt->second.pins.end(), secondPin) ==
                                   netIt->second.pins.end())
                            netIt->second.pins.push_back(secondPin);
                        updateAutomaticNetLabelPositions(project);
                    });
                    refreshProject();
                },
                [this](QPointF position) { placeNetLabel(position); },
                [this] {
                    if (!project_)
                        return;
                    project_->markDirty();
                    refreshSchematic();
                },
                [this](ice::ObjectId) { updateSelectionStatus(); },
                [this](ice::ObjectId instanceId) { openSymbolProperties(instanceId); });
            schematicCanvas_->setSymbolPlacementCallback([this](QPointF position) {
                if (!schematicSymbolsPanel_)
                    return;
                const auto *symbol = schematicSymbolsPanel_->selectedSymbol();
                if (!symbol) {
                    statusBar()->showMessage(QStringLiteral("Select a symbol in the Symbols tab first"));
                    return;
                }
                const auto symbolName = symbol->name;
                if (placeSymbolDefinition(*symbol, position))
                    statusBar()->showMessage(QStringLiteral("Placed %1").arg(symbolName));
            });
            schematicCanvas_->setToolEscapeCallback([this] { selectSchematicTool(); });
            schematicCanvas_->deleteSelected_ = [this](std::vector<ice::ObjectId> ids) {
                if (!project_)
                    return;
                project_->execute("delete symbols", [&](ice::Project &project) {
                    for (auto &[_, cell] : project.schematicCells) {
                        std::vector<ice::ObjectId> removedPins;
                        for (auto id : ids) {
                            auto instance = cell.instances.find(id);
                            if (instance == cell.instances.end())
                                continue;
                            auto symbol = cell.symbols.find(instance->second.symbol);
                            if (symbol != cell.symbols.end())
                                for (const auto &pin : symbol->second.pins)
                                    removedPins.push_back(pin.id);
                            cell.instances.erase(instance);
                        }
                        for (auto &[_, net] : cell.nets)
                            net.pins.erase(std::remove_if(net.pins.begin(), net.pins.end(),
                                                          [&](ice::ObjectId pin) {
                                                              return std::find(removedPins.begin(), removedPins.end(),
                                                                               pin) != removedPins.end();
                                                          }),
                                           net.pins.end());
                        for (auto it = cell.nets.begin(); it != cell.nets.end();)
                            if (it->second.pins.size() < 2)
                                it = cell.nets.erase(it);
                            else
                                ++it;
                    }
                });
                refreshProject();
            };
            view = schematicCanvas_;
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
                                                   ? 0
                                                   : project_->project().layoutCells.begin()->first);
                    if (cellId == 0) {
                        QMessageBox::information(this, QStringLiteral("Create Shape"),
                                                 QStringLiteral("Create a layout cell before drawing shapes."));
                        return;
                    }
                    const auto rectangle = QRectF(first, second).normalized();
                    const auto layerId = project_->project().layers.begin()->first;
                    project_->execute("create layout rectangle", [&](ice::Project &project) {
                        ice::addRectangle(project, cellId, layerId,
                                          {static_cast<ice::Coordinate>(rectangle.left()),
                                           static_cast<ice::Coordinate>(-rectangle.bottom()),
                                           static_cast<ice::Coordinate>(rectangle.width()),
                                           static_cast<ice::Coordinate>(rectangle.height())});
                    });
                    refreshProject();
                },
                [this](ice::ObjectId shapeId, QPointF delta) {
                    if (!project_)
                        return;
                    project_->execute("move layout shape", [&](ice::Project &project) {
                        if (!project.layoutCells.contains(activeLayoutId_))
                            return;
                        ice::moveRectangle(project, activeLayoutId_, shapeId, static_cast<ice::Coordinate>(delta.x()),
                                           static_cast<ice::Coordinate>(-delta.y()));
                    });
                    refreshProject();
                },
                [this](std::vector<ice::ObjectId> ids) {
                    if (!project_)
                        return;
                    project_->execute("delete layout shapes", [&](ice::Project &project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end())
                            return;
                        for (const auto id : ids)
                            cellIt->second.shapes.erase(id);
                    });
                    refreshProject();
                },
                [this] {
                    if (project_)
                        project_->markDirty();
                });
            view = layoutCanvas_;
        }
        view->setBackgroundBrush(ice::uiColor(ice::UiColor::Background2));
        view->setAlignment(Qt::AlignCenter);
        view->setAccessibleName(title);
        editorLayout->addWidget(view, 1);
        return editor;
    }

    QToolBar *createEditorToolbar(bool layoutEditor) {
        auto *toolbar =
            new QToolBar(layoutEditor ? QStringLiteral("Layout tools") : QStringLiteral("Schematic tools"), this);
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setAccessibleName(layoutEditor ? QStringLiteral("Layout toolbar")
                                                : QStringLiteral("Schematic toolbar"));

        auto addTool = [toolbar](const QString &iconName, const QString &name) {
            auto *action = toolbar->addAction(name);
            setLucideIcon(action, iconName);
            action->setToolTip(name);
            action->setStatusTip(name);
            return action;
        };

        auto *selectTool = addTool(QStringLiteral("mouse-pointer-2"), QStringLiteral("Select"));
        if (layoutEditor) {
            connect(selectTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_)
                    layoutCanvas_->setTool(LayoutCanvas::Tool::select);
            });
            auto *fitTool = addTool(QStringLiteral("scan"), QStringLiteral("Fit to view"));
            connect(fitTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_)
                    layoutCanvas_->fitToContent();
            });
            toolbar->addSeparator();
            addTool(QStringLiteral("grid-3x3"), QStringLiteral("Grid and snapping"));
            addTool(QStringLiteral("eye"), QStringLiteral("Toggle visibility"));
            toolbar->addSeparator();
            auto *rectangleTool = addTool(QStringLiteral("square"), QStringLiteral("Create rectangle"));
            connect(rectangleTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_)
                    layoutCanvas_->setTool(LayoutCanvas::Tool::rectangle);
            });
            addTool(QStringLiteral("pentagon"), QStringLiteral("Create polygon"));
            addTool(QStringLiteral("circle-dot"), QStringLiteral("Create port"));
            addTool(QStringLiteral("copy"), QStringLiteral("Create instance"));
            toolbar->addSeparator();
            addTool(QStringLiteral("rotate-ccw"), QStringLiteral("Rotate counterclockwise"));
            addTool(QStringLiteral("rotate-cw"), QStringLiteral("Rotate clockwise"));
            toolbar->addSeparator();
            addTool(QStringLiteral("trash"), QStringLiteral("Delete"));
            return toolbar;
        }

        selectTool->setCheckable(true);
        schematicSelectTool_ = selectTool;
        auto *placeSymbolTool = addTool(QStringLiteral("circuit-board"), QStringLiteral("Place Symbol"));
        placeSymbolTool->setCheckable(true);
        auto *placeLabelTool = addTool(QStringLiteral("tag"), QStringLiteral("Place Label"));
        placeLabelTool->setCheckable(true);
        auto *toolGroup = new QActionGroup(toolbar);
        toolGroup->setExclusive(true);
        for (auto *action : {selectTool, placeSymbolTool, placeLabelTool})
            toolGroup->addAction(action);
        selectTool->setChecked(true);
        const auto ensureToolChecked = [this, selectTool, placeSymbolTool, placeLabelTool] {
            if (!selectTool->isChecked() && !placeSymbolTool->isChecked() && !placeLabelTool->isChecked()) {
                selectTool->setChecked(true);
                if (schematicCanvas_) {
                    schematicCanvas_->setPlaceSymbolMode(false);
                    schematicCanvas_->setNetLabelMode(false);
                }
                if (toolEscapeShortcut_)
                    toolEscapeShortcut_->setEnabled(false);
            }
        };
        for (auto *action : {selectTool, placeSymbolTool, placeLabelTool})
            connect(action, &QAction::toggled, this, [ensureToolChecked](bool) { ensureToolChecked(); });
        connect(selectTool, &QAction::triggered, this, [this, selectTool] {
            if (!selectTool->isChecked())
                return;
            if (schematicCanvas_) {
                schematicCanvas_->setPlaceSymbolMode(false);
                schematicCanvas_->setNetLabelMode(false);
            }
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(false);
        });
        connect(placeSymbolTool, &QAction::triggered, this, [this, placeSymbolTool] {
            if (!placeSymbolTool->isChecked())
                return;
            if (schematicLeftTabs_ && schematicSymbolsPanel_)
                schematicLeftTabs_->setCurrentWidget(schematicSymbolsPanel_);
            if (schematicCanvas_) {
                const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
                schematicCanvas_->setPlaceSymbolPreview(symbol ? symbol->svg.toUtf8() : QByteArray());
                schematicCanvas_->setNetLabelMode(false);
                schematicCanvas_->setPlaceSymbolMode(true);
                schematicCanvas_->setFocus();
            }
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(true);
            statusBar()->showMessage(
                QStringLiteral("Select a symbol in the Symbols tab, then click in the schematic to place it"));
        });
        connect(placeLabelTool, &QAction::triggered, this, [this, placeLabelTool] {
            if (!placeLabelTool->isChecked())
                return;
            if (schematicCanvas_) {
                schematicCanvas_->setPlaceSymbolMode(false);
                schematicCanvas_->setNetLabelMode(true);
                schematicCanvas_->setFocus();
            }
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(true);
            statusBar()->showMessage(QStringLiteral("Click in the schematic to place a net label"));
        });

        toolbar->addSeparator();
        auto *toggleGridTool = addTool(QStringLiteral("grid-3x3"), QStringLiteral("Toggle Grid"));
        toggleGridTool->setCheckable(true);
        toggleGridTool->setChecked(true);
        connect(toggleGridTool, &QAction::toggled, this, [this](bool checked) {
            if (schematicCanvas_)
                schematicCanvas_->setGridVisible(checked);
        });
        auto *snapTool = addTool(QStringLiteral("magnet"), QStringLiteral("Snap to Grid"));
        snapTool->setCheckable(true);
        snapTool->setChecked(true);
        connect(snapTool, &QAction::toggled, this, [this](bool checked) {
            if (schematicCanvas_)
                schematicCanvas_->setSnapToGrid(checked);
        });
        auto *fitTool = addTool(QStringLiteral("scan"), QStringLiteral("Fit to View"));
        connect(fitTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->fitToContent();
        });

        toolbar->addSeparator();
        auto *upTool = addTool(QStringLiteral("arrow-up"), QStringLiteral("Go Up the Hierarchy"));
        connect(upTool, &QAction::triggered, this,
                [this] { statusBar()->showMessage(QStringLiteral("Hierarchy navigation is not available yet")); });
        auto *downTool = addTool(QStringLiteral("arrow-down"), QStringLiteral("Go Down the Hierarchy"));
        connect(downTool, &QAction::triggered, this,
                [this] { statusBar()->showMessage(QStringLiteral("Hierarchy navigation is not available yet")); });

        toolbar->addSeparator();
        auto *wrapTool = addTool(QStringLiteral("package"), QStringLiteral("Wrap into Symbol"));
        connect(wrapTool, &QAction::triggered, this,
                [this] { statusBar()->showMessage(QStringLiteral("Wrapping symbols is not available yet")); });
        auto *deleteTool = addTool(QStringLiteral("trash"), QStringLiteral("Delete"));
        connect(deleteTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->deleteSelectedSymbols();
        });
        return toolbar;
    }

    void createStatusBar() {
        auto *bar = new StatusBar(this);
        setStatusBar(bar);
        statusBar_ = bar;
        bar->setProjectName(QStringLiteral("No project open"));
        bar->showMessage(QStringLiteral("Ready — no project open"));
        bar->addPermanentWidget(new QLabel(QStringLiteral("DBU: 1 nm")));
        bar->addPermanentWidget(new QLabel(QStringLiteral("Grid: 1")));
        selectionStatus_ = new QLabel(QStringLiteral("Selection: none"));
        bar->addPermanentWidget(selectionStatus_);
    }

    ProjectPanel *projectPanel_ = nullptr;
    ProjectPanel *schematicHierarchy_ = nullptr;
    ProjectPanel *layoutHierarchy_ = nullptr;
    LayersPanel *layersPanel_ = nullptr;
    QTabWidget *schematicCellTabs_ = nullptr;
    QTabWidget *layoutCellTabs_ = nullptr;
    QStackedWidget *schematicEditorStack_ = nullptr;
    QWidget *schematicEditorPage_ = nullptr;
    EmptySchematicPanel *schematicEmptyPanel_ = nullptr;
    QWidget *schematicToolbarColumn_ = nullptr;
    QStackedWidget *layoutEditorStack_ = nullptr;
    QWidget *layoutEditorPage_ = nullptr;
    EmptyLayoutPanel *layoutEmptyPanel_ = nullptr;
    QWidget *layoutToolbarColumn_ = nullptr;
    ice::ObjectId activeSchematicId_ = 0;
    ice::ObjectId activeLayoutId_ = 0;
    std::unique_ptr<ice::ProjectSession> project_;
    QString projectPath_;
    QStackedWidget *workspaceStack_ = nullptr;
    StartupView *startupView_ = nullptr;
    QDockWidget *activityDock_ = nullptr;
    QTabWidget *documents_ = nullptr;
    SchematicCanvas *schematicCanvas_ = nullptr;
    LayoutCanvas *layoutCanvas_ = nullptr;
    bool layoutViewInitialized_ = false;
    QPlainTextEdit *simulationOutput_ = nullptr;
    QTabWidget *simulationTabs_ = nullptr;
    QPlainTextEdit *simulationNetlist_ = nullptr;
    QWidget *simulationPage_ = nullptr;
    QComboBox *schematicSelector_ = nullptr;
    QComboBox *simulatorSelector_ = nullptr;
    QComboBox *analysisSelector_ = nullptr;
    QDoubleSpinBox *stopTime_ = nullptr;
    QDoubleSpinBox *stepTime_ = nullptr;
    QPlainTextEdit *directiveEditor_ = nullptr;
    SimulationPlot *simulationPlot_ = nullptr;
    SimulationPanel *simulationPanel_ = nullptr;
    QTabWidget *graphTabs_ = nullptr;
    bool graphGridVisible_ = true;
    ice::ObjectId activeSimulationId_ = 0;
    PropertiesPanel *schematicPropertiesPanel_ = nullptr;
    SymbolsPanel *schematicSymbolsPanel_ = nullptr;
    QTabWidget *schematicLeftTabs_ = nullptr;
    SchematicNetlistPanel *schematicNetlistPanel_ = nullptr;
    std::string schematicSymbolsPdk_;
    bool schematicSymbolsLoaded_ = false;
    std::vector<SymbolDefinition> schematicBaseSymbols_;
    StatusBar *statusBar_ = nullptr;
    QLabel *selectionStatus_ = nullptr;
    QAction *schematicSelectTool_ = nullptr;
    QShortcut *toolEscapeShortcut_ = nullptr;
    ice::AppearanceMode appearanceMode_ = ice::AppearanceMode::System;
    QList<QAction *> commands_;
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ICE"));
    app.setApplicationName(QStringLiteral("ICE"));
    app.setApplicationVersion(QString::fromStdString(ice::version()));
    QApplication::setStyle(QStringLiteral("Fusion"));
    const auto appearance = ice::loadAppearanceMode();
    ice::applyAppearance(appearance);

    DocumentWindow window(appearance);
    if (argc == 3 && std::string(argv[1]) == "--project") {
        try {
            if (!window.setProject(ice::Project::load(argv[2])))
                return 0;
        } catch (const ice::ProjectError &) {
            window.statusBar()->showMessage(QStringLiteral("Unable to load project"));
        }
    }
    window.resize(1440, 900);
    window.show();
    return app.exec();
}
