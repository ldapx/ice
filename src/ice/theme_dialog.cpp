#include "theme_dialog.hpp"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <cstddef>

namespace ice {

ThemeDialog::ThemeDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Theme Palette"));
    resize(540, 620);

    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        QStringLiteral("Choose the colors used by the editor. Palette changes are saved and applied immediately "
                       "after accepting."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setColumnStretch(1, 1);

    const auto &entries = Theme::instance().entries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        colors_[index] = Theme::instance().color(entry.role);

        auto *name = new QLabel(
            QStringLiteral("%1  (%2)").arg(QString::fromLatin1(entry.name), QString::fromLatin1(entry.label)));
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(name, static_cast<int>(index), 0);

        auto *swatch = new QPushButton;
        swatch->setMinimumWidth(150);
        swatches_[index] = swatch;
        updateSwatch(index);
        connect(swatch, &QPushButton::clicked, this, [this, index, entry] {
            const auto chosen = QColorDialog::getColor(colors_[index], this,
                                                       QStringLiteral("Choose %1").arg(QString::fromLatin1(entry.name)),
                                                       QColorDialog::DontUseNativeDialog);
            if (!chosen.isValid())
                return;
            colors_[index] = chosen;
            updateSwatch(index);
        });
        grid->addWidget(swatch, static_cast<int>(index), 1);
    }

    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *reset = buttons->addButton(QStringLiteral("Reset defaults"), QDialogButtonBox::ResetRole);
    connect(reset, &QPushButton::clicked, this, [this] { resetSwatches(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        auto &theme = Theme::instance();
        const auto &entries = theme.entries();
        for (std::size_t index = 0; index < entries.size(); ++index)
            theme.setColor(entries[index].role, colors_[index]);
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ThemeDialog::updateSwatch(std::size_t index) {
    auto *swatch = swatches_[index];
    if (!swatch)
        return;
    const auto color = colors_[index];
    const auto text = color.lightness() > 140 ? QColor(Qt::black) : QColor(Qt::white);
    swatch->setText(color.name().toUpper());
    swatch->setStyleSheet(QStringLiteral("background-color: %1; color: %2; border: 1px solid %3; padding: 4px;")
                              .arg(color.name(), text.name(), themeColor(ThemeRole::Border1).name()));
}

void ThemeDialog::resetSwatches() {
    const auto &entries = Theme::instance().entries();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        colors_[index] = Theme::instance().defaultColor(entries[index].role);
        updateSwatch(index);
    }
}

} // namespace ice
