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
class QLabel;
class QPushButton;
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

    explicit PropertiesPanel(bool showDisplay = true, QWidget *parent = nullptr);

    void setOnParameterChanged(ParameterChanged callback);
    void setOnCellParameterChanged(CellParameterChanged callback);
    void setOnCellParameterAdded(CellParameterAdded callback);
    void setOnCellParametersRemoved(CellParametersRemoved callback);
    void setOnLayoutCellParameterChanged(LayoutCellParameterChanged callback);
    void setOnLayoutDeviceParameterChanged(LayoutDeviceParameterChanged callback);

    void refreshAppearance();
    void clearSelection();
    void setInstance(const ice::SchematicInstance &instance, const std::string &symbolName,
                     const std::string &symbolSvg);
    void setSchematic(const ice::SchematicCell &cell, const QString &symbolSvg);
    void setLayoutCell(const ice::LayoutCell &cell, const ice::LayoutDeviceCell *previewCell = nullptr);
    void setLayoutDevice(const ice::LayoutDeviceCell *cell);

  private:
    static QLabel *addRow(QFormLayout *form, const QString &label);
    void populateParameters(const std::vector<ice::Parameter> &parameters);
    void populateLayoutDeviceParameters(const std::vector<ice::LayoutDeviceParameter> &parameters);
    void handleParameterEdit(QTableWidgetItem *item);
    void updateParameterButtons();
    void addParameter();
    void removeParameters();

    QLabel *selection_ = nullptr;
    QStackedWidget *previewStack_ = nullptr;
    SymbolPreview *symbolPreview_ = nullptr;
    LayoutDevicePreview *layoutDevicePreview_ = nullptr;
    ice::LayoutDeviceCell layoutCellPreview_;
    QFrame *previewFrame_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QPushButton *addParameterButton_ = nullptr;
    QPushButton *removeParameterButton_ = nullptr;
    ice::ObjectId instanceId_ = 0;
    ice::ObjectId cellId_ = 0;
    ice::ObjectId layoutCellId_ = 0;
    std::string layoutDeviceName_;
    ParameterChanged onParameterChanged_;
    CellParameterChanged onCellParameterChanged_;
    CellParameterAdded onCellParameterAdded_;
    CellParametersRemoved onCellParametersRemoved_;
    LayoutCellParameterChanged onLayoutCellParameterChanged_;
    LayoutDeviceParameterChanged onLayoutDeviceParameterChanged_;
};
