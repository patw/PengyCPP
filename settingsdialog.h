#pragma once
#include "config.h"
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>

class CompactionSettings;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const Config& config, QWidget* parent = nullptr);
    Config config() const { return m_config; }

signals:
    /// Asks the main window to send a prompt (e.g. start the profile interview).
    /// Emitted from the Extras tab after the dialog has closed itself.
    void promptRequested(const QString& prompt);

private slots:
    void fetchModels();
    void refreshSkills(const QString& note = QString());
    void installSkills();
    void addSkillsPointer();
    void browseSkillsFolder();
    void openSkillsFolder();
    void openSkillRegistry();
    void startProfileInterview();

private:
    /// Accent-fill a button (the "this is the thing to press" state).
    void    setLit(QPushButton* button, bool lit);
    /// Append the pointer to the system message. True when it changed.
    bool    applyPointer();
    /// The folder named in the path field, resolved like the Python edition.
    QString skillsRoot() const;

    CompactionSettings* m_compaction;
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
    QSpinBox*   m_llmTimeout;
    QSpinBox*   m_toolTimeout;
    QSpinBox*   m_toolOutputMax;
    QSpinBox*   m_downloadMax;

    // Extras tab
    QLabel*      m_skillsStatus;
    QLabel*      m_skillsDetail;
    QLabel*      m_skillsPointerPreview;
    QLineEdit*   m_skillsPath;
    QCheckBox*   m_pointerCheck;
    QPushButton* m_installSkills;
    QPushButton* m_addPointer;
    QPushButton* m_openSkillsFolder;
    QPushButton* m_browseSkills;
    QPushButton* m_fillProfile;
    QPushButton* m_browseRegistry;
};
