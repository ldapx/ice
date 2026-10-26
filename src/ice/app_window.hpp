#pragma once

#include "appearance.hpp"
#include "icelib/core.hpp"

#include <memory>

class QString;

class AppWindow {
  public:
    virtual ~AppWindow() = default;
    virtual bool setProject(ice::Project project) = 0;
    virtual void show() = 0;
    virtual void resize(int width, int height) = 0;
    virtual void showMessage(const QString &message) = 0;
};

std::unique_ptr<AppWindow> createAppWindow(ice::AppearanceMode appearance);
