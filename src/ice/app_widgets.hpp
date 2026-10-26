#pragma once

#include "icelib/core.hpp"

#include <QAction>
#include <QDialog>
#include <QSize>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPaintEvent;
class QPushButton;

class DiagnosticsPanel : public QWidget {
  public:
    explicit DiagnosticsPanel(QWidget *parent = nullptr);
};

class ActivityPanel : public QWidget {
  public:
    explicit ActivityPanel(QWidget *parent = nullptr);
};

class CommandPalette : public QDialog {
  public:
    explicit CommandPalette(const QList<QAction *> &actions, QWidget *parent = nullptr);

  private:
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
};

class StatusBar : public QStatusBar {
  public:
    explicit StatusBar(QWidget *parent = nullptr);
    void setProjectName(const QString &name);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString projectName_;
};

class StartupView : public QWidget {
  public:
    using CreateProjectCallback = std::function<void(const QString &name, const QString &location)>;
    using OpenProjectCallback = std::function<void()>;
    using OpenRecentCallback = std::function<void(const QString &path)>;

    explicit StartupView(QWidget *parent = nullptr);
    void setOnCreateProject(CreateProjectCallback callback);
    void setOnOpenProject(OpenProjectCallback callback);
    void setOnOpenRecent(OpenRecentCallback callback);
    void setRecentProjects(const QStringList &paths);
    void clearCreateForm();
    void refreshAppearance();

  private:
    static QLabel *makeLink(const QString &text, const QString &url);

    QLineEdit *name_ = nullptr;
    QLineEdit *location_ = nullptr;
    QPushButton *createButton_ = nullptr;
    QListWidget *recent_ = nullptr;
    QLabel *subtitle_ = nullptr;
    QLabel *newsPlaceholder_ = nullptr;
    CreateProjectCallback onCreateProject_;
    OpenProjectCallback onOpenProject_;
    OpenRecentCallback onOpenRecent_;
};

class EmptySchematicPanel : public QWidget {
  public:
    using CreateCallback = std::function<void(const QString &name, const std::string &pdk)>;
    explicit EmptySchematicPanel(QWidget *parent = nullptr);
    void setOnCreate(CreateCallback callback);
    void refreshPdks(const std::filesystem::path &installDirectory);
    void prepareForDisplay();

  private:
    void submit();

    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    CreateCallback onCreate_;
};

class EmptyLayoutPanel : public QWidget {
  public:
    using CreateCallback = std::function<void(const QString &name, const std::string &pdk)>;
    explicit EmptyLayoutPanel(QWidget *parent = nullptr);
    void setOnCreate(CreateCallback callback);
    void refreshPdks(const std::filesystem::path &installDirectory);
    void prepareForDisplay();

  private:
    void submit();

    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    CreateCallback onCreate_;
};

class CreateLayoutDialog : public QDialog {
  public:
    explicit CreateLayoutDialog(const std::filesystem::path &installDirectory, QWidget *parent = nullptr);
    QString layoutName() const;
    std::string pdkId() const;

  private:
    QLineEdit *name_ = nullptr;
    QComboBox *pdk_ = nullptr;
    QLabel *pdkDescription_ = nullptr;
};

class WorkspaceStack : public QStackedWidget {
  public:
    using QStackedWidget::QStackedWidget;
    QSize minimumSizeHint() const override;
};
