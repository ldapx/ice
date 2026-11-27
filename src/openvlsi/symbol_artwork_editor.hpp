#pragma once

#include <QColor>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QPainterPath>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QAction;
class QActionGroup;
class QCheckBox;
class QDoubleSpinBox;
class QGraphicsPathItem;
class QLabel;
class QPushButton;
class QShowEvent;
class QToolBar;

// One vector shape of a device icon. Geometry lives directly in viewBox
// coordinates: the editor never stores item transforms, so the exported SVG
// does not depend on how a shape was moved or resized.
struct SymbolArtworkShape {
    QPainterPath path;
    // Parallel to the path's subpaths: true when the subpath was closed with Z.
    QVector<bool> closedSubpaths;
    QColor fill;   // invalid means no fill
    QColor stroke; // invalid means no stroke
    qreal strokeWidth = 2.0;
    Qt::PenCapStyle cap = Qt::FlatCap;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    Qt::FillRule fillRule = Qt::WindingFill;
};

// An editable shape on the icon canvas. Moving translates the path directly;
// resizing is handled by the canvas selection handles.
class SymbolArtworkItem : public QGraphicsItem {
  public:
    explicit SymbolArtworkItem(const SymbolArtworkShape &shape);

    const SymbolArtworkShape &artworkShape() const { return shape_; }
    void setShape(const SymbolArtworkShape &shape);

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

  protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;

  private:
    SymbolArtworkShape shape_;
    bool dragging_ = false;
    QPointF dragStart_;
    QPainterPath dragPath_;
};

// Interactive vector canvas for a device icon. Shapes can be selected,
// moved, resized, restyled, deleted, and drawn with the rectangle, ellipse,
// line, and freehand pencil tools. The document is exported as inline SVG.
class SymbolArtworkCanvas : public QGraphicsView {
  public:
    enum class Tool { select = 0, rectangle = 1, ellipse = 2, line = 3, pencil = 4 };

    explicit SymbolArtworkCanvas(QWidget *parent = nullptr);
    ~SymbolArtworkCanvas() override;

    void setTool(Tool tool);
    Tool tool() const { return tool_; }

    // Style used for new shapes and applied to the current selection.
    void setStroke(QColor color) { stroke_ = std::move(color); }
    void setFill(QColor color) { fill_ = std::move(color); }
    void setStrokeWidth(qreal width) { strokeWidth_ = width; }
    QColor stroke() const { return stroke_; }
    QColor fill() const { return fill_; }
    qreal strokeWidth() const { return strokeWidth_; }

    void setOnSelectionChanged(std::function<void()> callback);
    void setOnHistoryChanged(std::function<void()> callback);

    // Loads an SVG document. Returns false for malformed markup; the current
    // document is left untouched in that case. An empty string starts a blank
    // 120x60 icon.
    bool loadSvg(const QString &svg);
    QString toSvg() const;
    bool documentEmpty() const { return shapes().empty(); }
    // True when the document differs from the SVG it was loaded from. The
    // dialog returns the original markup untouched when nothing changed, so
    // opening and applying an icon does not rewrite unsupported SVG features.
    bool dirty() const;

    std::optional<SymbolArtworkShape> selectionShape() const;
    void applyStyleToSelection(QColor stroke, QColor fill, qreal strokeWidth);
    void deleteSelection();

    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    void undo();
    void redo();
    void fitDocument();

  protected:
    void showEvent(QShowEvent *event) override;
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

  private:
    using ShapeList = std::vector<SymbolArtworkShape>;

    ShapeList shapes() const;
    void restoreShapes(const ShapeList &shapes);
    void pushUndo(const ShapeList &before);
    void addShape(SymbolArtworkShape shape, const ShapeList &before);
    void cancelDrawing();
    QPainterPath drawingPath(const QPointF &scenePosition) const;
    SymbolArtworkShape currentShape(const QPainterPath &path, const QVector<bool> &closed) const;
    SymbolArtworkItem *selectedArtworkItem() const;
    QRectF selectionBox() const;
    int handleAtViewport(const QPoint &position) const;
    void beginResize(int handle, SymbolArtworkItem *item);
    void updateResize(const QPointF &scenePosition);

    QGraphicsScene *scene_ = nullptr;
    Tool tool_ = Tool::select;
    QColor stroke_ = QColor(Qt::black);
    QColor fill_;
    qreal strokeWidth_ = 2.0;
    QRectF viewBox_ = QRectF(0.0, 0.0, 120.0, 60.0);
    // Pixel size used by the SVG renderer when the icon is drawn on a
    // schematic. It is usually, but not always, equal to the viewBox size.
    QSizeF documentSize_ = QSizeF(120.0, 60.0);
    std::vector<ShapeList> undoStack_;
    std::vector<ShapeList> redoStack_;
    ShapeList initialShapes_;
    std::optional<ShapeList> pendingSnapshot_;
    std::function<void()> onSelectionChanged_;
    std::function<void()> onHistoryChanged_;
    QGraphicsPathItem *previewItem_ = nullptr;
    QPointF drawStart_;
    QVector<QPointF> pencilPoints_;
    bool drawing_ = false;
    bool panning_ = false;
    QPointF panStart_;
    bool spaceDown_ = false;
    SymbolArtworkItem *resizeItem_ = nullptr;
    int resizeHandle_ = -1;
    QRectF resizeStartBox_;
    QPainterPath resizeStartPath_;
    QPointF resizeAnchor_;
};

// Embeddable icon editor used as an editor tab. It bundles the icon tool
// column, the vector canvas, and the style/file options panel so the host can
// show it in place of its normal editor. It owns no project state: the host
// asks for the edited SVG with artwork() and decides when to commit it.
class SymbolArtworkEditor : public QWidget {
  public:
    explicit SymbolArtworkEditor(QWidget *parent = nullptr);

    // Loads the SVG markup to edit. An empty string starts a blank icon.
    void load(const QString &symbolName, const QString &currentSvg);
    QString artwork() const;
    bool documentEmpty() const;
    bool dirty() const;
    QString symbolName() const { return symbolName_; }

    void setOnApply(std::function<void()> callback);
    void setOnCancel(std::function<void()> callback);

  private:
    void loadFromFile();
    void saveToFile();
    void applyStyleControls();
    void refreshStyleFromSelection();
    void refreshHistoryButtons();
    void updateSwatches();

    QToolBar *toolbar_ = nullptr;
    SymbolArtworkCanvas *canvas_ = nullptr;
    QActionGroup *tools_ = nullptr;
    QAction *selectAction_ = nullptr;
    QAction *undoAction_ = nullptr;
    QAction *redoAction_ = nullptr;
    QCheckBox *strokeCheck_ = nullptr;
    QCheckBox *fillCheck_ = nullptr;
    QPushButton *strokeButton_ = nullptr;
    QPushButton *fillButton_ = nullptr;
    QDoubleSpinBox *widthSpin_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QColor strokeColor_ = QColor(Qt::black);
    QColor fillColor_ = QColor(230, 230, 230);
    QString symbolName_;
    // SVG of the icon as the host handed it to load(). Kept verbatim so an
    // untouched editor reproduces it exactly.
    QString originalSvg_;
    // SVG the canvas content was last loaded from (the project icon or an SVG
    // file). Dirtiness is measured against originalSvg_.
    QString baselineSvg_;
    bool updatingStyle_ = false;
    std::function<void()> onApply_;
    std::function<void()> onCancel_;
};
