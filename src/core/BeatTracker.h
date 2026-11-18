#pragma once

// Real-time beat detection for the Live tab's Follow music auto switch (and Find Tempo Again).
//
// Hits: the audio input is band-filtered (full range, or just the kick /
// snare / hi-hat range), turned into an onset curve (rises in loudness, every
// ~5 ms), and hits are picked where it peaks above an adaptive threshold.
//
// Tempo: from the spectral flux of the whole sound (how much each of ~36
// frequency bands rises), summed in four regions (bass, low mids, high mids,
// highs) that count the same however loud or wide they are (the one chosen
// in Listen to counts double): kick, snare, hats and chords all have a say,
// not only the loudest or widest drum. A singing voice is left out: its
// pitch is found from the cepstrum of each frame and the bins its harmonics
// are in don't count (vocals come and go off the beat). Every quarter second
// the autocorrelation of the last few seconds scores each tempo of the range
// by its beat, its multiples (bars) and its eighth notes. The scores don't
// pick the tempo directly: a tracker over the tempos (a hidden Markov model)
// keeps the current one and moves when the evidence against it adds up, so a
// breakdown, a dotted-eighth arp or a fill can't flip it, while a new song
// still takes over within seconds. Jumps to 2:3, 3:4 and similar relatives of
// the current tempo, the usual mistakes, need much more evidence.
//
// A beat grid locked to the hits gives steady beats even when some hits are
// missing, and can fire early to make up for the input's latency. It is a
// Kalman filter over when the next beat is and the period, and a few times a
// second it checks which offset of itself lines up best with the onsets (kicks
// counting double), so it can't stay on the off-beats or a syncopated snare it
// happened to start on. The tempo can be locked: the grid keeps following the
// hits, the tempo stays put.

#include <array>
#include <complex>
#include <deque>
#include <mutex>
#include <vector>

struct BeatSettings {
    enum Band { Full, Bass, Mids, Highs };
    int band = Bass;              // what to listen to
    double sensitivity = 0.5;     // 0..1: higher catches quieter hits (and more false ones)
    double minGapMs = 200;        // shortest time between two hits
    double minBpm = 70;           // tempo range: the tempo is folded into it (by octaves)
    double maxBpm = 180;
    double stability = 0.6;       // 0..1: how slowly the tempo follows changes
    double windowSeconds = 6;     // how much audio the tempo is worked out from
    bool locked = true;           // beats on the tempo grid (steady) or on every hit
    double latencyMs = 40;        // locked beats fire this much early (input latency)
    bool operator==(const BeatSettings&) const = default;
};

class BeatTracker {
public:
    explicit BeatTracker(int sampleRate = 48000);

    void setSettings(const BeatSettings& s);
    BeatSettings settings() const;
    void reset(); // forgets the tempo and beat grid (a locked tempo stays)

    // Feeds interleaved stereo samples that started at `time` (seconds). Any
    // thread; calls must be in order.
    void process(const float* stereo, int frames, double time);

    // Beats (their times, seconds) found since the last call.
    std::vector<double> takeBeats();
    double bpm() const;         // 0 until a tempo is found
    double confidence() const;  // 0..1: how clearly the audio has that tempo

    // Lock BPM: the tempo stays at `bpm` (or the current one if 0); the beat
    // grid still follows the hits. Unlocking carries on from it.
    void setTempoLocked(bool locked, double bpm = 0);
    bool tempoLocked() const;
    // x2 / /2: the tempo heard twice as fast or slow (a 60 BPM song read as 120).
    // The octave stays where it was put (outside the BPM range too); returns the tempo.
    double scaleTempo(double factor);
    // Set 1: the user hears a beat right now. If the beat grid is well off it (on the
    // off-beats, say), the grid moves to now; returns whether it moved.
    bool beatIsNow();
    // Find Tempo Again: forgets the tempo, the lock and the octave put by hand.
    void forget();

    // The recent onset curve for a display: strength and threshold (both
    // scaled to the curve's recent maximum), and which frames were hits and
    // beats; `frameSeconds` apart, oldest first.
    struct Trace {
        std::vector<float> strength, threshold;
        std::vector<bool> hit, beat;
        double frameSeconds = 0;
    };
    Trace trace(int frames = 600) const;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void setupFilters();
    void setupSpectrum();
    void setupTempoStates(); // the tempos the tracker can be at; keeps the current one
    static constexpr int kRegions = 4;
    using Flux = std::array<float, kRegions>;
    struct Spectrum {
        std::vector<float> mag;           // FFT magnitudes up to the top band
        std::vector<unsigned char> voice; // bins a held pitch's harmonics are in
    };
    Flux spectralFlux();                    // of the latest FFT frame, per region
    void findVoice(Spectrum& spec);         // marks a voice's harmonics (cepstrum)
    void frame(double energy, const Flux& flux, double time); // one onset frame
    void estimateTempo();
    double refineTempo(const std::vector<double>& o, double bpm) const;
    double gridPeriod() const;
    void kalmanHit(double t);
    void resetGrid();                 // the grid's period from the tempo, its uncertainty afresh
    void choosePhase(const std::vector<double>& curve, double start);
    void emitBeat(double t);

    mutable std::mutex m_mutex;
    BeatSettings m_s;
    int m_rate;
    int m_hop;                        // samples per onset frame
    double m_fps;                     // onset frames per second
    std::vector<Biquad> m_filters;
    // Spectral flux for the tempo.
    int m_fftSize = 2048;
    std::vector<float> m_ring;        // the last m_fftSize input samples
    size_t m_ringPos = 0;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_fft;
    struct Band {
        int from, to;                 // FFT bins [from, to)
        int region;
    };
    std::vector<Band> m_bands;
    std::deque<Spectrum> m_spectra;       // the last few frames
    std::vector<std::complex<float>> m_cepstrum;
    std::array<double, kRegions> m_regionWeight{};
    std::deque<Flux> m_flux;          // tempo onset curves, same frames as m_onset
    // Tempo tracker: probabilities of log-spaced tempos over an octave (all octaves of them).
    std::vector<double> m_states;     // BPM of each state
    std::vector<double> m_belief;
    double m_acc = 0;                 // energy of the frame being filled
    int m_accN = 0;
    double m_accStart = -1;           // time of the frame's first sample
    std::deque<double> m_level;       // recent log energy
    std::deque<float> m_onset;        // onset curve (whole tempo window)
    std::deque<double> m_onsetTime;
    std::deque<float> m_threshold;
    std::deque<bool> m_hitFlags, m_beatFlags;
    double m_lastHit = -1e9;
    int m_sinceTempo = 0;
    double m_bpm = 0, m_confidence = 0;
    bool m_tempoLocked = false;
    bool m_octavePinned = false;      // x2 / /2 was used: keep that octave
    double m_now = 0;                 // audio time processed so far
    double m_kfT = 0;                 // beat grid (a Kalman filter, with m_nextBeat): period, covariance [tt tT Tt TT]
    std::array<double, 4> m_kfP{};
    bool m_phaseByHand = false;       // Set 1 placed the grid: leave it there
    int m_phaseVotes = 0;             // estimates in a row another grid offset was clearly better
    double m_nextBeat = -1;           // next beat of the locked grid (audio time)
    double m_detectDelay = 0;         // how late a hit is seen after it starts (frame + filter)
    double m_lastSound = -1e9;        // last time the input was above the noise floor
    std::vector<double> m_out;        // beats not yet taken
};
