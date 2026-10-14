// Structured adversarial cases run in child processes: crashes/timeouts do not hide later failures.
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/Compositor.h"
#include "core/AudioMixer.h"
#include "core/MotionGraphics.h"
#include "core/FFmpeg.h"
#include <QGuiApplication>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTextStream>
#include <random>
#include <stdexcept>
#include <climits>
#include <cmath>

namespace {
int assertions=0;
void require(bool value,const QString& message) {
    ++assertions;
    if(!value) throw std::runtime_error(message.toStdString());
}
void write(const QString& path,const QByteArray& data) {
    QFile file(path); require(file.open(QIODevice::WriteOnly),"write fixture");
    require(file.write(data)==data.size(),"complete fixture write");
}
QJsonObject document() {
    auto tl=defaultTimeline(); ops::insertGenerated(tl,*findGeneratorPreset("solid_white"),0,-1,{});
    auto root=nestedTimelineToJson(tl);root["format"]="mutant-video-producer-project";root["version"]=1;
    root["settings"]=QJsonObject{{"width",160},{"height",90},{"fpsNum",30},{"fpsDen",1},{"sampleRate",48000}};
    return root;
}
void rejectUnchanged(Project& project,const QString& path,const QJsonObject& root) {
    write(path,QJsonDocument(root).toJson());
    auto before=project.snapshot(); const auto file=project.filePath(); const bool dirty=project.isModified();
    const int undo=project.undoStack()->count(),index=project.undoStack()->index();QString error;
    require(!project.load(path,&error),"malformed file accepted: "+path);
    require(!error.isEmpty(),"failure has no diagnostic");
    require(project.snapshot()==before && project.filePath()==file && project.isModified()==dirty &&
            project.undoStack()->count()==undo && project.undoStack()->index()==index,"failed load changed live project or undo history");
}
void run(const QString& name,const QDir& out,uint32_t seed,int rounds) {
    std::mt19937 rng(seed); auto root=document(); const QString path=out.filePath(name+".mvp");
    Project project;auto tl=defaultTimeline();ops::insertGenerated(tl,*findGeneratorPreset("solid_blue"),0,-1,{});project.applyEdit(tl,"unsaved baseline");
    if(name=="versions") {
        auto absent=root;absent.remove("version");rejectUnchanged(project,path,absent);
        auto obsolete=root;obsolete["format"]="montage-project";rejectUnchanged(project,path,obsolete);
        const QString obsoletePath=out.filePath("obsolete.mtg");
        rejectUnchanged(project,obsoletePath,root);
        QFile originalFile(obsoletePath);require(originalFile.open(QIODevice::ReadOnly),"read obsolete fixture");const QByteArray originalBytes=originalFile.readAll();originalFile.close();
        QString error;
        require(!project.save(obsoletePath,&error),"obsolete extension saved");
        require(originalFile.open(QIODevice::ReadOnly) && originalFile.readAll()==originalBytes,"refused save overwrote existing file");originalFile.close();
        require(!project.saveBackup(out.filePath("obsolete-backup.mtg"),&error) && !QFileInfo::exists(out.filePath("obsolete-backup.mtg")),"obsolete extension autosaved");
        for(auto version:QJsonArray{999,-1,0,"1",true,QJsonValue::Null}) {auto bad=root;bad["version"]=version;rejectUnchanged(project,path,bad);}
        root["version"]=1;write(path,QJsonDocument(root).toJson());require(project.load(path,nullptr),"supported project version loads");
    } else if(name=="media-identity") {
        for(int id:{0,-1,INT_MIN,INT_MAX,INT_MAX-1}) {
            auto bad=root;bad["media"]=QJsonArray{QJsonObject{{"id",id},{"path",out.filePath("missing-a.wav")}}};rejectUnchanged(project,path,bad);
        }
        auto bad=root;bad["media"]=QJsonArray{QJsonObject{{"id",1},{"path",out.filePath("missing-a.wav")}},QJsonObject{{"id",1},{"path",out.filePath("missing-b.wav")}}};rejectUnchanged(project,path,bad);
        bad["media"]=QJsonArray{QJsonObject{{"id",1},{"path",out.filePath("missing-a.wav")}},QJsonObject{{"id",2},{"path",out.filePath("missing-b.wav")}}};
        write(path,QJsonDocument(bad).toJson());require(project.load(path,nullptr),"distinct offline media IDs must remain loadable");
        MediaInfo info;info.path=out.filePath("new-import.wav");const int imported=project.addMedia(info);
        require(imported>2&&project.media().size()==3,"new import reused loaded media identity");
        require(project.addMedia(info)==imported&&project.media().size()==3,"same offline path was imported twice");
    } else if(name=="keyframes") {
        for(int i=0;i<rounds;++i) {
            const auto time=QString::number(1000+i);
            QJsonArray points{QJsonArray{},"wrong",QJsonArray{time},QJsonArray{time,.25},QJsonArray{time,.75,0},QJsonArray{"9223372036854775807",1,0},QJsonArray{"-9223372036854775808",1,0}};
            auto params=paramsFromJson(QJsonObject{{"gain",QJsonObject{{"keys",points}}}});
            auto& keys=params.at("gain").keys;
            require(keys.size()==1 && keys[0].time==1000+i && keys[0].value==.75,"malformed/duplicate/extreme parameter keys survived");
            auto bad=root;auto tracks=bad["tracks"].toArray();auto track=tracks[0].toObject();
            track["compositeEnvelope"]=QJsonObject{{"active",true},{"value",1},{"points",points}};tracks[0]=track;bad["tracks"]=tracks;
            write(path,QJsonDocument(bad).toJson());require(project.load(path,nullptr),"repairable envelope refused");
            auto& repaired=project.timeline().tracks[0].composite.points;
            require(repaired.size()==1 && repaired[0].time==1000+i && repaired[0].value==.75,"envelope key repair is inconsistent");
        }
    } else if(name=="nesting") {
        for(int depth=0;depth<=24;++depth) {
            QJsonObject nested=document();nested.remove("format");nested.remove("version");
            for(int d=0;d<depth;++d) {auto parent=document();auto tracks=parent["tracks"].toArray();auto t=tracks[0].toObject();auto clips=t["clips"].toArray();auto c=clips[0].toObject();c.remove("generator");c["sequence"]=nested;clips[0]=c;t["clips"]=clips;tracks[0]=t;parent["tracks"]=tracks;nested=parent;}
            nested["format"]="mutant-video-producer-project";nested["version"]=1;
            if(depth>16) rejectUnchanged(project,path,nested);
            else {write(path,QJsonDocument(nested).toJson());QString error;require(project.load(path,&error),"bounded sequence refused: "+error);require(timelineProblem(project.timeline()).isEmpty(),"loader and sequence-depth validator disagree at depth "+QString::number(depth));
                Compositor renderer; require(!renderer.render(*project.snapshot(),0,{32,18}).isNull(),"nested render failed");}
        }
    } else if(name=="references") {
        Timeline source=defaultTimeline();source.tracks[1].id=INT_MAX-1;
        Track music;music.id=100;music.type=TrackType::Audio;music.duckUnder=INT_MAX-1;source.tracks.push_back(music);
        int caption=ops::insertGenerated(source,*findGeneratorPreset("solid_white"),0,0,{});source.findClip(caption)->id=INT_MAX-2;
        Clip narration;narration.id=101;narration.length=kUsPerSecond;narration.narration=QJsonObject{{"caption_owner",INT_MAX-2}};source.tracks[1].clips.push_back(narration);
        auto bad=nestedTimelineToJson(source);bad["format"]="mutant-video-producer-project";bad["version"]=1;write(path,QJsonDocument(bad).toJson());require(project.load(path,nullptr),"repairable IDs refused");
        require(project.timeline().tracks[2].duckUnder==project.timeline().tracks[1].id,"repair renumbered track but broke its ducking reference");
        require(project.timeline().tracks[1].clips[0].narration["caption_owner"].toInt()==project.timeline().tracks[0].clips[0].id,"repair broke narration/caption ownership");
        source.tracks[2].duckUnder=source.tracks[2].id;source.tracks[2].duckAmount=-14;
        bad=nestedTimelineToJson(source);bad["format"]="mutant-video-producer-project";bad["version"]=1;
        write(path,QJsonDocument(bad).toJson());require(project.load(path,nullptr),"self-ducking track repair failed");
        require(project.timeline().tracks[2].duckUnder==0,"self-ducking reference survived repair");
        require(project.save(path,nullptr),"save repaired ducking");Project reloaded;
        require(reloaded.load(path,nullptr)&&reloaded.timeline()==project.timeline(),"disabled ducking changed on save/reload");
    } else if(name=="snapshot-storm") {
        Compositor reused;AudioMixer mixer(48000); std::vector<float> audio(1024*2); std::vector<std::shared_ptr<const ProjectSnapshot>> retained;
        for(int i=0;i<rounds;++i) {
            if(i%7==0)project.clear();auto edit=defaultTimeline();int id=ops::insertGenerated(edit,*findGeneratorPreset("solid_white"),0,-1,{});
            auto color=uint32_t(rng())&0xffffff;edit.findClip(id)->generator.params["color"].value=color;project.applyEdit(edit,"storm");
            auto snapshot=project.snapshot();Compositor fresh;
            require(reused.render(*snapshot,0,{64,36})==fresh.render(*snapshot,0,{64,36}),"reused renderer differs from fresh snapshot");
            mixer.mix(*snapshot,i*1024,1024,audio.data());require(std::all_of(audio.begin(),audio.end(),[](float value){return std::isfinite(value)&&value==0;}),"generated visual leaked audio");
            if(i%31==0)retained.push_back(snapshot);
            if(i%5==0){project.undoStack()->undo();project.undoStack()->redo();require(project.snapshot()->timeline==snapshot->timeline,"undo/redo changed state");}
        }
        for(auto snapshot:retained){Compositor fresh;require(reused.render(*snapshot,0,{64,36})==fresh.render(*snapshot,0,{64,36}),"retained immutable snapshot changed during later edits");}
    } else if(name=="motion-graphs") {
        for(int i=0;i<rounds;++i) {
            QJsonObject scene{{"version",2},{"variables",QJsonObject{{"a",QJsonObject{{"variable","b"}}},{"b",QJsonObject{{"variable","a"}}}}},{"layers",QJsonArray{QJsonObject{{"id","x"},{"type","rect"},{"x",QJsonObject{{"variable","a"}}},{"width",.2},{"height",.2},{"fill","#ffffff"}}}}};
            auto value=motion::evaluate(QJsonObject{{"variable","a"}},double(rng()%100)/30,scene);
            require(value.isNull()||value.isUndefined(),"cyclic variable produced fabricated numeric data");
            require(motion::evaluate(QJsonObject{{"variable","valid"}},0,QJsonObject{{"variables",QJsonObject{{"valid",.25}}}}).toDouble()==.25,"cycle poisoned evaluator recursion state");
            QImage image(64,36,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);motion::render(image,scene,0);
        }
    } else throw std::runtime_error("unknown case");
}
}
int main(int argc,char** argv) {
    QGuiApplication app(argc,argv);ff::initialize();auto args=app.arguments();
    if(args.value(1)=="--case") {
        try {run(args.value(2),QDir(args.value(3)),args.value(4).toUInt(),args.value(5).toInt());QTextStream(stdout)<<"PASS "<<args[2]<<" seed="<<args[4]<<" assertions="<<assertions<<Qt::endl;return 0;}
        catch(const std::exception& e){QTextStream(stdout)<<"FAIL "<<args[2]<<" seed="<<args[4]<<": "<<e.what()<<Qt::endl;return 1;}
    }
    // Without a folder, results go to the temp folder, never into wherever it was started from.
    const QString out=args.value(1,QDir::temp().filePath("mvp-adversarial-results"));QDir().mkpath(out);QTextStream(stdout)<<"results: "<<QDir(out).absolutePath()<<Qt::endl;
    const uint32_t seed=args.value(2,"13371337").toUInt();const int rounds=std::clamp(args.value(3,"512").toInt(),1,5000);
    int failures=0;QJsonArray report;
    for(auto name:{"versions","media-identity","keyframes","nesting","references","snapshot-storm","motion-graphs"}) {
        QProcess child;child.setProcessChannelMode(QProcess::MergedChannels);child.start(args[0],{"--case",name,out,QString::number(seed),QString::number(rounds)});
        const bool finished=child.waitForStarted(5000)&&child.waitForFinished(120000);
        if(!finished){child.kill();child.waitForFinished(5000);}
        auto log=child.readAll();write(QDir(out).filePath(QString(name)+".log"),log);
        const bool ok=finished&&child.exitStatus()==QProcess::NormalExit&&child.exitCode()==0;
        QTextStream(stdout)<<(ok?"PASS ":"FAIL ")<<name<<" seed="<<seed<<" "<<QString::fromUtf8(log).trimmed()<<Qt::endl;
        report.append(QJsonObject{{"case",name},{"seed",double(seed)},{"ok",ok},{"finished",finished},{"exit_code",child.exitCode()}});if(!ok)++failures;
    }
    write(QDir(out).filePath("summary.json"),QJsonDocument(report).toJson());return failures?1:0;
}
