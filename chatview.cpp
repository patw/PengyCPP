#include "chatview.h"
#include <QScrollBar>
#include <QDesktopServices>
#include <QRegularExpression>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUrl>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QFontDatabase>

ChatView::ChatView(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    m_theme = makeTheme("system", "default");
    applyTheme(m_theme, m_scale);
}

void ChatView::applyTheme(const Theme& theme, int scale) {
    m_theme = theme;
    m_scale = scale;
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(scaledFont(10, scale));
    setFont(font);
    setStyleSheet(QString("QTextBrowser { background-color:%1; color:%2; border:none; padding:0; }")
                  .arg(m_theme["bg"], m_theme["fg"]));
    document()->setDefaultStyleSheet(buildCss());
    m_cachedCss = buildCss();  // cache for use in buildHtml()
    if (!m_messages.isEmpty()) render();
}

QString ChatView::buildCss() const {
    QString fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    double bodyPt = scaledFont(10, m_scale);
    double labelPt = scaledFont(9, m_scale);
    double reasoningLabelPt = scaledFont(8.5, m_scale);
    return QString(R"CSS(
body { font-family:"%1"; font-size:%2pt; background-color:%3; color:%4; margin:8px; }
a { color:%5; text-decoration:none; }
pre { background-color:%15; color:%14; padding:10px; margin:6px 0; white-space:pre-wrap; word-wrap:break-word; }
.code-lang { font-size:%9pt; color:%16; margin-bottom:2px; font-family:monospace; }
table { border:1px solid %6; margin:6px 0; }
th, td { border:1px solid %6; padding:4px 10px; }
th { background-color:%7; font-weight:bold; }
img { max-width:600px; }
.role-user { color:%8; font-weight:bold; font-size:%9pt; margin:8px 0 2px 0; }
.role-assistant { color:%10; font-weight:bold; font-size:%9pt; margin:8px 0 2px 0; }
.tool-card { border:1px solid %11; padding:4px 8px; margin:6px 0; background-color:%12; }
.tool-link { color:%5; text-decoration:none; font-weight:bold; }
.tool-pre { background-color:%13; color:%14; padding:4px; margin:2px 0; font-size:%9pt; }
.muted { color:%16; }
.declined { color:%17; }
.reasoning-card { border:1px solid %18; padding:6px 10px; margin:6px 0; background-color:%19; }
.reasoning-link { color:%20; text-decoration:none; font-weight:bold; }
.reasoning-body { color:%16; font-size:%21pt; white-space:pre-wrap; word-wrap:break-word; margin-top:4px; }
code { background-color:%7; color:%14; padding:1px 3px; border-radius:2px; }
blockquote { border-left:3px solid %6; margin:6px 0; padding:2px 0 2px 10px; color:%16; }
hr { border:0; border-top:1px solid %6; margin:10px 0; }
ul, ol { margin:4px 0 8px 20px; padding-left:16px; }
li { margin:2px 0; }
h1, h2, h3, h4, h5, h6 { margin:10px 0 4px 0; }
h1 { font-size:14pt; } h2 { font-size:13pt; } h3 { font-size:11pt; } h4 { font-size:10pt; }
)CSS")
        .arg(fixed).arg(bodyPt).arg(m_theme["bg"], m_theme["fg"], m_theme["link"], m_theme["border"], m_theme["panel_2"],
             m_theme["user_label"]).arg(labelPt).arg(m_theme["assistant_label"], m_theme["border_soft"], m_theme["tool_bg"],
             m_theme["tool_arg_bg"], m_theme["code_fg"], m_theme["code_bg"], m_theme["muted"], m_theme["danger"],
             m_theme["reasoning_border"], m_theme["reasoning_bg"], m_theme["reasoning_fg"]).arg(reasoningLabelPt);
}

void ChatView::appendMessage(const QString& role, const QJsonValue& content, bool doRender) {
    if (role == "tool_request") {
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
    } else if (role == "tool_result") {
        // Find matching tool_block and set result
        QJsonObject obj = content.toObject();
        QString tcId = obj["tool_call_id"].toString();
        for (int i = m_messages.size() - 1; i >= 0; --i) {
            QJsonObject msg = m_messages[i].toObject();
            if (msg["role"].toString() == "tool_block"
                && msg["tool_call_id"].toString() == tcId) {
                msg["result"] = obj["content"];
                msg["declined"] = obj["declined"].toBool(false);
                m_messages[i] = msg;
                break;
            }
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
        m_messages.append(msg);
    }
    if (doRender)
        render();
}

void ChatView::renderNow() {
    render();
}

void ChatView::clear() {
    m_messages = QJsonArray();
    m_expandedTools.clear();
    m_expandedReasoning.clear();
    QTextBrowser::clear();
}

void ChatView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QString anchor = anchorAt(event->pos());
        if (anchor.startsWith("toggle://")) {
            QString toolId = anchor.mid(9);  // strlen("toggle://")
            if (m_expandedTools.contains(toolId)) {
                m_expandedTools.remove(toolId);
            } else {
                m_expandedTools.insert(toolId);
            }
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

void ChatView::render() {
    auto* sb = verticalScrollBar();
    bool atBottom = sb->value() >= sb->maximum() - 30;
    int prev = sb->value();

    setHtml(buildHtml());

    if (atBottom) {
        sb->setValue(sb->maximum());
    } else {
        sb->setValue(prev);
    }
}

QString ChatView::buildHtml() {
    QString html = QString("<html><head><style>%1</style></head><body>").arg(m_cachedCss);
    for (int i = 0; i < m_messages.size(); ++i) {
        html += renderMessage(m_messages[i].toObject(), i);
    }
    html += "</body></html>";
    return html;
}

QString ChatView::renderMessage(const QJsonObject& msg, int idx) const {
    QString role = msg["role"].toString();

    if (role == "user") {
        return QString(
            "<p class='role-user'>&#x1F9D1; You</p>"
            "<p style='margin:2px 0 10px 0;white-space:pre-wrap;'>%1</p>"
        ).arg(escapeHtml(msg["content"].toString()));

    } else if (role == "assistant") {
        QString content = msg["content"].toString();
        if (content.isEmpty()) return "";
        QString parts;
        if (msg.contains("reasoning_content")) {
            parts += renderReasoningBlock(msg["reasoning_content"].toString(), idx);
        }
        parts += QString(
            "<p class='role-assistant'>&#x1F916; Assistant</p>"
            "<div style='margin:2px 0 10px 0;'>%1</div>"
        ).arg(markdownToHtml(content));
        return parts;

    } else if (role == "tool_block") {
        return renderToolBlock(msg);
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
    if (urlStr.startsWith("file://")) {
        QString localPath = url.toLocalFile();
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
