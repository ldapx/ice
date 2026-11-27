#pragma once

#include "openvlsilib/core.hpp"
#include "openvlsilib/layout_library.hpp"
#include "openvlsilib/packages.hpp"
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
    void setCell(const openvlsi::LayoutDeviceCell *cell);
    void setModel(const openvlsi::LayoutPreviewModel &model);
    void setStyles(const openvlsi::LayoutRenderer::LayerStyleMap &styles,
                   const openvlsi::LayoutRenderer::DeviceStyleMap &deviceStyles);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    openvlsi::LayoutPreviewModel model_;
    openvlsi::LayoutRenderer::LayerStyleMap styles_;
    openvlsi::LayoutRenderer::DeviceStyleMap deviceStyles_;
};

class LayoutDevicePlacementDialog : public QDialog {
  public:
    LayoutDevicePlacementDialog(const openvlsi::LayoutDeviceLibrary &library, const std::string &suggestedName,
                                const std::vector<std::string> &schematicPins,
                                const std::map<std::string, std::string> &initialParameters, QWidget *parent = nullptr);

    const openvlsi::LayoutDeviceCell *selectedDevice() const;
    std::map<std::string, std::string> parameterValues() const;

  private:
    int scoreDevice(const openvlsi::LayoutDeviceCell &device) const;
    std::string initialValue(const std::string &parameterName) const;
    void rebuildList();
    void updateSelection();

    const openvlsi::LayoutDeviceLibrary &library_;
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

    using DeviceSelectedCallback = std::function<void(const openvlsi::LayoutDeviceCell *)>;
    void setOnDeviceSelected(DeviceSelectedCallback callback);
    void setOnDeviceActivated(std::function<void()> callback);
    void focusDeviceList();
    void clearDeviceSelection();
    void setPdk(const std::string &pdk);
    void setProjectDevices(const openvlsi::Project &project, const std::string &pdk);
    std::optional<openvlsi::ObjectId> projectCellForDevice(const openvlsi::LayoutDeviceCell *device) const;

    const openvlsi::LayoutDeviceLibrary &library() const;
    const openvlsi::LayoutDeviceCell *selectedDevice() const;
    bool setParameterValue(const std::string &deviceName, const std::string &parameterName, const std::string &value);
    bool setDeviceGeometry(const std::string &deviceName, const openvlsi::LayoutDeviceGeometry &geometry);
    void resetDevices();

  private:
    void notifySelection();
    bool ensureDeviceGeometry(std::size_t index);
    void rebuildCategories();
    void rebuildList(const QString &keepSelected = {});

    openvlsi::LayoutDeviceLibrary library_;
    std::filesystem::path libraryPath_;
    std::map<std::string, std::vector<openvlsi::LayoutDeviceParameter>> defaultParameters_;
    std::map<std::string, openvlsi::LayoutDeviceGeometry> defaultGeometry_;
    std::vector<openvlsi::LayoutDeviceCell> projectDevices_;
    std::vector<openvlsi::ObjectId> projectDeviceIds_;
    std::string projectDeviceSignature_;
    std::optional<std::string> activePdk_;
    QString loadError_;
    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    DeviceSelectedCallback onDeviceSelected_;
    std::function<void()> onDeviceActivated_;
};

std::filesystem::path layoutDeviceLibraryPath(const std::string &pdk = {});
std::string layoutDeviceCellPrefix(const openvlsi::LayoutDeviceLibrary &library);
std::string stripLayoutDevicePrefix(const openvlsi::LayoutDeviceLibrary &library, const std::string &cellName);
bool layoutPdkIdsMatch(const std::string &left, const std::string &right);
QString pythonExecutable();
QString openvlsipyPythonPath();

std::map<std::string, std::string> parameterValues(const std::vector<openvlsi::LayoutDeviceParameter> &parameters);

void addPdkLayers(openvlsi::Project &project, const openvlsi::PackagePdk &pdk, const std::string &layoutPdk = {},
                  const std::string &stack = {});
void ensureLayoutPdkLayers(openvlsi::Project &project, const std::filesystem::path &installDirectory);

void syncLayoutDeviceParameters(openvlsi::Project &project, openvlsi::LayoutCell &cell, const openvlsi::LayoutDeviceCell &device);
const openvlsi::LayoutDeviceCell *findLayoutDevice(const openvlsi::LayoutDeviceLibrary &library, const std::string &cellName);
const openvlsi::LayoutDeviceCell *findLayoutDeviceForCell(const openvlsi::LayoutDeviceLibrary &library,
                                                     const openvlsi::LayoutCell &cell);
const openvlsi::LayoutDeviceCell *findLayoutDeviceForCellName(const openvlsi::LayoutDeviceLibrary &library,
                                                         const std::string &cellName);

void applyLayoutDeviceShapes(openvlsi::Project &project, openvlsi::LayoutCell &cell, const openvlsi::LayoutDeviceLibrary &library,
                             const std::vector<openvlsi::LayoutDeviceShape> &sourceShapes,
                             const std::vector<openvlsi::LayoutDevicePort> &sourcePorts = {});
// Backfills ports on an existing library cell without regenerating geometry.
void applyLayoutDevicePorts(openvlsi::Project &project, openvlsi::LayoutCell &cell, const openvlsi::LayoutDeviceLibrary &library,
                            const std::vector<openvlsi::LayoutDevicePort> &sourcePorts);
std::optional<openvlsi::LayoutDeviceGeometry> generateLayoutDeviceGeometry(const openvlsi::LayoutDeviceCell &device,
                                                                      const std::map<std::string, std::string> &values,
                                                                      QString *error, const std::string &pdk = {});
