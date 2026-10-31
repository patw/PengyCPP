#include "settingsdialog.h"
#include "compactionsettings.h"
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
#include <QTimer>
#include <QApplication>
#include <QPointer>
#include <QAbstractItemView>
#include <QTabWidget>
#include <QFont>
#include "themehelper.h"
#include "iconhelper.h"
#include "about.h"
#include "skillshelper.h"
#include <QFileDialog>
#include <QDesktopServices>
#include <QUrl>

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
    const QString baseUrlTip = QString::fromUtf8("OpenAI-compatible API endpoint. The default is a local Ollama server, which needs no API key \u2014 e.g. http://127.0.0.1:11434/v1 (Ollama) or http://127.0.0.1:8080/v1 (llama.cpp).");
    m_baseUrl->setToolTip(baseUrlTip);
    llmForm->addRow(labelWithTip("Base URL:", baseUrlTip), m_baseUrl);

    m_apiKey  = new QLineEdit(cfg.apiKey);
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setToolTip("Bearer token sent in the Authorization header to the LLM provider.");
    llmForm->addRow(labelWithTip("API Key:", "Bearer token sent in the Authorization header to the LLM provider."), m_apiKey);

    auto* modelRow = new QHBoxLayout;
    m_model = new QComboBox;
    m_model->setEditable(true);
    m_model->setInsertPolicy(QComboBox::NoInsert);
    // The default model is deliberately empty (a local endpoint ships no model of
    // its own), so on a fresh install there is nothing to pre-select -- say what
    // to do about it rather than showing a blank entry in the list.
    if (!cfg.model.isEmpty()) {
        m_model->addItem(cfg.model);
        m_model->setCurrentText(cfg.model);
    }
    m_model->setPlaceholderText(QString::fromUtf8("Pick a model \u2014 press Fetch"));
    // Pre-populate from the persistent model cache (possibly stale, but populated).
    {
        QStringList toAdd;
        for (const QString& m : modelCacheForBaseUrl(cfg.baseUrl))
            if (m != cfg.model) toAdd << m;
        if (!toAdd.isEmpty()) {
            m_model->addItems(toAdd);
            // With no model configured, offer the first fetched one.
            m_model->setCurrentText(cfg.model.isEmpty() ? toAdd.first() : cfg.model);
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

    m_compaction = new CompactionSettings(cfg.toJson(), m_model);
    llmForm->addRow(m_compaction);

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

    // ── Extras tab ─────────────────────────────────────────────
    // Skills are inert files until the system message points at the index, so
    // this tab does both jobs in one place: seed the skills folder, then add
    // the pointer sentence. Nothing here writes outside the user's home, and
    // nothing happens until a button is pressed.
    const Theme skillsTheme = makeTheme(cfg.themeMode, cfg.themeAccent);
    const QString mutedStyle = QString("color:%1;").arg(skillsTheme["muted"]);

    auto* extrasTab = new QWidget;
    auto* extrasLayout = new QVBoxLayout(extrasTab);
    extrasLayout->setSpacing(6);

    auto* skillsHeading = new QLabel("Skills");
    QFont skillsFont = skillsHeading->font();
    skillsFont.setBold(true);
    skillsFont.setPointSize(skillsFont.pointSize() + 1);
    skillsHeading->setFont(skillsFont);
    extrasLayout->addWidget(skillsHeading);

    auto* skillsBlurb = new QLabel(
        "Skills are folders of markdown instructions that teach Pengy what its built-in "
        "tools don't cover. Pengy reads the skill index at the start of a task, so the "
        "folder is only useful once the system message mentions it \u2014 installing "
        "fills in both.");
    skillsBlurb->setWordWrap(true);
    skillsBlurb->setStyleSheet(mutedStyle);
    extrasLayout->addWidget(skillsBlurb);

    m_skillsStatus = new QLabel;
    m_skillsStatus->setWordWrap(true);
    extrasLayout->addWidget(m_skillsStatus);

    m_skillsDetail = new QLabel;
    m_skillsDetail->setWordWrap(true);
    m_skillsDetail->setStyleSheet(mutedStyle);
    extrasLayout->addWidget(m_skillsDetail);

    m_skillsPath = new QLineEdit(Skills::displayPath(
        Skills::resolveRoot(cfg.skillsDir, m_systemMsg->toPlainText())));
    m_skillsPath->setToolTip(
        "Where skills live. Defaults to ~/skills. Point it at a synced or shared tree "
        "if you keep skills elsewhere \u2014 only this one folder is consulted.");
    connect(m_skillsPath, &QLineEdit::editingFinished, this, [this]() { refreshSkills(); });

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_skillsPath, 1);
    m_browseSkills = new QPushButton("Browse\u2026");
    m_browseSkills->setToolTip("Choose an existing skills folder");
    connect(m_browseSkills, &QPushButton::clicked, this, [this]() { browseSkillsFolder(); });
    pathRow->addWidget(m_browseSkills);
    extrasLayout->addWidget(labelWithTip("Skills folder:", m_skillsPath->toolTip()));
    extrasLayout->addLayout(pathRow);

    auto* actionsRow = new QHBoxLayout;
    m_installSkills = new QPushButton("Install Skills");
    m_installSkills->setToolTip(
        "Copy the starter skills into the folder above and add the skills pointer to "
        "your system message. Existing files are never overwritten.");
    applyPengyIcon(m_installSkills, "save", skillsTheme, 15);
    connect(m_installSkills, &QPushButton::clicked, this, [this]() { installSkills(); });
    actionsRow->addWidget(m_installSkills);

    m_addPointer = new QPushButton("Add to System Message");
    m_addPointer->setToolTip(
        "Skills already exist here \u2014 append the one-line pointer to the system "
        "message so Pengy actually reads the index.");
    connect(m_addPointer, &QPushButton::clicked, this, [this]() { addSkillsPointer(); });
    actionsRow->addWidget(m_addPointer);

    m_openSkillsFolder = new QPushButton("Open Folder");
    m_openSkillsFolder->setToolTip("Open the skills folder in your file manager");
    applyPengyIcon(m_openSkillsFolder, "file", skillsTheme, 15);
    connect(m_openSkillsFolder, &QPushButton::clicked, this, [this]() { openSkillsFolder(); });
    actionsRow->addWidget(m_openSkillsFolder);
    actionsRow->addStretch();
    extrasLayout->addLayout(actionsRow);

    // Row 2: what to do *next* with the folder that now exists.
    auto* nextRow = new QHBoxLayout;
    m_fillProfile = new QPushButton("Fill In My Profile");
    m_fillProfile->setToolTip(
        "Sends one prompt: read the user_profile skill and interview me. Answers are "
        "written to user_profile/profile.md in the skills folder.");
    connect(m_fillProfile, &QPushButton::clicked, this, [this]() { startProfileInterview(); });
    nextRow->addWidget(m_fillProfile);

    m_browseRegistry = new QPushButton("Browse BotSkills");
    m_browseRegistry->setToolTip(
        "Open skills.catbee.ca \u2014 the public skill registry \u2014 in your browser. The "
        "botskills skill documents how Pengy searches and installs from it.");
    connect(m_browseRegistry, &QPushButton::clicked, this, [this]() { openSkillRegistry(); });
    nextRow->addWidget(m_browseRegistry);
    nextRow->addStretch();
    extrasLayout->addLayout(nextRow);

    m_pointerCheck = new QCheckBox("Add the skills pointer to my system message");
    m_pointerCheck->setChecked(true);
    m_pointerCheck->setToolTip(
        "When checked, installing also appends the pointer sentence shown below to the "
        "system message on the LLM tab. It is never added twice.");
    extrasLayout->addWidget(m_pointerCheck);

    m_skillsPointerPreview = new QLabel(
        Skills::pointer(skillsRoot() + QLatin1Char('/') + Skills::indexName()));
    m_skillsPointerPreview->setWordWrap(true);
    m_skillsPointerPreview->setStyleSheet(mutedStyle + "font-style:italic;");
    extrasLayout->addWidget(m_skillsPointerPreview);

    extrasLayout->addStretch();
    tabs->addTab(extrasTab, "Extras");
    refreshSkills();

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
        // An explicit ~/skills is the default: store "" rather than pinning a
        // value every future default change has to outvote.
        QString skillsDir = m_skillsPath->text().trimmed();
        if (Skills::expandPath(skillsDir) == Skills::defaultRoot()) skillsDir.clear();
        m_config.skillsDir = skillsDir;
        m_config = Config::fromJson(m_compaction->apply(m_config.toJson()));
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

    QPointer<SettingsDialog> self(this);
    QThread* thread = QThread::create([self, baseUrl, apiKey]() {
        QNetworkAccessManager mgr;
        QNetworkRequest req(QUrl(baseUrl + "/models"));
        // chatview.cpp's inline-image fetch already bounds itself this way;
        // without it a hung endpoint leaves loop.exec() blocked forever and
        // the Fetch button stuck disabled at "...".
        req.setTransferTimeout(15000);
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
        req.setRawHeader("api-key",       apiKey.toUtf8());
        req.setRawHeader("User-Agent",    "PengyAgent/1.0");

        QNetworkReply* reply = mgr.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
        deadline.start(15000);
        loop.exec();

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

            QMetaObject::invokeMethod(qApp, [ids, self]() {
                if (!self) return;
                auto* model = self->m_model;
                auto* btn = self->m_fetchBtn;
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
            QString err = QString("Fetch failed (HTTP %1) from %2/models: %3\n\nCheck your Base URL and API Key.")
                          .arg(code).arg(baseUrl).arg(reply->errorString());
            QMetaObject::invokeMethod(qApp, [err, self]() {
                if (!self) return;
                auto* btn = self->m_fetchBtn;
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

// ── Extras tab: starter skills ──────────────────────────────────────

QString SettingsDialog::skillsRoot() const {
    // The path field is authoritative; an emptied field still resolves through
    // the system message, then ~/skills (same order as the Python edition).
    return Skills::resolveRoot(m_skillsPath->text(), m_systemMsg->toPlainText());
}

void SettingsDialog::setLit(QPushButton* button, bool lit) {
    const Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    const QString iconName = button->property("pengyIcon").toString();
    if (!lit) {
        button->setStyleSheet(QString());
        if (!iconName.isEmpty()) applyPengyIcon(button, iconName, theme, 15);
        return;
    }
    button->setStyleSheet(QString(
        "QPushButton { background-color:%1; color:%2; border:1px solid %1; font-weight:600; }"
        "QPushButton:hover { background-color:%3; border-color:%3; }"
        "QPushButton:pressed { background-color:%3; }"
        "QPushButton:disabled { background-color:%4; color:%5; border:1px solid %6;"
        " font-weight:normal; }")
        .arg(theme["primary"], theme["primary_fg"], theme["primary_hover"],
             theme["panel"], theme["muted"], theme["border_soft"]));
    if (!iconName.isEmpty()) {
        // A dark icon on an accent fill disappears; invert it while lit.
        applyPengyIcon(button, iconName, theme, 15, "primary_fg");
    }
}

void SettingsDialog::refreshSkills(const QString& note) {
    const Skills::Status status = Skills::scan(skillsRoot());
    const QString shown = Skills::displayPath(status.root);
    const bool messageHasPointer = Skills::referencesSkills(m_systemMsg->toPlainText());

    m_skillsPointerPreview->setText(
        Skills::pointer(status.root + QLatin1Char('/') + Skills::indexName()));

    if (status.installed()) {
        m_skillsStatus->setText(QString("\u2713 %1 skill(s) at %2")
                                    .arg(status.skills.size()).arg(shown));
    } else if (status.exists) {
        m_skillsStatus->setText(QString("\u26a0 %1 exists but has no %2 \u2014 Pengy has "
                                        "nothing to read").arg(shown, Skills::indexName()));
    } else {
        m_skillsStatus->setText(QString("\u26a0 No skills folder at %1").arg(shown));
    }

    m_installSkills->setEnabled(!status.installed());
    m_installSkills->setText(status.installed() ? "Skills Installed" : "Install Skills");
    setLit(m_installSkills, !status.installed());

    const bool needsPointer = status.installed() && !messageHasPointer;
    m_addPointer->setVisible(status.installed());
    m_addPointer->setEnabled(needsPointer);
    setLit(m_addPointer, needsPointer);
    m_openSkillsFolder->setEnabled(status.exists);

    const bool needsProfile = Skills::profileNeedsInterview(status);
    m_fillProfile->setEnabled(status.skills.contains(Skills::profileSkill()));
    // The tab points at exactly ONE thing to press: install -> interview -> nothing.
    setLit(m_fillProfile, needsProfile);

    QStringList lines;
    if (!status.skills.isEmpty())
        lines << QString("Skills found: %1").arg(status.skills.join(", "));
    else if (status.installed())
        lines << "The index is there, but no skill folders were found.";
    if (needsProfile)
        lines << "Your profile is still a blank template \u2014 press Fill In My Profile "
                 "to answer a few questions.";
    if (status.installed() && messageHasPointer && note.isEmpty())
        lines << "Your system message already points at the skill index.";
    if (!note.isEmpty()) lines << note;
    m_skillsDetail->setText(lines.join("\n"));
}

bool SettingsDialog::applyPointer() {
    const Skills::Status status = Skills::scan(skillsRoot());
    if (!status.installed()) return false;
    const QString current = m_systemMsg->toPlainText();
    const QString updated = Skills::withReference(
        current, status.root + QLatin1Char('/') + Skills::indexName());
    if (updated == current) return false;
    m_systemMsg->setPlainText(updated);
    return true;
}

void SettingsDialog::installSkills() {
    const QString root = skillsRoot();
    const Skills::InstallReport report = Skills::install(root);
    if (report.failed > 0) {
        QMessageBox::warning(this, "Install Failed",
                             QString("Could not copy %1 file(s) into %2.")
                                 .arg(report.failed).arg(Skills::displayPath(root)));
    }
    const Skills::Status status = Skills::scan(root);
    QString note = QString("Installed %1 file(s) into %2.")
                       .arg(report.created).arg(Skills::displayPath(status.root));
    if (report.skipped > 0)
        note += QString(" Left %1 existing file(s) untouched.").arg(report.skipped);
    if (m_pointerCheck->isChecked()) {
        if (applyPointer())
            note += QString::fromUtf8(" System message updated \u2014 press OK to save it.");
        else if (status.installed())
            note += " System message already points at the index.";
    }
    refreshSkills(note);
}

void SettingsDialog::addSkillsPointer() {
    const bool changed = applyPointer();
    refreshSkills(changed
        ? QString::fromUtf8("System message updated \u2014 press OK to save it.")
        : QString("The system message already points at the skill index."));
}

void SettingsDialog::browseSkillsFolder() {
    const QString root = skillsRoot();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, "Choose a skills folder",
        QDir(root).exists() ? root : QDir::homePath());
    if (!chosen.isEmpty()) {
        m_skillsPath->setText(Skills::displayPath(chosen));
        refreshSkills();
    }
}

void SettingsDialog::openSkillsFolder() {
    const QString root = skillsRoot();
    if (QDir(root).exists())
        QDesktopServices::openUrl(QUrl::fromLocalFile(root));
}

void SettingsDialog::openSkillRegistry() {
    QDesktopServices::openUrl(QUrl("https://skills.catbee.ca"));
}

void SettingsDialog::startProfileInterview() {
    // Name the skill file rather than describing the task: the markdown is the
    // contract. Emitting and then accepting lets the main window save these
    // settings and send the prompt once the dialog is gone.
    const Skills::Status status = Skills::scan(skillsRoot());
    emit promptRequested(Skills::profileInterviewPrompt(status));
    accept();
}
