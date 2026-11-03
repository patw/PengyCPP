// Extras is setup-only: exercise the real dialog against temporary folders.
#include <QtTest>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFile>
#include "settingsdialog.h"
#include "skillshelper.h"

#ifdef PENGY_RUST_EDITION
#include <cstdlib>
#include <cstring>
// The dialog's unrelated metadata/cache APIs are stubbed; no Rust runtime,
// user config, model or network connection is needed for these UI tests.
extern "C" {
char* pengy_models_cached_for(const char*) {
    auto* p = static_cast<char*>(std::malloc(3));
    std::memcpy(p, "[]", 3); return p;
}
char* pengy_about_json() {
    auto* p = static_cast<char*>(std::malloc(3));
    std::memcpy(p, "{}", 3); return p;
}
bool pengy_models_cache_save(const char*, const char*) { return true; }
void pengy_free(char* p) { std::free(p); }
}
#endif

class TestSettingsExtras : public QObject {
    Q_OBJECT
    QUrl openedUrl;
    QTemporaryDir configDir;

    static auto config(const QString& root) {
        const QJsonObject json{{"skills_dir", root}, {"system_message", "You are Pengy."},
                               {"model", "test-model"}};
#ifdef PENGY_RUST_EDITION
        return json;
#else
        return Config::fromJson(json);
#endif
    }
    static QPushButton* button(SettingsDialog& dialog, const QString& text) {
        for (auto* b : dialog.findChildren<QPushButton*>())
            if (b->text() == text) return b;
        return nullptr;
    }
    static QString message(const SettingsDialog& dialog) {
#ifdef PENGY_RUST_EDITION
        return dialog.config()["system_message"].toString();
#else
        return dialog.config().systemMessage;
#endif
    }
public slots:
    void captureUrl(const QUrl& url) { openedUrl = url; }

private slots:
    void initTestCase() {
        QVERIFY(configDir.isValid());
#ifndef PENGY_RUST_EDITION
        setConfigDir(configDir.path());
#endif
    }
    void cleanupTestCase() {
#ifndef PENGY_RUST_EDITION
        setConfigDir(QString());
#endif
    }

    void setupHasNoChatAction() {
        QTemporaryDir tmp;
        SettingsDialog dialog(config(tmp.filePath("skills")));
        QVERIFY(button(dialog, "Install Skills"));
        QVERIFY(button(dialog, "Open Folder"));
        QVERIFY(button(dialog, "Browse BotSkills"));
        QVERIFY(!button(dialog, "Fill In My Profile"));
        QCOMPARE(dialog.metaObject()->indexOfSignal("promptRequested(QString)"), -1);
        QCOMPARE(dialog.metaObject()->indexOfSlot("startProfileInterview()"), -1);
        auto* tabs = dialog.findChild<QTabWidget*>();
        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(3), QString("Extras"));
    }

    void installStaysInSettingsAndOkSavesPointer() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        SettingsDialog dialog(config(root));
        button(dialog, "Install Skills")->click();
        QVERIFY(Skills::scan(root).installed());
        QVERIFY(QFileInfo(root + "/user_profile/profile.md").isFile());
        QCOMPARE(dialog.result(), int(QDialog::Rejected)); // install never accepts/sends
        QVERIFY(dialog.findChild<QTextEdit*>()->toPlainText().contains("skill_index.md"));
        QVERIFY(!message(dialog).contains("skill_index.md")); // still a draft
        QVERIFY(!button(dialog, "Skills Installed")->isEnabled());
        QVERIFY(button(dialog, "Open Folder")->isEnabled());
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(message(dialog).contains(root + "/skill_index.md"));
    }

    void partialFolderCanBeRepairedWithoutInterview() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        Skills::install(root);
        QVERIFY(QFile::remove(root + "/skill_index.md"));
        SettingsDialog dialog(config(root));
        QVERIFY(!button(dialog, "Fill In My Profile"));
        QVERIFY(button(dialog, "Install Skills")->isEnabled());
        button(dialog, "Install Skills")->click();
        QVERIFY(Skills::scan(root).installed());
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        dialog.reject();
        QVERIFY(!message(dialog).contains("skill_index.md"));
    }

    void folderAndRegistryUseOsUrlHandlers() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        Skills::install(root);
        SettingsDialog dialog(config(root));
        QDesktopServices::setUrlHandler("file", this, "captureUrl");
        QDesktopServices::setUrlHandler("https", this, "captureUrl");
        button(dialog, "Open Folder")->click();
        const QUrl folder = openedUrl;
        button(dialog, "Browse BotSkills")->click();
        const QUrl registry = openedUrl;
        QDesktopServices::unsetUrlHandler("file");
        QDesktopServices::unsetUrlHandler("https");
        QCOMPARE(folder, QUrl::fromLocalFile(root));
        QCOMPARE(registry, QUrl("https://skills.catbee.ca"));
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }
};
QTEST_MAIN(TestSettingsExtras)
#include "test_settings_extras.moc"
