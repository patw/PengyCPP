#pragma once
#include <QDialog>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QMenu>
#include "imagesave.h"

// A non-modal, click-anywhere-to-close image viewer bounded to the current screen.
class ImagePreview : public QDialog {
public:
    explicit ImagePreview(const QImage& image, QWidget* parent = nullptr,
                          const QString& source = {}, const QMap<QString, QByteArray>& cache = {})
        : QDialog(parent), m_source(source), m_cache(cache) {
        setObjectName("pengyImagePreview");
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setModal(false);
        setStyleSheet("QDialog { background: #171a20; } QLabel { background: transparent; }");
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip("Click to close image preview");
        QScreen* display = (parent ? parent->screen() : nullptr);
        if (!display) display = QGuiApplication::primaryScreen();
        const QSize bounds = display ? display->availableGeometry().size() : image.size();
        const QSize size = image.size().scaled(qMax(1, int(bounds.width() * 0.85)),
                                                qMax(1, int(bounds.height() * 0.85)),
                                                Qt::KeepAspectRatio);
        auto* picture = new QLabel(this);
        picture->setObjectName("pengyImagePreviewPicture");
        picture->setAlignment(Qt::AlignCenter);
        picture->setPixmap(QPixmap::fromImage(image).scaled(size, Qt::KeepAspectRatio,
                                                             Qt::SmoothTransformation));
        picture->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->addWidget(picture);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            close();
            event->accept();
        } else {
            QDialog::mousePressEvent(event);
        }
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        QMenu menu(this);
        QAction* save = menu.addAction("Save Image As…");
        if (menu.exec(event->globalPos()) == save) saveImageAs(m_source, m_cache, this);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            close();
            event->accept();
        } else {
            QDialog::keyPressEvent(event);
        }
    }
private:
    QString m_source;
    QMap<QString, QByteArray> m_cache;
};
