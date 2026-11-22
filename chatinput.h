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

    /// The composer's accept rule, shared with the window-wide drop target so a
    /// drop aimed anywhere in the window is judged by exactly the rules a drop
    /// aimed at the input box gets.  Static: it reads only the payload.
    static bool canAcceptDrag(const QMimeData* mime);

    /// Attach a dropped payload.  True if anything was taken.
    bool handleDrop(const QMimeData* mime);

    /// Toggle the "drop to attach" affordance for the current drag.
    void showDropCue(bool active);

    /// Bytes one attached text file may contribute to the prompt.  Attached text
    /// goes into the prompt verbatim, so it is paid for in context: the budget is
    /// a share of the model's context window, and MainWindow sets it from config.
    /// The default assumes the common 256K window (see text_attach.py in the
    /// Python edition for the measurement behind the constants).
    void setEmbedBudgetBytes(int bytes);
    int embedBudgetBytes() const { return m_embedBudgetBytes; }
    static int embedBudgetForContext(qulonglong contextTokens);

    /// A text file above this size is not prompt input: refuse it at attach time
    /// rather than let the request fail at the endpoint.
    static bool oversizedForPrompt(qulonglong sizeBytes);

    /// Render the queue of messages typed while a turn runs.
    ///
    /// `previews` empty means nothing is waiting (the strip hides). `held` says the
    /// queue is parked after a Stop; `running` says a turn is in flight. The
    /// placeholder is set from the same state, so the composer always says whether
    /// Enter will send or queue.
    void setQueueState(const QStringList& previews, bool held, bool running);

    /// Put text back in the composer (a queued message recalled for editing).
    void setComposerText(const QString& text);

    /// Attach files by path -- used to re-attach a recalled message's images.
    void attachPaths(const QStringList& paths) { attachFiles(paths); }

    /// True when there is nothing to send: no text and no attachments.
    bool composerEmpty() const;

#ifdef PENGY_UNIT_TEST
    InputEdit* editForTest() { return m_edit; }
    QLabel* queueLabelForTest() { return m_queueLabel; }
    void submitForTest() { onSubmit(); }
#endif

signals:
    void messageSent(const QString& text, const QStringList& images);

    /// Up (-1) / Down (+1) pressed on an empty composer: recall a queued message.
    void queueRecallRequested(int direction);

    /// Enter on an empty composer. MainWindow releases the oldest queued message
    /// if one is waiting, so a parked queue can be drained without retyping it.
    void emptySubmit();

private slots:
    void onSubmit();
    void pickFile();
    void onImagePasted(const QString& path);
    void attachFiles(const QStringList& paths);

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
    QLabel* m_queueLabel;
    QLabel* m_dropHint;
    bool m_turnRunning = false;
    QStringList m_attachments;
    QMimeDatabase m_mimeDb;
    int m_embedBudgetBytes = 224000;  // 256K window * 3.5 bytes/token * 25%
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
    void queueRecall(int direction);
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
