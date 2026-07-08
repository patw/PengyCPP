#pragma once
#include "config.h"
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QPushButton>
#include <QCheckBox>

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const Config& config, QWidget* parent = nullptr);
    Config config() const { return m_config; }

private slots:
    void fetchModels();

private:
    Config      m_config;
    QLineEdit*  m_baseUrl;
    QLineEdit*  m_apiKey;
    QComboBox*  m_model;
    QPushButton* m_fetchBtn;
    QLineEdit*  m_userAgent;
    QTextEdit*  m_systemMsg;
    QComboBox*  m_toolConfirm;
    QComboBox*  m_reasoningEffort;
    QCheckBox*  m_preserveReasoning;
    QSpinBox*   m_contextKeep;
    QComboBox*  m_uiScale;
    QComboBox*  m_themeMode;
    QComboBox*  m_themeAccent;
    QSpinBox*   m_toolTimeout;
};
