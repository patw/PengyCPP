// ChatView render-cache + auto-scroll pin regression tests.
// Mirrors PengyR's gui/tests/test_chatview_markdown.cpp.
//
// Build: cmake -DCMAKE_BUILD_TESTS=ON .. && cmake --build . --target pengy_chatview_test
#include <QApplication>
#include <QString>
#include <QJsonObject>
#include <QScrollBar>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QImage>
#include <iostream>
#include "chatview.h"

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

    // setHtml() replaces the whole document and resets the scrollbar to 0.
    // The old render() decided "am I at the bottom?" by reading sb->value()
    // *after* that reset — so any render landing while a previous render's
    // deferred scroll-to-bottom was still pending read value()==0, concluded
    // the user had scrolled up, and pinned the view to the top of the history.
    // These guard the explicit m_autoScroll flag that replaced that check.
    {
        ChatView v;
        v.resize(400, 600);
        v.show();
        app.processEvents();

        for (int i = 0; i < 60; ++i)
            v.appendMessageText("assistant", QString("line %1 ").arg(i).repeated(20), false);
        v.renderNow();
        app.processEvents();

        requireEqual(v.testAutoScroll() ? "true" : "false", "true", "pin starts true");
        v.renderNow();
        app.processEvents();
        requireEqual(v.testAutoScroll() ? "true" : "false", "true", "pin survives a render");

        v.renderNow();
        v.renderNow();
        app.processEvents();
        requireEqual(v.testAutoScroll() ? "true" : "false", "true", "interleaved render keeps the pin");

        v.verticalScrollBar()->setValue(0);
        app.processEvents();
        requireEqual(v.testAutoScroll() ? "true" : "false", "false", "genuine scroll up clears the pin");

        v.renderNow();
        app.processEvents();
        auto* sb = v.verticalScrollBar();
        if (sb->value() >= sb->maximum() / 2) {
            std::cerr << "FAIL: cleared pin yanked to bottom\n"
                      << "value=" << sb->value() << " max=" << sb->maximum() << std::endl;
            std::exit(1);
        }

        v.clear();
        requireEqual(v.testAutoScroll() ? "true" : "false", "true", "clear resets the pin");
    }

    return 0;
}
