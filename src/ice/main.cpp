#include "app_window.hpp"

#include "appearance.hpp"
#include "icelib/core.hpp"

#include <QApplication>
#include <QString>

#include <string>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ICE"));
    app.setApplicationName(QStringLiteral("ICE"));
    app.setApplicationVersion(QString::fromStdString(ice::version()));
    QApplication::setStyle(QStringLiteral("Fusion"));
    const auto appearance = ice::loadAppearanceMode();
    ice::applyAppearance(appearance);

    auto window = createAppWindow(appearance);
    if (argc == 3 && std::string(argv[1]) == "--project") {
        try {
            if (!window->setProject(ice::Project::load(argv[2])))
                return 0;
            window->setProjectPath(QString::fromStdString(argv[2]));
        } catch (const ice::ProjectError &) {
            window->showMessage(QStringLiteral("Unable to load project"));
        }
    }
    window->resize(1440, 900);
    window->show();
    return app.exec();
}
