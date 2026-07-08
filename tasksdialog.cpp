#include "tasksdialog.h"
#include "taskmanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QJsonDocument>

TasksDialog::TasksDialog(const Theme& theme, QWidget* parent) : QDialog(parent), m_theme(theme) { setWindowTitle("Tasks"); resize(640,520); setupUi(); loadTasks(); }
void TasksDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel("Tasks"); title->setStyleSheet("font-size:16pt;font-weight:bold;"); header->addWidget(title); header->addStretch();
    m_newBtn = new QPushButton("+ New Template"); connect(m_newBtn,&QPushButton::clicked,this,&TasksDialog::newTask); header->addWidget(m_newBtn); layout->addLayout(header);
    auto* hint = new QLabel("Use %placeholder% in templates to prompt for dynamic values."); hint->setWordWrap(true); layout->addWidget(hint);
    m_list = new QListWidget; m_list->setSelectionMode(QAbstractItemView::NoSelection); layout->addWidget(m_list,1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(buttons);
    /* Theme the dialog and New button (matches Python reference) */
    setStyleSheet(QString("QDialog{background-color:%1;color:%2;}QLabel{color:%2;}QListWidget{background-color:%3;color:%2;border:1px solid %4;border-radius:6px;}")
        .arg(m_theme["bg"], m_theme["fg"], m_theme["panel"], m_theme["border_soft"]));
    m_newBtn->setStyleSheet(QString("QPushButton{background-color:%1;color:%2;border:none;border-radius:8px;padding:7px 14px;font-weight:bold;}QPushButton:hover{background-color:%3;}")
        .arg(m_theme["primary"], m_theme["primary_fg"], m_theme["primary_hover"]));
}
void TasksDialog::loadTasks() {
    m_tasks = tasksLoad(); m_list->clear();
    if (m_tasks.isEmpty()) { auto* item=new QListWidgetItem; auto* label=new QLabel("No task templates yet. Click + New Template to create one."); label->setAlignment(Qt::AlignCenter); label->setStyleSheet("padding:28px;color:#667085;"); item->setSizeHint(label->sizeHint()); m_list->addItem(item); m_list->setItemWidget(item,label); return; }
    for (const auto& v : m_tasks) { QJsonObject task=v.toObject(); auto* item=new QListWidgetItem; item->setData(Qt::UserRole, task["id"].toString()); QWidget* row=makeTaskRow(task); item->setSizeHint(row->sizeHint()); m_list->addItem(item); m_list->setItemWidget(item,row); }
}
QWidget* TasksDialog::makeTaskRow(const QJsonObject& task) {
    auto* row = new QWidget; auto* layout = new QHBoxLayout(row); layout->setContentsMargins(8,6,6,6); layout->setSpacing(6);
    row->setObjectName("taskRow");
    row->setStyleSheet(QString("#taskRow{background-color:%1;color:%2;}").arg(m_theme["panel"], m_theme["fg"]));
    auto* col = new QWidget; auto* vl = new QVBoxLayout(col); vl->setContentsMargins(0,0,0,0); vl->setSpacing(2);
    auto* title = new QLabel(task["title"].toString("Untitled Task")); title->setStyleSheet(QString("font-weight:bold;color:%1;").arg(m_theme["fg"])); vl->addWidget(title);
    QString preview = task["template"].toString().replace('\n',' '); if (preview.size()>70) preview = preview.left(70) + "…"; auto* prev = new QLabel(preview); prev->setStyleSheet(QString("font-size:11px;color:%1;").arg(m_theme["muted"])); vl->addWidget(prev); layout->addWidget(col,1);
    QString btnStyle = QString("QPushButton{background-color:transparent;color:%1;border:none;border-radius:4px;font-size:15px;}QPushButton:hover{background-color:%2;}").arg(m_theme["fg"], m_theme["hover"]);
    auto addBtn=[&](const QString& txt,const QString& tip, auto fn){ auto* b=new QPushButton(txt); b->setFixedSize(32,32); b->setToolTip(tip); b->setStyleSheet(btnStyle); connect(b,&QPushButton::clicked,this,[=](){ fn(task); }); layout->addWidget(b); };
    addBtn("▶","Play task",[this](const QJsonObject&t){playTask(t);}); addBtn("✏","Edit task",[this](const QJsonObject&t){editTask(t);}); addBtn("🗑","Delete task",[this](const QJsonObject&t){deleteTask(t);}); return row;
}
void TasksDialog::newTask() { TaskEditDialog d({}, this); if (d.exec()==QDialog::Accepted) { taskCreate(d.title(), d.templ()); loadTasks(); } }
void TasksDialog::editTask(const QJsonObject& task) { TaskEditDialog d(task, this); if (d.exec()==QDialog::Accepted) { taskUpdate(task["id"].toString(), d.title(), d.templ()); loadTasks(); } }
void TasksDialog::deleteTask(const QJsonObject& task) { if (QMessageBox::question(this,"Delete Task",QString("Delete task '%1'?").arg(task["title"].toString("Untitled Task")), QMessageBox::Yes|QMessageBox::Cancel, QMessageBox::Cancel)==QMessageBox::Yes) { taskDelete(task["id"].toString()); loadTasks(); } }
void TasksDialog::playTask(const QJsonObject& task) { QString templ=task["template"].toString(); QStringList ph=extractPlaceholders(templ); QMap<QString,QString> vals; if (!ph.isEmpty()) { PlaceholderDialog d(ph,this); if (d.exec()!=QDialog::Accepted) return; vals=d.values(); } QString prompt=renderTaskTemplate(templ, vals).trimmed(); if (prompt.isEmpty()) { QMessageBox::warning(this,"Empty Task","This task produced an empty prompt."); return; } emit taskPlayed(prompt); accept(); }

TaskEditDialog::TaskEditDialog(const QJsonObject& task, QWidget* parent) : QDialog(parent) { setWindowTitle(task.isEmpty()?"New Task":"Edit Task"); resize(560,380); auto* layout=new QVBoxLayout(this); auto* form=new QFormLayout; m_title=new QLineEdit(task["title"].toString()); m_title->setPlaceholderText("e.g. Summarize YouTube Video"); form->addRow("Title",m_title); layout->addLayout(form); layout->addWidget(new QLabel("Prompt template")); m_template=new QPlainTextEdit(task["template"].toString()); m_template->setPlaceholderText("Summarize this youtube video: %Youtube Video URL% always use the youtube transcription skill!"); layout->addWidget(m_template,1); auto* hint=new QLabel("Placeholders use %name% and each unique name is requested once when played."); hint->setWordWrap(true); layout->addWidget(hint); auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel); connect(buttons,&QDialogButtonBox::accepted,this,&TaskEditDialog::acceptIfValid); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(buttons); }
QString TaskEditDialog::title() const { return m_title->text().trimmed(); }
QString TaskEditDialog::templ() const { return m_template->toPlainText(); }
void TaskEditDialog::acceptIfValid() { if (title().isEmpty()) { QMessageBox::warning(this,"Missing Title","Please enter a task title."); return; } if (templ().trimmed().isEmpty()) { QMessageBox::warning(this,"Missing Template","Please enter a prompt template."); return; } accept(); }

PlaceholderDialog::PlaceholderDialog(const QStringList& placeholders, QWidget* parent) : QDialog(parent) { setWindowTitle("Task Inputs"); resize(460, qMax(160, qMin(520, 90 + placeholders.size()*42))); auto* layout=new QVBoxLayout(this); auto* form=new QFormLayout; for (const QString& name: placeholders) { auto* e=new QLineEdit; e->setPlaceholderText(name); m_inputs[name]=e; form->addRow(name,e); } layout->addLayout(form); auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); connect(buttons,&QDialogButtonBox::accepted,this,&PlaceholderDialog::acceptIfValid); connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject); layout->addWidget(buttons); }
QMap<QString, QString> PlaceholderDialog::values() const { QMap<QString,QString> out; for (auto it=m_inputs.begin(); it!=m_inputs.end(); ++it) out[it.key()] = it.value()->text().trimmed(); return out; }
void PlaceholderDialog::acceptIfValid() { QStringList missing; for (auto it=m_inputs.begin(); it!=m_inputs.end(); ++it) if (it.value()->text().trimmed().isEmpty()) missing << it.key(); if (!missing.isEmpty()) { QMessageBox::warning(this,"Missing Input","Please fill in: " + missing.join(", ")); return; } accept(); }
