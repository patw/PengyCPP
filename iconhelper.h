#pragma once

#include "themehelper.h"
#include <QFile>
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

inline QIcon pengyIcon(const QString& name, const Theme& theme,
                       const QString& colorRole = "fg",
                       const QString& activeRole = "primary") {
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48}) {
        icon.addPixmap(renderPengyIcon(name, theme[colorRole], size), QIcon::Normal);
        icon.addPixmap(renderPengyIcon(name, theme[activeRole], size), QIcon::Active);
        icon.addPixmap(renderPengyIcon(name, theme["muted"], size), QIcon::Disabled);
    }
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
