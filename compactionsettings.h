#pragma once
// Shared JSON-backed Qt controls. Edits are per model; Save/Cancel never changes
// another model, global fallback, reactive recovery, or checkpoints.
#include <QWidget>
#include <QFormLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QJsonObject>
#include <QSignalBlocker>

class CompactionSettings : public QWidget {
public:
    CompactionSettings(const QJsonObject& config,QComboBox* models,QWidget* parent=nullptr)
        : QWidget(parent), original(config), models(models) {
        auto* form=new QFormLayout(this);form->setContentsMargins(0,0,0,0);
        mode=new QComboBox;mode->setObjectName("compactionMode");
        mode->addItem("Off","off");mode->addItem("Auto-discover","auto");mode->addItem("Explicit window","explicit");
        window=new QSpinBox;window->setObjectName("compactionWindow");window->setRange(1,2147483647);window->setSuffix(" tokens");window->setValue(262144);
        percent=new QSpinBox;percent->setObjectName("compactionPercent");percent->setRange(1,100);percent->setSuffix(" %");percent->setValue(config["compact_at_percent"].toInt(75));
        form->addRow("Proactive compaction:",mode);form->addRow("Selected-model window:",window);form->addRow("Threshold (all models):",percent);
        auto* note=new QLabel("Applies to the selected model. Off keeps reactive recovery and existing checkpoints. Auto needs endpoint metadata; an unknown window stays reactive. Threshold is shared across models.");note->setWordWrap(true);form->addRow(note);
        mode->setToolTip("Off / auto-discover / explicit window for the selected model only.");
        window->setToolTip("Combined input/output window. Typical: 131072, 262144, or 1048576. Only used in Explicit mode.");
        connect(mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{dirty=true;window->setEnabled(!current.isEmpty()&&mode->currentData()=="explicit");});
        connect(window,qOverload<int>(&QSpinBox::valueChanged),this,[this]{dirty=true;});
        connect(models,&QComboBox::currentTextChanged,this,[this](const QString& name){stash();load(name);});
        load(models->currentText());
    }
    QJsonObject apply(QJsonObject config) {
        stash();auto modes=config["context_window_modes"].toObject(),windows=config["context_windows"].toObject();
        for(auto it=drafts.begin();it!=drafts.end();++it){auto draft=it.value().toObject();modes[it.key()]=draft["mode"];if(draft["mode"]=="explicit")windows[it.key()]=draft["window"];}
        if(!drafts.isEmpty()){config["context_window_modes"]=modes;config["context_windows"]=windows;}
        config["compact_at_percent"]=percent->value();return config;
    }
private:
    QJsonObject original,drafts;
    QComboBox* models;QComboBox* mode;QSpinBox* window;QSpinBox* percent;
    QString current;bool dirty=false;
    void stash(){if(dirty&&!current.trimmed().isEmpty())drafts[current]=QJsonObject{{"mode",mode->currentData().toString()},{"window",window->value()}};dirty=false;}
    void load(const QString& name){
        current=name;auto windows=original["context_windows"].toObject();auto modes=original["context_window_modes"].toObject();
        qint64 value=windows.contains(name)?qint64(windows[name].toDouble()):qint64(original["context_window"].toDouble());
        QString selected=modes[name].toString();if(selected!="off"&&selected!="auto"&&selected!="explicit")selected=value>0?"explicit":(windows.contains(name)||!original["context_window_auto"].toBool()?"off":"auto");
        if(drafts.contains(name)){auto d=drafts[name].toObject();selected=d["mode"].toString();value=qint64(d["window"].toDouble());}
        const QSignalBlocker a(mode),b(window);mode->setCurrentIndex(mode->findData(selected));window->setValue(value>0?int(qMin(value,qint64(2147483647))):262144);
        mode->setEnabled(!name.trimmed().isEmpty());window->setEnabled(!name.trimmed().isEmpty()&&selected=="explicit");dirty=false;
    }
};
