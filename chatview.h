#pragma once
#include <QTextBrowser>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QMouseEvent>
#include <QMap>
#include <QMutex>

class ChatView : public QTextBrowser {
    Q_OBJECT
public:
    explicit ChatView(QWidget* parent = nullptr);
    void appendMessage(const QString& role, const QJsonValue& content);
    void appendMessageText(const QString& role, const QString& text) {
        appendMessage(role, QJsonValue(text));
    }
    void clear();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    QVariant loadResource(int type, const QUrl& url) override;

private slots:
    void onImageFetched(const QString& url, const QByteArray& data);

private:
    void    render();
    QString buildHtml();
    QString renderMessage(const QJsonObject& msg) const;
    QString renderToolBlock(const QJsonObject& msg) const;
    QString markdownToHtml(const QString& md) const;
    QString convertMarkdownTables(const QString& md) const;
    QString paragraphize(const QString& html) const;
    QString escapeHtml(const QString& text) const;
    void    fetchImage(const QString& url);

    QJsonArray       m_messages;
    QSet<QString>    m_expandedTools;
    QMap<QString, QByteArray> m_imageCache;
    QSet<QString>    m_imagePending;
    QMutex           m_imageMutex;
};
