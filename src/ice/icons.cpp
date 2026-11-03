#include "icons.hpp"

#include "ui_colors.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSvgRenderer>

#include <filesystem>
#include <vector>

namespace ice {
namespace {
std::filesystem::path iconDirectory() {
    const auto appDir = QCoreApplication::applicationDirPath().toStdString();
    const std::vector<std::filesystem::path> candidates = {
        std::filesystem::path(appDir) / "icons" / "lucide",
        std::filesystem::path(appDir) / "assets" / "icons" / "lucide",
        std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "icons" / "lucide",
#ifdef ICE_SOURCE_DIR
        std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "icons" / "lucide",
#endif
        std::filesystem::path("assets") / "icons" / "lucide",
        std::filesystem::path("/usr/share/ice/icons/lucide"),
    };
    for (const auto &candidate : candidates) {
        std::error_code status;
        if (std::filesystem::is_directory(candidate, status) && !status)
            return candidate;
    }
    return {};
}

QByteArray iconSource(const QString &name) {
    static QHash<QString, QByteArray> cache;
    if (const auto cached = cache.find(name); cached != cache.end())
        return cached.value();

    QByteArray source;
    const auto directory = iconDirectory();
    if (!directory.empty()) {
        const auto path = directory / (name.toStdString() + ".svg");
        QFile file(QString::fromStdString(path.string()));
        if (file.open(QIODevice::ReadOnly))
            source = file.readAll();
    }
    cache.insert(name, source);
    return source;
}

QPixmap renderIcon(const QByteArray &source, const QColor &color, int size) {
    auto colored = source;
    colored.replace("currentColor", color.name(QColor::HexRgb).toUtf8());

    const int pixelSize = size * 2;
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(colored);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(0.0, 0.0, pixelSize, pixelSize));
    }
    pixmap.setDevicePixelRatio(2.0);
    return pixmap;
}

QIcon buildIcon(const QByteArray &source, int size) {
    QIcon icon;
    icon.addPixmap(renderIcon(source, uiColor(UiColor::Text1), size), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderIcon(source, uiColor(UiColor::TextOnAccent), size), QIcon::Normal, QIcon::On);
    icon.addPixmap(renderIcon(source, uiColor(UiColor::Text2), size), QIcon::Disabled, QIcon::Off);
    icon.addPixmap(renderIcon(source, uiColor(UiColor::Text2), size), QIcon::Disabled, QIcon::On);
    return icon;
}
} // namespace

namespace {
QPixmap renderArtworkIcon(const QByteArray &source, const QColor &color, int size) {
    const int pixelSize = size * 2;
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);
    QSvgRenderer renderer(source);
    if (renderer.isValid()) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF target(0.0, 0.0, pixelSize, pixelSize);
        const auto bounds = renderer.defaultSize();
        if (bounds.isValid() && !bounds.isEmpty()) {
            const auto scaled = bounds.scaled(QSize(pixelSize, pixelSize), Qt::KeepAspectRatio);
            target = QRectF((pixelSize - scaled.width()) / 2.0, (pixelSize - scaled.height()) / 2.0, scaled.width(),
                            scaled.height());
        }
        renderer.render(&painter, target);
        // Symbol artwork is authored in black; tint it with the palette colour so
        // the icon stays visible in dark and light themes.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(QRectF(0.0, 0.0, pixelSize, pixelSize), color);
    }
    pixmap.setDevicePixelRatio(2.0);
    return pixmap;
}
} // namespace

QIcon symbolArtworkIcon(const QByteArray &svg, int size) {
    QIcon icon;
    icon.addPixmap(renderArtworkIcon(svg, uiColor(UiColor::Text1), size), QIcon::Normal, QIcon::Off);
    icon.addPixmap(renderArtworkIcon(svg, uiColor(UiColor::TextOnAccent), size), QIcon::Selected, QIcon::Off);
    icon.addPixmap(renderArtworkIcon(svg, uiColor(UiColor::Text2), size), QIcon::Disabled, QIcon::Off);
    return icon;
}

QIcon lucideIcon(const QString &name, int size) {
    const auto source = iconSource(name);
    if (source.isEmpty())
        return {};
    return buildIcon(source, size);
}

QPixmap appLogo(int size) {
    static const QPixmap logo = [] {
        const auto appDir = QCoreApplication::applicationDirPath().toStdString();
        const std::vector<std::filesystem::path> candidates = {
            std::filesystem::path(appDir) / "logo.png",
            std::filesystem::path(appDir) / "assets" / "logo.png",
            std::filesystem::path(appDir) / ".." / ".." / ".." / ".." / "assets" / "logo.png",
#ifdef ICE_SOURCE_DIR
            std::filesystem::path(ICE_SOURCE_DIR) / "assets" / "logo.png",
#endif
            std::filesystem::path("assets") / "logo.png",
            std::filesystem::path("/usr/share/ice/logo.png"),
        };
        for (const auto &candidate : candidates) {
            std::error_code status;
            if (std::filesystem::is_regular_file(candidate, status) && !status)
                return QPixmap(QString::fromStdString(candidate.string()));
        }
        return QPixmap();
    }();
    if (logo.isNull() || size <= 0)
        return logo;
    return logo.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace ice
