#include "core/AudioFinishing.h"
#include "core/AudioMixer.h"
#include "core/FFmpeg.h"
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace finishing {
namespace {
double db(double value) { return value > 0 ? 20 * std::log10(value) : -120; }
struct Graph {
    AVFilterGraph* graph = avfilter_graph_alloc();
    AVFilterContext *src = nullptr, *scan = nullptr, *sink = nullptr;
    ~Graph() { avfilter_graph_free(&graph); }
    int open(int rate) {
        if (!graph) return AVERROR(ENOMEM);
        graph->nb_threads = 1;
        auto args = QString("sample_rate=%1:sample_fmt=flt:channel_layout=stereo:time_base=1/%1").arg(rate).toUtf8();
        int ret = avfilter_graph_create_filter(&src, avfilter_get_by_name("abuffer"), "source", args.constData(), nullptr, graph);
        if (ret >= 0) ret = avfilter_graph_create_filter(&scan, avfilter_get_by_name("ebur128"), "loudness", "metadata=1:peak=sample+true:framelog=quiet", nullptr, graph);
        if (ret >= 0) ret = avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "sink", nullptr, nullptr, graph);
        if (ret >= 0) ret = avfilter_link(src, 0, scan, 0);
        if (ret >= 0) ret = avfilter_link(scan, 0, sink, 0);
        if (ret >= 0) ret = avfilter_graph_config(graph, nullptr);
        return ret;
    }
};
}
std::vector<Target> targets() {
    return {{"Online video / speech", -16, -1.5}, {"Louder online mix", -14, -1.5},
            {"EBU broadcast", -23, -1}, {"US broadcast", -24, -2}};
}
Report analyze(int rate, int64_t frames, const Source& source, const std::function<bool(double)>& progress) {
    Report r;
    if (rate < 8000 || rate > 192000 || frames <= 0 || frames > int64_t(rate) * 86400 || !source) {
        r.error = "Analyze a nonempty mix up to 24 hours at 8–192 kHz."; return r;
    }
    r.seconds = double(frames) / rate;
    Graph g;
    int ret = g.open(rate);
    if (ret < 0) { r.error = "Cannot initialize EBU R128 / true-peak analysis: " + ff::errorString(ret); return r; }
    double energy[2] = {}, cross = 0, peak = 0;
    int64_t silence = 0, longestSilence = 0;
    std::vector<float> buf(4096 * 2);
    auto out = ff::makeFrame();
    auto drain = [&] {
        int status;
        while ((status = av_buffersink_get_frame(g.sink, out.get())) >= 0) {
            auto read = [&](const char* key, double& maximum) {
                if (auto e = av_dict_get(out->metadata, key, nullptr, 0)) {
                    bool ok = false; double v = QString::fromUtf8(e->value).toDouble(&ok);
                    if (ok && std::isfinite(v)) maximum = std::max(maximum, v);
                }
            };
            read("lavfi.r128.M", r.momentaryMax); read("lavfi.r128.S", r.shortTermMax);
            av_frame_unref(out.get());
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF ? 0 : status;
    };
    try {
        for (int64_t pos = 0; pos < frames;) {
            if (progress && !progress(double(pos) / frames)) { r.cancelled = true; return r; }
            int n = int(std::min<int64_t>(4096, frames - pos));
            source(pos, n, buf.data());
            for (int i = 0; i < n; ++i) {
                for (int ch = 0; ch < 2; ++ch) {
                    auto& v = buf[size_t(i) * 2 + ch];
                    if (!std::isfinite(v)) { ++r.invalidSamples; v = 0; }
                    peak = std::max(peak, std::abs(double(v)));
                    energy[ch] += double(v) * v;
                    if (std::abs(v) >= 1) ++r.clippedSamples;
                }
                cross += double(buf[i*2]) * buf[i*2+1];
                if (std::max(std::abs(buf[i*2]), std::abs(buf[i*2+1])) < .0000316228f) ++silence;
                else { longestSilence = std::max(longestSilence, silence); silence = 0; }
            }
            auto f = ff::makeFrame();
            f->format = AV_SAMPLE_FMT_FLT; f->sample_rate = rate; f->nb_samples = n; f->pts = pos;
            const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            av_channel_layout_copy(&f->ch_layout, &stereo);
            ret = av_frame_get_buffer(f.get(), 0);
            if (ret >= 0) { std::memcpy(f->data[0], buf.data(), size_t(n)*2*sizeof(float)); ret = av_buffersrc_add_frame_flags(g.src, f.get(), 0); }
            if (ret >= 0) ret = drain();
            if (ret < 0) { r.error = ff::errorString(ret); return r; }
            pos += n;
        }
    } catch (const std::exception& e) { r.error = QString::fromUtf8(e.what()); return r; }
    ret = av_buffersrc_add_frame_flags(g.src, nullptr, 0);
    if (ret >= 0) ret = drain();
    if (ret < 0) { r.error = ff::errorString(ret); return r; }
    // Read exported scanner values directly: metadata peak values have only three decimal places.
    auto get = [&](const char* key, double& value) {
        double v;
        if (av_opt_get_double(g.scan->priv, key, 0, &v) < 0) return false;
        if (std::isfinite(v)) value = v;
        return true;
    };
    if (!get("integrated", r.integrated) || !get("range", r.range) || !get("true_peak", r.truePeak)) {
        r.error = "This FFmpeg build does not expose complete loudness measurements."; return r;
    }
    r.samplePeak = db(peak);
    r.truePeak = std::max(r.truePeak, r.samplePeak);
    r.audible = peak > .0000316228 && r.seconds >= .4 && r.integrated > -70;
    r.rmsLeft = db(std::sqrt(energy[0] / frames)); r.rmsRight = db(std::sqrt(energy[1] / frames));
    r.correlation = energy[0] > 0 && energy[1] > 0 ? std::clamp(cross / std::sqrt(energy[0]*energy[1]), -1., 1.) : 0;
    r.silenceSeconds = double(std::max(longestSilence, silence)) / rate;
    if (progress) progress(1);
    return r;
}
Report analyze(const ProjectSnapshot& snap, const std::function<bool(double)>& progress) {
    TimeUs end = 0;
    for (const auto& track : snap.timeline.tracks) for (const auto& c : track.clips) end = std::max(end, c.end());
    AudioMixer mixer(snap.settings.sampleRate);
    auto r = analyze(snap.settings.sampleRate, av_rescale(end, snap.settings.sampleRate, kUsPerSecond),
                     [&](int64_t pos, int n, float* out) { mixer.mix(snap, pos, n, out); }, progress);
    for (const auto& t : snap.timeline.tracks) if (t.type == TrackType::Audio) {
        if (t.muted && !t.clips.empty()) r.warnings << "Muted audio track: " + t.name;
        if (t.solo) r.warnings << "Solo is active: " + t.name;
    }
    QSet<int> used;
    std::function<void(const Timeline&,int)> collect = [&](const Timeline& timeline, int depth) {
        if (depth > 16) return;
        for (const auto& track : timeline.tracks) if (track.type == TrackType::Audio)
            for (const auto& clip : track.clips) {
                if (clip.isSequence()) collect(*clip.sequence.value, depth+1);
                else if (clip.mediaId) used.insert(clip.mediaId);
            }
    };
    collect(snap.timeline,0);
    for (int id : used) {
        auto it=snap.media.find(id);
        if (it==snap.media.end()) r.warnings << QString("Unresolved audio source ID: %1").arg(id);
        else if (!QFileInfo::exists(it->second->path)) r.warnings << "Missing audio source: " + it->second->path;
    }
    return r;
}
double safeGain(const Report& r, const Target& t) {
    if (!r.error.isEmpty() || r.cancelled || !r.audible || r.invalidSamples ||
        !std::isfinite(r.integrated) || !std::isfinite(r.truePeak) ||
        !std::isfinite(t.lufs) || !std::isfinite(t.truePeak) || t.lufs < -36 || t.lufs > -5 || t.truePeak < -12 || t.truePeak > 0)
        return std::numeric_limits<double>::quiet_NaN();
    double gain=std::min(t.lufs - r.integrated, t.truePeak - r.truePeak - .05);
    // The gain effect's supported range must never override the requested peak ceiling.
    return gain < -60 ? std::numeric_limits<double>::quiet_NaN() : std::min(gain,24.);
}
QStringList warnings(const Report& r, const Target& t) {
    auto out = r.warnings;
    if (!r.error.isEmpty()) out << r.error;
    if (r.cancelled) out << "Analysis cancelled; results are incomplete.";
    if (!r.audible) out << "No measurable programme loudness (silence, below the gate or less than 400 ms).";
    if (r.invalidSamples) out << QString("Invalid audio samples: %1. Fix the source or effect before finishing.").arg(r.invalidSamples);
    if (r.clippedSamples) out << QString("%1 channel samples reach or exceed full scale.").arg(r.clippedSamples);
    if (r.truePeak > t.truePeak + .05) out << QString("True peak exceeds the %1 dBTP ceiling.").arg(t.truePeak);
    if (r.audible && std::abs(r.integrated-t.lufs) > 1) out << QString("Mix is %1 LU from the selected loudness target.").arg(r.integrated-t.lufs, 0, 'f', 1);
    if (r.audible && r.correlation < -.25) out << "Strong negative stereo correlation: check playback in mono.";
    if (r.audible && std::abs(r.rmsLeft-r.rmsRight) > 6) out << "Left/right RMS levels differ by more than 6 dB.";
    if (r.silenceSeconds >= 3) out << QString("Longest near-silent section: %1 s. Check whether the pause is intentional.").arg(r.silenceSeconds, 0, 'f', 1);
    if (r.seconds < 3) out << "Short mix: short-term loudness and loudness range are not representative.";
    auto gain = safeGain(r, t);
    if (std::isfinite(gain) && gain < t.lufs-r.integrated-.1) out << "True-peak headroom limits the gain: reaching the loudness target needs dynamics control.";
    return out;
}
bool applyGain(Timeline& timeline, const Report& r, const Target& t, QString* error) {
    double gain = safeGain(r, t);
    if (!std::isfinite(gain)) { if(error)*error="Analyze a valid, audible mix and select a valid target first."; return false; }
    if (std::abs(gain) < .01) return true;
    EffectInstance fx; fx.id = timeline.nextId++; fx.type = "a_gain"; fx.params["gain"].value = gain;
    timeline.audioMasterEffects.push_back(fx);
    return true;
}
QJsonObject Report::json(const Target& t) const {
    QJsonArray notes; for (const auto& s : finishing::warnings(*this,t)) notes.append(s);
    double gain = safeGain(*this,t);
    return {{"ok",error.isEmpty()&&!cancelled},{"error",error},{"audible",audible},{"seconds",seconds},
            {"integrated_lufs",integrated},{"range_lu",range},{"true_peak_dbtp",truePeak},{"sample_peak_dbfs",samplePeak},
            {"momentary_max_lufs",momentaryMax},{"short_term_max_lufs",shortTermMax},{"stereo_correlation",correlation},
            {"rms_left_dbfs",rmsLeft},{"rms_right_dbfs",rmsRight},{"longest_silence_seconds",silenceSeconds},
            {"clipped_samples",double(clippedSamples)},{"invalid_samples",double(invalidSamples)},
            {"target_lufs",t.lufs},{"ceiling_dbtp",t.truePeak},{"safe_gain_db",std::isfinite(gain)?QJsonValue(gain):QJsonValue()}, {"warnings",notes}};
}
}
