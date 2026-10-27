#include "app_window.hpp"
#include "app_widgets.hpp"
#include "icelib/core.hpp"
#include "icelib/drc.hpp"
#include "icelib/interchange.hpp"
#include "icelib/layout_library.hpp"
#include "icelib/operations.hpp"
#include "icelib/packages.hpp"
#include "icelib/routing.hpp"
#include "icelib/simulation.hpp"
#include "layout_canvas.hpp"
#include "layout_devices.hpp"
#include "properties_panel.hpp"
#include "schematic_canvas.hpp"
#include "schematic_utils.hpp"
#include "side_panels.hpp"

#include "appearance.hpp"
#include "icons.hpp"
#include "package_ui.hpp"
#include "simulation_plot.hpp"
#include "symbol_ui.hpp"
#include "ui_colors.hpp"
#include "ui_helpers.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
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
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QtSvgWidgets/QGraphicsSvgItem>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace {

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
        flattenedLayouts_->clear();
        ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
        for (auto &[_, cell] : project.layoutCells) {
            if (!cell.library && cell.pdk == "ihp.sg13g2" && cell.name.starts_with("IHP "))
                cell.library = true;
        }
        if (layoutDevicesPanel_) {
            layoutDevicesPanel_->resetDevices();
            const auto &library = layoutDevicesPanel_->library();
            for (auto &[_, cell] : project.layoutCells) {
                if (!cell.library || cell.pdk != "ihp.sg13g2" || !cell.name.starts_with("IHP "))
                    continue;
                if (cell.device.empty()) {
                    if (const auto *device = findLayoutDeviceForCellName(library, cell.name))
                        cell.device = device->name;
                }
            }

            struct InstanceReference {
                ice::ObjectId parent = 0;
                ice::ObjectId instance = 0;
            };
            std::map<ice::ObjectId, std::vector<InstanceReference>> references;
            for (const auto &[parentId, parent] : project.layoutCells) {
                for (const auto &[instanceId, instance] : parent.instances) {
                    const auto childIt = project.layoutCells.find(instance.cell);
                    if (childIt == project.layoutCells.end() || !childIt->second.library ||
                        childIt->second.device.empty())
                        continue;
                    references[instance.cell].push_back({parentId, instanceId});
                }
            }
            for (const auto &[childId, childReferences] : references) {
                if (childReferences.size() <= 1)
                    continue;
                const auto source = project.layoutCells.at(childId);
                for (std::size_t index = 1; index < childReferences.size(); ++index) {
                    const auto cloneId =
                        cloneLibraryDeviceCell(project, source, uniqueDeviceCellName(project, source.device));
                    project.layoutCells.at(childReferences[index].parent)
                        .instances.at(childReferences[index].instance)
                        .cell = cloneId;
                }
            }

            for (auto &[_, cell] : project.layoutCells) {
                if (!cell.library)
                    continue;
                const auto *device = findLayoutDeviceForCell(library, cell);
                if (!device)
                    continue;
                if (cell.device.empty())
                    cell.device = device->name;
                syncLayoutDeviceParameters(project, cell, *device);
            }
        }
        project_ = std::make_unique<ice::ProjectSession>(std::move(project));
        resetSectionHistories();
        layoutViewInitialized_ = false;
        openLibraryLayoutCellTabs_.clear();
        clearLayoutDeviceSelection();
        if (layoutSelectTool_)
            layoutSelectTool_->setChecked(true);
        if (layoutCanvas_)
            layoutCanvas_->setTool(LayoutCanvas::Tool::select);
        projectPath_.clear();
        schematicSymbolsLoaded_ = false;
        schematicSymbolsPdk_.clear();
        if (schematicNetlistPanel_)
            schematicNetlistPanel_->clear();
        if (simulationNetlist_)
            simulationNetlist_->clear();
        builtSimulationNetlistSchematicId_ = 0;
        builtSimulationNetlist_.clear();
        refreshSimulationSchematics();
        refreshEmbeddedPanels();
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
        resetSectionHistories();
        updateHistoryPanels();
        clearLayoutDeviceSelection();
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->resetDevices();
        openLibraryLayoutCellTabs_.clear();
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
        if (layoutPropertiesPanel_)
            layoutPropertiesPanel_->clearSelection();
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
            project_->setLastHistoryLabel("Created schematic " + name.trimmed().toStdString());
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
        const auto installDirectory = configuredPackageInstallDirectory();
        CreateLayoutDialog dialog(installDirectory, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        createLayoutCell(dialog.layoutName(), dialog.pdkId());
    }

    void createLayoutCell(const QString &name, const std::string &pdk) {
        if (!project_ || name.trimmed().isEmpty())
            return;
        const auto installDirectory = configuredPackageInstallDirectory();
        auto pdkLabel = QStringLiteral("Generic PDK");
        std::optional<ice::PackagePdk> selectedPdk;
        bool addDependency = false;
        if (pdk != ice::genericPdkId) {
            const auto packages = installedPackages(installDirectory);
            const auto package = std::find_if(packages.begin(), packages.end(),
                                              [&](const auto &entry) { return entry.spec.name == pdk; });
            if (package == packages.end()) {
                QMessageBox::critical(
                    this, QStringLiteral("Create Layout Cell"),
                    QStringLiteral("The selected PDK package %1 is not installed.").arg(QString::fromStdString(pdk)));
                return;
            }
            if (!packageProvidesPdk(package->spec)) {
                QMessageBox::critical(
                    this, QStringLiteral("Create Layout Cell"),
                    QStringLiteral("Package %1 does not provide a PDK.").arg(QString::fromStdString(pdk)));
                return;
            }
            pdkLabel = packagePdkDisplayName(package->spec);
            if (package->spec.pdk)
                selectedPdk = *package->spec.pdk;
            const auto &dependencies = project_->project().packageDependencies;
            if (std::find(dependencies.begin(), dependencies.end(), pdk) == dependencies.end()) {
                const auto displayName = packagePdkDisplayName(package->spec);
                const auto response = QMessageBox::question(
                    this, QStringLiteral("Add Package Dependency"),
                    QStringLiteral("This layout cell uses the %1 PDK from package %2. Add %2 to the project's "
                                   "package dependency list?")
                        .arg(displayName, QString::fromStdString(pdk)),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (response != QMessageBox::Yes)
                    return;
                addDependency = true;
            }
        }
        try {
            project_->execute("create layout cell", [&](ice::Project &project) {
                if (addDependency)
                    project.packageDependencies.push_back(pdk);
                if (selectedPdk)
                    addPdkLayers(project, *selectedPdk);
                activeLayoutId_ = ice::createLayoutCell(project, name.trimmed().toStdString(), pdk);
            });
            project_->setLastHistoryLabel("Created layout cell " + name.trimmed().toStdString());
            refreshProject();
            statusBar()->showMessage(QStringLiteral("Layout cell created with %1").arg(pdkLabel));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Create Layout Cell"), QString::fromStdString(error.what()));
        }
    }

    void makeLayoutFromSchematic(ice::ObjectId schematicId) {
        if (!project_)
            return;
        const auto schematicIt = project_->project().schematicCells.find(schematicId);
        if (schematicIt == project_->project().schematicCells.end())
            return;
        const auto schematicName = QString::fromStdString(schematicIt->second.name);
        try {
            project_->execute("make layout from schematic", [&](ice::Project &project) {
                activeLayoutId_ = ice::createLayoutFromSchematic(project, schematicId);
                ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Make Layout from Schematic"),
                                  QString::fromStdString(error.what()));
            return;
        }
        layoutViewInitialized_ = false;
        if (documents_ && layoutPage_)
            documents_->setCurrentWidget(layoutPage_);
        if (layoutSelectTool_)
            layoutSelectTool_->setChecked(true);
        if (layoutCanvas_)
            layoutCanvas_->setTool(LayoutCanvas::Tool::select);
        refreshProject();
        statusBar()->showMessage(QStringLiteral("Layout created from schematic '%1'").arg(schematicName));
    }

    ice::ObjectId createLayoutDeviceInstanceCell(ice::Project &project, const ice::LayoutDeviceCell &device,
                                                 const ice::LayoutDeviceLibrary &library) const {
        const auto cellId = ice::createLayoutCell(project, uniqueDeviceCellName(project, device.name), library.pdk);
        auto &cell = project.layoutCells.at(cellId);
        cell.library = true;
        cell.device = device.name;
        syncLayoutDeviceParameters(project, cell, device);
        applyLayoutDeviceShapes(project, cell, library, device.shapes);
        ensureLayoutPdkLayers(project, configuredPackageInstallDirectory());
        return cellId;
    }

    void placeLayoutDevice(const QPointF &position) {
        if (!project_)
            return;
        if (!selectedLayoutDevice_) {
            statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
            return;
        }
        if (project_->project().layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Place Device"),
                                     QStringLiteral("Create a layout cell before placing a device."));
            return;
        }
        if (!layoutDevicesPanel_)
            return;
        auto activeId = activeLayoutId_;
        if (!project_->project().layoutCells.contains(activeId))
            activeId = project_->project().layoutCells.begin()->first;
        if (project_->project().layoutCells.at(activeId).library) {
            statusBar()->showMessage(QStringLiteral("Library cells are read-only"));
            return;
        }
        const auto bounds = selectedLayoutDevice_->bounds;
        const qreal centerX = bounds.valid() ? bounds.x + bounds.width / 2.0 : 0.0;
        const qreal centerY = bounds.valid() ? bounds.y + bounds.height / 2.0 : 0.0;
        const auto x = static_cast<ice::Coordinate>(std::lround(position.x() - centerX));
        const auto y = static_cast<ice::Coordinate>(std::lround(-position.y() - centerY));
        const auto &library = layoutDevicesPanel_->library();
        bool placed = false;
        std::string placedName;
        try {
            project_->execute("place layout device", [&](ice::Project &project) {
                const auto childId = createLayoutDeviceInstanceCell(project, *selectedLayoutDevice_, library);
                if (childId == activeId)
                    return;
                auto &cell = project.layoutCells.at(activeId);
                const auto baseName = selectedLayoutDevice_->name;
                std::string name;
                for (int index = 1;; ++index) {
                    name = baseName + "_" + std::to_string(index);
                    const bool exists = std::any_of(cell.instances.begin(), cell.instances.end(),
                                                    [&](const auto &entry) { return entry.second.name == name; });
                    if (!exists)
                        break;
                }
                const auto instanceId = project.nextId();
                cell.instances.emplace(instanceId,
                                       ice::LayoutInstance{instanceId, childId, name, x, y, ice::Orientation::r0});
                placedName = name;
                placed = true;
            });
            if (!placed) {
                statusBar()->showMessage(QStringLiteral("Cannot place a cell inside itself"));
                return;
            }
            project_->setLastHistoryLabel("Placed layout device " + selectedLayoutDevice_->name + " " + placedName);
            activeLayoutId_ = activeId;
            refreshProject();
            if (layoutPropertiesPanel_ && layoutLeftTabs_ && layoutLeftTabs_->currentWidget() == layoutDevicesPanel_)
                layoutPropertiesPanel_->setLayoutDevice(selectedLayoutDevice_);
            statusBar()->showMessage(
                QStringLiteral("Placed %1").arg(QString::fromStdString(selectedLayoutDevice_->name)));
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
        }
    }

    void openLayoutInstanceCell(ice::ObjectId instanceId) {
        if (!project_ || instanceId == 0)
            return;
        const auto activeIt = project_->project().layoutCells.find(activeLayoutId_);
        if (activeIt == project_->project().layoutCells.end())
            return;
        const auto instanceIt = activeIt->second.instances.find(instanceId);
        if (instanceIt == activeIt->second.instances.end())
            return;
        const auto cellId = instanceIt->second.cell;
        const auto childIt = project_->project().layoutCells.find(cellId);
        if (childIt == project_->project().layoutCells.end())
            return;
        openLibraryLayoutCellTabs_.insert(cellId);
        activeLayoutId_ = cellId;
        layoutViewInitialized_ = false;
        refreshEmbeddedPanels();
        refreshLayout();
        statusBar()->showMessage(QStringLiteral("Opened %1").arg(QString::fromStdString(childIt->second.name)));
    }

    void openLayoutParentCell() {
        if (!project_)
            return;
        for (const auto &[id, cell] : project_->project().layoutCells) {
            if (id == activeLayoutId_)
                continue;
            for (const auto &[_, instance] : cell.instances) {
                if (instance.cell != activeLayoutId_)
                    continue;
                openLibraryLayoutCellTabs_.insert(id);
                activeLayoutId_ = id;
                layoutViewInitialized_ = false;
                refreshEmbeddedPanels();
                refreshLayout();
                statusBar()->showMessage(QStringLiteral("Opened %1").arg(QString::fromStdString(cell.name)));
                return;
            }
        }
        statusBar()->showMessage(QStringLiteral("Already at the top of the layout hierarchy"));
    }

    void openSelectedLayoutCell() {
        if (!layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout instance first"));
            return;
        }
        openLayoutInstanceCell(instanceId);
    }

    void updateLayoutPathConfig() {
        if (!project_ || !layoutCanvas_)
            return;
        const auto &layers = project_->project().layers;
        const auto findLayer = [&](const char *name) -> ice::ObjectId {
            const auto wanted = QString::fromLatin1(name);
            for (const auto &[id, layer] : layers) {
                if (QString::fromStdString(layer.name).compare(wanted, Qt::CaseInsensitive) == 0)
                    return id;
            }
            return 0;
        };
        static constexpr std::array<std::pair<const char *, const char *>, 7> steps = {{
            {"Metal1", "Via1"},
            {"Metal2", "Via2"},
            {"Metal3", "Via3"},
            {"Metal4", "Via4"},
            {"Metal5", "TopVia1"},
            {"TopMetal1", "TopVia2"},
            {"TopMetal2", nullptr},
        }};
        LayoutPathConfig config;
        config.width = std::max(1, layoutPathWidth_);
        config.cell = activeLayoutId_;
        for (const auto &[metal, via] : steps) {
            const auto layerId = findLayer(metal);
            if (layerId == 0)
                break;
            const auto viaId = via ? findLayer(via) : 0;
            config.layers.push_back({layerId, viaId, metal});
        }
        if (config.layers.empty() && !layers.empty()) {
            const auto &[id, layer] = *layers.begin();
            config.layers.push_back({id, 0, layer.name});
        }
        layoutCanvas_->setPathConfig(config);
    }

    void setLayoutPathWidth(int width) {
        if (width <= 0)
            return;
        layoutPathWidth_ = std::min(width, 1000000);
        if (layoutCanvas_) {
            layoutCanvas_->setPathWidth(layoutPathWidth_);
            if (layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
                updateLayoutPathConfig();
        }
        statusBar()->showMessage(QStringLiteral("Path width: %1 nm").arg(layoutPathWidth_));
    }

    void addLayoutPathSegment(QPointF start, QPointF end, int width, ice::ObjectId layerId,
                              const std::vector<ice::ObjectId> &viaLayers) {
        if (!project_ || width <= 0)
            return;
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt == project_->project().layoutCells.end() || cellIt->second.library ||
            !project_->project().layers.contains(layerId))
            return;
        const auto polygon = layoutPathPolygon(start, end, width);
        if (polygon.size() < 4)
            return;
        std::vector<ice::LayoutPoint> points;
        points.reserve(static_cast<std::size_t>(polygon.size()));
        ice::Coordinate minX = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate minY = std::numeric_limits<ice::Coordinate>::max();
        ice::Coordinate maxX = std::numeric_limits<ice::Coordinate>::lowest();
        ice::Coordinate maxY = std::numeric_limits<ice::Coordinate>::lowest();
        for (const auto &point : polygon) {
            const auto x = static_cast<ice::Coordinate>(std::lround(point.x()));
            const auto y = static_cast<ice::Coordinate>(std::lround(-point.y()));
            points.push_back({x, y});
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
        if (maxX <= minX || maxY <= minY)
            return;
        const ice::Rect bounds{minX, minY, maxX - minX, maxY - minY};
        const auto centerX = static_cast<ice::Coordinate>(std::lround(start.x()));
        const auto centerY = static_cast<ice::Coordinate>(std::lround(-start.y()));
        const auto half = static_cast<ice::Coordinate>(width / 2);
        try {
            project_->execute("draw layout path", [&](ice::Project &project) {
                auto &cell = project.layoutCells.at(activeLayoutId_);
                const auto shapeId = project.nextId();
                cell.shapes.emplace(shapeId, ice::Shape{shapeId, layerId, std::nullopt, bounds, points});
                for (const auto viaLayer : viaLayers) {
                    if (viaLayer == 0 || !project.layers.contains(viaLayer))
                        continue;
                    ice::addRectangle(project, activeLayoutId_, viaLayer,
                                      {centerX - half, centerY - half, width, width});
                }
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Draw Path"), QString::fromStdString(error.what()));
            return;
        }
        refreshProject();
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
        if (cell->second.library) {
            QMessageBox::information(this, QStringLiteral("Remove Layout Cell"),
                                     QStringLiteral("Library cells are read-only and cannot be removed."));
            return;
        }
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
        project_->setLastHistoryLabel("Created layer " + name.trimmed().toStdString());
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
        if (dialog.indexDirectory() == defaultPackageIndexDirectory())
            settings.remove(QStringLiteral("packages/indexDirectory"));
        else
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
        layoutCanvas_->setGridSpacing(static_cast<qreal>(project.grid));
        auto cellIt = project.layoutCells.find(activeLayoutId_);
        if (cellIt == project.layoutCells.end()) {
            if (project.layoutCells.empty()) {
                if (layoutDevicesPanel_)
                    layoutDevicesPanel_->setPdk({});
                refreshLayoutProperties();
                return;
            }
            cellIt = project.layoutCells.begin();
            activeLayoutId_ = cellIt->first;
        }
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->setPdk(cellIt->second.pdk);
        const bool readOnly = cellIt->second.library;
        if (layoutPlaceDeviceTool_)
            layoutPlaceDeviceTool_->setEnabled(!readOnly);
        if (layoutRectangleTool_)
            layoutRectangleTool_->setEnabled(!readOnly);
        if (layoutPathTool_)
            layoutPathTool_->setEnabled(!readOnly && !project.layers.empty());
        if (layoutDeleteTool_)
            layoutDeleteTool_->setEnabled(!readOnly);
        if (layoutCanvas_) {
            layoutCanvas_->setReadOnly(readOnly);
            if (readOnly) {
                if (layoutSelectTool_)
                    layoutSelectTool_->setChecked(true);
                layoutCanvas_->setTool(LayoutCanvas::Tool::select);
            }
        }
        if (layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
            updateLayoutPathConfig();
        const auto styles = layerStylesFor(project);
        layoutCanvas_->setLayerStyles(styles);
        const auto layerVisible = [&](ice::ObjectId layerId) {
            const auto layer = project.layers.find(layerId);
            return layer == project.layers.end() || layer->second.visible;
        };
        for (const auto &[id, shape] : cellIt->second.shapes) {
            if (!layerVisible(shape.layer))
                continue;
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
            if (!readOnly) {
                item->setFlag(QGraphicsItem::ItemIsSelectable);
                item->setFlag(QGraphicsItem::ItemIsMovable);
            }
            item->setToolTip(readOnly ? QStringLiteral("Library cell shape %1").arg(id)
                                      : QStringLiteral("Shape %1").arg(id));
        }

        const auto drawCellToGroup = [&](const ice::LayoutCell &cell,
                                         const std::function<ice::Point(ice::Point)> &mapper,
                                         std::vector<ice::ObjectId> &ancestors, QGraphicsItemGroup *group) {
            std::function<void(const ice::LayoutCell &, const std::function<ice::Point(ice::Point)> &,
                               std::vector<ice::ObjectId> &)>
                drawCell;
            drawCell = [&](const ice::LayoutCell &currentCell,
                           const std::function<ice::Point(ice::Point)> &currentMapper,
                           std::vector<ice::ObjectId> &currentAncestors) {
                for (const auto &[_, shape] : currentCell.shapes) {
                    if (!layerVisible(shape.layer))
                        continue;
                    const auto style = styles.contains(shape.layer)
                                           ? styles.at(shape.layer)
                                           : LayerStyle{ice::uiColor(ice::UiColor::Border1), Qt::SolidPattern};
                    const QPen pen(style.color, 1);
                    QBrush brush(QColor(style.color.red(), style.color.green(), style.color.blue(), 110),
                                 style.pattern);
                    if (style.pattern == Qt::NoBrush)
                        brush = QBrush(Qt::NoBrush);
                    std::vector<ice::Point> points;
                    if (shape.polygon.size() >= 3) {
                        points.reserve(shape.polygon.size());
                        for (const auto &point : shape.polygon)
                            points.push_back({point.x, point.y});
                    } else {
                        const auto &rect = shape.rectangle;
                        points = {{rect.x, rect.y},
                                  {rect.x + rect.width, rect.y},
                                  {rect.x + rect.width, rect.y + rect.height},
                                  {rect.x, rect.y + rect.height}};
                    }
                    QPolygonF polygon;
                    polygon.reserve(static_cast<int>(points.size()));
                    for (const auto &point : points) {
                        const auto mapped = currentMapper(point);
                        polygon << QPointF(mapped.x, -mapped.y);
                    }
                    if (polygon.size() >= 3) {
                        auto *item = new QGraphicsPolygonItem(polygon);
                        item->setPen(pen);
                        item->setBrush(brush);
                        item->setAcceptedMouseButtons(Qt::NoButton);
                        group->addToGroup(item);
                    }
                }
                for (const auto &[_, child] : currentCell.instances) {
                    const auto childIt = project.layoutCells.find(child.cell);
                    if (childIt == project.layoutCells.end() ||
                        std::find(currentAncestors.begin(), currentAncestors.end(), child.cell) !=
                            currentAncestors.end())
                        continue;
                    const ice::Transform transform{child.x, child.y, child.orientation};
                    const auto parentMapper = currentMapper;
                    const auto childMapper = [parentMapper, transform](ice::Point point) {
                        return parentMapper(transform.apply(point));
                    };
                    currentAncestors.push_back(child.cell);
                    drawCell(childIt->second, childMapper, currentAncestors);
                    currentAncestors.pop_back();
                }
            };
            drawCell(cell, mapper, ancestors);
        };

        const auto identity = [](ice::Point point) { return point; };
        std::vector<ice::ObjectId> ancestors{activeLayoutId_};
        for (const auto &[_, instance] : cellIt->second.instances) {
            const auto childIt = project.layoutCells.find(instance.cell);
            if (childIt == project.layoutCells.end() ||
                std::find(ancestors.begin(), ancestors.end(), instance.cell) != ancestors.end())
                continue;
            auto *group = new QGraphicsItemGroup;
            group->setData(0, QVariant::fromValue<qulonglong>(instance.id));
            group->setData(1, QStringLiteral("layout-instance"));
            if (!readOnly)
                group->setFlag(QGraphicsItem::ItemIsSelectable);
            group->setToolTip(QStringLiteral("Instance %1").arg(QString::fromStdString(instance.name)));
            layoutCanvas_->scene()->addItem(group);
            const ice::Transform transform{instance.x, instance.y, instance.orientation};
            const std::function<ice::Point(ice::Point)> parentMapper = identity;
            const std::function<ice::Point(ice::Point)> childMapper = [parentMapper, transform](ice::Point point) {
                return parentMapper(transform.apply(point));
            };
            ancestors.push_back(instance.cell);
            drawCellToGroup(childIt->second, childMapper, ancestors, group);
            ancestors.pop_back();
            const auto bounds = group->childrenBoundingRect();
            if (bounds.isValid() && !bounds.isNull()) {
                auto *highlight = new QGraphicsRectItem(bounds.adjusted(-2, -2, 2, 2), group);
                auto accent = ice::uiColor(ice::UiColor::Accent4);
                QPen pen(accent, 1.5, Qt::DashLine);
                pen.setCosmetic(true);
                highlight->setPen(pen);
                QColor fill = accent;
                fill.setAlpha(25);
                highlight->setBrush(fill);
                highlight->setData(0, QStringLiteral("layout-instance-highlight"));
                highlight->setZValue(1000);
                highlight->setAcceptedMouseButtons(Qt::NoButton);
                highlight->setVisible(false);
            }
        }

        const auto plannedRect = [](const ice::PlannedDevice &device) {
            return QRectF(static_cast<qreal>(device.x),
                          -static_cast<qreal>(device.y) - static_cast<qreal>(device.height),
                          static_cast<qreal>(device.width), static_cast<qreal>(device.height));
        };
        // Stop a connection at the box edge instead of drawing through the box.
        const auto plannedEdge = [](const QRectF &rect, const QPointF &toward) {
            const auto center = rect.center();
            const auto delta = toward - center;
            if (delta.isNull())
                return center;
            const auto scaleX =
                delta.x() != 0.0 ? rect.width() * 0.5 / std::abs(delta.x()) : std::numeric_limits<qreal>::max();
            const auto scaleY =
                delta.y() != 0.0 ? rect.height() * 0.5 / std::abs(delta.y()) : std::numeric_limits<qreal>::max();
            return center + delta * std::min(scaleX, scaleY);
        };
        QPen connectionPen(ice::uiColor(ice::UiColor::Accent4));
        connectionPen.setCosmetic(true);
        connectionPen.setWidthF(1.5);
        for (const auto &[connectionId, connection] : cellIt->second.plannedConnections) {
            const auto from = cellIt->second.plannedDevices.find(connection.from);
            const auto to = cellIt->second.plannedDevices.find(connection.to);
            if (from == cellIt->second.plannedDevices.end() || to == cellIt->second.plannedDevices.end())
                continue;
            const auto fromRect = plannedRect(from->second);
            const auto toRect = plannedRect(to->second);
            auto *line = layoutCanvas_->scene()->addLine(
                QLineF(plannedEdge(fromRect, toRect.center()), plannedEdge(toRect, fromRect.center())), connectionPen);
            line->setZValue(-5.0);
            line->setData(0, QVariant::fromValue<qulonglong>(connectionId));
            line->setAcceptedMouseButtons(Qt::NoButton);
            if (!connection.name.empty())
                line->setToolTip(QString::fromStdString(connection.name));
        }
        for (const auto &[deviceId, device] : cellIt->second.plannedDevices) {
            layoutCanvas_->scene()->addItem(createLayoutPlannedDeviceItem(deviceId, QString::fromStdString(device.name),
                                                                          plannedRect(device), readOnly));
        }
        layoutCanvas_->restoreLayoutInstanceSelection();
        if (!layoutViewInitialized_) {
            layoutCanvas_->fitToContent();
            layoutViewInitialized_ = true;
        }
        refreshLayoutProperties();
    }

    void clearLayoutDeviceSelection() {
        if (layoutDevicesPanel_)
            layoutDevicesPanel_->clearDeviceSelection();
        selectedLayoutDevice_ = nullptr;
        if (layoutCanvas_)
            layoutCanvas_->setDevicePreview(nullptr);
    }

    void transformSelectedLayoutInstance(ice::Orientation step, const QString &operation) {
        if (!project_ || !layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout device first"));
            return;
        }
        project_->execute(operation.toStdString(), [&](ice::Project &project) {
            const auto cellIt = project.layoutCells.find(activeLayoutId_);
            if (cellIt == project.layoutCells.end() || cellIt->second.library)
                return;
            if (auto instance = cellIt->second.instances.find(instanceId); instance != cellIt->second.instances.end())
                instance->second.orientation = composeOrientation(instance->second.orientation, step);
        });
        if (const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
            cellIt != project_->project().layoutCells.end()) {
            if (const auto instance = cellIt->second.instances.find(instanceId);
                instance != cellIt->second.instances.end())
                project_->setLastHistoryLabel(operation.toStdString() + " " + instance->second.name);
        }
        refreshProject();
    }

    void snapSelectedLayoutInstance() {
        if (!project_ || !layoutCanvas_)
            return;
        const auto instanceId = layoutCanvas_->selectedLayoutInstance();
        if (instanceId == 0) {
            statusBar()->showMessage(QStringLiteral("Select a layout device first"));
            return;
        }
        project_->execute("snap layout device", [&](ice::Project &project) {
            const auto cellIt = project.layoutCells.find(activeLayoutId_);
            if (cellIt == project.layoutCells.end() || cellIt->second.library)
                return;
            if (auto instance = cellIt->second.instances.find(instanceId); instance != cellIt->second.instances.end()) {
                instance->second.x = project.snap(instance->second.x);
                instance->second.y = project.snap(instance->second.y);
            }
        });
        if (const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
            cellIt != project_->project().layoutCells.end()) {
            if (const auto instance = cellIt->second.instances.find(instanceId);
                instance != cellIt->second.instances.end())
                project_->setLastHistoryLabel("Snapped " + instance->second.name + " to grid");
        }
        refreshProject();
    }

    void refreshLayoutProperties(bool bringToFront = false) {
        if (!layoutPropertiesPanel_)
            return;
        if (layoutCanvas_ && layoutCanvas_->currentTool() == LayoutCanvas::Tool::path) {
            layoutPropertiesPanel_->setPathTool(layoutCanvas_->pathWidth(), layoutCanvas_->pathLayerName());
            if (bringToFront && layoutRightTabs_)
                layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
            return;
        }
        if (project_ && layoutCanvas_ && layoutCanvas_->currentTool() == LayoutCanvas::Tool::select) {
            const auto instanceId = layoutCanvas_->selectedLayoutInstance();
            if (instanceId != 0) {
                const auto activeIt = project_->project().layoutCells.find(activeLayoutId_);
                if (activeIt != project_->project().layoutCells.end()) {
                    const auto instanceIt = activeIt->second.instances.find(instanceId);
                    if (instanceIt != activeIt->second.instances.end()) {
                        const auto childIt = project_->project().layoutCells.find(instanceIt->second.cell);
                        if (childIt != project_->project().layoutCells.end()) {
                            std::optional<ice::LayoutDeviceCell> preview;
                            if (childIt->second.library && !childIt->second.device.empty() && layoutDevicesPanel_)
                                preview = makeLayoutCellPreview(project_->project(), childIt->second,
                                                                layoutDevicesPanel_->library());
                            layoutPropertiesPanel_->setLayoutCell(childIt->second, preview ? &*preview : nullptr);
                            if (bringToFront && layoutRightTabs_)
                                layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                            return;
                        }
                    }
                }
            }
        }
        if (layoutLeftTabs_ && layoutDevicesPanel_ && layoutLeftTabs_->currentWidget() == layoutDevicesPanel_) {
            layoutPropertiesPanel_->setLayoutDevice(selectedLayoutDevice_);
            return;
        }
        if (!project_) {
            layoutPropertiesPanel_->clearSelection();
            return;
        }
        const auto cellIt = project_->project().layoutCells.find(activeLayoutId_);
        if (cellIt != project_->project().layoutCells.end()) {
            std::optional<ice::LayoutDeviceCell> preview;
            if (cellIt->second.library && !cellIt->second.device.empty() && layoutDevicesPanel_)
                preview = makeLayoutCellPreview(project_->project(), cellIt->second, layoutDevicesPanel_->library());
            layoutPropertiesPanel_->setLayoutCell(cellIt->second, preview ? &*preview : nullptr);
        } else {
            layoutPropertiesPanel_->clearSelection();
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
        if (documents_ && layoutPage_ && documents_->currentWidget() == layoutPage_) {
            if (layoutLeftTabs_ && layoutDevicesPanel_)
                layoutLeftTabs_->setCurrentWidget(layoutDevicesPanel_);
            if (layoutDevicesPanel_)
                layoutDevicesPanel_->focusDeviceList();
            statusBar()->showMessage(QStringLiteral("Select a layout device"));
            return;
        }
        if (!project_ || project_->project().schematicCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Place Device"),
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
            activateSchematicPlaceSymbolTool(definition, true);
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
        }
    }

    bool placeSymbolDefinition(const SymbolDefinition &definition, QPointF position) {
        if (!project_ || !schematicCanvas_ || project_->project().schematicCells.empty())
            return false;
        try {
            const auto cellId = project_->project().schematicCells.contains(activeSchematicId_)
                                    ? activeSchematicId_
                                    : project_->project().schematicCells.begin()->first;
            std::string placedName;
            project_->execute("place symbol", [&](ice::Project &project) {
                const auto symbolId = ice::addSymbol(project, cellId, definition.name.toStdString(),
                                                     definition.svg.toStdString(), definition.cell);
                auto &symbol = project.schematicCells.at(cellId).symbols.at(symbolId);
                symbol.label = definition.label;
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
                placedName = instanceName;
                const auto instanceId = ice::addSchematicInstance(project, cellId, symbolId, instanceName,
                                                                  static_cast<ice::Coordinate>(position.x()),
                                                                  static_cast<ice::Coordinate>(-position.y()));
                // Seed the symbol's default parameters (resistance, capacitance,
                // ...) so they are editable in the properties panel right away.
                auto &placedInstance = project.schematicCells.at(cellId).instances.at(instanceId);
                ice::seedDefaultParameters(project, placedInstance, instanceDefaults(definition));
            });
            project_->setLastHistoryLabel("Placed " + definition.name.toStdString() + " " + placedName);
            refreshProject();
            return true;
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Place Device"), QString::fromStdString(error.what()));
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

    void updateSchematicNetlistState() {
        if (!schematicNetlistPanel_)
            return;
        if (!project_ || project_->project().schematicCells.empty()) {
            schematicNetlistPanel_->clear();
            return;
        }
        const auto &cells = project_->project().schematicCells;
        auto cell = cells.find(activeSchematicId_);
        if (cell == cells.end())
            cell = cells.begin();
        if (cell != cells.end())
            schematicNetlistPanel_->setCell(project_->project(), &cell->second);
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
        updateSchematicNetlistState();
        updateSimulationNetlistState();
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

    void updateLayoutCellParameter(ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name,
                                   const std::string &value) {
        if (!project_)
            return;
        const auto cellIt = project_->project().layoutCells.find(cellId);
        if (cellIt == project_->project().layoutCells.end())
            return;
        const auto cellName = cellIt->second.name;
        const auto *device =
            layoutDevicesPanel_ ? findLayoutDeviceForCell(layoutDevicesPanel_->library(), cellIt->second) : nullptr;
        if (!device || !device->generator) {
            try {
                project_->execute("edit layout parameter", [&](ice::Project &project) {
                    const auto editIt = project.layoutCells.find(cellId);
                    if (editIt == project.layoutCells.end())
                        return;
                    for (auto &parameter : editIt->second.parameters) {
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
            return;
        }

        auto values = parameterValues(device->parameters);
        for (const auto &parameter : cellIt->second.parameters)
            values[parameter.name] = parameter.value;
        values[name] = value;

        QString generationError;
        std::optional<ice::LayoutDeviceGeometry> geometry;
        {
            WaitCursorGuard waitCursor;
            statusBar()->showMessage(QStringLiteral("Regenerating %1…").arg(QString::fromStdString(device->name)));
            geometry = generateLayoutDeviceGeometry(*device, values, &generationError);
        }
        if (!geometry) {
            QMessageBox::warning(this, QStringLiteral("Regenerate Device"),
                                 QStringLiteral("Could not regenerate %1 geometry:\n\n%2")
                                     .arg(QString::fromStdString(device->name), generationError));
            refreshLayoutProperties();
            return;
        }

        try {
            project_->execute("regenerate layout parameter", [&](ice::Project &project) {
                const auto editIt = project.layoutCells.find(cellId);
                if (editIt == project.layoutCells.end())
                    return;
                auto &cell = editIt->second;
                syncLayoutDeviceParameters(project, cell, *device);
                for (auto &parameter : cell.parameters) {
                    if (const auto generated = values.find(parameter.name); generated != values.end())
                        parameter.value = generated->second;
                }
                if (cell.library)
                    applyLayoutDeviceShapes(project, cell, layoutDevicesPanel_->library(), geometry->shapes);
            });
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Regenerate Device"), QString::fromStdString(error.what()));
            refreshLayoutProperties();
            return;
        }

        refreshProject();
        statusBar()->showMessage(QStringLiteral("Regenerated %1 geometry (%2 polygons)")
                                     .arg(QString::fromStdString(cellName))
                                     .arg(geometry->shapes.size()));
    }

    void updateLayoutDeviceParameter(const std::string &deviceName, const std::string &name, const std::string &value) {
        if (!layoutDevicesPanel_)
            return;
        const auto *device = findLayoutDevice(layoutDevicesPanel_->library(), deviceName);
        if (!device)
            return;
        auto values = parameterValues(device->parameters);
        values[name] = value;
        if (!device->generator) {
            layoutDevicesPanel_->setParameterValue(deviceName, name, value);
            return;
        }

        QString generationError;
        std::optional<ice::LayoutDeviceGeometry> geometry;
        {
            WaitCursorGuard waitCursor;
            statusBar()->showMessage(QStringLiteral("Regenerating %1…").arg(QString::fromStdString(deviceName)));
            geometry = generateLayoutDeviceGeometry(*device, values, &generationError);
        }
        if (!geometry) {
            QMessageBox::warning(this, QStringLiteral("Regenerate Device"),
                                 QStringLiteral("Could not regenerate %1 geometry:\n\n%2")
                                     .arg(QString::fromStdString(deviceName), generationError));
            refreshLayoutProperties();
            return;
        }

        for (const auto &[parameterName, parameterValue] : values)
            layoutDevicesPanel_->setParameterValue(deviceName, parameterName, parameterValue);
        layoutDevicesPanel_->setDeviceGeometry(deviceName, *geometry);
        selectedLayoutDevice_ = device;
        if (layoutCanvas_)
            layoutCanvas_->setDevicePreview(device);
        refreshLayoutProperties();
        statusBar()->showMessage(QStringLiteral("Updated default %1 PCell parameters for new placements")
                                     .arg(QString::fromStdString(deviceName)));
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
                QMessageBox::information(this, QStringLiteral("Device Properties"),
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
        // (axis selections, limits, ...) redraws the current data instead of
        // discarding it and forcing another simulation run.
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
            if (const auto previous = previousPlots.find(graphs[i].id); previous != previousPlots.end())
                plot->setResult(previous->second.result);
            const auto tabName = QString::fromStdString(graphs[i].name);
            const auto idx = graphTabs_->addTab(plot, tabName);
            graphTabs_->tabBar()->setTabData(idx, QVariant::fromValue<qulonglong>(graphs[i].id));
        }
        const int restored = (prevIndex >= 0 && prevIndex < graphTabs_->count()) ? prevIndex : 0;
        graphTabs_->setCurrentIndex(restored);
    }

    // Called when the SimulationPanel selection changes.
    void updateAnalysisControlLine() {
        if (!analysisControl_)
            return;
        const auto analysis = analysisSelector_->currentData().toString();
        const bool transient = analysis == QStringLiteral("transient");
        if (useInitialConditions_)
            useInitialConditions_->setEnabled(transient);
        QString line;
        if (transient) {
            line = QStringLiteral(".tran %1 %2").arg(stepTime_->value()).arg(stopTime_->value());
            if (useInitialConditions_ && useInitialConditions_->isChecked())
                line += QStringLiteral(" uic");
        } else if (analysis == QStringLiteral("dc")) {
            line = QStringLiteral(".dc V1 0 1 0.01");
        } else if (analysis == QStringLiteral("ac")) {
            line = QStringLiteral(".ac dec 10 1 1e6");
        } else {
            line = QStringLiteral(".op");
        }
        analysisControl_->setText(line);
        analysisControl_->setCursorPosition(0);
    }

    void loadSimulationById(ice::ObjectId id, ice::ObjectId graphId = 0) {
        const bool sameSimulation = (id == activeSimulationId_);
        activeSimulationId_ = id;
        if (!project_ || id == 0) {
            rebuildGraphTabs({});
            updateSimulationNetlistState();
            return;
        }
        const auto &sims = project_->project().simulations;
        auto it = sims.find(id);
        if (it == sims.end()) {
            rebuildGraphTabs({});
            updateSimulationNetlistState();
            return;
        }
        const auto &sim = it->second;

        // Block signals so we don't re-save while populating.
        QSignalBlocker b1(schematicSelector_), b2(simulatorSelector_), b3(analysisSelector_), b4(stopTime_),
            b5(stepTime_), b6(additionalDirectives_), b7(useInitialConditions_), b8(signalsCapture_);

        const auto schIdx = schematicSelector_->findData(QVariant::fromValue<qulonglong>(sim.schematic));
        if (schIdx >= 0)
            schematicSelector_->setCurrentIndex(schIdx);
        simulatorSelector_->setCurrentIndex(simulatorSelector_->findData(QString::fromStdString(sim.simulator)));
        analysisSelector_->setCurrentIndex(analysisSelector_->findData(QString::fromStdString(sim.analysis)));
        stopTime_->setValue(sim.stop);
        stepTime_->setValue(sim.step);
        QStringList additional;
        for (const auto &line : QString::fromStdString(sim.directives).split(QLatin1Char('\n'))) {
            const auto trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !isAnalysisControlDirective(trimmed))
                additional << trimmed;
        }
        additionalDirectives_->setText(additional.join(QLatin1Char(' ')));
        signalsCapture_->setText(QString::fromStdString(sim.captureSignals));
        useInitialConditions_->setChecked(sim.use_initial_conditions);
        updateAnalysisControlLine();

        // Avoid rebuilding the graph tabs (and losing plot results) when the
        // selection did not actually change, e.g. after a property commit.
        if (!sameSimulation)
            rebuildGraphTabs(sim.graphs);
        if (graphId != 0)
            selectGraphTab(graphId);
        updateSimulationNetlistState();
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
            sim.use_initial_conditions = useInitialConditions_->isChecked();
            sim.directives = additionalDirectives_->text().toStdString();
            sim.captureSignals = signalsCapture_->text().toStdString();
        });
        updateSimulationNetlistState();
    }

    void pickSimulationSignal() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        const auto schematicId = selectedSimulationSchematicId();
        if (schematicId == 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("Select a schematic in the Simulation properties first."));
            return;
        }
        if (!schematicCellTabs_ || !schematicCanvas_ || !documents_ || !schematicPage_) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("The schematic editor is not available."));
            return;
        }
        int targetIndex = -1;
        for (int index = 0; index < schematicCellTabs_->count(); ++index) {
            if (schematicCellTabs_->tabBar()->tabData(index).toULongLong() == schematicId) {
                targetIndex = index;
                break;
            }
        }
        if (targetIndex < 0) {
            QMessageBox::information(this, QStringLiteral("Select Signal"),
                                     QStringLiteral("The selected schematic is no longer available."));
            return;
        }
        schematicCellTabs_->setCurrentIndex(targetIndex);
        activeSchematicId_ = schematicId;
        refreshSchematic();
        selectSchematicTool();
        schematicCanvas_->setSignalPickMode(true);
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(false);
        documents_->setCurrentWidget(schematicPage_);
        schematicCanvas_->setFocus();
        statusBar()->showMessage(QStringLiteral("Click a node or wire to capture it (Esc cancels)"));
    }

    void finishSimulationSignalPick(ice::ObjectId netId) {
        if (!project_ || netId == 0 || !signalsCapture_)
            return;
        auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
        if (cellIt == project_->project().schematicCells.end())
            return;
        const auto netIt = cellIt->second.nets.find(netId);
        if (netIt == cellIt->second.nets.end())
            return;
        const auto node = netIt->second.name.empty() ? "n" + std::to_string(netId) : netIt->second.name;
        const auto signal = QStringLiteral("v(%1)").arg(QString::fromStdString(node));
        auto signalNames = parseSignalList(signalsCapture_->text());
        const auto normalize = [](QString text) {
            text = text.trimmed().toLower();
            if (text.startsWith(QStringLiteral("v(")) && text.endsWith(QLatin1Char(')')))
                text = text.mid(2, text.size() - 3);
            return text;
        };
        const bool alreadyPresent = std::any_of(signalNames.begin(), signalNames.end(), [&](const QString &existing) {
            return normalize(existing) == normalize(signal);
        });
        if (!alreadyPresent) {
            signalNames << signal;
            signalsCapture_->setText(signalNames.join(QStringLiteral(", ")));
            commitSimulationProperties();
            statusBar()->showMessage(QStringLiteral("Captured signal %1").arg(signal));
        } else {
            statusBar()->showMessage(QStringLiteral("Signal %1 is already captured").arg(signal));
        }
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        signalsCapture_->setFocus();
        signalsCapture_->setCursorPosition(signalsCapture_->text().size());
    }

    void cancelSimulationSignalPick() {
        if (documents_ && simulationPage_)
            documents_->setCurrentWidget(simulationPage_);
        if (signalsCapture_) {
            signalsCapture_->setFocus();
            signalsCapture_->setCursorPosition(signalsCapture_->text().size());
        }
        statusBar()->showMessage(QStringLiteral("Signal selection cancelled"));
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
        updateSimulationNetlistState();
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
            const auto signalNames = parseSignalList(signalsCapture_ ? signalsCapture_->text() : QString());
            if (!signalNames.isEmpty())
                graph.ySignals.push_back(signalNames.front().toStdString());
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

    // Edit the current graph's name, axis signal selections, and limits.
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

        // Build a small dialog for editing the name, axis selections, and limits.
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Edit Graph"));
        dlg.resize(500, 500);
        auto *dlgLayout = new QVBoxLayout(&dlg);
        auto *form = new QFormLayout;
        auto *nameEdit = new QLineEdit(QString::fromStdString(currentGraph.name));
        auto *xAxisCombo = new QComboBox;
        const auto capturedSignals = parseSignalList(signalsCapture_ ? signalsCapture_->text() : QString());
        xAxisCombo->addItem(QStringLiteral("Time (s)"), QStringLiteral("time"));
        for (const auto &signal : capturedSignals)
            xAxisCombo->addItem(signal, signal);
        int xIndex = xAxisCombo->findData(QString::fromStdString(currentGraph.xAxis));
        if (xIndex < 0 && currentGraph.xAxis == "Time (s)")
            xIndex = xAxisCombo->findData(QStringLiteral("time"));
        if (xIndex < 0)
            xIndex = 0;
        xAxisCombo->setCurrentIndex(xIndex);

        auto *ySignalsList = new QListWidget;
        ySignalsList->setMinimumHeight(130);
        ySignalsList->setSelectionMode(QAbstractItemView::NoSelection);
        ySignalsList->setAccessibleName(QStringLiteral("Y axis signals"));
        const auto selectedYSignal = [&](const QString &data) {
            for (const auto &signal : currentGraph.ySignals) {
                const auto current = QString::fromStdString(signal);
                if (current.compare(data, Qt::CaseInsensitive) == 0)
                    return true;
                if (data == QStringLiteral("time") &&
                    current.compare(QStringLiteral("Time (s)"), Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        };
        const auto addYSignal = [&](const QString &label, const QString &data) {
            auto *item = new QListWidgetItem(label, ySignalsList);
            item->setData(Qt::UserRole, data);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(selectedYSignal(data) ? Qt::Checked : Qt::Unchecked);
        };
        addYSignal(QStringLiteral("Time (s)"), QStringLiteral("time"));
        for (const auto &signal : capturedSignals)
            addYSignal(signal, signal);
        bool anyYChecked = false;
        for (int index = 0; index < ySignalsList->count(); ++index)
            anyYChecked |= ySignalsList->item(index)->checkState() == Qt::Checked;
        if (!anyYChecked && ySignalsList->count() > 0)
            ySignalsList->item(ySignalsList->count() > 1 ? 1 : 0)->setCheckState(Qt::Checked);

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
        form->addRow(QStringLiteral("X axis:"), xAxisCombo);
        form->addRow(QStringLiteral("Y axis signals:"), ySignalsList);
        form->addRow(QStringLiteral("X limits:"), makeLimitRow(xMinEdit, xMaxEdit));
        form->addRow(QStringLiteral("Y limits:"), makeLimitRow(yMinEdit, yMaxEdit));
        dlgLayout->addLayout(form);
        auto *limitsHint = new QLabel(
            QStringLiteral("X selects one captured signal or time. Check one or more Y signals; they share the X "
                           "axis. Leave a limit blank to auto-scale that axis. Limits only change how the graph is "
                           "drawn; they do not affect simulation parameters or data acquisition."));
        limitsHint->setWordWrap(true);
        dlgLayout->addWidget(limitsHint);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        dlgLayout->addWidget(buttons);

        std::optional<double> newXMin;
        std::optional<double> newXMax;
        std::optional<double> newYMin;
        std::optional<double> newYMax;
        std::vector<std::string> newYSignals;
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
            newYSignals.clear();
            for (int index = 0; index < ySignalsList->count(); ++index) {
                const auto *item = ySignalsList->item(index);
                if (item->checkState() != Qt::Checked)
                    continue;
                const auto signal = item->data(Qt::UserRole).toString().toStdString();
                if (!signal.empty() && std::find(newYSignals.begin(), newYSignals.end(), signal) == newYSignals.end())
                    newYSignals.push_back(signal);
            }
            if (newYSignals.empty()) {
                QMessageBox::warning(&dlg, QStringLiteral("Y axis signals"),
                                     QStringLiteral("Select at least one Y axis signal."));
                return;
            }
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
        const auto newXAxis = xAxisCombo->currentData().toString();

        project_->execute("edit graph", [simId, graphId, newName, newXAxis, newYSignals, newXMin, newXMax, newYMin,
                                         newYMax](ice::Project &project) {
            auto it = project.simulations.find(simId);
            if (it == project.simulations.end())
                return;
            for (auto &g : it->second.graphs) {
                if (g.id != graphId)
                    continue;
                g.name = newName.toStdString();
                g.xAxis = newXAxis.toStdString();
                g.ySignals = newYSignals;
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
        request.use_initial_conditions = sim.use_initial_conditions;
        // The circuit comes from the schematic the simulation points at, so
        // Run simulates what you drew instead of hand-typed directives. The
        // additional directives box is reserved for model cards / .include
        // lines.
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
        for (const auto &line : additionalDirectives_->text().split('\n')) {
            const auto trimmed = line.trimmed();
            // Skip analysis/control directives the runner already handles from
            // the simulation properties; keep model cards and includes.
            if (trimmed.isEmpty() || isAnalysisControlDirective(trimmed))
                continue;
            request.netlist.push_back(trimmed.toStdString());
        }
        // The captured-signal list tells the adapter which vectors to fetch
        // back from ngspice. Graph axis selections then pick from those traces.
        for (const auto &signal : parseSignalList(QString::fromStdString(sim.captureSignals)))
            request.probeSignals.push_back(signal.toStdString());

        return request;
    }

    ice::ObjectId selectedSimulationSchematicId() const {
        if (!schematicSelector_)
            return 0;
        return static_cast<ice::ObjectId>(schematicSelector_->currentData().toULongLong());
    }

    ice::SimulationItem currentSimulationFormItem() const {
        ice::SimulationItem sim;
        sim.schematic = selectedSimulationSchematicId();
        sim.simulator = simulatorSelector_->currentData().toString().toStdString();
        sim.analysis = analysisSelector_->currentData().toString().toStdString();
        sim.stop = stopTime_->value();
        sim.step = stepTime_->value();
        sim.use_initial_conditions = useInitialConditions_->isChecked();
        sim.directives = additionalDirectives_->text().toStdString();
        sim.captureSignals = signalsCapture_->text().toStdString();
        return sim;
    }

    QString simulationNetlistText() {
        if (!project_ || selectedSimulationSchematicId() == 0)
            return {};
        const auto sim = currentSimulationFormItem();
        QStringList notes;
        try {
            const auto request = buildSimulationRequest(sim, notes);
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
            return text.isEmpty() ? QStringLiteral("* Empty netlist") : text;
        } catch (const ice::ProjectError &error) {
            return QString::fromStdString(error.what());
        }
    }

    void updateSimulationBuildButton() {
        const auto schematicId = selectedSimulationSchematicId();
        const bool hasSimulation = project_ && activeSimulationId_ != 0;
        const bool hasSchematic = project_ && schematicId != 0;
        const bool upToDate = builtSimulationNetlistSchematicId_ == schematicId &&
                              builtSimulationNetlist_ == currentSimulationNetlist_ &&
                              builtSimulationNetlistSchematicId_ != 0;
        if (simulationBuildNetlistButton_)
            simulationBuildNetlistButton_->setEnabled(hasSimulation && hasSchematic && !upToDate);
        if (simulationRunButton_)
            simulationRunButton_->setEnabled(hasSimulation && hasSchematic && upToDate);
    }

    void updateSimulationControlStates() {
        const bool hasSimulation = project_ && activeSimulationId_ != 0;
        const auto setPropertyEnabled = [hasSimulation](QWidget *widget) {
            if (widget)
                widget->setEnabled(hasSimulation);
        };
        setPropertyEnabled(schematicSelector_);
        setPropertyEnabled(simulatorSelector_);
        setPropertyEnabled(analysisSelector_);
        setPropertyEnabled(stopTime_);
        setPropertyEnabled(stepTime_);
        setPropertyEnabled(useInitialConditions_);
        setPropertyEnabled(analysisControl_);
        setPropertyEnabled(signalsCapture_);
        setPropertyEnabled(additionalDirectives_);
        if (signalSelectButton_)
            signalSelectButton_->setEnabled(hasSimulation);
        if (simulationAddGraphButton_)
            simulationAddGraphButton_->setEnabled(hasSimulation);
        if (simulationDeleteGraphButton_)
            simulationDeleteGraphButton_->setEnabled(hasSimulation);
        if (simulationEditGraphButton_)
            simulationEditGraphButton_->setEnabled(hasSimulation);
        if (simulationGridButton_)
            simulationGridButton_->setEnabled(hasSimulation);
        updateSimulationBuildButton();
    }

    void updateSimulationNetlistState() {
        if (!simulationNetlist_)
            return;
        if (!project_ || selectedSimulationSchematicId() == 0) {
            currentSimulationNetlist_.clear();
            builtSimulationNetlist_.clear();
            builtSimulationNetlistSchematicId_ = 0;
            simulationNetlist_->clear();
            updateSimulationControlStates();
            return;
        }
        currentSimulationNetlist_ = simulationNetlistText();
        updateSimulationControlStates();
    }

    void buildSimulationNetlist() {
        const auto schematicId = selectedSimulationSchematicId();
        if (!simulationNetlist_ || !project_ || activeSimulationId_ == 0 || schematicId == 0)
            return;
        simulationNetlist_->setPlainText(currentSimulationNetlist_);
        builtSimulationNetlistSchematicId_ = schematicId;
        builtSimulationNetlist_ = currentSimulationNetlist_;
        updateSimulationBuildButton();
        statusBar()->showMessage(QStringLiteral("Simulation netlist built"));
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
        if (activeSimulationId_ == 0) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        if (selectedSimulationSchematicId() == 0) {
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a schematic first."));
            return;
        }
        const auto simIt = project_->project().simulations.find(activeSimulationId_);
        if (simIt == project_->project().simulations.end()) {
            activeSimulationId_ = 0;
            QMessageBox::information(this, QStringLiteral("Run Simulation"),
                                     QStringLiteral("Select a simulation first."));
            return;
        }
        const auto formSim = currentSimulationFormItem();
        // Honor edits currently visible in the form while retaining the
        // selected simulation's graphs.
        ice::SimulationItem sim = simIt->second;
        sim.schematic = formSim.schematic;
        sim.simulator = formSim.simulator;
        sim.analysis = formSim.analysis;
        sim.stop = formSim.stop;
        sim.step = formSim.step;
        sim.use_initial_conditions = formSim.use_initial_conditions;
        sim.directives = formSim.directives;
        sim.captureSignals = formSim.captureSignals;
        simulationOutput_->setPlainText(QStringLiteral("Running simulation..."));
        documents_->setCurrentWidget(simulationPage_);
        if (simulationTabs_ && simulationGraphsPage_)
            simulationTabs_->setCurrentWidget(simulationGraphsPage_);
        QApplication::processEvents();

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

    void ensureDrcProcess() {
        if (drcProcess_)
            return;
        drcProcess_ = new QProcess(this);
        drcProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(drcProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            drcLog_ += QString::fromLocal8Bit(drcProcess_->readAllStandardOutput());
            constexpr int maximumLogSize = 200000;
            if (drcLog_.size() > maximumLogSize)
                drcLog_ = drcLog_.right(maximumLogSize);
        });
        connect(drcProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            const auto message = QStringLiteral("Unable to start gdscheck: %1").arg(drcProcess_->errorString());
            if (activityPanel_)
                activityPanel_->setDrcError(message);
            statusBar()->showMessage(QStringLiteral("DRC failed: gdscheck could not be started"));
        });
        connect(drcProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int exitCode, QProcess::ExitStatus status) { finishDrc(exitCode, status); });
    }

    void runDrc() {
        if (!project_) {
            QMessageBox::information(this, QStringLiteral("Run DRC"),
                                     QStringLiteral("Create or open a project first."));
            return;
        }
        if (drcProcess_ && drcProcess_->state() != QProcess::NotRunning) {
            statusBar()->showMessage(QStringLiteral("DRC is already running"));
            return;
        }
        const auto &project = project_->project();
        if (project.layoutCells.empty()) {
            QMessageBox::information(this, QStringLiteral("Run DRC"),
                                     QStringLiteral("Create or import a layout cell first."));
            return;
        }
        auto cellId = activeLayoutId_;
        if (!project.layoutCells.contains(cellId))
            cellId = project.layoutCells.begin()->first;
        const auto &cell = project.layoutCells.at(cellId);
        const auto processName = ice::gdsCheckProcessForPdk(cell.pdk);
        if (processName.empty()) {
            QMessageBox::warning(
                this, QStringLiteral("Run DRC"),
                QStringLiteral("The active layout cell uses the Generic PDK. gdscheck requires a PDK process deck. "
                               "Create the layout with an installed PDK such as IHP SG13G2 first."));
            return;
        }

        auto temporary = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/ice-drc-XXXXXX"));
        if (!temporary->isValid()) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"),
                                  QStringLiteral("Unable to create a temporary directory for the DRC run."));
            return;
        }
        const auto gdsPath = temporary->filePath(QStringLiteral("layout.gds"));
        const auto reportPath = temporary->filePath(QStringLiteral("drc.lyrdb"));
        const bool flattened = flattenedLayouts_->contains(cellId);
        try {
            if (flattened)
                ice::LayoutInterchange::exportFlattenedGds(project, cellId, gdsPath.toStdString());
            else
                ice::LayoutInterchange::exportGds(project, cellId, gdsPath.toStdString());
        } catch (const ice::ProjectError &error) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"), QString::fromStdString(error.what()));
            return;
        } catch (const std::exception &error) {
            QMessageBox::critical(this, QStringLiteral("Run DRC"), QString::fromUtf8(error.what()));
            return;
        }

        ensureDrcProcess();
        drcTempDir_ = std::move(temporary);
        drcReportPath_ = reportPath;
        drcLog_.clear();
        const ice::GdsCheckOptions options{gdsPath.toStdString(), reportPath.toStdString(), processName, "TOP", "main"};
        QStringList arguments;
        for (const auto &argument : ice::gdsCheckArguments(options))
            arguments << QString::fromStdString(argument);
        drcProcess_->setProgram(QStringLiteral("gdscheck"));
        drcProcess_->setArguments(arguments);
        drcProcess_->setWorkingDirectory(drcTempDir_->path());

        if (activityDock_)
            activityDock_->show();
        if (activityPanel_) {
            activityPanel_->setDrcRunning(QStringLiteral("Running gdscheck for %1 with process %2%3...")
                                              .arg(QString::fromStdString(cell.name),
                                                   QString::fromStdString(processName),
                                                   flattened ? QStringLiteral(" (flattened)") : QString()));
            activityPanel_->showDrcTab();
        }
        statusBar()->showMessage(QStringLiteral("Running DRC with gdscheck..."));
        drcProcess_->start();
    }

    void finishDrc(int exitCode, QProcess::ExitStatus status) {
        const bool reportExists = !drcReportPath_.isEmpty() && QFileInfo::exists(drcReportPath_);
        if (reportExists) {
            try {
                const auto report = ice::parseGdsCheckLyrdb(drcReportPath_.toStdString());
                if (activityPanel_)
                    activityPanel_->setDrcReport(report, drcReportPath_);
                if (activityDock_)
                    activityDock_->show();
                if (activityPanel_)
                    activityPanel_->showDrcTab();
                const auto violations = report.drc.size();
                statusBar()->showMessage(violations == 0
                                             ? QStringLiteral("DRC finished: no violations")
                                             : QStringLiteral("DRC finished: %1 violation(s)").arg(violations));
                return;
            } catch (const ice::ProjectError &error) {
                const auto message =
                    QStringLiteral("Unable to parse the gdscheck report: %1").arg(QString::fromStdString(error.what()));
                if (activityPanel_)
                    activityPanel_->setDrcError(message);
                statusBar()->showMessage(QStringLiteral("DRC report parsing failed"));
                return;
            }
        }

        auto details = drcLog_.trimmed();
        if (details.size() > 2000)
            details = details.right(2000);
        if (details.isEmpty())
            details = QStringLiteral("gdscheck did not generate a report.");
        if (status == QProcess::CrashExit)
            details.prepend(QStringLiteral("gdscheck crashed. "));
        const auto message = QStringLiteral("DRC failed (exit code %1): %2").arg(exitCode).arg(details);
        if (activityPanel_)
            activityPanel_->setDrcError(message);
        if (activityDock_)
            activityDock_->show();
        if (activityPanel_)
            activityPanel_->showDrcTab();
        statusBar()->showMessage(QStringLiteral("DRC failed"));
    }

    void refreshProject() {
        if (!project_)
            return;
        refreshSimulationSchematics();
        refreshEmbeddedPanels();
        refreshSchematic();
        refreshLayout();
        updateHistoryPanels();
        updateSimulationNetlistState();
    }

    struct SectionHistoryEntry {
        QString label;
        ice::Project project;
    };

    int historyScope(const QString &label) const {
        const auto lower = label.toLower();
        const bool layout = lower.contains(QStringLiteral("layout")) || lower.contains(QStringLiteral("layer"));
        const bool schematic = !layout && (lower.contains(QStringLiteral("schematic")) ||
                               lower.contains(QStringLiteral("symbol")) || lower.contains(QStringLiteral("net")) ||
                               lower.contains(QStringLiteral("pin")) || lower.contains(QStringLiteral("label")) ||
                               lower.contains(QStringLiteral("parameter")) || lower.contains(QStringLiteral("rotat")) ||
                               lower.contains(QStringLiteral("flip")) || lower.contains(QStringLiteral("mov")) ||
                               lower.contains(QStringLiteral("delet")) || lower.contains(QStringLiteral("connect")) ||
                               lower.startsWith(QStringLiteral("placed ")));
        return (layout ? 2 : 0) | (schematic || lower.contains(QStringLiteral("pdk technology")) ? 1 : 0);
    }

    void resetSectionHistories() {
        schematicHistory_.clear();
        layoutHistory_.clear();
        schematicHistoryPosition_ = 0;
        layoutHistoryPosition_ = 0;
        syncedGlobalHistoryPosition_ = project_ ? project_->historyPosition() : 0;
        const auto snapshot = project_ ? project_->project() : ice::Project{};
        schematicHistory_.push_back({QStringLiteral("Initial state"), snapshot});
        layoutHistory_.push_back({QStringLiteral("Initial state"), snapshot});
    }

    void syncSectionHistories() {
        if (!project_)
            return;
        const auto globalPosition = project_->historyPosition();
        if (globalPosition < syncedGlobalHistoryPosition_) {
            resetSectionHistories();
            return;
        }
        const auto &history = project_->history();
        while (syncedGlobalHistoryPosition_ < globalPosition &&
               syncedGlobalHistoryPosition_ < history.size()) {
            const auto label = QString::fromStdString(history[syncedGlobalHistoryPosition_]);
            const auto scope = historyScope(label);
            if (scope & 1) {
                schematicHistory_.resize(schematicHistoryPosition_ + 1);
                schematicHistory_.push_back({label, project_->project()});
                ++schematicHistoryPosition_;
            }
            if (scope & 2) {
                layoutHistory_.resize(layoutHistoryPosition_ + 1);
                layoutHistory_.push_back({label, project_->project()});
                ++layoutHistoryPosition_;
            }
            ++syncedGlobalHistoryPosition_;
        }
    }

    void restoreSectionHistory(bool schematic, std::size_t position) {
        if (!project_)
            return;
        auto restored = project_->project();
        const auto &entries = schematic ? schematicHistory_ : layoutHistory_;
        if (position >= entries.size())
            return;
        if (schematic)
            restored.schematicCells = entries[position].project.schematicCells;
        else {
            restored.layers = entries[position].project.layers;
            restored.layoutCells = entries[position].project.layoutCells;
        }
        project_->replaceProject(std::move(restored));
        if (schematic)
            schematicHistoryPosition_ = position;
        else
            layoutHistoryPosition_ = position;
        refreshProject();
    }

    void undoCurrentSection() {
        syncSectionHistories();
        const bool schematic = !documents_ || documents_->currentWidget() == schematicPage_;
        auto &position = schematic ? schematicHistoryPosition_ : layoutHistoryPosition_;
        if (position > 0)
            restoreSectionHistory(schematic, position - 1);
    }

    void redoCurrentSection() {
        syncSectionHistories();
        const bool schematic = !documents_ || documents_->currentWidget() == schematicPage_;
        const auto &entries = schematic ? schematicHistory_ : layoutHistory_;
        auto &position = schematic ? schematicHistoryPosition_ : layoutHistoryPosition_;
        if (position + 1 < entries.size())
            restoreSectionHistory(schematic, position + 1);
    }

    void updateHistoryPanels() {
        if (!schematicHistoryPanel_ || !layoutHistoryPanel_)
            return;
        syncSectionHistories();
        QStringList schematic;
        QStringList layout;
        std::vector<std::size_t> schematicPositions;
        std::vector<std::size_t> layoutPositions;
        for (std::size_t index = 1; index <= schematicHistoryPosition_; ++index) {
            schematic.append(schematicHistory_[index].label);
            schematicPositions.push_back(index);
        }
        for (std::size_t index = 1; index <= layoutHistoryPosition_; ++index) {
            layout.append(layoutHistory_[index].label);
            layoutPositions.push_back(index);
        }
        schematicHistoryPanel_->setHistory(schematic, schematicPositions);
        layoutHistoryPanel_->setHistory(layout, layoutPositions);
    }

    void jumpToSchematicHistory(std::size_t position) { restoreSectionHistory(true, position); }
    void jumpToLayoutHistory(std::size_t position) { restoreSectionHistory(false, position); }

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
                if (cell.library && !openLibraryLayoutCellTabs_.contains(id))
                    continue;
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
        const auto selected = schematicSelector_->currentData().toULongLong();
        QSignalBlocker blocker(schematicSelector_);
        schematicSelector_->clear();
        if (project_) {
            for (const auto &[id, cell] : project_->project().schematicCells)
                schematicSelector_->addItem(QString::fromStdString(cell.name), QVariant::fromValue<qulonglong>(id));
        }
        const int index = schematicSelector_->findData(QVariant::fromValue<qulonglong>(selected));
        if (index >= 0)
            schematicSelector_->setCurrentIndex(index);
        updateSimulationNetlistState();
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
        if (!project_ || project_->project().schematicCells.empty()) {
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clear();
            if (schematicNetlistPanel_)
                schematicNetlistPanel_->clear();
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
        std::map<ice::ObjectId, ice::ObjectId> pinNetIds;
        for (const auto &[netId, net] : cell.nets)
            for (const auto pinId : net.pins)
                pinNetIds[pinId] = netId;
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
            case ice::Orientation::mx:
                group->setTransform(QTransform(1, 0, 0, -1, 0, 0));
                break;
            case ice::Orientation::my:
                group->setTransform(QTransform(-1, 0, 0, 1, 0, 0));
                break;
            case ice::Orientation::m45:
                group->setTransform(QTransform(0, 1, 1, 0, 0, 0));
                break;
            case ice::Orientation::m135:
                group->setTransform(QTransform(0, -1, -1, 0, 0, 0));
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
            if (symbolIt->second.label) {
                const auto bounds = label->boundingRect();
                label->setPos(symbolIt->second.label->x - bounds.width() / 2.0,
                              -symbolIt->second.label->y - bounds.height() / 2.0);
            } else {
                label->setPos(-artworkSize.width() / 2.0, artworkSize.height() / 2.0 + 6);
            }
            label->setData(0, QStringLiteral("symbol-label"));
            for (const auto &pin : symbolIt->second.pins) {
                auto *pinItem = new QGraphicsEllipseItem(QRectF(-5, -5, 10, 10), group);
                pinItem->setPen(QPen(ice::uiColor(ice::UiColor::Axis1)));
                pinItem->setBrush(QBrush(ice::uiColor(ice::UiColor::Accent1)));
                pinItem->setFlag(QGraphicsItem::ItemIsSelectable, false);
                pinItem->setData(0, QStringLiteral("pin"));
                pinItem->setData(1, QVariant::fromValue<qulonglong>(pin.id));
                pinItem->setData(2, static_cast<int>(rotateDirection(pin.direction, instance.orientation)));
                if (const auto net = pinNetIds.find(pin.id); net != pinNetIds.end())
                    pinItem->setData(3, QVariant::fromValue<qulonglong>(net->second));
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
            const auto root = pinViews.find(net.pins.front());
            if (root == pinViews.end())
                continue;
            // A net is a tree rooted at its first pin. Route one branch to
            // every other member so multi-pin nets remain fully visible.
            for (std::size_t index = 1; index < net.pins.size(); ++index) {
                const auto target = pinViews.find(net.pins[index]);
                if (target == pinViews.end())
                    continue;
                auto path = routeWire(root->second.point, root->second.direction, target->second.point,
                                      target->second.direction, scene, net.pins.front(), net.pins[index]);
                auto *wire = scene->addPath(path, QPen(ice::uiColor(ice::UiColor::Accent4), 2));
                wire->setZValue(-1);
                wire->setData(0, QStringLiteral("wire"));
                wire->setData(1, QVariant::fromValue<qulonglong>(netId));
            }
        }
        // Selection is view state over the domain model, so re-apply it after
        // the scene has been rebuilt from the database.
        schematicCanvas_->restoreSelection();
        updateSelectionStatus();
        schematicCanvas_->refreshPinVisibility();
    }

    static QRectF symbolObstacleRect(QGraphicsItem *symbol) {
        for (auto *child : symbol->childItems())
            if (child->data(0).toString() == QStringLiteral("symbol-artwork"))
                return child->sceneBoundingRect();
        return symbol->sceneBoundingRect();
    }

    QPainterPath routeWire(QPointF start, ice::PinDirection startDirection, QPointF target,
                           ice::PinDirection targetDirection, QGraphicsScene *scene, ice::ObjectId startPin,
                           ice::ObjectId targetPin) const {
        constexpr qreal minimumExtension = 30.0;
        const auto startVector = directionVector(startDirection);
        const auto targetVector = directionVector(targetDirection);
        const QPointF startExit = start + startVector * minimumExtension;
        const QPointF targetExit = target + targetVector * minimumExtension;

        ice::SchematicRouteRequest request;
        request.start = {start.x(), start.y()};
        request.startExit = {startExit.x(), startExit.y()};
        request.target = {target.x(), target.y()};
        request.targetExit = {targetExit.x(), targetExit.y()};
        request.clearance = 2.0;

        for (auto *item : scene->items()) {
            const auto kind = item->data(0).toString();
            if (kind == QStringLiteral("symbol")) {
                const auto rect = symbolObstacleRect(item);
                if (rect.isValid() && !rect.isEmpty())
                    request.obstacleRects.push_back({rect.left(), rect.top(), rect.right(), rect.bottom()});
            } else if (kind == QStringLiteral("pin")) {
                const auto pinId = item->data(1).toULongLong();
                if (pinId == startPin || pinId == targetPin)
                    continue;
                const auto position = item->scenePos();
                const auto direction = static_cast<ice::PinDirection>(item->data(2).toInt());
                const auto exit = position + directionVector(direction) * minimumExtension;
                request.obstacleSegments.push_back({{position.x(), position.y()}, {exit.x(), exit.y()}});
            }
        }

        // A minimum-extension point must remain reachable even when projected
        // artwork bounds extend slightly past the pin.
        const auto contains = [](const ice::RouteRect &rect, const ice::RoutePoint &point) {
            return point.x > std::min(rect.left, rect.right) && point.x < std::max(rect.left, rect.right) &&
                   point.y > std::min(rect.top, rect.bottom) && point.y < std::max(rect.top, rect.bottom);
        };
        std::erase_if(request.obstacleRects, [&](const ice::RouteRect &rect) {
            return contains(rect, request.startExit) || contains(rect, request.targetExit);
        });

        const auto route = ice::routeSchematicWire(request);
        QPainterPath path;
        if (route.points.empty()) {
            path.moveTo(start);
            path.lineTo(target);
            return path;
        }
        path.moveTo(route.points.front().x, route.points.front().y);
        for (std::size_t index = 1; index < route.points.size(); ++index)
            path.lineTo(route.points[index].x, route.points[index].y);
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
        auto *undoAction = addCommand(QStringLiteral("Undo"), QKeySequence(QStringLiteral("Ctrl+Z")));
        auto *redoAction = addCommand(QStringLiteral("Redo"), QKeySequence(QStringLiteral("Ctrl+Y")));
        editMenu->addAction(undoAction);
        editMenu->addAction(redoAction);
        connect(undoAction, &QAction::triggered, this, [this] { undoCurrentSection(); });
        connect(redoAction, &QAction::triggered, this, [this] { redoCurrentSection(); });
        editMenu->addSeparator();
        editMenu->addAction(addCommand(QStringLiteral("Cut"), QKeySequence::Cut));
        editMenu->addAction(addCommand(QStringLiteral("Copy"), QKeySequence::Copy));
        editMenu->addAction(addCommand(QStringLiteral("Paste"), QKeySequence::Paste));
        // A shortcut is deliberately omitted here: the properties editor is a
        // modal dialog, so a window level shortcut would steal Return from the
        // line edits inside it.
        auto *propertiesAction = addCommand(QStringLiteral("Device Properties"));
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
        auto *placeSymbolAction = addCommand(QStringLiteral("Place Device"), QKeySequence(QStringLiteral("P")));
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
        auto *drcAction = addCommand(QStringLiteral("Run DRC"));
        toolsMenu->addAction(drcAction);
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
        connect(drcAction, &QAction::triggered, this, &DocumentWindow::runDrc);
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
        projectPanel_->setLayoutFlattenState(flattenedLayouts_);
        projectPanel_->setOnFlattenChanged([this] { refreshEmbeddedPanels(); });
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
        activityPanel_ = new ActivityPanel;
        activityDock_->setWidget(activityPanel_);
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

    void activateSchematicPlaceSymbolTool(const SymbolDefinition &definition, bool clearPanelSelection = false) {
        if (schematicPlaceSymbolTool_)
            schematicPlaceSymbolTool_->setChecked(true);
        if (schematicLeftTabs_ && schematicSymbolsPanel_)
            schematicLeftTabs_->setCurrentWidget(schematicSymbolsPanel_);
        selectedSchematicSymbol_ = definition;
        if (clearPanelSelection && schematicSymbolsPanel_)
            schematicSymbolsPanel_->selectSymbol(definition.name);
        if (schematicCanvas_) {
            schematicCanvas_->setPlaceSymbolPreview(definition.svg.toUtf8());
            schematicCanvas_->setNetLabelMode(false);
            schematicCanvas_->setPlaceSymbolMode(true);
        }
        if (toolEscapeShortcut_)
            toolEscapeShortcut_->setEnabled(true);
        statusBar()->showMessage(
            QStringLiteral("Select a device in the Devices tab, then click in the schematic to place it"));
    }

    void activateSchematicPlaceSymbolTool() {
        const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
        if (symbol)
            activateSchematicPlaceSymbolTool(*symbol);
    }

    void selectSchematicTool() {
        if (schematicSelectTool_)
            schematicSelectTool_->setChecked(true);
        if (schematicCanvas_) {
            schematicCanvas_->setSignalPickMode(false);
            schematicCanvas_->setPlaceSymbolMode(false);
            schematicCanvas_->setNetLabelMode(false);
        }
        if (schematicSymbolsPanel_)
            schematicSymbolsPanel_->clearSymbolSelection();
        selectedSchematicSymbol_.reset();
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
        if (layoutOptionsColumn_)
            layoutOptionsColumn_->setVisible(hasLayouts);

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
            if (!hasLayouts) {
                layoutEmptyPanel_->refreshPdks(configuredPackageInstallDirectory());
                if (!showingEmpty)
                    layoutEmptyPanel_->prepareForDisplay();
            }
        }
    }

    void createDocuments() {
        documents_ = new QTabWidget;
        auto *documents = documents_;
        documents->setTabsClosable(false);
        auto *schematicPage = new QWidget;
        schematicPage_ = schematicPage;
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
        schematicHierarchy_->setOnMakeLayoutFromSchematic([this](ice::ObjectId id) { makeLayoutFromSchematic(id); });
        schematicSymbolsPanel_ = new SymbolsPanel;
        schematicSymbolsPanel_->setOnSelectionChanged([this] {
            if (!schematicCanvas_)
                return;
            const auto *symbol = schematicSymbolsPanel_ ? schematicSymbolsPanel_->selectedSymbol() : nullptr;
            if (symbol)
                selectedSchematicSymbol_ = *symbol;
            schematicCanvas_->setPlaceSymbolPreview(symbol ? symbol->svg.toUtf8() : QByteArray());
        });
        schematicSymbolsPanel_->setOnSymbolSelected([this] { activateSchematicPlaceSymbolTool(); });
        schematicLeftTabs_ = new QTabWidget;
        schematicLeftTabs_->addTab(schematicHierarchy_, QStringLiteral("Hierarchy"));
        schematicLeftTabs_->addTab(schematicSymbolsPanel_, QStringLiteral("Devices"));
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
        schematicHistoryPanel_ = new HistoryPanel;
        schematicHistoryPanel_->setOnHistorySelected([this](std::size_t position) {
            jumpToSchematicHistory(position);
        });
        schematicProperties->addTab(schematicHistoryPanel_, QStringLiteral("History"));
        schematicContentLayout->addWidget(schematicProperties, 0);
        schematicLayout->addWidget(schematicContent, 1);
        documents->addTab(schematicPage, QStringLiteral("Schematic"));

        auto *layoutPage = new QWidget;
        layoutPage_ = layoutPage;
        auto *layoutLayout = new QVBoxLayout(layoutPage);
        layoutLayout->setContentsMargins(0, 0, 0, 0);
        layoutLayout->setSpacing(0);
        layoutCellTabs_ = new QTabWidget;
        layoutCellTabs_->setDocumentMode(true);
        auto *layoutContent = new QWidget;
        auto *layoutContentLayout = new QHBoxLayout(layoutContent);
        layoutHierarchy_ = new ProjectPanel(false, false, true, true);
        layoutHierarchy_->setLayoutFlattenState(flattenedLayouts_);
        layoutHierarchy_->setOnFlattenChanged([this] { refreshEmbeddedPanels(); });
        layoutHierarchy_->setOnAddClicked([this] { createLayoutCell(); });
        layoutHierarchy_->setOnRemoveClicked([this](ice::ObjectId id) { removeLayoutCell(id); });
        layoutLeftTabs_ = new QTabWidget;
        layoutLeftTabs_->addTab(layoutHierarchy_, QStringLiteral("Hierarchy"));
        layoutDevicesPanel_ = new LayoutDevicesPanel;
        layoutLeftTabs_->addTab(layoutDevicesPanel_, QStringLiteral("Devices"));
        layoutContentLayout->addWidget(layoutLeftTabs_, 0);
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
        layoutEmptyPanel_->setOnCreate(
            [this](const QString &name, const std::string &pdk) { createLayoutCell(name, pdk); });
        layoutEditorStack_ = new QStackedWidget;
        layoutEditorStack_->addWidget(layoutEmptyPanel_);
        layoutEditorStack_->addWidget(layoutEditorPage_);
        layoutContentLayout->addWidget(layoutEditorStack_, 1);
        auto *layoutOptionsToolbar = createLayoutOptionsToolbar();
        layoutOptionsToolbar->setOrientation(Qt::Vertical);
        layoutOptionsColumn_ = new QWidget;
        auto *layoutOptionsLayout = new QVBoxLayout(layoutOptionsColumn_);
        layoutOptionsLayout->setContentsMargins(0, 0, 0, 0);
        layoutOptionsLayout->addStretch(1);
        layoutOptionsLayout->addWidget(layoutOptionsToolbar, 0, Qt::AlignHCenter);
        layoutOptionsLayout->addStretch(1);
        layoutContentLayout->addWidget(layoutOptionsColumn_, 0);
        layoutRightTabs_ = new QTabWidget;
        auto *layoutRightTabs = layoutRightTabs_;
        layoutPropertiesPanel_ = new PropertiesPanel(false);
        layoutPropertiesPanel_->setOnLayoutCellParameterChanged(
            [this](ice::ObjectId cellId, ice::ObjectId parameterId, const std::string &name, const std::string &value) {
                updateLayoutCellParameter(cellId, parameterId, name, value);
            });
        layoutPropertiesPanel_->setOnLayoutDeviceParameterChanged(
            [this](const std::string &deviceName, const std::string &name, const std::string &value) {
                updateLayoutDeviceParameter(deviceName, name, value);
            });
        layoutPropertiesPanel_->setOnPathWidthChanged([this](int width) { setLayoutPathWidth(width); });
        layoutRightTabs->addTab(layoutPropertiesPanel_, QStringLiteral("Properties"));
        layersPanel_ = new LayersPanel;
        layersPanel_->setOnLayerVisibilityChanged([this](ice::ObjectId layerId, bool visible) {
            if (!project_)
                return;
            project_->execute("toggle layer visibility", [&](ice::Project &project) {
                const auto layer = project.layers.find(layerId);
                if (layer == project.layers.end() || layer->second.visible == visible)
                    return;
                layer->second.visible = visible;
            });
            refreshLayout();
        });
        layersPanel_->setOnAllLayersVisibilityChanged([this](bool visible) {
            if (!project_)
                return;
            bool changed = false;
            project_->execute(visible ? "show all layers" : "hide all layers", [&](ice::Project &project) {
                for (auto &[_, layer] : project.layers) {
                    if (layer.visible == visible)
                        continue;
                    layer.visible = visible;
                    changed = true;
                }
            });
            if (!changed)
                return;
            layersPanel_->setProject(project_->project());
            refreshLayout();
        });
        layoutRightTabs->addTab(layersPanel_, QStringLiteral("Layers"));
        layoutHistoryPanel_ = new HistoryPanel;
        layoutHistoryPanel_->setOnHistorySelected([this](std::size_t position) { jumpToLayoutHistory(position); });
        layoutRightTabs->addTab(layoutHistoryPanel_, QStringLiteral("History"));
        layoutContentLayout->addWidget(layoutRightTabs, 0);
        layoutLayout->addWidget(layoutContent, 1);
        documents->addTab(layoutPage, QStringLiteral("Layout"));

        if (layoutDevicesPanel_) {
            layoutDevicesPanel_->setOnDeviceSelected([this](const ice::LayoutDeviceCell *cell) {
                selectedLayoutDevice_ = cell;
                if (layoutCanvas_)
                    layoutCanvas_->setDevicePreview(cell);
                if (layoutPropertiesPanel_ && layoutLeftTabs_ &&
                    layoutLeftTabs_->currentWidget() == layoutDevicesPanel_) {
                    layoutPropertiesPanel_->setLayoutDevice(cell);
                    if (layoutRightTabs_)
                        layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                }
            });
            layoutDevicesPanel_->setOnDeviceActivated([this] {
                if (!selectedLayoutDevice_ || (layoutCanvas_ && layoutCanvas_->isReadOnly()))
                    return;
                if (layoutPlaceDeviceTool_)
                    layoutPlaceDeviceTool_->setChecked(true);
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::placeDevice);
                    layoutCanvas_->setFocus();
                }
                statusBar()->showMessage(QStringLiteral("Click in the layout to place %1")
                                             .arg(QString::fromStdString(selectedLayoutDevice_->name)));
            });
            selectedLayoutDevice_ = layoutDevicesPanel_->selectedDevice();
            if (layoutCanvas_) {
                layoutCanvas_->setDevicePreview(selectedLayoutDevice_);
                layoutCanvas_->setDevicePlacementCallback([this](QPointF position) { placeLayoutDevice(position); });
                layoutCanvas_->setInstanceActivatedCallback(
                    [this](ice::ObjectId instanceId) { openLayoutInstanceCell(instanceId); });
                layoutCanvas_->setInstanceSelectionCallback([this](ice::ObjectId) { refreshLayoutProperties(true); });
                layoutCanvas_->setToolEscapeCallback([this] {
                    if (layoutSelectTool_)
                        layoutSelectTool_->setChecked(true);
                    if (layoutCanvas_)
                        layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                    clearLayoutDeviceSelection();
                    refreshLayoutProperties(true);
                });
            }
        }
        connect(layoutLeftTabs_, &QTabWidget::currentChanged, this, [this](int) {
            if (!layoutLeftTabs_ || !layoutDevicesPanel_)
                return;
            if (layoutLeftTabs_->currentWidget() == layoutDevicesPanel_) {
                refreshLayoutProperties();
                if (layoutRightTabs_)
                    layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
            } else {
                refreshLayout();
            }
        });

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
        auto *propertiesColumns = new QHBoxLayout;
        auto *propertiesColumn = new QVBoxLayout;
        auto *simulationForm = new QFormLayout;
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
        useInitialConditions_ = new QCheckBox(QStringLiteral("Use initial conditions"));
        analysisControl_ = new QLineEdit;
        analysisControl_->setReadOnly(true);
        analysisControl_->setPlaceholderText(QStringLiteral("Generated analysis control line"));
        signalsCapture_ = new QLineEdit;
        signalsCapture_->setClearButtonEnabled(true);
        signalsCapture_->setPlaceholderText(QStringLiteral("e.g. v(out), v(in)  (comma-separated)"));
        signalSelectButton_ = new QPushButton;
        signalSelectButton_->setFixedWidth(32);
        signalSelectButton_->setToolTip(QStringLiteral("Select a node or wire from the schematic"));
        signalSelectButton_->setAccessibleName(QStringLiteral("Select signal from schematic"));
        signalSelectButton_->setEnabled(false);
        setLucideIcon(signalSelectButton_, QStringLiteral("mouse-pointer-2"), 16);
        auto *signalRow = new QWidget;
        auto *signalRowLayout = new QHBoxLayout(signalRow);
        signalRowLayout->setContentsMargins(0, 0, 0, 0);
        signalRowLayout->addWidget(signalsCapture_, 1);
        signalRowLayout->addWidget(signalSelectButton_);
        additionalDirectives_ = new QLineEdit;
        additionalDirectives_->setPlaceholderText(
            QStringLiteral("Additional SPICE directives, for example: .include models.sp"));
        simulationForm->addRow(QStringLiteral("Schematic"), schematicSelector_);
        simulationForm->addRow(QStringLiteral("Simulator"), simulatorSelector_);
        simulationForm->addRow(QStringLiteral("Analysis"), analysisSelector_);
        simulationForm->addRow(QStringLiteral("Stop time / end"), stopTime_);
        simulationForm->addRow(QStringLiteral("Step / increment"), stepTime_);
        simulationForm->addRow(QString(), useInitialConditions_);
        simulationForm->addRow(QStringLiteral("Analysis Control Line"), analysisControl_);
        simulationForm->addRow(QStringLiteral("Signals to capture"), signalRow);
        simulationForm->addRow(QStringLiteral("Additional directives"), additionalDirectives_);
        propertiesColumn->addLayout(simulationForm);
        propertiesColumn->addStretch();

        auto *runButton = new QPushButton(QStringLiteral("Run Simulation"));
        setLucideIcon(runButton, QStringLiteral("play"), 16);
        runButton->setToolTip(QStringLiteral("Run simulation"));
        runButton->setAccessibleName(QStringLiteral("Run simulation"));
        runButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        runButton->setEnabled(false);
        simulationRunButton_ = runButton;
        simulationBuildNetlistButton_ = new QPushButton(QStringLiteral("Build Netlist"));
        setLucideIcon(simulationBuildNetlistButton_, QStringLiteral("refresh-cw"), 16);
        simulationBuildNetlistButton_->setToolTip(QStringLiteral("Generate the netlist for this simulation"));
        simulationBuildNetlistButton_->setAccessibleName(QStringLiteral("Build simulation netlist"));
        simulationBuildNetlistButton_->setEnabled(false);
        simulationBuildNetlistButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *runRow = new QHBoxLayout;
        runRow->addWidget(simulationBuildNetlistButton_, 1);
        runRow->addWidget(runButton, 1);
        propertiesColumn->addLayout(runRow);

        simulationNetlist_ = new QPlainTextEdit;
        simulationNetlist_->setReadOnly(true);
        simulationNetlist_->setLineWrapMode(QPlainTextEdit::NoWrap);
        simulationNetlist_->setPlaceholderText(QStringLiteral("Build the netlist to see it here."));
        simulationOutput_ = new QPlainTextEdit;
        simulationOutput_->setReadOnly(true);
        simulationOutput_->setPlaceholderText(QStringLiteral("Simulation status and result summary"));
        auto *rightSplitter = new QSplitter(Qt::Vertical);
        rightSplitter->setChildrenCollapsible(false);
        rightSplitter->addWidget(simulationNetlist_);
        rightSplitter->addWidget(simulationOutput_);
        rightSplitter->setStretchFactor(0, 1);
        rightSplitter->setStretchFactor(1, 1);
        propertiesColumns->addLayout(propertiesColumn, 1);
        propertiesColumns->addWidget(rightSplitter, 1);
        propertiesLayout->addLayout(propertiesColumns, 1);
        simulationTabs_->addTab(propertiesPage, QStringLiteral("Properties"));

        // Graph tabs area
        auto *graphsPage = new QWidget;
        simulationGraphsPage_ = graphsPage;
        auto *graphsLayout = new QVBoxLayout(graphsPage);
        graphsLayout->setContentsMargins(0, 0, 0, 0);
        auto *graphControls = new QWidget;
        auto *graphControlsLayout = new QHBoxLayout(graphControls);
        graphControlsLayout->setContentsMargins(0, 0, 0, 0);
        graphControlsLayout->setSpacing(2);
        simulationAddGraphButton_ = new QPushButton;
        simulationAddGraphButton_->setToolTip(QStringLiteral("Add graph to this simulation"));
        simulationAddGraphButton_->setAccessibleName(QStringLiteral("Add graph"));
        simulationAddGraphButton_->setFixedWidth(32);
        simulationAddGraphButton_->setEnabled(false);
        setLucideIcon(simulationAddGraphButton_, QStringLiteral("plus"), 16);
        simulationDeleteGraphButton_ = new QPushButton;
        simulationDeleteGraphButton_->setToolTip(QStringLiteral("Delete current graph"));
        simulationDeleteGraphButton_->setAccessibleName(QStringLiteral("Delete current graph"));
        simulationDeleteGraphButton_->setFixedWidth(32);
        simulationDeleteGraphButton_->setEnabled(false);
        setLucideIcon(simulationDeleteGraphButton_, QStringLiteral("trash"), 16);
        simulationEditGraphButton_ = new QPushButton;
        simulationEditGraphButton_->setToolTip(QStringLiteral("Edit graph name, axis selections, and limits"));
        simulationEditGraphButton_->setAccessibleName(QStringLiteral("Edit graph"));
        simulationEditGraphButton_->setFixedWidth(32);
        simulationEditGraphButton_->setEnabled(false);
        setLucideIcon(simulationEditGraphButton_, QStringLiteral("pencil"), 16);
        simulationGridButton_ = new QPushButton;
        simulationGridButton_->setCheckable(true);
        simulationGridButton_->setChecked(true);
        simulationGridButton_->setToolTip(QStringLiteral("Show or hide the graph grid"));
        simulationGridButton_->setAccessibleName(QStringLiteral("Toggle graph grid"));
        simulationGridButton_->setFixedWidth(32);
        simulationGridButton_->setEnabled(false);
        setLucideIcon(simulationGridButton_, QStringLiteral("grid-3x3"), 16);
        graphControlsLayout->addWidget(simulationAddGraphButton_);
        graphControlsLayout->addWidget(simulationDeleteGraphButton_);
        graphControlsLayout->addWidget(simulationEditGraphButton_);
        graphControlsLayout->addWidget(simulationGridButton_);

        graphTabs_ = new QTabWidget;
        graphTabs_->setDocumentMode(true);
        graphTabs_->setTabsClosable(false);
        graphTabs_->setCornerWidget(graphControls, Qt::TopRightCorner);
        simulationPlot_ = new SimulationPlot;
        graphTabs_->addTab(simulationPlot_, QStringLiteral("Graph 1"));
        graphsLayout->addWidget(graphTabs_, 1);
        simulationTabs_->addTab(graphsPage, QStringLiteral("Graphs"));

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
        connect(schematicSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, saveSimProps] {
            saveSimProps();
            updateSimulationNetlistState();
        });
        connect(simulatorSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [saveSimProps] { saveSimProps(); });
        connect(analysisSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stopTime_, &QDoubleSpinBox::editingFinished, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stepTime_, &QDoubleSpinBox::editingFinished, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(stopTime_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { updateAnalysisControlLine(); });
        connect(stepTime_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { updateAnalysisControlLine(); });
        connect(useInitialConditions_, &QCheckBox::toggled, this, [this, saveSimProps] {
            updateAnalysisControlLine();
            saveSimProps();
        });
        connect(signalsCapture_, &QLineEdit::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(signalSelectButton_, &QPushButton::clicked, this, [this] { pickSimulationSignal(); });
        connect(additionalDirectives_, &QLineEdit::editingFinished, this, [saveSimProps] { saveSimProps(); });
        connect(additionalDirectives_, &QLineEdit::textChanged, this,
                [this](const QString &) { updateSimulationNetlistState(); });
        connect(simulationBuildNetlistButton_, &QPushButton::clicked, this, [this] { buildSimulationNetlist(); });
        updateAnalysisControlLine();
        updateSimulationControlStates();

        // ── Wire graph buttons ────────────────────────────────────────────
        connect(simulationAddGraphButton_, &QPushButton::clicked, this, [this] { addGraph(activeSimulationId_); });
        connect(simulationDeleteGraphButton_, &QPushButton::clicked, this, [this] { deleteCurrentGraph(); });
        connect(simulationEditGraphButton_, &QPushButton::clicked, this, [this] { editGraph(); });
        connect(simulationGridButton_, &QPushButton::toggled, this, [this](bool visible) {
            graphGridVisible_ = visible;
            if (!graphTabs_)
                return;
            for (int index = 0; index < graphTabs_->count(); ++index)
                if (auto *plot = dynamic_cast<SimulationPlot *>(graphTabs_->widget(index)))
                    plot->setGridVisible(visible);
        });
        connect(graphTabs_, &QTabWidget::currentChanged, this, [this](int) { refreshGraphPlot(); });

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
        connect(documents_, &QTabWidget::currentChanged, this, [this](int) {
            if (schematicCanvas_ && schematicCanvas_->isSignalPickMode() &&
                documents_->currentWidget() != schematicPage_)
                schematicCanvas_->setSignalPickMode(false);
        });

        connect(runButton, &QPushButton::clicked, this, &DocumentWindow::runSimulation);
        documents->addTab(simulationPage_, QStringLiteral("Simulation"));
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
            schematicCanvas_ = createSchematicCanvas();
            schematicCanvas_->setCallbacks(
                [this](ice::ObjectId id, QPointF position) {
                    if (!project_)
                        return;
                    std::string historyLabel;
                    bool moved = false;
                    for (const auto &[_, cell] : project_->project().schematicCells) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            historyLabel = "Moved " +
                                           (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) +
                                           " " + instance->second.name;
                            break;
                        }
                    }
                    project_->execute("move symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                moved = it->second.x != static_cast<ice::Coordinate>(position.x()) ||
                                        it->second.y != static_cast<ice::Coordinate>(-position.y());
                                it->second.x = static_cast<ice::Coordinate>(position.x());
                                it->second.y = static_cast<ice::Coordinate>(-position.y());
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                    if (moved && !historyLabel.empty())
                        project_->setLastHistoryLabel(std::move(historyLabel));
                },
                [this](ice::ObjectId id) {
                    if (!project_)
                        return;
                    std::string historyLabel;
                    for (const auto &[_, cell] : project_->project().schematicCells) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            historyLabel = "Rotated " +
                                           (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) +
                                           " " + instance->second.name + " - 90°";
                            break;
                        }
                    }
                    project_->execute("rotate symbol", [&](ice::Project &project) {
                        for (auto &[_, cell] : project.schematicCells)
                            if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                                it->second.orientation =
                                    composeOrientation(it->second.orientation, ice::Orientation::r90);
                                updateAutomaticNetLabelPositions(project);
                                return;
                            }
                    });
                    if (!historyLabel.empty())
                        project_->setLastHistoryLabel(std::move(historyLabel));
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
            schematicCanvas_->setDisconnectCallback([this](ice::ObjectId pinId) {
                if (!project_)
                    return;
                auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
                if (cellIt == project_->project().schematicCells.end())
                    cellIt = project_->project().schematicCells.begin();
                if (cellIt == project_->project().schematicCells.end())
                    return;
                const auto cellId = cellIt->first;
                bool changed = false;
                project_->execute("disconnect pin", [&](ice::Project &project) {
                    auto &cell = project.schematicCells.at(cellId);
                    for (auto netIt = cell.nets.begin(); netIt != cell.nets.end();) {
                        auto &pins = netIt->second.pins;
                        const auto before = pins.size();
                        pins.erase(std::remove(pins.begin(), pins.end(), pinId), pins.end());
                        if (pins.size() == before) {
                            ++netIt;
                            continue;
                        }
                        changed = true;
                        if (pins.size() < 2)
                            netIt = cell.nets.erase(netIt);
                        else
                            ++netIt;
                    }
                    if (changed)
                        updateAutomaticNetLabelPositions(project);
                });
                if (changed)
                    refreshProject();
            });
            schematicCanvas_->setNetLabelMovedCallback([this](ice::ObjectId netId, QPointF position) {
                if (!project_)
                    return;
                auto cellIt = project_->project().schematicCells.find(activeSchematicId_);
                if (cellIt == project_->project().schematicCells.end())
                    cellIt = project_->project().schematicCells.begin();
                if (cellIt == project_->project().schematicCells.end())
                    return;
                const auto cellId = cellIt->first;
                bool changed = false;
                project_->execute("move net label", [&](ice::Project &project) {
                    auto &cell = project.schematicCells.at(cellId);
                    auto netIt = cell.nets.find(netId);
                    if (netIt == cell.nets.end())
                        return;
                    const auto x = static_cast<ice::Coordinate>(position.x());
                    const auto y = static_cast<ice::Coordinate>(-position.y());
                    if (netIt->second.x == x && netIt->second.y == y)
                        return;
                    netIt->second.x = x;
                    netIt->second.y = y;
                    changed = true;
                });
                if (changed)
                    refreshProject();
                else
                    refreshSchematic();
            });
            schematicCanvas_->setFlipCallback([this](ice::ObjectId id) {
                if (!project_)
                    return;
                std::string historyLabel;
                for (const auto &[_, cell] : project_->project().schematicCells) {
                    if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                        const auto symbol = cell.symbols.find(instance->second.symbol);
                        historyLabel = "Flipped " +
                                       (symbol == cell.symbols.end() ? std::string("device") : symbol->second.name) +
                                       " " + instance->second.name + " horizontally";
                        break;
                    }
                }
                project_->execute("flip symbol", [&](ice::Project &project) {
                    for (auto &[_, cell] : project.schematicCells)
                        if (auto it = cell.instances.find(id); it != cell.instances.end()) {
                            it->second.orientation = composeOrientation(it->second.orientation, ice::Orientation::my);
                            updateAutomaticNetLabelPositions(project);
                            return;
                        }
                });
                if (!historyLabel.empty())
                    project_->setLastHistoryLabel(std::move(historyLabel));
                refreshProject();
            });
            schematicCanvas_->setSymbolPlacementCallback([this](QPointF position) {
                if (!selectedSchematicSymbol_) {
                    statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
                    return;
                }
                const auto symbolName = selectedSchematicSymbol_->name;
                if (placeSymbolDefinition(*selectedSchematicSymbol_, position))
                    statusBar()->showMessage(QStringLiteral("Placed %1").arg(symbolName));
            });
            schematicCanvas_->setToolEscapeCallback([this] { selectSchematicTool(); });
            schematicCanvas_->setSignalPickedCallback(
                [this](ice::ObjectId netId) { finishSimulationSignalPick(netId); });
            schematicCanvas_->setSignalPickCancelledCallback([this] { cancelSimulationSignalPick(); });
            schematicCanvas_->setDeleteCallback([this](std::vector<ice::ObjectId> ids) {
                if (!project_)
                    return;
                std::vector<std::string> deletedNames;
                for (const auto &[_, cell] : project_->project().schematicCells) {
                    for (const auto id : ids) {
                        if (const auto instance = cell.instances.find(id); instance != cell.instances.end()) {
                            const auto symbol = cell.symbols.find(instance->second.symbol);
                            deletedNames.push_back((symbol == cell.symbols.end() ? std::string("device")
                                                                                  : symbol->second.name) +
                                                    " " + instance->second.name);
                        }
                    }
                }
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
                if (deletedNames.size() == 1)
                    project_->setLastHistoryLabel("Deleted " + deletedNames.front());
                else
                    project_->setLastHistoryLabel("Deleted " + std::to_string(deletedNames.size()) +
                                                  " schematic devices");
                refreshProject();
            });
            view = schematicCanvas_;
        } else {
            layoutCanvas_ = createLayoutCanvas();
            layoutCanvas_->setPathSegmentCallback([this](QPointF start, QPointF end, int width, ice::ObjectId layer,
                                                         const std::vector<ice::ObjectId> &viaLayers) {
                addLayoutPathSegment(start, end, width, layer, viaLayers);
            });
            layoutCanvas_->setPathLayerChangedCallback([this](ice::ObjectId, const std::string &name) {
                statusBar()->showMessage(QStringLiteral("Path layer: %1 · width: %2 nm")
                                             .arg(QString::fromStdString(name))
                                             .arg(layoutPathWidth_));
                if (layoutPropertiesPanel_ && layoutCanvas_ && layoutCanvas_->currentTool() == LayoutCanvas::Tool::path)
                    layoutPropertiesPanel_->setPathTool(layoutCanvas_->pathWidth(), name);
            });
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
                    if (project_->project().layoutCells.at(cellId).library)
                        return;
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
                [this](ice::ObjectId objectId, QPointF delta) {
                    if (!project_)
                        return;
                    project_->execute("move layout object", [&](ice::Project &project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end() || cellIt->second.library)
                            return;
                        if (cellIt->second.shapes.contains(objectId)) {
                            ice::moveRectangle(project, activeLayoutId_, objectId,
                                               static_cast<ice::Coordinate>(delta.x()),
                                               static_cast<ice::Coordinate>(-delta.y()));
                            return;
                        }
                        if (auto planned = cellIt->second.plannedDevices.find(objectId);
                            planned != cellIt->second.plannedDevices.end()) {
                            planned->second.x += static_cast<ice::Coordinate>(delta.x());
                            planned->second.y -= static_cast<ice::Coordinate>(delta.y());
                            return;
                        }
                        if (auto instance = cellIt->second.instances.find(objectId);
                            instance != cellIt->second.instances.end()) {
                            instance->second.x += static_cast<ice::Coordinate>(delta.x());
                            instance->second.y -= static_cast<ice::Coordinate>(delta.y());
                        }
                    });
                    refreshProject();
                },
                [this](std::vector<ice::ObjectId> ids) {
                    if (!project_)
                        return;
                    project_->execute("delete layout objects", [&](ice::Project &project) {
                        auto cellIt = project.layoutCells.find(activeLayoutId_);
                        if (cellIt == project.layoutCells.end() || cellIt->second.library)
                            return;
                        for (const auto id : ids) {
                            if (cellIt->second.plannedDevices.erase(id) != 0) {
                                std::erase_if(cellIt->second.plannedConnections, [&](const auto &entry) {
                                    return entry.second.from == id || entry.second.to == id;
                                });
                                continue;
                            }
                            if (cellIt->second.shapes.erase(id) == 0)
                                cellIt->second.instances.erase(id);
                        }
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
            selectTool->setCheckable(true);
            selectTool->setChecked(true);
            layoutSelectTool_ = selectTool;

            auto *placeSymbolTool = addTool(QStringLiteral("circuit-board"), QStringLiteral("Place Symbol"));
            placeSymbolTool->setCheckable(true);
            layoutPlaceDeviceTool_ = placeSymbolTool;

            auto *rectangleTool = addTool(QStringLiteral("square"), QStringLiteral("Create Rectangle"));
            rectangleTool->setCheckable(true);
            layoutRectangleTool_ = rectangleTool;

            auto *polygonTool = addTool(QStringLiteral("pentagon"), QStringLiteral("Create Polygon"));
            connect(polygonTool, &QAction::triggered, this,
                    [this] { statusBar()->showMessage(QStringLiteral("Create Polygon is not available yet")); });

            auto *pathTool = addTool(QStringLiteral("route"), QStringLiteral("Draw Path"));
            pathTool->setCheckable(true);
            layoutPathTool_ = pathTool;

            auto *textTool = addTool(QStringLiteral("type"), QStringLiteral("Draw Text"));
            connect(textTool, &QAction::triggered, this,
                    [this] { statusBar()->showMessage(QStringLiteral("Draw Text is not available yet")); });

            auto *rulerTool = addTool(QStringLiteral("ruler"), QStringLiteral("Ruler"));
            connect(rulerTool, &QAction::triggered, this,
                    [this] { statusBar()->showMessage(QStringLiteral("Ruler is not available yet")); });

            auto *toolGroup = new QActionGroup(toolbar);
            toolGroup->setExclusive(true);
            for (auto *action : {selectTool, placeSymbolTool, rectangleTool, pathTool})
                toolGroup->addAction(action);

            connect(selectTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_)
                    layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                clearLayoutDeviceSelection();
                refreshLayoutProperties(true);
            });
            connect(placeSymbolTool, &QAction::triggered, this, [this] {
                if (layoutLeftTabs_ && layoutDevicesPanel_)
                    layoutLeftTabs_->setCurrentWidget(layoutDevicesPanel_);
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::placeDevice);
                    layoutCanvas_->setFocus();
                }
                if (!selectedLayoutDevice_)
                    statusBar()->showMessage(QStringLiteral("Select a device in the Devices tab first"));
                else
                    statusBar()->showMessage(QStringLiteral("Click in the layout to place %1")
                                                 .arg(QString::fromStdString(selectedLayoutDevice_->name)));
            });
            connect(rectangleTool, &QAction::triggered, this, [this] {
                if (layoutCanvas_) {
                    layoutCanvas_->setTool(LayoutCanvas::Tool::rectangle);
                    layoutCanvas_->setFocus();
                }
                refreshLayoutProperties(false);
            });
            connect(pathTool, &QAction::triggered, this, [this, pathTool] {
                if (!pathTool->isChecked() || !layoutCanvas_ || layoutCanvas_->isReadOnly())
                    return;
                updateLayoutPathConfig();
                if (!layoutCanvas_->pathAvailable()) {
                    statusBar()->showMessage(QStringLiteral("No layout layers are available for paths"));
                    if (layoutSelectTool_)
                        layoutSelectTool_->setChecked(true);
                    layoutCanvas_->setTool(LayoutCanvas::Tool::select);
                    return;
                }
                layoutCanvas_->setTool(LayoutCanvas::Tool::path);
                layoutCanvas_->setFocus();
                if (layoutRightTabs_)
                    layoutRightTabs_->setCurrentWidget(layoutPropertiesPanel_);
                refreshLayoutProperties(true);
                statusBar()->showMessage(
                    QStringLiteral("Path: %1 · %2 nm · Shift = right angles · hold left + wheel = layer · "
                                   "right-click = finish")
                        .arg(QString::fromStdString(layoutCanvas_->pathLayerName()))
                        .arg(layoutCanvas_->pathWidth()));
            });
            return toolbar;
        }

        selectTool->setCheckable(true);
        schematicSelectTool_ = selectTool;
        auto *placeSymbolTool = addTool(QStringLiteral("circuit-board"), QStringLiteral("Place Device"));
        placeSymbolTool->setCheckable(true);
        schematicPlaceSymbolTool_ = placeSymbolTool;
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
                if (schematicSymbolsPanel_)
                    schematicSymbolsPanel_->clearSymbolSelection();
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
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clearSymbolSelection();
            if (toolEscapeShortcut_)
                toolEscapeShortcut_->setEnabled(false);
        });
        connect(placeSymbolTool, &QAction::triggered, this, [this, placeSymbolTool] {
            if (!placeSymbolTool->isChecked())
                return;
            activateSchematicPlaceSymbolTool();
            if (schematicCanvas_)
                schematicCanvas_->setFocus();
        });
        connect(placeLabelTool, &QAction::triggered, this, [this, placeLabelTool] {
            if (!placeLabelTool->isChecked())
                return;
            if (schematicCanvas_) {
                schematicCanvas_->setPlaceSymbolMode(false);
                schematicCanvas_->setNetLabelMode(true);
                schematicCanvas_->setFocus();
            }
            if (schematicSymbolsPanel_)
                schematicSymbolsPanel_->clearSymbolSelection();
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
        auto *rotateTool = addTool(QStringLiteral("rotate-cw"), QStringLiteral("Rotate"));
        connect(rotateTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->rotateSelectedSymbols();
        });
        auto *flipTool = addTool(QStringLiteral("flip-horizontal-2"), QStringLiteral("Flip Horizontal"));
        connect(flipTool, &QAction::triggered, this, [this] {
            if (schematicCanvas_)
                schematicCanvas_->flipSelectedSymbols();
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

    QToolBar *createLayoutOptionsToolbar() {
        auto *toolbar = new QToolBar(QStringLiteral("Layout options"), this);
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(18, 18));
        toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar->setAccessibleName(QStringLiteral("Layout options toolbar"));

        auto addTool = [toolbar](const QString &iconName, const QString &name) {
            auto *action = toolbar->addAction(name);
            setLucideIcon(action, iconName);
            action->setToolTip(name);
            action->setStatusTip(name);
            return action;
        };

        auto *gridTool = addTool(QStringLiteral("grid-3x3"), QStringLiteral("Toggle Grid"));
        gridTool->setCheckable(true);
        gridTool->setChecked(true);
        connect(gridTool, &QAction::toggled, this, [this](bool checked) {
            if (layoutCanvas_)
                layoutCanvas_->setGridVisible(checked);
        });
        auto *snapTool = addTool(QStringLiteral("magnet"), QStringLiteral("Snap to Grid"));
        snapTool->setCheckable(true);
        snapTool->setChecked(true);
        connect(snapTool, &QAction::toggled, this, [this](bool enabled) {
            if (layoutCanvas_)
                layoutCanvas_->setSnapToGrid(enabled);
        });
        auto *fitTool = addTool(QStringLiteral("scan"), QStringLiteral("Fit to View"));
        connect(fitTool, &QAction::triggered, this, [this] {
            if (layoutCanvas_)
                layoutCanvas_->fitToContent();
        });

        toolbar->addSeparator();
        auto *rotateTool = addTool(QStringLiteral("rotate-cw"), QStringLiteral("Rotate"));
        connect(rotateTool, &QAction::triggered, this, [this] {
            transformSelectedLayoutInstance(ice::Orientation::r90, QStringLiteral("rotate layout device"));
        });
        auto *flipHorizontalTool = addTool(QStringLiteral("flip-horizontal-2"), QStringLiteral("Flip Horizontally"));
        connect(flipHorizontalTool, &QAction::triggered, this, [this] {
            transformSelectedLayoutInstance(ice::Orientation::my, QStringLiteral("flip layout device"));
        });
        auto *flipVerticalTool = addTool(QStringLiteral("flip-vertical-2"), QStringLiteral("Flip Vertically"));
        connect(flipVerticalTool, &QAction::triggered, this, [this] {
            transformSelectedLayoutInstance(ice::Orientation::mx, QStringLiteral("flip layout device"));
        });

        toolbar->addSeparator();
        auto *upTool = addTool(QStringLiteral("arrow-up"), QStringLiteral("Hierarchy Go Up"));
        connect(upTool, &QAction::triggered, this, [this] { openLayoutParentCell(); });
        auto *downTool = addTool(QStringLiteral("arrow-down"), QStringLiteral("Hierarchy Go Down"));
        connect(downTool, &QAction::triggered, this, [this] { openSelectedLayoutCell(); });

        toolbar->addSeparator();
        auto *deleteTool = addTool(QStringLiteral("trash"), QStringLiteral("Delete"));
        layoutDeleteTool_ = deleteTool;
        connect(deleteTool, &QAction::triggered, this, [this] {
            if (layoutCanvas_)
                layoutCanvas_->deleteSelectedShapes();
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
    QWidget *layoutOptionsColumn_ = nullptr;
    ice::ObjectId activeSchematicId_ = 0;
    ice::ObjectId activeLayoutId_ = 0;
    std::set<ice::ObjectId> openLibraryLayoutCellTabs_;
    std::unique_ptr<ice::ProjectSession> project_;
    std::shared_ptr<std::set<ice::ObjectId>> flattenedLayouts_ = std::make_shared<std::set<ice::ObjectId>>();
    QString projectPath_;
    QStackedWidget *workspaceStack_ = nullptr;
    StartupView *startupView_ = nullptr;
    QDockWidget *activityDock_ = nullptr;
    ActivityPanel *activityPanel_ = nullptr;
    QProcess *drcProcess_ = nullptr;
    std::unique_ptr<QTemporaryDir> drcTempDir_;
    QString drcReportPath_;
    QString drcLog_;
    QTabWidget *documents_ = nullptr;
    QWidget *schematicPage_ = nullptr;
    QWidget *layoutPage_ = nullptr;
    SchematicCanvas *schematicCanvas_ = nullptr;
    LayoutCanvas *layoutCanvas_ = nullptr;
    bool layoutViewInitialized_ = false;
    QPlainTextEdit *simulationOutput_ = nullptr;
    QTabWidget *simulationTabs_ = nullptr;
    QPlainTextEdit *simulationNetlist_ = nullptr;
    QWidget *simulationPage_ = nullptr;
    QWidget *simulationGraphsPage_ = nullptr;
    QComboBox *schematicSelector_ = nullptr;
    QComboBox *simulatorSelector_ = nullptr;
    QComboBox *analysisSelector_ = nullptr;
    QDoubleSpinBox *stopTime_ = nullptr;
    QDoubleSpinBox *stepTime_ = nullptr;
    QLineEdit *analysisControl_ = nullptr;
    QLineEdit *signalsCapture_ = nullptr;
    QLineEdit *additionalDirectives_ = nullptr;
    QCheckBox *useInitialConditions_ = nullptr;
    QPushButton *signalSelectButton_ = nullptr;
    QPushButton *simulationBuildNetlistButton_ = nullptr;
    QPushButton *simulationRunButton_ = nullptr;
    QPushButton *simulationAddGraphButton_ = nullptr;
    QPushButton *simulationDeleteGraphButton_ = nullptr;
    QPushButton *simulationEditGraphButton_ = nullptr;
    QPushButton *simulationGridButton_ = nullptr;
    QString currentSimulationNetlist_;
    QString builtSimulationNetlist_;
    ice::ObjectId builtSimulationNetlistSchematicId_ = 0;
    SimulationPlot *simulationPlot_ = nullptr;
    SimulationPanel *simulationPanel_ = nullptr;
    QTabWidget *graphTabs_ = nullptr;
    bool graphGridVisible_ = true;
    ice::ObjectId activeSimulationId_ = 0;
    PropertiesPanel *schematicPropertiesPanel_ = nullptr;
    PropertiesPanel *layoutPropertiesPanel_ = nullptr;
    QTabWidget *layoutRightTabs_ = nullptr;
    LayoutDevicesPanel *layoutDevicesPanel_ = nullptr;
    QTabWidget *layoutLeftTabs_ = nullptr;
    const ice::LayoutDeviceCell *selectedLayoutDevice_ = nullptr;
    SymbolsPanel *schematicSymbolsPanel_ = nullptr;
    QTabWidget *schematicLeftTabs_ = nullptr;
    SchematicNetlistPanel *schematicNetlistPanel_ = nullptr;
    HistoryPanel *schematicHistoryPanel_ = nullptr;
    HistoryPanel *layoutHistoryPanel_ = nullptr;
    std::vector<SectionHistoryEntry> schematicHistory_;
    std::vector<SectionHistoryEntry> layoutHistory_;
    std::size_t schematicHistoryPosition_ = 0;
    std::size_t layoutHistoryPosition_ = 0;
    std::size_t syncedGlobalHistoryPosition_ = 0;
    std::string schematicSymbolsPdk_;
    bool schematicSymbolsLoaded_ = false;
    std::vector<SymbolDefinition> schematicBaseSymbols_;
    std::optional<SymbolDefinition> selectedSchematicSymbol_;
    StatusBar *statusBar_ = nullptr;
    QLabel *selectionStatus_ = nullptr;
    QAction *schematicSelectTool_ = nullptr;
    QAction *schematicPlaceSymbolTool_ = nullptr;
    QAction *layoutSelectTool_ = nullptr;
    QAction *layoutPlaceDeviceTool_ = nullptr;
    QAction *layoutRectangleTool_ = nullptr;
    QAction *layoutPathTool_ = nullptr;
    QAction *layoutDeleteTool_ = nullptr;
    int layoutPathWidth_ = 200;
    QShortcut *toolEscapeShortcut_ = nullptr;
    ice::AppearanceMode appearanceMode_ = ice::AppearanceMode::System;
    QList<QAction *> commands_;
};

} // namespace

class AppWindowAdapter final : public AppWindow {
  public:
    explicit AppWindowAdapter(std::unique_ptr<DocumentWindow> window) : window_(std::move(window)) {}

    bool setProject(ice::Project project) override { return window_->setProject(std::move(project)); }
    void show() override { window_->show(); }
    void resize(int width, int height) override { window_->resize(width, height); }
    void showMessage(const QString &message) override { window_->statusBar()->showMessage(message); }

  private:
    std::unique_ptr<DocumentWindow> window_;
};

std::unique_ptr<AppWindow> createAppWindow(ice::AppearanceMode appearance) {
    return std::make_unique<AppWindowAdapter>(std::make_unique<DocumentWindow>(appearance));
}
