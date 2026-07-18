#include "image_utils.h"
#include <QImage>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QPainter>

static const int    kDefaultMaxDimension = 4096;
static const double kDefaultMaxMb        = 4.5;
static const int    kDefaultQuality      = 85;

static QString guessMime(const QString& path) {
    QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png")  return "image/png";
    if (ext == "gif")  return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp")  return "image/bmp";
    return "image/jpeg";
}

static QByteArray encodeJpeg(const QImage& img, int quality) {
    QByteArray buf;
    QBuffer buffer(&buf);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "JPEG", quality);
    return buf;
}

ImageResult imagePreprocess(const QString& path, int maxDimension, double maxMb, int quality) {
    ImageResult result;
    if (maxDimension <= 0) maxDimension = kDefaultMaxDimension;
    if (maxMb <= 0.0)      maxMb        = kDefaultMaxMb;
    if (quality <= 0)      quality      = kDefaultQuality;

    QImage img(path);
    if (img.isNull()) return result;

    int w = img.width(), h = img.height();
    int maxBytes = static_cast<int>(maxMb * 1048576.0);
    QString mime = guessMime(path);

    // Step 1: dimension cap
    if (w > maxDimension || h > maxDimension) {
        img = img.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    // Step 2: convert lossless → JPEG if not already lossy
    bool isLossless = (mime == "image/png" || mime == "image/gif" || mime == "image/bmp");
    if (isLossless) {
        if (img.hasAlphaChannel()) {
            // Flatten to white background
            QImage flat(img.size(), QImage::Format_RGB888);
            flat.fill(Qt::white);
            QPainter p(&flat);
            p.drawImage(0, 0, img);
            p.end();
            img = flat;
        } else if (img.format() != QImage::Format_RGB888) {
            img = img.convertToFormat(QImage::Format_RGB888);
        }
        QByteArray jpeg = encodeJpeg(img, quality);
        if (jpeg.size() <= maxBytes) {
            result.bytes_base64 = jpeg.toBase64();
            result.mime = "image/jpeg";
            result.ok = true;
            return result;
        }
    }

    // Step 3: try to fit under size limit
    QByteArray buf;
    QBuffer buffer(&buf);
    buffer.open(QIODevice::WriteOnly);
    const char* fmt = (mime == "image/png") ? "PNG" :
                      (mime == "image/webp") ? "WEBP" : "JPEG";
    int saveQuality = (mime == "image/png" || mime == "image/webp") ? -1 : quality;
    img.save(&buffer, fmt, saveQuality);

    if (buf.size() <= maxBytes) {
        result.bytes_base64 = buf.toBase64();
        result.mime = mime;
        result.ok = true;
        return result;
    }

    // Try lower JPEG quality
    for (int q : {75, 60, 45, 30}) {
        QByteArray jpeg = encodeJpeg(
            img.hasAlphaChannel() ? img.convertToFormat(QImage::Format_RGB888) : img, q);
        if (jpeg.size() <= maxBytes) {
            result.bytes_base64 = jpeg.toBase64();
            result.mime = "image/jpeg";
            result.ok = true;
            return result;
        }
    }

    // Last resort: shrink
    for (double scale : {0.75, 0.5, 0.33}) {
        QImage small = img.scaled(
            static_cast<int>(img.width() * scale),
            static_cast<int>(img.height() * scale),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray jpeg = encodeJpeg(
            small.hasAlphaChannel() ? small.convertToFormat(QImage::Format_RGB888) : small, 60);
        if (jpeg.size() <= maxBytes) {
            result.bytes_base64 = jpeg.toBase64();
            result.mime = "image/jpeg";
            result.ok = true;
            return result;
        }
    }

    // Absolute last resort — tiny thumbnail
    QImage tiny = img.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray jpeg = encodeJpeg(
        tiny.hasAlphaChannel() ? tiny.convertToFormat(QImage::Format_RGB888) : tiny, 40);
    result.bytes_base64 = jpeg.toBase64();
    result.mime = "image/jpeg";
    result.ok = true;
    return result;
}
