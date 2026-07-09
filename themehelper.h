#pragma once
#include <QString>
#include <QMap>
#include <QApplication>
#include <QPalette>
#include <QColor>
#include <QtGlobal>

struct Theme {
    QMap<QString, QString> c;
    QString operator[](const QString& key) const { return c.value(key); }
};

inline bool isDarkColor(const QColor& col) {
    return (0.299 * col.red() + 0.587 * col.green() + 0.114 * col.blue()) < 128;
}

inline bool isDarkSystemPalette() {
    if (!qApp) return false;
    QColor col = qApp->palette().color(QPalette::Window);
    return isDarkColor(col);
}

inline void applyAccentSurface(Theme& t, const QString& mode, const QString& accent) {
    const QString m = (mode == "dark") ? "dark" : "light";
    const QString a = accent;
    auto put = [&](std::initializer_list<std::pair<const char*, const char*>> vals) {
        for (const auto& kv : vals) t.c[QString::fromLatin1(kv.first)] = QString::fromLatin1(kv.second);
    };

    if (m == "light") {
        if (a == "blue") put({{"bg","#eef5ff"},{"panel","#e4efff"},{"panel_2","#d8e8ff"},{"panel_3","#c6dcff"},{"input_bg","#fbfdff"},{"border","#aac7f6"},{"border_soft","#c7dbfb"},{"hover","#dceaff"},{"selection","#cfe1ff"},{"tool_bg","#f4f8ff"},{"tool_arg_bg","#e4efff"},{"code_bg","#eaf3ff"},{"user_label","#174ea6"},{"assistant_label","#1c6a55"}});
        else if (a == "teal") put({{"bg","#ecfdfb"},{"panel","#dff8f5"},{"panel_2","#cef0ec"},{"panel_3","#b8e6e0"},{"input_bg","#fbfffe"},{"border","#95d5ce"},{"border_soft","#b9e6e1"},{"hover","#d6f4f0"},{"selection","#c3ece7"},{"tool_bg","#f2fbfa"},{"tool_arg_bg","#dff8f5"},{"code_bg","#e7f7f5"},{"user_label","#126a70"},{"assistant_label","#277252"}});
        else if (a == "green") put({{"bg","#f0faec"},{"panel","#e4f5df"},{"panel_2","#d6edcf"},{"panel_3","#c4e2bb"},{"input_bg","#fcfffb"},{"border","#a8d39d"},{"border_soft","#c4e4bd"},{"hover","#ddf2d7"},{"selection","#cfebc7"},{"tool_bg","#f5fbf2"},{"tool_arg_bg","#e4f5df"},{"code_bg","#ecf7e8"},{"user_label","#2b6d1f"},{"assistant_label","#2f7a3e"}});
        else if (a == "orange") put({{"bg","#fff4e5"},{"panel","#ffe9cf"},{"panel_2","#ffddb5"},{"panel_3","#f6ca94"},{"input_bg","#fffdf9"},{"border","#e7b06a"},{"border_soft","#f2cf9e"},{"hover","#ffe6c5"},{"selection","#ffd8a3"},{"tool_bg","#fff8ef"},{"tool_arg_bg","#ffe9cf"},{"code_bg","#fff0da"},{"user_label","#9a4d00"},{"assistant_label","#5f6f1f"}});
        else if (a == "red") put({{"bg","#fff0f2"},{"panel","#ffe2e7"},{"panel_2","#ffd3dc"},{"panel_3","#f6bbc8"},{"input_bg","#fffafa"},{"border","#e6a0ad"},{"border_soft","#f0c1ca"},{"hover","#ffe0e6"},{"selection","#ffcbd6"},{"tool_bg","#fff6f7"},{"tool_arg_bg","#ffe2e7"},{"code_bg","#ffedf0"},{"user_label","#a30d2d"},{"assistant_label","#30704c"}});
        else if (a == "pink") put({{"bg","#fff0fa"},{"panel","#ffe3f5"},{"panel_2","#ffd4ef"},{"panel_3","#f4bde2"},{"input_bg","#fffafd"},{"border","#df9aca"},{"border_soft","#efbfdf"},{"hover","#ffe1f4"},{"selection","#ffcdec"},{"tool_bg","#fff6fc"},{"tool_arg_bg","#ffe3f5"},{"code_bg","#ffedf8"},{"user_label","#9d2a78"},{"assistant_label","#2d7054"}});
        else if (a == "purple") put({{"bg","#f7f0ff"},{"panel","#efe4ff"},{"panel_2","#e4d4ff"},{"panel_3","#d3bdf6"},{"input_bg","#fdfaff"},{"border","#b89ce6"},{"border_soft","#d2c0f0"},{"hover","#eadfff"},{"selection","#dfd0ff"},{"tool_bg","#fbf7ff"},{"tool_arg_bg","#efe4ff"},{"code_bg","#f3ebff"},{"user_label","#6d2bbd"},{"assistant_label","#32704e"}});
    } else {
        if (a == "blue") put({{"bg","#071225"},{"panel","#0b1b33"},{"panel_2","#11284a"},{"panel_3","#173762"},{"input_bg","#050b17"},{"border","#25466f"},{"border_soft","#163151"},{"hover","#122c52"},{"selection","#173b70"},{"tool_bg","#0a172b"},{"tool_arg_bg","#06101f"},{"code_bg","#050b17"},{"user_label","#89b4fa"},{"assistant_label","#94e2d5"}});
        else if (a == "teal") put({{"bg","#061a1a"},{"panel","#092626"},{"panel_2","#103a3a"},{"panel_3","#155151"},{"input_bg","#041111"},{"border","#1d5c5c"},{"border_soft","#123f3f"},{"hover","#103535"},{"selection","#164949"},{"tool_bg","#0a2020"},{"tool_arg_bg","#061515"},{"code_bg","#041111"},{"user_label","#94e2d5"},{"assistant_label","#a6e3a1"}});
        else if (a == "green") put({{"bg","#081807"},{"panel","#0d240b"},{"panel_2","#163613"},{"panel_3","#204b1c"},{"input_bg","#050f04"},{"border","#2b5d27"},{"border_soft","#1b4018"},{"hover","#173315"},{"selection","#21491d"},{"tool_bg","#0b1d09"},{"tool_arg_bg","#071406"},{"code_bg","#050f04"},{"user_label","#a6e3a1"},{"assistant_label","#94e2d5"}});
        else if (a == "orange") put({{"bg","#211306"},{"panel","#301b08"},{"panel_2","#47290e"},{"panel_3","#623813"},{"input_bg","#160c03"},{"border","#7a4a1e"},{"border_soft","#51300f"},{"hover","#3e250d"},{"selection","#5a3411"},{"tool_bg","#261607"},{"tool_arg_bg","#1b0f04"},{"code_bg","#160c03"},{"user_label","#fab387"},{"assistant_label","#a6e3a1"}});
        else if (a == "red") put({{"bg","#24080f"},{"panel","#350c17"},{"panel_2","#4c1220"},{"panel_3","#66182c"},{"input_bg","#170409"},{"border","#78263a"},{"border_soft","#551828"},{"hover","#42111d"},{"selection","#61192b"},{"tool_bg","#2a0912"},{"tool_arg_bg","#1b050b"},{"code_bg","#170409"},{"user_label","#f38ba8"},{"assistant_label","#a6e3a1"}});
        else if (a == "pink") put({{"bg","#250719"},{"panel","#360b25"},{"panel_2","#501237"},{"panel_3","#6b184a"},{"input_bg","#170410"},{"border","#7b285a"},{"border_soft","#58193f"},{"hover","#46102f"},{"selection","#631845"},{"tool_bg","#2b091d"},{"tool_arg_bg","#1b0513"},{"code_bg","#170410"},{"user_label","#f5c2e7"},{"assistant_label","#94e2d5"}});
        else if (a == "purple") put({{"bg","#170b2b"},{"panel","#21123d"},{"panel_2","#321b5a"},{"panel_3","#45257a"},{"input_bg","#0f071c"},{"border","#5b3a8e"},{"border_soft","#3a2264"},{"hover","#2c184f"},{"selection","#402371"},{"tool_bg","#1b0e33"},{"tool_arg_bg","#110820"},{"code_bg","#0f071c"},{"user_label","#cba6f7"},{"assistant_label","#a6e3a1"}});
    }
}

inline Theme makeTheme(const QString& mode, const QString& accent) {
    QString resolved = mode;
    if (resolved != "light" && resolved != "dark") resolved = isDarkSystemPalette() ? "dark" : "light";
    Theme t;
    if (resolved == "dark") {
        t.c = {{"mode","dark"},{"bg","#1e1e2e"},{"fg","#cdd6f4"},{"panel","#181825"},{"panel_2","#313244"},{"panel_3","#45475a"},{"input_bg","#11111b"},{"input_fg","#cdd6f4"},{"border","#45475a"},{"border_soft","#313244"},{"muted","#a6adc8"},{"code_bg","#11111b"},{"code_fg","#cdd6f4"},{"hover","#313244"},{"selection","#25324a"},{"tool_bg","#181825"},{"tool_arg_bg","#11111b"},{"user_label","#89b4fa"},{"assistant_label","#a6e3a1"},{"pygments_style","monokai"},{"syntax_keyword","#66d9ef"},{"syntax_string","#e6db74"},{"syntax_comment","#959077"},{"syntax_number","#ae81ff"}};
    } else {
        t.c = {{"mode","light"},{"bg","#ffffff"},{"fg","#1e1e2e"},{"panel","#f8f9fb"},{"panel_2","#f0f2f5"},{"panel_3","#e8edf5"},{"input_bg","#ffffff"},{"input_fg","#1e1e2e"},{"border","#c9ced6"},{"border_soft","#dde2ea"},{"muted","#667085"},{"code_bg","#f5f7fa"},{"code_fg","#27313f"},{"hover","#edf2fa"},{"selection","#e8f0fe"},{"tool_bg","#fafbfc"},{"tool_arg_bg","#f0f2f5"},{"user_label","#0b3d91"},{"assistant_label","#0f6b3f"},{"pygments_style","friendly"},{"syntax_keyword","#007020"},{"syntax_string","#4070a0"},{"syntax_comment","#60a0b0"},{"syntax_number","#40a070"}};
    }

    QMap<QString, QString> primary = {{"default","#1e66f5"},{"blue","#1e66f5"},{"teal","#179299"},{"green","#40a02b"},{"orange","#df8e1d"},{"red","#d20f39"},{"pink","#ea76cb"},{"purple","#8839ef"}};
    QMap<QString, QString> hover = {{"default","#4478f7"},{"blue","#4478f7"},{"teal","#1fa9b1"},{"green","#56b641"},{"orange","#fea82f"},{"red","#e64553"},{"pink","#f18bd4"},{"purple","#9b5cf6"}};
    QMap<QString, QString> secondary = {{"default","#89b4fa"},{"blue","#89b4fa"},{"teal","#94e2d5"},{"green","#a6e3a1"},{"orange","#fab387"},{"red","#f38ba8"},{"pink","#f5c2e7"},{"purple","#cba6f7"}};
    QMap<QString, QString> link = {{"default","#1e66f5"},{"blue","#1e66f5"},{"teal","#179299"},{"green","#40a02b"},{"orange","#df8e1d"},{"red","#d20f39"},{"pink","#c94eb3"},{"purple","#8839ef"}};
    QString a = primary.contains(accent) ? accent : "default";
    applyAccentSurface(t, resolved, a);
    t.c["accent_name"] = a;
    t.c["primary"] = primary[a];
    t.c["primary_hover"] = hover[a];
    t.c["primary_fg"] = (a == "pink") ? "#2b1224" : "#ffffff";
    t.c["secondary"] = secondary[a];
    t.c["link"] = link[a];
    t.c["focus"] = secondary[a];
    t.c["danger"] = "#d20f39"; t.c["danger_hover"] = "#e64553";
    t.c["success"] = "#40a02b"; t.c["success_soft"] = "#a6e3a1";
    t.c["warning"] = "#df8e1d"; t.c["warning_hover"] = "#fea82f";
    t.c["running"] = "#fab387"; t.c["declined"] = "#d20f39";
    return t;
}

// main.cpp bakes ui_scale into QT_SCALE_FACTOR at launch, which makes Qt natively
// scale every logical pixel (fonts and widget geometry alike) for the whole app.
// scaledSize()/scaledFont() must divide that back out, or a value already covered
// by QT_SCALE_FACTOR gets multiplied again here. Before a restart QT_SCALE_FACTOR
// still reflects the old setting, so this correctly yields a live-preview delta;
// after a restart it collapses to a no-op once the two agree.
inline double dpiScaleAlreadyApplied() {
    bool ok = false;
    double v = qEnvironmentVariable("QT_SCALE_FACTOR").toDouble(&ok);
    return (ok && v > 0) ? v : 1.0;
}

inline int scaledSize(int px, int scale) {
    return qMax(1, qRound(px * (qBound(50, scale, 300) / 100.0) / dpiScaleAlreadyApplied()));
}
inline double scaledFont(int pt, int scale) {
    return qMax(1.0, pt * (qBound(50, scale, 300) / 100.0) / dpiScaleAlreadyApplied());
}

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
QPushButton:pressed { background-color:%6; }
QPushButton:disabled { color:%12; background-color:%5; border-color:%3; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QComboBox { background-color:%13; color:%14; border:1px solid %4; border-radius:6px; padding:4px 6px; selection-background-color:%11; selection-color:%15; }
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus { border-color:%16; }
QDialog { background-color:%1; color:%2; }
QMenu { background-color:%5; color:%2; border:1px solid %4; }
QMenu::item:selected { background-color:%6; }
)" ).arg(t["bg"], t["fg"], t["border_soft"], t["border"], t["panel"], t["selection"], t["hover"], t["panel_2"])
    .arg(padV).arg(padH).arg(t["primary"], t["muted"], t["input_bg"], t["input_fg"], t["primary_fg"], t["focus"]);
}
