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
#include <QTabWidget>
#include <QFont>
#include "themehelper.h"
#include "iconhelper.h"
#include "about.h"

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

/* Helper: create a QLabel with a tooltip */
static QLabel* labelWithTip(const QString& text, const QString& tip) {
    auto* lbl = new QLabel(text);
    lbl->setToolTip(tip);
    return lbl;
}

SettingsDialog::SettingsDialog(const Config& cfg, QWidget* parent)
    : QDialog(parent), m_config(cfg) {
    setWindowTitle("Settings");
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    auto* tabs   = new QTabWidget;

    // ── UI tab ──────────────────────────────────────────────────
    auto* uiTab  = new QWidget;
    auto* uiForm = new QFormLayout(uiTab);
    uiForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_uiScale = new WidePopupComboBox;
    int scales[] = {75, 100, 110, 125, 135, 150, 175, 200};
    int idx = 1;
    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        m_uiScale->addItem(QString("%1%").arg(scales[i]), scales[i]);
        if (scales[i] == cfg.uiScale) idx = static_cast<int>(i);
    }
    m_uiScale->setCurrentIndex(idx);
    m_uiScale->setToolTip("Scales the entire UI. Restart Pengy to apply a change.");
    uiForm->addRow(labelWithTip("UI Scale:", "Scales the entire UI. Restart Pengy to apply a change."), m_uiScale);
    auto* scaleNote = new QLabel("UI scale changes take effect after restarting Pengy.");
    scaleNote->setWordWrap(true);
    scaleNote->setStyleSheet(QString("color:%1;").arg(makeTheme(cfg.themeMode, cfg.themeAccent)["muted"]));
    uiForm->addRow("", scaleNote);
    m_themeMode = new WidePopupComboBox;
    m_themeMode->addItem("System", "system");
    m_themeMode->addItem("Light", "light");
    m_themeMode->addItem("Dark", "dark");
    for (int i = 0; i < m_themeMode->count(); ++i) {
        if (m_themeMode->itemData(i).toString() == cfg.themeMode) m_themeMode->setCurrentIndex(i);
    }
    m_themeMode->setToolTip("System follows your OS theme; Light and Dark override it.");
    uiForm->addRow(labelWithTip("Theme mode:", "System follows your OS theme; Light and Dark override it."), m_themeMode);

    m_themeAccent = new WidePopupComboBox;
    const QStringList accents = {"default", "blue", "teal", "green", "orange", "red", "pink", "purple"};
    for (const QString& a : accents) m_themeAccent->addItem(a.left(1).toUpper() + a.mid(1), a);
    for (int i = 0; i < m_themeAccent->count(); ++i) {
        if (m_themeAccent->itemData(i).toString() == cfg.themeAccent) m_themeAccent->setCurrentIndex(i);
    }
    m_themeAccent->setToolTip("Highlight color for buttons, links, and selection highlights.");
    uiForm->addRow(labelWithTip("Accent color:", "Highlight color for buttons, links, and selection highlights."), m_themeAccent);

    tabs->addTab(uiTab, "UI");

    // ── LLM tab ─────────────────────────────────────────────────
    auto* llmTab  = new QWidget;
    auto* llmForm = new QFormLayout(llmTab);
    llmForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_baseUrl = new QLineEdit(cfg.baseUrl);
    m_baseUrl->setToolTip("OpenAI-compatible API endpoint, e.g. https://api.openai.com/v1 or a local llama.cpp server.");
    llmForm->addRow(labelWithTip("Base URL:", "OpenAI-compatible API endpoint, e.g. https://api.openai.com/v1 or a local llama.cpp server."), m_baseUrl);

    m_apiKey  = new QLineEdit(cfg.apiKey);
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setToolTip("Bearer token sent in the Authorization header to the LLM provider.");
    llmForm->addRow(labelWithTip("API Key:", "Bearer token sent in the Authorization header to the LLM provider."), m_apiKey);

    auto* modelRow = new QHBoxLayout;
    m_model = new QComboBox;
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    m_model->addItem(cfg.model);
    m_model->setCurrentText(cfg.model);
    // Pre-populate from the persistent model cache (possibly stale, but populated).
    {
        QStringList toAdd;
        for (const QString& m : modelCacheForBaseUrl(cfg.baseUrl))
            if (m != cfg.model) toAdd << m;
        if (!toAdd.isEmpty()) {
            m_model->addItems(toAdd);
            m_model->setCurrentText(cfg.model);
        }
    }
    m_model->setToolTip("Model name sent in chat completion requests. Use Fetch to list available models from the endpoint.");
    modelRow->addWidget(m_model, 1);

    m_fetchBtn = new QPushButton("Fetch");
    m_fetchBtn->setToolTip("Fetch available models from the /models endpoint");
    m_fetchBtn->setFixedWidth(80);
    applyPengyIcon(m_fetchBtn, "refresh", makeTheme(cfg.themeMode, cfg.themeAccent), 15);
    connect(m_fetchBtn, &QPushButton::clicked, this, &SettingsDialog::fetchModels);
    modelRow->addWidget(m_fetchBtn);

    llmForm->addRow(labelWithTip("Model:", "Model name sent in chat completion requests. Use Fetch to list available models from the endpoint."), modelRow);

    m_systemMsg = new QTextEdit(cfg.systemMessage);
    m_systemMsg->setMaximumHeight(scaledSize(100, cfg.uiScale));
    m_systemMsg->setToolTip("The system prompt that sets the assistant's behavior, tone, and constraints.");
    llmForm->addRow(labelWithTip("System Message:", "The system prompt that sets the assistant's behavior, tone, and constraints."), m_systemMsg);

    m_reasoningEffort = new QComboBox;
    m_reasoningEffort->addItem("Provider default — do not send reasoning option", "");
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
    m_reasoningEffort->setToolTip("Optional best-effort reasoning depth hint. Only supported by some models/providers; others may reject unknown values.");
    llmForm->addRow(labelWithTip("Reasoning effort:", "Optional best-effort reasoning depth hint. Only supported by some models/providers."), m_reasoningEffort);

    m_preserveReasoning = new QCheckBox("Keep reasoning fields in conversation history");
    m_preserveReasoning->setChecked(cfg.preserveReasoning);
    m_preserveReasoning->setToolTip("When checked, reasoning_content / reasoning / reasoning_details fields returned by the provider are kept. Leave off if your proxy rejects unknown message fields.");
    llmForm->addRow(labelWithTip("Preserve reasoning:", "When checked, reasoning_content / reasoning / reasoning_details fields returned by the provider are kept."), m_preserveReasoning);

    m_llmTimeout = new QSpinBox;
    m_llmTimeout->setRange(1, 3600);
    m_llmTimeout->setSuffix(" sec");
    m_llmTimeout->setToolTip("HTTP timeout in seconds for each LLM API request. Increase if your model is slow to respond.");
    m_llmTimeout->setValue(cfg.llmTimeout);
    llmForm->addRow(labelWithTip("LLM timeout:", "HTTP timeout in seconds for each LLM API request. Increase if your model is slow to respond."), m_llmTimeout);

    tabs->addTab(llmTab, "LLM");

    // ── Tools tab ───────────────────────────────────────────────
    auto* toolsTab  = new QWidget;
    auto* toolsForm = new QFormLayout(toolsTab);
    toolsForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

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
    m_toolConfirm->setToolTip("YOLO runs everything without asking. Safe auto-approves read-only tools. None confirms every tool call.");
    toolsForm->addRow(labelWithTip("Tool Confirmation:", "YOLO runs everything without asking. Safe auto-approves read-only tools. None confirms every tool call."), m_toolConfirm);

    m_contextKeep = new QSpinBox;
    m_contextKeep->setRange(0, 999);
    m_contextKeep->setSpecialValueText("Keep all");
    m_contextKeep->setSuffix(" turns");
    m_contextKeep->setToolTip("Tool results older than N turns are elided to save context window. 0 = keep everything.");
    m_contextKeep->setValue(cfg.contextKeepTurns);
    toolsForm->addRow(labelWithTip("Keep tool results:", "Tool results older than N turns are elided to save context window. 0 = keep everything."), m_contextKeep);

    m_toolTimeout = new QSpinBox;
    m_toolTimeout->setRange(-1, 3600);
    m_toolTimeout->setSpecialValueText("No timeout");
    m_toolTimeout->setSuffix(" sec");
    m_toolTimeout->setToolTip("Maximum wall-clock time a single tool invocation can run before being killed. -1 = no timeout.");
    m_toolTimeout->setValue(cfg.toolTimeout);
    toolsForm->addRow(labelWithTip("Tool timeout:", "Maximum wall-clock time a single tool invocation can run before being killed. -1 = no timeout."), m_toolTimeout);

    m_toolOutputMax = new QSpinBox;
    m_toolOutputMax->setRange(0, 500000);
    m_toolOutputMax->setSpecialValueText("No limit");
    m_toolOutputMax->setSuffix(" chars");
    m_toolOutputMax->setToolTip("Tool output longer than this is snipped (head+tail) to avoid blowing up the context window. 0 = no limit.");
    m_toolOutputMax->setValue(cfg.toolOutputMaxChars);
    toolsForm->addRow(labelWithTip("Max tool output:", "Tool output longer than this is snipped (head+tail) to avoid blowing up the context window. 0 = no limit."), m_toolOutputMax);

    m_downloadMax = new QSpinBox;
    m_downloadMax->setRange(0, 1000000);
    m_downloadMax->setSpecialValueText("No limit");
    m_downloadMax->setSuffix(" MB");
    m_downloadMax->setToolTip("Default maximum download size for download_file. 0 = no limit.");
    m_downloadMax->setValue(cfg.downloadMaxMb);
    toolsForm->addRow(labelWithTip("Max download:", "Default maximum download size for download_file. 0 = no limit."), m_downloadMax);

    m_userAgent = new QLineEdit(cfg.userAgent);
    m_userAgent->setToolTip("HTTP User-Agent header sent with LLM API requests and any HTTP-based tool calls.");
    toolsForm->addRow(labelWithTip("User Agent:", "HTTP User-Agent header sent with LLM API requests and any HTTP-based tool calls."), m_userAgent);

    tabs->addTab(toolsTab, "Tools");

    // ── About tab ───────────────────────────────────────────────
    auto* aboutTab    = new QWidget;
    auto* aboutLayout = new QVBoxLayout(aboutTab);
    aboutLayout->setSpacing(8);

    auto* versionLabel = new QLabel(pengyEditionLine("C++"));
    QFont versionFont = versionLabel->font();
    versionFont.setBold(true);
    versionFont.setPointSize(versionFont.pointSize() + 2);
    versionLabel->setFont(versionFont);
    aboutLayout->addWidget(versionLabel);

    auto* repoLabel = new QLabel(QString("<a href=\"%1\">%1</a>").arg(kPengyGithubUrl));
    repoLabel->setOpenExternalLinks(true);
    aboutLayout->addWidget(repoLabel);

    auto* websiteLabel = new QLabel(QString("<a href=\"%1\">%1</a>").arg(kPengyWebsiteUrl));
    websiteLabel->setOpenExternalLinks(true);
    aboutLayout->addWidget(websiteLabel);

    auto* descriptionLabel = new QLabel(kPengyDescription);
    descriptionLabel->setWordWrap(true);
    aboutLayout->addWidget(descriptionLabel);

    auto* catbeeLabel = new QLabel(
        QString("%1 <a href=\"%2\">%2</a>").arg(kCatbeeBlurb, kCatbeeUrl));
    catbeeLabel->setWordWrap(true);
    catbeeLabel->setOpenExternalLinks(true);
    aboutLayout->addWidget(catbeeLabel);

    auto* copyrightLabel = new QLabel(
        QString("%1<br><a href=\"%2\">%3</a>")
            .arg(pengyCopyrightLine(), kPengyLicenseUrl, kPengyLicenseName));
    copyrightLabel->setOpenExternalLinks(true);
    aboutLayout->addWidget(copyrightLabel);

    aboutLayout->addStretch();
    tabs->addTab(aboutTab, "About");

    layout->addWidget(tabs);
    layout->addStretch();

    // ── buttons ─────────────────────────────────────────────────
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
        m_config.llmTimeout        = m_llmTimeout->value();
        m_config.toolTimeout       = m_toolTimeout->value();
        m_config.toolOutputMaxChars = m_toolOutputMax->value();
        m_config.downloadMaxMb       = m_downloadMax->value();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    adjustSize();
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

            // Persist so the dropdown stays populated across sessions.
            modelCacheSave(baseUrl, ids);

            QMetaObject::invokeMethod(model, [model, btn, ids, self]() {
                btn->setEnabled(true);
                btn->setText("Fetch");
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
                btn->setText("Fetch");
                if (self) QMessageBox::warning(self, "Fetch Failed", err);
            }, Qt::QueuedConnection);
        }
        reply->deleteLater();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
