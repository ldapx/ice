#include "theme.hpp"

#include <QApplication>
#include <QSettings>
#include <QTextStream>
#include <QWidget>

#include <algorithm>

namespace ice {
namespace {
constexpr std::array<ThemeEntry, Theme::roleCount> kEntries = {{
    {ThemeRole::Accent1, "accent-1", "Accent 1", "#fff47e", "#b8860b"},
    {ThemeRole::Accent2, "accent-2", "Accent 2", "#fffde2", "#a88f00"},
    {ThemeRole::Accent3, "accent-3", "Accent 3", "#6afdff", "#008b8b"},
    {ThemeRole::Accent4, "accent-4", "Accent 4", "#828aff", "#3f51b5"},
    {ThemeRole::Accent5, "accent-5", "Accent 5", "#9a16ff", "#7a00cc"},
    {ThemeRole::Accent6, "accent-6", "Accent 6", "#bc4fff", "#9c27b0"},
    {ThemeRole::Background1, "background-1", "Background 1", "#272730", "#f2ead4"},
    {ThemeRole::Background2, "background-2", "Background 2", "#19191c", "#fffde2"},
    {ThemeRole::Surface1, "surface-1", "Surface 1", "#31313c", "#f7f0dc"},
    {ThemeRole::Border1, "border-1", "Border 1", "#464652", "#c8b98a"},
    {ThemeRole::Text1, "text-1", "Text 1", "#fffde2", "#272730"},
    {ThemeRole::Text2, "text-2", "Text 2", "#a8a8b3", "#6b6b76"},
    {ThemeRole::TextOnAccent, "text-on-accent", "Text on accent", "#19191c", "#fffde2"},
    {ThemeRole::Grid1, "grid-1", "Grid 1", "#2a2a32", "#e8e0c8"},
    {ThemeRole::Grid2, "grid-2", "Grid 2", "#383842", "#cfc6aa"},
    {ThemeRole::Axis1, "axis-1", "Axis 1", "#9a9aa6", "#272730"},
}};

std::size_t indexOf(ThemeRole role) { return static_cast<std::size_t>(role); }
} // namespace

Theme::Theme() { resetToDefaults(); }

Theme &Theme::instance() {
    static Theme theme;
    return theme;
}

const std::array<ThemeEntry, Theme::roleCount> &Theme::entries() const { return kEntries; }

QColor Theme::color(ThemeRole role) const { return colors_[indexOf(role)]; }

QColor Theme::defaultColor(ThemeRole role) const {
    const auto &entry = kEntries[indexOf(role)];
    return QColor(QString::fromLatin1(preset_ == ThemePreset::Light ? entry.lightHex : entry.darkHex));
}

void Theme::setColor(ThemeRole role, const QColor &color) {
    if (color.isValid())
        colors_[indexOf(role)] = color;
}

void Theme::resetToDefaults() {
    for (const auto &entry : kEntries)
        colors_[indexOf(entry.role)] = defaultColor(entry.role);
}

void Theme::applyPreset(ThemePreset preset) {
    preset_ = preset;
    resetToDefaults();
}

void Theme::load() {
    QSettings settings;
    const auto presetName = settings.value(QStringLiteral("theme/preset"), QStringLiteral("dark")).toString();
    preset_ =
        presetName.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0 ? ThemePreset::Light : ThemePreset::Dark;
    resetToDefaults();
    for (const auto &entry : kEntries) {
        const auto value =
            settings.value(QStringLiteral("theme/") + QString::fromLatin1(entry.name), defaultColor(entry.role).name())
                .toString();
        const QColor color(value);
        if (color.isValid())
            colors_[indexOf(entry.role)] = color;
    }
}

void Theme::save() const {
    QSettings settings;
    settings.setValue(QStringLiteral("theme/preset"),
                      preset_ == ThemePreset::Light ? QStringLiteral("light") : QStringLiteral("dark"));
    for (const auto &entry : kEntries)
        settings.setValue(QStringLiteral("theme/") + QString::fromLatin1(entry.name),
                          colors_[indexOf(entry.role)].name());
}

QPalette Theme::palette() const {
    const auto text = color(ThemeRole::Text1);
    const auto muted = color(ThemeRole::Text2);
    QPalette result;
    result.setColor(QPalette::Window, color(ThemeRole::Background2));
    result.setColor(QPalette::WindowText, text);
    result.setColor(QPalette::Base, color(ThemeRole::Background1));
    result.setColor(QPalette::AlternateBase, color(ThemeRole::Surface1));
    result.setColor(QPalette::ToolTipBase, color(ThemeRole::Background1));
    result.setColor(QPalette::ToolTipText, text);
    result.setColor(QPalette::Text, text);
    result.setColor(QPalette::Button, color(ThemeRole::Surface1));
    result.setColor(QPalette::ButtonText, text);
    result.setColor(QPalette::BrightText, color(ThemeRole::Accent1));
    result.setColor(QPalette::Highlight, color(ThemeRole::Accent4));
    result.setColor(QPalette::HighlightedText, color(ThemeRole::TextOnAccent));
    result.setColor(QPalette::Link, color(ThemeRole::Accent3));
    result.setColor(QPalette::PlaceholderText, muted);
    result.setColor(QPalette::Disabled, QPalette::Text, muted);
    result.setColor(QPalette::Disabled, QPalette::ButtonText, muted);
    result.setColor(QPalette::Disabled, QPalette::WindowText, muted);
    return result;
}

QString Theme::styleSheet() const {
    const auto accent1 = color(ThemeRole::Accent1);
    const auto accent3 = color(ThemeRole::Accent3);
    const auto accent4 = color(ThemeRole::Accent4);
    const auto accent5 = color(ThemeRole::Accent5);
    const auto background1 = color(ThemeRole::Background1);
    const auto background2 = color(ThemeRole::Background2);
    const auto surface1 = color(ThemeRole::Surface1);
    const auto border1 = color(ThemeRole::Border1);
    const auto text = color(ThemeRole::Text1);
    const auto muted = color(ThemeRole::Text2);
    const auto onAccent = color(ThemeRole::TextOnAccent);

    QString css;
    QTextStream out(&css);
    out << "QWidget { background-color: " << background2.name() << "; color: " << text.name() << "; }\n"
        << "QMainWindow, QDialog { background-color: " << background2.name() << "; }\n"
        << "QLabel, QCheckBox, QRadioButton { background: transparent; }\n"
        << "QMenuBar, QToolBar, QStatusBar { background-color: " << background2.name() << "; color: " << text.name()
        << "; border: none; }\n"
        << "QMenuBar::item { background: transparent; padding: 4px 8px; }\n"
        << "QMenuBar::item:selected { background: " << accent5.name() << "; color: " << onAccent.name() << "; }\n"
        << "QMenu { background: " << background1.name() << "; color: " << text.name() << "; border: 1px solid "
        << border1.name() << "; }\n"
        << "QMenu::item { padding: 4px 20px; }\n"
        << "QMenu::item:selected { background: " << accent5.name() << "; color: " << onAccent.name() << "; }\n"
        << "QMenu::separator { height: 1px; background: " << border1.name() << "; margin: 4px 8px; }\n"
        << "QToolButton, QPushButton { background: " << surface1.name() << "; color: " << text.name()
        << "; border: 1px solid " << border1.name() << "; border-radius: 3px; padding: 4px 8px; }\n"
        << "QToolButton:hover, QPushButton:hover { background: " << accent4.name() << "; color: " << onAccent.name()
        << "; border-color: " << accent4.name() << "; }\n"
        << "QToolButton:pressed, QPushButton:pressed { background: " << accent5.name() << "; color: " << onAccent.name()
        << "; }\n"
        << "QToolButton:checked, QPushButton:checked { background: " << accent4.name() << "; color: " << onAccent.name()
        << "; border-color: " << accent4.name() << "; }\n"
        << "QToolButton:disabled, QPushButton:disabled { background: " << background1.name()
        << "; color: " << muted.name() << "; }\n"
        << "QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QListWidget, QTreeWidget, "
           "QTableWidget { background: "
        << background1.name() << "; color: " << text.name() << "; border: 1px solid " << border1.name()
        << "; selection-background-color: " << accent4.name() << "; selection-color: " << onAccent.name() << "; }\n"
        << "QComboBox QAbstractItemView { background: " << background1.name() << "; color: " << text.name()
        << "; selection-background-color: " << accent4.name() << "; selection-color: " << onAccent.name() << "; }\n"
        << "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus, "
           "QDoubleSpinBox:focus { border: 1px solid "
        << accent3.name() << "; }\n"
        << "QHeaderView::section { background: " << surface1.name() << "; color: " << text.name()
        << "; border: 1px solid " << border1.name() << "; padding: 3px; }\n"
        << "QTreeView::item:hover, QListView::item:hover, QTableView::item:hover { background: " << surface1.name()
        << "; }\n"
        << "QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: "
        << accent4.name() << "; color: " << onAccent.name() << "; }\n"
        << "QTabWidget::pane { border: 1px solid " << border1.name() << "; background: " << background2.name()
        << "; }\n"
        << "QTabBar::tab { background: " << background1.name() << "; color: " << text.name() << "; padding: 5px 10px; "
        << "border: 1px solid " << border1.name() << "; border-bottom: none; }\n"
        << "QTabBar::tab:selected { background: " << accent5.name() << "; color: " << onAccent.name() << "; }\n"
        << "QTabBar::tab:hover:!selected { background: " << surface1.name() << "; }\n"
        << "QDockWidget { color: " << text.name() << "; }\n"
        << "QDockWidget::title { background: " << background1.name() << "; padding: 4px; }\n"
        << "QGroupBox { border: 1px solid " << border1.name()
        << "; border-radius: 4px; margin-top: 10px; padding-top: 6px; }\n"
        << "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: " << accent1.name()
        << "; }\n"
        << "QGraphicsView { border: none; }\n"
        << "QToolTip { background: " << background1.name() << "; color: " << text.name() << "; border: 1px solid "
        << accent4.name() << "; padding: 3px; }\n"
        << "QScrollBar:vertical { background: " << background2.name() << "; width: 10px; margin: 0; }\n"
        << "QScrollBar::handle:vertical { background: " << accent4.name() << "; min-height: 20px; border-radius: 5px; "
        << "}\n"
        << "QScrollBar:horizontal { background: " << background2.name() << "; height: 10px; margin: 0; }\n"
        << "QScrollBar::handle:horizontal { background: " << accent4.name() << "; min-width: 20px; border-radius: 5px; "
        << "}\n"
        << "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }\n"
        << "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
        << "QProgressBar { background: " << background1.name() << "; border: 1px solid " << border1.name()
        << "; border-radius: 3px; text-align: center; color: " << text.name() << "; }\n"
        << "QProgressBar::chunk { background: " << accent4.name() << "; }\n"
        << "QSplitter::handle { background: " << border1.name() << "; }\n"
        << "QStatusBar QLabel { background: transparent; }\n";
    return css;
}

void Theme::applyToApplication() const {
    if (!qApp)
        return;
    qApp->setPalette(palette());
    qApp->setStyleSheet(styleSheet());
    for (auto *widget : QApplication::allWidgets())
        widget->update();
}

} // namespace ice
