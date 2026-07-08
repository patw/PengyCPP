#pragma once
#include "config.h"
#include <QString>
#include <QMap>
#include <QApplication>
#include <QPalette>

struct Theme {
    QMap<QString, QString> c;
    QString operator[](const QString& key) const { return c.value(key); }
};

inline bool isDarkSystemPalette() {
    if (!qApp) return false;
    QColor col = qApp->palette().color(QPalette::Window);
    return (0.299 * col.red() + 0.587 * col.green() + 0.114 * col.blue()) < 128;
}

inline Theme makeTheme(const QString& mode, const QString& accent) {
    QString resolved = mode;
    if (resolved != "light" && resolved != "dark") resolved = isDarkSystemPalette() ? "dark" : "light";
    Theme t;
    if (resolved == "dark") {
        t.c = {{"bg","#1e1e2e"},{"fg","#cdd6f4"},{"panel","#181825"},{"panel_2","#313244"},{"input_bg","#11111b"},{"input_fg","#cdd6f4"},{"border","#45475a"},{"border_soft","#313244"},{"muted","#a6adc8"},{"code_bg","#11111b"},{"code_fg","#cdd6f4"},{"hover","#313244"},{"selection","#25324a"},{"tool_bg","#181825"},{"tool_arg_bg","#11111b"},{"user_label","#89b4fa"},{"assistant_label","#a6e3a1"}};
    } else {
        t.c = {{"bg","#ffffff"},{"fg","#1e1e2e"},{"panel","#f8f9fb"},{"panel_2","#f0f2f5"},{"input_bg","#ffffff"},{"input_fg","#1e1e2e"},{"border","#c9ced6"},{"border_soft","#dde2ea"},{"muted","#667085"},{"code_bg","#f5f7fa"},{"code_fg","#27313f"},{"hover","#edf2fa"},{"selection","#e8f0fe"},{"tool_bg","#fafbfc"},{"tool_arg_bg","#f0f2f5"},{"user_label","#0b3d91"},{"assistant_label","#0f6b3f"}};
    }
    QMap<QString, QString> accents = {{"default","#1e66f5"},{"blue","#1e66f5"},{"teal","#179299"},{"green","#40a02b"},{"orange","#df8e1d"},{"red","#d20f39"},{"pink","#ea76cb"},{"purple","#8839ef"}};
    QMap<QString, QString> hover = {{"default","#4478f7"},{"blue","#4478f7"},{"teal","#1fa9b1"},{"green","#56b641"},{"orange","#fea82f"},{"red","#e64553"},{"pink","#f18bd4"},{"purple","#9b5cf6"}};
    QString a = accents.contains(accent) ? accent : "default";
    t.c["primary"] = accents[a];
    t.c["primary_hover"] = hover[a];
    t.c["primary_fg"] = (a == "pink") ? "#2b1224" : "#ffffff";
    t.c["danger"] = "#d20f39"; t.c["danger_hover"] = "#e64553";
    t.c["warning"] = "#df8e1d"; t.c["warning_hover"] = "#fea82f";
    t.c["running"] = "#fab387"; t.c["success_soft"] = "#a6e3a1";
    t.c["mode"] = resolved;
    return t;
}

inline int scaledSize(int px, int scale) { return qMax(1, qRound(px * qBound(50, scale, 300) / 100.0)); }
inline int scaledFont(int pt, int scale) { return qMax(1, qRound(pt * qBound(50, scale, 300) / 100.0)); }

inline QString appStyleSheet(const Theme& t, int scale) {
    int padV = scaledSize(5, scale), padH = scaledSize(10, scale);
    return QString(R"(
QMainWindow, QWidget { background-color:%1; color:%2; }
QSplitter::handle { background-color:%3; }
QFrame { color:%2; border-color:%4; }
QLabel { color:%2; }
QListWidget { background-color:%5; color:%2; border:1px solid %3; border-radius:6px; outline:none; }
QListWidget::item { color:%2; padding:4px; border-radius:6px; }
QListWidget::item:selected { background-color:%6; color:%2; }
QListWidget::item:hover { background-color:%7; }
QPushButton { background-color:%8; color:%2; border:1px solid %4; border-radius:8px; padding:%9px %10px; }
QPushButton:hover { background-color:%7; border-color:%11; }
QPushButton:disabled { color:%12; background-color:%5; border-color:%3; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox { background-color:%13; color:%14; border:1px solid %4; border-radius:6px; padding:4px 6px; selection-background-color:%11; selection-color:%15; }
QDialog { background-color:%1; color:%2; }
QMenu { background-color:%5; color:%2; border:1px solid %4; }
QMenu::item:selected { background-color:%6; }
)" ).arg(t["bg"], t["fg"], t["border_soft"], t["border"], t["panel"], t["selection"], t["hover"], t["panel_2"])
    .arg(padV).arg(padH).arg(t["primary"], t["muted"], t["input_bg"], t["input_fg"], t["primary_fg"]);
}
