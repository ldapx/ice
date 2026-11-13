#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "layout_devices.hpp"
#include "symbol_ui.hpp"

#include <QWidget>

#include <functional>
#include <string>
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
    using ParameterChanged = std::function<void(ice::ObjectId instanceId, ice::ObjectId parameterId,
                                                const std::string &name, const std::string &value)>;
    using CellParameterChanged = std::function<void(ice::ObjectId cellId, ice::ObjectId parameterId,
                                                    const std::string &name, const std::string &value)>;
    using CellParameterAdded = std::function<void(ice::ObjectId cellId, const std::string &name)>;
    using CellParametersRemoved =
        std::function<void(ice::ObjectId cellId, const std::vector<ice::ObjectId> &parameterIds)>;
    using LayoutCellParameterChanged = std::function<void(ice::ObjectId cellId, ice::ObjectId parameterId,
                                                          const std::string &name, const std::string &value)>;
    using LayoutDeviceParameterChanged =
        std::function<void(const std::string &deviceName, const std::string &name, const std::string &value)>;
    using LayoutPortChanged = std::function<void(const ice::Port &port, const std::string &netName)>;
    using PathWidthChanged = std::function<void(int width)>;
    using GridSpacingChanged = std::function<void(int spacing)>;
    using LayoutShapeSizeChanged = std::function<void(ice::ObjectId shapeId, int width, int height)>;

    explicit PropertiesPanel(bool showDisplay = true, QWidget *parent = nullptr);

    void setOnParameterChanged(ParameterChanged callback);
    void setOnCellParameterChanged(CellParameterChanged callback);
    void setOnCellParameterAdded(CellParameterAdded callback);
    void setOnCellParametersRemoved(CellParametersRemoved callback);
    void setOnLayoutCellParameterChanged(LayoutCellParameterChanged callback);
    void setOnLayoutDeviceParameterChanged(LayoutDeviceParameterChanged callback);
    void setOnLayoutPortChanged(LayoutPortChanged callback);
    void setOnPathWidthChanged(PathWidthChanged callback);
    void setOnGridSpacingChanged(GridSpacingChanged callback);
    void setOnLayoutShapeSizeChanged(LayoutShapeSizeChanged callback);

    void refreshAppearance();
    // Shows the pencil button in the panel heading. It is a placeholder until
    // the properties editor lands, so it intentionally has no action yet.
    void setEditButtonVisible(bool visible);
    void clearSelection();
    void setInstance(const ice::SchematicInstance &instance, const std::string &symbolName,
                     const std::string &symbolSvg);
    void setSchematic(const ice::SchematicCell &cell, const QString &symbolSvg);
    void setLayoutPreviewStyles(const ice::LayoutRenderer::LayerStyleMap &styles,
                                const ice::LayoutRenderer::DeviceStyleMap &deviceStyles);
    void setLayoutCell(const ice::LayoutCell &cell, const ice::LayoutPreviewModel &model);
    void setLayoutDevice(const ice::LayoutDeviceCell *cell);
    void setPathTool(int width, const std::string &layerName);
    // The model carries only the selected figure (including the members of a
    // text polygon), so the preview never shows the rest of the cell.
    void setLayoutShape(const ice::Shape &shape, const std::string &layerName, const std::string &displayName,
                        const std::string &typeName, const ice::LayoutPreviewModel &model);
    void setLayoutPort(const ice::Port &port, const std::string &layerName, const std::string &netName);
    void setLayoutOverview(const ice::LayoutCell &cell, const ice::LayoutPreviewModel &model, int spacing);

  private:
    void setTitle(const QString &title);
    void populateParameters(const std::vector<ice::Parameter> &parameters);
    void setParametersVisible(bool visible);
    void populateLayoutDeviceParameters(const std::vector<ice::LayoutDeviceParameter> &parameters);
    void handleParameterEdit(QTableWidgetItem *item);
    void updateParameterButtons();
    void addParameter();
    void removeParameters();

    QLabel *title_ = nullptr;
    QPushButton *editButton_ = nullptr;
    QStackedWidget *previewStack_ = nullptr;
    SymbolPreview *symbolPreview_ = nullptr;
    LayoutDevicePreview *layoutDevicePreview_ = nullptr;
    ice::LayoutRenderer::LayerStyleMap layoutPreviewStyles_;
    ice::LayoutRenderer::DeviceStyleMap layoutPreviewDeviceStyles_;
    QFrame *previewFrame_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QPushButton *addParameterButton_ = nullptr;
    QPushButton *removeParameterButton_ = nullptr;
    ice::ObjectId instanceId_ = 0;
    ice::ObjectId cellId_ = 0;
    ice::ObjectId layoutCellId_ = 0;
    std::string layoutDeviceName_;
    bool pathMode_ = false;
    bool shapeMode_ = false;
    bool shapeIsRectangle_ = false;
    ice::ObjectId layoutShapeId_ = 0;
    int layoutShapeWidth_ = 0;
    int layoutShapeHeight_ = 0;
    int pathWidth_ = 200;
    std::string pathLayerName_;
    bool portMode_ = false;
    ice::Port layoutPort_;
    std::string layoutPortLayerName_;
    std::string layoutPortNetName_;
    QGroupBox *gridOptions_ = nullptr;
    QSpinBox *gridSpacingSpin_ = nullptr;
    QWidget *parametersHeader_ = nullptr;
    ParameterChanged onParameterChanged_;
    CellParameterChanged onCellParameterChanged_;
    CellParameterAdded onCellParameterAdded_;
    CellParametersRemoved onCellParametersRemoved_;
    LayoutCellParameterChanged onLayoutCellParameterChanged_;
    LayoutDeviceParameterChanged onLayoutDeviceParameterChanged_;
    LayoutPortChanged onLayoutPortChanged_;
    PathWidthChanged onPathWidthChanged_;
    GridSpacingChanged onGridSpacingChanged_;
    LayoutShapeSizeChanged onLayoutShapeSizeChanged_;
};
