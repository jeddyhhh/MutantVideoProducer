#include "ui/Vst3Window.h"

#include "core/Project.h"
#include "core/Vst3Host.h"

#include <QCloseEvent>
#include <QMessageBox>
#include <QTimer>
#include <functional>
#include <map>

namespace {

// The effect with this id anywhere in the timeline (events, tracks, master, racks).
EffectInstance* effectById(Timeline& tl, int id)
{
    std::function<EffectInstance*(EffectChain&)> find = [&](EffectChain& chain) -> EffectInstance* {
        for (EffectInstance& e : chain) {
            if (e.id == id)
                return &e;
            for (RackChain& c : e.chains)
                if (EffectInstance* hit = find(c.effects))
                    return hit;
        }
        return nullptr;
    };
    for (Track& t : tl.tracks) {
        if (EffectInstance* e = find(t.effects))
            return e;
        for (Clip& c : t.clips)
            if (EffectInstance* e = find(c.effects))
                return e;
    }
    return find(tl.audioMasterEffects);
}

using WindowKey = std::pair<std::shared_ptr<const int>, int>;
std::map<WindowKey, QPointer<Vst3Window>>& windows()
{
    static std::map<WindowKey, QPointer<Vst3Window>> open;
    return open;
}

} // namespace

void Vst3Window::open(Project* project, int effectId, QWidget* parent)
{
    const WindowKey key{project->snapshot()->session, effectId};
    const auto existing = windows().find(key);
    if (existing != windows().end() && existing->second) {
        existing->second->raise();
        existing->second->activateWindow();
        return;
    }
    Timeline tl = project->timeline();
    const EffectInstance* e = effectById(tl, effectId);
    if (!e)
        return;
    auto text = [&](const char* key) {
        auto it = e->params.find(QString::fromLatin1(key));
        return it == e->params.end() ? QString() : it->second.text;
    };
    QString error;
    std::unique_ptr<Vst3Plugin> plugin = Vst3Plugin::open(text("plugin"), text("class"), &error);
    if (!plugin || !plugin->hasEditor()) {
        QMessageBox::warning(parent, QObject::tr("VST3 Plugin"),
                             plugin ? QObject::tr("%1 has no window of its own.").arg(plugin->name())
                                    : QObject::tr("The plugin could not be opened: %1").arg(error.isEmpty() ? QObject::tr("none chosen") : error));
        return;
    }
    plugin->prepare(48000, 512); // (so edits reach its state)
    if (const QString state = text("state"); !state.isEmpty())
        plugin->setState(QByteArray::fromBase64(state.toLatin1()));
    auto* w = new Vst3Window(project, effectId, parent);
    w->m_plugin = std::move(plugin);
    w->setWindowTitle(w->m_plugin->name());
    w->m_plugin->onEditorResize = [w](QSize s) {
        w->m_resizing = true;
        w->resize(s);
        w->m_resizing = false;
    };
    const QSize size = w->m_plugin->openEditor(reinterpret_cast<void*>(w->winId()));
    if (size.isEmpty()) {
        QMessageBox::warning(parent, QObject::tr("VST3 Plugin"), QObject::tr("%1's window could not be opened.").arg(w->m_plugin->name()));
        delete w;
        return;
    }
    w->resize(size);
    w->show();
    windows()[key] = w;
}

Vst3Window::Vst3Window(Project* project, int effectId, QWidget* parent)
    : QWidget(parent, Qt::Window), m_project(project), m_session(project->snapshot()->session), m_effectId(effectId), m_timer(new QTimer(this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_NativeWindow);
    m_timer->setInterval(400); // one undo step per pause in the editing
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (m_plugin && m_plugin->flushEdits())
            save();
    });
    connect(project, &Project::changed, this, [this] {
        if (!m_project || m_session != m_project->snapshot()->session) {
            m_timer->stop();
            close(); // save() refuses to write the old plugin into the new project
        }
    });
    connect(project, &QObject::destroyed, this, [this] { m_timer->stop(); close(); });
    m_timer->start();
}

Vst3Window::~Vst3Window()
{
    windows().erase(WindowKey{m_session, m_effectId});
    if (m_plugin)
        m_plugin->closeEditor();
}

void Vst3Window::save()
{
    if (!m_project || m_session != m_project->snapshot()->session || !m_plugin)
        return;
    Timeline tl = m_project->timeline();
    EffectInstance* e = effectById(tl, m_effectId);
    if (!e)
        return; // the effect was removed: nothing to save into
    const QString state = QString::fromLatin1(m_plugin->state().toBase64());
    if (e->params[QStringLiteral("state")].text == state)
        return;
    e->params[QStringLiteral("state")].text = state;
    m_project->applyEdit(tl, tr("Change Plugin Setting"));
}

void Vst3Window::closeEvent(QCloseEvent* e)
{
    if (m_plugin) { // (also what changed without an edit, e.g. a preset picked in the plugin)
        m_plugin->flushEdits();
        save();
    }
    QWidget::closeEvent(e);
}

void Vst3Window::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (m_plugin && !m_resizing)
        m_plugin->setEditorSize(size());
}
