#include "core/AudioMixer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>

namespace {
constexpr uint64_t kEvictAfterCalls = 200;

bool hasActiveEffectsList(const EffectChain& chain)
{
    return std::any_of(chain.begin(), chain.end(), [](const EffectInstance& e) { return e.enabled; });
}
}

float measureClipPeak(const MediaItem& media, const Clip& clip, int rate)
{
    if (!media.info.audioStream(clip.streamIndex))
        return 0.0f;
    AudioDecoder dec;
    if (!dec.open(media.path, clip.streamIndex, rate))
        return 0.0f;
    const int64_t start = usToSamples(clip.sourceIn, rate);
    const int64_t total = usToSamples(clip.sourceSpan(), rate);
    std::vector<float> buf(size_t(rate) * 2);
    float peak = 0.0f;
    for (int64_t pos = 0; pos < total; pos += rate) {
        const int n = int(std::min<int64_t>(rate, total - pos));
        dec.read(start + pos, n, buf.data());
        for (int i = 0; i < n * 2; ++i)
            peak = std::max(peak, std::abs(buf[size_t(i)]));
    }
    return peak;
}

AudioMixer::AudioMixer(int sampleRate) : m_rate(sampleRate) {}
AudioMixer::~AudioMixer() = default;

void AudioMixer::reset()
{
    m_sequences.clear(); m_sequenceSnapshots.clear(); m_sequenceStretch.clear();
    m_entries.clear();
    m_fx.clear();
    m_speed.clear();
    m_clipFx.clear();
    m_racks.clear();
    m_trackFx.clear();
    m_masterFx = {};
    m_duck.clear();
    m_trackPeaks.clear();
    m_session.reset();
}

AudioMixer::Entry* AudioMixer::acquire(const MediaItem& media, int streamIndex, int clipId, int64_t sourcePos)
{
    Entry* best = nullptr;
    int64_t bestCost = 0;
    for (auto& e : m_entries) {
        if (e->path != media.path || e->mediaId != media.id || e->revision != media.revision || e->streamIndex != streamIndex || e->lastUsed == m_tick)
            continue;
        if (e->failed)
            return nullptr;
        if (e->lastClipId == clipId) {
            best = e.get();
            break;
        }
        const int64_t pos = e->decoder->position();
        const int64_t cost = sourcePos >= pos ? sourcePos - pos : (pos - sourcePos) * 4;
        if (!best || cost < bestCost) {
            best = e.get();
            bestCost = cost;
        }
    }
    if (!best) {
        auto e = std::make_unique<Entry>();
        e->path = media.path;
        e->mediaId = media.id;
        e->revision = media.revision;
        e->streamIndex = streamIndex;
        e->decoder = std::make_unique<AudioDecoder>();
        e->failed = !e->decoder->open(media.path, streamIndex, m_rate);
        best = e.get();
        m_entries.push_back(std::move(e));
        if (best->failed)
            return nullptr;
    }
    best->lastClipId = clipId;
    best->lastUsed = m_tick;
    return best;
}

void AudioMixer::fetchSource(AudioDecoder& dec, SpeedState& st, int64_t from, int frames, float* out)
{
    const int64_t cached = int64_t(st.cache.size() / 2);
    if (from < st.cacheStart || from + frames > st.cacheStart + cached) {
        // Load a block around the request; when moving backwards (reverse
        // playback) the block ends at the request so the next ones hit too.
        const int blockLen = std::max(frames, m_rate / 2);
        const int64_t blockStart = from < st.cacheStart ? from + frames - blockLen : from;
        st.cache.resize(size_t(blockLen) * 2);
        dec.read(blockStart, blockLen, st.cache.data());
        st.cacheStart = blockStart;
    }
    std::memcpy(out, st.cache.data() + (from - st.cacheStart) * 2, sizeof(float) * 2 * size_t(frames));
}

void AudioMixer::renderSpeed(AudioDecoder& dec, const MediaItem& media, const Clip& clip, int64_t cs, int64_t a, int n, float* out)
{
    SpeedState& st = m_speed[clip.id];
    if (st.path != media.path || st.mediaId != media.id || st.revision != media.revision || st.streamIndex != clip.streamIndex) {
        st = {};
        st.path = media.path; st.mediaId = media.id;
        st.revision = media.revision; st.streamIndex = clip.streamIndex;
    }
    st.lastUsed = m_tick;
    if (clip.hasRamp()) {
        renderRamp(dec, clip, st, cs, a, n, out);
        return;
    }
    const double s = clip.speed;
    const double srcStart = double(usToSamples(clip.sourceIn, m_rate));
    const double span = double(usToSamples(clip.length, m_rate)) * s;
    auto pos = [&](int64_t k) {
        const double local = double(k - cs) * s;
        return clip.reverse ? srcStart + span - local : srcStart + local;
    };

    const bool stretch = clip.preservePitch && std::abs(s - 1.0) > 1e-6 && !st.tempoFailed;
    if (stretch) {
        if (!st.tempo || st.nextOut != a || st.tempoSpeed != s || st.tempoReverse != clip.reverse) {
            st.tempo = std::make_unique<AudioTempo>();
            if (!st.tempo->reset(s, m_rate)) {
                st.tempoFailed = true; // fall back to varispeed
                renderSpeed(dec, media, clip, cs, a, n, out);
                return;
            }
            st.tempoSpeed = s;
            st.tempoReverse = clip.reverse;
            st.srcCursor = std::llround(pos(a));
            st.nextOut = a;
            st.fifo.clear();
        }
        constexpr int kChunk = 2048;
        std::vector<float> in(size_t(kChunk) * 2);
        for (int guard = 0; st.fifo.size() < size_t(n) * 2 && guard < 512; ++guard) {
            if (!clip.reverse) {
                fetchSource(dec, st, st.srcCursor, kChunk, in.data());
                st.srcCursor += kChunk;
            } else {
                fetchSource(dec, st, st.srcCursor - kChunk, kChunk, in.data());
                st.srcCursor -= kChunk;
                for (int i = 0, j = kChunk - 1; i < j; ++i, --j) {
                    std::swap(in[size_t(i) * 2], in[size_t(j) * 2]);
                    std::swap(in[size_t(i) * 2 + 1], in[size_t(j) * 2 + 1]);
                }
            }
            st.tempo->push(in.data(), kChunk);
            st.tempo->pull(st.fifo);
        }
        const size_t have = std::min(st.fifo.size(), size_t(n) * 2);
        std::memcpy(out, st.fifo.data(), sizeof(float) * have);
        std::memset(out + have, 0, sizeof(float) * (size_t(n) * 2 - have));
        st.fifo.erase(st.fifo.begin(), st.fifo.begin() + have);
        st.nextOut = a + n;
        return;
    }

    // Varispeed (pitch follows speed) or plain reverse: linear interpolation.
    const double p0 = pos(a), p1 = pos(a + n - 1);
    const int64_t lo = int64_t(std::floor(std::min(p0, p1)));
    const int64_t hi = int64_t(std::ceil(std::max(p0, p1))) + 2;
    m_srcBuf.resize(size_t(hi - lo) * 2);
    fetchSource(dec, st, lo, int(hi - lo), m_srcBuf.data());
    const int64_t len = hi - lo;
    for (int i = 0; i < n; ++i) {
        const double p = pos(a + i) - double(lo);
        const int64_t i0 = std::clamp<int64_t>(int64_t(std::floor(p)), 0, len - 1);
        const int64_t i1 = std::min(i0 + 1, len - 1);
        const float f = float(p - std::floor(p));
        out[i * 2] = m_srcBuf[size_t(i0) * 2] + (m_srcBuf[size_t(i1) * 2] - m_srcBuf[size_t(i0) * 2]) * f;
        out[i * 2 + 1] = m_srcBuf[size_t(i0) * 2 + 1] + (m_srcBuf[size_t(i1) * 2 + 1] - m_srcBuf[size_t(i0) * 2 + 1]) * f;
    }
}

void AudioMixer::renderRamp(AudioDecoder& dec, const Clip& clip, SpeedState& st, int64_t cs, int64_t a, int n, float* out)
{
    const double usPerSample = double(kUsPerSecond) / m_rate;
    const double srcStart = double(usToSamples(clip.sourceIn, m_rate));
    const double span = double(clip.sourceSpan());
    // Source sample position at timeline sample k, along the ramp.
    auto pos = [&](int64_t k) {
        const double off = std::clamp(clip.rampOffset(double(k - cs) * usPerSample), 0.0, span);
        return srcStart + (clip.reverse ? span - off : off) / usPerSample;
    };

    if (clip.preservePitch) {
        if (!st.stretch)
            st.stretch = std::make_unique<AudioStretch>();
        st.stretch->render(a, n, out, pos, [&](int64_t from, int count, float* dst) { fetchSource(dec, st, from, count, dst); });
        return;
    }

    // Varispeed: the pitch follows the ramp (linear interpolation).
    m_rampPos.resize(size_t(n));
    double pmin = 1e300, pmax = -1e300;
    for (int i = 0; i < n; ++i) {
        const double p = pos(a + i);
        m_rampPos[size_t(i)] = p;
        pmin = std::min(pmin, p);
        pmax = std::max(pmax, p);
    }
    const int64_t lo = int64_t(std::floor(pmin));
    const int64_t len = int64_t(std::ceil(pmax)) + 2 - lo;
    m_srcBuf.resize(size_t(len) * 2);
    fetchSource(dec, st, lo, int(len), m_srcBuf.data());
    for (int i = 0; i < n; ++i) {
        const double p = m_rampPos[size_t(i)] - double(lo);
        const int64_t i0 = std::clamp<int64_t>(int64_t(std::floor(p)), 0, len - 1);
        const int64_t i1 = std::min(i0 + 1, len - 1);
        const float f = float(p - std::floor(p));
        out[i * 2] = m_srcBuf[size_t(i0) * 2] + (m_srcBuf[size_t(i1) * 2] - m_srcBuf[size_t(i0) * 2]) * f;
        out[i * 2 + 1] = m_srcBuf[size_t(i0) * 2 + 1] + (m_srcBuf[size_t(i1) * 2 + 1] - m_srcBuf[size_t(i0) * 2 + 1]) * f;
    }
}

AudioMixer::FxState& AudioMixer::fxState(const EffectInstance& e, const AudioEffectDef& def)
{
    FxState& st = m_fx[e.id];
    st.lastUsed = m_tick;
    if (!st.proc) {
        st.proc = def.create();
        st.proc->reset(m_rate);
        st.expected = -2; // fresh: no reset needed on first use
    }
    return st;
}

int AudioMixer::chainLatency(const EffectChain& chain, TimeUs keyTime)
{
    int total = 0;
    for (const EffectInstance& e : chain) {
        if (!e.enabled)
            continue;
        if (e.isRack()) {
            total += rackLatency(e, keyTime);
            continue;
        }
        if (const AudioEffectDef* def = findAudioEffect(e.type))
            total += fxState(e, *def).proc->latency(resolveParams(def->params, e.params, keyTime));
    }
    return total;
}

namespace {
// Replaces samples that are not numbers (or infinite) with silence; true if
// there were any. One bad sample in a filter's memory would silence it for good.
bool scrub(float* buf, size_t count)
{
    bool bad = false;
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(buf[i])) {
            buf[i] = 0.0f;
            bad = true;
        }
    return bad;
}
} // namespace

void AudioMixer::processChain(const EffectChain& chain, float* buf, int frames, int64_t timelinePos, TimeUs keyTime,
                              bool forceReset)
{
    for (const EffectInstance& e : chain) {
        if (!e.enabled)
            continue;
        if (e.isRack()) {
            processRack(e, buf, frames, timelinePos, keyTime, forceReset);
            continue;
        }
        const AudioEffectDef* def = findAudioEffect(e.type);
        if (!def)
            continue;
        FxState& st = fxState(e, *def);
        if (forceReset || (st.expected != timelinePos && st.expected != -2))
            st.proc->reset(m_rate); // a jump: clear tails and envelopes
        st.proc->process(buf, frames, resolveParams(def->params, e.params, keyTime));
        if (scrub(buf, size_t(frames) * 2))
            st.proc->reset(m_rate);
        st.expected = timelinePos + frames;
    }
}

int AudioMixer::rackLatency(const EffectInstance& rack, TimeUs keyTime)
{
    int latency = 0;
    for (const RackChain* c : activeRackChains(rack))
        latency = std::max(latency, chainLatency(c->effects, keyTime));
    return latency;
}

namespace {
// Delays `x` (stereo) by the length of `line`, which carries the history.
void delayThrough(std::vector<float>& line, float* x, size_t count)
{
    if (line.empty())
        return;
    std::vector<float> joined(line.size() + count);
    std::copy(line.begin(), line.end(), joined.begin());
    std::copy(x, x + count, joined.begin() + std::ptrdiff_t(line.size()));
    std::copy(joined.begin(), joined.begin() + std::ptrdiff_t(count), x);
    std::copy(joined.end() - std::ptrdiff_t(line.size()), joined.end(), line.begin());
}
} // namespace

void AudioMixer::processRack(const EffectInstance& rack, float* buf, int frames, int64_t pos, TimeUs keyTime, bool forceReset)
{
    const auto chains = activeRackChains(rack);
    if (chains.empty())
        return; // dry
    RackState& st = m_racks[rack.id];
    st.lastUsed = m_tick;
    // Chains with less latency are delayed to line up with the slowest one.
    std::vector<int> lat;
    int maxLat = 0;
    for (const RackChain* c : chains) {
        lat.push_back(chainLatency(c->effects, keyTime));
        maxLat = std::max(maxLat, lat.back());
    }
    std::vector<int> wanted;
    for (int l : lat)
        wanted.push_back(maxLat - l);
    wanted.push_back(maxLat);
    if (forceReset || st.expected != pos || st.delayLengths != wanted) {
        st.delayLengths = wanted;
        st.delays.assign(wanted.size(), {});
        for (size_t i = 0; i < wanted.size(); ++i)
            st.delays[i].assign(size_t(wanted[i]) * 2, 0.0f);
    }
    const size_t count = size_t(frames) * 2;
    st.in.assign(buf, buf + count);
    st.sum.assign(count, 0.0f);
    for (size_t i = 0; i < chains.size(); ++i) {
        st.tmp = st.in;
        processChain(chains[i]->effects, st.tmp.data(), frames, pos, keyTime, forceReset);
        delayThrough(st.delays[i], st.tmp.data(), count);
        const double db = resolveParams(rackChainParams(true), chains[i]->params, keyTime).num(QStringLiteral("level"));
        const float gain = db <= -59.9 ? 0.0f : float(std::pow(10.0, db / 20.0));
        for (size_t k = 0; k < count; ++k)
            st.sum[k] += st.tmp[k] * gain;
    }
    delayThrough(st.delays.back(), st.in.data(), count);
    const float wet = float(std::clamp(resolveParams(rackParams(), rack.params, keyTime).num(QStringLiteral("mix")) / 100.0, 0.0, 1.0));
    for (size_t k = 0; k < count; ++k)
        buf[k] = st.in[k] * (1.0f - wet) + st.sum[k] * wet;
    st.expected = pos + frames;
}

bool AudioMixer::mixTrackInput(const ProjectSnapshot& snap, const Track& track, int64_t pos, int frames, float* buf)
{
    std::memset(buf, 0, sizeof(float) * 2 * size_t(frames));
    const int64_t end = pos + frames;
    bool anyClip = false;
    for (size_t ci = 0; ci < track.clips.size(); ++ci) {
        const Clip& clip = track.clips[ci];
        if (clip.muted)
            continue;
        const int64_t cs = usToSamples(clip.start, m_rate);
        const int64_t ce = usToSamples(clip.end(), m_rate);
        if (cs >= end)
            break;
        const int64_t a = std::max(pos, cs);
        const int64_t b = std::min(end, ce);
        if (a >= b)
            continue;
        if (clip.isSequence()) {
            auto& mixer = m_sequences[clip.id];
            if (!mixer) mixer = std::make_unique<AudioMixer>(m_rate);
            const auto& child=m_sequenceSnapshots[clip.id].get(snap,clip.sequence);
            const int n = int(b-a); m_tmp.resize(size_t(n)*2);
            auto readSequence=[&](int64_t from,int count,float* dst) {
                if (clip.speed == 1 && !clip.reverse && !clip.hasRamp()&&!clip.loops()) {
                    mixer->mix(child,usToSamples(clip.sourceTimeAt(samplesToUs(from,m_rate)),m_rate),count,dst);
                } else {
                    auto position=[&](int64_t sample){return double(clip.sourceTimeAt(samplesToUs(sample,m_rate)))*m_rate/kUsPerSecond;};
                    if(clip.preservePitch) {
                        auto& stretch=m_sequenceStretch[clip.id];if(!stretch)stretch=std::make_unique<AudioStretch>();
                        stretch->render(from,count,dst,position,[&](int64_t at,int size,float* target){mixer->mix(child,at,size,target);});
                    } else {
                        std::vector<double> positions(size_t(count),0);int64_t lo=std::numeric_limits<int64_t>::max(),hi=std::numeric_limits<int64_t>::min();
                        for(int i=0;i<count;++i){positions[size_t(i)]=position(from+i);lo=std::min(lo,int64_t(std::floor(positions[size_t(i)])));hi=std::max(hi,int64_t(std::floor(positions[size_t(i)]))+1);}
                        if(hi-lo>2000000){std::fill(dst,dst+count*2,0);return;}
                        std::vector<float> source(size_t(hi-lo+1)*2);mixer->mix(child,lo,int(hi-lo+1),source.data());
                        for(int i=0;i<count;++i){const double p=positions[size_t(i)]-lo;const auto index=size_t(std::floor(p));const float fraction=float(p-index);for(int ch=0;ch<2;++ch)dst[i*2+ch]=source[index*2+ch]*(1-fraction)+source[(index+1)*2+ch]*fraction;}
                    }
                }
            };
            const TimeUs key=samplesToUs(a,m_rate)-clip.start;const int latency=hasActiveEffectsList(clip.effects)?chainLatency(clip.effects,key):0;
            if(!latency){readSequence(a,n,m_tmp.data());if(hasActiveEffectsList(clip.effects))processChain(clip.effects,m_tmp.data(),n,a,key);}
            else {
                auto& state=m_clipFx[clip.id];state.lastUsed=m_tick;
                if(state.expected!=a||state.latency!=latency){state.latency=latency;m_prime.resize(size_t(latency)*2);readSequence(a,latency,m_prime.data());processChain(clip.effects,m_prime.data(),latency,a,key,true);state.inputPos=a+latency;}
                readSequence(state.inputPos,n,m_tmp.data());processChain(clip.effects,m_tmp.data(),n,state.inputPos,key);state.inputPos+=n;state.expected=a+n;
            }
            const auto env=effectiveEnvelope(track,ci); float* dst=buf+(a-pos)*2;
            for(int i=0;i<n;++i) {
                const float gain=clip.gain*float(envelopeGain(clip,env,samplesToUs(a+i,m_rate)));
                dst[i*2]+=m_tmp[size_t(i)*2]*gain; dst[i*2+1]+=m_tmp[size_t(i)*2+1]*gain;
            }
            anyClip=true; continue;
        }
        auto mit = snap.media.find(clip.mediaId);
        if (mit == snap.media.end() || !mit->second->info.audioStream(clip.streamIndex))
            continue;

        const bool altered = clip.speed != 1.0 || clip.reverse || clip.hasRamp();
        const int64_t srcPos = usToSamples(clip.sourceTimeAt(samplesToUs(a, m_rate)), m_rate);
        Entry* e = acquire(*mit->second, clip.streamIndex, clip.id, srcPos);
        if (!e)
            continue;
        const int n = int(b - a);
        // Reads the event's audio for timeline samples [from, from + count).
        auto readClip = [&](int64_t from, int count, float* dst) {
            if (altered) {
                renderSpeed(*e->decoder, *mit->second, clip, cs, from, count, dst);
            } else if (clip.loopLength > 0) {
                // A looping event: the file again from its start at each end.
                const int64_t loop = std::max<int64_t>(1, usToSamples(clip.loopLength, m_rate));
                int64_t src = (usToSamples(clip.sourceIn, m_rate) + (from - cs)) % loop;
                while (count > 0) {
                    const int n = int(std::min<int64_t>(count, loop - src));
                    e->decoder->read(src, n, dst);
                    dst += size_t(n) * 2;
                    count -= n;
                    src = 0;
                }
            } else {
                e->decoder->read(usToSamples(clip.sourceIn, m_rate) + (from - cs), count, dst);
            }
        };
        m_tmp.resize(size_t(n) * 2);

        // Event FX run on the event's own audio, before its fades and level.
        if (hasActiveEffectsList(clip.effects)) {
            const TimeUs key = samplesToUs(a, m_rate) - clip.start;
            const int latency = chainLatency(clip.effects, key);
            if (latency == 0) {
                readClip(a, n, m_tmp.data());
                processChain(clip.effects, m_tmp.data(), n, a, key);
            } else {
                // Delay compensation: feed the chain `latency` samples ahead.
                ClipFxState& cf = m_clipFx[clip.id];
                cf.lastUsed = m_tick;
                if (cf.expected != a || cf.latency != latency) {
                    cf.latency = latency;
                    m_prime.resize(size_t(latency) * 2);
                    readClip(a, latency, m_prime.data());
                    processChain(clip.effects, m_prime.data(), latency, a, key, true); // output discarded
                    cf.inputPos = a + latency;
                }
                readClip(cf.inputPos, n, m_tmp.data());
                processChain(clip.effects, m_tmp.data(), n, cf.inputPos, key);
                cf.inputPos += n;
                cf.expected = a + n;
            }
        } else {
            readClip(a, n, m_tmp.data());
        }

        const ClipEnvelope env = effectiveEnvelope(track, ci);
        const float base = clip.gain;
        const bool faded = env.fadeIn > 0 || env.fadeOut > 0;
        float* dst = buf + (a - pos) * 2;
        for (int i = 0; i < n; ++i) {
            float g = base;
            if (faded)
                g *= float(envelopeGain(clip, env, samplesToUs(a + i, m_rate)));
            dst[i * 2] += m_tmp[size_t(i) * 2] * g;
            dst[i * 2 + 1] += m_tmp[size_t(i) * 2 + 1] * g;
        }
        anyClip = true;
    }
    return anyClip;
}

void AudioMixer::mixTracks(const ProjectSnapshot& snap, int64_t pos, int frames, float* out)
{
    std::memset(out, 0, sizeof(float) * 2 * size_t(frames));
    const auto& tracks = snap.timeline.tracks;
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
                                     [](const Track& t) { return t.type == TrackType::Audio && t.solo; });
    // Each track's output (after FX, level and pan) first, then ducking, then the sum.
    // (Local buffers: mixTracks also runs while priming the master.)
    std::vector<std::vector<float>> outputs(tracks.size());
    std::vector<float> trackBuf(size_t(frames) * 2);
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        const Track& track = tracks[ti];
        if (track.type != TrackType::Audio || track.muted || (anySolo && !track.solo))
            continue;
        std::vector<float>& dst = outputs[ti];
        dst.assign(size_t(frames) * 2, 0.0f);
        float* mine = dst.data(); // this track's own output
        // Constant-power-ish pan law kept simple: attenuate the opposite side.
        const float panL = track.pan > 0 ? 1.0f - track.pan : 1.0f;
        const float panR = track.pan < 0 ? 1.0f + track.pan : 1.0f;
        const bool trackFx = hasActiveEffectsList(track.effects);
        if (!trackFx) {
            if (!mixTrackInput(snap, track, pos, frames, trackBuf.data()))
                continue;
        } else {
            // Track FX keep running between events so reverb/echo tails ring out.
            const TimeUs key = samplesToUs(pos, m_rate);
            const int latency = chainLatency(track.effects, key);
            if (latency == 0) {
                mixTrackInput(snap, track, pos, frames, trackBuf.data());
                processChain(track.effects, trackBuf.data(), frames, pos, key);
            } else {
                // Delay compensation: feed the track's chain `latency` samples ahead.
                ClipFxState& ts = m_trackFx[track.id];
                ts.lastUsed = m_tick;
                if (ts.expected != pos || ts.latency != latency) {
                    ts.latency = latency;
                    m_trackPrime.resize(size_t(latency) * 2);
                    mixTrackInput(snap, track, pos, latency, m_trackPrime.data());
                    processChain(track.effects, m_trackPrime.data(), latency, pos, key, true); // output discarded
                    ts.inputPos = pos + latency;
                }
                mixTrackInput(snap, track, ts.inputPos, frames, trackBuf.data());
                processChain(track.effects, trackBuf.data(), frames, ts.inputPos, samplesToUs(ts.inputPos, m_rate));
                ts.inputPos += frames;
                ts.expected = pos + frames;
            }
        }
        if (!track.volume.active && !track.panEnvelope.active) {
            const float gl = track.level * panL, gr = track.level * panR;
            for (int i = 0; i < frames; ++i) {
                mine[i * 2] += trackBuf[size_t(i) * 2] * gl;
                mine[i * 2 + 1] += trackBuf[size_t(i) * 2 + 1] * gr;
            }
        } else {
            // Envelopes: evaluate every 32 samples and interpolate in between.
            constexpr int kStep = 32;
            auto gains = [&](int i, float& l, float& r) {
                const TimeUs t = samplesToUs(pos + i, m_rate);
                const double g = track.gainAt(t), pn = track.panAt(t);
                l = float(g * (pn > 0 ? 1.0 - pn : 1.0));
                r = float(g * (pn < 0 ? 1.0 + pn : 1.0));
            };
            float l0, r0;
            gains(0, l0, r0);
            for (int i0 = 0; i0 < frames; i0 += kStep) {
                const int i1 = std::min(frames, i0 + kStep);
                float l1, r1;
                gains(i1, l1, r1);
                for (int i = i0; i < i1; ++i) {
                    const float f = float(i - i0) / float(i1 - i0);
                    mine[i * 2] += trackBuf[size_t(i) * 2] * (l0 + (l1 - l0) * f);
                    mine[i * 2 + 1] += trackBuf[size_t(i) * 2 + 1] * (r0 + (r1 - r0) * f);
                }
                l0 = l1;
                r0 = r1;
            }
        }
    }
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        if (outputs[ti].empty())
            continue;
        const Track& track = tracks[ti];
        if (track.duckUnder && track.duckUnder != track.id) {
            const int side = snap.timeline.trackIndex(track.duckUnder);
            if (side >= 0 && !outputs[size_t(side)].empty())
                applyDuck(track, outputs[size_t(side)].data(), outputs[ti].data(), frames, pos);
            else
                applyDuck(track, nullptr, outputs[ti].data(), frames, pos); // silent voice: back up
        }
        auto& peak = m_trackPeaks[track.id];
        const float* src = outputs[ti].data();
        for (int i = 0; i < frames; ++i) {
            out[i * 2] += src[i * 2];
            out[i * 2 + 1] += src[i * 2 + 1];
            peak.first = std::max(peak.first, std::abs(src[i * 2]));
            peak.second = std::max(peak.second, std::abs(src[i * 2 + 1]));
        }
    }
}

void AudioMixer::applyDuck(const Track& track, const float* side, float* buf, int frames, int64_t pos)
{
    // A gated envelope follower on blocks of 64 samples: the voice is "on"
    // above -40 dBFS; the duck comes down in ~25 ms, holds 0.3 s after the
    // voice stops and recovers over ~0.5 s.
    constexpr int kBlock = 64;
    const float threshold = 0.01f; // -40 dBFS
    const float target = std::pow(10.0f, std::min(0.0f, track.duckAmount) / 20.0f);
    const float attack = 1.0f - std::exp(-float(kBlock) / (0.025f * float(m_rate)));
    const float release = 1.0f - std::exp(-float(kBlock) / (0.5f * float(m_rate)));
    const int holdSamples = int(0.3 * m_rate);
    DuckState& st = m_duck[track.id];
    bool fresh = st.expected != pos;
    for (int b0 = 0; b0 < frames; b0 += kBlock) {
        const int b1 = std::min(frames, b0 + kBlock);
        float level = 0;
        if (side)
            for (int i = b0; i < b1; ++i)
                level = std::max({level, std::abs(side[i * 2]), std::abs(side[i * 2 + 1])});
        if (level > threshold)
            st.hold = holdSamples;
        else
            st.hold = std::max(0, st.hold - (b1 - b0));
        const float want = st.hold > 0 ? target : 1.0f;
        const float from = st.gain;
        if (fresh) { // after a seek: start where the voice is now, no swell
            st.gain = want;
            fresh = false;
        } else {
            st.gain += (want - st.gain) * (want < st.gain ? attack : release);
        }
        for (int i = b0; i < b1; ++i) {
            const float g = from + (st.gain - from) * float(i - b0 + 1) / float(b1 - b0);
            buf[i * 2] *= g;
            buf[i * 2 + 1] *= g;
        }
    }
    st.expected = pos + frames;
}

std::map<int, std::pair<float, float>> AudioMixer::takeTrackPeaks()
{
    std::map<int, std::pair<float, float>> out;
    out.swap(m_trackPeaks);
    return out;
}

void AudioMixer::mix(const ProjectSnapshot& snap, int64_t pos, int frames, float* out)
{
    if (m_session != snap.session) {
        reset();
        m_session = snap.session;
    }
    ++m_tick;
    const EffectChain& master = snap.timeline.audioMasterEffects;
    const TimeUs key = samplesToUs(pos, m_rate);
    const int latency = hasActiveEffectsList(master) ? chainLatency(master, key) : 0;
    if (latency == 0) {
        mixTracks(snap, pos, frames, out);
        if (hasActiveEffectsList(master))
            processChain(master, out, frames, pos, key);
    } else {
        // Master FX with delay (look-ahead limiters, loudness normalisers):
        // mix everything `latency` samples ahead so the output lines up.
        if (m_masterFx.expected != pos || m_masterFx.latency != latency) {
            m_masterFx.latency = latency;
            m_masterPrime.resize(size_t(latency) * 2);
            mixTracks(snap, pos, latency, m_masterPrime.data());
            processChain(master, m_masterPrime.data(), latency, pos, key, true); // output discarded
            m_masterFx.inputPos = pos + latency;
        }
        mixTracks(snap, m_masterFx.inputPos, frames, out);
        processChain(master, out, frames, m_masterFx.inputPos, samplesToUs(m_masterFx.inputPos, m_rate));
        m_masterFx.inputPos += frames;
        m_masterFx.expected = pos + frames;
    }
    scrub(out, size_t(frames) * 2);

    if (m_tick % 64 == 0) {
        for (auto it = m_speed.begin(); it != m_speed.end();)
            it = m_tick - it->second.lastUsed > kEvictAfterCalls ? m_speed.erase(it) : std::next(it);
        for (auto it = m_fx.begin(); it != m_fx.end();)
            it = m_tick - it->second.lastUsed > kEvictAfterCalls ? m_fx.erase(it) : std::next(it);
        for (auto it = m_clipFx.begin(); it != m_clipFx.end();)
            it = m_tick - it->second.lastUsed > kEvictAfterCalls ? m_clipFx.erase(it) : std::next(it);
        for (auto it = m_racks.begin(); it != m_racks.end();)
            it = m_tick - it->second.lastUsed > kEvictAfterCalls ? m_racks.erase(it) : std::next(it);
        for (auto it = m_trackFx.begin(); it != m_trackFx.end();)
            it = m_tick - it->second.lastUsed > kEvictAfterCalls ? m_trackFx.erase(it) : std::next(it);
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                       [&](const std::unique_ptr<Entry>& e) {
                                           return !e->failed && m_tick - e->lastUsed > kEvictAfterCalls;
                                       }),
                        m_entries.end());
    }
}
