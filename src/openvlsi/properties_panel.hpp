#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/layout_library.hpp"
#include "layout_devices.hpp"
#include "symbol_ui.hpp"

#include <QWidget>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QFormLayout;
class QFrame;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;

class PropertiesPanel : public QWidget {
  public:
    using ParameterChanged = std::function<void(openvlsi::ObjectId instanceId, openvlsi::ObjectId parameterId,
                                                const std::string &name, const std::string &value)>;
    using CellParameterChanged = std::function<void(openvlsi::ObjectId cellId, openvlsi::ObjectId parameterId,
                                                    const std::string &name, const std::string &value)>;
    // Renaming a parameter keeps its id and value; the callback receives the
    // new name. Names are unique per instance or cell.
    using ParameterRenamed =
        std::function<void(openvlsi::ObjectId instanceId, openvlsi::ObjectId parameterId, const std::string &name)>;
    using CellParameterRenamed =
        std::function<void(openvlsi::ObjectId cellId, openvlsi::ObjectId parameterId, const std::string &name)>;
    using CellParameterAdded = std::function<void(openvlsi::ObjectId cellId, const std::string &name)>;
    using CellParametersRemoved =
        std::function<void(openvlsi::ObjectId cellId, const std::vector<openvlsi::ObjectId> &parameterIds)>;
    using LayoutCellParameterChanged = std::function<void(openvlsi::ObjectId cellId, openvlsi::ObjectId parameterId,
                                                          const std::string &name, const std::string &value)>;
    using LayoutDeviceParameterChanged =
        std::function<void(const std::string &deviceName, const std::string &name, const std::string &value)>;
    using LayoutPortChanged = std::function<void(const openvlsi::Port &port, const std::string &netName)>;
    using GridSpacingChanged = std::function<void(int spacing)>;
    using LayoutShapeSizeChanged = std::function<void(openvlsi::ObjectId shapeId, int width, int height)>;
    using LayoutPathPointChanged = std::function<void(openvlsi::ObjectId shapeId, int pointIndex, int x, int y)>;
    using LayoutPathSegmentChanged =
        std::function<void(openvlsi::ObjectId shapeId, int segmentIndex, int width, openvlsi::ObjectId layer)>;
    using LayoutPortLayer = std::pair<openvlsi::ObjectId, std::string>;
    using SymbolIconEditRequested = std::function<void(openvlsi::ObjectId instanceId)>;

    explicit PropertiesPanel(bool showDisplay = true, QWidget *parent = nullptr);

    void setOnParameterChanged(ParameterChanged callback);
    void setOnCellParameterChanged(CellParameterChanged callback);
    void setOnParameterRenamed(ParameterRenamed callback);
    void setOnCellParameterRenamed(CellParameterRenamed callback);
    void setOnCellParameterAdded(CellParameterAdded callback);
    void setOnCellParametersRemoved(CellParametersRemoved callback);
    void setOnLayoutCellParameterChanged(LayoutCellParameterChanged callback);
    void setOnLayoutDeviceParameterChanged(LayoutDeviceParameterChanged callback);
    void setOnLayoutPortChanged(LayoutPortChanged callback);
    void setOnGridSpacingChanged(GridSpacingChanged callback);
    void setOnLayoutShapeSizeChanged(LayoutShapeSizeChanged callback);
    void setOnLayoutPathPointChanged(LayoutPathPointChanged callback);
    void setOnLayoutPathSegmentChanged(LayoutPathSegmentChanged callback);
    void setOnSymbolIconEditRequested(SymbolIconEditRequested callback);

    void refreshAppearance();
    // Shows the pencil button in the panel heading. The button is enabled only
    // for user-created schematic devices and opens the icon editor.
    void setEditButtonVisible(bool visible);
    void clearSelection();
    // `iconEditable` is true for instances whose symbol represents a drawn
    // schematic cell (a user-created block), not a primitive library device.
    void setInstance(const openvlsi::SchematicInstance &instance, const std::string &symbolName,
                     const std::string &symbolSvg, bool iconEditable = false);
    void setSchematic(const openvlsi::SchematicCell &cell, const QString &symbolSvg);
    void setLayoutPreviewStyles(const openvlsi::LayoutRenderer::LayerStyleMap &styles,
                                const openvlsi::LayoutRenderer::DeviceStyleMap &deviceStyles);
    void setLayoutCell(const openvlsi::LayoutCell &cell, const openvlsi::LayoutPreviewModel &model);
    void setLayoutDevice(const openvlsi::LayoutDeviceCell *cell);
    void setPathTool(const std::string &layerName);
    // The model carries only the selected figure (including the members of a
    // text polygon), so the preview never shows the rest of the cell.
    void setLayoutShape(const openvlsi::Shape &shape, const std::string &layerName, const std::string &displayName,
                        const std::string &typeName, const openvlsi::LayoutPreviewModel &model);
    // Shows numeric editing for one selected path point or a read-only summary
    // for a multi-point selection. The outgoing segment (or the incoming one
    // for the last point) is exposed for width and layer changes.
    void setLayoutPathPoints(const openvlsi::Shape &shape, const std::vector<int> &pointIndices,
                             const std::string &layerName, const std::vector<LayoutPortLayer> &layers,
                             const openvlsi::LayoutPreviewModel &model);
    void setLayoutPort(const openvlsi::Port &port, const std::string &layerName, const std::string &netName,
                       const std::vector<LayoutPortLayer> &layers = {});
    void setLayoutOverview(const openvlsi::LayoutCell &cell, const openvlsi::LayoutPreviewModel &model, int spacing);

  private:
    void setTitle(const QString &title);
    // Swaps the details view for a short hint when nothing is selected; the
    // panel heading is hidden because the enclosing tab already says
    // "Properties".
    void setEmptyState(bool empty);
    void populateParameters(const std::vector<openvlsi::Parameter> &parameters);
    void setParametersVisible(bool visible);
    void populateLayoutDeviceParameters(const std::vector<openvlsi::LayoutDeviceParameter> &parameters);
    void handleParameterEdit(QTableWidgetItem *item);
    void updateParameterButtons();
    void addParameter();
    void removeParameters();

    QLabel *title_ = nullptr;
    QLabel *emptyHint_ = nullptr;
    QWidget *contentSplitter_ = nullptr;
    QPushButton *editButton_ = nullptr;
    QStackedWidget *previewStack_ = nullptr;
    SymbolPreview *symbolPreview_ = nullptr;
    LayoutDevicePreview *layoutDevicePreview_ = nullptr;
    openvlsi::LayoutRenderer::LayerStyleMap layoutPreviewStyles_;
    openvlsi::LayoutRenderer::DeviceStyleMap layoutPreviewDeviceStyles_;
    QFrame *previewFrame_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QPushButton *addParameterButton_ = nullptr;
    QPushButton *removeParameterButton_ = nullptr;
    openvlsi::ObjectId instanceId_ = 0;
    openvlsi::ObjectId cellId_ = 0;
    openvlsi::ObjectId layoutCellId_ = 0;
    std::string layoutDeviceName_;
    bool pathMode_ = false;
    bool shapeMode_ = false;
    bool shapeIsRectangle_ = false;
    bool pathPointMode_ = false;
    openvlsi::ObjectId layoutShapeId_ = 0;
    int layoutShapeWidth_ = 0;
    int layoutShapeHeight_ = 0;
    int layoutPathPointIndex_ = -1;
    int layoutPathSegmentIndex_ = -1;
    int layoutPathPointX_ = 0;
    int layoutPathPointY_ = 0;
    int layoutPathSegmentWidth_ = 0;
    openvlsi::ObjectId layoutPathSegmentLayer_ = 0;
    std::vector<int> layoutPathPointIndices_;
    std::vector<LayoutPortLayer> layoutPathLayers_;
    std::string pathLayerName_;
    bool portMode_ = false;
    openvlsi::Port layoutPort_;
    std::string layoutPortLayerName_;
    std::string layoutPortNetName_;
    QGroupBox *gridOptions_ = nullptr;
    QSpinBox *gridSpacingSpin_ = nullptr;
    QWidget *parametersHeader_ = nullptr;
    ParameterChanged onParameterChanged_;
    CellParameterChanged onCellParameterChanged_;
    ParameterRenamed onParameterRenamed_;
    CellParameterRenamed onCellParameterRenamed_;
    CellParameterAdded onCellParameterAdded_;
    CellParametersRemoved onCellParametersRemoved_;
    LayoutCellParameterChanged onLayoutCellParameterChanged_;
    LayoutDeviceParameterChanged onLayoutDeviceParameterChanged_;
    LayoutPortChanged onLayoutPortChanged_;
    GridSpacingChanged onGridSpacingChanged_;
    LayoutShapeSizeChanged onLayoutShapeSizeChanged_;
    LayoutPathPointChanged onLayoutPathPointChanged_;
    LayoutPathSegmentChanged onLayoutPathSegmentChanged_;
    SymbolIconEditRequested onSymbolIconEditRequested_;
    bool editButtonVisible_ = false;
    bool symbolIconEditable_ = false;
    bool emptyState_ = true;
    // Parameter names may be edited only where a rename callback exists: on
    // schematic instances and schematic cells.
    bool parameterNamesEditable_ = false;
};
