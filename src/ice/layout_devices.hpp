#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "icelib/packages.hpp"
#include "layout_render.hpp"

#include <QDialog>
#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPaintEvent;
class QTableWidget;

class LayoutDevicePreview : public QWidget {
  public:
    explicit LayoutDevicePreview(QWidget *parent = nullptr);
    void setCell(const ice::LayoutDeviceCell *cell);
    void setModel(const ice::LayoutPreviewModel &model);
    void setStyles(const ice::LayoutRenderer::LayerStyleMap &styles,
                   const ice::LayoutRenderer::DeviceStyleMap &deviceStyles);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    ice::LayoutPreviewModel model_;
    ice::LayoutRenderer::LayerStyleMap styles_;
    ice::LayoutRenderer::DeviceStyleMap deviceStyles_;
};

class LayoutDevicePlacementDialog : public QDialog {
  public:
    LayoutDevicePlacementDialog(const ice::LayoutDeviceLibrary &library, const std::string &suggestedName,
                                const std::vector<std::string> &schematicPins,
                                const std::map<std::string, std::string> &initialParameters, QWidget *parent = nullptr);

    const ice::LayoutDeviceCell *selectedDevice() const;
    std::map<std::string, std::string> parameterValues() const;

  private:
    int scoreDevice(const ice::LayoutDeviceCell &device) const;
    std::string initialValue(const std::string &parameterName) const;
    void rebuildList();
    void updateSelection();

    const ice::LayoutDeviceLibrary &library_;
    std::string suggestedName_;
    std::vector<std::string> schematicPins_;
    std::map<std::string, std::string> initialParameters_;
    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    LayoutDevicePreview *preview_ = nullptr;
    QTableWidget *parameters_ = nullptr;
    QLabel *hint_ = nullptr;
};

class LayoutDevicesPanel : public QWidget {
  public:
    explicit LayoutDevicesPanel(QWidget *parent = nullptr);

    using DeviceSelectedCallback = std::function<void(const ice::LayoutDeviceCell *)>;
    void setOnDeviceSelected(DeviceSelectedCallback callback);
    void setOnDeviceActivated(std::function<void()> callback);
    void focusDeviceList();
    void clearDeviceSelection();
    void setPdk(const std::string &pdk);

    const ice::LayoutDeviceLibrary &library() const;
    const ice::LayoutDeviceCell *selectedDevice() const;
    bool setParameterValue(const std::string &deviceName, const std::string &parameterName, const std::string &value);
    bool setDeviceGeometry(const std::string &deviceName, const ice::LayoutDeviceGeometry &geometry);
    void resetDevices();

  private:
    void notifySelection();
    void rebuildCategories();
    void rebuildList(const QString &keepSelected = {});

    ice::LayoutDeviceLibrary library_;
    std::filesystem::path libraryPath_;
    std::map<std::string, std::vector<ice::LayoutDeviceParameter>> defaultParameters_;
    std::map<std::string, ice::LayoutDeviceGeometry> defaultGeometry_;
    std::optional<std::string> activePdk_;
    QString loadError_;
    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    DeviceSelectedCallback onDeviceSelected_;
    std::function<void()> onDeviceActivated_;
};

std::filesystem::path layoutDeviceLibraryPath(const std::string &pdk = {});
std::string layoutDeviceCellPrefix(const ice::LayoutDeviceLibrary &library);
std::string stripLayoutDevicePrefix(const ice::LayoutDeviceLibrary &library, const std::string &cellName);
bool layoutPdkIdsMatch(const std::string &left, const std::string &right);
std::filesystem::path ihpSourceRoot();
QString pythonExecutable();
QString icepyPythonPath();

std::map<std::string, std::string> parameterValues(const std::vector<ice::LayoutDeviceParameter> &parameters);

void addPdkLayers(ice::Project &project, const ice::PackagePdk &pdk, const std::string &layoutPdk = {});
void ensureLayoutPdkLayers(ice::Project &project, const std::filesystem::path &installDirectory);

void syncLayoutDeviceParameters(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceCell &device);
const ice::LayoutDeviceCell *findLayoutDevice(const ice::LayoutDeviceLibrary &library, const std::string &cellName);
const ice::LayoutDeviceCell *findLayoutDeviceForCell(const ice::LayoutDeviceLibrary &library,
                                                     const ice::LayoutCell &cell);
const ice::LayoutDeviceCell *findLayoutDeviceForCellName(const ice::LayoutDeviceLibrary &library,
                                                         const std::string &cellName);
std::string uniqueDeviceCellName(const ice::Project &project, const std::string &deviceName,
                                 const std::string &prefix = {});
ice::ObjectId cloneLibraryDeviceCell(ice::Project &project, const ice::LayoutCell &source, const std::string &name);

void applyLayoutDeviceShapes(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                             const std::vector<ice::LayoutDeviceShape> &sourceShapes,
                             const std::vector<ice::LayoutDevicePort> &sourcePorts = {});
// Backfills pins on an existing library cell without regenerating geometry.
void applyLayoutDevicePorts(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                            const std::vector<ice::LayoutDevicePort> &sourcePorts);
std::optional<ice::LayoutDeviceGeometry> generateLayoutDeviceGeometry(const ice::LayoutDeviceCell &device,
                                                                      const std::map<std::string, std::string> &values,
                                                                      QString *error, const std::string &pdk = {});
