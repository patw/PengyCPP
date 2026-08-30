#pragma once

#include "themehelper.h"
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QSvgRenderer>
#include <QToolButton>

inline QPixmap renderPengyIcon(const QString& name, const QString& color, int size) {
    QFile file(QString(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    svg.replace("__COLOR__", color.toUtf8());
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

// Cached by (name, fg color, active color, muted color): callers like the
// chat-history sidebar request the exact same icon for every row's
// Save/Delete buttons, and rebuilding a 5-size x 3-state QIcon (15
// QSvgRenderer + QPainter passes) from scratch on every call was the
// dominant cost behind "New Chat feels slow" once the sidebar had more than
// a handful of chats. A QIcon is safe to share: nothing here or downstream
// mutates one after construction. A function-local static in an inline
// function is one shared object across translation units per the C++
// standard, so this is safe without a separate .cpp.
inline QIcon pengyIcon(const QString& name, const Theme& theme,
                       const QString& colorRole = "fg",
                       const QString& activeRole = "primary") {
    static QHash<QString, QIcon> cache;
    const QString key = name + QLatin1Char('|') + theme[colorRole] + QLatin1Char('|')
                       + theme[activeRole] + QLatin1Char('|') + theme["muted"];
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return it.value();

    QIcon icon;
    for (int size : {16, 20, 24, 32, 48}) {
        icon.addPixmap(renderPengyIcon(name, theme[colorRole], size), QIcon::Normal);
        icon.addPixmap(renderPengyIcon(name, theme[activeRole], size), QIcon::Active);
        icon.addPixmap(renderPengyIcon(name, theme["muted"], size), QIcon::Disabled);
    }
    cache.insert(key, icon);
    return icon;
}

inline void applyPengyIcon(QAbstractButton* button, const QString& name,
                           const Theme& theme, int size = 16,
                           const QString& colorRole = "fg",
                           const QString& activeRole = "primary") {
    button->setProperty("pengyIcon", name);
    button->setIcon(pengyIcon(name, theme, colorRole, activeRole));
    button->setIconSize(QSize(size, size));
}
