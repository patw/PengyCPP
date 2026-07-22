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
    // bulk load, then call renderNow() once — rendering per message is O(n^2)
    // (each append rebuilds the full HTML and calls setHtml).
    void appendMessage(const QString& role, const QJsonValue& content, bool doRender = true);
    void appendMessageText(const QString& role, const QString& text, bool doRender = true) {
        appendMessage(role, QJsonValue(text), doRender);
    }
    void renderNow();
    void clear();
    void applyTheme(const Theme& theme, int scale = 100);

#ifdef PENGY_UNIT_TEST
    QString testMarkdownToHtml(const QString& md) const { return markdownToHtml(md); }
#endif

protected:
    void mousePressEvent(QMouseEvent* event) override;
    QVariant loadResource(int type, const QUrl& url) override;

private slots:
    void onImageFetched(const QString& url, const QByteArray& data);

private:
    void render();
    QString buildHtml();
    QString buildCss() const;
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
    QSet<QString> m_expandedTools;
    QSet<int> m_expandedReasoning;

    // Image caching for external HTTP images
    QMap<QString, QByteArray> m_imageCache;  // url -> raw bytes (empty = failed)
    QSet<QString> m_imagePending;            // urls currently being fetched
    QMutex m_imageMutex;
};
