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
    void appendMessage(const QString& role, const QJsonValue& content, bool doRender = true);
    void appendMessageText(const QString& role, const QString& text, bool doRender = true) {
        appendMessage(role, QJsonValue(text), doRender);
    }
    void renderNow();
    void clear();
    void applyTheme(const Theme& theme, int scale = 100);

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
    bool testAutoScroll() const { return m_autoScroll; }
    QVariant testLoadImage(const QUrl& url) { return loadResource(QTextDocument::ImageResource, url); }
#endif

protected:
    void mousePressEvent(QMouseEvent* event) override;
    QVariant loadResource(int type, const QUrl& url) override;

private slots:
    void onImageFetched(const QString& url, const QByteArray& data);
    void onScrollChanged(int value);

private:
    void render();
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

    Theme m_theme;
    int m_scale = 100;
    QString m_cachedCss;  // rebuilt only in applyTheme()

    QJsonArray m_messages;
    // Rendered HTML per message, parallel to m_messages. A null QString means
    // "needs render". buildHtml() used to re-run the markdown converter and
    // syntax highlighter over the entire history on every append, making a
    // conversation O(n^2) to type into; memoising per message removes it.
    QList<QString> m_htmlCache;
    QSet<QString> m_expandedTools;
    QSet<int> m_expandedReasoning;

    // Image caching for external HTTP images
    QMap<QString, QByteArray> m_imageCache;  // url -> raw bytes (empty = failed)
    QSet<QString> m_imagePending;            // urls currently being fetched
    QMutex m_imageMutex;

    // Auto-scroll tracking. setHtml() replaces the whole document and resets
    // the scrollbar to the top, so sb->value() right after a render is 0 —
    // *not* a reliable "the user scrolled here" signal. We keep an explicit
    // m_autoScroll flag updated only by genuine user scrolling (see
    // onScrollChanged), and guard the spurious reset with m_rendering.
    bool m_autoScroll = true;
    bool m_rendering = false;
};
