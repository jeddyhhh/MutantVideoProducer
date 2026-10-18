#include "core/Captions.h"
#include <cstdio>

#include "core/AudioMixer.h"
#include "core/Project.h"

#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>

#ifdef MVP_WHISPER
#include <whisper.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

// "01:02:03,456" / "02:03.456" (WebVTT may leave the hours out). Up to 99
// hours: more is a damaged file (and would overflow).
bool parseTime(const QString& s, TimeUs& t)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(?:(\\d{1,2}):)?(\\d{1,2}):(\\d{1,2})[,.](\\d{1,3})"));
    const auto m = re.match(s);
    if (!m.hasMatch())
        return false;
    const QString ms = m.captured(4).leftJustified(3, QLatin1Char('0'));
    t = ((m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 60 + m.captured(3).toLongLong()) * kUsPerSecond
        + ms.toLongLong() * 1000;
    return true;
}

QString formatTime(TimeUs t, QChar decimal)
{
    const int64_t ms = std::max<int64_t>(0, t / 1000);
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(decimal)
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

std::vector<Caption> timelineCaptions(const Timeline& tl, int clipId)
{
    auto isText = [](const Clip& c) { return c.generator.type == QLatin1String("text") || c.generator.type == QLatin1String("kinetic_text"); };
    const Track* chosen = nullptr;
    for (const Track& t : tl.tracks)
        for (const Clip& c : t.clips)
            if (!chosen && c.id == clipId && isText(c))
                chosen = &t;
    for (const Track& t : tl.tracks)
        if (!chosen && std::any_of(t.clips.begin(), t.clips.end(), isText))
            chosen = &t;
    std::vector<Caption> captions;
    if (chosen)
        for (const Clip& c : chosen->clips)
            if (isText(c))
                if (auto it = c.generator.params.find(QStringLiteral("text")); it != c.generator.params.end() && !it->second.text.isEmpty())
                    captions.push_back({c.start, c.end(), it->second.text,
                        c.generator.params.contains(QStringLiteral("caption_speaker"))
                            ? int(c.generator.params.at(QStringLiteral("caption_speaker")).value) : 0});
    return captions;
}

std::vector<Caption> parseSubtitles(const QString& text)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>|\\{\\\\[^}]*\\}"));
    std::vector<Caption> out;
    QString all = text;
    all.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (all.startsWith(QChar(0xfeff)))
        all.remove(0, 1);
    for (const QString& block : all.split(QRegularExpression(QStringLiteral("\n\\s*\n")), Qt::SkipEmptyParts)) {
        const QStringList lines = block.split(QLatin1Char('\n'));
        const int arrow = int(std::find_if(lines.begin(), lines.end(), [](const QString& l) { return l.contains(QLatin1String("-->")); }) - lines.begin());
        if (arrow >= lines.size())
            continue; // the WEBVTT header, NOTE / STYLE blocks
        Caption c;
        const QStringList times = lines[arrow].split(QStringLiteral("-->"));
        if (!parseTime(times[0], c.start) || !parseTime(times.value(1), c.end) || c.end <= c.start)
            continue;
        QStringList body;
        for (int i = arrow + 1; i < lines.size(); ++i)
            body << QString(lines[i]).remove(tags).trimmed();
        c.text = body.join(QLatin1Char('\n')).trimmed();
        if (!c.text.isEmpty())
            out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    return out;
}

QString writeSrt(const std::vector<Caption>& captions)
{
    QString s;
    int n = 0;
    for (const Caption& c : captions)
        s += QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(++n).arg(formatTime(c.start, QLatin1Char(',')), formatTime(c.end, QLatin1Char(',')), c.text);
    return s;
}

QString writeVtt(const std::vector<Caption>& captions)
{
    QString s = QStringLiteral("WEBVTT\n\n");
    for (const Caption& c : captions)
        s += QStringLiteral("%1 --> %2\n%3\n\n").arg(formatTime(c.start, QLatin1Char('.')), formatTime(c.end, QLatin1Char('.')), c.text);
    return s;
}

std::vector<float> timelineSpeechAudio(const ProjectSnapshot& snap, TimeUs from, TimeUs to)
{
    // Nothing to hear after the last audio event; at most kMaxSpeech at a time
    // (held in memory: 2 hours is 460 MB).
    constexpr TimeUs kMaxSpeech = 2 * 3600 * kUsPerSecond;
    to = std::min({to, snap.timeline.soundEnd(), from + kMaxSpeech});
    constexpr int kRate = 16000, kChunk = 16000;
    AudioMixer mixer(kRate);
    std::vector<float> mono, stereo(size_t(kChunk) * 2);
    const int64_t first = usToSamples(from, kRate), last = usToSamples(to, kRate);
    mono.reserve(size_t(std::max<int64_t>(0, last - first)));
    for (int64_t pos = first; pos < last; pos += kChunk) {
        const int n = int(std::min<int64_t>(kChunk, last - pos));
        mixer.mix(snap, pos, n, stereo.data());
        for (int i = 0; i < n; ++i)
            mono.push_back(0.5f * (stereo[size_t(i) * 2] + stereo[size_t(i) * 2 + 1]));
    }
    return mono;
}

void splitLongCaptions(std::vector<Caption>& out, int maxChars)
{
    std::vector<Caption> cut;
    for (const Caption& c : out) {
        const QStringList words = c.text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QStringList lines{QString()};
        for (const QString& w : words) {
            if (!lines.back().isEmpty() && lines.back().size() + 1 + w.size() > std::max(8, maxChars))
                lines.append(QString());
            lines.back() += (lines.back().isEmpty() ? QString() : QStringLiteral(" ")) + w;
        }
        const double total = std::max<qsizetype>(1, c.text.size());
        qsizetype done = 0;
        for (const QString& line : lines) { // (timed by its share of the characters)
            Caption piece = c;
            piece.text = line;
            piece.start = c.start + TimeUs((c.end - c.start) * (done / total));
            done += line.size() + 1;
            piece.end = &line == &lines.back() ? c.end : c.start + TimeUs((c.end - c.start) * (std::min<double>(done, total) / total));
            cut.push_back(piece);
        }
    }
    out = std::move(cut);
}

#ifdef MVP_WHISPER

bool whisperAvailable()
{
    return true;
}

bool whisperOnGpu()
{
#if defined(MVP_WHISPER_VULKAN) && defined(_WIN32)
    // A Vulkan driver to load (vulkan-1.dll is delay-loaded), and not turned off (MVP_WHISPER_CPU).
    static const bool vulkan = LoadLibraryW(L"vulkan-1.dll") != nullptr;
    return vulkan && qEnvironmentVariableIsEmpty("MVP_WHISPER_CPU");
#else
    return false;
#endif
}

bool transcribe(const QString& modelPath, const std::vector<float>& mono16k, const TranscribeOptions& options,
                const std::function<bool(double)>& progress, std::vector<Caption>& out, QString* error)
{
    out.clear();
    whisper_log_set([](ggml_log_level, const char*, void*) {}, nullptr); // quiet
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = whisperOnGpu();
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), cp);
    if (!ctx && cp.use_gpu) { // the GPU couldn't take it: the CPU
        cp.use_gpu = false;
        ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), cp);
    }
    if (!ctx) {
        if (error)
            *error = QStringLiteral("could not load the speech model %1").arg(modelPath);
        return false;
    }
    struct Callbacks {
        const std::function<bool(double)>* progress;
        bool cancelled = false;
    } cb{&progress};
    whisper_full_params p = whisper_full_default_params(options.speakerTurns ? WHISPER_SAMPLING_GREEDY : WHISPER_SAMPLING_BEAM_SEARCH);
    const QByteArray lang = options.language.toUtf8();
    p.language = lang.constData();
    p.detect_language = false;
    p.n_threads = std::max(1, QThread::idealThreadCount() - 1);
    p.print_progress = p.print_realtime = p.print_timestamps = p.print_special = false;
    p.token_timestamps = true; // short captions: split by length, on words
    p.max_len = options.wordTimestamps ? 0 : std::max(8, options.maxChars);
    const QByteArray prompt = options.initialPrompt.toUtf8();
    if (!prompt.isEmpty()) p.initial_prompt = prompt.constData();
    p.translate = options.translate;
    p.tdrz_enable = options.speakerTurns;
    if (options.speakerTurns) { // (splitting by length loses the turn marks)
        p.token_timestamps = false;
        p.max_len = 0;
        p.split_on_word = false;
    }
    p.split_on_word = true;
    p.progress_callback = [](whisper_context*, whisper_state*, int percent, void* user) {
        auto* c = static_cast<Callbacks*>(user);
        if (*c->progress && !(*c->progress)(percent / 100.0))
            c->cancelled = true;
    };
    p.progress_callback_user_data = &cb;
    p.abort_callback = [](void* user) { return static_cast<Callbacks*>(user)->cancelled; };
    p.abort_callback_user_data = &cb;
    const int rc = whisper_full(ctx, p, mono16k.data(), int(mono16k.size()));
    int speaker = 0;
    if (rc == 0 && !cb.cancelled)
        for (int i = 0; i < whisper_full_n_segments(ctx); ++i) {
            if (options.wordTimestamps) {
                Caption word;
                auto flush = [&] { if (!word.text.trimmed().isEmpty() && word.end > word.start) { word.text=word.text.trimmed(); out.push_back(word); } word={}; };
                for(int j=0;j<whisper_full_n_tokens(ctx,i);++j) {
                    const auto data=whisper_full_get_token_data(ctx,i,j);
                    if(qEnvironmentVariableIsSet("MVP_ALIGNMENT_DEBUG"))std::fprintf(stderr,"TOKEN %lld %lld %s\n",static_cast<long long>(data.t0),static_cast<long long>(data.t1),whisper_full_get_token_text(ctx,i,j));
                    if(data.id>=whisper_token_eot(ctx) || data.t0<0) continue;
                    const QString text=QString::fromUtf8(whisper_full_get_token_text(ctx,i,j));
                    if(text.isEmpty())continue;
                    if(text.front().isSpace()&&!word.text.isEmpty()){if(word.end<=word.start)word.end=std::max(word.start+1,data.t0*10000);flush();}
                    if(word.text.isEmpty())word.start=data.t0*10000;
                    word.text+=text;word.end=std::max(word.end,data.t1*10000);
                }
                flush();continue;
            }
            Caption c;
            c.speaker = speaker;
            if (options.speakerTurns && whisper_full_get_segment_speaker_turn_next(ctx, i))
                speaker = 1 - speaker; // (the next caption is someone else)
            c.start = whisper_full_get_segment_t0(ctx, i) * 10000; // (10 ms units)
            c.end = whisper_full_get_segment_t1(ctx, i) * 10000;
            c.text = QString::fromUtf8(whisper_full_get_segment_text(ctx, i)).trimmed();
            if (!c.text.isEmpty() && c.end > c.start)
                out.push_back(c);
        }
    whisper_free(ctx);
    // Whisper's segment times can include the silence before (and after) the
    // words: start each caption where the sound starts, end it where it stops.
    constexpr int kWindow = 320; // 20 ms
    auto loudness = [&](int64_t w) {
        double sum = 0;
        for (int64_t i = w * kWindow; i < std::min<int64_t>((w + 1) * kWindow, int64_t(mono16k.size())); ++i)
            sum += double(mono16k[size_t(i)]) * mono16k[size_t(i)];
        return std::sqrt(sum / kWindow);
    };
    for (Caption& c : out) {
        const int64_t first = c.start * 16 / 1000 / kWindow, last = std::min<int64_t>(c.end * 16 / 1000 / kWindow, int64_t(mono16k.size()) / kWindow);
        double peak = 0;
        for (int64_t w = first; w < last; ++w)
            peak = std::max(peak, loudness(w));
        int64_t a = first, b = last;
        while (a < b && loudness(a) < peak * 0.1)
            ++a;
        while (b > a + 1 && loudness(b - 1) < peak * 0.1)
            --b;
        if (b > a && peak > 1e-4) {
            c.start = std::max<TimeUs>(c.start, a * kWindow * 1000 / 16 - 100000); // (a little lead-in)
            c.end = std::min<TimeUs>(c.end, b * kWindow * 1000 / 16 + 200000);
        }
    }
    if (options.speakerTurns) // (a whole turn came as one caption, 140 characters on one line)
        splitLongCaptions(out, options.maxChars);
    if (cb.cancelled || rc != 0) {
        if (error)
            *error = cb.cancelled ? QStringLiteral("cancelled") : QStringLiteral("speech recognition failed (%1)").arg(rc);
        return false;
    }
    return true;
}

#else

bool whisperAvailable()
{
    return false;
}

bool whisperOnGpu()
{
    return false;
}

bool transcribe(const QString&, const std::vector<float>&, const TranscribeOptions&, const std::function<bool(double)>&, std::vector<Caption>& out,
                QString* error)
{
    out.clear();
    if (error)
        *error = QStringLiteral("this build has no speech recognition (Whisper)");
    return false;
}

#endif
