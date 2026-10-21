#include "core/AiRuntime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);app.setApplicationName("MvpRuntimeTest");app.setOrganizationName("Mutant Video Producer");
    // Explicit network integration mode is used by Windows CI and local delivery checks.
    if(app.arguments().value(1)=="--install" || app.arguments().value(1)=="--cancel-repair") {
        const QString id=app.arguments().value(2),device=app.arguments().value(3,"cpu");QString error;
        const bool cancel=app.arguments().value(1)=="--cancel-repair";
        for(const auto& r:parseAiRuntimes(aiRuntimeCatalog(),&error))if(r.id==id) {
            const QString before=aiRuntimeInstalledPath(r);
            if(cancel && before.isEmpty()){qCritical()<<"Install the engine before testing cancelled repair";return 1;}
            QString previous;
            const bool ok=installAiRuntime(r,device,[&](double value,const QString& message){const QString stage=message.section('\n',0,0);if(previous!=stage){qInfo().noquote()<<stage;previous=stage;}return !cancel || value<.98;},&error);
            if(cancel){const bool preserved=!ok && error=="Cancelled" && aiRuntimeInstalledPath(r)==before;qInfo()<<"Cancelled repair preserved active engine:"<<preserved;return preserved?0:1;}
            if(!ok)qCritical().noquote()<<error;
            return ok?0:1;
        }
        qCritical()<<"Unknown component or invalid catalog"<<error;return 1;
    }
    if(parseAiRuntimes(aiRuntimeCatalog()).size()!=2){qCritical()<<"Deployed upstream catalog is invalid";return 1;}
    QTemporaryDir temp;if(!temp.isValid())return 1;
    qputenv("MVP_AI_RUNTIME_DIR",temp.filePath("engines").toUtf8());qputenv("MVP_AI_CATALOG",temp.filePath("catalog.json").toUtf8());
    auto hash=[](const QByteArray& b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());};
    auto write=[](const QString& p,const QByteArray& b){QDir().mkpath(QFileInfo(p).absolutePath());QFile f(p);return f.open(QIODevice::WriteOnly)&&f.write(b)==b.size();};
    const QString digest=hash("locked dependencies");QJsonArray entries;
    for(const QString& id:{QString("speech"),QString("rvc")})entries.append(QJsonObject{{"id",id},{"name",id},{"python","3.12.12"},{"lock","locks/"+id+".txt"},{"lock_sha256",digest},{"cuda_lock","locks/"+id+"-cuda.txt"},{"cuda_lock_sha256",digest}});
    QJsonObject catalog{{"version",2},{"platform","windows-x86_64"},{"uv_sha256",hash("bootstrap")},{"components",entries}};
    int passed=0,failed=0;auto check=[&](bool ok,const QString& message){qInfo().noquote()<<(ok?"PASS":"FAIL")<<message;if(ok)++passed;else ++failed;};
    auto runtimes=parseAiRuntimes(catalog);check(runtimes.size()==2,"valid upstream catalog");
    if(runtimes.size()!=2)return 1;
    auto bad=catalog;bad["version"]=1;check(parseAiRuntimes(bad).empty(),"legacy pack catalog rejected");
    {   // The engines folder chosen in AI Components is used; an unwritable one is refused; Default goes back; the variable wins.
        const QByteArray env=qgetenv("MVP_AI_RUNTIME_DIR");qunsetenv("MVP_AI_RUNTIME_DIR");
        const QString chosen=temp.filePath("chosen");
        const bool set=setAiRuntimeRoot(chosen),used=QDir(aiRuntimeRoot())==QDir(chosen);
        write(temp.filePath("a-file"),"not a folder");QString why;
        const bool refused=!setAiRuntimeRoot(temp.filePath("a-file/inside"),&why) && !why.isEmpty() && QDir(aiRuntimeRoot())==QDir(chosen);
        setAiRuntimeRoot({});const bool back=QDir(aiRuntimeRoot())==QDir(aiRuntimeDefaultRoot());
        qputenv("MVP_AI_RUNTIME_DIR",env);const bool variable=QDir(aiRuntimeRoot())==QDir(QString::fromUtf8(env));
        check(set && used && refused && back && variable,QString("engines folder: the chosen one used (%1 %2), an unwritable one refused (%3), Default (%4) and MVP_AI_RUNTIME_DIR (%5)")
                                                             .arg(set).arg(used).arg(refused).arg(back).arg(variable));
    }
    for(const QString& key:{QString("id"),QString("lock"),QString("cuda_lock"),QString("python"),QString("lock_sha256")}) {
        auto entry=entries[0].toObject();entry[key]="../../escape";auto changed=entries;changed[0]=entry;bad=catalog;bad["components"]=changed;
        check(parseAiRuntimes(bad).empty(),"invalid catalog field rejected: "+key);
    }
    bad=catalog;bad["components"]=QJsonArray{entries[0],entries[0]};check(parseAiRuntimes(bad).empty(),"duplicate components rejected");
    write(temp.filePath("catalog.json"),QJsonDocument(catalog).toJson());const auto r=runtimes.front();
    check(aiRuntimeInstalledPath(r).isEmpty(),"unpublished environment is unavailable");
    const QString generation="speech-11111111-1111-1111-1111-111111111111";
    const QString python=QDir(aiRuntimeRoot()).filePath(generation+"/Scripts/python.exe");write(python,"private python");
    const QString fingerprint=hash((r.python+r.lockSha256+r.cudaSha256).toUtf8());
    QJsonObject marker{{"generation",generation},{"fingerprint",fingerprint},{"python_sha256",hash("private python")}};
    const QString active=QDir(aiRuntimeRoot()).filePath("speech-active.json");write(active,QJsonDocument(marker).toJson());
    check(aiRuntimeInstalledPath(r)==python,"only verified active engine is available");
#ifdef _WIN32
    check(aiRuntimeWorkerArguments("speech",python).size()==2,"Python launch isolates user configuration and selects script");
#endif
    QString error;const QByteArray before=QJsonDocument(marker).toJson();
    check(!installAiRuntime(r,"cpu",{},&error),"missing or tampered setup helper fails before downloads");
    QFile f(active);f.open(QIODevice::ReadOnly);check(f.readAll()==before && aiRuntimeInstalledPath(r)==python,"failed repair preserves the existing active engine");f.close();
    write(python,"modified python");check(aiRuntimeInstalledPath(r).isEmpty(),"modified interpreter rejected");
    marker["generation"]="../escape";write(active,QJsonDocument(marker).toJson());check(aiRuntimeInstalledPath(r).isEmpty(),"active pointer cannot escape managed root");
    qInfo()<<passed<<"passed,"<<failed<<"failed";return failed?1:0;
}
