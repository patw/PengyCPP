#include "compactionsettings.h"
#include <QApplication>
#include <QTest>
#include <QJsonObject>

class CompactionControlsTest : public QObject {
    Q_OBJECT
private slots:
    void perModelDraftsAndCancel() {
        QJsonObject config{{"context_windows",QJsonObject{{"local",262144},{"fast",1048576}}},{"compact_at_percent",75},{"auto_context_recovery",true}};
        QComboBox model;model.setEditable(true);model.addItems({"local","fast"});model.setCurrentText("local");
        CompactionSettings widget(config,&model);
        auto mode=widget.findChild<QComboBox*>("compactionMode");auto window=widget.findChild<QSpinBox*>("compactionWindow");auto percent=widget.findChild<QSpinBox*>("compactionPercent");
        QVERIFY(mode&&window&&percent);QCOMPARE(mode->currentData().toString(),QString("explicit"));QCOMPARE(window->value(),262144);
        mode->setCurrentIndex(mode->findData("off"));model.setCurrentText("fast");
        QCOMPARE(mode->currentData().toString(),QString("explicit"));QCOMPARE(window->value(),1048576);
        // Until apply(), callers' settings are untouched (Cancel semantics).
        QVERIFY(!config.contains("context_window_modes"));
        percent->setValue(80);auto saved=widget.apply(config);
        QCOMPARE(saved["context_window_modes"].toObject()["local"].toString(),QString("off"));
        QVERIFY(!saved["context_window_modes"].toObject().contains("fast"));
        QCOMPARE(saved["context_windows"],config["context_windows"]);
        QCOMPARE(saved["auto_context_recovery"].toBool(),true);QCOMPARE(saved["compact_at_percent"].toInt(),80);
        model.setCurrentText("local");mode->setCurrentIndex(mode->findData("auto"));saved=widget.apply(config);
        QCOMPARE(saved["context_window_modes"].toObject()["local"].toString(),QString("auto"));QCOMPARE(saved["context_windows"],config["context_windows"]);
    }
    void unknownModelAndExplicitWindow() {
        QComboBox model;model.setEditable(true);
        CompactionSettings widget(QJsonObject{},&model);
        auto mode=widget.findChild<QComboBox*>("compactionMode");auto window=widget.findChild<QSpinBox*>("compactionWindow");
        QVERIFY(!mode->isEnabled());model.setCurrentText("limited");mode->setCurrentIndex(mode->findData("explicit"));window->setValue(131072);
        auto saved=widget.apply({});QCOMPARE(saved["context_windows"].toObject()["limited"].toInt(),131072);
        QCOMPARE(saved["context_window_modes"].toObject()["limited"].toString(),QString("explicit"));
    }
};
QTEST_MAIN(CompactionControlsTest)
#include "test_compactionsettings.moc"
