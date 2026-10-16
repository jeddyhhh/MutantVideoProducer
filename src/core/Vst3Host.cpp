#include "core/Vst3Host.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#if defined(MVP_VST3) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

using namespace Steinberg;

namespace {

bool same(const TUID a, const TUID b)
{
    return FUnknownPrivate::iidEqual(a, b);
}

// FUnknown for the host's objects. Owned ones live as long as the plugin
// instance and are never deleted through release().
#define MVP_FUNKNOWN(Iface)                                                                 \
    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override                     \
    {                                                                                           \
        if (same(queried, FUnknown_iid) || same(queried, Iface##_iid)) {                               \
            addRef();                                                                           \
            *obj = static_cast<Iface*>(this);                                                   \
            return kResultOk;                                                                   \
        }                                                                                       \
        *obj = nullptr;                                                                         \
        return kNoInterface;                                                                    \
    }                                                                                           \
    uint32 PLUGIN_API addRef() override { return ++m_refs; }                                    \
    uint32 PLUGIN_API release() override { return --m_refs; }                                   \
    std::atomic<uint32> m_refs{1};

class HostApp : public Vst::IHostApplication {
public:
    MVP_FUNKNOWN(Vst::IHostApplication)
    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        const char16_t text[] = u"Mutant Video Producer";
        std::copy(std::begin(text), std::end(text), name);
        return kResultOk;
    }
    // ponytail: no host-made IMessage / IAttributeList (plugins send no messages then); add if a plugin needs them.
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override
    {
        *obj = nullptr;
        return kResultFalse;
    }
};
HostApp& hostApp()
{
    static HostApp app;
    return app;
}

class MemoryStream : public IBStream {
public:
    MVP_FUNKNOWN(IBStream)
    QByteArray data;
    int64 pos = 0;
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        const int32 n = int32(std::clamp<int64>(int64(data.size()) - pos, 0, numBytes));
        memcpy(buffer, data.constData() + pos, size_t(n));
        pos += n;
        if (numBytesRead)
            *numBytesRead = n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
    {
        if (pos + numBytes > data.size())
            data.resize(qsizetype(pos + numBytes));
        memcpy(data.data() + pos, buffer, size_t(numBytes));
        pos += numBytes;
        if (numBytesWritten)
            *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 to, int32 mode, int64* result) override
    {
        const int64 base = mode == kIBSeekSet ? 0 : (mode == kIBSeekCur ? pos : data.size());
        pos = std::clamp<int64>(base + to, 0, data.size());
        if (result)
            *result = pos;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* p) override
    {
        if (p)
            *p = pos;
        return kResultOk;
    }
};

class ParamQueue : public Vst::IParamValueQueue {
public:
    MVP_FUNKNOWN(Vst::IParamValueQueue)
    Vst::ParamID id = 0;
    std::vector<std::pair<int32, Vst::ParamValue>> points;
    Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return int32(points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, Vst::ParamValue& value) override
    {
        if (index < 0 || index >= int32(points.size()))
            return kResultFalse;
        offset = points[size_t(index)].first;
        value = points[size_t(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, Vst::ParamValue value, int32& index) override
    {
        index = int32(points.size());
        points.emplace_back(offset, value);
        return kResultOk;
    }
};

// Parameter changes for one process call: the last value of each edited parameter.
class ParamChanges : public Vst::IParameterChanges {
public:
    MVP_FUNKNOWN(Vst::IParameterChanges)
    std::vector<std::unique_ptr<ParamQueue>> queues;
    int used = 0;
    int32 PLUGIN_API getParameterCount() override { return used; }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        return index >= 0 && index < used ? queues[size_t(index)].get() : nullptr;
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override
    {
        for (int i = 0; i < used; ++i)
            if (queues[size_t(i)]->id == id) {
                index = i;
                return queues[size_t(i)].get();
            }
        if (used == int(queues.size()))
            queues.push_back(std::make_unique<ParamQueue>());
        ParamQueue* q = queues[size_t(used)].get();
        q->id = id;
        q->points.clear();
        index = used++;
        return q;
    }
    void clear() { used = 0; }
};

// The plugin's DLL, loaded once and kept (unloading plugins is unreliable).
struct Module {
    HMODULE dll = nullptr;
    IPluginFactory* factory = nullptr;
};

QString binaryPath(const QString& path)
{
    const QFileInfo info(path);
    if (info.isDir()) // a bundle: Name.vst3/Contents/x86_64-win/Name.vst3
        return QDir(path).filePath(QStringLiteral("Contents/x86_64-win/") + info.fileName());
    return path;
}

Module* module(const QString& path, QString* error)
{
    static std::mutex mutex;
    static std::map<QString, Module> modules;
    std::lock_guard lock(mutex);
    const QString file = QFileInfo(binaryPath(path)).absoluteFilePath();
    if (auto it = modules.find(file); it != modules.end())
        return &it->second;
    Module m;
    m.dll = LoadLibraryW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(file).utf16()));
    if (!m.dll) {
        if (error)
            *error = QStringLiteral("could not load %1").arg(file);
        return nullptr;
    }
    if (auto init = reinterpret_cast<bool (*)()>(GetProcAddress(m.dll, "InitDll")))
        init();
    auto getFactory = reinterpret_cast<IPluginFactory* (*)()>(GetProcAddress(m.dll, "GetPluginFactory"));
    m.factory = getFactory ? getFactory() : nullptr;
    if (!m.factory) {
        if (error)
            *error = QStringLiteral("%1 is not a VST3 plugin").arg(QFileInfo(path).fileName());
        return nullptr; // (the DLL stays loaded)
    }
    return &modules.emplace(file, m).first->second;
}

std::vector<PClassInfo> effectClasses(IPluginFactory* f)
{
    std::vector<PClassInfo> list;
    for (int32 i = 0; i < f->countClasses(); ++i) {
        PClassInfo info{};
        if (f->getClassInfo(i, &info) == kResultOk && strcmp(info.category, kVstAudioEffectClass) == 0)
            list.push_back(info);
    }
    return list;
}

template <class T>
void releaseInto(T*& p)
{
    if (p)
        p->release();
    p = nullptr;
}

} // namespace

struct Vst3Plugin::Impl : public Vst::IComponentHandler, public IPlugFrame {
    // IComponentHandler / IPlugFrame (one object, both interfaces).
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (same(iid, FUnknown_iid) || same(iid, Vst::IComponentHandler_iid)) {
            *obj = static_cast<Vst::IComponentHandler*>(this);
            return kResultOk;
        }
        if (same(iid, IPlugFrame_iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override
    {
        std::lock_guard lock(editMutex);
        edits[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
    tresult PLUGIN_API resizeView(IPlugView* v, ViewRect* r) override
    {
        if (!v || !r)
            return kInvalidArgument;
        v->onSize(r);
        if (owner && owner->onEditorResize)
            owner->onEditorResize(QSize(r->getWidth(), r->getHeight()));
        return kResultOk;
    }

    Vst3Plugin* owner = nullptr;
    QString name;
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    bool controllerIsComponent = false;
    Vst::IConnectionPoint* componentPoint = nullptr;
    Vst::IConnectionPoint* controllerPoint = nullptr;
    IPlugView* view = nullptr;
    bool active = false;
    int maxBlock = 0;
    int inChannels = 0, outChannels = 2;
    std::vector<float> inBuf, outBuf;
    std::mutex editMutex;
    std::map<Vst::ParamID, Vst::ParamValue> edits;
    ParamChanges changes;

    void takeEdits()
    {
        changes.clear();
        std::lock_guard lock(editMutex);
        for (const auto& [id, value] : edits) {
            int32 index = 0, point = 0;
            changes.addParameterData(id, index)->addPoint(0, value, point);
        }
        edits.clear();
    }

    void run(int frames, bool withInput)
    {
        std::vector<float*> ins(static_cast<size_t>(inChannels)), outs(static_cast<size_t>(outChannels));
        for (int c = 0; c < inChannels; ++c)
            ins[size_t(c)] = inBuf.data() + size_t(c) * maxBlock;
        for (int c = 0; c < outChannels; ++c)
            outs[size_t(c)] = outBuf.data() + size_t(c) * maxBlock;
        Vst::AudioBusBuffers in{}, out{};
        in.numChannels = inChannels;
        in.channelBuffers32 = ins.data();
        out.numChannels = outChannels;
        out.channelBuffers32 = outs.data();
        Vst::ProcessData data{};
        data.processMode = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples = frames;
        data.numInputs = withInput && inChannels > 0 ? 1 : 0;
        data.inputs = &in;
        data.numOutputs = outChannels > 0 ? 1 : 0;
        data.outputs = &out;
        takeEdits();
        data.inputParameterChanges = changes.used ? &changes : nullptr;
        processor->process(data);
    }

    ~Impl()
    {
        if (view) {
            view->removed();
            releaseInto(view);
        }
        if (active) {
            processor->setProcessing(false);
            component->setActive(false);
        }
        if (componentPoint && controllerPoint) {
            componentPoint->disconnect(controllerPoint);
            controllerPoint->disconnect(componentPoint);
        }
        releaseInto(componentPoint);
        releaseInto(controllerPoint);
        if (controller) {
            controller->setComponentHandler(nullptr);
            if (!controllerIsComponent)
                controller->terminate();
            releaseInto(controller);
        }
        releaseInto(processor);
        if (component) {
            component->terminate();
            releaseInto(component);
        }
    }
};

Vst3Plugin::Vst3Plugin(std::unique_ptr<Impl> impl) : d(std::move(impl))
{
    d->owner = this;
}

Vst3Plugin::~Vst3Plugin() = default;

bool vst3Available()
{
    return true;
}

QStringList vst3InstalledPlugins()
{
    QStringList dirs = {QStringLiteral("C:/Program Files/Common Files/VST3"),
                        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("Programs/Common/VST3"))};
    QStringList found;
    for (const QString& dir : dirs) {
        QDirIterator it(dir, {QStringLiteral("*.vst3")}, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            if (!p.contains(QLatin1String(".vst3/"), Qt::CaseInsensitive)) // not the binary inside a bundle
                found << p;
        }
    }
    found.sort(Qt::CaseInsensitive);
    return found;
}

QStringList vst3EffectClasses(const QString& path, QString* error)
{
    QStringList names;
    if (Module* m = module(path, error))
        for (const PClassInfo& info : effectClasses(m->factory))
            names << QString::fromUtf8(info.name);
    return names;
}

std::unique_ptr<Vst3Plugin> Vst3Plugin::open(const QString& path, const QString& className, QString* error)
{
    Module* m = module(path, error);
    if (!m)
        return nullptr;
    const std::vector<PClassInfo> classes = effectClasses(m->factory);
    auto cls = std::find_if(classes.begin(), classes.end(), [&](const PClassInfo& c) {
        return className.isEmpty() || QString::fromUtf8(c.name) == className;
    });
    if (cls == classes.end()) {
        if (error)
            *error = className.isEmpty() ? QStringLiteral("%1 has no audio effect").arg(QFileInfo(path).fileName())
                                         : QStringLiteral("%1 is not in %2").arg(className, QFileInfo(path).fileName());
        return nullptr;
    }
    auto d = std::make_unique<Impl>();
    d->name = QString::fromUtf8(cls->name);
    if (m->factory->createInstance(cls->cid, Vst::IComponent_iid, reinterpret_cast<void**>(&d->component)) != kResultOk || !d->component
        || d->component->initialize(&hostApp()) != kResultOk) {
        releaseInto(d->component);
        if (error)
            *error = QStringLiteral("%1 could not be started").arg(d->name);
        return nullptr;
    }
    if (d->component->queryInterface(Vst::IAudioProcessor_iid, reinterpret_cast<void**>(&d->processor)) != kResultOk) {
        if (error)
            *error = QStringLiteral("%1 does not process audio").arg(d->name);
        return nullptr;
    }
    // The edit controller: a separate class, or the component itself.
    TUID controllerId{};
    if (d->component->getControllerClassId(controllerId) == kResultOk
        && m->factory->createInstance(controllerId, Vst::IEditController_iid, reinterpret_cast<void**>(&d->controller)) == kResultOk && d->controller) {
        if (d->controller->initialize(&hostApp()) != kResultOk)
            releaseInto(d->controller);
    } else if (d->component->queryInterface(Vst::IEditController_iid, reinterpret_cast<void**>(&d->controller)) == kResultOk) {
        d->controllerIsComponent = true;
    }
    if (d->controller) {
        if (!d->controllerIsComponent) {
            d->component->queryInterface(Vst::IConnectionPoint_iid, reinterpret_cast<void**>(&d->componentPoint));
            d->controller->queryInterface(Vst::IConnectionPoint_iid, reinterpret_cast<void**>(&d->controllerPoint));
            if (d->componentPoint && d->controllerPoint) {
                d->componentPoint->connect(d->controllerPoint);
                d->controllerPoint->connect(d->componentPoint);
            }
            MemoryStream s;
            if (d->component->getState(&s) == kResultOk) {
                s.pos = 0;
                d->controller->setComponentState(&s);
            }
        }
        d->controller->setComponentHandler(d.get());
    }
    return std::unique_ptr<Vst3Plugin>(new Vst3Plugin(std::move(d)));
}

QString Vst3Plugin::name() const
{
    return d->name;
}

bool Vst3Plugin::prepare(double sampleRate, int maxBlock)
{
    if (d->active) {
        d->processor->setProcessing(false);
        d->component->setActive(false);
        d->active = false;
    }
    Vst::SpeakerArrangement stereo = Vst::SpeakerArr::kStereo;
    const int32 inputs = d->component->getBusCount(Vst::kAudio, Vst::kInput), outputs = d->component->getBusCount(Vst::kAudio, Vst::kOutput);
    if (outputs < 1)
        return false;
    d->processor->setBusArrangements(&stereo, inputs > 0 ? 1 : 0, &stereo, 1); // (a plugin may refuse: its own layout then)
    for (int32 i = 0; i < inputs; ++i)
        d->component->activateBus(Vst::kAudio, Vst::kInput, i, i == 0);
    for (int32 i = 0; i < outputs; ++i)
        d->component->activateBus(Vst::kAudio, Vst::kOutput, i, i == 0);
    Vst::BusInfo info{};
    d->inChannels = inputs > 0 && d->component->getBusInfo(Vst::kAudio, Vst::kInput, 0, info) == kResultOk ? info.channelCount : 0;
    d->outChannels = d->component->getBusInfo(Vst::kAudio, Vst::kOutput, 0, info) == kResultOk ? info.channelCount : 2;
    d->maxBlock = std::max(1, maxBlock);
    d->inBuf.assign(size_t(std::max(1, d->inChannels)) * d->maxBlock, 0.0f);
    d->outBuf.assign(size_t(std::max(1, d->outChannels)) * d->maxBlock, 0.0f);
    Vst::ProcessSetup setup{Vst::kRealtime, Vst::kSample32, d->maxBlock, sampleRate};
    if (d->processor->setupProcessing(setup) != kResultOk)
        return false;
    d->component->setActive(true);
    d->processor->setProcessing(true);
    d->active = true;
    return true;
}

void Vst3Plugin::process(float* stereo, int frames)
{
    if (!d->active)
        return;
    for (int done = 0; done < frames;) {
        const int n = std::min(frames - done, d->maxBlock);
        float* io = stereo + size_t(done) * 2;
        for (int c = 0; c < d->inChannels; ++c) // stereo to the plugin's channels (mono: both mixed)
            for (int i = 0; i < n; ++i)
                d->inBuf[size_t(c) * d->maxBlock + i] = d->inChannels == 1 ? 0.5f * (io[i * 2] + io[i * 2 + 1]) : io[i * 2 + std::min(c, 1)];
        d->run(n, true);
        if (d->outChannels > 0)
            for (int i = 0; i < n; ++i) {
                io[i * 2] = d->outBuf[size_t(i)];
                io[i * 2 + 1] = d->outBuf[size_t(std::min(d->outChannels - 1, 1)) * d->maxBlock + i];
            }
        done += n;
    }
}

int Vst3Plugin::latency() const
{
    return d->active ? int(d->processor->getLatencySamples()) : 0;
}

QByteArray Vst3Plugin::state() const
{
    MemoryStream s;
    return d->component->getState(&s) == kResultOk ? s.data : QByteArray();
}

bool Vst3Plugin::setState(const QByteArray& state)
{
    MemoryStream s;
    s.data = state;
    if (d->component->setState(&s) != kResultOk)
        return false;
    if (d->controller && !d->controllerIsComponent) {
        s.pos = 0;
        d->controller->setComponentState(&s);
    }
    return true;
}

bool Vst3Plugin::hasEditor() const
{
    return d->controller != nullptr;
}

QSize Vst3Plugin::openEditor(void* parentWindow)
{
    closeEditor();
    if (!d->controller)
        return {};
    d->view = d->controller->createView(Vst::ViewType::kEditor);
    if (!d->view || d->view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        releaseInto(d->view);
        return {};
    }
    d->view->setFrame(d.get());
    if (d->view->attached(parentWindow, kPlatformTypeHWND) != kResultOk) {
        releaseInto(d->view);
        return {};
    }
    ViewRect r{};
    d->view->getSize(&r);
    return QSize(r.getWidth(), r.getHeight());
}

void Vst3Plugin::closeEditor()
{
    if (!d->view)
        return;
    d->view->setFrame(nullptr);
    d->view->removed();
    releaseInto(d->view);
}

void Vst3Plugin::setEditorSize(QSize size)
{
    if (!d->view)
        return;
    ViewRect r(0, 0, size.width(), size.height());
    if (d->view->canResize() == kResultTrue && d->view->checkSizeConstraint(&r) == kResultTrue)
        d->view->onSize(&r);
}

bool Vst3Plugin::flushEdits()
{
    {
        std::lock_guard lock(d->editMutex);
        if (d->edits.empty())
            return false;
    }
    if (d->active)
        d->run(0, false); // (a call with no audio delivers parameter changes)
    else
        d->takeEdits();
    return true;
}

#else // no VST3 in this build

struct Vst3Plugin::Impl {};
Vst3Plugin::Vst3Plugin(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
Vst3Plugin::~Vst3Plugin() = default;
bool vst3Available() { return false; }
QStringList vst3InstalledPlugins() { return {}; }
QStringList vst3EffectClasses(const QString&, QString* error)
{
    if (error)
        *error = QStringLiteral("this build has no VST3 support");
    return {};
}
std::unique_ptr<Vst3Plugin> Vst3Plugin::open(const QString&, const QString&, QString* error)
{
    if (error)
        *error = QStringLiteral("this build has no VST3 support");
    return nullptr;
}
QString Vst3Plugin::name() const { return {}; }
bool Vst3Plugin::prepare(double, int) { return false; }
void Vst3Plugin::process(float*, int) {}
int Vst3Plugin::latency() const { return 0; }
QByteArray Vst3Plugin::state() const { return {}; }
bool Vst3Plugin::setState(const QByteArray&) { return false; }
bool Vst3Plugin::hasEditor() const { return false; }
QSize Vst3Plugin::openEditor(void*) { return {}; }
void Vst3Plugin::closeEditor() {}
void Vst3Plugin::setEditorSize(QSize) {}
bool Vst3Plugin::flushEdits() { return false; }

#endif

namespace {

// The a_vst3 effect: loads the plugin named in its settings (again when they
// change) and follows the saved state.
class Vst3Processor : public AudioProcessor {
public:
    void reset(int sampleRate) override
    {
        m_rate = sampleRate;
        m_prepared = false;
    }
    void process(float* stereo, int frames, const Params& p) override
    {
        const QString path = p.text(QStringLiteral("plugin")), cls = p.text(QStringLiteral("class"));
        if (path != m_path || cls != m_class) {
            m_path = path;
            m_class = cls;
            m_state.clear();
            m_plugin = path.isEmpty() ? nullptr : Vst3Plugin::open(path, cls);
            m_prepared = false;
        }
        if (!m_plugin)
            return; // (no plugin: the sound goes through)
        if (!m_prepared) {
            m_prepared = m_plugin->prepare(m_rate, 1024);
            m_state.clear(); // set again below
        }
        const QString state = p.text(QStringLiteral("state"));
        if (state != m_state) {
            m_state = state;
            if (!state.isEmpty())
                m_plugin->setState(QByteArray::fromBase64(state.toLatin1()));
        }
        const float mix = float(std::clamp(p.num(QStringLiteral("mix")), 0.0, 1.0));
        if (mix < 1.0f)
            m_dry.assign(stereo, stereo + size_t(frames) * 2);
        m_plugin->process(stereo, frames);
        if (mix < 1.0f)
            for (size_t i = 0; i < size_t(frames) * 2; ++i)
                stereo[i] = m_dry[i] + (stereo[i] - m_dry[i]) * mix;
    }
    int latency(const Params&) override { return m_plugin ? m_plugin->latency() : 0; }

private:
    int m_rate = 48000;
    bool m_prepared = false;
    QString m_path, m_class, m_state;
    std::unique_ptr<Vst3Plugin> m_plugin;
    std::vector<float> m_dry;
};

} // namespace

std::unique_ptr<AudioProcessor> createVst3Processor()
{
    return std::make_unique<Vst3Processor>();
}
