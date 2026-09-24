#pragma once
#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QMessageBox>
#include <QSaveFile>
#include <QUrl>
#include <QDir>
#include <QMap>

struct SavableImage {
    QByteArray bytes;
    QString name;
    bool isValid() const { return !bytes.isEmpty(); }
};

inline SavableImage imageBytesForSave(const QString& source, const QMap<QString, QByteArray>& cache) {
    QUrl url(source);
    QByteArray data;
    QString name;
    if (url.isLocalFile() || QDir::isAbsolutePath(source)) {
        QString path = url.isLocalFile() ? url.toLocalFile() : source;
        if (QFileInfo(path).fileName() == "thumbnail-256-v1.jpg") {
            const QString display = QFileInfo(path).dir().filePath("image-display-v1.jpg");
            if (QFileInfo::exists(display)) path = display;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        data = file.readAll();
        name = QFileInfo(path).fileName();
    } else if (url.scheme() == "http" || url.scheme() == "https") {
        data = cache.value(source);
        name = QFileInfo(url.path()).fileName();
    } else if (source.startsWith("data:")) {
        const int comma = source.indexOf(',');
        if (comma < 0) return {};
        const QByteArray payload = source.mid(comma + 1).toLatin1();
        data = source.left(comma).contains(";base64", Qt::CaseInsensitive)
            ? QByteArray::fromBase64(payload) : QByteArray::fromPercentEncoding(payload);
        name = "image";
    } else {
        return {};
    }
    QImage image;
    if (data.isEmpty() || !image.loadFromData(data)) return {};
    QString ext;
    if (data.startsWith("\x89PNG\r\n\x1a\n")) ext = ".png";
    else if (data.startsWith("\xff\xd8\xff")) ext = ".jpg";
    else if (data.startsWith("GIF87a") || data.startsWith("GIF89a")) ext = ".gif";
    else if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP") ext = ".webp";
    else {
        ext = ".png";
        data.clear();
        QBuffer buffer(&data);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    }
    if (name.isEmpty()) name = "image";
    if (!name.endsWith(ext, Qt::CaseInsensitive)) name = QFileInfo(name).completeBaseName() + ext;
    return {data, name};
}

inline bool saveImageAs(const QString& source, const QMap<QString, QByteArray>& cache, QWidget* parent) {
    const SavableImage image = imageBytesForSave(source, cache);
    if (!image.isValid()) {
        QMessageBox::warning(parent, "Cannot Save Image", "The image is no longer available.");
        return false;
    }
    const QString destination = QFileDialog::getSaveFileName(parent, "Save Image As", image.name,
                                                              "Images (*.png *.jpg *.jpeg *.gif *.webp)");
    if (destination.isEmpty()) return false;
    QSaveFile file(destination);
    if (!file.open(QIODevice::WriteOnly) || file.write(image.bytes) != image.bytes.size() || !file.commit()) {
        QMessageBox::warning(parent, "Cannot Save Image", "Could not save image to the selected location.");
        return false;
    }
    return true;
}
