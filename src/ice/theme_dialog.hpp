#pragma once

#include "theme.hpp"

#include <QDialog>

#include <array>

class QPushButton;

namespace ice {

// Edits a working copy of the theme palette. Colors are written into Theme
// only when the dialog is accepted.
class ThemeDialog : public QDialog {
  public:
    explicit ThemeDialog(QWidget *parent = nullptr);

  private:
    void updateSwatch(std::size_t index);
    void resetSwatches();

    std::array<QPushButton *, Theme::roleCount> swatches_{};
    std::array<QColor, Theme::roleCount> colors_{};
};

} // namespace ice
