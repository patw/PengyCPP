#pragma once
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QListWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QMap>
#include "themehelper.h"

class TasksDialog : public QDialog {
    Q_OBJECT
public:
    explicit TasksDialog(const Theme& theme, QWidget* parent = nullptr);
signals:
    void taskPlayed(const QString& prompt);
private:
    void setupUi();
    void loadTasks();
    QWidget* makeTaskRow(const QJsonObject& task);
    void newTask();
    void editTask(const QJsonObject& task);
    void deleteTask(const QJsonObject& task);
    void playTask(const QJsonObject& task);
    QListWidget* m_list;
    QPushButton* m_newBtn;
    QJsonArray m_tasks;
    Theme m_theme;
};

class TaskEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit TaskEditDialog(const QJsonObject& task = {}, QWidget* parent = nullptr);
    QString title() const;
    QString templ() const;
private:
    void acceptIfValid();
    QLineEdit* m_title;
    QPlainTextEdit* m_template;
};

class PlaceholderDialog : public QDialog {
    Q_OBJECT
public:
    explicit PlaceholderDialog(const QStringList& placeholders, QWidget* parent = nullptr);
    QMap<QString, QString> values() const;
private:
    void acceptIfValid();
    QMap<QString, QLineEdit*> m_inputs;
};
