#pragma once

#include "icelib/packages.hpp"

#include <QDialog>
#include <QString>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPixmap;
class QVBoxLayout;
class QWidget;

struct PackageManagerOptions {
    std::filesystem::path indexDirectory;
    std::filesystem::path installDirectory;
    std::filesystem::path projectPath;
    bool hasProject = false;
    // Called after installing a PDK package when a project is open.
    std::function<void(const ice::PackageSpec &)> activatePackage;
};

std::filesystem::path defaultPackageIndexDirectory();
std::filesystem::path defaultPackageInstallDirectory();
std::filesystem::path configuredPackageIndexDirectory();
std::filesystem::path configuredPackageInstallDirectory();
std::string defaultPackageIndexUrl();

// Scans the install directory for package manifests. The returned entries use
// the package's own .github/logo.png and expose the installed manifest.
std::vector<ice::PackageIndexEntry> installedPackages(const std::filesystem::path &installDirectory);
std::vector<ice::PackageIndexEntry> installedPdkPackages(const std::filesystem::path &installDirectory);
bool packageProvidesPdk(const ice::PackageSpec &package);
QString packagePdkDisplayName(const ice::PackageSpec &package);

// Shows the project dependency prompt for missing packages and installs the
// selected packages from the index. Returns true when every missing package is
// installed, or false when the user closes the project.
bool installMissingPackages(QWidget *parent, const std::vector<std::string> &packageNames,
                            const std::filesystem::path &indexDirectory, const std::filesystem::path &installDirectory);

class CreateSchematicDialog : public QDialog {
  public:
    explicit CreateSchematicDialog(const std::filesystem::path &installDirectory, QWidget *parent = nullptr);

    QString schematicName() const;
    std::string pdkId() const;

  private:
    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    QLabel *pdkDescription_ = nullptr;
};

class PackageManagerDialog : public QDialog {
  public:
    explicit PackageManagerDialog(PackageManagerOptions options, QWidget *parent = nullptr);

    std::filesystem::path indexDirectory() const;
    std::filesystem::path installDirectory() const;

  private:
    void refreshIndex(bool refreshRepository);
    void rebuildPackageList();
    QWidget *createPackageCard(const ice::PackageIndexEntry &entry);
    QPixmap packageLogoPixmap(const ice::PackageIndexEntry &entry) const;
    std::optional<ice::PackageSpec> installedPackage(const std::string &name) const;
    void installIndexedPackage(const ice::PackageIndexEntry &entry);
    void uninstallPackage(const ice::PackageIndexEntry &entry);
    void installFromUrl();
    void installRepository(const std::string &url, const std::string &revision, const QString &label);
    void activatePackage(const ice::PackageSpec &spec);
    void mergeInstalledPackages();
    void writeProjectLock(const ice::PackageSpec &spec);
    void setStatus(const QString &message);
    void updateInstallLocationLabel();

    PackageManagerOptions options_;
    QLineEdit *indexPath_ = nullptr;
    QLabel *installLocation_ = nullptr;
    QLabel *status_ = nullptr;
    QWidget *packageContainer_ = nullptr;
    QVBoxLayout *packageLayout_ = nullptr;
    std::vector<ice::PackageIndexEntry> entries_;
};
