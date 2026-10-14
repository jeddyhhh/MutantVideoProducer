#include "core/AiRuntime.h"
#include "core/Brand.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUuid>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
bool hashValid(const QString& h) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(h).hasMatch();
}
QString hashFile(const QString& path) {
    QFile f(path); QCryptographicHash h(QCryptographicHash::Sha256);
    return f.open(QIODevice::ReadOnly) && h.addData(&f)?QString::fromLatin1(h.result().toHex()):QString();
}
QString runtimeData() {
    const QString path=qEnvironmentVariable("MVP_AI_CATALOG");
    return path.isEmpty()?QCoreApplication::applicationDirPath()+"/runtime":QFileInfo(path).absolutePath();
}
QString markerPath(const QString& id) { return QDir(aiRuntimeRoot()).filePath(id+"-active.json"); }
QString fingerprint(const AiRuntime& r) {
    return QString::fromLatin1(QCryptographicHash::hash((r.python+r.lockSha256+r.cudaSha256).toUtf8(),QCryptographicHash::Sha256).toHex());
}
QJsonObject readMarker(const QString& id) {
    QFile f(markerPath(id));
    return f.open(QIODevice::ReadOnly) && f.size()<4096?QJsonDocument::fromJson(f.readAll()).object():QJsonObject();
}
}
std::vector<AiRuntime> parseAiRuntimes(const QJsonObject& catalog,QString* error) {
    auto fail=[&]()->std::vector<AiRuntime>{if(error)*error="The AI setup catalog is invalid or unsupported.";return {};};
    if(catalog["version"].toInt()!=2 || catalog["platform"]!="windows-x86_64" || !hashValid(catalog["uv_sha256"].toString()))return fail();
    std::vector<AiRuntime> result;
    for(const auto& value:catalog["components"].toArray()) {
        const auto o=value.toObject();
        AiRuntime r{o["id"].toString(),o["name"].toString(),o["python"].toString(),o["lock"].toString(),o["lock_sha256"].toString(),o["cuda_lock"].toString(),o["cuda_lock_sha256"].toString()};
        if((r.id!="speech" && r.id!="rvc") || r.name.isEmpty() || r.python!="3.12.12"
            || r.lock!="locks/"+r.id+".txt" || r.cudaLock!="locks/"+r.id+"-cuda.txt"
            || !hashValid(r.lockSha256) || !hashValid(r.cudaSha256))return fail();
        for(const auto& previous:result)if(previous.id==r.id)return fail();
        result.push_back(r);
    }
    return result.size()==2?result:fail();
}
QJsonObject aiRuntimeCatalog() {
    QFile f(runtimeData()+"/catalog.json");
    return f.open(QIODevice::ReadOnly) && f.size()<1024*1024?QJsonDocument::fromJson(f.readAll()).object():QJsonObject();
}
QString aiRuntimeRoot() {
    const QString path=qEnvironmentVariable("MVP_AI_RUNTIME_DIR");
    return path.isEmpty()?brand::storageLocation(QStandardPaths::AppLocalDataLocation)+"/ai-runtimes":QDir(path).absolutePath();
}
QString aiRuntimeInstalledPath(const AiRuntime& r) {
    if((r.id!="speech" && r.id!="rvc") || !hashValid(r.lockSha256) || !hashValid(r.cudaSha256))return {};
    const auto marker=readMarker(r.id);const QString generation=marker["generation"].toString();
    static const QRegularExpression safe(QStringLiteral("^(speech|rvc)-[0-9a-f-]{36}$"));
    if(!safe.match(generation).hasMatch() || !generation.startsWith(r.id+"-") || marker["fingerprint"]!=fingerprint(r))return {};
    const QString python=QDir(aiRuntimeRoot()).filePath(generation+"/Scripts/python.exe");
    return hashValid(marker["python_sha256"].toString()) && hashFile(python)==marker["python_sha256"].toString()?python:QString();
}
QString aiRuntimeWorkerPath(const QString& id,const QString& bundled) {
#ifdef _WIN32
    for(const auto& r:parseAiRuntimes(aiRuntimeCatalog()))if(r.id==id) {
        const auto path=aiRuntimeInstalledPath(r);if(!path.isEmpty())return path;
    }
#endif
    return bundled;
}
QStringList aiRuntimeWorkerArguments(const QString& id,const QString& program) {
#ifdef _WIN32
    for(const auto& r:parseAiRuntimes(aiRuntimeCatalog()))if(r.id==id && !program.isEmpty() && aiRuntimeInstalledPath(r)==program)
        return {"-I",QCoreApplication::applicationDirPath()+(id=="speech"?"/voice/worker.py":"/voice/rvc/worker.py")};
#endif
    return {};
}
QString aiRuntimeCudaLibraries() {
    for(const auto& r:parseAiRuntimes(aiRuntimeCatalog()))if(r.id=="rvc") {
        const auto p=aiRuntimeInstalledPath(r);
        if(!p.isEmpty())return QFileInfo(p).dir().absoluteFilePath("../Lib/site-packages/torch/lib");
    }
    return {};
}
bool installAiRuntime(const AiRuntime& r,const QString& device,const AiRuntimeProgress& progress,QString* error) {
    auto fail=[&](const QString& message){if(error)*error=message;return false;};
#ifndef _WIN32
    return fail("Optional AI setup currently supports Windows only.");
#else
    if(device!="cpu" && device!="cuda")return fail("Choose cpu or cuda acceleration.");
    const auto catalog=aiRuntimeCatalog();bool valid=false;
    for(const auto& known:parseAiRuntimes(catalog))if(known.id==r.id && fingerprint(known)==fingerprint(r))valid=true;
    const QString uv=runtimeData()+"/uv.exe",lock=runtimeData()+"/"+r.lock,cudaLock=runtimeData()+"/"+r.cudaLock;
    if(!valid || hashFile(uv)!=catalog["uv_sha256"].toString() || hashFile(lock)!=r.lockSha256 || hashFile(cudaLock)!=r.cudaSha256)
        return fail("AI setup files are missing or damaged. Reinstall the editor before downloading engines.");
    const QString root=aiRuntimeRoot();if(!QDir().mkpath(root))return fail("Could not create the AI components folder.");
    QLockFile guard(QDir(root).filePath("setup.lock"));if(!guard.tryLock(0))return fail("AI setup is already running in another window.");
    const QStorageInfo disk(root);
    const qint64 required=r.id=="speech"?2LL*1024*1024*1024:(device=="cuda"?12LL:5LL)*1024*1024*1024;
    if(disk.isValid() && disk.bytesAvailable()>=0 && disk.bytesAvailable()<required)return fail("There is not enough disk space for this AI engine and its download cache.");
    const QString generation=r.id+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString destination=QDir(root).filePath(generation);
    auto env=QProcessEnvironment::systemEnvironment();
    for(const auto& key:env.keys())if(key.startsWith("UV_") || key.startsWith("PIP_") || key.startsWith("PYTHON"))env.remove(key);
    env.insert("UV_PYTHON_INSTALL_DIR",QDir(root).filePath("python"));env.insert("UV_CACHE_DIR",QDir(root).filePath("cache"));
    env.insert("UV_NO_CONFIG","1");env.insert("UV_PYTHON_INSTALL_BIN","0");env.insert("UV_INDEX_URL","https://pypi.org/simple");
    env.insert("UV_NO_PROGRESS","1");env.insert("UV_PYTHON","3.12.12");
    auto run=[&](const QString& program,QStringList args,double value,const QString& label) {
        if(progress && !progress(value,label))return fail("Cancelled");
        QProcess process;process.setProcessEnvironment(env);process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(program,args);if(!process.waitForStarted(5000))return fail("Could not start AI setup: "+process.errorString());
        HANDLE job=CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        HANDLE handle=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,DWORD(process.processId()));
        if(!job || !handle || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof(info)) || !AssignProcessToJobObject(job,handle)) {
            if(job)CloseHandle(job);job=nullptr;
        }
        if(handle)CloseHandle(handle);
        QString output;
        while(process.state()!=QProcess::NotRunning) {
            process.waitForFinished(100);output=(output+QString::fromUtf8(process.readAll())).right(12000);
            if(progress && !progress(value,label+"\n"+output.right(1600))) {
                if(job) {CloseHandle(job);job=nullptr;}process.kill();process.waitForFinished(5000);return fail("Cancelled");
            }
        }
        if(job)CloseHandle(job);output=(output+QString::fromUtf8(process.readAll())).right(12000);
        return process.exitStatus()==QProcess::NormalExit && !process.exitCode()?true:fail(label+" failed.\n"+output);
    };
    const QString python=destination+"/Scripts/python.exe";
    bool ok=run(uv,{"python","install",r.python,"--no-bin"},.05,"Downloading private Python from Astral's upstream distribution")
        && run(uv,{"venv","--managed-python","--python",r.python,destination},.18,"Creating isolated AI environment");
    if(ok)ok=run(uv,{"pip","sync","--python",python,"--require-hashes","--only-binary",":all:",device=="cuda" && r.id=="rvc"?cudaLock:lock},.25,"Downloading verified wheels from PyPI and PyTorch");
    if(ok && device=="cuda" && r.id=="speech") {
        ok=run(uv,{"pip","uninstall","--python",python,"onnxruntime"},.75,"Selecting the CUDA speech backend")
            && run(uv,{"pip","install","--python",python,"--no-deps","--require-hashes","--only-binary",":all:","-r",cudaLock},.8,"Downloading CUDA speech support from PyPI");
    }
    const QString smoke=r.id=="speech"?"import kokoro_onnx,phonemizer,espeakng_loader,onnxruntime; print(onnxruntime.get_available_providers())":
        "import torch,numpy,scipy,librosa,faiss,parselmouth,soundfile,transformers; import sys; sys.path.insert(0,sys.argv[1]); from infer.module.models import SynthesizerTrnMs768NSFsid; from infer.vc.pipeline import Pipeline; import infer.hubert; print(torch.__version__)";
    if(ok)ok=run(python,{"-I","-c",smoke,QCoreApplication::applicationDirPath()+"/voice/rvc/vendor"},.92,"Checking the installed engine");
    if(ok && progress && !progress(.98,"Finishing AI setup"))ok=fail("Cancelled");
    if(ok) {
        const QString hash=hashFile(python);
        QSaveFile f(markerPath(r.id));const QByteArray bytes=QJsonDocument(QJsonObject{{"generation",generation},{"fingerprint",fingerprint(r)},{"device",device},{"python_sha256",hash}}).toJson();
        ok=!hash.isEmpty() && f.open(QIODevice::WriteOnly) && f.write(bytes)==bytes.size() && f.commit();
        if(!ok)fail("Could not activate the installed AI engine.");
    }
    // An immediate child generated above. Windows venv launchers contain absolute paths.
    if(!ok)QDir(destination).removeRecursively();
    if(ok && progress)progress(1,"AI engine installed — ready for offline use");
    return ok;
#endif
}
