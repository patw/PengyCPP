#include "settingsdialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QThread>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QPointer>
#include <QAbstractItemView>

/* ComboBox whose dropdown popup is ~50% wider than the combo itself,
   so short-content combos (scale %, theme, accent) feel proportional
   when the UI is scaled up. */
class WidePopupComboBox : public QComboBox {
public:
    explicit WidePopupComboBox(QWidget* parent = nullptr) : QComboBox(parent) {}
    void showPopup() override {
        QComboBox::showPopup();
        QWidget* popup = view()->parentWidget();
        if (popup) popup->setFixedWidth(static_cast<int>(width() * 1.5));
    }
};

SettingsDialog::SettingsDialog(const Config& cfg, QWidget* parent)
    : QDialog(parent), m_config(cfg) {
    setWindowTitle("Settings");
    setModal(true);
    resize(520, 520);

    auto* layout = new QVBoxLayout(this);
    auto* form   = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_baseUrl = new QLineEdit(cfg.baseUrl);
    m_apiKey  = new QLineEdit(cfg.apiKey);
    m_apiKey->setEchoMode(QLineEdit::Password);

    auto* modelRow = new QHBoxLayout;
    m_model = new QComboBox;
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    m_model->addItem(cfg.model);
    m_model->setCurrentText(cfg.model);
    modelRow->addWidget(m_model, 1);

    m_fetchBtn = new QPushButton("↻ Fetch");
    m_fetchBtn->setToolTip("Fetch available models from the endpoint");
    m_fetchBtn->setFixedWidth(80);
    connect(m_fetchBtn, &QPushButton::clicked, this, &SettingsDialog::fetchModels);
    modelRow->addWidget(m_fetchBtn);

    m_userAgent = new QLineEdit(cfg.userAgent);

    form->addRow("Base URL:",  m_baseUrl);
    form->addRow("API Key:",   m_apiKey);
    form->addRow("Model:",     modelRow);
    form->addRow("User Agent:", m_userAgent);

    m_systemMsg = new QTextEdit(cfg.systemMessage);
    m_systemMsg->setMaximumHeight(80);
    form->addRow("System Message:", m_systemMsg);

    m_toolConfirm = new QComboBox;
    m_toolConfirm->addItem("YOLO (All) — execute everything, no questions asked",      "all");
    m_toolConfirm->addItem("Safe Only — auto-approve read-only tools; confirm write/execute", "safe");
    m_toolConfirm->addItem("None — confirm every tool before execution",               "none");
    for (int i = 0; i < m_toolConfirm->count(); ++i) {
        if (m_toolConfirm->itemData(i).toString() == cfg.toolConfirmation) {
            m_toolConfirm->setCurrentIndex(i);
            break;
        }
    }
    form->addRow("Tool Confirmation:", m_toolConfirm);

    m_reasoningEffort = new QComboBox;
    m_reasoningEffort->addItem("Provider default — do not send", "");
    m_reasoningEffort->addItem("Off / none", "none");
    m_reasoningEffort->addItem("Minimal", "minimal");
    m_reasoningEffort->addItem("Low", "low");
    m_reasoningEffort->addItem("Medium", "medium");
    m_reasoningEffort->addItem("High", "high");
    m_reasoningEffort->addItem("Extra high", "xhigh");
    m_reasoningEffort->addItem("Max", "max");
    for (int i = 0; i < m_reasoningEffort->count(); ++i) {
        if (m_reasoningEffort->itemData(i).toString() == cfg.reasoningEffort) {
            m_reasoningEffort->setCurrentIndex(i);
            break;
        }
    }
    m_reasoningEffort->setToolTip("Optional best-effort reasoning depth. Provider default omits the parameter.");
    form->addRow("Reasoning effort:", m_reasoningEffort);

    m_preserveReasoning = new QCheckBox("Preserve returned reasoning fields");
    m_preserveReasoning->setChecked(cfg.preserveReasoning);
    m_preserveReasoning->setToolTip("Keeps reasoning_content/reasoning/reasoning_details when providers return them.");
    form->addRow("Reasoning preservation:", m_preserveReasoning);

    m_contextKeep = new QSpinBox;
    m_contextKeep->setRange(0, 999);
    m_contextKeep->setSpecialValueText("Keep all");
    m_contextKeep->setSuffix(" turns");
    m_contextKeep->setToolTip("Tool results older than this many turns are elided. 0 = keep all.");
    m_contextKeep->setValue(cfg.contextKeepTurns);
    form->addRow("Keep tool results:", m_contextKeep);

    m_uiScale = new WidePopupComboBox;
    int scales[] = {75, 100, 125, 150, 175, 200};
    int idx = 1;
    for (int i = 0; i < 6; ++i) {
        m_uiScale->addItem(QString("%1%").arg(scales[i]), scales[i]);
        if (scales[i] == cfg.uiScale) idx = i;
    }
    m_uiScale->setCurrentIndex(idx);
    form->addRow("UI Scale (restart to apply):", m_uiScale);

    m_themeMode = new WidePopupComboBox;
    m_themeMode->addItem("System", "system");
    m_themeMode->addItem("Light", "light");
    m_themeMode->addItem("Dark", "dark");
    for (int i = 0; i < m_themeMode->count(); ++i) {
        if (m_themeMode->itemData(i).toString() == cfg.themeMode) m_themeMode->setCurrentIndex(i);
    }
    form->addRow("Theme mode:", m_themeMode);

    m_themeAccent = new WidePopupComboBox;
    const QStringList accents = {"default", "blue", "teal", "green", "orange", "red", "pink", "purple"};
    for (const QString& a : accents) m_themeAccent->addItem(a.left(1).toUpper() + a.mid(1), a);
    for (int i = 0; i < m_themeAccent->count(); ++i) {
        if (m_themeAccent->itemData(i).toString() == cfg.themeAccent) m_themeAccent->setCurrentIndex(i);
    }
    form->addRow("Accent color:", m_themeAccent);

    m_toolTimeout = new QSpinBox;
    m_toolTimeout->setRange(-1, 3600);
    m_toolTimeout->setSpecialValueText("No timeout");
    m_toolTimeout->setSuffix(" sec");
    m_toolTimeout->setValue(cfg.toolTimeout);
    form->addRow("Tool timeout:", m_toolTimeout);

    layout->addLayout(form);
    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        m_config.baseUrl           = m_baseUrl->text();
        m_config.apiKey            = m_apiKey->text();
        m_config.model             = m_model->currentText();
        m_config.userAgent         = m_userAgent->text();
        m_config.systemMessage     = m_systemMsg->toPlainText();
        m_config.toolConfirmation  = m_toolConfirm->currentData().toString();
        m_config.reasoningEffort   = m_reasoningEffort->currentData().toString();
        m_config.preserveReasoning = m_preserveReasoning->isChecked();
        m_config.contextKeepTurns  = m_contextKeep->value();
        m_config.uiScale           = m_uiScale->currentData().toInt();
        m_config.themeMode         = m_themeMode->currentData().toString();
        m_config.themeAccent       = m_themeAccent->currentData().toString();
        m_config.toolTimeout       = m_toolTimeout->value();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void SettingsDialog::fetchModels() {
    m_fetchBtn->setEnabled(false);
    m_fetchBtn->setText("...");

    QString baseUrl = m_baseUrl->text().trimmed();
    QString apiKey  = m_apiKey->text();

    QThread* thread = QThread::create([this, baseUrl, apiKey]() {
        QNetworkAccessManager mgr;
        QNetworkRequest req(QUrl(baseUrl + "/models"));
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
        req.setRawHeader("api-key",       apiKey.toUtf8());
        req.setRawHeader("User-Agent",    "PengyAgent/1.0");

        QNetworkReply* reply = mgr.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        QComboBox*         model = m_model;
        QPushButton*       btn   = m_fetchBtn;
        QPointer<SettingsDialog> self = this;

        if (reply->error() == QNetworkReply::NoError) {
            QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).object()["data"].toArray();
            QStringList ids;
            for (const QJsonValue& v : arr) {
                QString id = v.toObject()["id"].toString();
                if (!id.isEmpty()) ids << id;
            }
            ids.sort();

            QMetaObject::invokeMethod(model, [model, btn, ids, self]() {
                btn->setEnabled(true);
                btn->setText("↻ Fetch");
                if (ids.isEmpty()) {
                    if (self) QMessageBox::information(self, "No Models",
                                  "The endpoint returned an empty model list.");
                    return;
                }
                QString current = model->currentText();
                model->clear();
                model->addItems(ids);
                model->setCurrentText(ids.contains(current) ? current : ids.first());
            }, Qt::QueuedConnection);
        } else {
            int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString err = QString("HTTP %1 from %2/models\n\nCheck your Base URL and API Key.")
                          .arg(code).arg(baseUrl);
            QMetaObject::invokeMethod(model, [btn, err, self]() {
                btn->setEnabled(true);
                btn->setText("↻ Fetch");
                if (self) QMessageBox::warning(self, "Fetch Failed", err);
            }, Qt::QueuedConnection);
        }
        reply->deleteLater();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
