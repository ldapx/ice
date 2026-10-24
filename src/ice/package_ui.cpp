#include "package_ui.hpp"

#include "icelib/core.hpp"

#include "icons.hpp"
#include "theme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <system_error>
#include <utility>

namespace {
class WaitCursor {
  public:
    WaitCursor() { QApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursor() { QApplication::restoreOverrideCursor(); }
    WaitCursor(const WaitCursor &) = delete;
    WaitCursor &operator=(const WaitCursor &) = delete;
};

void setButtonIcon(QPushButton *button, const QString &name) {
    button->setProperty("lucideIcon", name);
    button->setIcon(ice::lucideIcon(name));
}

QString packageLabel(const ice::PackageSpec &spec) {
    return QStringLiteral("%1 %2").arg(QString::fromStdString(spec.name), QString::fromStdString(spec.version));
}

class PackageRepositoryDialog : public QDialog {
  public:
    explicit PackageRepositoryDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle(QStringLiteral("Install Package from Repository"));
        setModal(true);
        resize(600, 290);

        auto *layout = new QVBoxLayout(this);
        auto *description = new QLabel(
            QStringLiteral("Clone and install a package repository directly. The repository must contain a "
                           "package.toml manifest and may provide .github/logo.png for the package manager."));
        description->setWordWrap(true);
        layout->addWidget(description);

        auto *form = new QFormLayout;
        url_ = new QLineEdit;
        url_->setPlaceholderText(QStringLiteral("https://github.com/owner/package.git"));
        url_->setClearButtonEnabled(true);
        form->addRow(QStringLiteral("Repository URL"), url_);
        revision_ = new QLineEdit;
        revision_->setPlaceholderText(QStringLiteral("main, dev, tag, or commit (optional)"));
        revision_->setClearButtonEnabled(true);
        form->addRow(QStringLiteral("Branch or tag"), revision_);
        layout->addLayout(form);
        layout->addStretch();

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto *install = buttons->button(QDialogButtonBox::Ok);
        install->setText(QStringLiteral("Install"));
        setButtonIcon(install, QStringLiteral("download"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &PackageRepositoryDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &PackageRepositoryDialog::reject);
        url_->setFocus();
    }

    std::string url() const { return url_->text().trimmed().toStdString(); }
    std::string revision() const { return revision_->text().trimmed().toStdString(); }

    void accept() override {
        if (url_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Package repository"),
                                 QStringLiteral("Enter a package repository URL."));
            url_->setFocus();
            return;
        }
        QDialog::accept();
    }

  private:
    QLineEdit *url_ = nullptr;
    QLineEdit *revision_ = nullptr;
};

bool packageIsInstalled(const std::filesystem::path &installDirectory, const std::string &name) {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos)
        return false;
    const auto root = installDirectory / name;
    std::error_code error;
    if (!std::filesystem::is_regular_file(root / "package.toml", error) || error)
        return false;
    try {
        return ice::PackageManager::readManifest(root).name == name;
    } catch (const ice::ProjectError &) {
        return false;
    }
}

class MissingPackagesDialog : public QDialog {
  public:
    MissingPackagesDialog(std::vector<std::string> missing, std::map<std::string, ice::PackageIndexEntry> indexEntries,
                          std::filesystem::path installDirectory, QWidget *parent = nullptr)
        : QDialog(parent), missing_(std::move(missing)), indexEntries_(std::move(indexEntries)),
          installDirectory_(std::move(installDirectory)) {
        setWindowTitle(QStringLiteral("Missing Project Packages"));
        setModal(true);
        resize(700, 440);

        auto *layout = new QVBoxLayout(this);
        auto *description =
            new QLabel(QStringLiteral("This project depends on packages that are not installed. Install the missing "
                                      "packages to open the project, or close the project."));
        description->setWordWrap(true);
        layout->addWidget(description);

        list_ = new QListWidget;
        list_->setAccessibleName(QStringLiteral("Missing package dependencies"));
        layout->addWidget(list_, 1);
        for (const auto &name : missing_) {
            auto *item = new QListWidgetItem(list_);
            item->setData(Qt::UserRole, QString::fromStdString(name));
            const auto entry = indexEntries_.find(name);
            if (entry == indexEntries_.end() || entry->second.repositoryUrl.empty()) {
                item->setText(
                    QStringLiteral("%1 — not available in the package index").arg(QString::fromStdString(name)));
                continue;
            }
            auto text = QStringLiteral("%1 %2").arg(QString::fromStdString(entry->second.spec.name),
                                                    QString::fromStdString(entry->second.spec.version));
            if (!entry->second.spec.description.empty())
                text += QStringLiteral(" — %1").arg(QString::fromStdString(entry->second.spec.description));
            item->setText(text);
            item->setToolTip(QString::fromStdString(entry->second.repositoryUrl));
            QPixmap logo(QString::fromStdString(entry->second.logo.string()));
            if (!logo.isNull())
                item->setIcon(QIcon(logo.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            if (packageIsInstalled(installDirectory_, name))
                item->setText(item->text() + QStringLiteral(" — installed"));
        }

        status_ = new QLabel;
        status_->setWordWrap(true);
        status_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
        layout->addWidget(status_);

        auto *buttons = new QHBoxLayout;
        installButton_ = new QPushButton(QStringLiteral("Install Missing Packages"));
        setButtonIcon(installButton_, QStringLiteral("download"));
        buttons->addWidget(installButton_);
        buttons->addStretch();
        auto *close = new QPushButton(QStringLiteral("Close Project"));
        buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(installButton_, &QPushButton::clicked, this, &MissingPackagesDialog::installAll);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
    }

  private:
    void installAll() {
        installButton_->setEnabled(false);
        status_->setText(QStringLiteral("Installing missing packages…"));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        QStringList failures;
        {
            WaitCursor wait;
            for (const auto &name : missing_) {
                if (packageIsInstalled(installDirectory_, name))
                    continue;
                const auto entry = indexEntries_.find(name);
                if (entry == indexEntries_.end() || entry->second.repositoryUrl.empty()) {
                    failures
                        << QStringLiteral("%1: not available in the package index").arg(QString::fromStdString(name));
                    continue;
                }
                try {
                    ice::PackageManager::installGitPackage(installDirectory_, entry->second.repositoryUrl,
                                                           entry->second.repositoryRevision);
                } catch (const std::exception &error) {
                    failures << QStringLiteral("%1: %2").arg(QString::fromStdString(name),
                                                             QString::fromUtf8(error.what()));
                }
            }
        }

        bool allInstalled = true;
        for (const auto &name : missing_)
            allInstalled = allInstalled && packageIsInstalled(installDirectory_, name);
        if (allInstalled) {
            status_->setText(QStringLiteral("All missing packages are installed."));
            accept();
            return;
        }
        for (int row = 0; row < list_->count(); ++row) {
            auto *item = list_->item(row);
            const auto name = item->data(Qt::UserRole).toString().toStdString();
            if (packageIsInstalled(installDirectory_, name) && !item->text().endsWith(QStringLiteral(" — installed")))
                item->setText(item->text() + QStringLiteral(" — installed"));
        }
        status_->setText(QStringLiteral("Some packages could not be installed:\n%1")
                             .arg(failures.isEmpty() ? QStringLiteral("Unknown installation error.")
                                                     : failures.join(QStringLiteral("\n"))));
        installButton_->setEnabled(true);
    }

    std::vector<std::string> missing_;
    std::map<std::string, ice::PackageIndexEntry> indexEntries_;
    std::filesystem::path installDirectory_;
    QListWidget *list_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *installButton_ = nullptr;
};
} // namespace

std::filesystem::path defaultPackageIndexDirectory() {
    const std::filesystem::path home(QDir::homePath().toStdString());
    const auto checkout = home / "repositories" / "ice-packages";
    std::error_code error;
    if (std::filesystem::is_directory(checkout, error) && !error)
        return checkout;
    return home / ".ice" / "package-index";
}

std::filesystem::path defaultPackageInstallDirectory() {
    return std::filesystem::path(QDir::homePath().toStdString()) / ".ice" / "packages";
}

std::string defaultPackageIndexUrl() { return "https://github.com/ldapx/ice-packages.git"; }

std::filesystem::path configuredPackageIndexDirectory() {
    QSettings settings;
    const auto stored = settings.value(QStringLiteral("packages/indexDirectory")).toString();
    return stored.isEmpty() ? defaultPackageIndexDirectory() : std::filesystem::path(stored.toStdString());
}

std::filesystem::path configuredPackageInstallDirectory() {
    QSettings settings;
    const auto stored = settings.value(QStringLiteral("packages/localRepository")).toString();
    return stored.isEmpty() ? defaultPackageInstallDirectory() : std::filesystem::path(stored.toStdString());
}

std::vector<ice::PackageIndexEntry> installedPackages(const std::filesystem::path &installDirectory) {
    return ice::PackageManager::scanPackageIndex(installDirectory);
}

std::vector<ice::PackageIndexEntry> installedPdkPackages(const std::filesystem::path &installDirectory) {
    std::vector<ice::PackageIndexEntry> result;
    for (auto &entry : installedPackages(installDirectory)) {
        const auto capability = entry.spec.capabilities.find("pdk");
        if (capability != entry.spec.capabilities.end() && capability->second)
            result.push_back(std::move(entry));
    }
    return result;
}

bool installMissingPackages(QWidget *parent, const std::vector<std::string> &packageNames,
                            const std::filesystem::path &indexDirectory,
                            const std::filesystem::path &installDirectory) {
    if (packageNames.empty())
        return true;
    std::map<std::string, ice::PackageIndexEntry> entries;
    for (auto &entry : ice::PackageManager::scanPackageIndex(indexDirectory))
        entries.emplace(entry.spec.name, std::move(entry));
    MissingPackagesDialog dialog(packageNames, std::move(entries), installDirectory, parent);
    return dialog.exec() == QDialog::Accepted;
}

CreateSchematicDialog::CreateSchematicDialog(const std::filesystem::path &installDirectory, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Create Schematic"));
    setModal(true);
    resize(600, 300);

    auto *layout = new QVBoxLayout(this);
    auto *description =
        new QLabel(QStringLiteral("Choose the PDK used by this schematic. Generic PDKs use only the built-in symbol "
                                  "library; package PDKs add their symbols to the generic library."));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *form = new QFormLayout;
    name_ = new QLineEdit(QStringLiteral("top"));
    name_->setClearButtonEnabled(true);
    form->addRow(QStringLiteral("Schematic name"), name_);
    pdk_ = new QComboBox;
    pdk_->setAccessibleName(QStringLiteral("Schematic PDK"));
    pdk_->addItem(QStringLiteral("Generic PDK"), QString::fromLatin1(ice::genericPdkId));
    pdk_->setItemData(0, QStringLiteral("Built-in generic symbol library only."), Qt::ToolTipRole);
    for (const auto &entry : installedPdkPackages(installDirectory)) {
        const auto label =
            QStringLiteral("%1  (%2)")
                .arg(QString::fromStdString(entry.spec.name), QString::fromStdString(entry.spec.version));
        pdk_->addItem(label, QString::fromStdString(entry.spec.name));
        auto detail = QStringLiteral("Package PDK. Generic and package symbols are available.");
        if (!entry.spec.description.empty())
            detail = QString::fromStdString(entry.spec.description) + QStringLiteral("  ") + detail;
        pdk_->setItemData(pdk_->count() - 1, detail, Qt::ToolTipRole);
    }
    form->addRow(QStringLiteral("PDK"), pdk_);
    layout->addLayout(form);

    pdkDescription_ = new QLabel;
    pdkDescription_->setWordWrap(true);
    pdkDescription_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
    layout->addWidget(pdkDescription_);
    layout->addStretch();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Create"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancel"));
    layout->addWidget(buttons);
    connect(pdk_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { pdkDescription_->setText(pdk_->currentData(Qt::ToolTipRole).toString()); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (name_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Create Schematic"), QStringLiteral("Enter a schematic name."));
            name_->setFocus();
            return;
        }
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    pdkDescription_->setText(pdk_->currentData(Qt::ToolTipRole).toString());
    name_->selectAll();
    name_->setFocus();
}

QString CreateSchematicDialog::schematicName() const { return name_->text().trimmed(); }

std::string CreateSchematicDialog::pdkId() const { return pdk_->currentData().toString().toStdString(); }

PackageManagerDialog::PackageManagerDialog(PackageManagerOptions options, QWidget *parent)
    : QDialog(parent), options_(std::move(options)) {
    if (options_.indexDirectory.empty())
        options_.indexDirectory = defaultPackageIndexDirectory();
    if (options_.installDirectory.empty())
        options_.installDirectory = defaultPackageInstallDirectory();

    setWindowTitle(QStringLiteral("Manage Packages"));
    setModal(true);
    resize(900, 640);
    setMinimumSize(680, 440);

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        QStringLiteral("Browse packages from the index, install a repository by URL, or remove installed packages. "
                       "Package logos are loaded from each package's .github/logo.png."));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *indexRow = new QHBoxLayout;
    indexRow->addWidget(new QLabel(QStringLiteral("Package index:")));
    indexPath_ = new QLineEdit(QString::fromStdString(options_.indexDirectory.string()));
    indexPath_->setPlaceholderText(QStringLiteral("Path to the package index repository"));
    indexPath_->setClearButtonEnabled(true);
    indexPath_->setToolTip(QStringLiteral("A Git repository whose package entries are Git submodules. "
                                          "Default remote: %1")
                               .arg(QString::fromStdString(defaultPackageIndexUrl())));
    indexRow->addWidget(indexPath_, 1);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"));
    setButtonIcon(refresh, QStringLiteral("refresh-cw"));
    indexRow->addWidget(refresh);
    layout->addLayout(indexRow);

    installLocation_ = new QLabel;
    installLocation_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
    layout->addWidget(installLocation_);
    updateInstallLocationLabel();

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    packageContainer_ = new QWidget;
    packageLayout_ = new QVBoxLayout(packageContainer_);
    packageLayout_->setContentsMargins(0, 0, 4, 0);
    packageLayout_->setSpacing(8);
    scroll->setWidget(packageContainer_);
    layout->addWidget(scroll, 1);

    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_);

    auto *footer = new QHBoxLayout;
    auto *installUrl = new QPushButton(QStringLiteral("Install from Repository URL…"));
    setButtonIcon(installUrl, QStringLiteral("download"));
    footer->addWidget(installUrl);
    footer->addStretch();
    auto *close = new QPushButton(QStringLiteral("Close"));
    footer->addWidget(close);
    layout->addLayout(footer);

    connect(refresh, &QPushButton::clicked, this, [this] { refreshIndex(true); });
    connect(installUrl, &QPushButton::clicked, this, &PackageManagerDialog::installFromUrl);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    refreshIndex(false);
}

std::filesystem::path PackageManagerDialog::indexDirectory() const {
    return std::filesystem::path(indexPath_->text().trimmed().toStdString());
}

std::filesystem::path PackageManagerDialog::installDirectory() const { return options_.installDirectory; }

void PackageManagerDialog::updateInstallLocationLabel() {
    installLocation_->setText(QStringLiteral("Packages are installed into %1")
                                  .arg(QString::fromStdString(options_.installDirectory.string())));
}

void PackageManagerDialog::refreshIndex(bool refreshRepository) {
    options_.indexDirectory = indexDirectory();
    if (options_.indexDirectory.empty()) {
        entries_.clear();
        rebuildPackageList();
        setStatus(QStringLiteral("Enter a package index path or click Refresh to clone the default index."));
        return;
    }

    QString refreshError;
    if (refreshRepository) {
        setStatus(QStringLiteral("Refreshing package index…"));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        try {
            WaitCursor wait;
            std::error_code error;
            if (!std::filesystem::exists(options_.indexDirectory, error) || error) {
                ice::PackageManager::cloneIndex(options_.indexDirectory, defaultPackageIndexUrl());
            } else {
                ice::PackageManager::updateIndex(options_.indexDirectory);
            }
        } catch (const std::exception &error) {
            refreshError =
                QStringLiteral("Unable to refresh the package index: %1").arg(QString::fromUtf8(error.what()));
        }
    }

    entries_ = ice::PackageManager::scanPackageIndex(options_.indexDirectory);
    mergeInstalledPackages();
    rebuildPackageList();
    std::error_code indexError;
    const bool indexExists = std::filesystem::is_directory(options_.indexDirectory, indexError) && !indexError;
    if (!refreshError.isEmpty()) {
        setStatus(refreshError);
    } else if (entries_.empty()) {
        if (!indexExists) {
            setStatus(QStringLiteral("Package index not found at %1. Click Refresh to clone the default index.")
                          .arg(QString::fromStdString(options_.indexDirectory.string())));
        } else {
            setStatus(QStringLiteral("No packages found in %1. Click Refresh to initialize the index submodules.")
                          .arg(QString::fromStdString(options_.indexDirectory.string())));
        }
    } else {
        const auto installedCount = std::count_if(entries_.begin(), entries_.end(), [this](const auto &entry) {
            return installedPackage(entry.spec.name).has_value();
        });
        setStatus(QStringLiteral("%1 package(s), %2 installed.")
                      .arg(entries_.size())
                      .arg(static_cast<qulonglong>(installedCount)));
    }
}

void PackageManagerDialog::rebuildPackageList() {
    while (auto *item = packageLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (entries_.empty()) {
        auto *empty = new QLabel(QStringLiteral("No indexed packages to display."));
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(
            QStringLiteral("color: %1; padding: 24px;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
        packageLayout_->addWidget(empty);
    } else {
        for (const auto &entry : entries_)
            packageLayout_->addWidget(createPackageCard(entry));
    }
    packageLayout_->addStretch();
}

QPixmap PackageManagerDialog::packageLogoPixmap(const ice::PackageIndexEntry &entry) const {
    constexpr int size = 78;
    QPixmap canvas(size * 2, size * 2);
    canvas.setDevicePixelRatio(2.0);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF frame(1.0, 1.0, size - 2.0, size - 2.0);
    QPainterPath clip;
    clip.addRoundedRect(frame, 6.0, 6.0);
    painter.setClipPath(clip);
    painter.fillRect(frame, ice::themeColor(ice::ThemeRole::Background2));

    const QPixmap source(QString::fromStdString(entry.logo.string()));
    if (!entry.logo.empty() && !source.isNull()) {
        const qreal target = size - 10.0;
        const qreal scale = std::min(target / source.width(), target / source.height());
        const QSizeF scaled(source.width() * scale, source.height() * scale);
        const QRectF targetRect((size - scaled.width()) / 2.0, (size - scaled.height()) / 2.0, scaled.width(),
                                scaled.height());
        painter.drawPixmap(targetRect, source, QRectF(source.rect()));
    } else {
        auto font = painter.font();
        font.setBold(true);
        font.setPointSize(font.pointSize() + 10);
        painter.setFont(font);
        painter.setPen(ice::themeColor(ice::ThemeRole::Text2));
        const auto name = QString::fromStdString(entry.spec.name);
        painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, name.left(2).toUpper());
    }

    painter.setClipping(false);
    painter.setPen(ice::themeColor(ice::ThemeRole::Border1));
    painter.drawRoundedRect(frame, 6.0, 6.0);
    return canvas;
}

std::optional<ice::PackageSpec> PackageManagerDialog::installedPackage(const std::string &name) const {
    if (name.empty() || name.find_first_of("/\\") != std::string::npos)
        return std::nullopt;
    const auto root = options_.installDirectory / name;
    std::error_code error;
    if (!std::filesystem::is_regular_file(root / "package.toml", error) || error)
        return std::nullopt;
    try {
        auto spec = ice::PackageManager::readManifest(root);
        if (spec.name != name)
            return std::nullopt;
        return spec;
    } catch (const ice::ProjectError &) {
        return std::nullopt;
    }
}

void PackageManagerDialog::mergeInstalledPackages() {
    for (auto entry : installedPackages(options_.installDirectory)) {
        const auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto &candidate) {
            return candidate.spec.name == entry.spec.name;
        });
        if (existing == entries_.end())
            entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const auto &left, const auto &right) { return left.spec.name < right.spec.name; });
}

QWidget *PackageManagerDialog::createPackageCard(const ice::PackageIndexEntry &entry) {
    const auto installed = installedPackage(entry.spec.name);
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("packageCard"));
    card->setStyleSheet(QStringLiteral("#packageCard { background: %1; border: 1px solid %2; border-radius: 7px; } "
                                       "#packageLogo { background: %3; border: 1px solid %2; border-radius: 6px; }")
                            .arg(ice::themeColor(ice::ThemeRole::Surface1).name(),
                                 ice::themeColor(ice::ThemeRole::Border1).name(),
                                 ice::themeColor(ice::ThemeRole::Background2).name()));
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(12, 10, 12, 10);
    row->setSpacing(12);

    auto *logo = new QLabel;
    logo->setObjectName(QStringLiteral("packageLogo"));
    logo->setFixedSize(78, 78);
    logo->setAlignment(Qt::AlignCenter);
    logo->setPixmap(packageLogoPixmap(entry));
    logo->setToolTip(entry.logo.empty() ? QStringLiteral("This package does not provide .github/logo.png.")
                                        : QString::fromStdString(entry.logo.string()));
    row->addWidget(logo, 0, Qt::AlignTop);

    auto *text = new QVBoxLayout;
    text->setSpacing(3);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto *name = new QLabel(QString::fromStdString(entry.spec.name));
    auto nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSize(nameFont.pointSize() + 2);
    name->setFont(nameFont);
    titleRow->addWidget(name);
    auto *version = new QLabel(QStringLiteral("v%1").arg(QString::fromStdString(entry.spec.version)));
    version->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
    titleRow->addWidget(version);
    if (installed) {
        auto *badge = new QLabel(QStringLiteral("Installed %1").arg(QString::fromStdString(installed->version)));
        badge->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 600;").arg(ice::themeColor(ice::ThemeRole::Accent3).name()));
        titleRow->addWidget(badge);
    }
    titleRow->addStretch();
    text->addLayout(titleRow);

    if (!entry.spec.description.empty()) {
        auto *description = new QLabel(QString::fromStdString(entry.spec.description));
        description->setWordWrap(true);
        text->addWidget(description);
    }

    QStringList details;
    const auto capabilities = ice::PackageManager::capabilities(entry.spec);
    if (!capabilities.empty()) {
        QStringList names;
        for (const auto &capability : capabilities)
            names << QString::fromStdString(capability);
        details << QStringLiteral("Provides: %1").arg(names.join(QStringLiteral(", ")));
    }
    if (!entry.repositoryRevision.empty())
        details << QStringLiteral("Branch: %1").arg(QString::fromStdString(entry.repositoryRevision));
    if (!entry.spec.license.empty())
        details << QStringLiteral("License: %1").arg(QString::fromStdString(entry.spec.license));
    auto *meta = new QLabel(details.join(QStringLiteral("  •  ")));
    meta->setWordWrap(true);
    meta->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
    text->addWidget(meta);
    row->addLayout(text, 1);

    auto *actions = new QVBoxLayout;
    actions->setSpacing(6);
    if (installed) {
        if (entry.spec.name == "ihp.sg13g2") {
            auto *activate = new QPushButton(QStringLiteral("Activate"));
            setButtonIcon(activate, QStringLiteral("circuit-board"));
            activate->setEnabled(options_.hasProject);
            if (!options_.hasProject)
                activate->setToolTip(QStringLiteral("Open a project before activating the technology."));
            connect(activate, &QPushButton::clicked, this, [this, spec = *installed] { activatePackage(spec); });
            actions->addWidget(activate);
        }
        auto *uninstall = new QPushButton(QStringLiteral("Uninstall"));
        setButtonIcon(uninstall, QStringLiteral("trash"));
        uninstall->setToolTip(QStringLiteral("Remove this package from the local package repository."));
        connect(uninstall, &QPushButton::clicked, this, [this, entry] { uninstallPackage(entry); });
        actions->addWidget(uninstall);
    } else {
        auto *install = new QPushButton(QStringLiteral("Install"));
        setButtonIcon(install, QStringLiteral("download"));
        install->setEnabled(!entry.repositoryUrl.empty());
        if (entry.repositoryUrl.empty())
            install->setToolTip(QStringLiteral("No repository URL is declared for this indexed package."));
        else
            install->setToolTip(QString::fromStdString(entry.repositoryUrl));
        connect(install, &QPushButton::clicked, this, [this, entry] { installIndexedPackage(entry); });
        actions->addWidget(install);
    }
    row->addLayout(actions);

    return card;
}

void PackageManagerDialog::installIndexedPackage(const ice::PackageIndexEntry &entry) {
    if (entry.repositoryUrl.empty()) {
        setStatus(QStringLiteral("The indexed package %1 does not declare a repository URL.")
                      .arg(QString::fromStdString(entry.spec.name)));
        return;
    }
    installRepository(entry.repositoryUrl, entry.repositoryRevision,
                      QStringLiteral("%1 %2").arg(QString::fromStdString(entry.spec.name),
                                                  QString::fromStdString(entry.spec.version)));
}

void PackageManagerDialog::uninstallPackage(const ice::PackageIndexEntry &entry) {
    const auto name = QString::fromStdString(entry.spec.name);
    const auto response = QMessageBox::question(
        this, QStringLiteral("Uninstall Package"),
        QStringLiteral("Uninstall %1?\n\nAny project that depends on this package will require it to be "
                       "reinstalled when the project is opened again.")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (response != QMessageBox::Yes)
        return;
    try {
        WaitCursor wait;
        ice::PackageManager::uninstallPackage(options_.installDirectory, entry.spec.name);
    } catch (const std::exception &error) {
        setStatus(QStringLiteral("Uninstall failed: %1").arg(QString::fromUtf8(error.what())));
        return;
    }
    refreshIndex(false);
    setStatus(QStringLiteral("Uninstalled %1.").arg(name));
}

void PackageManagerDialog::installFromUrl() {
    PackageRepositoryDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    installRepository(dialog.url(), dialog.revision(), QStringLiteral("package"));
}

void PackageManagerDialog::installRepository(const std::string &url, const std::string &revision,
                                             const QString &label) {
    setStatus(QStringLiteral("Installing %1…").arg(label));
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    std::optional<ice::PackageSpec> spec;
    try {
        WaitCursor wait;
        spec = ice::PackageManager::installGitPackage(options_.installDirectory, url, revision);
    } catch (const std::exception &error) {
        setStatus(QStringLiteral("Installation failed: %1").arg(QString::fromUtf8(error.what())));
        return;
    }

    QString message = QStringLiteral("Installed %1.").arg(packageLabel(*spec));
    if (options_.hasProject) {
        try {
            writeProjectLock(*spec);
            message += QStringLiteral(" Project lockfile updated.");
        } catch (const std::exception &error) {
            message += QStringLiteral(" Lockfile not updated: %1").arg(QString::fromUtf8(error.what()));
        }
    }
    if (spec->name == "ihp.sg13g2") {
        if (options_.hasProject && options_.activatePackage) {
            try {
                options_.activatePackage(*spec);
                message += QStringLiteral(" IHP SG13G2 installed and activated.");
            } catch (const std::exception &error) {
                message += QStringLiteral(" Activation failed: %1").arg(QString::fromUtf8(error.what()));
            }
        } else {
            message += QStringLiteral(" Open a project and activate it from the package list.");
        }
    }
    refreshIndex(false);
    setStatus(message);
}

void PackageManagerDialog::activatePackage(const ice::PackageSpec &spec) {
    if (!options_.hasProject) {
        setStatus(QStringLiteral("Open a project before activating %1.").arg(packageLabel(spec)));
        return;
    }
    if (!options_.activatePackage) {
        setStatus(QStringLiteral("%1 does not provide an activation action.").arg(packageLabel(spec)));
        return;
    }
    try {
        options_.activatePackage(spec);
        setStatus(QStringLiteral("Activated %1.").arg(packageLabel(spec)));
        rebuildPackageList();
    } catch (const std::exception &error) {
        setStatus(QStringLiteral("Activation failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void PackageManagerDialog::writeProjectLock(const ice::PackageSpec &spec) {
    if (!options_.hasProject)
        return;
    const auto requested = std::map<std::string, std::string>{{spec.name, spec.version}};
    auto lock = ice::PackageManager::resolve({options_.installDirectory}, requested, ice::version());
    const auto project = std::filesystem::path(options_.projectPath);
    const auto lockPath = project.empty() ? std::filesystem::path("ice.lock") : project.parent_path() / "ice.lock";
    ice::PackageManager::writeLockfile(lockPath, lock);
}

void PackageManagerDialog::setStatus(const QString &message) {
    status_->setText(message);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(ice::themeColor(ice::ThemeRole::Text2).name()));
}
