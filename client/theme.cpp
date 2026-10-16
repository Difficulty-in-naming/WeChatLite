#include "theme.h"
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStyleHints>
#include <QWidget>

namespace
{
// Keep the original light rules so the reference appearance is preserved.
const char lightStyle[] = R"(
        QWidget { color:#19191a; }
        QMainWindow, QDialog { background:#fafafa; }
        QLabel { background:transparent; }
        #titleBar { background:#e0e0e0; }
        #sidebar, #groups { background:#eeeef0; border:0; outline:0; }
        #main, #messages, #composeArea { background:#fafafa; border:0; }
        #composeFrame { background:#fafafa; border:1px solid #e3e3e3; border-radius:9px; }
        #chatTitle { font-size:14px; font-weight:400; color:#19191a; }
        #muted { color:#9f9fa6; font-size:12px; }
        #footnote { color:#9f9fa6; font-size:11px; }
        #demoLabel { color:#8a8a8a; font-size:11px; }
        #divider { background:#f1f1f1; border:0; }
        #composer { background:transparent; border:0; font-size:14px; selection-background-color:#b7d6f1; padding:0; }
        QLineEdit { border:1px solid #dedede; border-radius:5px; background:#fafafa; padding:6px 9px; selection-background-color:#b7d6f1; }
        QLineEdit:focus { border:1px solid #bcbcbc; }
        #search { background:#fafafa; border:0; padding:0 8px; color:#333333; font-size:12px; }
        QPushButton { background:#f3f3f3; border:1px solid #dedede; border-radius:4px; padding:6px 12px; }
        QPushButton:hover { background:#e9e9e9; }
        QPushButton:pressed { background:#dedede; }
        QPushButton:disabled { color:#b0b0b0; background:#efefef; border-color:#e8e8e8; }
        #sendButton { background:#e9e9e9; color:#07a569; border:0; font-size:12px; padding:0; }
        #sendButton:hover { background:#dedede; }
        #sendButton:disabled { background:#ededed; color:#b0b0b0; }
        #iconButton, #attachButton, #windowButton, #closeWindow { background:transparent; border:0; border-radius:3px; padding:0; }
        #iconButton:hover, #attachButton:hover { background:#e5e5e5; }
        #windowButton:hover { background:#d4d4d4; }
        #closeWindow:hover { background:#e55555; }
        #textButton { background:transparent; border:0; color:#9f9fa6; padding:2px 0; font-size:11px; }
        #attachmentCard { background:#ffffff; border:1px solid #dedede; border-radius:5px; }
        #fileType { color:#e49338; font-size:16px; padding:0 5px; }
        #removeAttachment { background:transparent; border:0; color:#999999; padding:0; font-size:18px; }
        #mentionPopup { font-size:14px; background:#ededed; border:1px solid #d3d3d3; border-radius:6px; padding:3px; }
        #mentionPopup::item { border-radius:3px; color:#19191a; padding-left:1px; }
        #mentionPopup::item:selected { background:#d4d4d4; color:#19191a; }
        QScrollBar:vertical { background:transparent; width:6px; margin:2px 0; }
        QScrollBar::handle:vertical { background:#d2d2d2; border-radius:3px; min-height:24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { height:6px; background:transparent; }
        QScrollBar::handle:horizontal { background:#d2d2d2; border-radius:3px; min-width:24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
        QSplitter::handle { background:#dedede; }
        #announcementDialog { background:#fafafa; }
        #announcementBody { background:transparent; border:0; padding:0; color:#19191a; }
        #announcementUpdated, #announcementState, #announcementReadOnly { color:#9f9fa6; font-size:12px; }
        #announcementRetry { background:transparent; color:#334e88; border:0; padding:3px; }
    )";

const char darkStyle[] = R"(
        QWidget { color:#ededed; }
        QMainWindow, QDialog { background:#202020; }
        QLabel { background:transparent; }
        QComboBox, QAbstractSpinBox { color:#ededed; background:#292929; border:1px solid #44474b; border-radius:4px; padding:4px 7px; }
        QComboBox:disabled, QAbstractSpinBox:disabled { color:#777c81; background:#292b2d; }
        QComboBox QAbstractItemView { color:#ededed; background:#292929; selection-background-color:#335d79; selection-color:#ffffff; }
        QPlainTextEdit, QTextEdit, QTextBrowser, QListWidget, QTreeWidget, QTableWidget { color:#ededed; background-color:#292929; border:1px solid #44474b; selection-background-color:#335d79; selection-color:#ffffff; }
        QTabWidget::pane { border:1px solid #3a3d40; }
        QTabBar::tab { color:#b7bbc1; background:#292929; border:1px solid #3a3d40; padding:5px 10px; }
        QTabBar::tab:selected { color:#ffffff; background:#3a3d40; }
        QTabBar::tab:disabled { color:#777c81; }
        QHeaderView::section { color:#ededed; background:#333638; border:1px solid #44474b; }
        QMenu { color:#ededed; background:#292929; border:1px solid #44474b; }
        QMenu::item:selected { color:#ffffff; background:#335d79; }
        QMenu::item:disabled { color:#777c81; }
        #titleBar { background:#181818; }
        #sidebar, #groups { background:#262626; border:0; outline:0; }
        #main, #messages, #composeArea { background:#202020; border:0; }
        #composeFrame { background:#202020; border:1px solid #3a3d40; border-radius:9px; }
        #chatTitle { font-size:14px; font-weight:400; color:#ededed; }
        #muted { color:#999ca3; font-size:12px; }
        #footnote { color:#999ca3; font-size:11px; }
        #demoLabel { color:#a5a5a5; font-size:11px; }
        #divider { background:#2c2e30; border:0; }
        #composer { background:transparent; border:0; font-size:14px; selection-background-color:#335d79; selection-color:#ffffff; padding:0; }
        QLineEdit { border:1px solid #44474b; border-radius:5px; background:#292929; padding:6px 9px; selection-background-color:#335d79; selection-color:#ffffff; }
        QLineEdit:focus { border:1px solid #777d84; }
        #search { background:#333333; border:0; padding:0 8px; color:#ededed; font-size:12px; }
        QPushButton { background:#333638; border:1px solid #44474b; border-radius:4px; padding:6px 12px; }
        QPushButton:hover { background:#424649; }
        QPushButton:pressed { background:#4d5357; }
        QPushButton:disabled { color:#777c81; background:#292b2d; border-color:#34373a; }
        #sendButton { background:#34383a; color:#49d49a; border:0; font-size:12px; padding:0; }
        #sendButton:hover { background:#41474a; }
        #sendButton:disabled { background:#2d3032; color:#777c81; }
        #iconButton, #attachButton, #windowButton, #closeWindow { background:transparent; border:0; border-radius:3px; padding:0; }
        #iconButton:hover, #attachButton:hover { background:#383b3e; }
        #windowButton:hover { background:#333639; }
        #closeWindow:hover { background:#c74b4b; }
        #textButton { background:transparent; border:0; color:#a9adb3; padding:2px 0; font-size:11px; }
        #attachmentCard { background:#292929; border:1px solid #44474b; border-radius:5px; }
        #fileType { color:#efb161; font-size:16px; padding:0 5px; }
        #removeAttachment { background:transparent; border:0; color:#b7bbc1; padding:0; font-size:18px; }
        #mentionPopup { font-size:14px; background:#303030; border:1px solid #4b4f53; border-radius:6px; padding:3px; }
        #mentionPopup::item { border-radius:3px; color:#ededed; padding-left:1px; }
        #mentionPopup::item:selected { background:#464b50; color:#ffffff; }
        QScrollBar:vertical { background:transparent; width:6px; margin:2px 0; }
        QScrollBar::handle:vertical { background:#555b60; border-radius:3px; min-height:24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { height:6px; background:transparent; }
        QScrollBar::handle:horizontal { background:#555b60; border-radius:3px; min-width:24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width:0; }
        QSplitter::handle { background:#44474b; }
        #announcementDialog { background:#202020; }
        #announcementBody { background:transparent; border:0; padding:0; color:#ededed; }
        #announcementUpdated, #announcementState, #announcementReadOnly { color:#999ca3; font-size:12px; }
        #announcementRetry { background:transparent; color:#75bfff; border:0; padding:3px; }
    )";

QString normalized(const QString &mode)
{
    return mode == "light" || mode == "dark" ? mode : QStringLiteral("system");
}

QPalette colors(bool dark)
{
    QPalette palette(QColor(dark ? "#333638" : "#f3f3f3"));
    palette.setColor(QPalette::Window, QColor(dark ? "#202020" : "#fafafa"));
    palette.setColor(QPalette::Base, QColor(dark ? "#292929" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(dark ? "#303234" : "#f3f3f3"));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText})
        palette.setColor(role, QColor(dark ? "#ededed" : "#19191a"));
    palette.setColor(QPalette::ToolTipBase, QColor(dark ? "#333638" : "#fafafa"));
    palette.setColor(QPalette::PlaceholderText, QColor(dark ? "#999ca3" : "#9f9fa6"));
    palette.setColor(QPalette::Highlight, QColor(dark ? "#335d79" : "#b7d6f1"));
    palette.setColor(QPalette::HighlightedText, QColor(dark ? "#ffffff" : "#19191a"));
    palette.setColor(QPalette::Link, QColor(dark ? "#75bfff" : "#334e88"));
    palette.setColor(QPalette::LinkVisited, QColor(dark ? "#b8a6e8" : "#7557a4"));
    palette.setColor(QPalette::Accent, QColor(dark ? "#49d49a" : "#07a569"));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(dark ? "#777c81" : "#b0b0b0"));
    if (dark)
    {
        palette.setColor(QPalette::Light, QColor("#555b60"));
        palette.setColor(QPalette::Midlight, QColor("#44474b"));
        palette.setColor(QPalette::Mid, QColor("#3a3d40"));
        palette.setColor(QPalette::Dark, QColor("#191b1d"));
        palette.setColor(QPalette::Shadow, QColor("#101112"));
    }
    return palette;
}

class Controller final : public QObject
{
  public:
    explicit Controller(QObject *parent) : QObject(parent)
    {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { queueRefresh(); });
    }
    QString selected = "system";
    void select(const QString &value, bool persist)
    {
        selected = normalized(value);
        if (persist)
            QSettings().setValue("themeMode", selected);
        const auto scheme = selected == "light" ? Qt::ColorScheme::Light :
                            selected == "dark" ? Qt::ColorScheme::Dark : Qt::ColorScheme::Unknown;
        QGuiApplication::styleHints()->setColorScheme(scheme);
        refresh();
    }
  private:
    bool queued = false;
    bool refreshing = false;
    void queueRefresh()
    {
        if (queued)
            return;
        queued = true;
        QMetaObject::invokeMethod(this, [this]
        {
            queued = false;
            refresh(); // Resolve the current request, never a captured obsolete scheme signal.
        }, Qt::QueuedConnection);
    }
    void refresh()
    {
        if (refreshing)
        {
            queueRefresh();
            return;
        }
        refreshing = true;
        const bool dark = selected == "dark" || (selected == "system" &&
            QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        const auto palette = colors(dark);
        if (qApp->palette() != palette)
            qApp->setPalette(palette);
        const auto style = QString::fromLatin1(dark ? darkStyle : lightStyle);
        if (qApp->styleSheet() != style)
            qApp->setStyleSheet(style);
        for (auto *widget : QApplication::allWidgets())
            widget->update();
        refreshing = false;
    }
};
QPointer<Controller> controller;
Controller *instance()
{
    if (!controller && qApp)
        controller = new Controller(qApp);
    return controller;
}
}

void Theme::initialize(const QString &mode)
{
    if (auto *theme = instance())
        theme->select(mode, false);
}
void Theme::apply(const QString &mode)
{
    if (auto *theme = instance())
        theme->select(mode, true);
}
QString Theme::mode()
{
    return controller ? controller->selected : normalized(QSettings().value("themeMode", "system").toString());
}
bool Theme::isDark()
{
    return qApp && qApp->palette().color(QPalette::Window).lightness() < 128;
}
