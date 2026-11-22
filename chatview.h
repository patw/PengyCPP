#pragma once
#include "themehelper.h"
#include <QTextBrowser>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QMouseEvent>
#include <QMap>
#include <QMutex>
#include <QThread>

class ChatView : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);
    // doRender=false appends without rebuilding the document. Use it to batch a
    // bulk load, then call renderNow() once. Per-message HTML is memoised (see
    // m_htmlCache), so an append no longer re-runs markdown over the whole
    // history — but render() still calls setHtml(), which re-lays-out the whole
    // document, so batching a bulk load is still worth it.
    //
    // `source` is a user/assistant message's index in chat["messages"], which
    // search hits point at (see revealHits); -1 for everything else.
    void appendMessage(const QString& role, const QJsonValue& content, bool doRender = true, int source = -1);
    void appendMessageText(const QString& role, const QString& text, bool doRender = true, int source = -1) {
        appendMessage(role, QJsonValue(text), doRender, source);
    }
    // Mark `terms` in the messages whose chat index is in `sources` and scroll
    // the first one into view.  False if none of them is shown here (e.g. the
    // chat was redacted since the search ran).
    bool revealHits(const QList<int>& sources, const QStringList& terms);
    void renderNow();
    void clear();
    void applyTheme(const Theme& theme, int scale = 100);
    // Human / assistant role labels, derived from config by MainWindow. The
    // defaults are the strings Pengy shipped with, so a ChatView used without
    // MainWindow renders exactly as before.
    void applyIdentity(const QString& user, const QString& assistant);

#ifdef PENGY_UNIT_TEST
    QString testMarkdownToHtml(const QString& md) const { return markdownToHtml(md); }
    // Render-cache hooks: testBuildHtml() uses the cache, testBuildHtmlCold()
    // forces a full re-render. The two must always agree.
    QString testBuildHtml() { return buildHtml(); }
    QString testBuildHtmlCold() { invalidateAll(); return buildHtml(); }
    void testExpandTool(const QString& id) {
        m_expandedTools.insert(id);
        invalidate(toolBlockIndex(id));
    }
    void testExpandReasoning(int idx) {
        m_expandedReasoning.insert(idx);
        invalidate(idx);
    }
    int testCacheSize() const { return m_htmlCache.size(); }
    bool testFollow() const { return m_follow; }
    bool testAnchorHeld() const { return m_hasAnchor; }
    bool testRenderPending() const { return m_renderPending; }
    QList<int> testHitMarks() const { return m_hitMarks.keys(); }
    QString testCachedHtml(int idx) const { return m_htmlCache.value(idx); }
    QVariant testLoadImage(const QUrl& url) { return loadResource(QTextDocument::ImageResource, url); }
    void testOpenImagePreview(const QString& source) { openImagePreview(source); }
    QString testImageAt(const QPoint& pos) const { return imageAt(pos); }
#endif

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    QVariant loadResource(int type, const QUrl& url) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onImageFetched(const QString& url, const QByteArray& data);
    void onScrollAction(int action);
    void onRangeChanged(int minimum, int maximum);
    void onSliderPressed();
    void onSliderMoved(int value);
    void onSliderReleased();

private:
    void render(bool force = false);
    void settle();
    void scheduleSettle();
    void endDrag();
    void flushPendingScroll();
    void noteUserScroll(int value = -1);
    void captureAnchor();
    bool anchorTop(int& position, int& offset) const;
    void applyAnchor(int position, int offset);
    void scrollToHit();
    void clearHitMarks();
    QString buildHtml();
    QString buildCss() const;
    void invalidate(int idx);
    void invalidateAll();
    int toolBlockIndex(const QString& toolCallId) const;
    QString renderMessage(const QJsonObject& msg, int idx) const;
    QString renderToolBlock(const QJsonObject& msg) const;
    QString renderReasoningBlock(const QString& reasoning, int idx) const;
    QString markdownToHtml(const QString& md) const;
    QString convertMarkdownTables(const QString& md) const;
    QString convertMarkdownBlocks(const QString& html) const;
    QString highlightCode(const QString& code, const QString& lang) const;
    QString paragraphize(const QString& html) const;
    QString escapeHtml(const QString& text) const;
    void fetchImage(const QString& url);
    QString imageAt(const QPoint& pos) const;
    void openImagePreview(const QString& source);
    QDialog* m_preview = nullptr;

    Theme m_theme;
    int m_scale = 100;
    QString m_cachedCss;  // rebuilt only in applyTheme()
    // Role labels for the user/assistant lines, set by MainWindow from config.
    // Defaults are the strings Pengy shipped with.
    QString m_userLabel = "You";
    QString m_assistantLabel = "Assistant";

    QJsonArray m_messages;
    // Rendered HTML per message, parallel to m_messages. A null QString means
    // "needs render". buildHtml() used to re-run the markdown converter and
    // syntax highlighter over the entire history on every append, making a
    // conversation O(n^2) to type into; memoising per message removes it.
    QList<QString> m_htmlCache;
    QSet<QString> m_expandedTools;
    QSet<int> m_expandedReasoning;
    // Search hits being shown: view index -> words to mark (see revealHits).
    QMap<int, QStringList> m_hitMarks;

    // Image caching for external HTTP images
    QMap<QString, QByteArray> m_imageCache;  // url -> raw bytes (empty = failed)
    QSet<QString> m_imagePending;            // urls currently being fetched
    QMutex m_imageMutex;

    // Follow-the-feed state.  Intent comes from real input events only (wheel,
    // keys, scrollbar drag): setHtml() replaces the document, reports moves of
    // its own, and keeps moving the maximum for several layout passes
    // afterwards, so the scrollbar is not evidence of what the user wants.
    bool m_follow = true;
    // While not following: document position + sub-line offset of the character
    // at the top of the viewport, re-applied after each render.
    bool m_hasAnchor = false;
    int m_anchorPosition = 0;
    int m_anchorOffset = 0;
    bool m_settleQueued = false;
    bool m_dragging = false;        // user is holding the scrollbar thumb
    bool m_renderPending = false;   // a render was deferred by that drag
};
