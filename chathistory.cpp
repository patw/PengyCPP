#include "chathistory.h"
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include "themehelper.h"
#include "iconhelper.h"
#include "chatsearch.h"
#include <QApplication>
#include <QPointer>
#include <QResizeEvent>
#include <QShortcut>
#include <QTextDocument>
#include <QThread>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLineEdit>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

// Result rows keep the hit message indexes and the search words here.
constexpr int kHitsRole = Qt::UserRole + 1;
constexpr int kTermsRole = Qt::UserRole + 2;

// HTML-escape `text`, bolding the [start, end) UTF-16 `spans`.
QString highlightSpans(const QString& text, QList<QPair<int, int>> spans) {
    std::sort(spans.begin(), spans.end());
    QString out;
    int pos = 0;
    for (const auto& [start, end] : spans) {
        if (start < pos || end > text.size()) continue;
        out += text.mid(pos, start - pos).toHtmlEscaped();
        out += "<b>" + text.mid(start, end - start).toHtmlEscaped() + "</b>";
        pos = end;
    }
    return out + text.mid(pos).toHtmlEscaped();
}

}  // namespace

void SearchResultsList::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    for (int i = 0; i < count(); ++i) relayoutRow(item(i));
}

void SearchResultsList::relayoutRow(QListWidgetItem* item) {
    // QLabel's heightForWidth() undercounts wrapped rich text, so measure the
    // same HTML in a QTextDocument at the width the row will really get.
    auto* label = qobject_cast<QLabel*>(itemWidget(item));
    if (!label) return;
    const int width = qMax(50, viewport()->width() - 4);
    const QMargins m = label->contentsMargins();
    QTextDocument doc;
    doc.setDefaultFont(label->font());
    doc.setDocumentMargin(0);
    doc.setHtml(label->text());
    doc.setTextWidth(width - 8 - m.left() - m.right());
    item->setSizeHint(QSize(width, int(std::ceil(doc.size().height())) + m.top() + m.bottom() + 8));
}

ChatHistoryWidget::ChatHistoryWidget(QWidget* parent) : QWidget(parent) {
    m_theme = makeTheme("system", "default");
    setupUi();
}

ChatHistoryWidget::~ChatHistoryWidget() {
    cancelSearch();
}

void ChatHistoryWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    // New Chat, Settings and Tasks share one row (Settings and Tasks as icon
    // buttons), which frees the row the search box sits in.
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(4);
    m_newChatBtn = new QPushButton("New Chat");
    m_newChatBtn->setFixedHeight(36);
    connect(m_newChatBtn, &QPushButton::clicked, this, &ChatHistoryWidget::newChatRequested);
    topRow->addWidget(m_newChatBtn, 1);

    m_settingsBtn = new QPushButton;
    m_settingsBtn->setToolTip("Settings");
    m_settingsBtn->setAccessibleName("Settings");
    connect(m_settingsBtn, &QPushButton::clicked, this, &ChatHistoryWidget::settingsRequested);
    topRow->addWidget(m_settingsBtn);

    m_tasksBtn = new QPushButton;
    m_tasksBtn->setToolTip("Tasks — run a saved prompt template");
    m_tasksBtn->setAccessibleName("Tasks");
    connect(m_tasksBtn, &QPushButton::clicked, this, &ChatHistoryWidget::tasksRequested);
    topRow->addWidget(m_tasksBtn);
    layout->addLayout(topRow);

    // Search: typing filters the list by title; Enter searches messages.
    m_searchInput = new QLineEdit;
    m_searchInput->setPlaceholderText("Search chats (Ctrl+K)");
    m_searchInput->setToolTip("Type to filter by title. Press Enter to search message text too.\n"
                              "Words can be in any order; \"quote\" an exact phrase.");
    m_searchInput->setClearButtonEnabled(true);
    m_searchIconAction = m_searchInput->addAction(QIcon(), QLineEdit::LeadingPosition);
    connect(m_searchInput, &QLineEdit::textChanged, this, &ChatHistoryWidget::onSearchTextChanged);
    connect(m_searchInput, &QLineEdit::returnPressed, this, &ChatHistoryWidget::runSearch);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_searchInput);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, m_searchInput, &QLineEdit::clear);
    layout->addWidget(m_searchInput);

    layout->addSpacing(2);

    // Chat list, and the search results that replace it while a message
    // search is showing.
    m_listStack = new QStackedWidget;
    m_chatList = new QListWidget;
    m_chatList->setStyleSheet("QListWidget::item { padding: 2px; }");
    connect(m_chatList, &QListWidget::itemClicked, this, &ChatHistoryWidget::onItemClicked);
    m_listStack->addWidget(m_chatList);

    auto* resultsPage = new QWidget;
    auto* resultsLayout = new QVBoxLayout(resultsPage);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(4);
    m_searchStatus = new QLabel;
    m_searchStatus->setWordWrap(true);
    resultsLayout->addWidget(m_searchStatus);
    m_resultsList = new SearchResultsList;
    m_resultsList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_resultsList, &QListWidget::itemClicked, this, &ChatHistoryWidget::onResultClicked);
    resultsLayout->addWidget(m_resultsList, 1);
    m_listStack->addWidget(resultsPage);
    layout->addWidget(m_listStack, 1);

    layout->addSpacing(8);

    auto* qsFrame = new QFrame;
    qsFrame->setFrameShape(QFrame::StyledPanel);
    auto* qsLayout = new QVBoxLayout(qsFrame);
    qsLayout->setContentsMargins(8, 8, 8, 8);
    qsLayout->setSpacing(4);

    auto* statusRow = new QHBoxLayout;
    m_statusLabel = new QLabel("Status");
    m_statusLabel->setStyleSheet(QString("font-weight: bold; color: %1;").arg(m_theme["fg"]));
    statusRow->addWidget(m_statusLabel);

    m_statusDot = new QLabel("●");
    m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["success_soft"]));
    statusRow->addWidget(m_statusDot);

    m_statusText = new QLabel("Idle");
    m_statusText->setStyleSheet(QString("color: %1;").arg(m_theme["fg"]));
    statusRow->addWidget(m_statusText);
    statusRow->addStretch();
    qsLayout->addLayout(statusRow);

    auto* qsDivider = new QFrame;
    qsDivider->setFrameShape(QFrame::HLine);
    qsLayout->addWidget(qsDivider);

    auto* modelRow = new QHBoxLayout;
    m_modelLabel = new QLabel("Model:");
    m_modelLabel->setStyleSheet(QString("color: %1;").arg(m_theme["fg"]));
    modelRow->addWidget(m_modelLabel);

    m_modelCombo = new QComboBox;
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->setMinimumWidth(scaledSize(120, m_scale));
    m_modelCombo->setToolTip(
        "Model used for this tab's messages. Populated from the saved "
        "model list — use Settings → Fetch to refresh it.");
    connect(m_modelCombo, qOverload<int>(&QComboBox::activated),
            this, [this](int) { onModelCommit(); });
    connect(m_modelCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &ChatHistoryWidget::onModelCommit);
    modelRow->addWidget(m_modelCombo, 1);
    qsLayout->addLayout(modelRow);

    m_modelHint = new QLabel("");
    m_modelHint->setWordWrap(true);
    m_modelHint->setStyleSheet(QString("color: %1; font-size: 9pt;").arg(m_theme["muted"]));
    m_modelHint->hide();  // shown only when setModels() has no cached list
    qsLayout->addWidget(m_modelHint);

    auto* effortRow = new QHBoxLayout;
    m_effortLabel = new QLabel("Effort:");
    effortRow->addWidget(m_effortLabel);
    m_effortCombo = new QComboBox;
    m_effortCombo->setObjectName("effortCombo");
    m_effortCombo->addItem("Global setting", "global");
    m_effortCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_effortCombo->setMinimumContentsLength(10);
    m_effortCombo->addItem("Provider default", "");
    m_effortCombo->addItem("Off / none", "none");
    m_effortCombo->addItem("Low", "low");
    m_effortCombo->addItem("Medium", "medium");
    m_effortCombo->addItem("High", "high");
    m_effortCombo->addItem("Extra high", "xhigh");
    m_effortCombo->addItem("Max", "max");
    m_effortCombo->setToolTip("Reasoning effort for this tab; saved with the chat. Global setting follows Settings. "
        "Provider default sends no hint. Supported levels depend on the model/provider. Changes apply to the next message.");
    connect(m_effortCombo, qOverload<int>(&QComboBox::activated), this,
        [this](int) { emit effortChanged(m_effortCombo->currentData().toString()); });
    effortRow->addWidget(m_effortCombo, 1);
    qsLayout->addLayout(effortRow);

    m_tokensLabel = new QLabel("Tokens: —");
    m_tokensLabel->setStyleSheet(QString("color: %1;").arg(m_theme["fg"]));
    qsLayout->addWidget(m_tokensLabel);
    m_rateLabel = new QLabel("Last: — tok/s");
    m_rateLabel->setObjectName("responseRateLabel");
    m_rateLabel->setToolTip("Last final API response only: API-reported output tokens divided by successful request "
        "wall time. Includes network latency, prompt processing and reasoning; excludes tool execution, "
        "confirmation waits and retry backoff. Not a pure decoding speed. — means no measurement available.");
    qsLayout->addWidget(m_rateLabel);

    layout->addWidget(qsFrame);

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, &ChatHistoryWidget::blinkDot);
    applyTheme(m_theme, m_scale);
}

void ChatHistoryWidget::applyTheme(const Theme& theme, int scale) {
    m_theme = theme;
    m_scale = scale;
    setStyleSheet(QString("ChatHistoryWidget { background-color:%1; color:%2; } QFrame { border-color:%3; }")
                  .arg(theme["panel"], theme["fg"], theme["border"]));
    const QString listStyle = QString(R"(
QListWidget { background-color:%1; color:%2; border:1px solid %3; border-radius:6px; outline:none; }
QListWidget::item { color:%2; padding:2px; border-radius:6px; }
QListWidget::item:selected { background-color:%4; color:%2; }
QListWidget::item:hover { background-color:%5; }
)" ).arg(theme["panel"], theme["fg"], theme["border_soft"], theme["selection"], theme["hover"]);
    m_chatList->setStyleSheet(listStyle);
    m_resultsList->setStyleSheet(listStyle);
    int controlH = scaledSize(36, scale);
    m_newChatBtn->setFixedHeight(controlH);
    for (QPushButton* button : {m_settingsBtn, m_tasksBtn}) button->setFixedSize(controlH, controlH);
    m_searchInput->setFixedHeight(scaledSize(30, scale));
    m_searchIconAction->setIcon(QIcon(renderPengyIcon("search", theme["muted"], scaledSize(16, scale))));
    m_searchStatus->setStyleSheet(QString("color:%1;").arg(theme["muted"]));
    applyPengyIcon(m_newChatBtn, "new-chat", theme, scaledSize(16, scale));
    applyPengyIcon(m_settingsBtn, "settings", theme, scaledSize(16, scale));
    applyPengyIcon(m_tasksBtn, "tasks", theme, scaledSize(16, scale));
    if (m_statusLabel) m_statusLabel->setStyleSheet(QString("font-weight:bold; color:%1;").arg(theme["fg"]));
    for (QLabel* label : {m_statusText, m_modelLabel, m_effortLabel, m_tokensLabel, m_rateLabel}) {
        if (label) label->setStyleSheet(QString("color:%1;").arg(theme["fg"]));
    }
    if (m_modelHint) m_modelHint->setStyleSheet(QString("color:%1; font-size: 9pt;").arg(theme["muted"]));
}

QWidget* ChatHistoryWidget::makeItemWidget(const QString& id, const QString& title) {
    auto* w = new QWidget;
    w->setStyleSheet(QString("background-color:%1; color:%2;").arg(m_theme["panel"], m_theme["fg"]));
    auto* layout = new QHBoxLayout(w);
    layout->setContentsMargins(4, 2, 2, 2);
    layout->setSpacing(2);

    auto* label = new QLabel(title);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(label, 1);

    QString btnStyle = QString(
        "QPushButton { background-color:transparent; color:%1; border:none; border-radius:4px; font-size:13px; padding:0px; }"
        "QPushButton:hover { background-color:%2; }").arg(m_theme["fg"], m_theme["hover"]);

    auto* editBtn = new QPushButton;
    editBtn->setFixedSize(scaledSize(24, m_scale), scaledSize(24, m_scale));
    editBtn->setToolTip("Edit chat (rename, generate title, export)");
    editBtn->setAccessibleName("Edit chat");
    applyPengyIcon(editBtn, "edit", m_theme, scaledSize(14, m_scale));
    editBtn->setStyleSheet(btnStyle);
    connect(editBtn, &QPushButton::clicked, this, [this, id]() { emit editRequested(id); });
    layout->addWidget(editBtn);

    auto* delBtn = new QPushButton;
    delBtn->setFixedSize(scaledSize(24, m_scale), scaledSize(24, m_scale));
    delBtn->setToolTip("Delete chat");
    delBtn->setAccessibleName("Delete chat");
    applyPengyIcon(delBtn, "delete", m_theme, scaledSize(14, m_scale), "fg", "danger");
    delBtn->setStyleSheet(btnStyle);
    connect(delBtn, &QPushButton::clicked, this, [this, id]() { emit deleteRequested(id); });
    layout->addWidget(delBtn);

    return w;
}

QString ChatHistoryWidget::itemTitle(QListWidgetItem* item) const {
    QWidget* w = m_chatList->itemWidget(item);
    QLabel* label = w ? w->findChild<QLabel*>() : nullptr;
    return label ? label->text() : QString();
}

void ChatHistoryWidget::loadChats(const QJsonArray& chats) {
    m_chatList->clear();
    for (const QJsonValue& v : chats) {
        QJsonObject chat = v.toObject();
        QString id    = chat["id"].toString();
        QString title = chat["title"].toString();
        auto* item   = new QListWidgetItem;
        item->setData(Qt::UserRole, id);
        auto* widget = makeItemWidget(id, title);
        item->setSizeHint(QSize(0, qMax(widget->sizeHint().height(), 32)));
        m_chatList->addItem(item);
        m_chatList->setItemWidget(item, widget);
    }
    if (!m_searchInput->text().isEmpty()) applyTitleFilter();
}

void ChatHistoryWidget::addChat(const QString& id, const QString& title) {
    // Insert one new row at the top, without touching the other rows. A
    // freshly created chat is always the newest (chats sort newest-first),
    // so it always belongs at index 0 -- no need to pay loadChats()'s full
    // clear-and-rebuild, which costs one QWidget (with icon-bearing buttons)
    // per *existing* row and was the dominant cost behind "New Chat feels
    // slow" once the sidebar has more than a handful of chats.
    auto* item = new QListWidgetItem;
    item->setData(Qt::UserRole, id);
    auto* widget = makeItemWidget(id, title);
    item->setSizeHint(QSize(0, qMax(widget->sizeHint().height(), 32)));
    m_chatList->insertItem(0, item);
    m_chatList->setItemWidget(item, widget);
}

void ChatHistoryWidget::removeChat(const QString& id) {
    // For callers that already deleted the chat from disk themselves (e.g.
    // MainWindow discarding an empty "New Chat" tab) and just need the
    // sidebar to stop showing a row for it. A no-op if the chat was never
    // shown here, or has already been removed.
    for (int i = 0; i < m_chatList->count(); i++) {
        if (m_chatList->item(i)->data(Qt::UserRole).toString() == id) {
            delete m_chatList->takeItem(i);
            return;
        }
    }
}

void ChatHistoryWidget::selectChatById(const QString& id) {
    for (int i = 0; i < m_chatList->count(); i++) {
        if (m_chatList->item(i)->data(Qt::UserRole).toString() == id) {
            m_chatList->setCurrentRow(i);
            return;
        }
    }
}

void ChatHistoryWidget::updateChatTitle(const QString& id, const QString& title) {
    for (int i = 0; i < m_chatList->count(); i++) {
        auto* item = m_chatList->item(i);
        if (item->data(Qt::UserRole).toString() == id) {
            auto* w = m_chatList->itemWidget(item);
            if (w) {
                auto* label = w->findChild<QLabel*>();
                if (label) label->setText(title);
            }
            return;
        }
    }
}

void ChatHistoryWidget::onItemClicked(QListWidgetItem* item) {
    emit chatSelected(item->data(Qt::UserRole).toString());
}

// ── Search ──────────────────────────────────────────────────────────────

void ChatHistoryWidget::focusSearch() {
    m_searchInput->setFocus(Qt::ShortcutFocusReason);
    m_searchInput->selectAll();
}

void ChatHistoryWidget::onSearchTextChanged() {
    // Any edit makes shown message results stale: back to the (title-
    // filtered) chat list until Enter is pressed again.
    cancelSearch();
    m_listStack->setCurrentIndex(0);
    applyTitleFilter();
}

void ChatHistoryWidget::applyTitleFilter() {
    // Hide rows whose title lacks any search word (instant, no file reads).
    const QStringList terms = ChatSearch::parseQuery(m_searchInput->text());
    for (int i = 0; i < m_chatList->count(); ++i) {
        const QString title = itemTitle(m_chatList->item(i)).toLower();
        bool all = true;
        for (const QString& t : terms) all = all && title.contains(t);
        m_chatList->setRowHidden(i, !all);
    }
}

void ChatHistoryWidget::cancelSearch() {
    ++m_searchGeneration;
    m_searchCancel->store(true);
}

void ChatHistoryWidget::runSearch() {
    const QString query = m_searchInput->text();
    if (ChatSearch::parseQuery(query).isEmpty()) return;
    cancelSearch();
    m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = m_searchCancel;
    const int generation = m_searchGeneration;
    m_resultsList->clear();
    m_searchStatus->setText("Searching…");
    m_listStack->setCurrentIndex(1);

    // The thread checks `cancel` between chats and never touches us once
    // we're gone; events reach us on the UI thread.
    QPointer<ChatHistoryWidget> guard(this);
    QThread* thread = QThread::create([guard, query, cancel, generation]() {
        ChatSearch::run(query, ChatSearch::kMaxResults, [&](const QJsonObject& event) {
            if (cancel->load()) return false;
            QMetaObject::invokeMethod(qApp, [guard, generation, event]() {
                if (guard) guard->onSearchEvent(generation, event);
            });
            return true;
        });
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ChatHistoryWidget::onSearchEvent(int generation, const QJsonObject& event) {
    if (generation != m_searchGeneration) return;
    const QString kind = event["type"].toString();
    auto found = [](int n) { return QString("%1 chat%2 found").arg(n).arg(n == 1 ? "" : "s"); };
    if (kind == "result") {
        addResultRow(event);
        m_searchStatus->setText(found(m_resultsList->count()) + ", searching…");
    } else if (kind == "progress") {
        m_searchStatus->setText(QString("%1, searching… %2/%3").arg(found(m_resultsList->count()))
                                .arg(event["scanned"].toInt()).arg(event["total"].toInt()));
    } else if (kind == "done") {
        const int n = event["results"].toInt();
        QString text = n ? found(n) : QString("No chats match.");
        if (event["truncated"].toBool())
            text = QString("First %1 chats shown. Add words to narrow the search.").arg(n);
        m_searchStatus->setText(text + "  Esc to go back.");
    }
}

void ChatHistoryWidget::addResultRow(const QJsonObject& result) {
    const QStringList terms = ChatSearch::parseQuery(m_searchInput->text());
    QString title = result["title"].toString();
    if (title.isEmpty()) title = "Untitled";
    QList<QPair<int, int>> titleSpans;
    const QString low = title.toLower();
    if (low.size() == title.size()) {
        for (const QString& t : terms) {
            const int i = low.indexOf(t);
            if (i >= 0) titleSpans.append({i, i + int(t.size())});
        }
    }
    const QString muted = m_theme["muted"];
    QString html = "<b>" + highlightSpans(title, titleSpans) + "</b>";
    const QString when = result["created_at"].toString().left(10);
    if (!when.isEmpty())
        html += QString(" <span style='color:%1;'>%2</span>").arg(muted, when.toHtmlEscaped());
    QVariantList sources;
    for (const QJsonValue& v : result["hits"].toArray()) {
        const QJsonObject hit = v.toObject();
        const QString snippet = hit["snippet"].toString();
        QList<QPair<int, int>> spans;  // code points -> UTF-16
        for (const QJsonValue& s : hit["spans"].toArray()) {
            const QJsonArray span = s.toArray();
            spans.append({ChatSearch::codePointToUtf16(snippet, span[0].toInt()),
                          ChatSearch::codePointToUtf16(snippet, span[1].toInt())});
        }
        html += QString("<br><span style='color:%1;'>%2:</span> %3")
                    .arg(muted, hit["role"].toString() == "user" ? "You" : "AI", highlightSpans(snippet, spans));
        sources << hit["index"].toInt();
    }
    auto* label = new QLabel(html);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setContentsMargins(4, 4, 4, 4);
    label->setStyleSheet(QString("color:%1; background:transparent;").arg(m_theme["fg"]));
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* item = new QListWidgetItem;
    item->setData(Qt::UserRole, result["id"].toString());
    item->setData(kHitsRole, sources);
    item->setData(kTermsRole, terms);
    m_resultsList->addItem(item);
    m_resultsList->setItemWidget(item, label);
    m_resultsList->relayoutRow(item);
}

void ChatHistoryWidget::onResultClicked(QListWidgetItem* item) {
    if (!item) return;
    QList<int> sources;
    for (const QVariant& v : item->data(kHitsRole).toList()) sources << v.toInt();
    emit searchHitSelected(item->data(Qt::UserRole).toString(), sources, item->data(kTermsRole).toStringList());
}

void ChatHistoryWidget::setThinking(bool thinking) {
    if (thinking) {
        m_dotPhase = true;
        m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["danger"]));
        m_statusText->setText("Thinking…");
        m_blinkTimer->start();
    } else {
        m_blinkTimer->stop();
        m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["success_soft"]));
        m_statusText->setText("Idle");
    }
}

void ChatHistoryWidget::setToolRunning(bool running) {
    if (running) {
        m_blinkTimer->stop();
        m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["running"]));
        m_statusText->setText("Running Tool…");
        // Force an immediate repaint so the orange dot is visible before
        // the caller unblocks the worker thread — otherwise the dot can
        // flip straight from red (thinking) → red again without painting.
        m_statusDot->repaint();
    } else {
        m_dotPhase = true;
        m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["danger"]));
        m_statusText->setText("Thinking…");
        m_blinkTimer->start();
    }
}

void ChatHistoryWidget::setRetrying(const QString& text) {
    // Transient status while the LLM backs off and retries (429/529). Not
    // blinking and not "Running Tool…": this is a network backoff pause.
    m_blinkTimer->stop();
    m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(m_theme["muted"]));
    m_statusText->setText(text);
}

void ChatHistoryWidget::blinkDot() {
    m_dotPhase = !m_dotPhase;
    QString color = m_dotPhase ? m_theme["danger"] : "transparent";
    m_statusDot->setStyleSheet(QString("color: %1; font-size: 14px;").arg(color));
}

void ChatHistoryWidget::setModels(const QStringList& models, const QString& current) {
    QStringList items = models;
    if (!current.isEmpty() && !items.contains(current))
        items.prepend(current);
    m_modelCombo->clear();
    m_modelCombo->addItems(items);
    if (!current.isEmpty())
        m_modelCombo->setCurrentText(current);
    m_currentModel = current.isEmpty() ? m_modelCombo->currentText() : current;

    // An empty QLabel still claims a line of layout height, so hide it
    // outright once populated instead of just clearing its text -- otherwise
    // it leaves a permanent gap above the effort selector.
    if (models.isEmpty()) {
        m_modelHint->setText("No cached model list — use Settings → Fetch to populate.");
        m_modelHint->show();
    } else {
        m_modelHint->setText("");
        m_modelHint->hide();
    }
}

void ChatHistoryWidget::onModelCommit() {
    QString model = m_modelCombo->currentText().trimmed();
    if (model.isEmpty() || model == m_currentModel)
        return;
    m_currentModel = model;
    emit modelChanged(model);
}

void ChatHistoryWidget::updateQuickSettings(const QString& model, const QString& effort) {
    if (!model.isEmpty()) {
        m_modelCombo->setCurrentText(model);
        m_currentModel = model;
    }
    const QSignalBlocker blocker(m_effortCombo);
    int index = m_effortCombo->findData(effort);
    m_effortCombo->setCurrentIndex(index < 0 ? 0 : index);
}

void ChatHistoryWidget::updateResponseRate(double tokensPerSecond) {
    m_rateLabel->setText(tokensPerSecond >= 0 && std::isfinite(tokensPerSecond)
        ? QString("Last: %1 tok/s").arg(tokensPerSecond, 0, 'f', 1)
        : QString("Last: — tok/s"));
}

void ChatHistoryWidget::updateTokenUsage(int prompt, int completion) {
    if (prompt || completion)
        m_tokensLabel->setText(QString("Tokens: %1 in / %2 out").arg(prompt).arg(completion));
    else
        m_tokensLabel->setText("Tokens: —");
}
