#include "core/ProjectCollection.h"
#include "core/Effects.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <limits>
#include <vector>

namespace collection {
namespace {
void slot(QJsonObject& o, const QString& key, Role role, const Rewrite& rewrite) {
    const auto value=o.value(key);
    if (value.isString() && !value.toString().isEmpty()) o[key] = rewrite(value.toString(), role);
}
void scene(QJsonObject& s, const Rewrite& rewrite, int sceneDepth=0) {
    if(sceneDepth>32)return;
    auto variables = s.value("variables").toObject(); QSet<QString> visiting, rewritten;
    QHash<QString,Role> bindings;
    std::function<void(QJsonValue,Role)> record=[&](QJsonValue value,Role role) {
        if(!value.isObject())return;auto o=value.toObject();
        if(o.contains("variable"))bindings[o.value("variable").toString()]=role;
        for(auto key:o.value("keyframes").toArray())if(key.toArray().size()>1)record(key.toArray()[1],role);
    };
    std::function<void(QJsonValue)> discover=[&](QJsonValue value) {
        if(value.isArray()){for(auto v:value.toArray())discover(v);return;}
        if(!value.isObject())return;auto o=value.toObject();
        if(o.value("type")=="image")record(o.value("path"),Role::Asset);
        if(o.contains("font"))record(o.value("font"),Role::Font);
        for(auto it=o.begin();it!=o.end();++it)if(it.value().isObject()||it.value().isArray())discover(it.value());
    };
    discover(s);
    std::function<QJsonValue(QJsonValue, Role)> property = [&](QJsonValue value, Role role) -> QJsonValue {
        if (value.isString()) return value.toString().isEmpty() ? value : QJsonValue(rewrite(value.toString(), role));
        if (!value.isObject()) return value;
        auto o = value.toObject();
        if (o.contains("variable")) {
            const QString id = o.value("variable").toString();
            bindings[id]=role;
            if (!visiting.contains(id) && !rewritten.contains(id) && variables.contains(id)) {
                visiting.insert(id); variables[id] = property(variables[id], role); visiting.remove(id); rewritten.insert(id);
            }
        } else if (o.contains("keyframes")) {
            auto keys = o.value("keyframes").toArray();
            for (int i=0; i<keys.size(); ++i) { auto key=keys[i].toArray(); if(key.size()>1) { key[1]=property(key[1],role); keys[i]=key; } }
            o["keyframes"] = keys;
        }
        return o;
    };
    std::function<QJsonArray(QJsonArray,int)> layers = [&](QJsonArray nodes,int depth) {
        if(depth>32)return nodes;
        for(int i=0;i<nodes.size();++i) {
            auto layer=nodes[i].toObject();
            if(layer.value("type")=="image")layer["path"]=property(layer.value("path"),Role::Asset);
            if(layer.contains("font"))layer["font"]=property(layer.value("font"),Role::Font);
            if(layer.contains("children"))layer["children"]=layers(layer.value("children").toArray(),depth+1);
            nodes[i]=layer;
        }
        return nodes;
    };
    if(s.contains("layers"))s["layers"]=layers(s.value("layers").toArray(),0);
    auto symbols=s.value("symbols").toObject();
    for(auto it=symbols.begin();it!=symbols.end();++it) {
        auto symbol=it.value().toObject();auto local=symbol.value("variables").toObject();
        // Symbols inherit scene variables; preserve any rewrites of inherited file bindings.
        auto combined=variables;for(auto v=local.begin();v!=local.end();++v)combined[v.key()]=v.value();
        symbol["variables"]=combined;scene(symbol,rewrite,sceneDepth+1);
        auto rewrittenVariables=symbol.value("variables").toObject();
        for(auto v=rewrittenVariables.begin();v!=rewrittenVariables.end();++v) {
            if(local.contains(v.key()))local[v.key()]=v.value();else variables[v.key()]=v.value();
        }
        if(local.isEmpty())symbol.remove("variables");else symbol["variables"]=local;
        it.value()=symbol;
    }
    if(s.contains("symbols"))s["symbols"]=symbols;
    // Instance/repeater overrides can provide image filenames for a symbol binding.
    std::function<QJsonArray(QJsonArray)> overrides=[&](QJsonArray nodes) {
        for(int i=0;i<nodes.size();++i) {
            auto layer=nodes[i].toObject();auto local=layer.value("variables").toObject();
            if(!local.isEmpty()) {
                auto inherited=variables;auto previousRewritten=rewritten;rewritten.clear();
                for(auto v=local.begin();v!=local.end();++v)variables[v.key()]=v.value();
                for(auto v=local.begin();v!=local.end();++v)if(bindings.contains(v.key()))variables[v.key()]=property(variables[v.key()],bindings[v.key()]);
                for(auto v=local.begin();v!=local.end();++v)v.value()=variables[v.key()];
                for(auto v=variables.begin();v!=variables.end();++v)if(!local.contains(v.key()))inherited[v.key()]=v.value();
                variables=inherited;rewritten=previousRewritten;layer["variables"]=local;
            }
            if(layer.contains("children"))layer["children"]=overrides(layer.value("children").toArray());nodes[i]=layer;
        }
        return nodes;
    };
    if(s.contains("layers"))s["layers"]=overrides(s.value("layers").toArray());
    if(s.contains("variables"))s["variables"]=variables;
}
void walk(QJsonObject& o, const Rewrite& rewrite, int depth=0) {
    if(depth>128) return; // project/animation parsers already impose smaller limits
    const QString type=o.value("type").toString();
    if(type=="graphics_overlay" && o.value("data").isObject() && !o.value("data").toObject().isEmpty()) { auto data=o.value("data").toObject(); scene(data,rewrite); o["data"]=data; }
    slot(o,"source_path",Role::Asset,rewrite);
    if(o.value("params").isObject()) {
        auto params=o.value("params").toObject();
        if(const auto definition=effectMeta(type)) for(const auto& p:*definition->params) {
            if(p.fileFilter.isEmpty() && p.type!=ParamType::Font) continue;
            auto value=params.value(p.id).toObject();
            slot(value,"text",p.type==ParamType::Font?Role::Font:p.id=="plugin"?Role::Plugin:Role::Asset,rewrite);
            if(params.contains(p.id))params[p.id]=value;
        }
        // Generated text uses the same serialized Font parameter.
        if(params.value("font").isObject()) { auto font=params.value("font").toObject(); slot(font,"text",Role::Font,rewrite); params["font"]=font; }
        o["params"]=params;
    }
    if(o.value("rvc").isObject()) { auto rvc=o.value("rvc").toObject(); slot(rvc,"model",Role::VoiceModel,rewrite); slot(rvc,"index",Role::VoiceModel,rewrite); o["rvc"]=rvc; }
    slot(o,"alignment_model",Role::VoiceModel,rewrite);
    for(const auto& key:{"media","takes","liveOverlays","overlays"}) if(o.value(key).isArray()) {
        auto entries=o.value(key).toArray();
        for(int i=0;i<entries.size();++i) if(entries[i].isObject()) {auto entry=entries[i].toObject();slot(entry,key==QString("liveOverlays")||key==QString("overlays")?"image":"path",key==QString("media")||key==QString("takes")?Role::Media:Role::Asset,rewrite);entries[i]=entry;}
        o[key]=entries;
    }
    for(auto it=o.begin();it!=o.end();++it) {
        if(it.value().isObject()) {auto child=it.value().toObject();walk(child,rewrite,depth+1);it.value()=child;}
        else if(it.value().isArray()) {auto array=it.value().toArray();for(int i=0;i<array.size();++i)if(array[i].isObject()){auto child=array[i].toObject();walk(child,rewrite,depth+1);array[i]=child;}it.value()=array;}
    }
}
QString safeName(QString name) {
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"),"_");
    name=name.left(120);
    while(name.endsWith('.')||name.endsWith(' '))name.chop(1);
    if(QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch())name.prepend('_');
    return name.isEmpty()?QStringLiteral("asset"):name;
}
bool writeJson(const QString& path,const QJsonObject& object,QString& error) {
    QSaveFile f(path);auto bytes=QJsonDocument(object).toJson(QJsonDocument::Indented);
    if(!f.open(QIODevice::WriteOnly)||f.write(bytes)!=bytes.size()||!f.commit()){error=f.errorString();return false;}return true;
}
void usedMedia(const QJsonObject& o,QSet<int>& ids,int depth=0) {
    if(depth>128)return;
    for(auto track:o["tracks"].toArray())for(auto value:track.toObject()["clips"].toArray()) {
        auto clip=value.toObject();ids.insert(clip["media"].toInt());
        for(auto take:clip["narration"].toObject()["takes"].toArray())ids.insert(take.toObject()["media"].toInt());
        if(clip.contains("sequence"))usedMedia(clip["sequence"].toObject(),ids,depth+1);
    }
}
}
void rewriteFiles(QJsonObject& document,const Rewrite& rewrite) {walk(document,rewrite);}

Result collect(QJsonObject document,const QString& sourceDirectory,const QString& destination,
               const QString& projectName,const Options& options,const Progress& progress) {
    Result result;
    const QString target=QDir::cleanPath(QFileInfo(destination).absoluteFilePath());const QDir parent=QFileInfo(target).absoluteDir();
    if(destination.trimmed().isEmpty()||QFileInfo::exists(target)||!parent.exists()) {result.error="Choose a new collection folder inside an existing directory. Existing folders are never overwritten.";return result;}
    if(document["format"]!="montage-project") {result.error="Not a Mutant Video Producer project.";return result;}
    if(!options.includeUnusedMedia) {
        QSet<int> used;usedMedia(document,used);QJsonArray media;
        for(auto value:document["media"].toArray())if(used.contains(value.toObject()["id"].toInt()))media.append(value);
        document["media"]=media;
    }
    struct File {QString source,relative; qint64 size; QDateTime modified; bool missing;};
    std::vector<File> files;QHash<QString,QString> assigned;QSet<QString> rewrittenPaths;QStringList plugins,fonts,skipped;
    qint64 total=0;
    rewriteFiles(document,[&](const QString& path,Role role) {
        if(result.cancelled)return path;
        if(progress&&!progress(0,total,path)){result.cancelled=true;return path;}
        if(role==Role::Font){if(!fonts.contains(path))fonts<<path;return path;}
        if(role==Role::Plugin){if(!plugins.contains(path))plugins<<path;return path;}
        if(rewrittenPaths.contains(path))return path;
        const QFileInfo source(QDir(sourceDirectory).absoluteFilePath(path));
        const QString absolute=source.absoluteFilePath();
        if(role==Role::VoiceModel&&!options.includeVoiceModels){if(!skipped.contains(absolute))skipped<<absolute;return absolute;}
        QString identity=source.canonicalFilePath();if(identity.isEmpty())identity=QDir::cleanPath(absolute);
#ifdef Q_OS_WIN
        identity=identity.toCaseFolded();
#endif
        if(assigned.contains(identity))return assigned[identity];
        const bool missing=!source.isFile();
        QString folder=missing?"Missing":role==Role::Media?"Media":role==Role::VoiceModel?"VoiceModels":"Assets";
        const QString relative=folder+'/'+QString("%1-%2").arg(files.size()+1,4,10,QChar('0')).arg(safeName(source.fileName()));
        assigned[identity]=relative;rewrittenPaths.insert(relative);files.push_back({absolute,relative,missing?0:source.size(),source.lastModified(),missing});
        if(missing)result.missing<<absolute;
        else if(source.size()>std::numeric_limits<qint64>::max()-total)result.error="Collection size overflow.";
        else total+=source.size();
        return relative;
    });
    if(result.cancelled||!result.error.isEmpty())return result;
    QStorageInfo storage(parent.absolutePath());
    if(storage.isValid()&&storage.isReady()&&storage.bytesAvailable()>=0&&storage.bytesAvailable()-16*1024*1024LL<total) {result.error="Not enough free disk space for this collection.";return result;}
    if(progress&&!progress(0,total,{})){result.cancelled=true;return result;}
    QTemporaryDir staging(parent.filePath(".mvp-collect-XXXXXX"));
    if(!staging.isValid()){result.error="Could not create the collection's temporary folder.";return result;}
    QJsonArray manifest;
    for(const auto& file:files) {
        QJsonObject entry{{"source",file.source},{"collected",file.relative},{"size",double(file.size)},{"status",file.missing?"missing":"copied"}};
        if(!file.missing) {
            const QString output=QDir(staging.path()).filePath(file.relative);
            if(!QDir().mkpath(QFileInfo(output).absolutePath())){result.error="Could not create asset directory.";return result;}
            QFile input(file.source);QSaveFile copy(output);QCryptographicHash hash(QCryptographicHash::Sha256);
            if(!input.open(QIODevice::ReadOnly)||!copy.open(QIODevice::WriteOnly)){result.error="Cannot copy "+file.source+": "+(input.isOpen()?copy.errorString():input.errorString());return result;}
            qint64 count=0;
            while(count<file.size) {
                auto bytes=input.read(std::min<qint64>(1024*1024,file.size-count));
                if(bytes.isEmpty()){result.error="Read failed or source was truncated: "+file.source;return result;}
                if(copy.write(bytes)!=bytes.size()){result.error="Write failed: "+copy.errorString();return result;}
                hash.addData(bytes);count+=bytes.size();result.copiedBytes+=bytes.size();
                if(progress&&!progress(result.copiedBytes,total,file.source)){result.cancelled=true;return result;}
            }
            QFileInfo after(file.source);
            if(count!=file.size||after.size()!=file.size||after.lastModified()!=file.modified){result.error="Source changed during collection: "+file.source;return result;}
            if(!copy.commit()){result.error=copy.errorString();return result;}
            entry["sha256"]=QString::fromLatin1(hash.result().toHex());++result.copiedFiles;
        }
        manifest.append(entry);
    }
    if(progress&&!progress(result.copiedBytes,total,{})){result.cancelled=true;return result;}
    if(!result.missing.isEmpty())result.warnings<<QString("%1 required file(s) are missing. See collection-report.json.").arg(result.missing.size());
    if(!plugins.isEmpty())result.warnings<<"VST plugins are not copied; install the listed plugins on the destination computer.";
    if(!fonts.isEmpty())result.warnings<<"Fonts are not copied; the report lists fonts to install for matching titles.";
    if(!skipped.isEmpty())result.warnings<<"Voice models were excluded; existing narration plays, but regenerating it needs those models.";
    document.remove("autosaveOf");document["collection"]=QJsonObject{{"version",1},{"relativeAssets",true}};
    auto media=document["media"].toArray();for(int i=0;i<media.size();++i){auto item=media[i].toObject();item["relativePath"]=item["path"];media[i]=item;}document["media"]=media;
    QString filename=safeName(QFileInfo(projectName).completeBaseName());filename+=".mtg";
    QJsonObject report{{"project",filename},{"files",manifest},{"missing",QJsonArray::fromStringList(result.missing)},
                       {"plugins",QJsonArray::fromStringList(plugins)},{"fonts",QJsonArray::fromStringList(fonts)},
                       {"excludedVoiceModels",QJsonArray::fromStringList(skipped)},{"warnings",QJsonArray::fromStringList(result.warnings)},
                       {"copiedFiles",result.copiedFiles},{"copiedBytes",double(result.copiedBytes)}};
    if(!writeJson(QDir(staging.path()).filePath(filename),document,result.error)||!writeJson(QDir(staging.path()).filePath("collection-report.json"),report,result.error))return result;
    if(progress&&!progress(result.copiedBytes,total,{})){result.cancelled=true;return result;}
    if(!QDir().rename(staging.path(),target)){result.error="Could not publish collection folder (destination may already exist).";return result;}
    staging.setAutoRemove(false);result.ok=true;result.projectPath=QDir(target).filePath(filename);result.reportPath=QDir(target).filePath("collection-report.json");return result;
}
}
