#pragma once
#include "themehelper.h"
#include <QWidget>
#include <QLabel>
#include <QTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMap>

class InputEdit;

class ChatInputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatInputWidget(QWidget* parent = nullptr);
    void applyTheme(const Theme& theme, int scale = 100);

signals:
    void messageSent(const QString& text, const QStringList& images);

private slots:
    void onSubmit();
    void pickFile();
    void onImagePasted(const QString& path);
    void attachFiles(const QStringList& paths);
    void showDropCue(bool active);

private:
    void addChip(const QString& path);
    void removeChip(const QString& path, QWidget* chip);
    void clearChips();
    bool isImageFile(const QString& path) const;
    bool isTextFile(const QString& path) const;

    Theme m_theme;
    int m_scale = 100;

    InputEdit* m_edit;
    QPushButton* m_attachBtn;
    QWidget* m_chipsRow;
    QLabel* m_dropHint;
    QStringList m_attachments;
    QMimeDatabase m_mimeDb;
};

/// Subclassed QTextEdit that intercepts clipboard image paste
class InputEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit InputEdit(QWidget* parent = nullptr);
    void applyTheme(const Theme& theme, int scale = 100);
#ifdef PENGY_UNIT_TEST
    void dropEventForTest(QDropEvent* event) { dropEvent(event); }
    void dragEnterEventForTest(QDragEnterEvent* event) { dragEnterEvent(event); }
    void dragLeaveEventForTest(QDragLeaveEvent* event) { dragLeaveEvent(event); }
#endif

signals:
    void submitPressed();
    void imagePasted(const QString& path);
    void filesDropped(const QStringList& paths);
    void fileDragActive(bool active);

protected:
    void insertFromMimeData(const QMimeData* source) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void autoSize();
    void setFileDragging(bool active);
    bool m_fileDragging = false;
    Theme m_theme;
    int m_scale = 100;
    int m_minH = 40;
    int m_maxH = 200;
};
