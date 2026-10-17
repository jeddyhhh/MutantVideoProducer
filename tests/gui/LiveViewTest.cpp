// LiveView's scripted GUI test, kept out of the editor's own source.
#include "ui/LiveView.h"
#include "ui/Theme.h"
#include "core/BeatTracker.h"
#include "core/AudioDecoder.h"
#include "core/LivePadSets.h"
#include "core/LiveRecorder.h"
#include "core/Project.h"
#include "ui/DeviceChainView.h"
#include "core/MediaInfo.h"
#include "ui/AudioMeter.h"
#include "ui/EffectsBrowser.h"
#include "ui/LiveClipsBrowser.h"
#include "ui/BeatTuner.h"
#include "ui/LiveOverlaysDialog.h"
#include "ui/LivePadPanel.h"
#include "ui/MidiHub.h"
#include "ui/PreviewWidget.h"
#include "ui/SetlistDialog.h"
#include "ui/StreamDialog.h"
#include "core/FFmpeg.h"
#include <QProcess>
#include "core/VideoShare.h"
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSlider>
#include <QTextEdit>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWheelEvent>
#include <cmath>
#include <QScrollArea>
#include <QTabWidget>

int LiveView::runSelfTest(QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        if (!ok)
            ++failures;
    };
    auto wait = [](int ms, const std::function<bool()>& done) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms && !done()) {
            QEventLoop loop;
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return done();
    };
    refreshSources();
    int screenItem = -1;
    for (int i = 0; i < m_source->count(); ++i) {
        const int s = m_source->itemData(i).toInt();
        if (s >= 0 && s < int(m_sources.size()) && m_sources[size_t(s)].kind == LiveSource::Kind::Screen) {
            screenItem = i;
            break;
        }
    }
    check(screenItem >= 0, QStringLiteral("live sources: %1 found, including a screen").arg(m_sources.size()));
    if (screenItem < 0)
        return failures;
    m_source->setCurrentIndex(screenItem);
    setActive(true);
    QString failure;
    QObject ctx;
    connect(m_engine, &LiveEngine::failed, &ctx, [&](const QString& e) { failure = e; });
    QImage last;
    connect(m_engine, &LiveEngine::frameReady, &ctx, [&](const QImage& img) { last = img; });
    m_frames = 0;
    startStop();
    const bool frames = wait(8000, [&] { return m_frames >= 10 || !failure.isEmpty(); }) && failure.isEmpty();
    check(frames && !last.isNull(), QStringLiteral("screen capture delivers pictures (%1 frames, %2x%3)%4")
                                        .arg(m_frames).arg(last.width()).arg(last.height())
                                        .arg(failure.isEmpty() ? QString() : QStringLiteral(": ") + failure));
    // An effect in the Live FX chain changes the picture.
    const QImage before = last;
    m_devices->addEffect(QStringLiteral("invert"));
    const int start = m_frames;
    wait(3000, [&] { return m_frames >= start + 5; });
    double diff = 0;
    if (!before.isNull() && before.size() == last.size()) {
        int64_t sum = 0, n = 0;
        for (int y = 0; y < last.height(); y += 7)
            for (int x = 0; x < last.width(); x += 7, ++n)
                sum += std::abs(qGray(before.pixel(x, y)) - qGray(last.pixel(x, y)));
        diff = n ? double(sum) / double(n) : 0;
    }
    check(diff > 20, QStringLiteral("the Live FX chain is applied (Invert changes the picture by %1)").arg(diff, 0, 'f', 1));
    check(!m_project->timeline().liveEffects.empty(), QStringLiteral("Live FX are kept in the project"));
    m_project->undoStack()->undo();
    check(m_project->timeline().liveEffects.empty(), QStringLiteral("adding a Live effect can be undone"));
    // Pads: a pad's chain goes on top of the Live FX when pressed, and off again.
    // How far a picture is from the plain one: the mean grey difference per pixel (a
    // difference of means would miss Invert on a picture that averages mid-grey).
    QImage plain;
    auto changed = [&](const QImage& img) {
        if (plain.isNull() || img.size() != plain.size())
            return 0.0;
        int64_t sum = 0, n = 0;
        for (int y = 0; y < img.height(); y += 7)
            for (int x = 0; x < img.width(); x += 7, ++n)
                sum += std::abs(qGray(img.pixel(x, y)) - qGray(plain.pixel(x, y)));
        return n ? double(sum) / double(n) : 0.0;
    };
    auto settle = [&](int frames) {
        const int target = m_frames + frames;
        wait(4000, [&] { return m_frames >= target; });
    };
    editChain(1);
    m_devices->addEffect(QStringLiteral("invert"));
    check(m_project->timeline().livePads[0].effects.size() == 1 && m_project->timeline().liveEffects.empty(),
          QStringLiteral("an effect added while editing pad 1 goes into the pad, not the Live FX"));
    m_pads->transitionBox()->setCurrentIndex(0); // cut
    settle(5);
    plain = last.copy();
    m_pads->padButton(0)->click();
    settle(5);
    const double onDiff = changed(last);
    check(m_activePad == 0 && onDiff > 20,
          QStringLiteral("pressing pad 1 switches its chain on (changes the picture by %1)").arg(onDiff, 0, 'f', 0));
    m_pads->padButton(0)->click();
    settle(5);
    const double offDiff = changed(last);
    check(m_activePad == -1 && offDiff < onDiff / 2,
          QStringLiteral("pressing it again switches it off (%1 from the plain picture)").arg(offDiff, 0, 'f', 0));
    // Crossfade over 2 s: half-way, the picture is between the two.
    m_pads->transitionBox()->setCurrentIndex(std::max(0, m_pads->transitionBox()->findData(QStringLiteral("dissolve"))));
    m_pads->durationBox()->setCurrentIndex(std::max(0, m_pads->durationBox()->findData(2.0)));
    QElapsedTimer fade;
    m_pads->padButton(0)->click();
    fade.start();
    wait(1100, [&] { return fade.elapsed() >= 1000; });
    const double midDiff = changed(last);
    wait(2500, [&] { return fade.elapsed() >= 2300; });
    settle(3);
    const double endDiff = changed(last);
    check(midDiff > endDiff * 0.15 && midDiff < endDiff * 0.85,
          QStringLiteral("a 2 s crossfade between pads is half-way after 1 s (changed by %1, then %2)")
              .arg(midDiff, 0, 'f', 0).arg(endDiff, 0, 'f', 0));
    m_pads->padButton(0)->click(); // off again
    m_pads->transitionBox()->setCurrentIndex(0);
    settle(3);
    // Pads are saved with the project.
    {
        Timeline tl = m_project->timeline();
        tl.livePads[3].name = QStringLiteral("Test pad");
        m_project->setTimelineLive(tl);
        const QString path = QDir(QFileInfo(liveRecordingsFolder()).absoluteFilePath()).filePath(QStringLiteral("pads.mvp"));
        QString error;
        Project reloaded;
        const bool ok = m_project->save(path, &error) && reloaded.load(path, &error);
        check(ok && reloaded.timeline().livePads == m_project->timeline().livePads,
              QStringLiteral("pads are saved with the project%1").arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error));
        tl.livePads[3].name.clear();
        m_project->setTimelineLive(tl);
    }
    // Pop Out: the picture moves to its own window and comes back.
    togglePopOut();
    settle(5);
    const bool popped = m_popOut && m_popOut->isVisible() && !m_popOut->image().isNull() && m_display->image().isNull();
    check(popped, QStringLiteral("Pop Out moves the picture into its own window (\"%1\", %2x%3)")
                      .arg(m_popOut ? m_popOut->windowTitle() : QString()).arg(m_popOut ? m_popOut->width() : 0).arg(m_popOut ? m_popOut->height() : 0));
    if (m_popOut)
        m_popOut->close();
    settle(5);
    check(m_popOut && !m_popOut->isVisible() && !m_display->image().isNull() && !m_popOutButton->isChecked(),
          QStringLiteral("closing that window brings the picture back"));
    editChain(0);
    m_project->undoStack()->undo(); // the pad's Invert
    check(m_project->timeline().livePads[0].effects.empty(), QStringLiteral("editing a pad can be undone"));

    // Locked pads, pad sets and the auto switch.
    {
        const int undoIndex = m_project->undoStack()->index();
        qputenv("MVP_PADSETS_DIR", QDir(liveRecordingsFolder()).filePath(QStringLiteral("pad-sets")).toUtf8());
        padsets::remove(QStringLiteral("Test Set A"));
        editChain(1);
        m_devices->addEffect(QStringLiteral("invert"));
        m_pads->setLocked(0, true);
        emit m_pads->dropped(0, QString::fromLatin1(EffectsBrowser::kEffectMime), QStringLiteral("sepia"));
        check(m_project->timeline().livePads[0].effects.size() == 1 && m_pads->isLocked(0) && !m_devices->isEnabled(),
              QStringLiteral("a locked pad takes no dropped effects and its chain is read-only"));
        check(m_pads->saveSetAs(QStringLiteral("Test Set A")) && padsets::names().contains(QStringLiteral("Test Set A")),
              QStringLiteral("the pads save as a set"));
        editChain(2);
        m_devices->addEffect(QStringLiteral("sepia"));
        const bool modified = m_pads->setModified();
        const bool loaded = m_pads->loadSet(QStringLiteral("Test Set A"));
        const LivePads& pads = m_project->timeline().livePads;
        check(modified && loaded && pads[1].effects.empty() && pads[0].effects.size() == 1 && pads[0].locked && !m_pads->setModified(),
              QStringLiteral("loading a set brings back its pads and locks (changes since were marked: %1)").arg(modified ? "yes" : "no"));
        // Auto switch on a tempo: 240 BPM, every beat, between two pads.
        editChain(2);
        m_devices->addEffect(QStringLiteral("sepia"));
        int switches = 0;
        QObject autoCtx;
        connect(m_pads, &LivePadPanel::padPressed, &autoCtx, [&] { ++switches; });
        m_pads->bpmBox()->setValue(240);
        m_pads->everyBox()->setCurrentIndex(0);
        m_pads->randomBox()->setChecked(false);
        m_pads->setAutoMode(LivePadPanel::AutoTempo);
        wait(1700, [] { return false; });
        m_pads->setAutoMode(LivePadPanel::AutoOff);
        const int tempoSwitches = switches;
        check(tempoSwitches >= 4 && tempoSwitches <= 9 && (m_activePad == 0 || m_activePad == 1),
              QStringLiteral("auto switch on a 240 BPM tempo: %1 switches in 1.7 s").arg(tempoSwitches));
        // Auto switch on beats heard: the engine's beats (150 BPM) switch pads.
        switches = 0;
        m_pads->setAutoMode(LivePadPanel::AutoAudio);
        for (int i = 0; i < 8; ++i) {
            m_pads->audioBeat(150.0, 0.8);
            wait(400, [] { return false; });
        }
        m_pads->setAutoMode(LivePadPanel::AutoOff);
        check(switches >= 7 && m_pads->bpm() == 150,
              QStringLiteral("auto switch on audio beats: %1 switches for 8 beats, %2 BPM shown").arg(switches).arg(m_pads->bpm()));
        if (m_activePad >= 0)
            triggerPad(m_activePad);
        // Create Random Effects Chain (pad menu) and Fill Empty Pads (set menu).
        const bool lockedRefused = !m_pads->randomizePad(0, 3);
        const bool made = m_pads->randomizePad(2, 3, 20261005); // a fixed chain, so the test is the same every time
        const LivePad wild = m_project->timeline().livePads[2];
        check(lockedRefused && made && wild.effects.size() >= 6 && wild.effects.size() <= 8 && !wild.name.isEmpty(),
              QStringLiteral("Create Random Effects Chain > Insane fills a pad (\"%1\", %2 effects; a locked pad is left alone)")
                  .arg(wild.name).arg(wild.effects.size()));
        QString statsName;
        double statsMs = 0;
        QObject statsCtx;
        connect(m_engine, &LiveEngine::statsChanged, &statsCtx, [&](double, double, double ms, const QString& name, QSize) {
            statsName = name;
            statsMs = ms;
        });
        triggerPad(2);
        QElapsedTimer firstTimer;
        firstTimer.start();
        int framesBefore = m_frames;
        wait(15000, [&] { return m_frames > framesBefore || !failure.isEmpty(); }); // the first frame compiles its shaders
        const qint64 firstMs = firstTimer.elapsed();
        // (Software OpenGL in test machines takes ~0.2 s a frame for this chain; a GPU takes a few ms.)
        framesBefore = m_frames;
        QElapsedTimer playTimer;
        playTimer.start();
        wait(10000, [&] { return m_frames >= framesBefore + 3 || !failure.isEmpty(); });
        QStringList types;
        for (const EffectInstance& e : wild.effects)
            types << e.type;
        check(m_frames >= framesBefore + 3 && failure.isEmpty(),
              QStringLiteral("the random chain plays live (%1 frames in %2 ms, first after %3 ms, %4: %5)%6")
                  .arg(m_frames - framesBefore).arg(playTimer.elapsed()).arg(firstMs).arg(statsName + QStringLiteral(" %1 ms/frame").arg(statsMs, 0, 'f', 0), types.join(QLatin1Char(',')))
                  .arg(failure.isEmpty() ? QString() : QStringLiteral(": ") + failure));
        triggerPad(2);
        const int filled = m_pads->randomizeEmptyPads(0);
        int lowPads = 0;
        for (int i = 3; i < kLivePads; ++i)
            lowPads += m_project->timeline().livePads[size_t(i)].effects.size() == 2;
        check(filled == kLivePads - 3 && lowPads == filled && m_project->timeline().livePads[2].name == wild.name,
              QStringLiteral("Fill Empty Pads with Random Chains > Low fills only the empty pads (%1 filled)").arg(filled));
        padsets::remove(QStringLiteral("Test Set A"));
        while (m_project->undoStack()->index() > undoIndex)
            m_project->undoStack()->undo();
        editChain(0);
        check(m_project->timeline().livePads == LivePads(), QStringLiteral("pad edits, locks and set loads undo"));
    }

    // A broken-console effect runs live too.
    m_devices->addEffect(QStringLiteral("vg_nes_dirty"));
    const int start2 = m_frames;
    const bool still = wait(4000, [&] { return m_frames >= start2 + 5 || !failure.isEmpty(); });
    check(still && failure.isEmpty(), QStringLiteral("NES: Dirty Cartridge renders live (%1 more frames)").arg(m_frames - start2));
    // Record a clip with the effects, then find it in the Editor's From Live tab.
    QString recorded, recError;
    double recSecs = 0;
    connect(m_engine, &LiveEngine::recordingFinished, &ctx, [&](const QString& p, bool ok, const QString& e, double s) {
        recorded = ok ? p : QString();
        recError = e;
        recSecs = s;
        if (!ok)
            recorded = QStringLiteral("!");
    });
    // Record with sound: a generated tone stands in for a microphone.
    m_audioSources.push_back({QStringLiteral("Test tone"), QStringLiteral("lavfi"), QStringLiteral("sine=frequency=440:sample_rate=48000,arealtime")});
    m_audio->addItem(QStringLiteral("Test tone"), int(m_audioSources.size()) - 1);
    const int audioBefore = m_audio->currentIndex();
    QString audioOpened, audioError;
    connect(m_engine, &LiveEngine::audioStatus, &ctx, [&](const QString& d, const QString& e) {
        audioOpened = d;
        audioError = e;
    });
    m_audio->setCurrentIndex(m_audio->count() - 1);
    wait(3000, [&] { return !audioOpened.isEmpty() || !audioError.isEmpty(); });
    check(!audioOpened.isEmpty(), QStringLiteral("the Audio input opens while capturing (%1)").arg(audioOpened.isEmpty() ? audioError : audioOpened));
    toggleRecording();
    wait(300, [] { return false; });
    const bool recording = m_engine->isRecording();
    wait(2200, [] { return false; });
    toggleRecording();
    wait(5000, [&] { return !recorded.isEmpty(); });
    const MediaInfo info = recorded.size() > 1 ? probeMedia(recorded) : MediaInfo();
    check(recording && info.ok() && info.hasVideo() && info.duration > kUsPerSecond * 3 / 2,
          QStringLiteral("records a clip with the effects (%1 s reported, %2 s in the file, %3x%4)%5")
              .arg(recSecs, 0, 'f', 2).arg(double(info.duration) / kUsPerSecond, 0, 'f', 2)
              .arg(info.hasVideo() ? info.video.front().width : 0).arg(info.hasVideo() ? info.video.front().height : 0)
              .arg(recError.isEmpty() ? QString() : QStringLiteral(": ") + recError));
    float soundPeak = 0;
    if (info.hasAudio()) {
        AudioDecoder adec;
        std::vector<float> buf(48000 * 2);
        if (adec.open(recorded, info.audio.front().index, 48000))
            adec.read(48000 / 2, 48000, buf.data());
        for (float v : buf)
            soundPeak = std::max(soundPeak, std::abs(v));
    }
    check(info.hasAudio() && soundPeak > 0.1f,
          QStringLiteral("the clip has the Audio input's sound (%1 audio stream(s), peak %2)").arg(info.audio.size()).arg(soundPeak, 0, 'f', 3));
    // Beats heard: a generated 120 BPM kick drum as the input.
    {
        m_audioSources.push_back({QStringLiteral("Test beat"), QStringLiteral("lavfi"),
                                  QStringLiteral("aevalsrc=exprs='0.8*exp(-14*mod(t,0.5))*sin(2*PI*55*t)':sample_rate=48000,arealtime")});
        m_audio->addItem(QStringLiteral("Test beat"), int(m_audioSources.size()) - 1);
        int beats = 0;
        double heard = 0;
        connect(m_engine, &LiveEngine::beat, &ctx, [&](double bpm, double) {
            ++beats;
            heard = bpm;
        });
        m_audio->setCurrentIndex(m_audio->count() - 1);
        wait(7000, [&] { return beats >= 8 && std::abs(heard - 120) < 1.0; });
        check(beats >= 6 && std::abs(heard - 120) < 1.5,
              QStringLiteral("beat detection on a 120 BPM kick from the Audio input: %1 BPM, %2 beats").arg(heard, 0, 'f', 2).arg(beats));
        // The Tune window shows what it hears (a picture of it goes next to the test recordings).
        // Showing it must not slow the picture down: compare the frames shown
        // and the time the window takes on the UI thread.
        const int framesBefore = m_frames;
        wait(2000, [] { return false; });
        const int without = m_frames - framesBefore;
        m_pads->showBeatTuner();
        auto* tuner = m_pads->findChild<BeatTuner*>();
        wait(300, [] { return false; });
        const int framesWith = m_frames;
        const double busyBefore = tuner ? tuner->busyMs() : 0;
        wait(2000, [] { return false; });
        const int with = m_frames - framesWith;
        const double busy = tuner ? (tuner->busyMs() - busyBefore) / 2 : 0;
        check(with >= without * 85 / 100 && busy < 60,
              QStringLiteral("the Tune window doesn't slow the picture down (%1 frames in 2 s with it open, %2 without; it takes %3 ms/s)")
                  .arg(with).arg(without).arg(busy, 0, 'f', 1));
        if (tuner) {
            if (qEnvironmentVariableIsSet("MVP_LIVE_DIR"))
                tuner->grab().save(QDir(qEnvironmentVariable("MVP_LIVE_DIR")).filePath(QStringLiteral("beat_tuner.png")));
            tuner->hide();
        }
        check(tuner != nullptr, QStringLiteral("Tune... opens the Beat Detection window"));
        // Lock BPM: the music changes to a 150 BPM kick (a new input), the tempo stays
        // and the beats keep coming; Find Tempo Again unlocks it and hears 150.
        BeatTracker* tracker = m_engine->beatTracker();
        m_pads->lockBpmButton()->click();
        const double locked = tracker->bpm();
        m_audioSources.push_back({QStringLiteral("Test beat 150"), QStringLiteral("lavfi"),
                                  QStringLiteral("aevalsrc=exprs='0.8*exp(-14*mod(t,0.4))*sin(2*PI*55*t)':sample_rate=48000,arealtime")});
        m_audio->addItem(QStringLiteral("Test beat 150"), int(m_audioSources.size()) - 1);
        m_audio->setCurrentIndex(m_audio->count() - 1);
        beats = 0;
        wait(5000, [] { return false; });
        check(m_pads->lockBpmButton()->isChecked() && tracker->tempoLocked() && std::abs(locked - 120) < 1.5 && tracker->bpm() == locked
                  && std::abs(heard - locked) < 0.01 && beats >= 6,
              QStringLiteral("Lock BPM keeps %1 BPM when the music changes to 150 (%2 beats in 5 s at %3)").arg(locked, 0, 'f', 2).arg(beats).arg(heard, 0, 'f', 2));
        QPushButton* again = nullptr;
        if (tuner)
            for (QPushButton* b : tuner->findChildren<QPushButton*>())
                if (b->text() == tr("Find Tempo Again"))
                    again = b;
        if (again)
            again->click();
        wait(7000, [&] { return std::abs(tracker->bpm() - 150) < 1.0; });
        check(again && !m_pads->lockBpmButton()->isChecked() && !tracker->tempoLocked() && std::abs(tracker->bpm() - 150) < 1.5,
              QStringLiteral("Find Tempo Again unlocks it and hears the new tempo (%1 BPM)").arg(tracker->bpm(), 0, 'f', 2));
        // /2 and x2: the tempo heard halves and stays there, then doubles back.
        m_pads->setAutoMode(LivePadPanel::AutoAudio);
        m_pads->scaleTempo(0.5);
        wait(3000, [] { return false; });
        const double halved = tracker->bpm();
        const double shown = m_pads->bpmBox()->value();
        m_pads->scaleTempo(2.0);
        wait(1500, [] { return false; });
        check(std::abs(halved - 75) < 1.5 && std::abs(shown - 75) <= 1 && std::abs(tracker->bpm() - 150) < 1.5,
              QStringLiteral("/2 holds the 150 BPM beat at %1 (shown %2), x2 brings it back to %3").arg(halved, 0, 'f', 2).arg(shown).arg(tracker->bpm(), 0, 'f', 2));
        // Find Tempo Again under the pads: locked, then forgotten and found again.
        m_pads->lockBpmButton()->click();
        m_pads->findTempoButton()->click();
        const bool forgotten = !m_pads->lockBpmButton()->isChecked() && !tracker->tempoLocked() && tracker->bpm() == 0;
        wait(7000, [&] { return std::abs(tracker->bpm() - 150) < 0.5; });
        check(forgotten && std::abs(tracker->bpm() - 150) < 1.5 && std::abs(m_pads->bpmBox()->value() - tracker->bpm()) < 0.6,
              QStringLiteral("Find Tempo Again under the pads unlocks and forgets the tempo, then hears %1 (shown %2)")
                  .arg(tracker->bpm(), 0, 'f', 2).arg(m_pads->bpmBox()->text()));
        // Set 1 right after a beat: that beat is the 1, the bar counts on from it.
        std::vector<int> bar;
        bool armed = false;
        int startBar = 0;
        connect(m_engine, &LiveEngine::beat, &ctx, [&] {
            if (!armed) {
                armed = true;
                m_pads->setDownbeat();
                startBar = m_pads->barBeat();
            } else {
                bar.push_back(m_pads->barBeat());
            }
        });
        wait(5000, [&] { return bar.size() >= 8; });
        bool counts = bar.size() >= 8 && startBar == 1;
        for (size_t i = 0; counts && i < bar.size(); ++i)
            counts = bar[i] == int(i % 4) + 2 - (i % 4 == 3 ? 4 : 0);
        QStringList seq;
        for (int v : bar)
            seq << QString::number(v);
        check(counts, QStringLiteral("Set 1 on a beat makes it the 1: then %1").arg(seq.join(QLatin1Char(' '))));
        m_pads->setAutoMode(LivePadPanel::AutoOff);
    }
    m_audio->setCurrentIndex(audioBefore);
    auto* clips = window()->findChild<LiveClipsBrowser*>();
    bool listed = false;
    wait(3000, [&] {
        if (!clips)
            return false;
        clips->refresh();
        for (int i = 0; i < clips->count(); ++i)
            listed |= clips->item(i)->data(Qt::UserRole).toString() == recorded;
        return listed;
    });
    check(listed, QStringLiteral("the clip is listed in the From Live tab"));
    if (clips && listed) {
        const size_t mediaBefore = m_project->media().size();
        emit clips->addToTimelineRequested(recorded);
        emit clips->addToTimelineRequested(recorded);
        int onTimeline = 0;
        for (const auto& t : m_project->timeline().tracks)
            for (const auto& c : t.clips)
                if (auto m = m_project->mediaItem(c.mediaId); m && m->path == recorded)
                    ++onTimeline;
        check(m_project->media().size() == mediaBefore + 1 && onTimeline >= 2,
              QStringLiteral("adding it to the timeline imports it once (%1 new media, %2 events)")
                  .arg(m_project->media().size() - mediaBefore).arg(onTimeline));
    }
    // ---- Momentary pads, pad keys, MIDI, modulation, the layer, overlays, the replay buffer ----
    {
        QObject featCtx;
        if (m_activePad >= 0)
            triggerPad(m_activePad);
        m_pads->transitionBox()->setCurrentIndex(0); // cut
        m_pads->setAutoMode(LivePadPanel::AutoOff);
        auto meanGray = [](const QImage& img, QRect r = QRect()) {
            if (r.isNull())
                r = img.rect();
            r &= img.rect();
            int64_t sum = 0, n = 0;
            for (int y = r.top(); y <= r.bottom(); y += 5)
                for (int x = r.left(); x <= r.right(); x += 5, ++n)
                    sum += qGray(img.pixel(x, y));
            return n ? double(sum) / double(n) : 0.0;
        };
        auto diffIn = [](const QImage& a, const QImage& b, QRect r) {
            if (a.size() != b.size())
                return 999.0;
            r &= a.rect();
            int64_t sum = 0, n = 0;
            for (int y = r.top(); y <= r.bottom(); y += 5)
                for (int x = r.left(); x <= r.right(); x += 5, ++n)
                    sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
            return n ? double(sum) / double(n) : 0.0;
        };
        {
            Timeline tl = m_project->timeline();
            tl.liveEffects.clear();
            tl.livePads[1] = LivePad();
            tl.livePads[1].effects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
            tl.livePads[1].momentary = true;
            tl.livePads[2] = LivePad();
            tl.livePads[2].effects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
            m_project->applyEdit(tl, QStringLiteral("Test pads"));
        }
        settle(5);
        plain = last.copy();
        // A momentary pad: on while held, off when let go.
        runAction(QStringLiteral("pad:1"), true);
        settle(5);
        const double heldDiff = changed(last);
        runAction(QStringLiteral("pad:1"), false);
        settle(5);
        const double releasedDiff = changed(last);
        check(heldDiff > 20 && releasedDiff < heldDiff / 2 && m_activePad == -1,
              QStringLiteral("a momentary pad is on while held (changes the picture by %1) and off when let go (%2)").arg(heldDiff, 0, 'f', 0).arg(releasedDiff, 0, 'f', 0));
        // Pad keys: 3 switches pad 3 on and off (when the window has the keyboard).
        window()->raise();
        window()->activateWindow();
        wait(500, [&] { return QApplication::activeWindow() == window(); });
        if (QApplication::activeWindow() == window()) {
            auto key = [&](QEvent::Type type) {
                QKeyEvent e(type, Qt::Key_3, Qt::NoModifier, QStringLiteral("3"));
                QApplication::sendEvent(QApplication::focusWidget() ? static_cast<QObject*>(QApplication::focusWidget()) : this, &e);
            };
            key(QEvent::KeyPress);
            key(QEvent::KeyRelease);
            const bool on = m_activePad == 2;
            key(QEvent::KeyPress);
            key(QEvent::KeyRelease);
            check(on && m_activePad == -1, QStringLiteral("the 3 key switches pad 3 on and off"));
        } else {
            log << QStringLiteral("SKIP  pad keys: the window doesn't have the keyboard");
        }
        // MIDI: learn a note for pad 3; it then switches it; controllers reach modulation.
        const QMap<QString, QString> mapBefore = m_midiMap;
        m_midiLearn = QStringLiteral("pad:2");
        m_midiLearnLabel = QStringLiteral("pad 3");
        MidiMessage noteOn, noteOff, cc;
        noteOn.status = 0x90;
        noteOn.data1 = 60;
        noteOn.data2 = 100;
        noteOff.status = 0x80;
        noteOff.data1 = 60;
        midiMessage(noteOn); // learned, not played
        const bool learned = m_midiMap.value(QStringLiteral("n:0:60")) == QLatin1String("pad:2") && m_activePad == -1;
        midiMessage(noteOff);
        midiMessage(noteOn);
        const bool played = m_activePad == 2;
        midiMessage(noteOff);
        midiMessage(noteOn);
        check(learned && played && m_activePad == -1, QStringLiteral("Learn MIDI: a note learned for pad 3 switches it on and off"));
        m_midiMap = mapBefore;
        saveMidiMap();
        // Modulation: Brightness moved by MIDI controller 7, then by the beat.
        {
            Timeline tl = m_project->timeline();
            EffectInstance br = makeEffectOfType(QStringLiteral("brightness_contrast"), tl.allocId());
            Modulation m;
            m.source = Modulation::Source::Midi;
            m.channel = 0;
            m.cc = 7;
            br.params[QStringLiteral("brightness")].mod = m;
            tl.liveEffects = {br};
            m_project->applyEdit(tl, QStringLiteral("Test modulation"));
        }
        cc.status = 0xB0;
        cc.data1 = 7;
        cc.data2 = 127;
        midiMessage(cc);
        settle(5);
        const double high = meanGray(last);
        cc.data2 = 0;
        midiMessage(cc);
        settle(5);
        const double low = meanGray(last);
        check(high - low > 60 && m_engine->modInputs().midi[0][7] == 0,
              QStringLiteral("a MIDI controller moves a modulated setting (Brightness: mean grey %1 at 127, %2 at 0)").arg(high, 0, 'f', 0).arg(low, 0, 'f', 0));
        {
            Timeline tl = m_project->timeline();
            Modulation m;
            m.source = Modulation::Source::Beat;
            m.amount = 0.8;
            m.beats = 1;
            tl.liveEffects[0].params[QStringLiteral("brightness")].value = -0.4;
            tl.liveEffects[0].params[QStringLiteral("brightness")].mod = m;
            m_project->applyEdit(tl, QStringLiteral("Test modulation"));
        }
        m_pads->bpmBox()->setValue(120);
        double lo = 1e9, hi = -1;
        wait(1500, [&] {
            const double g = meanGray(last);
            lo = std::min(lo, g);
            hi = std::max(hi, g);
            return false;
        });
        check(hi - lo > 40, QStringLiteral("a beat-pulse modulation flashes the picture on the beat (mean grey %1 to %2)").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0));
        {
            Timeline tl = m_project->timeline();
            tl.liveEffects.clear();
            m_project->applyEdit(tl, QStringLiteral("Test modulation"));
        }
        settle(5);
        plain = last.copy();
        // The layer: a video file in the bottom-right corner, then with its own FX.
        const QString video = QDir(QStringLiteral("testmedia")).absoluteFilePath(QStringLiteral("h264_aac.mp4"));
        if (QFileInfo::exists(video)) {
            QString layerDesc, layerErr;
            connect(m_engine, &LiveEngine::layerStatus, &featCtx, [&](const QString& d, const QString& e) {
                layerDesc = d;
                layerErr = e;
            });
            LiveSource file;
            file.kind = LiveSource::Kind::File;
            file.name = QStringLiteral("test video");
            file.id = video;
            LiveLayerSettings ls;
            ls.mode = LiveLayerSettings::Mode::PictureInPicture;
            ls.corner = 3;
            ls.size = 0.33;
            m_engine->setLayerSettings(ls);
            m_engine->setLayer(file);
            wait(3000, [&] { return !layerDesc.isEmpty() || !layerErr.isEmpty(); });
            settle(10);
            const QImage withLayer = last.copy();
            const int w = withLayer.width(), h = withLayer.height(), m = int(w * 0.03);
            const QRect corner(w - m - int(w * 0.33) + 6, h - m - int(w * 0.33 * 9 / 16) + 6, int(w * 0.33) - 12, int(w * 0.33 * 9 / 16) - 12);
            const double cornerDiff = diffIn(plain, withLayer, corner);
            {
                Timeline tl = m_project->timeline();
                tl.liveLayerEffects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
                m_project->applyEdit(tl, QStringLiteral("Test layer FX"));
            }
            settle(5);
            const double fxDiff = diffIn(withLayer, last, corner);
            check(!layerDesc.isEmpty() && cornerDiff > 15 && fxDiff > 30,
                  QStringLiteral("a video file as the layer shows in the bottom-right corner (changed by %1), with its own FX (invert: %2)%3")
                      .arg(cornerDiff, 0, 'f', 0).arg(fxDiff, 0, 'f', 0).arg(layerErr.isEmpty() ? QString() : QStringLiteral(": ") + layerErr));
            m_engine->setLayer(LiveSource());
            Timeline tl = m_project->timeline();
            tl.liveLayerEffects.clear();
            m_project->applyEdit(tl, QStringLiteral("Test layer FX"));
            settle(5);
        } else {
            log << QStringLiteral("SKIP  the layer: testmedia/h264_aac.mp4 is needed");
        }
        // The switcher: the test video as input 2; cut to it (it fills the output) and back.
        if (QFileInfo::exists(video)) {
            LiveSource file;
            file.kind = LiveSource::Kind::File;
            file.name = QStringLiteral("test video");
            file.id = video;
            QList<QImage> thumbs;
            connect(m_engine, &LiveEngine::inputFrames, &featCtx, [&](const QList<QImage>& t) { thumbs = t; });
            m_engine->setInputs({file});
            wait(4000, [&] { return thumbs.size() == 2 && !thumbs[1].isNull(); });
            settle(5);
            plain = last.copy();
            setPreviewInput(1);
            cutTo(m_preview, false);
            settle(8);
            const QImage onTwo = last.copy();
            const double cut = diffIn(plain, onTwo, onTwo.rect());
            const bool swapped = m_program == 1 && m_preview == 0; // what was on air waits in preview
            cutTo(0, false);
            settle(8);
            const double back = diffIn(plain, last, last.rect());
            check(thumbs.size() == 2 && !thumbs[1].isNull() && cut > 15 && back < cut / 2 && swapped && last.size() == plain.size(),
                  QStringLiteral("the switcher: a second input shows in its tile, Cut puts it on air at the output's size (changed by %1), "
                                 "and back again (%2)").arg(cut, 0, 'f', 0).arg(back, 0, 'f', 0));
            m_engine->setInputs({});
            settle(3);
        }
        // Overlays: big text in the middle.
        plain = last.copy();
        {
            Timeline tl = m_project->timeline();
            LiveOverlay o;
            o.kind = LiveOverlay::Kind::Text;
            o.text = QStringLiteral("MVP");
            o.anchor = 4;
            o.size = 25;
            tl.liveOverlays = {o};
            m_project->applyEdit(tl, QStringLiteral("Test overlay"));
        }
        settle(5);
        const QRect centre(last.width() / 4, last.height() * 3 / 8, last.width() / 2, last.height() / 4);
        const double overlayDiff = diffIn(plain, last, centre);
        {
            Timeline tl = m_project->timeline();
            tl.liveOverlays.clear();
            m_project->applyEdit(tl, QStringLiteral("Test overlay"));
        }
        check(overlayDiff > 10, QStringLiteral("a text overlay is drawn over the output (changes the middle by %1)").arg(overlayDiff, 0, 'f', 0));
        // The replay buffer: on for a few seconds, then saved as a clip.
        QString replayPath;
        bool replayOk = false;
        double replaySecs = 0;
        connect(m_engine, &LiveEngine::replaySaved, &featCtx, [&](const QString& p, bool ok, const QString&, double s) {
            replayPath = p;
            replayOk = ok;
            replaySecs = s;
        });
        m_replayLength->setCurrentIndex(0); // 15 s
        m_replay->setChecked(true);
        wait(3000, [&] { return false; });
        saveReplay();
        wait(5000, [&] { return !replayPath.isEmpty(); });
        m_replay->setChecked(false);
        check(replayOk && QFileInfo::exists(replayPath) && replaySecs >= 1.5,
              QStringLiteral("the replay buffer saves the last seconds as a clip (%1 s, %2)").arg(replaySecs, 0, 'f', 1).arg(QFileInfo(replayPath).fileName()));
        // Screenshot: the big display's picture as a PNG, at its own size.
        m_lastScreenshot.clear();
        const QImage onDisplay = m_display->image();
        m_screenshot->click();
        const QImage saved(m_lastScreenshot);
        check(!onDisplay.isNull() && !saved.isNull() && saved.size() == onDisplay.size() && m_lastScreenshot.endsWith(QLatin1String(".png")),
              QStringLiteral("Screenshot saves the big display's picture (%1 x %2, %3)").arg(saved.width()).arg(saved.height()).arg(QFileInfo(m_lastScreenshot).fileName()));
        // ... and it shows up in the Editor's From Live tab, as a still.
        auto listed = [&] {
            if (auto* clips = window()->findChild<LiveClipsBrowser*>())
                for (int i = 0; i < clips->count(); ++i)
                    if (clips->item(i)->data(Qt::UserRole).toString() == QFileInfo(m_lastScreenshot).absoluteFilePath()
                        && clips->item(i)->text().contains(QLatin1String("Still")))
                        return true;
            return false;
        };
        wait(5000, listed);
        check(listed(), QStringLiteral("the screenshot appears in the Editor's From Live tab as a still"));
        {
            Timeline tl = m_project->timeline();
            tl.livePads[1] = LivePad();
            tl.livePads[2] = LivePad();
            m_project->applyEdit(tl, QStringLiteral("Test pads"));
        }
    }
    // ---- Launch on the bar, MIDI clock, a spectrum overlay, scenes ----
    {
        QObject featCtx;
        if (m_activePad >= 0)
            triggerPad(m_activePad);
        m_pads->transitionBox()->setCurrentIndex(0); // cut
        {
            Timeline tl = m_project->timeline();
            tl.livePads[2] = LivePad();
            tl.livePads[2].effects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
            tl.liveEffects.clear();
            tl.liveOverlays.clear();
            m_project->applyEdit(tl, QStringLiteral("Test pads"));
        }
        // Launch on the next bar: pad 3 waits for a 1 of the beat clock (Auto off, 240 BPM: a bar a second).
        m_pads->setAutoMode(LivePadPanel::AutoOff);
        m_pads->bpmBox()->setValue(240);
        m_pads->launchBox()->setCurrentIndex(LivePadPanel::LaunchBar);
        int launchedOn = -1;
        connect(m_pads, &LivePadPanel::padPressed, &featCtx, [&](int pad) {
            if (pad == 2 && launchedOn < 0)
                launchedOn = m_pads->barBeat() - 1; // where the beat clock is as it launches
        });
        runAction(QStringLiteral("pad:2"), true);
        const bool waiting = m_activePad == -1 && m_pads->pendingPad() == 2;
        wait(2500, [&] { return m_activePad == 2; });
        check(waiting && m_activePad == 2 && launchedOn == 0,
              QStringLiteral("Launch on the next bar: a pressed pad waits (dashed) and switches on the 1 (beat %1 of the bar; waited %2, active %3, pending %4)")
                  .arg(launchedOn + 1).arg(waiting).arg(m_activePad).arg(m_pads->pendingPad()));
        m_pads->launchBox()->setCurrentIndex(LivePadPanel::LaunchNow);
        triggerPad(2);
        // MIDI clock at 100 BPM drives the beat clock (Auto: MIDI clock), the bar counted from Start.
        m_pads->setAutoMode(LivePadPanel::AutoMidi);
        std::vector<int> bars;
        connect(m_pads, &LivePadPanel::beatTicked, &featCtx, [&](double, int barBeat) { bars.push_back(barBeat); });
        MidiMessage clockMsg;
        clockMsg.status = 0xFA;
        midiMessage(clockMsg);
        QElapsedTimer clockTimer;
        clockTimer.start();
        const double tick = 60.0 / 100.0 / 24.0;
        for (int i = 0; i < 24 * 6; ++i) { // six beats of ticks, in real time
            while (clockTimer.nsecsElapsed() / 1e9 < i * tick)
                wait(1, [] { return true; });
            clockMsg.status = 0xF8;
            clockMsg.time = clockTimer.nsecsElapsed() / 1e9;
            midiMessage(clockMsg);
        }
        const std::vector<int> expected{0, 1, 2, 3, 0, 1};
        check(bars == expected && std::abs(m_pads->bpmBox()->value() - 100) < 2,
              QStringLiteral("MIDI clock drives the beats (%1 beats from the 1) and shows its tempo (%2 BPM)").arg(bars.size()).arg(m_pads->bpmBox()->value(), 0, 'f', 2));
        m_pads->setAutoMode(LivePadPanel::AutoOff);
        // A spectrum over the bottom of the picture, moving with the audio input.
        const int beatSource = m_audio->findText(QStringLiteral("Test beat"));
        if (beatSource >= 0) {
            m_audio->setCurrentIndex(beatSource);
            wait(1500, [] { return false; });
            plain = last.copy();
            {
                Timeline tl = m_project->timeline();
                LiveOverlay o;
                o.color = 0xff00ff; // magenta stands out on light and dark screens alike (white bars vanished on a white page)
                o.kind = LiveOverlay::Kind::Spectrum;
                o.anchor = 7;
                o.size = 30;
                o.width = 80;
                tl.liveOverlays = {o};
                m_project->applyEdit(tl, QStringLiteral("Test spectrum"));
            }
            double most = 0;
            QImage previous;
            double moving = 0;
            wait(2000, [&] {
                const QRect bottom(last.width() / 10, last.height() * 6 / 10, last.width() * 8 / 10, last.height() * 35 / 100);
                if (plain.size() == last.size()) {
                    int64_t sum = 0, n = 0, sumMove = 0;
                    for (int y = bottom.top(); y < bottom.bottom(); y += 6)
                        for (int x = bottom.left(); x < bottom.right(); x += 6, ++n) {
                            sum += std::abs(qGray(last.pixel(x, y)) - qGray(plain.pixel(x, y)));
                            if (previous.size() == last.size())
                                sumMove += std::abs(qGray(last.pixel(x, y)) - qGray(previous.pixel(x, y)));
                        }
                    most = std::max(most, n ? double(sum) / n : 0.0);
                    moving = std::max(moving, n ? double(sumMove) / n : 0.0);
                }
                previous = last.copy();
                return false;
            });
            {
                Timeline tl = m_project->timeline();
                tl.liveOverlays.clear();
                m_project->applyEdit(tl, QStringLiteral("Test spectrum"));
            }
            m_audio->setCurrentIndex(0);
            check(most > 8 && moving > 2, QStringLiteral("a spectrum overlay draws the audio input and moves with it (%1 drawn, %2 moving)").arg(most, 0, 'f', 1).arg(moving, 0, 'f', 1));
        } else {
            log << QStringLiteral("SKIP  spectrum overlay: no test audio source");
        }
        // Scenes: save a look (Live FX invert, pad 3 on), change everything, recall it.
        {
            Timeline tl = m_project->timeline();
            tl.liveEffects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
            m_project->applyEdit(tl, QStringLiteral("Test scene"));
        }
        if (m_activePad != 2)
            triggerPad(2);
        const LiveScenes scenesBefore = m_project->timeline().liveScenes;
        saveScene(0);
        const bool saved = m_project->timeline().liveScenes[0].used && m_project->timeline().liveScenes[0].pad == 2;
        triggerPad(2); // off
        {
            Timeline tl = m_project->timeline();
            tl.liveEffects.clear();
            m_project->applyEdit(tl, QStringLiteral("Test scene"));
        }
        runAction(QStringLiteral("scene:0"), true); // (F1 / MIDI play the same action)
        check(saved && m_activePad == 2 && m_project->timeline().liveEffects.size() == 1 && m_activeScene == 0,
              QStringLiteral("a scene saves the look and brings it back (Live FX and the active pad)"));
        triggerPad(2);
        Timeline tl = m_project->timeline();
        tl.liveScenes = scenesBefore;
        tl.liveEffects.clear();
        tl.livePads[2] = LivePad();
        m_project->applyEdit(tl, QStringLiteral("Test scene"));
    }
    // ---- Setlist: a song loads its pad set, sets its tempo and recalls its scene; the next and back ----
    {
        const Timeline before = m_project->timeline();
        padsets::remove(QStringLiteral("Test Setlist Set"));
        LivePadSet set;
        set.name = QStringLiteral("Test Setlist Set");
        set.pads[0].name = QStringLiteral("From the set");
        padsets::save(set);
        Timeline tl = m_project->timeline();
        tl.liveSetlist.assign(2, LiveSong());
        tl.liveSetlist[0].name = QStringLiteral("One");
        tl.liveSetlist[0].padSet = set.name;
        tl.liveSetlist[0].bpm = 128;
        tl.liveSetlist[0].notes = QStringLiteral("big chorus");
        tl.liveSetlist[1].name = QStringLiteral("Two");
        m_project->applyEdit(tl, QStringLiteral("Test setlist"));
        m_currentSong = -1;
        const bool listed = m_song->count() == 2 && m_song->itemText(0).contains(QStringLiteral("One"));
        runAction(QStringLiteral("song:next"), true); // (Page Down / MIDI play the same action)
        const bool first = m_currentSong == 0 && m_project->timeline().livePads[0].name == QStringLiteral("From the set")
                           && std::abs(m_pads->bpm() - 128) < 0.01 && m_pads->lockBpmButton()->isChecked() && m_song->currentIndex() == 0
                           && m_songNotes->text() == QStringLiteral("big chorus");
        runAction(QStringLiteral("song:next"), true);
        const bool second = m_currentSong == 1 && !m_pads->lockBpmButton()->isChecked()
                            && m_project->timeline().livePads[0].name == QStringLiteral("From the set") && m_songNotes->text().isEmpty();
        runAction(QStringLiteral("song:next"), true); // the last: stays
        const bool stays = m_currentSong == 1;
        runAction(QStringLiteral("song:prev"), true);
        check(listed && first && second && stays && m_currentSong == 0,
              QStringLiteral("setlist: song 1 loads its pad set, locks 128 BPM and shows its notes; Next goes to song 2 (listening "
                             "for the tempo, keeping the pads), stops at the last; Previous goes back"));
        SetlistDialog dlg(m_project->timeline().liveSetlist, [] {
            LiveSong s;
            s.name = QStringLiteral("Now");
            s.bpm = 99;
            return s;
        });
        const bool same = dlg.songs() == m_project->timeline().liveSetlist;
        dlg.addSong(LiveSong());
        check(same && dlg.songs().size() == 3, QStringLiteral("Edit Setlist shows the songs as they are and adds more"));
        m_project->applyEdit(before, QStringLiteral("Test setlist"));
        padsets::remove(set.name);
    }
    // ---- Send to: Spout and the virtual camera share the output while capturing ----
    auto meanDiff = [](const QImage& a, const QImage& b) {
        if (a.isNull() || a.size() != b.size())
            return 999.0;
        int64_t sum = 0, n = 0;
        for (int y = 0; y < a.height(); y += 9)
            for (int x = 0; x < a.width(); x += 9, ++n)
                sum += std::abs(qGray(a.pixel(x, y)) - qGray(b.pixel(x, y)));
        return n ? double(sum) / double(n) : 999.0;
    };
    // ---- Follow: switching a pad on shows its effects; switching it off, the Live FX again ----
    {
        editChain(0);
        if (m_activePad >= 0)
            triggerPad(m_activePad);
        triggerPad(4);
        const bool shown = m_pads->editing() == 5 && m_devices->target().id == 5;
        triggerPad(4);
        const bool back = m_pads->editing() == 0;
        triggerPad(4);
        editChain(LivePadPanel::kLayerTarget); // looking at something else when it goes off: left alone
        triggerPad(4);
        const bool kept = m_pads->editing() == LivePadPanel::kLayerTarget;
        editChain(0);
        auto* follow = m_pads->findChild<QCheckBox*>(QStringLiteral("livePadFollow"));
        follow->setChecked(false);
        triggerPad(6);
        const bool stays = m_pads->editing() == 0;
        triggerPad(6);
        follow->setChecked(true);
        check(shown && back && kept && stays,
              QStringLiteral("Follow: switching pad 5 on shows its effects, off shows the Live FX again (unless something else "
                             "was opened meanwhile); with Follow off the chain stays put"));
        editChain(0);
    }
    // ---- Layouts: the same controls in each, the picture keeps coming, a laptop screen is enough ----
    {
        QStringList problems;
        const QList<QWidget*> controls{m_start, m_rec, m_goLive, m_saveReplay, m_popOutButton, m_source, m_audio, m_size, m_fps, m_mirror,
                                       m_layer, m_layerMode, m_layerBlend, m_recMode, m_replay, m_spoutBox, m_cameraBox, m_midi, m_song,
                                       m_songPrev, m_songNext, m_sceneButtons[0], m_sceneButtons[7], m_devices, m_browser, m_pads,
                                       m_pads->padButton(15), m_display};
        for (int l : {1, 2, 0}) {
            setLayoutMode(l);
            const int before = m_frames;
            settle(4);
            if (m_frames < before + 4)
                problems << QStringLiteral("layout %1: no pictures").arg(l);
            for (QWidget* w : controls)
                if (!isAncestorOf(w))
                    problems << QStringLiteral("layout %1: %2 is missing").arg(l).arg(w->objectName().isEmpty() ? QString::fromLatin1(w->metaObject()->className()) : w->objectName());
            if (!m_display->isVisibleTo(this) || !m_strip->isVisibleTo(this) || !m_showBar->isVisibleTo(this))
                problems << QStringLiteral("layout %1: the picture, strip or show bar is hidden").arg(l);
            const QSize least = window()->minimumSizeHint();
            if (least.width() > 1366 || least.height() > 768)
                problems << QStringLiteral("layout %1 needs %2x%3").arg(l).arg(least.width()).arg(least.height());
        }
        // Splitter positions are kept per layout.
        setLayoutMode(1);
        QSplitter* deck = nullptr;
        for (const auto& [name, sp] : m_frameSplitters)
            if (name == QLatin1String("deck"))
                deck = sp;
        if (deck)
            deck->setSizes({500, 900});
        const int padsWidth = deck ? deck->sizes().value(0) : 0;
        setLayoutMode(0);
        setLayoutMode(1);
        for (const auto& [name, sp] : m_frameSplitters)
            if (name == QLatin1String("deck") && std::abs(sp->sizes().value(0) - padsWidth) > 4)
                problems << QStringLiteral("the Stage deck forgot its divider (%1, was %2)").arg(sp->sizes().value(0)).arg(padsWidth);
        setLayoutMode(0);
        check(problems.isEmpty() && layoutMode() == 0 && QSettings().value(QStringLiteral("live/layout")).toInt() == 0,
              QStringLiteral("layouts Sidebar, Stage and Compact: all %1 controls there, the picture keeps coming, each fits 1366x768, "
                             "dividers remembered per layout%2")
                  .arg(controls.size()).arg(problems.isEmpty() ? QString() : QStringLiteral(": ") + problems.join(QStringLiteral("; "))));
    }
    // ---- Pan and zoom: scroll and drag on the picture frame the source in the output ----
    {
        const int sizeBefore = m_size->currentIndex();
        for (int i = 0; i < m_size->count(); ++i)
            if (m_size->itemData(i).toSize() == QSize(1080, 1920) && m_size->itemData(i, Qt::UserRole + 1).toBool())
                m_size->setCurrentIndex(i);
        m_framing->setCurrentIndex(0); // crop to fill
        setPanZoom({});
        settle(6);
        const QImage framedBefore = last.copy();
        const QRectF pic = m_display->pictureRect();
        const QPointF middle = pic.center();
        // Two notches of the wheel at the middle: 1.15^2 in.
        QWheelEvent wheel(middle, m_display->mapToGlobal(middle), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_display, &wheel);
        const double zoomed = m_panZoom.zoom;
        // Drag a quarter of the picture's width to the right: the source moves right (its centre goes left).
        auto mouse = [&](QEvent::Type type, QPointF pos) {
            QMouseEvent ev(type, pos, m_display->mapToGlobal(pos), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), Qt::NoModifier);
            QApplication::sendEvent(m_display, &ev);
        };
        const double centerBefore = m_panZoom.center.x();
        mouse(QEvent::MouseButtonPress, middle);
        mouse(QEvent::MouseMove, middle + QPointF(pic.width() / 8, 0));
        mouse(QEvent::MouseMove, middle + QPointF(pic.width() / 4, 0));
        mouse(QEvent::MouseButtonRelease, middle + QPointF(pic.width() / 4, 0));
        const double centerAfter = m_panZoom.center.x();
        settle(6);
        const QImage framedAfter = last.copy();
        double change = 0;
        if (framedAfter.size() == framedBefore.size()) {
            int64_t sum = 0, n = 0;
            for (int y = 0; y < framedAfter.height(); y += 9)
                for (int x = 0; x < framedAfter.width(); x += 9, ++n)
                    sum += std::abs(qGray(framedAfter.pixel(x, y)) - qGray(framedBefore.pixel(x, y)));
            change = n ? double(sum) / double(n) : 0;
        }
        const bool engineHasIt = m_engine->panZoom() == m_panZoom;
        const bool label = m_panZoomLabel->text().contains(QStringLiteral("132%"));
        // Reset from Setup > Input.
        for (QToolButton* b : m_sidebar->findChildren<QToolButton*>())
            if (b->text() == tr("Reset"))
                b->click();
        const bool reset = m_panZoom.isDefault() && m_engine->panZoom().isDefault();
        m_size->setCurrentIndex(sizeBefore);
        settle(4);
        check(std::abs(zoomed - 1.3225) < 0.01 && centerAfter < centerBefore - 0.01 && framedAfter.size() == QSize(1080, 1920) && change > 3
                  && engineHasIt && label && reset,
              QStringLiteral("pan and zoom on the picture: two wheel notches zoom to %1%, a drag moves the source (centre %2 -> %3), "
                             "the portrait output changes (%4) but keeps its size; Reset puts it back")
                  .arg(int(std::lround(zoomed * 100))).arg(centerBefore, 0, 'f', 3).arg(centerAfter, 0, 'f', 3).arg(change, 0, 'f', 1));
    }
    // ---- Portrait sizes: the output becomes that shape, framed as chosen ----
    {
        const int before = m_size->currentIndex();
        auto sizeItem = [&](QSize s) {
            for (int i = 0; i < m_size->count(); ++i)
                if (m_size->itemData(i).toSize() == s && m_size->itemData(i, Qt::UserRole + 1).toBool())
                    return i;
            return -1;
        };
        auto shapeAfter = [&](int item, int framing) {
            m_framing->setCurrentIndex(framing);
            m_size->setCurrentIndex(item);
            settle(6);
            return last.size();
        };
        const QSize crop = shapeAfter(sizeItem(QSize(1080, 1920)), 0);
        const QSize fit = shapeAfter(sizeItem(QSize(720, 1280)), 1);
        // The screen is wider than tall: fitted, the rows at the top are black bars.
        bool bars = true;
        for (int x = 0; x < last.width(); x += 16)
            bars = bars && qGray(last.pixel(x, 4)) < 8;
        const QSize blur = shapeAfter(sizeItem(QSize(1080, 1350)), 2);
        const QSize square = shapeAfter(sizeItem(QSize(1080, 1080)), 0);
        const bool framingShown = m_framing->isVisibleTo(this);
        // 720p stays 1280x720 whatever the source's shape; only Source size follows the source.
        const QSize hd = shapeAfter(sizeItem(QSize(1280, 720)), 1);
        const bool framingAt720 = m_framing->isVisibleTo(this);
        m_size->setCurrentIndex(0);
        settle(4);
        const bool noFramingAtSource = !m_framing->isVisibleTo(this);
        m_size->setCurrentIndex(before);
        settle(4);
        check(crop == QSize(1080, 1920) && fit == QSize(720, 1280) && bars && blur == QSize(1080, 1350) && square == QSize(1080, 1080)
                  && framingShown && hd == QSize(1280, 720) && framingAt720 && noFramingAtSource && last.width() > last.height(),
              QStringLiteral("portrait sizes: the output becomes 1080x1920 (cropped), 720x1280 (fitted, black bars), 1080x1350 "
                             "(blurred background) and 1080x1080; 720p is a fixed 1280x720 (%3x%4) with Framing, Source size has none; "
                             "back to %1x%2 after").arg(last.width()).arg(last.height()).arg(hd.width()).arg(hd.height()));
    }
    // ---- Cue: a pad previewed beside the output, not live; Take puts it live ----
    {
        const Timeline before = m_project->timeline();
        Timeline tl = m_project->timeline();
        tl.livePads[5] = LivePad();
        tl.livePads[5].name = QStringLiteral("Cue test");
        tl.livePads[5].effects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
        m_project->applyEdit(tl, QStringLiteral("Test cue"));
        if (m_activePad >= 0)
            triggerPad(m_activePad); // nothing live but the Live FX
        settle(3);
        emit m_pads->padCued(5); // (Shift+click, or right-click > Cue)
        m_cueFrames = 0;
        wait(4000, [&] { return m_cueFrames >= 4; });
        settle(2);
        const QImage cue = m_cueView->image();
        const QImage outNow = last;
        // The cue is the output inverted (smaller); the output itself unchanged.
        double inverted = 999;
        if (!cue.isNull() && !outNow.isNull()) {
            const QImage small = outNow.scaled(cue.size());
            int64_t sum = 0, n = 0;
            for (int y = 0; y < cue.height(); y += 5)
                for (int x = 0; x < cue.width(); x += 5, ++n)
                    sum += std::abs(qGray(cue.pixel(x, y)) - (255 - qGray(small.pixel(x, y))));
            inverted = n ? double(sum) / double(n) : 999;
        }
        const bool notLive = m_activePad == -1 && m_pads->cuedPad() == 5 && m_cuePanel->isVisible();
        check(m_cueFrames >= 4 && inverted < 25 && notLive,
              QStringLiteral("Cue: Shift+click pad 6 previews it beside the output (%1 pictures, %2 from the output inverted) "
                             "while the output stays as it is")
                  .arg(m_cueFrames).arg(inverted, 0, 'f', 1));
        runAction(QStringLiteral("take"), true); // (the Take button / MIDI)
        settle(4);
        check(m_activePad == 5 && !m_cuePanel->isVisible() && m_pads->cuedPad() == -1,
              QStringLiteral("Take puts the cued pad live and closes the cue"));
        triggerPad(5);
        m_project->applyEdit(before, QStringLiteral("Test cue"));
    }
    // ---- Go Live: streaming to a server (a local one here: FFmpeg listening) ----
    {
        const QString ffmpegExe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../../../deps/ffmpeg/bin/ffmpeg.exe"));
        if (QFileInfo::exists(ffmpegExe)) {
            const QString received = QDir(QFileInfo(QString::fromLocal8Bit(qgetenv("MVP_LIVE_DIR"))).absolutePath()).filePath(QStringLiteral("stream_test.flv"));
            QFile::remove(received);
            QProcess server;
            server.start(ffmpegExe, {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
                                     QStringLiteral("-listen"), QStringLiteral("1"), QStringLiteral("-i"),
                                     QStringLiteral("rtmp://127.0.0.1:19358/live/test"), QStringLiteral("-c"), QStringLiteral("copy"), received});
            server.waitForStarted(5000);
            wait(1200, [] { return false; });
            QSettings s;
            s.setValue(QStringLiteral("stream/service"), 4); // Custom
            s.setValue(QStringLiteral("stream/server"), QStringLiteral("rtmp://127.0.0.1:19358/live/test"));
            s.remove(QStringLiteral("stream/key"));
            s.setValue(QStringLiteral("stream/kbps"), 3000);
            toggleStream();
            const bool live = wait(10000, [&] { return m_streamLive; });
            const bool label = m_streamStatus->text().contains(QStringLiteral("LIVE"));
            wait(3000, [] { return false; });
            toggleStream(); // End Stream
            const bool ended = wait(5000, [&] { return !m_engine->isStreaming() && !m_streamLive; });
            server.waitForFinished(8000);
            if (server.state() != QProcess::NotRunning)
                server.kill();
            int pictures = 0;
            ff::InputFormatPtr in;
            if (QFileInfo(received).size() > 0 && ff::openInput(received, in) >= 0) {
                ff::PacketPtr pkt = ff::makePacket();
                while (av_read_frame(in.get(), pkt.get()) >= 0) {
                    pictures += in->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
                    av_packet_unref(pkt.get());
                }
            }
            in.reset();
            QFile::remove(received); // (it is a capture of the screen)
            check(live && label && ended && pictures > 50 && m_goLive->text() == tr("Go Live"),
                  QStringLiteral("Go Live streams the output to the server (%1 pictures arrived, \"LIVE\" shown); End Stream stops it")
                      .arg(pictures));
        }
    }
    if (m_spoutBox->isEnabled()) {
        m_spoutBox->setChecked(true);
        wait(4000, [&] { return !m_spoutSender.isEmpty(); });
        const QString sender = m_spoutSender;
        settle(3);
        const QImage got = spout::receive(sender);
        const double diff = meanDiff(got, last);
        m_spoutBox->setChecked(false);
        wait(3000, [&] { return m_spoutSender.isEmpty(); });
        check(!sender.isEmpty() && diff < 15 && !spout::senders().contains(sender) && m_spoutSender.isEmpty(),
              QStringLiteral("Send to Spout: \"%1\" carries the output (%2x%3, %4 from the picture shown); off takes it away")
                  .arg(sender).arg(got.width()).arg(got.height()).arg(diff, 0, 'f', 1));
    }
    if (m_cameraBox->isEnabled()) {
        m_cameraBox->setChecked(true);
        wait(4000, [&] { return m_cameraSize.isValid(); });
        const QSize size = m_cameraSize;
        m_cameraBox->setChecked(false);
        wait(3000, [&] { return !m_cameraSize.isValid(); });
        check(size == QSize(last.width() & ~1, last.height() & ~1) && !m_cameraSize.isValid() && m_shareStatus->text().isEmpty(),
              QStringLiteral("Send to the virtual camera: it gets the output at %1x%2; off stops it").arg(size.width()).arg(size.height()));
    }
    m_engine->stop();
    check(!m_engine->isRunning(), QStringLiteral("capture stops"));
    m_project->undoStack()->undo();
    return failures;
}
