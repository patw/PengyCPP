// Standalone regression test for the desktop composer's file-drop path.
#include "chatinput.h"
#include <QApplication>
#include <QDir>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QLabel>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QUrl>
#include <iostream>

static void check(bool ok, const char* label) {
    if (!ok) {
        std::cerr << "FAIL: " << label << std::endl;
        std::exit(1);
    }
}

static bool drop(InputEdit* edit, const QList<QUrl>& urls) {
    QMimeData mime;
    mime.setUrls(urls);
    QDropEvent event(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    edit->dropEventForTest(&event);
    return event.isAccepted();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    const QString text = dir.filePath("notes.md");
    const QString image = dir.filePath("photo.png");
    QFile file(text);
    check(file.open(QIODevice::WriteOnly), "create text file");
    file.write("hello from file");
    file.close();
    check(QImage(2, 2, QImage::Format_RGB32).save(image), "create image");

    ChatInputWidget widget;
    auto* edit = widget.findChild<InputEdit*>();
    check(edit != nullptr, "find composer edit");
    auto* hint = [&widget]() -> QLabel* {
        for (auto* label : widget.findChildren<QLabel*>())
            if (label->text() == "Drop files to attach") return label;
        return nullptr;
    }();
    check(hint != nullptr && hint->isHidden(), "drop hint initially hidden");
    QMimeData hoverMime;
    hoverMime.setUrls({QUrl::fromLocalFile(text)});
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &hoverMime,
                          Qt::LeftButton, Qt::NoModifier);
    edit->dragEnterEventForTest(&enter);
    check(enter.isAccepted() && !hint->isHidden(), "local file hover shows hint");
    check(edit->property("fileDragActive").toBool(), "local file hover highlights edit");
    QDragLeaveEvent leave;
    edit->dragLeaveEventForTest(&leave);
    check(hint->isHidden() && !edit->property("fileDragActive").toBool(),
          "leaving restores normal composer");
    QMimeData remoteMime;
    remoteMime.setUrls({QUrl("https://example.com/file.txt")});
    QDragEnterEvent invalid(QPoint(5, 5), Qt::CopyAction, &remoteMime,
                            Qt::LeftButton, Qt::NoModifier);
    edit->dragEnterEventForTest(&invalid);
    check(!invalid.isAccepted() && hint->isHidden(), "remote URL does not show hint");
    edit->dragEnterEventForTest(&enter);
    check(!hint->isHidden(), "hint returns on next file hover");
    int emissions = 0;
    QString message;
    QStringList images;
    QObject::connect(&widget, &ChatInputWidget::messageSent, &widget,
                     [&](const QString& text, const QStringList& files) {
                         ++emissions;
                         message = text;
                         images = files;
                     });
    check(drop(edit, {QUrl::fromLocalFile(text), QUrl::fromLocalFile(image)}), "accept files");
    check(hint->isHidden() && !edit->property("fileDragActive").toBool(),
          "drop restores normal composer");
    check(edit->toPlainText().isEmpty(), "file URLs do not enter message text");
    check(drop(edit, {QUrl::fromLocalFile(text)}), "accept duplicate drop");
    check(!drop(edit, {QUrl("https://example.com/file.txt"), QUrl::fromLocalFile(dir.path())}),
          "ignore remote URLs and directories");
    edit->setPlainText("question");
    QMetaObject::invokeMethod(&widget, "onSubmit", Qt::DirectConnection);
    check(emissions == 1, "one send");
    check(message == "[File: notes.md]\n```\nhello from file\n```\n\nquestion", "text file once");
    check(images == QStringList{image}, "image forwarded once");

    // A picture dragged out of a browser, a chat client or a screenshot tool
    // arrives as raw image data with no file URL; the URL-only handler ignored
    // every one of those drops.
    QMimeData imageMime;
    QImage swatch(4, 4, QImage::Format_RGB32);
    swatch.fill(0x3366FF);
    imageMime.setImageData(swatch);
    QDragEnterEvent imageEnter(QPoint(5, 5), Qt::CopyAction, &imageMime,
                               Qt::LeftButton, Qt::NoModifier);
    edit->dragEnterEventForTest(&imageEnter);
    check(imageEnter.isAccepted() && !hint->isHidden(), "raw image hover shows hint");
    QDropEvent imageDrop(QPointF(5, 5), Qt::CopyAction, &imageMime,
                         Qt::LeftButton, Qt::NoModifier);
    edit->dropEventForTest(&imageDrop);
    check(imageDrop.isAccepted(), "raw image drop accepted");
    check(edit->toPlainText().isEmpty(), "image data does not touch the draft");
    check(hint->isHidden() && !edit->property("fileDragActive").toBool(),
          "image drop restores normal composer");
    edit->setPlainText("second");
    QMetaObject::invokeMethod(&widget, "onSubmit", Qt::DirectConnection);
    check(emissions == 2, "second send");
    check(images.size() == 1 && images.first().endsWith(".png") && QFile::exists(images.first()),
          "dropped image saved to a PNG and forwarded");

    // The rules the window-wide drop target on MainWindow delegates to.
    QMimeData fileMime;
    fileMime.setUrls({QUrl::fromLocalFile(text)});
    QMimeData webMime;
    webMime.setUrls({QUrl("https://example.com/file.txt")});
    QMimeData dirMime;
    dirMime.setUrls({QUrl::fromLocalFile(dir.path())});
    check(ChatInputWidget::canAcceptDrag(&fileMime), "window accepts local files");
    check(ChatInputWidget::canAcceptDrag(&imageMime), "window accepts raw image drags");
    check(!ChatInputWidget::canAcceptDrag(&webMime), "window refuses web URLs");
    check(!ChatInputWidget::canAcceptDrag(&dirMime), "window refuses directories");
    check(!ChatInputWidget::canAcceptDrag(nullptr), "window refuses an empty payload");
    check(!widget.handleDrop(&webMime), "handleDrop takes nothing from a web URL");
    check(!widget.handleDrop(&dirMime), "handleDrop takes nothing from a directory");
    check(widget.handleDrop(&fileMime), "handleDrop attaches a local file");
    check(widget.handleDrop(&imageMime), "handleDrop attaches dropped image data");
    edit->setPlainText("third");
    QMetaObject::invokeMethod(&widget, "onSubmit", Qt::DirectConnection);
    check(emissions == 3, "third send");
    check(images.size() == 1 && images.first().endsWith(".png"),
          "window-level drop forwarded its image");
    check(message.contains("[File: notes.md]"), "window-level drop embedded the text file");
    return 0;
}
