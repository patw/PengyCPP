#include "chatview.h"
#include <algorithm>
#include "imagepreview.h"
#include "imagesave.h"
#include <QMenu>
#include <QTextImageFormat>
#include "attachments.h"
#include <QScrollBar>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrl>
#include <QDir>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QFontDatabase>
#include <QFileInfo>
#include <QKeyEvent>
#include <QWheelEvent>

namespace {
// Within this many pixels of the bottom the view counts as "at the end of the
// feed", so scrolling back to the bottom resumes following.
constexpr int kBottomMargin = 30;
}  // namespace

ChatView::ChatView(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    // QTextBrowser enables drops by default, and a read-only one declines them,
    // so drops already bubble up to MainWindow.  Turn it off explicitly: that
    // fallback is Qt-internal, and an editable transcript would otherwise start
    // swallowing drops meant for the composer.
    setAcceptDrops(false);
    m_theme = makeTheme("system", "default");
    applyTheme(m_theme, m_scale);
    // Intent to stop or resume following comes from real input events only
    // (wheel / keys / scrollbar): setHtml() resets the bar and moves the
    // maximum by itself for several layout passes, so valueChanged() is
    // deliberately not connected.
    auto* bar = verticalScrollBar();
    connect(bar, &QAbstractSlider::actionTriggered, this, &ChatView::onScrollAction);
    connect(bar, &QAbstractSlider::rangeChanged, this, &ChatView::onRangeChanged);
    connect(bar, &QAbstractSlider::sliderPressed, this, &ChatView::onSliderPressed);
    connect(bar, &QAbstractSlider::sliderMoved, this, &ChatView::onSliderMoved);
    connect(bar, &QAbstractSlider::sliderReleased, this, &ChatView::onSliderReleased);
}

void ChatView::applyTheme(const Theme& theme, int scale) {
    m_theme = theme;
    m_scale = scale;
    auto font = scaledChatFont(scale);
    setFont(font);
    document()->setDefaultFont(font);
    setStyleSheet(QString("QTextBrowser { background-color:%1; color:%2; border:none; padding:0; }")
                  .arg(m_theme["bg"], m_theme["fg"]));
    document()->setDefaultStyleSheet(buildCss());
    m_cachedCss = buildCss();  // cache for use in buildHtml()
    // A new theme means new syntax-highlight colours, so every cached code
    // block is stale — not just the CSS in the <head>.
    invalidateAll();
    if (!m_messages.isEmpty()) render();
}

void ChatView::applyIdentity(const QString& user, const QString& assistant) {
    // Labels belong to the view and the node, never to the chat file; see
    // configChatIdentity() for the derivation. Cached HTML is dropped because a
    // label is part of every user and assistant card.
    if (user == m_userLabel && assistant == m_assistantLabel) return;
    m_userLabel = user;
    m_assistantLabel = assistant;
    invalidateAll();
    if (!m_messages.isEmpty()) render();
}

QString ChatView::buildCss() const {
    QString fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    return QString(R"CSS(
body { font-family:"%1"; font-size:1em; background-color:%3; color:%4; margin:8px; }
a { color:%5; text-decoration:none; }
pre { background-color:%15; color:%14; padding:10px; margin:6px 0; white-space:pre-wrap; word-wrap:break-word; }
.code-lang { font-size:0.9em; color:%16; margin-bottom:2px; font-family:monospace; }
table { border:1px solid %6; margin:6px 0; }
th, td { border:1px solid %6; padding:4px 10px; }
th { background-color:%7; font-weight:bold; }
img { max-width:600px; }
.attachment-gallery { display:flex; flex-wrap:wrap; gap:6px; margin:4px 0; }
.attachment-card { width:96px; height:96px; object-fit:cover; border-radius:6px; }
.attachment-unavailable { color:%16; font-size:0.85em; padding:6px; border:1px dashed %6; }
.role-user { color:%8; font-weight:bold; font-size:0.9em; margin:8px 0 2px 0; }
.role-assistant { color:%10; font-weight:bold; font-size:0.9em; margin:8px 0 2px 0; }
.tool-card { border:1px solid %11; padding:4px 8px; margin:6px 0; background-color:%12; }
.tool-link { color:%5; text-decoration:none; font-weight:bold; }
.tool-pre { background-color:%13; color:%14; padding:4px; margin:2px 0; font-size:0.9em; }
.muted { color:%16; }
.declined { color:%17; }
.reasoning-card { border:1px solid %18; padding:6px 10px; margin:6px 0; background-color:%19; }
.reasoning-link { color:%20; text-decoration:none; font-weight:bold; }
.reasoning-body { color:%16; font-size:0.85em; white-space:pre-wrap; word-wrap:break-word; margin-top:4px; }
.notice-card { border:1px solid %21; padding:6px 10px; margin:6px 0; background-color:%22; color:%23; }
.error-card { border:1px solid %24; padding:6px 10px; margin:6px 0; background-color:%25; color:%26; }
code { background-color:%7; color:%14; padding:1px 3px; border-radius:2px; }
blockquote { border-left:3px solid %6; margin:6px 0; padding:2px 0 2px 10px; color:%16; }
hr { border:0; border-top:1px solid %6; margin:10px 0; }
ul, ol { margin:4px 0 8px 20px; padding-left:16px; }
li { margin:2px 0; }
h1, h2, h3, h4, h5, h6 { margin:10px 0 4px 0; }
h1 { font-size:1.4em; } h2 { font-size:1.3em; } h3 { font-size:1.1em; } h4, h5, h6 { font-size:1em; }
.search-hit { background-color:%27; font-weight:bold; }
)CSS")
        .arg(fixed).arg(m_theme["bg"], m_theme["fg"], m_theme["link"], m_theme["border"], m_theme["panel_2"],
             m_theme["user_label"]).arg(m_theme["assistant_label"], m_theme["border_soft"], m_theme["tool_bg"],
             m_theme["tool_arg_bg"], m_theme["code_fg"], m_theme["code_bg"], m_theme["muted"], m_theme["danger"],
             m_theme["reasoning_border"], m_theme["reasoning_bg"], m_theme["reasoning_fg"],
             m_theme["notice_border"], m_theme["notice_bg"], m_theme["notice_fg"],
             m_theme["error_border"], m_theme["error_bg"], m_theme["error_fg"]).arg(m_theme["selection"]);
}

// Wrap each search word in rendered `html` in a search-hit span.  Words are
// matched case-insensitively in the text between tags only, against the
// escaped text, so a word holding & or < still finds its entity form.  A word
// split across tags (e.g. highlighted code tokens) is not marked.
static QString markSearchTerms(const QString& html, const QStringList& terms) {
    QStringList forms;
    for (const QString& t : terms) {
        if (t.isEmpty()) continue;
        // User messages escape quotes as &quot;; markdown output leaves them bare.
        QString noQuotes = t;
        noQuotes.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
        for (const QString& form : {noQuotes, t.toHtmlEscaped()})
            if (!forms.contains(form)) forms << QRegularExpression::escape(form);
    }
    if (forms.isEmpty()) return html;
    std::sort(forms.begin(), forms.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    const QRegularExpression words('(' + forms.join('|') + ')', QRegularExpression::CaseInsensitiveOption
                                                        | QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    QString out;
    int pos = 0;
    auto markText = [&](const QString& text) {
        QString marked = text;
        marked.replace(words, QStringLiteral("<span class='search-hit'>\\1</span>"));
        out += marked;
    };
    for (auto it = tag.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        markText(html.mid(pos, m.capturedStart() - pos));
        out += m.captured();
        pos = m.capturedEnd();
    }
    markText(html.mid(pos));
    return out;
}

void ChatView::appendMessage(const QString& role, const QJsonValue& content, bool doRender, int source) {
    if (role == "user") {
        QJsonObject obj = content.toObject();
        QJsonObject msg;
        msg["role"] = "user";
        msg["content"] = obj.contains("content") ? obj["content"] : content;
        msg["attachments"] = obj["attachments"];
        if (source >= 0) msg["source"] = source;
        m_messages.append(msg);
        m_htmlCache.append(QString());
        if (doRender) {
            // Sending a message ends a search jump: back to following the chat.
            clearHitMarks();
            m_follow = true;
            m_hasAnchor = false;
        }
    } else if (role == "tool_request") {
        // Create a unified tool_block with result = null (not yet available)
        QJsonObject obj = content.toObject();
        QJsonObject msg;
        msg["role"] = "tool_block";
        msg["tool_call_id"] = obj["tool_call_id"];
        msg["name"] = obj["name"];
        msg["args"] = obj["args"];
        msg["result"] = QJsonValue::Null;
        msg["declined"] = false;
        m_messages.append(msg);
        m_htmlCache.append(QString());
    } else if (role == "tool_result") {
        // Find matching tool_block and set result
        QJsonObject obj = content.toObject();
        QString tcId = obj["tool_call_id"].toString();
        int i = toolBlockIndex(tcId);
        if (i >= 0) {
            QJsonObject msg = m_messages[i].toObject();
            msg["result"] = obj["content"];
            msg["declined"] = obj["declined"].toBool(false);
            m_messages[i] = msg;
            // Mutated in place: its "(running…)" header is now stale.
            invalidate(i);
        }
    } else {
        QJsonObject msg;
        msg["role"] = role;
        if (content.isString()) {
            msg["content"] = content.toString();
        } else if (content.isObject()) {
            QJsonObject obj = content.toObject();
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                msg[it.key()] = it.value();
            }
        }
        if (source >= 0) msg["source"] = source;
        m_messages.append(msg);
        m_htmlCache.append(QString());
    }
    if (doRender)
        render();
}

void ChatView::renderNow() {
    // force: a caller that re-renders on purpose (tab opened, chat redacted)
    // must not be told to wait for a scrollbar drag to finish.
    render(true);
}

void ChatView::clear() {
    if (m_preview) {
        m_preview->close();
        m_preview->deleteLater();
        m_preview = nullptr;
    }
    m_messages = QJsonArray();
    m_htmlCache.clear();
    m_expandedTools.clear();
    m_expandedReasoning.clear();
    m_hitMarks.clear();
    m_follow = true;
    m_hasAnchor = false;
    m_renderPending = false;
    QTextBrowser::clear();
}

QString ChatView::imageAt(const QPoint& pos) const {
    QTextCursor cursor = cursorForPosition(pos);
    for (int offset : {0, -1}) {
        int position = cursor.position() + offset;
        if (position < 0) continue;
        QTextCursor candidate(document());
        candidate.setPosition(position);
        QTextCharFormat format = candidate.charFormat();
        if (!format.isImageFormat()) continue;
        if (position + 1 >= document()->characterCount()) continue;
        const QRect start = cursorRect(candidate);
        QTextCursor after(document());
        after.setPosition(position + 1);
        const QRect end = cursorRect(after);
        if (pos.y() >= start.y() && pos.y() < start.y() + start.height()
            && pos.x() >= start.x() && pos.x() < end.x())
            return format.toImageFormat().name();
    }
    return {};
}

void ChatView::openImagePreview(const QString& source) {
    QUrl url(source);
    QImage image;
    if (url.isLocalFile() || QDir::isAbsolutePath(source)) {
        QString path = url.isLocalFile() ? url.toLocalFile() : source;
        if (path.endsWith("/thumbnail-256-v1.jpg")) {
            QString display = QFileInfo(path).dir().filePath("image-display-v1.jpg");
            if (QFile::exists(display)) path = display;
        }
        image.load(path);
    } else if (source.startsWith("http://") || source.startsWith("https://")) {
        QByteArray data;
        { QMutexLocker lock(&m_imageMutex); data = m_imageCache.value(source); }
        if (!data.isEmpty()) image.loadFromData(data);
    } else if (source.startsWith("data:")) {
        QVariant resource = loadResource(QTextDocument::ImageResource, url);
        if (resource.canConvert<QImage>()) image = resource.value<QImage>();
    }
    if (image.isNull()) return;
    if (m_preview) {
        m_preview->close();
        m_preview->deleteLater();
    }
    m_preview = new ImagePreview(image, this, source, m_imageCache);
    m_preview->show();
}

void ChatView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QString source = imageAt(event->pos());
        if (!source.isEmpty()) {
            openImagePreview(source);
            return;
        }
        QString anchor = anchorAt(event->pos());
        if (anchor.startsWith("toggle://")) {
            QString toolId = anchor.mid(9);  // strlen("toggle://")
            if (m_expandedTools.contains(toolId)) {
                m_expandedTools.remove(toolId);
            } else {
                m_expandedTools.insert(toolId);
            }
            invalidate(toolBlockIndex(toolId));
            render();
            return;
        }
        if (anchor.startsWith("reasoning://")) {
            bool ok;
            int idx = anchor.mid(QString("reasoning://").length()).toInt(&ok);
            if (ok) {
                if (m_expandedReasoning.contains(idx)) {
                    m_expandedReasoning.remove(idx);
                } else {
                    m_expandedReasoning.insert(idx);
                }
                invalidate(idx);
                render();
                return;
            }
        }
        if (anchor.startsWith("http://") || anchor.startsWith("https://")) {
            QDesktopServices::openUrl(QUrl(anchor));
            return;
        }
    }
    QTextBrowser::mousePressEvent(event);
}

void ChatView::contextMenuEvent(QContextMenuEvent* event) {
    const QString source = imageAt(event->pos());
    if (source.isEmpty()) {
        QTextBrowser::contextMenuEvent(event);
        return;
    }
    QMenu menu(this);
    QAction* save = menu.addAction("Save Image As…");
    if (menu.exec(event->globalPos()) == save) {
        QMap<QString, QByteArray> cached;
        { QMutexLocker lock(&m_imageMutex); cached = m_imageCache; }
        saveImageAs(source, cached, this);
    }
}

// ── follow the feed / hold the reading spot ─────────────────────────────────

void ChatView::wheelEvent(QWheelEvent* event) {
    // A wheel notch is a genuine scroll, and the base class applies the new
    // value synchronously, so intent can be read straight afterwards. The
    // settle runs first so the notch lands on the position the reader is
    // actually on — a render leaves the bar at the top until that runs.
    endDrag();  // a lost slider release must not freeze the view
    flushPendingScroll();
    QTextBrowser::wheelEvent(event);
    noteUserScroll();
}

void ChatView::keyPressEvent(QKeyEvent* event) {
    endDrag();
    flushPendingScroll();
    QTextBrowser::keyPressEvent(event);
    switch (event->key()) {
    case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_PageUp: case Qt::Key_PageDown:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_Space:
        noteUserScroll();
        break;
    default:
        break;
    }
}

void ChatView::onScrollAction(int action) {
    // Scrollbar arrows / trough clicks. Fires *before* the new value is applied,
    // so the resulting position is only known on the next tick. An upward
    // action stops following immediately — otherwise a render landing in that
    // gap would drag the view back to the bottom and undo the click.
    // QAbstractSlider::SliderAction: SingleStepSub=2, PageStepSub=4,
    // SliderToMinimum=5 (the value is an int here for the same reason).
    endDrag();
    flushPendingScroll();
    if (action == 2 || action == 4 || action == 5) {
        m_follow = false;
        captureAnchor();
    }
    QTimer::singleShot(0, this, [this] { noteUserScroll(); });
}

void ChatView::onSliderPressed() {
    // Holding the thumb: do not rebuild the document under it. setHtml()
    // resets the bar mid-drag, which both fights the drag (the thumb jumps
    // away from the pointer) and loses the drag's own value. Catch up once the
    // user lets go.
    endDrag();
    flushPendingScroll();
    m_dragging = true;
}

void ChatView::onSliderMoved(int value) {
    // sliderMoved() carries the value the drag is heading for, so it can be
    // judged without waiting for the bar to settle.
    noteUserScroll(value);
}

void ChatView::onSliderReleased() {
    // Read intent and the reading spot while the bar still holds the drag's
    // value: the catch-up render in endDrag() resets it.
    noteUserScroll();
    endDrag();
}

void ChatView::flushPendingScroll() {
    // A render leaves the bar at the top until the settle runs. Scrolling from
    // there — or capturing the reading spot there — sent the view to the top of
    // the chat: the wheel moved from 0 and the anchor became the document start.
    if (m_settleQueued) settle();
}

void ChatView::endDrag() {
    if (!m_dragging) return;
    m_dragging = false;
    if (m_renderPending) {
        m_renderPending = false;
        render();
    }
}

void ChatView::noteUserScroll(int value) {
    // A real input event moved the viewport: (re)decide whether to follow.
    // Following resumes as soon as the user brings the view back to the bottom
    // — "give me the live feed again" — and stops the moment they move away.
    auto* bar = verticalScrollBar();
    const int position = value < 0 ? bar->value() : value;
    m_follow = position >= bar->maximum() - kBottomMargin;
    if (m_follow) {
        m_hasAnchor = false;
        scheduleSettle();   // close the last few pixels
    } else {
        captureAnchor();
    }
}

void ChatView::captureAnchor() {
    int position = 0, offset = 0;
    if (!anchorTop(position, offset)) return;  // hidden: keep hideEvent's anchor
    m_anchorPosition = position;
    m_anchorOffset = offset;
    m_hasAnchor = true;
}

bool ChatView::anchorTop(int& position, int& offset) const {
    // Document position and sub-line offset of the character at the top of the
    // viewport. False without a laid-out viewport (a hidden tab), where the
    // geometry means nothing and must not be recorded.
    if (!isVisible() || viewport()->height() <= 0) return false;
    const QTextCursor cursor = cursorForPosition(QPoint(0, 0));
    position = cursor.position();
    offset = cursorRect(cursor).y();
    return true;
}

void ChatView::applyAnchor(int position, int offset) {
    QTextCursor cursor(document());
    cursor.setPosition(position);
    const int delta = cursorRect(cursor).y() - offset;
    if (delta) {
        auto* bar = verticalScrollBar();
        bar->setValue(bar->value() + delta);
    }
}

void ChatView::scheduleSettle() {
    // Re-assert the scroll position once Qt has finished laying out. setHtml()
    // relayouts asynchronously and the scrollbar maximum changes over several
    // passes (measured in the Python edition: 0 -> 2986 -> 3307 -> 3266 for one
    // append), so a jump to the bottom issued immediately afterwards lands on a
    // stale maximum and leaves the view short of the end.
    if (m_settleQueued) return;
    m_settleQueued = true;
    QTimer::singleShot(0, this, [this] { settle(); });
}

void ChatView::settle() {
    m_settleQueued = false;
    if (m_dragging) return;  // the user owns the bar right now
    auto* bar = verticalScrollBar();
    if (m_follow) {
        if (bar->value() != bar->maximum()) bar->setValue(bar->maximum());
    } else if (m_hasAnchor) {
        applyAnchor(m_anchorPosition, m_anchorOffset);
    }
}

void ChatView::onRangeChanged(int, int) {
    // The document is still settling (images resolving, tables reflowing):
    // re-anchor to the position already decided instead of trusting the
    // maximum seen one tick ago.
    if (m_follow || m_hasAnchor) scheduleSettle();
}

void ChatView::showEvent(QShowEvent* event) {
    QTextBrowser::showEvent(event);
    scheduleSettle();
}

void ChatView::resizeEvent(QResizeEvent* event) {
    // Qt re-lays the document out at the new width: keep following the end, or
    // hold the reading spot, instead of letting the resize decide.
    QTextBrowser::resizeEvent(event);
    scheduleSettle();
}

void ChatView::render(bool force) {
    if (m_dragging && !force) {
        m_renderPending = true;  // rebuilt on slider release
        return;
    }
    if (m_follow) m_hasAnchor = false;
    // Not following: keep the anchor already held. NEVER re-read the viewport
    // here — setHtml() below resets the bar to the top, so a second render in
    // the same burst (a turn emits several cards back to back) would capture the
    // document start and park the view there. The reading spot is captured when
    // the user moves the bar and nowhere else.
    setHtml(buildHtml());
    scheduleSettle();
}

// ── search hits ─────────────────────────────────────────────────────────────

bool ChatView::revealHits(const QList<int>& sources, const QStringList& terms) {
    clearHitMarks();
    for (int i = 0; i < m_messages.size(); ++i) {
        const QJsonValue source = m_messages[i].toObject().value("source");
        if (source.isDouble() && sources.contains(source.toInt())) {
            m_hitMarks.insert(i, terms);
            invalidate(i);
        }
    }
    if (m_hitMarks.isEmpty()) return false;
    // Stay put on later renders instead of snapping back to the bottom.
    m_follow = false;
    m_hasAnchor = false;
    render();
    // The anchor's position is only known once Qt has laid out the new
    // document, and this must run after any pending settle.
    QTimer::singleShot(0, this, &ChatView::scrollToHit);
    return true;
}

void ChatView::scrollToHit() {
    scrollToAnchor("pengy-hit");
    // A little of the previous message above it, so the hit isn't flush
    // against the top edge.
    verticalScrollBar()->setValue(verticalScrollBar()->value() - 24);
    // Hold this spot (not the bottom) on the renders that follow.
    captureAnchor();
}

void ChatView::clearHitMarks() {
    for (int idx : m_hitMarks.keys()) invalidate(idx);
    m_hitMarks.clear();
}

// ── render cache ────────────────────────────────────────────────────────────
// Anything that changes a message's *rendered output* must invalidate it:
// content mutation (a tool result arriving), expand/collapse toggles, and theme
// changes (which alter the syntax highlighter's colours, not just the CSS).

void ChatView::invalidate(int idx) {
    if (idx >= 0 && idx < m_htmlCache.size())
        m_htmlCache[idx] = QString();  // null == needs render
}

void ChatView::invalidateAll() {
    m_htmlCache.fill(QString());
}

int ChatView::toolBlockIndex(const QString& toolCallId) const {
    for (int i = m_messages.size() - 1; i >= 0; --i) {
        QJsonObject msg = m_messages[i].toObject();
        if (msg["role"].toString() == "tool_block"
            && msg["tool_call_id"].toString() == toolCallId)
            return i;
    }
    return -1;
}

QString ChatView::buildHtml() {
    // Keep the cache aligned with m_messages so a stale length can't misindex.
    while (m_htmlCache.size() < m_messages.size()) m_htmlCache.append(QString());
    while (m_htmlCache.size() > m_messages.size()) m_htmlCache.removeLast();

    QString html = QString("<html><head><style>%1</style></head><body>").arg(m_cachedCss);
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_htmlCache[i].isNull()) {
            QString rendered = renderMessage(m_messages[i].toObject(), i);
            // renderMessage may legitimately produce nothing (e.g. an empty
            // assistant message); store empty-but-non-null so it stays cached.
            if (!m_hitMarks.isEmpty() && i == m_hitMarks.firstKey())
                rendered = "<a name='pengy-hit'></a>" + rendered;
            m_htmlCache[i] = rendered.isNull() ? QLatin1String("") : rendered;
        }
        html += m_htmlCache[i];
    }
    html += "</body></html>";
    return html;
}

QString ChatView::renderMessage(const QJsonObject& msg, int idx) const {
    QString role = msg["role"].toString();

    if (role == "user") {
        QString gallery;
        for (const QJsonValue& value : msg["attachments"].toArray()) {
            QJsonObject ref = value.toObject();
            QString id = ref["id"].toString();
            if (ref["kind"].toString() == "image" && attachmentIdIsValid(id)) {
                QString path = attachmentDerivativePath(id, "thumbnail-256-v1.jpg");
                if (QFile::exists(path)) {
                    gallery += QString("<a href=\"%1\"><img class='attachment-card' src=\"%1\" alt=\"%2\"></a>")
                        .arg(QUrl::fromLocalFile(path).toString(), escapeHtml(ref["name"].toString("Image")));
                } else {
                    gallery += "<span class='attachment-unavailable'>Attachment unavailable</span>";
                }
            } else {
                gallery += "<span class='attachment-unavailable'>Unsupported attachment</span>";
            }
        }
        return QString(
            "<p class='role-user'>%1</p>"
            "<div class='attachment-gallery'>%2</div>"
            "<p style='margin:2px 0 10px 0;white-space:pre-wrap;'>%3</p>"
        ).arg(escapeHtml(m_userLabel), gallery, m_hitMarks.contains(idx)
                    ? markSearchTerms(escapeHtml(msg["content"].toString()), m_hitMarks.value(idx))
                    : escapeHtml(msg["content"].toString()));

    } else if (role == "assistant") {
        QString content = msg["content"].toString();
        if (content.isEmpty()) return "";
        QString parts;
        if (msg.contains("reasoning_content")) {
            parts += renderReasoningBlock(msg["reasoning_content"].toString(), idx);
        }
        parts += QString(
            "<p class='role-assistant'>%1</p>"
            "<div style='margin:2px 0 10px 0;'>%2</div>"
        ).arg(escapeHtml(m_assistantLabel), m_hitMarks.contains(idx) ? markSearchTerms(markdownToHtml(content), m_hitMarks.value(idx))
                                       : markdownToHtml(content));
        return parts;

    } else if (role == "tool_block") {
        return renderToolBlock(msg);
    } else if (role == "notice" || role == "error") {
        // Harness cards (info notice / failed-turn error), not something the
        // model said: their own surface, with no "Assistant" label. The class
        // name is the role, so the theme picks the colours.
        return QString("<div class='%1-card'>%2</div>")
            .arg(role, escapeHtml(msg["content"].toString()));
    }

    return "";
}

QString ChatView::renderReasoningBlock(const QString& reasoning, int idx) const {
    bool expanded = m_expandedReasoning.contains(idx);
    QString arrow = expanded ? "&#9660;" : "&#9654;";

    // First line preview for collapsed state
    QString firstLine = reasoning.section('\n', 0, 0);
    QString preview = firstLine.left(120);
    if (firstLine.length() > 120) preview += "&#8230;";

    QString header = QString(
        "<a class='reasoning-link' href='reasoning://%1'>%2&nbsp;Reasoning</a>"
    ).arg(idx).arg(arrow);

    QString inner = "<div style='margin-bottom:2px;'>" + header + "</div>";

    if (expanded) {
        QString reasoningEscaped = escapeHtml(reasoning);
        inner += "<div class='reasoning-body'>" + reasoningEscaped + "</div>";
    } else {
        inner += "<div class='reasoning-body muted'>" + escapeHtml(preview) + "</div>";
    }

    return "<div class='reasoning-card'>" + inner + "</div>";
}

QString ChatView::renderToolBlock(const QJsonObject& msg) const {
    QString toolCallId = msg["tool_call_id"].toString();
    QString name = msg["name"].toString();
    QJsonObject args = msg["args"].toObject();
    bool hasResult = !msg["result"].isNull() && !msg["result"].isUndefined();
    QString result = hasResult ? msg["result"].toString() : QString();
    bool declined = msg["declined"].toBool(false);
    bool expanded = m_expandedTools.contains(toolCallId);

    QString arrow = expanded ? "&#9660;" : "&#9654;";
    QString nameSafe = escapeHtml(name);

    // Build args preview
    QStringList argParts;
    for (auto it = args.begin(); it != args.end(); ++it) {
        QString val;
        if (it.value().isString()) {
            val = "'" + it.value().toString() + "'";
        } else {
            val = QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact);
        }
        argParts.append(it.key() + "=" + val);
    }
    QString argsPreview = argParts.join(", ");
    bool truncated = argsPreview.length() > 60;
    if (truncated) {
        argsPreview = argsPreview.left(59);
    }

    QString label = arrow + "&nbsp;Tool:&nbsp;" + nameSafe;
    if (!argsPreview.isEmpty()) {
        label += "&nbsp;[" + escapeHtml(argsPreview) + "]";
        if (truncated) {
            label += "&#8230;";
        }
    }

    QString status;
    if (!hasResult && !declined) {
        status = "&nbsp;<i class='muted'>(running&#8230;)</i>";
    } else if (declined) {
        status = "&nbsp;<i class='declined'>(declined)</i>";
    }

    QString header = QString(
        "<a class='tool-link' href='toggle://%1'>%2</a>%3"
    ).arg(toolCallId, label, status);

    QString inner = "<div style='margin-bottom:2px;'>" + header + "</div>";

    if (expanded) {
        QString argsJson = escapeHtml(
            QJsonDocument(args).toJson(QJsonDocument::Indented));
        inner += QString(
            "<div style='margin-top:4px;'>"
            "<b>Arguments:</b>"
            "<pre class='tool-pre'>%1</pre>"
            "</div>"
        ).arg(argsJson);

        if (hasResult) {
            QString resultLabel = declined ? "Result (declined)" : "Result";
            QString resultEscaped = escapeHtml(result);
            inner += QString(
                "<div>"
                "<b>%1:</b>"
                "<pre class='tool-pre'>%2</pre>"
                "</div>"
            ).arg(resultLabel, resultEscaped);
        }
    }

    return QString(
        "<div class='tool-card'>"
        "%1"
        "</div>"
    ).arg(inner);
}

QString ChatView::markdownToHtml(const QString& md) const {
    // ── Phase 1: escape user text ────────────────────────────────────
    // toHtmlEscaped prevents literal < > " & from user/LLM text from
    // breaking QTextBrowser's HTML parser.  However it also mangles raw
    // HTML tags that the LLM legitimately emits (like <img src="...">).
    // We fix that in phase 3 by unescaping safe inline tags.
    QString result = md.toHtmlEscaped();

    // ── Phase 2: markdown → HTML ─────────────────────────────────────
    // Convert ``` code blocks FIRST — prevents their contents from
    // being falsely matched by table / image / link regexes below.
    static QRegularExpression codeBlockRx("```([A-Za-z0-9_+.-]*)\\n([\\s\\S]*?)```");
    {
        QList<QRegularExpressionMatch> matches;
        QRegularExpressionMatchIterator it = codeBlockRx.globalMatch(result);
        while (it.hasNext()) matches.append(it.next());
        for (int i = matches.size() - 1; i >= 0; --i) {
            const QRegularExpressionMatch& m = matches[i];
            QString lang = m.captured(1).trimmed();
            QString code = m.captured(2);
            QString html;
            if (!lang.isEmpty()) {
                html += "<div class='code-lang'>" + lang + "</div>";
            }
            html += "<pre><code>" + highlightCode(code, lang) + "</code></pre>";
            result.replace(m.capturedStart(), m.capturedLength(), html);
        }
    }

    // Inline code BEFORE image/link so `![not-an-image](url)` is safe
    static QRegularExpression inlineCodeRx("`([^`]+)`");
    result.replace(inlineCodeRx, "<code>\\1</code>");

    // Images ![alt](url) before links — the ! prefix disambiguates
    static QRegularExpression imageRx("!\\[([^\\]]*)\\]\\(([^\\)]+)\\)");
    result.replace(imageRx, "<img src=\"\\2\" alt=\"\\1\">");

    // Links [text](url)
    static QRegularExpression linkRx("\\[([^\\]]*)\\]\\(([^\\)]+)\\)");
    result.replace(linkRx, "<a href=\"\\2\">\\1</a>");

    // Markdown tables
    result = convertMarkdownTables(result);

    // Headings (# … ######) — must be at line start; process back-to-front
    {
        static QRegularExpression headingRx("^(#{1,6})\\s+(.+)$",
                                            QRegularExpression::MultilineOption);
        QList<QRegularExpressionMatch> matches;
        QRegularExpressionMatchIterator it = headingRx.globalMatch(result);
        while (it.hasNext()) matches.append(it.next());
        for (int i = matches.size() - 1; i >= 0; --i) {
            const QRegularExpressionMatch &m = matches[i];
            int level = m.captured(1).length();
            QString tag = QString("h%1").arg(level);
            result.replace(m.capturedStart(), m.capturedLength(),
                           "<" + tag + ">" + m.captured(2) + "</" + tag + ">");
        }
    }

    // Lists, blockquotes, and horizontal rules
    result = convertMarkdownBlocks(result);

    // **bold** and *italic*
    static QRegularExpression boldRx("\\*\\*(.+?)\\*\\*");
    result.replace(boldRx, "<b>\\1</b>");
    static QRegularExpression italicRx("\\*(.+?)\\*");
    result.replace(italicRx, "<i>\\1</i>");

    // Qt table spacing hack
    result.replace("<table>", "<table cellspacing=\"0\">");

    // ── Phase 3: unescape safe HTML tags ─────────────────────────────
    // Phase 1 escaped EVERYTHING.  Now we restore known-safe inline
    // HTML tags that the LLM might emit directly (e.g. <img>, <br>,
    // <video>, <svg>, <a>).  The regex matches &lt;tagname...&gt;
    // for a whitelist of inline/media tags.
    //
    // [^<]*? is lazy-scan: there are no literal '<' after toHtmlEscaped
    // (they all became &lt;), so it scans until the first &gt;.  The
    // trailing /? captures self-closing <br/> <img ... /> etc.
    static QRegularExpression unescapeRx(
        "&lt;(/?)(img|br|video|source|audio|svg|path|circle|rect|line|polyline|polygon|"
        "ellipse|text|g|defs|clipPath|a|b|i|u|em|strong|code|span|sub|sup|mark|hr)"
        "([^<]*?/?)&gt;");
    result.replace(unescapeRx, "<\\1\\2\\3>");

    // The escaped raw tag's attribute delimiters are still `&quot;` after
    // restoring its angle brackets. QTextDocument does not reliably treat
    // those entities as delimiters in an image source, yielding a broken-image
    // icon for otherwise-valid raw `<img src="file:///…">` responses. Decode
    // quotes only inside our already-whitelisted restored tags; normal prose
    // remains HTML-escaped.
    static QRegularExpression escapedQuoteInSafeTagRx(
        "(<(?:img|br|video|source|audio|svg|path|circle|rect|line|polyline|polygon|"
        "ellipse|text|g|defs|clipPath|a|b|i|u|em|strong|code|span|sub|sup|mark|hr)"
        "[^>]*?)&quot;");
    while (escapedQuoteInSafeTagRx.match(result).hasMatch())
        result.replace(escapedQuoteInSafeTagRx, "\\1\"");

    // ── Phase 4: paragraph wrapping ──────────────────────────────────
    result = paragraphize(result);

    return result;
}


QString ChatView::convertMarkdownBlocks(const QString& html) const {
    QStringList lines = html.split('\n');
    QStringList out;
    bool inUl = false;
    bool inOl = false;
    bool inBlockquote = false;
    bool inPre = false;

    auto closeLists = [&]() {
        if (inUl) { out.append("</ul>"); inUl = false; }
        if (inOl) { out.append("</ol>"); inOl = false; }
    };
    auto closeBlockquote = [&]() {
        if (inBlockquote) { out.append("</blockquote>"); inBlockquote = false; }
    };

    static QRegularExpression ulRx("^\\s*[-+]\\s+(.+)$");
    static QRegularExpression starUlRx("^\\s*\\*\\s+(.+)$");
    static QRegularExpression olRx("^\\s*\\d+[\\.)]\\s+(.+)$");
    static QRegularExpression bqRx("^\\s*&gt;\\s?(.*)$");
    static QRegularExpression hrRx("^\\s*(?:-{3,}|\\*{3,}|_{3,})\\s*$");

    for (const QString& rawLine : lines) {
        QString trimmed = rawLine.trimmed();

        if (rawLine.contains("<pre")) inPre = true;
        if (inPre) {
            closeLists();
            closeBlockquote();
            out.append(rawLine);
            if (rawLine.contains("</pre>")) inPre = false;
            continue;
        }

        if (trimmed.isEmpty()) {
            closeLists();
            closeBlockquote();
            out.append(rawLine);
            continue;
        }

        QRegularExpressionMatch bq = bqRx.match(rawLine);
        if (bq.hasMatch()) {
            closeLists();
            if (!inBlockquote) {
                out.append("<blockquote>");
                inBlockquote = true;
            } else {
                out.append("<br>");
            }
            out.append(bq.captured(1));
            continue;
        }

        QRegularExpressionMatch ol = olRx.match(rawLine);
        if (ol.hasMatch()) {
            closeBlockquote();
            if (inUl) { out.append("</ul>"); inUl = false; }
            if (!inOl) { out.append("<ol>"); inOl = true; }
            out.append("<li>" + ol.captured(1) + "</li>");
            continue;
        }

        QRegularExpressionMatch ul = ulRx.match(rawLine);
        if (!ul.hasMatch()) ul = starUlRx.match(rawLine);
        if (ul.hasMatch()) {
            closeBlockquote();
            if (inOl) { out.append("</ol>"); inOl = false; }
            if (!inUl) { out.append("<ul>"); inUl = true; }
            out.append("<li>" + ul.captured(1) + "</li>");
            continue;
        }

        if (hrRx.match(rawLine).hasMatch()) {
            closeLists();
            closeBlockquote();
            out.append("<hr>");
            continue;
        }

        closeLists();
        closeBlockquote();
        out.append(rawLine);
    }

    closeLists();
    closeBlockquote();
    return out.join("\n");
}

QString ChatView::highlightCode(const QString& code, const QString& lang) const {
    static const QMap<QString, QString> aliases = {
        {"py","python"}, {"python3","python"},
        {"js","javascript"}, {"jsx","javascript"}, {"ts","javascript"}, {"tsx","javascript"}, {"typescript","javascript"}, {"node","javascript"}, {"mjs","javascript"},
        {"c++","cpp"}, {"cc","cpp"}, {"cxx","cpp"}, {"hpp","cpp"}, {"h","c"},
        {"rs","rust"},
        {"sh","shell"}, {"bash","shell"}, {"zsh","shell"},
        {"rb","ruby"},
        {"golang","go"},
    };
    QString family = lang.trimmed().toLower();
    if (aliases.contains(family)) family = aliases.value(family);

    static const QMap<QString, QStringList> keywordSets = {
        {"python", {"def","class","return","if","elif","else","for","while","in","not","and","or","is","import","from","as","with","try","except","finally","raise","yield","lambda","pass","break","continue","global","nonlocal","assert","del","None","True","False","async","await"}},
        {"javascript", {"function","return","if","else","for","while","in","of","var","let","const","new","class","extends","import","export","from","as","try","catch","finally","throw","typeof","instanceof","null","undefined","true","false","async","await","switch","case","default","break","continue","this","super","yield","delete","void"}},
        {"c", {"int","char","float","double","void","if","else","for","while","do","switch","case","default","break","continue","return","struct","union","enum","typedef","static","const","extern","sizeof","unsigned","signed","long","short","goto","volatile"}},
        {"cpp", {"int","char","float","double","void","bool","if","else","for","while","do","switch","case","default","break","continue","return","class","struct","union","enum","typedef","namespace","using","template","typename","public","private","protected","virtual","override","new","delete","this","static","const","constexpr","auto","nullptr","true","false","try","catch","throw","friend","operator","inline","explicit","sizeof","unsigned","signed","long","short"}},
        {"rust", {"fn","let","mut","if","else","match","for","while","loop","in","return","struct","enum","impl","trait","pub","use","mod","crate","self","Self","super","as","ref","move","dyn","where","async","await","unsafe","const","static","true","false","break","continue","type"}},
        {"java", {"public","private","protected","class","interface","extends","implements","static","final","void","int","long","short","byte","char","float","double","boolean","if","else","for","while","do","switch","case","default","break","continue","return","new","this","super","try","catch","finally","throw","throws","import","package","true","false","null","enum","abstract","synchronized"}},
        {"go", {"func","package","import","var","const","type","struct","interface","if","else","for","range","return","switch","case","default","break","continue","go","defer","chan","select","map","nil","true","false","make","new"}},
        {"shell", {"if","then","else","elif","fi","for","while","do","done","case","esac","function","return","export","local","exit","in","break","continue"}},
        {"sql", {"select","from","where","insert","update","delete","create","table","drop","alter","join","left","right","inner","outer","on","group","by","order","having","as","and","or","not","null","values","into","set","distinct","limit","union","in","exists","between","like"}},
        {"ruby", {"def","end","if","elsif","else","unless","while","until","for","in","do","class","module","return","yield","begin","rescue","ensure","raise","require","true","false","nil","self","new"}},
        {"php", {"function","return","if","else","elseif","for","foreach","while","do","switch","case","default","break","continue","class","interface","extends","implements","public","private","protected","static","new","echo","print","true","false","null","namespace","use","try","catch","finally","throw"}},
    };

    static const QMap<QString, QString> commentStyle = {
        {"python","#"}, {"shell","#"}, {"ruby","#"},
        {"javascript","//"}, {"c","//"}, {"cpp","//"}, {"rust","//"}, {"java","//"}, {"go","//"}, {"php","//"},
        {"sql","--"},
    };

    // Build one alternation pattern in priority order (comment > strings > numbers >
    // keywords) and remember which role each capture group maps to, so a single
    // left-to-right pass never re-highlights text already claimed by an earlier group.
    QStringList altParts;
    QList<QString> roles;

    QString cstyle = commentStyle.value(family);
    if (cstyle == "#") {
        altParts << "(#[^\\n]*)";
        roles << "comment";
    } else if (cstyle == "//") {
        altParts << "(/\\*[\\s\\S]*?\\*/|//[^\\n]*)";
        roles << "comment";
    } else if (cstyle == "--") {
        altParts << "(--[^\\n]*)";
        roles << "comment";
    }

    // code has already been through QString::toHtmlEscaped(), so a literal "
    // is &quot; — match the entity, not the character.
    altParts << "(&quot;(?:[^&]|&(?!quot;))*&quot;)";
    roles << "string";
    altParts << "('(?:[^'\\\\]|\\\\.)*')";
    roles << "string";
    altParts << "(\\b\\d+(?:\\.\\d+)?\\b)";
    roles << "number";

    QStringList kws = keywordSets.value(family);
    if (!kws.isEmpty()) {
        altParts << ("(\\b(?:" + kws.join('|') + ")\\b)");
        roles << "keyword";
    }

    // The pattern depends only on `family`, so compile once per language and
    // reuse. Recompiling per code block showed up when rendering long chats.
    // GUI-thread only, so a plain static cache is safe here.
    static QHash<QString, QRegularExpression> rxCache;
    auto cached = rxCache.constFind(family);
    if (cached == rxCache.constEnd())
        cached = rxCache.insert(family, QRegularExpression(altParts.join('|')));
    const QRegularExpression& rx = *cached;

    bool lightMode = (m_theme["mode"] != "dark");

    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = rx.globalMatch(code);
    while (it.hasNext()) {
        QRegularExpressionMatch m = it.next();
        out += code.mid(last, m.capturedStart() - last);
        QString role;
        for (int g = 1; g <= roles.size(); ++g) {
            if (m.capturedStart(g) != -1) { role = roles[g - 1]; break; }
        }
        QString color = m_theme.c.value("syntax_" + role, m_theme["fg"]);
        QString style;
        if (role == "comment" && lightMode) style = "font-style:italic;";
        if (role == "keyword" && lightMode) style += "font-weight:bold;";
        out += "<span style='color:" + color + ";" + style + "'>" + m.captured(0) + "</span>";
        last = m.capturedStart() + m.capturedLength();
    }
    out += code.mid(last);
    return out;
}

QString ChatView::paragraphize(const QString& html) const {
    QStringList lines = html.split('\n');
    QStringList result;
    QStringList currentPara;
    bool inBlock = false;
    int blockDepth = 0;

    // Block-level tags that should never be wrapped in <p> or have <br> injected
    static const QStringList blockTags = {
        "table", "pre", "div", "p", "h1", "h2", "h3", "h4",
        "ul", "ol", "li", "blockquote", "hr", "img", "video", "svg"
    };

    // Pre-compile regexes for matching opening and closing block tags
    static QRegularExpression openRx("^<(" + blockTags.join('|') + R"()[\s>/])");
    static QRegularExpression countOpenRx("<(" + blockTags.join('|') + R"()[\s>])");
    static QRegularExpression countCloseRx("</(" + blockTags.join('|') + R"()\s*>)");

    for (const QString& line : lines) {
        QString stripped = line.trimmed();

        if (!inBlock) {
            if (openRx.match(stripped).hasMatch()) {
                // Transition from text to block — flush current paragraph first
                if (!currentPara.isEmpty()) {
                    QString text = currentPara.join('\n').trimmed();
                    if (!text.isEmpty()) {
                        text.replace('\n', "<br>");
                        result.append("<p>" + text + "</p>");
                    }
                    currentPara.clear();
                }
                inBlock = true;
                int opens = 0;
                auto it = countOpenRx.globalMatch(stripped);
                while (it.hasNext()) { it.next(); opens++; }
                int closes = 0;
                it = countCloseRx.globalMatch(stripped);
                while (it.hasNext()) { it.next(); closes++; }
                blockDepth = opens - closes;
                result.append(line);
            } else if (stripped.isEmpty()) {
                // Blank line — flush current paragraph
                if (!currentPara.isEmpty()) {
                    QString text = currentPara.join('\n').trimmed();
                    if (!text.isEmpty()) {
                        text.replace('\n', "<br>");
                        result.append("<p>" + text + "</p>");
                    }
                    currentPara.clear();
                }
            } else {
                currentPara.append(line);
            }
        } else {
            // Inside a block — append verbatim, track depth
            result.append(line);
            int opens = 0;
            auto it = countOpenRx.globalMatch(stripped);
            while (it.hasNext()) { it.next(); opens++; }
            int closes = 0;
            it = countCloseRx.globalMatch(stripped);
            while (it.hasNext()) { it.next(); closes++; }
            blockDepth += opens - closes;
            if (blockDepth <= 0) {
                inBlock = false;
                blockDepth = 0;
            }
        }
    }

    // Flush remaining paragraph
    if (!currentPara.isEmpty()) {
        QString text = currentPara.join('\n').trimmed();
        if (!text.isEmpty()) {
            text.replace('\n', "<br>");
            result.append("<p>" + text + "</p>");
        }
    }

    return result.join('\n');
}

QString ChatView::convertMarkdownTables(const QString& md) const {
    // Convert markdown tables to HTML.
    // Format: header row, separator row (e.g. |---|---|), data rows.
    QStringList lines = md.split('\n');
    QString out;
    QStringList tableBuf;
    bool inTable = false;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        bool isTableLine = line.startsWith('|') && line.endsWith('|');

        if (isTableLine) {
            tableBuf.append(line);
            inTable = true;
        } else {
            if (inTable) {
                // Render accumulated table
                if (tableBuf.size() >= 2) {
                    // Check that second line is a separator (e.g. |---|:---:|---|)
                    QString sep = tableBuf[1];
                    static QRegularExpression sepRx("^\\|[\\s:\\-|]+\\|$");
                    if (sepRx.match(sep).hasMatch()) {
                        // Valid table — render to HTML
                        out += "<table>\n";

                        // Header
                        QStringList headerCells = tableBuf[0].mid(1, tableBuf[0].length() - 2).split('|');
                        out += "<tr>";
                        for (const QString& cell : headerCells) {
                            out += "<th>" + cell.trimmed() + "</th>";
                        }
                        out += "</tr>\n";

                        // Data rows (skip header[0] and separator[1])
                        for (int r = 2; r < tableBuf.size(); ++r) {
                            QStringList cells = tableBuf[r].mid(1, tableBuf[r].length() - 2).split('|');
                            out += "<tr>";
                            for (const QString& cell : cells) {
                                out += "<td>" + cell.trimmed() + "</td>";
                            }
                            out += "</tr>\n";
                        }
                        out += "</table>\n";
                    } else {
                        // Not a valid table — output raw lines
                        for (const QString& l : tableBuf) {
                            out += l + "\n";
                        }
                    }
                } else {
                    // Single-line table row, not enough for a table
                    for (const QString& l : tableBuf) {
                        out += l + "\n";
                    }
                }
                tableBuf.clear();
                inTable = false;
            }
            out += lines[i] + "\n";
        }
    }

    // Handle trailing table at EOF
    if (inTable && tableBuf.size() >= 2) {
        QString sep = tableBuf[1];
        static QRegularExpression sepRx2("^\\|[\\s:\\-|]+\\|$");
        if (sepRx2.match(sep).hasMatch()) {
            out += "<table>\n";
            QStringList headerCells = tableBuf[0].mid(1, tableBuf[0].length() - 2).split('|');
            out += "<tr>";
            for (const QString& cell : headerCells) {
                out += "<th>" + cell.trimmed() + "</th>";
            }
            out += "</tr>\n";
            for (int r = 2; r < tableBuf.size(); ++r) {
                QStringList cells = tableBuf[r].mid(1, tableBuf[r].length() - 2).split('|');
                out += "<tr>";
                for (const QString& cell : cells) {
                    out += "<td>" + cell.trimmed() + "</td>";
                }
                out += "</tr>\n";
            }
            out += "</table>\n";
        } else {
            for (const QString& l : tableBuf) {
                out += l + "\n";
            }
        }
    } else if (inTable) {
        for (const QString& l : tableBuf) {
            out += l + "\n";
        }
    }

    return out;
}

QString ChatView::escapeHtml(const QString& text) const {
    return text.toHtmlEscaped();
}

QVariant ChatView::loadResource(int type, const QUrl& url) {
    if (type != QTextDocument::ImageResource) {
        return QTextBrowser::loadResource(type, url);
    }

    QString urlStr = url.toString();

    // ── HTTP/HTTPS images: cached network fetch ──────────────────────
    if (urlStr.startsWith("http://") || urlStr.startsWith("https://")) {
        bool shouldFetch = false;
        {
            QMutexLocker lock(&m_imageMutex);
            if (m_imageCache.contains(urlStr)) {
                QByteArray data = m_imageCache[urlStr];
                if (!data.isEmpty()) {
                    QImage image;
                    if (image.loadFromData(data)) {
                        if (image.width() > 600) {
                            image = image.scaledToWidth(600, Qt::SmoothTransformation);
                        }
                        return QVariant::fromValue(image);
                    }
                }
                // Empty data = previously failed, don't retry
                return QVariant();
            }
            if (!m_imagePending.contains(urlStr)) {
                m_imagePending.insert(urlStr);
                shouldFetch = true;
            }
        }

        if (shouldFetch) {
            fetchImage(urlStr);
        }
        // Not yet loaded — Qt leaves a blank space until re-render
        return QVariant();
    }

    // ── Local file images: load directly from disk ───────────────────
    // Skills normally emit file:/// URLs, but an LLM may wrap the returned
    // absolute macOS path directly as <img src="/Users/...">. QTextDocument
    // passes that through as a scheme-less QUrl, so accept it explicitly.
    if (url.isLocalFile() || QDir::isAbsolutePath(urlStr)) {
        QString localPath = url.isLocalFile() ? url.toLocalFile() : urlStr;
        QImage image;
        if (image.load(localPath)) {
            if (image.width() > 600) {
                image = image.scaledToWidth(600, Qt::SmoothTransformation);
            }
            return QVariant::fromValue(image);
        }
    }

    // ── Data URIs: Qt doesn't handle data: natively, decode ourselves ─
    if (urlStr.startsWith("data:")) {
        // Format: data:[<mediatype>][;base64],<data>
        int commaIdx = urlStr.indexOf(",");
        if (commaIdx > 0) {
            QString header = urlStr.left(commaIdx);
            bool isBase64 = header.contains(";base64");
            QByteArray encoded = urlStr.mid(commaIdx + 1).toUtf8();
            QByteArray raw;
            if (isBase64) {
                raw = QByteArray::fromBase64(encoded);
            } else {
                raw = QByteArray::fromPercentEncoding(encoded);
            }
            QImage image;
            if (image.loadFromData(raw)) {
                if (image.width() > 600) {
                    image = image.scaledToWidth(600, Qt::SmoothTransformation);
                }
                return QVariant::fromValue(image);
            }
        }
        return QVariant();
    }

    // ── Base class for anything else ─────────────────────────────────
    return QTextBrowser::loadResource(type, url);
}

void ChatView::fetchImage(const QString& urlStr) {
    // Use a background thread to fetch the image
    auto* thread = QThread::create([this, urlStr]() {
        QByteArray data;
        {
            QNetworkAccessManager mgr;
            QNetworkRequest req{QUrl(urlStr)};
            req.setHeader(QNetworkRequest::UserAgentHeader, "PengyAgent/1.0");
            req.setTransferTimeout(10000);

            QNetworkReply* reply = mgr.get(req);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(10000);

            loop.exec();

            if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
                data = reply->readAll();
                if (data.size() > 4 * 1024 * 1024) {
                    data = data.left(4 * 1024 * 1024);
                }
            } else {
                data = QByteArray(); // empty = failed sentinel
            }
            reply->deleteLater();
        }

        {
            QMutexLocker lock(&m_imageMutex);
            m_imageCache[urlStr] = data;
            m_imagePending.remove(urlStr);
        }

        // Trigger re-render on the main thread if we got data
        if (!data.isEmpty()) {
            QMetaObject::invokeMethod(this, "onImageFetched", Qt::QueuedConnection,
                                      Q_ARG(QString, urlStr), Q_ARG(QByteArray, data));
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ChatView::onImageFetched(const QString& /*url*/, const QByteArray& /*data*/) {
    render();
}
