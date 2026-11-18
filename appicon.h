#pragma once

#include <QColorSpace>
#include <QIcon>
#include <QImage>
#include <QPixmap>

inline QIcon pengyApplicationIcon() {
#ifdef Q_OS_MACOS
    // Qt 6.11.1's QImage::toCGImage can pass an invalid CGColorSpace to
    // CoreGraphics for this PNG's embedded profile (macOS 26.7.3, arm64).
    // Drop only the profile metadata, leaving pixels/alpha unchanged, so
    // Qt's native icon conversion uses its default RGB color space instead.
    QImage image(QStringLiteral(":/pengy.png"));
    image.setColorSpace(QColorSpace());
    return QIcon(QPixmap::fromImage(image));
#else
    return QIcon(QStringLiteral(":/pengy.png"));
#endif
}
