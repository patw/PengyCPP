// ChatView render-cache + follow-the-feed regression tests.
// Mirrors PengyR's gui/tests/test_chatview_markdown.cpp.
//
// Build: cmake -DCMAKE_BUILD_TESTS=ON .. && cmake --build . --target pengy_chatview_test
#include <QApplication>
#include <QString>
#include <QJsonObject>
#include <QScrollBar>
#include <QWheelEvent>
#include <QAbstractSlider>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QImage>
#include <QDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QTemporaryDir>
#include "imagesave.h"
#include <iostream>
#include "chatview.h"
#include <QScrollBar>
#include <QThread>

static void requireContains(const QString& haystack, const QString& needle, const char* label) {
    if (!haystack.contains(needle)) {
        std::cerr << "FAIL: " << label << "\nExpected to contain: "
                  << needle.toStdString() << "\nGot:\n"
                  << haystack.toStdString() << std::endl;
        std::exit(1);
    }
}

static void requireNotContains(const QString& haystack, const QString& needle, const char* label) {
    if (haystack.contains(needle)) {
        std::cerr << "FAIL: " << label << "\nExpected NOT to contain: "
                  << needle.toStdString() << "\nGot:\n"
                  << haystack.toStdString() << std::endl;
        std::exit(1);
    }
}

static void requireEqual(const QString& got, const QString& want, const char* label) {
    if (got != want) {
        std::cerr << "FAIL: " << label << "\nExpected:\n"
                  << want.toStdString() << "\nGot:\n"
                  << got.toStdString() << std::endl;
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ChatView view;
    // The transcript must never claim a drop meant for the composer.  A read-only
    // QTextBrowser declines drops today, but that is Qt-internal behaviour, so the
    // flag is turned off explicitly.
    if (view.acceptDrops()) {
        std::cerr << "FAIL: transcript claims drops" << std::endl;
        return 1;
    }

    // Role labels: defaults are the shipped strings, MainWindow's derivation
    // replaces them, and a label is config-sourced text that must be escaped.
    {
        ChatView labelled;
        labelled.appendMessageText("user", "hi");
        labelled.appendMessageText("assistant", "hello");
        requireContains(labelled.testBuildHtml(), "class='role-user'>You",
                        "default human label");
        requireContains(labelled.testBuildHtml(), "class='role-assistant'>Assistant",
                        "default agent label");
        labelled.applyIdentity("Pat", "Pengy (behopc)");
        requireContains(labelled.testBuildHtml(), "class='role-user'>Pat &#x1F9D1;",
                        "human label follows config, label-first");
        requireContains(labelled.testBuildHtml(), "class='role-assistant'>Pengy (behopc) &#x1F916;",
                        "agent label names the node");
        labelled.applyIdentity("<b>Pat</b>", "Assistant");
        requireNotContains(labelled.testBuildHtml(), "class='role-user'><b>Pat",
                           "label is HTML-escaped, not injected");
        requireContains(labelled.testBuildHtml(), "&lt;b&gt;Pat",
                        "escaped label still renders");
    }

    // Inline images and attachment thumbnails open a larger, dismissible Qt preview.
    {
        QTemporaryDir temp;
        const QString image = temp.filePath("wide.png");
        if (!QImage(1000, 500, QImage::Format_RGB32).save(image)) std::exit(1);
        ChatView v;
        v.resize(700, 600);
        v.show();
        v.appendMessageText("assistant", "![Picture](" + QUrl::fromLocalFile(image).toString() + ")");
        QApplication::processEvents();
        requireEqual(v.testImageAt(QPoint(100, 60)), QUrl::fromLocalFile(image).toString(), "image hit");
        requireEqual(v.testImageAt(QPoint(630, 60)), "", "outside image");
        v.testOpenImagePreview(QUrl::fromLocalFile(image).toString());
        auto* preview = v.findChild<QDialog*>("pengyImagePreview");
        if (!preview || !preview->isVisible()) std::exit(1);
        auto* picture = preview->findChild<QLabel*>("pengyImagePreviewPicture");
        if (!picture || picture->pixmap().width() <= 600) std::exit(1);
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(preview, &click);
        if (preview->isVisible()) std::exit(1);
        v.testOpenImagePreview(QUrl::fromLocalFile(temp.filePath("missing.png")).toString());
        if (preview->isVisible()) std::exit(1);
    }

    // Save full-size attachment bytes rather than thumbnail or 600px inline image.
    {
        QTemporaryDir temp;
        const QString thumb = temp.filePath("thumbnail-256-v1.jpg");
        const QString display = temp.filePath("image-display-v1.jpg");
        if (!QImage(100, 50, QImage::Format_RGB32).save(thumb)
            || !QImage(900, 450, QImage::Format_RGB32).save(display)) std::exit(1);
        const SavableImage saved = imageBytesForSave(QUrl::fromLocalFile(thumb).toString(), {});
        QFile file(display);
        if (!file.open(QIODevice::ReadOnly) || saved.bytes != file.readAll()
            || saved.name != "image-display-v1.jpg") std::exit(1);
        const QByteArray bytes = saved.bytes;
        QMap<QString, QByteArray> cache{{"https://example.org/pic?x=1", bytes}};
        if (imageBytesForSave("https://example.org/pic?x=1", cache).bytes != bytes
            || imageBytesForSave("https://example.org/pic?x=1", {}).isValid()
            || imageBytesForSave("data:image/jpeg;base64," + QString::fromLatin1(bytes.toBase64()), {}).bytes != bytes)
            std::exit(1);
    }

    // ── render cache ────────────────────────────────────────────────────
    {
        ChatView v;

        v.appendMessageText("user", "hello **world**", false);
        QJsonObject asst;
        asst["content"] = QString("```python\nprint(1)\n```");
        asst["reasoning_content"] = QString("head\nTAIL-LINE");
        v.appendMessage("assistant", asst, false);
        requireEqual(v.testBuildHtml(), v.testBuildHtmlCold(), "cached == cold (user+assistant)");
        requireEqual(QString::number(v.testCacheSize()), "2", "cache tracks appends");

        QJsonObject req;
        req["tool_call_id"] = "t1";
        req["name"] = "read_file";
        req["args"] = QJsonObject{{"path", "/x"}};
        v.appendMessage("tool_request", req, false);
        requireContains(v.testBuildHtml(), "running", "pending tool shows (running...)");

        QJsonObject res;
        res["tool_call_id"] = "t1";
        res["content"] = QString("SECRET-PAYLOAD");
        res["declined"] = false;
        v.appendMessage("tool_result", res, false);
        QString afterResult = v.testBuildHtml();
        requireNotContains(afterResult, "running", "tool result clears (running...)");
        requireEqual(afterResult, v.testBuildHtmlCold(), "cached == cold (after tool result)");

        v.testBuildHtml();
        v.testExpandTool("t1");
        requireContains(v.testBuildHtml(), "SECRET-PAYLOAD", "tool expand shows result");

        v.testBuildHtml();
        v.testExpandReasoning(1);
        requireContains(v.testBuildHtml(), "TAIL-LINE", "reasoning expand shows full text");

        v.testBuildHtml();
        v.applyTheme(makeTheme("dark", "default"), 100);
        requireEqual(v.testBuildHtml(), v.testBuildHtmlCold(), "cached == cold (after theme change)");

        v.clear();
        requireEqual(QString::number(v.testCacheSize()), "0", "clear resets cache");
    }

    // ── harness cards (notice / error) ────────────────────────────────
    // Neither is an assistant turn: each renders on its own highlighted surface,
    // without the "Assistant" label they used to carry.
    {
        struct Card { const char* role; const char* text; const char* cls;
                      const char* bg; const char* fg; };
        const Card cards[] = {
            {"notice", "Context recovery — full history retained.", "notice-card", "#cff4fc", "#055160"},
            {"error",  "Error: API error (HTTP 500): boom",        "error-card",  "#f8d7da", "#58151c"},
        };
        for (const Card& c : cards) {
            ChatView v;
            // Pin the theme so the resolved CSS colours are deterministic (the
            // buildCss placeholder chain is positional; a bad index would show
            // up here as a wrong or missing colour).
            v.applyTheme(makeTheme("light", "default"), 100);
            v.appendMessage(c.role, QString::fromUtf8(c.text), false);
            const QString html = v.testBuildHtml();
            requireContains(html, QString("<div class='%1'>").arg(c.cls), "card renders as its own surface");
            requireContains(html, QString::fromUtf8(c.text), "card text present");
            requireContains(html, c.bg, "card bg colour resolved in CSS");
            requireContains(html, c.fg, "card fg colour resolved in CSS");
            requireNotContains(html, "class='role-assistant'", "card is not labelled Assistant");
            requireNotContains(html, "&#x1F916; Assistant", "card carries no assistant header");
            requireEqual(html, v.testBuildHtmlCold(), "cached == cold (card)");
        }
    }

    // ── auto-scroll pin (regression: "snaps back up to old history") ────
    // ── Local image URLs ───────────────────────────────────────────────
    // Skills emit file:/// URLs, but an LLM can turn a returned absolute path
    // into raw <img src="/Users/...">. Both must reach the chat document as
    // usable image resources rather than leaving a broken-image placeholder.
    {
        const QString imagePath = QDir::homePath()
            + "/Pictures/even-weirder-elephants-zoom-background.png";
        if (QFile::exists(imagePath)) {
            ChatView v;
            const QString rawPathHtml = v.testMarkdownToHtml(
                QString("<img src=\"%1\" alt=\"local image\">").arg(imagePath));
            const QString fileUrlHtml = v.testMarkdownToHtml(
                QString("<img src=\"%1\" alt=\"local image\">")
                    .arg(QUrl::fromLocalFile(imagePath).toString()));
            requireContains(rawPathHtml, imagePath, "raw absolute image path survives markdown");
            requireContains(fileUrlHtml, "file:///", "file URL survives markdown");
            const QUrl fileUrl = QUrl::fromLocalFile(imagePath);
            requireContains(fileUrlHtml, QString("src=\"%1\"").arg(fileUrl.toString()),
                            "raw HTML image source has literal quotes");
            const QVariant rawImage = v.testLoadImage(QUrl(imagePath));
            const QVariant fileImage = v.testLoadImage(fileUrl);
            if (!rawImage.canConvert<QImage>() || rawImage.value<QImage>().isNull()) {
                std::cerr << "FAIL: raw absolute image path loads" << std::endl;
                return 1;
            }
            if (!fileImage.canConvert<QImage>() || fileImage.value<QImage>().isNull()) {
                std::cerr << "FAIL: file URL image loads" << std::endl;
                return 1;
            }
            v.appendMessageText("assistant",
                QString("<img src=\"%1\" alt=\"local image\">").arg(fileUrl.toString()));
            app.processEvents();
            const QVariant documentImage = v.document()->resource(
                QTextDocument::ImageResource, fileUrl);
            if (!documentImage.canConvert<QImage>() || documentImage.value<QImage>().isNull()) {
                std::cerr << "FAIL: rendered document resolves file:/// image" << std::endl;
                return 1;
            }
        }
    }

    // ── following the feed (regression: "bounces back up to old history") ───
    // setHtml() replaces the whole document: Qt resets the bar, moves the
    // maximum over several layout passes, and reports valueChanged() for moves
    // of its own. Reading intent out of that geometry is what stranded the view
    // mid-history while output kept arriving. Intent now comes from real input
    // events only (wheel / keys / scrollbar), and the reading spot is held by
    // document position. These drive the widget the way a person does, because
    // a programmatic setValue() must NOT read as a scroll.
    {
        ChatView v;
        v.resize(400, 600);
        v.show();
        for (int i = 0; i < 60; ++i)
            v.appendMessageText("assistant", QString("line %1 ").arg(i).repeated(20), false);
        v.renderNow();
        for (int i = 0; i < 4; ++i) app.processEvents();

        auto* sb = v.verticalScrollBar();
        auto settle = [&app](int rounds = 4) {
            for (int i = 0; i < rounds; ++i) app.processEvents();
        };
        auto wheel = [&app, &v](int notches) {
            QPointF pos(20, 20);
            QWheelEvent event(pos, v.viewport()->mapToGlobal(QPoint(20, 20)),
                              QPoint(0, 0), QPoint(0, 120 * notches),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(v.viewport(), &event);
        };

        requireEqual(v.testFollow() ? "true" : "false", "true", "follow starts on");
        requireEqual(QString::number(sb->value()), QString::number(sb->maximum()),
                     "loaded chat starts at the end");

        // New output keeps the view on the last line.
        for (int n = 0; n < 3; ++n) {
            v.appendMessageText("assistant", QString("chunk %1 ").arg(n).repeated(40));
            settle();
            if (sb->value() != sb->maximum()) {
                std::cerr << "FAIL: did not stay pinned to the end\nvalue=" << sb->value()
                          << " max=" << sb->maximum() << std::endl;
                std::exit(1);
            }
        }

        // A scroll the user did not make (what setHtml() / relayout do) must not
        // clear the follow flag — that was the bug.
        sb->setValue(0);
        settle();
        requireEqual(v.testFollow() ? "true" : "false", "true", "programmatic scroll keeps following");
        v.appendMessageText("assistant", QString("more output ").repeated(40));
        settle();
        if (sb->value() != sb->maximum()) {
            std::cerr << "FAIL: programmatic scroll stranded the view\nvalue=" << sb->value()
                      << " max=" << sb->maximum() << std::endl;
            std::exit(1);
        }

        // A wheel notch towards older output stops following.
        wheel(+3);
        settle();
        requireEqual(v.testFollow() ? "true" : "false", "false", "wheel up stops following");

        // …and the reading spot then holds still while output arrives.
        const int reading = sb->value();
        for (int n = 0; n < 4; ++n) {
            v.appendMessageText("assistant", QString("while reading %1 ").arg(n).repeated(40));
            settle();
            if (sb->value() != reading) {
                std::cerr << "FAIL: view drifted while reading\n" << reading << " -> "
                          << sb->value() << std::endl;
                std::exit(1);
            }
        }

        // Back at the bottom: "give me the live feed again".
        sb->triggerAction(QAbstractSlider::SliderToMaximum);
        settle();
        requireEqual(v.testFollow() ? "true" : "false", "true", "bottom resumes following");
        if (sb->value() != sb->maximum()) {
            std::cerr << "FAIL: resuming did not reach the end\nvalue=" << sb->value()
                      << " max=" << sb->maximum() << std::endl;
            std::exit(1);
        }
        v.appendMessageText("assistant", QString("live again ").repeated(40));
        settle();
        if (sb->value() != sb->maximum()) {
            std::cerr << "FAIL: did not follow after resuming\nvalue=" << sb->value()
                      << " max=" << sb->maximum() << std::endl;
            std::exit(1);
        }

        // A scrollbar trough click upwards also stops following.
        sb->triggerAction(QAbstractSlider::SliderPageStepSub);
        settle();
        requireEqual(v.testFollow() ? "true" : "false", "false", "page-up click stops following");

        // Holding the thumb defers the rebuild so the drag is not fought.
        v.verticalScrollBar()->sliderPressed();
        v.appendMessageText("assistant", QString("arrives mid-drag ").repeated(40));
        requireEqual(v.testRenderPending() ? "true" : "false", "true", "drag defers the render");
        v.verticalScrollBar()->sliderReleased();
        settle();
        requireEqual(v.testRenderPending() ? "true" : "false", "false", "release catches up");

        // ── burst: several renders before the deferred scroll work runs ──────
        // A real turn emits cards back to back. render() used to re-read the
        // viewport for each one, but setHtml() had already reset the bar, so the
        // anchor became the document start and the view jumped to the very top.
        {
            ChatView b;
            b.resize(400, 600);
            b.show();
            for (int i = 0; i < 40; ++i)
                b.appendMessageText("assistant", QString("line %1 ").arg(i).repeated(20), false);
            b.renderNow();
            auto settleB = [&app](int rounds = 6) {
                for (int i = 0; i < rounds; ++i) app.processEvents();
            };
            settleB();
            auto* sbb = b.verticalScrollBar();
            // A real wheel notch, so the setup does not depend on when Qt applies
            // an action triggered from code.
            QPointF pos(20, 20);
            QWheelEvent up(pos, b.viewport()->mapToGlobal(QPoint(20, 20)),
                           QPoint(0, 0), QPoint(0, 120 * 6),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(b.viewport(), &up);
            settleB();
            const int reading = sbb->value();
            if (reading <= 0 || reading >= sbb->maximum()) {
                std::cerr << "FAIL: could not scroll back to set up the burst test (value="
                          << reading << " max=" << sbb->maximum() << ")\n";
                std::exit(1);
            }
            for (int n = 0; n < 3; ++n) {
                QJsonObject req{{"tool_call_id", QString("b%1").arg(n)},
                                {"name", "bash"}, {"args", QJsonObject()}};
                b.appendMessage("tool_request", req);
                QJsonObject res{{"tool_call_id", QString("b%1").arg(n)},
                                {"content", QString("out\n").repeated(40)},
                                {"declined", false}};
                b.appendMessage("tool_result", res);
                b.appendMessageText("assistant", QString("narration %1 ").arg(n).repeated(30));
            }
            settleB();
            const int drift = sbb->value() - reading;
            if (sbb->value() == 0 || drift < -60 || drift > 60) {
                std::cerr << "FAIL: a burst of output moved the reading spot\n"
                          << "reading=" << reading << " after burst=" << sbb->value()
                          << " max=" << sbb->maximum() << "\n";
                std::exit(1);
            }
            requireEqual(b.testFollow() ? "true" : "false", "false", "burst keeps the mode");
        }

        // A hidden tab (another tab in front) keeps its place, or the end.
        v.hide();
        settle();
        v.appendMessageText("assistant", QString("produced while hidden ").repeated(40));
        settle();
        v.show();
        settle();
        requireEqual(v.testFollow() ? "true" : "false", "false", "hidden tab keeps the mode");
        requireEqual(v.testAnchorHeld() ? "true" : "false", "true", "hidden tab holds an anchor");

        v.clear();
        requireEqual(v.testFollow() ? "true" : "false", "true", "clear resets follow");
    }

    // Search jump: revealHits() marks the words in the hit messages only (not
    // labels or other messages), anchors the first, scrolls to it and stops
    // following; a later bottom-scroll queued by the tab opening must not
    // undo it; sending a message clears the marks and follows again.
    {
        ChatView v;
        v.resize(500, 300);
        QString filler;
        for (int i = 0; i < 60; ++i) filler += QString("paragraph %1\n\n").arg(i);
        v.appendMessageText("user", "first question", false, 0);
        v.appendMessageText("assistant", filler, false, 1);
        QJsonObject req{{"tool_call_id", "t"}, {"name", "glob"}, {"args", QJsonObject()}};
        v.appendMessage("tool_request", req, false);
        v.appendMessageText("assistant", "the NGINX & <b>answer</b>", false, 3);
        v.appendMessageText("user", "thanks nginx\n\n" + filler, false, 5);
        v.renderNow();  // queues a scroll-to-bottom, like opening a tab does
        v.show();

        if (!v.revealHits({3}, {"nginx", "&"})) { std::cerr << "FAIL: hit not found\n"; std::exit(1); }
        if (v.testHitMarks() != QList<int>{3}) { std::cerr << "FAIL: wrong messages marked\n"; std::exit(1); }
        const QString hit = v.testCachedHtml(3);
        requireContains(hit, "<a name='pengy-hit'></a>", "first hit is anchored");
        requireContains(hit, "<span class='search-hit'>NGINX</span>", "word marked case-insensitively");
        requireContains(hit, "<span class='search-hit'>&amp;</span>", "escaped word marked");
        requireNotContains(v.testCachedHtml(4), "search-hit", "other messages are not marked");
        requireNotContains(hit, "Assistant</span>", "label not marked");

        auto* sb = v.verticalScrollBar();
        for (int i = 0; i < 50 && (sb->value() == 0 || sb->value() == sb->maximum()); ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        if (sb->value() == 0 || sb->value() == sb->maximum()) {
            std::cerr << "FAIL: did not scroll to the hit, value=" << sb->value()
                      << " max=" << sb->maximum() << std::endl;
            std::exit(1);
        }
        requireEqual(v.testFollow() ? "true" : "false", "false", "jump stops following");

        if (v.revealHits({42}, {"nginx"})) { std::cerr << "FAIL: redacted hit reported\n"; std::exit(1); }
        v.revealHits({3}, {"nginx"});
        v.appendMessageText("user", "next", true, 6);
        requireEqual(v.testFollow() ? "true" : "false", "true", "sending follows again");
        requireNotContains(v.testCachedHtml(3), "search-hit", "sending clears the marks");
    }

    return 0;
}
