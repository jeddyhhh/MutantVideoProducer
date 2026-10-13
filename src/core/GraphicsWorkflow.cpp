#include "core/GraphicsWorkflow.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include <QColor>
#include <QJsonArray>
#include <QSet>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <functional>

namespace graphics_workflow {
namespace {
constexpr double canvasWidth = 1600, canvasHeight = 900;
QJsonObject find(const QJsonObject& scene, const QString& id) {
    for (auto v : scene["layers"].toArray()) if(v.toObject()["id"].toString()==id)return v.toObject();
    return {};
}
QJsonValue shifted(QJsonValue value, double delta) {
    if (!value.isObject()) return value.toDouble()+delta;
    auto spec = value.toObject();
    if(spec.contains("keyframes")) {
        auto keys = spec["keyframes"].toArray();
        for(int i=0;i<keys.size();++i){auto k=keys[i].toArray();k[1]=k[1].toDouble()+delta;keys[i]=k;}
        spec["keyframes"]=keys;
    } else if(spec.contains("variable")) spec["offset"]=spec["offset"].toDouble()+delta;
    else if(spec.contains("audio")) spec["base"]=spec["base"].toDouble()+delta;
    return spec;
}
QString selectionError(const QJsonObject& scene, const QStringList& ids) {
    if(ids.isEmpty())return "Select at least one layer.";
    QSet<QString> selected(ids.begin(),ids.end());
    if(selected.size()!=ids.size())return "Each selected layer must appear once.";
    for(auto id:ids){auto layer=find(scene,id);if(layer.isEmpty())return "Unknown layer: "+id;
        if(locked(scene,id))return "Unlock the selected layer or its parent first: "+id;
        auto parent=layer["parent"].toString();
        for(int depth=0;!parent.isEmpty()&&depth<16;++depth){if(selected.contains(parent))return "Select a group or its children, not both.";parent=find(scene,parent)["parent"].toString();}
    }
    return {};
}
QJsonObject moved(const QJsonObject& scene,QJsonObject layer,QPointF delta,double time) {
    if(scene["version"].toInt()==2) delta=motion::editDelta(scene,layer["id"].toString(),time,{canvasWidth,canvasHeight},delta);
    if(layer.contains("follow")){auto f=layer["follow"].toObject();f["offset_x"]=shifted(f["offset_x"],delta.x());f["offset_y"]=shifted(f["offset_y"],delta.y());layer["follow"]=f;}
    else{layer["x"]=shifted(layer["x"],delta.x());layer["y"]=shifted(layer["y"],delta.y());}
    return layer;
}
QHash<QString,QRectF> allBoxes(const QJsonObject& scene,double time) {
    QHash<QString,QRectF> result;
    if(scene["version"].toInt()==2){auto boxes=motion::allBounds(scene,time,{canvasWidth,canvasHeight});for(auto it=boxes.constBegin();it!=boxes.constEnd();++it){auto b=it.value();result.insert(it.key(),{b.x()/canvasWidth,b.y()/canvasHeight,b.width()/canvasWidth,b.height()/canvasHeight});}}
    else for(auto v:scene["layers"].toArray()){auto layer=v.toObject();result.insert(layer["id"].toString(),bounds(scene,layer,time));}
    return result;
}
}
QJsonObject prepare(const QJsonObject& scene) {
    auto result=scene;QJsonArray out;QSet<QString> used;
    std::function<void(QJsonArray)> collect=[&](QJsonArray a){for(auto v:a){auto o=v.toObject();used.insert(o["id"].toString());if(o["type"]=="group")collect(o["children"].toArray());}};collect(scene["layers"].toArray());
    int counter=0;
    std::function<void(QJsonArray,QString,int)> flatten=[&](QJsonArray a,QString parent,int depth){if(depth>16)return;for(auto v:a){auto o=v.toObject();if(o["id"].toString().isEmpty()){QString id;do{id="workflow-"+QString::number(++counter);}while(used.contains(id));used.insert(id);o["id"]=id;}if(!parent.isEmpty()&&!o.contains("parent"))o["parent"]=parent;
        auto children=o.value("children").toArray();bool group=o["type"]=="group"&&scene["version"].toInt()==2;if(group)o.remove("children");out.append(o);if(group)flatten(children,o["id"].toString(),depth+1);}};
    flatten(scene["layers"].toArray(),{},0);result["layers"]=out;return result;
}
bool locked(const QJsonObject& scene,const QString& id) {
    auto node=find(scene,id);
    for(int depth=0;!node.isEmpty()&&depth<=16;++depth){if(node["locked"].toBool())return true;node=find(scene,node["parent"].toString());}
    return false;
}
QRectF bounds(const QJsonObject& scene,const QJsonObject& layer,double time) {
    if(scene["version"].toInt()==2){auto b=motion::bounds(scene,layer["id"].toString(),time,{canvasWidth,canvasHeight});return {b.x()/canvasWidth,b.y()/canvasHeight,b.width()/canvasWidth,b.height()/canvasHeight};}
    auto n=[&](const char* key,double fallback=0){return motion::evaluate(layer[key],time,scene).toDouble(fallback);};
    QRectF b(n("x"),n("y"),n("width",.3),n("height",.1));
    QTransform t;t.translate(b.center().x()*canvasWidth,b.center().y()*canvasHeight);t.rotate(n("rotation"));t.translate(-b.center().x()*canvasWidth,-b.center().y()*canvasHeight);
    auto r=t.mapRect(QRectF(b.x()*canvasWidth,b.y()*canvasHeight,b.width()*canvasWidth,b.height()*canvasHeight));
    return {r.x()/canvasWidth,r.y()/canvasHeight,r.width()/canvasWidth,r.height()/canvasHeight};
}
QJsonObject translate(const QJsonObject& scene,const QStringList& ids,QPointF delta,double time,QString* error) {
    QString problem=selectionError(scene,ids);
    if(!std::isfinite(time)||time<0||!std::isfinite(delta.x())||!std::isfinite(delta.y()))problem="Use finite offsets and a nonnegative time.";
    if(!problem.isEmpty()){if(error)*error=problem;return {};}
    auto result=scene;auto layers=scene["layers"].toArray();
    for(int i=0;i<layers.size();++i){auto layer=layers[i].toObject();if(ids.contains(layer["id"].toString()))layers[i]=moved(scene,layer,delta,time);}
    result["layers"]=layers;
    problem=graphics::validateScene(result);if(!problem.isEmpty()){if(error)*error=problem;return {};}
    return result;
}
QJsonObject layout(const QJsonObject& scene,const QStringList& ids,const QString& operation,const QString& reference,double time,QString* error) {
    QString problem=selectionError(scene,ids);
    const QStringList operations{"left","hcenter","right","top","vcenter","bottom","distribute_h","distribute_v"};
    if(!operations.contains(operation))problem="Unknown layout operation.";
    if(!QStringList{"selection","canvas","safe"}.contains(reference))problem="Reference must be selection, canvas or safe.";
    if(!std::isfinite(time)||time<0)problem="Use a finite nonnegative time.";
    bool distribution=operation.startsWith("distribute_");
    if(distribution&&ids.size()<3)problem="Select at least three layers to distribute.";
    if(!problem.isEmpty()){if(error)*error=problem;return {};}
    QVector<QRectF> boxes;QRectF combined;
    auto evaluated=allBoxes(scene,time);
    for(auto id:ids){auto box=evaluated.value(id);boxes.append(box);combined=combined.isNull()?box:combined.united(box);}
    auto target=reference=="canvas"?QRectF(0,0,1,1):reference=="safe"?QRectF(.05,.05,.9,.9):combined;
    QVector<QPointF> offsets(ids.size());
    if(distribution){bool horizontal=operation=="distribute_h";QVector<int> order;for(int i=0;i<ids.size();++i)order.append(i);
        std::stable_sort(order.begin(),order.end(),[&](int a,int b){return horizontal?boxes[a].left()<boxes[b].left():boxes[a].top()<boxes[b].top();});
        double total=0;for(auto b:boxes)total+=horizontal?b.width():b.height();
        double extent=horizontal?target.width():target.height();double gap=(extent-total)/(ids.size()-1);
        double pos=horizontal?target.left():target.top();
        for(auto i:order){if(horizontal)offsets[i].setX(pos-boxes[i].left());else offsets[i].setY(pos-boxes[i].top());pos+=(horizontal?boxes[i].width():boxes[i].height())+gap;}
    }else for(int i=0;i<boxes.size();++i){auto b=boxes[i];if(operation=="left")offsets[i].setX(target.left()-b.left());if(operation=="right")offsets[i].setX(target.right()-b.right());if(operation=="hcenter")offsets[i].setX(target.center().x()-b.center().x());if(operation=="top")offsets[i].setY(target.top()-b.top());if(operation=="bottom")offsets[i].setY(target.bottom()-b.bottom());if(operation=="vcenter")offsets[i].setY(target.center().y()-b.center().y());}
    auto result=scene;auto layers=scene["layers"].toArray();
    for(int i=0;i<layers.size();++i){auto layer=layers[i].toObject();int index=ids.indexOf(layer["id"].toString());if(index>=0)layers[i]=moved(scene,layer,offsets[index],time);}
    result["layers"]=layers;problem=graphics::validateScene(result);if(!problem.isEmpty()){if(error)*error=problem;return {};}
    return result;
}
QPointF snap(const QJsonObject& scene,const QStringList& ids,QPointF delta,double time,double tolerance,QVector<double>* xGuides,QVector<double>* yGuides) {
    if(xGuides)xGuides->clear();if(yGuides)yGuides->clear();
    if(ids.isEmpty()||!std::isfinite(tolerance)||tolerance<=0)return delta;
    auto evaluated=allBoxes(scene,time);
    QRectF box;for(auto id:ids){auto b=evaluated.value(id);box=box.isNull()?b:box.united(b);}box.translate(delta);
    QVector<double> xs{0,.05,.5,.95,1},ys=xs;
    for(auto v:scene["layers"].toArray()){auto l=v.toObject();auto id=l["id"].toString();if(ids.contains(id)||!motion::evaluate(l["visible"],time,scene).toBool(true))continue;
        bool related=false;for(auto selected:ids){auto node=l;for(int d=0;!node.isEmpty()&&d<16;++d){if(node["parent"].toString()==selected)related=true;node=find(scene,node["parent"].toString());}auto node2=find(scene,selected);for(int d=0;!node2.isEmpty()&&d<16;++d){if(node2["parent"].toString()==id)related=true;node2=find(scene,node2["parent"].toString());}}
        if(related)continue;auto b=evaluated.value(id);xs<<b.left()<<b.center().x()<<b.right();ys<<b.top()<<b.center().y()<<b.bottom();}
    auto closest=[&](QVector<double> anchors,QVector<double> guides,QVector<double>* drawn){double best=tolerance,offset=0,line=0;bool found=false;for(auto a:anchors)for(auto g:guides)if(std::abs(g-a)<best){best=std::abs(g-a);offset=g-a;line=g;found=true;}if(found&&drawn)drawn->append(line);return offset;};
    return delta+QPointF(closest({box.left(),box.center().x(),box.right()},xs,xGuides),closest({box.top(),box.center().y(),box.bottom()},ys,yGuides));
}
QJsonObject setLocked(const QJsonObject& scene,const QStringList& ids,bool value,QString* error) {
    if(ids.isEmpty()){if(error)*error="Select layers to lock or unlock.";return {};}
    for(auto id:ids)if(find(scene,id).isEmpty()){if(error)*error="Unknown layer: "+id;return {};}
    auto result=scene;auto a=scene["layers"].toArray();for(int i=0;i<a.size();++i){auto o=a[i].toObject();if(ids.contains(o["id"].toString())){o["locked"]=value;a[i]=o;}}result["layers"]=a;
    auto problem=graphics::validateScene(result);if(!problem.isEmpty()){if(error)*error=problem;return {};}
    return result;
}
QString validateBrand(const QJsonObject& brand) {
    if(brand["version"].toInt()!=1||brand["name"].toString().trimmed().isEmpty()||brand["name"].toString().size()>128||brand["font"].toString().trimmed().isEmpty()||brand["font"].toString().size()>256)return "Brand kits need version 1, a name and a font.";
    for(auto role:{"primary","secondary","accent","background","foreground"})if(!brand[role].isString()||!QColor(brand[role].toString()).isValid())return "Brand colours must be valid: "+QString(role);
    return {};
}
QJsonObject applyBrand(const QJsonObject& scene,const QJsonObject& brand,QString* error) {
    auto problem=validateBrand(brand);if(!problem.isEmpty()){if(error)*error=problem;return {};}
    auto result=scene;
    std::function<QJsonArray(QJsonArray,const QJsonObject&)> visit=[&](QJsonArray layers,const QJsonObject& context){for(int i=0;i<layers.size();++i){auto l=layers[i].toObject();
        if(!locked(context,l["id"].toString())&&!l["locked"].toBool()) {
            auto role=l["brand_role"].toString();if(brand.contains(role)&&QStringList{"primary","secondary","accent","background","foreground"}.contains(role)&&!l["fill"].isObject())l["fill"]=brand[role];
            if(l["type"]=="text"&&!l["font"].isObject())l["font"]=brand["font"];
            if(l.contains("children"))l["children"]=visit(l["children"].toArray(),context);
        }layers[i]=l;}return layers;};
    auto context=prepare(scene);
    result["layers"]=visit(scene["layers"].toArray(),context);
    QSet<QString> protectedSymbols;
    for(auto v:context["layers"].toArray()){auto l=v.toObject();if(l["type"]=="instance"&&locked(context,l["id"].toString()))protectedSymbols.insert(l["symbol"].toString());}
    auto symbols=scene["symbols"].toObject();
    for(int pass=0;pass<16;++pass){auto previous=protectedSymbols;for(auto id:previous){std::function<void(QJsonArray)> protect=[&](QJsonArray layers){for(auto v:layers){auto l=v.toObject();if(l["type"]=="instance")protectedSymbols.insert(l["symbol"].toString());protect(l["children"].toArray());}};protect(symbols[id].toObject()["layers"].toArray());}if(previous==protectedSymbols)break;}
    for(auto it=symbols.begin();it!=symbols.end();++it){if(protectedSymbols.contains(it.key()))continue;auto def=it.value().toObject();auto local=def;local["version"]=2;def["layers"]=visit(def["layers"].toArray(),prepare(local));it.value()=def;}if(!symbols.isEmpty())result["symbols"]=symbols;
    result["brand"]=brand;
    problem=graphics::validateScene(result);if(!problem.isEmpty()){if(error)*error=problem;return {};}
    return result;
}
}
