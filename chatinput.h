#pragma once
#include <QWidget>
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

signals:
    void messageSent(const QString& text, const QStringList& images);

private slots:
    void onSubmit();
    void pickFile();
    void onImagePasted(const QString& path);

private:
    void addChip(const QString& path);
    void removeChip(const QString& path, QWidget* chip);
    void clearChips();
    bool isImageFile(const QString& path) const;
    bool isTextFile(const QString& path) const;

    InputEdit* m_edit;
    QPushButton* m_attachBtn;
    QWidget* m_chipsRow;
    QStringList m_attachments;
    QMimeDatabase m_mimeDb;
};

/// Subclassed QTextEdit that intercepts clipboard image paste
class InputEdit : public QTextEdit {
    Q_OBJECT
public:
    explicit InputEdit(QWidget* parent = nullptr);

signals:
    void submitPressed();
    void imagePasted(const QString& path);

protected:
    void insertFromMimeData(const QMimeData* source) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
};
