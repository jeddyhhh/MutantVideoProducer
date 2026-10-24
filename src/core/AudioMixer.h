#pragma once

#include "core/AudioDecoder.h"
#include "core/AudioEffects.h"
#include "core/AudioStretch.h"
#include "core/AudioTempo.h"
#include "core/Project.h"

#include <map>
#include <memory>
#include <vector>

// Peak level (linear, max |sample|) of an audio event's source span, or 0.
float measureClipPeak(const MediaItem& media, const Clip& clip, int rate = 48000);

// Mixes all audio tracks into interleaved stereo float. Owns its decoders;
// use one mixer per thread.
class AudioMixer {
public:
    explicit AudioMixer(int sampleRate);
    ~AudioMixer();

    int sampleRate() const { return m_rate; }

    // Mixes `frames` frames starting at timeline sample `pos` into `out`
    // (2 * frames floats, overwritten).
    void mix(const ProjectSnapshot& snap, int64_t pos, int frames, float* out);

    void reset();

    // Peak levels (linear) each audio track sent to the mix since the last
    // call (after its FX, level, pan and ducking), by track id.
    std::map<int, std::pair<float, float>> takeTrackPeaks();

private:
    // Auto ducking: smoothed gain of a ducked track, per track id.
    struct DuckState {
        float gain = 1.0f;
        int hold = 0;          // samples the duck stays down after the voice stops
        int64_t expected = -1; // next timeline sample (anything else: a seek)
    };
    std::map<int, DuckState> m_duck;
    std::map<int, std::pair<float, float>> m_trackPeaks;
    // Applies `track`'s ducking to buf (frames at timeline sample pos) from the
    // level of `side` (the track it ducks under, after its level and pan).
    void applyDuck(const Track& track, const float* side, float* buf, int frames, int64_t pos);

    std::shared_ptr<const int> m_session;
    struct Entry {
        QString path;
        int mediaId = 0;
        int revision = 0; // MediaItem::revision (relinked media gets new decoders)
        int streamIndex = -1;
        std::unique_ptr<AudioDecoder> decoder;
        bool failed = false;
        int lastClipId = 0;
        uint64_t lastUsed = 0;
    };
    Entry* acquire(const MediaItem& media, int streamIndex, int clipId, int64_t sourcePos);

    // Per-event state for events playing at another speed or reversed.
    struct SpeedState {
        QString path;
        int mediaId = 0, revision = -1, streamIndex = -1;
        std::vector<float> cache;     // decoded source block (stereo)
        int64_t cacheStart = 0;
        std::unique_ptr<AudioTempo> tempo;
        double tempoSpeed = 0;
        bool tempoReverse = false;
        bool tempoFailed = false;
        int64_t nextOut = -1;         // timeline sample the tempo output continues at
        int64_t srcCursor = 0;        // next source sample to feed
        std::vector<float> fifo;      // stretched output not yet used
        uint64_t lastUsed = 0;
        std::unique_ptr<AudioStretch> stretch; // pitch-preserved speed ramps
    };
    void fetchSource(AudioDecoder& dec, SpeedState& st, int64_t from, int frames, float* out);
    void renderSpeed(AudioDecoder& dec, const MediaItem& media, const Clip& clip, int64_t clipStartSample, int64_t a, int n, float* out);
    void renderRamp(AudioDecoder& dec, const Clip& clip, SpeedState& st, int64_t clipStartSample, int64_t a, int n,
                    float* out);
    std::vector<double> m_rampPos;
    std::map<int, SpeedState> m_speed; // by clip id

    // Effect processors, by EffectInstance id. `expected` is the timeline
    // sample the next block should start at; anything else (a seek) resets.
    struct FxState {
        std::unique_ptr<AudioProcessor> proc;
        int64_t expected = -1;
        uint64_t lastUsed = 0;
        int64_t quiet = 0; // samples nothing has gone in or come out for
        bool passed = false; // silence is passing it by
    };
    void processChain(const EffectChain& chain, float* buf, int frames, int64_t timelinePos, TimeUs keyTime,
                      bool forceReset = false);
    FxState& fxState(const EffectInstance& e, const AudioEffectDef& def);
    int chainLatency(const EffectChain& chain, TimeUs keyTime);
    // Racks: parallel chains summed (with delay compensation between them).
    struct RackState {
        std::vector<std::vector<float>> delays; // per playing chain, then the dry signal
        std::vector<int> delayLengths;
        int64_t expected = -1;
        uint64_t lastUsed = 0;
        std::vector<float> in, tmp, sum;
    };
    void processRack(const EffectInstance& rack, float* buf, int frames, int64_t timelinePos, TimeUs keyTime, bool forceReset);
    int rackLatency(const EffectInstance& rack, TimeUs keyTime);
    std::map<int, RackState> m_racks;

    // Event FX with latency read their input ahead so the output lines up.
    struct ClipFxState {
        int64_t expected = -1;   // timeline sample the next output block starts at
        int64_t inputPos = 0;    // timeline sample of the next input to feed
        int latency = 0;
        uint64_t lastUsed = 0;
    };
    std::map<int, ClipFxState> m_clipFx;
    // Track FX and master FX are compensated the same way: the bus is fed
    // `latency` samples ahead of the output position.
    std::map<int, ClipFxState> m_trackFx; // by track id
    ClipFxState m_masterFx;
    std::vector<float> m_prime, m_trackPrime, m_masterPrime;
    // Sums every audio track (after its FX, envelopes, level and pan) for
    // timeline samples [pos, pos + frames) into out (overwritten).
    void mixTracks(const ProjectSnapshot& snap, int64_t pos, int frames, float* out);
    // One track's events (with their FX and fades) for [pos, pos + frames)
    // into buf (overwritten); returns false if no event plays there.
    bool mixTrackInput(const ProjectSnapshot& snap, const Track& track, int64_t pos, int frames, float* buf);
    std::map<int, FxState> m_fx;

    std::map<int, std::unique_ptr<AudioMixer>> m_sequences;
    std::map<int, SequenceSnapshotCache> m_sequenceSnapshots;
    std::map<int, std::unique_ptr<AudioStretch>> m_sequenceStretch;
    int m_rate;
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<float> m_tmp;
    std::vector<float> m_srcBuf;
    uint64_t m_tick = 0;
};
