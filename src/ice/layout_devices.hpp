#pragma once

#include "icelib/core.hpp"
#include "icelib/layout_library.hpp"
#include "icelib/packages.hpp"

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

class LayoutDevicePreview : public QWidget {
  public:
    explicit LayoutDevicePreview(QWidget *parent = nullptr);
    void setCell(const ice::LayoutDeviceCell *cell);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    const ice::LayoutDeviceCell *cell_ = nullptr;
};

class LayoutDevicesPanel : public QWidget {
  public:
    explicit LayoutDevicesPanel(QWidget *parent = nullptr);

    using DeviceSelectedCallback = std::function<void(const ice::LayoutDeviceCell *)>;
    void setOnDeviceSelected(DeviceSelectedCallback callback);
    void setOnDeviceActivated(std::function<void()> callback);
    void focusDeviceList();
    void clearDeviceSelection();

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
    std::map<std::string, std::vector<ice::LayoutDeviceParameter>> defaultParameters_;
    std::map<std::string, ice::LayoutDeviceGeometry> defaultGeometry_;
    QString loadError_;
    QLineEdit *search_ = nullptr;
    QComboBox *category_ = nullptr;
    QListWidget *list_ = nullptr;
    DeviceSelectedCallback onDeviceSelected_;
    std::function<void()> onDeviceActivated_;
};

std::filesystem::path layoutDeviceLibraryPath();
std::filesystem::path layoutPcellScriptPath();
std::filesystem::path ihpSourceRoot();
QString pythonExecutable();

std::map<std::string, std::string> parameterValues(const std::vector<ice::LayoutDeviceParameter> &parameters);

void addPdkLayers(ice::Project &project, const ice::PackagePdk &pdk);
void ensureLayoutPdkLayers(ice::Project &project, const std::filesystem::path &installDirectory);

void syncLayoutDeviceParameters(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceCell &device);
const ice::LayoutDeviceCell *findLayoutDevice(const ice::LayoutDeviceLibrary &library, const std::string &cellName);
const ice::LayoutDeviceCell *findLayoutDeviceForCell(const ice::LayoutDeviceLibrary &library,
                                                     const ice::LayoutCell &cell);
const ice::LayoutDeviceCell *findLayoutDeviceForCellName(const ice::LayoutDeviceLibrary &library,
                                                         const std::string &cellName);
std::string uniqueDeviceCellName(const ice::Project &project, const std::string &deviceName);
ice::ObjectId cloneLibraryDeviceCell(ice::Project &project, const ice::LayoutCell &source, const std::string &name);

void applyLayoutDeviceShapes(ice::Project &project, ice::LayoutCell &cell, const ice::LayoutDeviceLibrary &library,
                             const std::vector<ice::LayoutDeviceShape> &sourceShapes);
std::optional<ice::LayoutDeviceCell> makeLayoutCellPreview(const ice::Project &project, const ice::LayoutCell &cell,
                                                           const ice::LayoutDeviceLibrary &library);
std::optional<ice::LayoutDeviceGeometry> generateLayoutDeviceGeometry(const ice::LayoutDeviceCell &device,
                                                                      const std::map<std::string, std::string> &values,
                                                                      QString *error);
