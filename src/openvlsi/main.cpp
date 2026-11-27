#include "app_window.hpp"

#include "appearance.hpp"
#include "openvlsilib/core.hpp"

#include <QApplication>
#include <QString>

#include <string>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("OpenVLSI"));
    app.setApplicationName(QStringLiteral("OpenVLSI"));
    app.setApplicationVersion(QString::fromStdString(openvlsi::version()));
    QApplication::setStyle(openvlsi::createAppStyle());
    const auto appearance = openvlsi::loadAppearanceMode();
    openvlsi::applyAppearance(appearance);

    auto window = createAppWindow(appearance);
    if (argc == 3 && std::string(argv[1]) == "--project") {
        try {
            if (!window->setProject(openvlsi::Project::load(argv[2])))
                return 0;
            window->setProjectPath(QString::fromStdString(argv[2]));
        } catch (const openvlsi::ProjectError &) {
            window->showMessage(QStringLiteral("Unable to load project"));
        }
    }
    window->resize(1440, 900);
    window->show();
    return app.exec();
}
