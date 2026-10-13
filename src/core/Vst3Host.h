#pragma once

// VST3 audio effect plugins (Windows; built in when deps/vst3/pluginterfaces
// is present): finding, loading and running them, their saved state, and
// their own editor window. The "VST3 Plugin" audio effect (a_vst3) keeps the
// plugin's file, class and state (base64) as its settings; each mixer runs
// its own instance, following the saved state.

#include "core/AudioEffects.h"

#include <QByteArray>
#include <QSize>
#include <QStringList>
#include <functional>
#include <memory>

bool vst3Available();
// .vst3 plugins in the standard folders (Common Files\VST3 and the user's).
QStringList vst3InstalledPlugins();
// The audio effect classes in a plugin file (a shell such as Waves' has many).
QStringList vst3EffectClasses(const QString& path, QString* error = nullptr);

class Vst3Plugin {
public:
    ~Vst3Plugin();
    // `className` empty = the first audio effect in the file.
    static std::unique_ptr<Vst3Plugin> open(const QString& path, const QString& className, QString* error = nullptr);
    QString name() const;
    // Ready to process stereo at `sampleRate` in blocks of up to `maxBlock`.
    bool prepare(double sampleRate, int maxBlock);
    // In place, interleaved stereo; edits made in the editor go in first.
    void process(float* stereo, int frames);
    int latency() const;
    QByteArray state() const;
    bool setState(const QByteArray& state);

    // The plugin's editor in a native window (GUI thread).
    bool hasEditor() const;
    QSize openEditor(void* parentWindow); // its size; empty = failed
    void closeEditor();
    void setEditorSize(QSize size);       // the host window was resized
    std::function<void(QSize)> onEditorResize; // the plugin asks for a new size
    // Delivers the editor's changes since the last call to the processor
    // (so state() has them); false if there were none.
    bool flushEdits();

    struct Impl;

private:
    explicit Vst3Plugin(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

// The a_vst3 effect's processor (AudioEffects.cpp).
std::unique_ptr<AudioProcessor> createVst3Processor();
