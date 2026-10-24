#pragma once

#include "icelib/core.hpp"

#include <QDialog>
#include <QString>
#include <QSvgWidget>

#include <cstddef>
#include <string>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QPaintEvent;
class QTabWidget;
class QTableWidget;

// QSvgWidget stretches the SVG to fill its rectangle. This variant preserves
// the artwork aspect ratio and centers it inside the available space.
class SymbolPreview : public QSvgWidget {
  public:
    using QSvgWidget::QSvgWidget;

  protected:
    void paintEvent(QPaintEvent *event) override;
};

struct SymbolDefinition {
    QString name;
    QString category;
    QString svg;
    std::vector<ice::Pin> pins;
    // Default instance parameters seeded by the symbol sidecar (e.g. the
    // resistance of a resistor); placed instances copy them at drop time.
    std::vector<ice::Parameter> parameters;
};

struct ParameterEdit {
    ice::ObjectId id = 0;
    QString name;
    QString value;
};

std::vector<SymbolDefinition> genericSymbolLibrary();
std::vector<ice::Parameter> instanceDefaults(const SymbolDefinition &definition);
std::string nextInstanceName(const ice::SchematicCell &cell, const std::string &symbolName);

class SymbolLibraryDialog : public QDialog {
  public:
    explicit SymbolLibraryDialog(std::vector<SymbolDefinition> generic, bool ihpEnabled, QWidget *parent = nullptr);
    const SymbolDefinition &selected() const;

  private:
    void applyFilter(const QString &text);
    void showCurrentTab();
    void updatePreview(std::size_t index);
    QListWidget *currentList() const;

    QLineEdit *search_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QLabel *noResults_ = nullptr;
    QLabel *symbolName_ = nullptr;
    QLabel *symbolCategory_ = nullptr;
    QLabel *symbolPins_ = nullptr;
    QLabel *symbolParameters_ = nullptr;
    SymbolPreview *preview_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    std::vector<SymbolDefinition> symbols_;
    std::size_t selectedIndex_ = 0;
};

// The single properties dialog used for schematic instances. The dialog is a
// projection of a domain object: it reads and writes a plain instance struct
// and never touches the project database directly.
class SymbolPropertiesDialog : public QDialog {
  public:
    SymbolPropertiesDialog(const std::string &symbolName, const ice::SchematicInstance &instance,
                           QWidget *parent = nullptr);

    QString instanceName() const;
    ice::Coordinate positionX() const;
    ice::Coordinate positionY() const;
    ice::Orientation orientation() const;
    std::vector<ParameterEdit> parameters() const;

  private:
    struct OrientationEntry {
        QString label;
        ice::Orientation orientation;
    };

    static const std::vector<OrientationEntry> &orientations();
    void addParameterRow(ParameterEdit parameter);

    QLineEdit *name_ = nullptr;
    QSpinBox *x_ = nullptr;
    QSpinBox *y_ = nullptr;
    QComboBox *orientation_ = nullptr;
    QTableWidget *parameters_ = nullptr;
};
