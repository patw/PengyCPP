#pragma once
#include <QString>
#include <QByteArray>

/// Preprocess an image for LLM vision APIs.
/// Returns base64-encoded bytes and MIME type, or empty QByteArray on failure.
struct ImageResult {
    QByteArray bytes_base64;
    QString    mime;
    bool       ok = false;
};

/// Preprocess an image file.
/// max_dimension: max pixels on any side (0 = default 4096)
/// max_mb: max output size in MB (0 = default 4.5)
/// quality: JPEG quality 0-100 (0 = default 85)
ImageResult imagePreprocess(const QString& path,
                            int maxDimension = 0,
                            double maxMb = 0.0,
                            int quality = 0);
