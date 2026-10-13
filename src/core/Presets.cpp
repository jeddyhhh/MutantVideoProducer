#include "core/Brand.h"
#include "core/Presets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <algorithm>

namespace {

// ---- Factory preset builders ---------------------------------------------------

struct Dev {
    const char* type;
    std::vector<std::pair<const char*, double>> values;
    const char* text = nullptr; // FFmpeg filter chain (a_ffmpeg)
};

EffectInstance build(const Dev& d)
{
    EffectInstance e = makeEffectOfType(QString::fromLatin1(d.type), 0);
    for (const auto& [id, v] : d.values)
        e.params[QString::fromLatin1(id)].value = v;
    if (d.text)
        e.params[QStringLiteral("filter")].text = QString::fromUtf8(d.text);
    return e;
}

QString slug(const QString& name)
{
    QString s;
    for (const QChar c : name.toLower()) {
        if (c.isLetterOrNumber())
            s += c;
        else if (!s.endsWith(QLatin1Char('-')))
            s += QLatin1Char('-');
    }
    while (s.endsWith(QLatin1Char('-')))
        s.chop(1);
    return s.isEmpty() ? QStringLiteral("preset") : s;
}

struct Rack {
    std::vector<std::tuple<const char*, double, std::vector<Dev>>> chains; // name, level dB, devices
    double mix = 100;
};

std::vector<Preset> buildFactory()
{
    std::vector<Preset> list;
    auto single = [&](const char* type, const char* name, const char* description, std::vector<std::pair<const char*, double>> values,
                      const char* text = nullptr) {
        Preset p;
        p.name = QString::fromUtf8(name);
        p.id = QStringLiteral("f:%1/%2").arg(QString::fromLatin1(type), slug(p.name));
        p.description = QString::fromUtf8(description);
        p.effectType = QString::fromLatin1(type);
        p.audio = true;
        p.chain = {build({type, std::move(values), text})};
        list.push_back(std::move(p));
    };
    auto chain = [&](const char* category, const char* name, const char* description, std::vector<Dev> devices) {
        Preset p;
        p.name = QString::fromUtf8(name);
        p.id = QStringLiteral("f:chain/%1").arg(slug(p.name));
        p.category = QString::fromUtf8(category);
        p.description = QString::fromUtf8(description);
        p.audio = true;
        for (const Dev& d : devices)
            p.chain.push_back(build(d));
        list.push_back(std::move(p));
    };
    auto rack = [&](const char* name, const char* description, const Rack& r) {
        Preset p;
        p.name = QString::fromUtf8(name);
        p.id = QStringLiteral("f:chain/%1").arg(slug(p.name));
        p.category = QStringLiteral("Racks");
        p.description = QString::fromUtf8(description);
        p.audio = true;
        EffectInstance rk = makeRack(0);
        rk.params[QStringLiteral("mix")].value = r.mix;
        for (const auto& [chainName, level, devices] : r.chains) {
            EffectChain effects;
            for (const Dev& d : devices)
                effects.push_back(build(d));
            RackChain c = makeRackChain(true, QString::fromUtf8(chainName), std::move(effects));
            c.params[QStringLiteral("level")].value = level;
            rk.chains.push_back(std::move(c));
        }
        p.chain = {rk};
        list.push_back(std::move(p));
    };

    // ---- Single devices --------------------------------------------------------------
    single("a_eq", "Vocal Presence", "Clears low mud and lifts the 3-4 kHz presence range for speech.",
           {{"lowCut", 80}, {"mid1Freq", 250}, {"mid1Gain", -2}, {"mid1Q", 1}, {"mid2Freq", 3500}, {"mid2Gain", 4}, {"mid2Q", 1.2},
            {"highFreq", 10000}, {"highGain", 2}});
    single("a_eq", "Bass Boost", "+6 dB low shelf.", {{"lowFreq", 90}, {"lowGain", 6}});
    single("a_eq", "Treble Boost", "+6 dB high shelf.", {{"highFreq", 6000}, {"highGain", 6}});
    single("a_eq", "Warmth", "A little more low end, a little less top.", {{"lowFreq", 180}, {"lowGain", 3}, {"highFreq", 9000}, {"highGain", -2}});
    single("a_eq", "Clear Muddiness", "Cuts the boxy 300 Hz region.", {{"mid1Freq", 300}, {"mid1Gain", -5}, {"mid1Q", 1.4}});
    single("a_eq", "Air", "Opens up the very top end.", {{"highFreq", 12000}, {"highGain", 5}});
    single("a_eq", "Loudness Smile", "Classic boosted lows and highs.",
           {{"lowFreq", 100}, {"lowGain", 4}, {"mid1Freq", 1000}, {"mid1Gain", -2}, {"mid1Q", 0.7}, {"highFreq", 8000}, {"highGain", 4}});
    single("a_eq", "Rumble Remover", "Low cut at 80 Hz for wind, handling noise and traffic.", {{"lowCut", 80}});
    single("a_eq", "Telephone EQ", "Only the telephone band, with a nasal mid bump.",
           {{"lowCut", 300}, {"highCut", 3400}, {"mid1Freq", 1500}, {"mid1Gain", 5}, {"mid1Q", 0.8}});
    single("a_eq", "Radio Voice", "Tight, forward broadcast voice.",
           {{"lowCut", 120}, {"mid1Freq", 200}, {"mid1Gain", -3}, {"mid2Freq", 3500}, {"mid2Gain", 5}, {"mid2Q", 0.9}, {"highCut", 12000}});

    single("a_filter", "Underwater", "Steep low-pass at 400 Hz.", {{"type", 0}, {"frequency", 400}, {"q", 0.707}, {"slope", 1}});
    single("a_filter", "Next Room", "Low-pass at 1.2 kHz: sound through a wall.", {{"type", 0}, {"frequency", 1200}, {"q", 0.707}, {"slope", 1}});
    single("a_filter", "Tinny", "Steep high-pass at 1 kHz.", {{"type", 1}, {"frequency", 1000}, {"q", 0.707}, {"slope", 1}});
    single("a_filter", "Rumble Filter", "High-pass at 80 Hz.", {{"type", 1}, {"frequency", 80}, {"q", 0.707}, {"slope", 0}});
    single("a_filter", "Telephone Band", "Band-pass around 1 kHz.", {{"type", 2}, {"frequency", 1000}, {"q", 0.5}, {"slope", 1}});
    single("a_filter", "Hum Notch 50 Hz", "Removes 50 Hz mains hum (Europe, Asia, Australia).", {{"type", 3}, {"frequency", 50}, {"q", 10}});
    single("a_filter", "Hum Notch 60 Hz", "Removes 60 Hz mains hum (Americas).", {{"type", 3}, {"frequency", 60}, {"q", 10}});

    single("a_compressor", "Gentle Vocal", "Light, transparent levelling for voice.",
           {{"threshold", -20}, {"ratio", 3}, {"attack", 10}, {"release", 150}, {"knee", 6}, {"makeup", 3}});
    single("a_compressor", "Podcast Leveler", "Evens out loud and quiet speakers.",
           {{"threshold", -24}, {"ratio", 4}, {"attack", 5}, {"release", 120}, {"knee", 6}, {"makeup", 6}});
    single("a_compressor", "Punchy Drums", "Slow attack lets transients through.",
           {{"threshold", -18}, {"ratio", 4}, {"attack", 30}, {"release", 80}, {"knee", 3}, {"makeup", 3}});
    single("a_compressor", "Heavy Squash", "Extreme compression for effect.",
           {{"threshold", -35}, {"ratio", 12}, {"attack", 1}, {"release", 60}, {"knee", 0}, {"makeup", 12}});
    single("a_compressor", "Bus Glue", "Gentle mix-bus compression.",
           {{"threshold", -12}, {"ratio", 2}, {"attack", 30}, {"release", 300}, {"knee", 6}, {"makeup", 1}});

    single("a_limiter", "Broadcast -1 dB", "Peaks never above -1 dBFS.", {{"ceiling", -1}, {"release", 60}});
    single("a_limiter", "Streaming -2 dB", "Extra headroom for lossy streaming codecs.", {{"ceiling", -2}, {"release", 60}});
    single("a_limiter", "Brickwall -0.1 dB", "Maximum level, fast release.", {{"ceiling", -0.1}, {"release", 30}});

    single("a_gate", "Voice Gate", "Silences background between phrases.", {{"threshold", -45}, {"range", -80}, {"attack", 1}, {"hold", 50}, {"release", 150}});
    single("a_gate", "Tight Gate", "Fast, hard gate for drums.", {{"threshold", -35}, {"range", -90}, {"attack", 0.5}, {"hold", 10}, {"release", 50}});
    single("a_gate", "Soft Gate", "Turns noise down rather than off.", {{"threshold", -50}, {"range", -20}, {"attack", 5}, {"hold", 100}, {"release", 400}});

    single("a_reverb", "Small Room", "Short, natural room.", {{"room", 0.3}, {"damping", 0.6}, {"wet", 0.15}, {"dry", 1}, {"width", 0.8}, {"predelay", 5}});
    single("a_reverb", "Large Hall", "Big concert hall.", {{"room", 0.85}, {"damping", 0.35}, {"wet", 0.3}, {"dry", 0.9}, {"width", 1}, {"predelay", 30}});
    single("a_reverb", "Cathedral", "Huge stone space with a long tail.",
           {{"room", 0.98}, {"damping", 0.2}, {"wet", 0.45}, {"dry", 0.7}, {"width", 1}, {"predelay", 60}});
    single("a_reverb", "Bathroom", "Bright, tiled, close.", {{"room", 0.45}, {"damping", 0.1}, {"wet", 0.3}, {"dry", 1}, {"width", 0.6}, {"predelay", 2}});
    single("a_reverb", "Plate", "Smooth studio plate.", {{"room", 0.7}, {"damping", 0.15}, {"wet", 0.25}, {"dry", 1}, {"width", 1}, {"predelay", 0}});
    single("a_reverb", "Subtle Ambience", "Just a hint of space.", {{"room", 0.2}, {"damping", 0.5}, {"wet", 0.1}, {"dry", 1}, {"width", 1}, {"predelay", 8}});

    single("a_delay", "Slapback", "Single short echo, 50s rock'n'roll.", {{"time", 110}, {"feedback", 0.1}, {"mix", 0.3}, {"damping", 6000}});
    single("a_delay", "Ping-Pong Echo", "Echoes bounce left and right.", {{"time", 375}, {"feedback", 0.45}, {"mix", 0.35}, {"damping", 7000}, {"pingpong", 1}});
    single("a_delay", "Canyon Echo", "Long, fading echoes.", {{"time", 800}, {"feedback", 0.55}, {"mix", 0.4}, {"damping", 4000}});
    single("a_delay", "Dub Delay", "Dark, repeating dub echoes.", {{"time", 500}, {"feedback", 0.75}, {"mix", 0.4}, {"damping", 2500}});
    single("a_delay", "Doubler", "Very short delay that thickens a voice.", {{"time", 25}, {"feedback", 0}, {"mix", 0.5}, {"damping", 20000}});

    single("a_chorus", "Subtle Chorus", "Gentle thickening.", {{"rate", 0.6}, {"depth", 2}, {"delay", 15}, {"feedback", 0}, {"mix", 0.3}});
    single("a_chorus", "Lush Chorus", "Wide, shimmering chorus.", {{"rate", 1.2}, {"depth", 6}, {"delay", 20}, {"feedback", 0.2}, {"mix", 0.5}});
    single("a_chorus", "Vibrato", "Pitch wobble only (fully wet).", {{"rate", 5}, {"depth", 2}, {"delay", 5}, {"feedback", 0}, {"mix", 1}});
    single("a_chorus", "Seasick", "Slow, deep warble.", {{"rate", 1.5}, {"depth", 8}, {"delay", 10}, {"feedback", 0}, {"mix", 1}});

    single("a_flanger", "Jet Flanger", "Classic slow jet sweep.", {{"rate", 0.15}, {"depth", 4}, {"delay", 2}, {"feedback", 0.8}, {"mix", 0.5}});
    single("a_flanger", "Subtle Flange", "Gentle movement.", {{"rate", 0.3}, {"depth", 1.5}, {"delay", 1}, {"feedback", 0.4}, {"mix", 0.3}});
    single("a_flanger", "Metallic", "Resonant, robotic comb.", {{"rate", 2}, {"depth", 0.5}, {"delay", 3}, {"feedback", 0.9}, {"mix", 0.6}});

    single("a_stereo", "Mono", "Both channels mixed to mono.", {{"mono", 1}});
    single("a_stereo", "Wide", "Wider stereo image.", {{"width", 1.6}});
    single("a_stereo", "Narrow", "Narrower stereo image.", {{"width", 0.5}});
    single("a_stereo", "Swap Channels", "Left and right swapped.", {{"swap", 1}});

    single("a_denoise", "Light Cleanup", "Subtle steady-noise reduction.", {{"amount", 6}, {"floor", -50}});
    single("a_denoise", "Hiss Removal", "Removes tape / preamp hiss.", {{"amount", 12}, {"floor", -40}});
    single("a_denoise", "Heavy Noise Reduction", "Strong reduction; may sound processed.", {{"amount", 24}, {"floor", -35}});
    single("a_denoise", "Changing Noise", "For noise that changes over time.", {{"amount", 12}, {"floor", -40}, {"adaptive", 1}});

    single("a_pitch", "Chipmunk", "Up 7 semitones.", {{"semitones", 7}});
    single("a_pitch", "Octave Up", "Up 12 semitones.", {{"semitones", 12}});
    single("a_pitch", "Deep Voice", "Down 4 semitones.", {{"semitones", -4}});
    single("a_pitch", "Monster", "Down 9 semitones.", {{"semitones", -9}});
    single("a_pitch", "Octave Down", "Down 12 semitones.", {{"semitones", -12}});

    single("a_gain", "Boost 6 dB", "+6 dB.", {{"gain", 6}});
    single("a_gain", "Cut 6 dB", "-6 dB.", {{"gain", -6}});
    single("a_gain", "Invert Phase", "Flips the polarity.", {{"invert", 1}});

    single("a_ffmpeg", "Loudness Normalize (EBU R128)", "-16 LUFS loudness, -1.5 dB true peak.", {}, "loudnorm=I=-16:TP=-1.5:LRA=11");
    single("a_ffmpeg", "Dynamic Normalizer", "Evens out loudness over time.", {}, "dynaudnorm=f=150:g=15");
    single("a_ffmpeg", "De-esser", "Tames harsh 's' sounds.", {}, "deesser=i=0.6");
    single("a_ffmpeg", "Bitcrusher", "Lo-fi bit reduction.", {}, "acrusher=bits=6:mode=log:aa=1");
    single("a_ffmpeg", "Tremolo", "Volume wobble.", {}, "tremolo=f=6:d=0.7");
    single("a_ffmpeg", "Robot Voice", "Flattens the phase for a robotic voice.", {},
           "afftfilt=real='hypot(re,im)*sin(0)':imag='hypot(re,im)*cos(0)':win_size=512:overlap=0.75");
    single("a_ffmpeg", "Whisper", "Randomised phase turns voice into a whisper.", {},
           "afftfilt=real='hypot(re,im)*cos((random(0)*2-1)*2*3.14)':imag='hypot(re,im)*sin((random(1)*2-1)*2*3.14)':win_size=128:overlap=0.8");
    single("a_ffmpeg", "Soft Clip", "Warm saturation.", {}, "asoftclip=type=tanh");
    single("a_ffmpeg", "Crystalizer", "Adds sparkle and detail.", {}, "crystalizer=i=2");
    single("a_ffmpeg", "Phaser", "Swirling phaser.", {}, "aphaser=type=t:speed=0.5");

    // ---- Chains ---------------------------------------------------------------------------
    const Dev hp80{"a_filter", {{"type", 1}, {"frequency", 80}, {"q", 0.707}, {"slope", 0}}};
    const Dev limit1{"a_limiter", {{"ceiling", -1}, {"release", 60}}};
    const Dev gentleComp{"a_compressor", {{"threshold", -20}, {"ratio", 3}, {"attack", 10}, {"release", 150}, {"knee", 6}, {"makeup", 3}}};
    const Dev podcastComp{"a_compressor", {{"threshold", -24}, {"ratio", 4}, {"attack", 5}, {"release", 120}, {"knee", 6}, {"makeup", 6}}};
    const Dev squash{"a_compressor", {{"threshold", -35}, {"ratio", 12}, {"attack", 1}, {"release", 60}, {"knee", 0}, {"makeup", 12}}};
    const Dev busGlue{"a_compressor", {{"threshold", -12}, {"ratio", 2}, {"attack", 30}, {"release", 300}, {"knee", 6}, {"makeup", 1}}};
    const Dev presence{"a_eq", {{"lowCut", 80}, {"mid1Freq", 250}, {"mid1Gain", -2}, {"mid2Freq", 3500}, {"mid2Gain", 4}, {"mid2Q", 1.2},
                                {"highFreq", 10000}, {"highGain", 2}}};
    const Dev smallRoom{"a_reverb", {{"room", 0.3}, {"damping", 0.6}, {"wet", 0.15}, {"dry", 1}, {"width", 0.8}, {"predelay", 5}}};
    const Dev hall{"a_reverb", {{"room", 0.85}, {"damping", 0.35}, {"wet", 0.3}, {"dry", 0.9}, {"width", 1}, {"predelay", 30}}};
    const Dev cathedral{"a_reverb", {{"room", 0.98}, {"damping", 0.2}, {"wet", 0.45}, {"dry", 0.7}, {"width", 1}, {"predelay", 60}}};

    chain("Voice", "Podcast Voice", "Rumble filter, light noise cleanup, levelling, presence EQ and a -1 dB limiter.",
          {hp80, {"a_denoise", {{"amount", 6}, {"floor", -50}}}, podcastComp, presence, limit1});
    chain("Voice", "Radio DJ", "Heavily compressed, bright and bassy broadcast voice.",
          {{"a_compressor", {{"threshold", -28}, {"ratio", 6}, {"attack", 3}, {"release", 120}, {"knee", 3}, {"makeup", 8}}},
           {"a_eq", {{"lowFreq", 100}, {"lowGain", 4}, {"mid2Freq", 3500}, {"mid2Gain", 4}, {"highFreq", 10000}, {"highGain", 2}}},
           {"a_limiter", {{"ceiling", -0.5}, {"release", 60}}}});
    chain("Voice", "Clean Up Dialogue", "Rumble filter, soft gate, hiss removal, de-esser and gentle compression.",
          {hp80, {"a_gate", {{"threshold", -50}, {"range", -20}, {"attack", 5}, {"hold", 100}, {"release", 400}}},
           {"a_denoise", {{"amount", 12}, {"floor", -40}}}, {"a_ffmpeg", {}, "deesser=i=0.5"}, gentleComp});
    chain("Voice", "Audiobook Narration", "Warm, even and close.",
          {{"a_filter", {{"type", 1}, {"frequency", 70}, {"q", 0.707}, {"slope", 0}}}, gentleComp,
           {"a_eq", {{"lowFreq", 180}, {"lowGain", 2}, {"highFreq", 9000}, {"highGain", -1}}}, limit1});
    chain("Voice", "Chipmunk", "High, squeaky voice.", {{"a_pitch", {{"semitones", 7}}}, {"a_eq", {{"highFreq", 6000}, {"highGain", 2}}}});
    chain("Voice", "Monster", "Deep, growling voice in a small room.",
          {{"a_pitch", {{"semitones", -9}}}, {"a_eq", {{"lowFreq", 90}, {"lowGain", 5}}}, smallRoom});
    chain("Voice", "Robot", "Phase-flattened, metallic robot voice.",
          {{"a_ffmpeg", {}, "afftfilt=real='hypot(re,im)*sin(0)':imag='hypot(re,im)*cos(0)':win_size=512:overlap=0.75"},
           {"a_flanger", {{"rate", 2}, {"depth", 0.5}, {"delay", 3}, {"feedback", 0.9}, {"mix", 0.4}}},
           {"a_eq", {{"lowCut", 150}, {"mid2Freq", 2000}, {"mid2Gain", 3}}}});
    chain("Voice", "Whisper", "Breathy whisper.",
          {{"a_ffmpeg", {}, "afftfilt=real='hypot(re,im)*cos((random(0)*2-1)*2*3.14)':imag='hypot(re,im)*sin((random(1)*2-1)*2*3.14)':win_size=128:overlap=0.8"},
           {"a_eq", {{"lowCut", 300}}}});

    chain("Character", "Telephone", "Narrow band, squashed and slightly crunchy.",
          {{"a_filter", {{"type", 2}, {"frequency", 1000}, {"q", 0.5}, {"slope", 1}}}, squash, {"a_ffmpeg", {}, "acrusher=bits=10:mode=log:aa=1:mix=0.5"}});
    chain("Character", "Walkie-Talkie", "Harsh, crushed radio transmission.",
          {{"a_filter", {{"type", 2}, {"frequency", 1500}, {"q", 1.2}, {"slope", 1}}}, squash,
           {"a_ffmpeg", {}, "acrusher=bits=6:samples=2:mode=log:aa=1"}, {"a_gain", {{"gain", -3}}}});
    chain("Character", "Megaphone", "Loud, distorted megaphone.",
          {{"a_filter", {{"type", 1}, {"frequency", 500}, {"q", 0.707}, {"slope", 1}}}, {"a_filter", {{"type", 0}, {"frequency", 4000}, {"q", 0.707}, {"slope", 1}}},
           {"a_gain", {{"gain", 6}}}, {"a_ffmpeg", {}, "asoftclip=type=atan"}, podcastComp});
    chain("Character", "AM Radio", "Band-limited, compressed old radio.",
          {{"a_eq", {{"lowCut", 200}, {"highCut", 5000}, {"mid1Freq", 1500}, {"mid1Gain", 5}, {"mid1Q", 0.8}}}, podcastComp});
    chain("Character", "Lo-Fi", "Bit-crushed and warbly.",
          {{"a_eq", {{"lowCut", 150}, {"highCut", 6000}}}, {"a_ffmpeg", {}, "acrusher=bits=8:mode=log:aa=1:mix=0.8"},
           {"a_chorus", {{"rate", 0.6}, {"depth", 2}, {"delay", 15}, {"feedback", 0}, {"mix", 0.3}}}});
    chain("Character", "Underwater", "Muffled and wobbling.",
          {{"a_filter", {{"type", 0}, {"frequency", 400}, {"q", 0.707}, {"slope", 1}}},
           {"a_chorus", {{"rate", 0.3}, {"depth", 8}, {"delay", 20}, {"feedback", 0.3}, {"mix", 0.6}}},
           {"a_reverb", {{"room", 0.7}, {"damping", 0.9}, {"wet", 0.35}, {"dry", 0.8}, {"width", 1}, {"predelay", 10}}}});
    chain("Character", "Next Room", "Heard through a wall.",
          {{"a_filter", {{"type", 0}, {"frequency", 900}, {"q", 0.707}, {"slope", 1}}}, smallRoom, {"a_gain", {{"gain", -6}}}});

    chain("Space", "Stadium PA", "Slapback from the far stand and a big hall.",
          {{"a_delay", {{"time", 140}, {"feedback", 0.15}, {"mix", 0.25}, {"damping", 6000}}}, hall,
           {"a_eq", {{"mid2Freq", 2500}, {"mid2Gain", 3}}}});
    chain("Space", "Cathedral", "Huge stone space.", {{"a_eq", {{"lowCut", 100}}}, cathedral});
    chain("Space", "Concert Hall", "Natural large hall.", {hall});
    chain("Space", "Cave Echo", "Distant echoes in a cave.",
          {{"a_delay", {{"time", 800}, {"feedback", 0.55}, {"mix", 0.4}, {"damping", 4000}}},
           {"a_reverb", {{"room", 0.8}, {"damping", 0.5}, {"wet", 0.3}, {"dry", 1}, {"width", 1}, {"predelay", 20}}}});
    chain("Space", "Space Echo", "Ping-pong dub echoes into a plate.",
          {{"a_delay", {{"time", 500}, {"feedback", 0.7}, {"mix", 0.4}, {"damping", 2500}, {"pingpong", 1}}},
           {"a_reverb", {{"room", 0.7}, {"damping", 0.15}, {"wet", 0.25}, {"dry", 1}, {"width", 1}, {"predelay", 0}}}});
    chain("Space", "Haunted", "Low, shimmering and distant.",
          {{"a_pitch", {{"semitones", -3}}}, {"a_chorus", {{"rate", 1.2}, {"depth", 6}, {"delay", 20}, {"feedback", 0.2}, {"mix", 0.5}}}, cathedral});

    chain("Cleanup & Mastering", "Remove Hum (50 Hz)", "Notches 50 Hz hum and its first harmonic.",
          {{"a_filter", {{"type", 3}, {"frequency", 50}, {"q", 10}}}, {"a_filter", {{"type", 3}, {"frequency", 100}, {"q", 10}}},
           {"a_filter", {{"type", 1}, {"frequency", 30}, {"q", 0.707}, {"slope", 0}}}});
    chain("Cleanup & Mastering", "Remove Hum (60 Hz)", "Notches 60 Hz hum and its first harmonic.",
          {{"a_filter", {{"type", 3}, {"frequency", 60}, {"q", 10}}}, {"a_filter", {{"type", 3}, {"frequency", 120}, {"q", 10}}},
           {"a_filter", {{"type", 1}, {"frequency", 30}, {"q", 0.707}, {"slope", 0}}}});
    chain("Cleanup & Mastering", "Noise Cleanup", "Rumble filter, hiss removal and a soft gate.",
          {{"a_filter", {{"type", 1}, {"frequency", 60}, {"q", 0.707}, {"slope", 0}}}, {"a_denoise", {{"amount", 12}, {"floor", -40}}},
           {"a_gate", {{"threshold", -50}, {"range", -20}, {"attack", 5}, {"hold", 100}, {"release", 400}}}});
    chain("Cleanup & Mastering", "Broadcast Loudness", "Glue compression, -16 LUFS loudness and a -1 dB limiter.",
          {busGlue, {"a_ffmpeg", {}, "loudnorm=I=-16:TP=-1.5:LRA=11"}, limit1});
    chain("Cleanup & Mastering", "Music Under Voice", "Ducks a music bed and clears room for speech.",
          {{"a_eq", {{"mid1Freq", 1500}, {"mid1Gain", -6}, {"mid1Q", 0.6}}}, {"a_gain", {{"gain", -10}}}});
    chain("Cleanup & Mastering", "Master Glue", "Gentle smile EQ, bus compression and a limiter.",
          {{"a_eq", {{"lowFreq", 100}, {"lowGain", 2}, {"highFreq", 8000}, {"highGain", 2}}}, busGlue, {"a_limiter", {{"ceiling", -0.3}, {"release", 60}}}});

    rack("Parallel Compression", "New York compression: the dry signal plus a heavily squashed copy.",
         Rack{{{"Dry", 0.0, {}}, {"Crushed", -6.0, {squash}}}});
    rack("Wide Doubler", "The dry voice plus a slightly detuned, delayed and widened copy.",
         Rack{{{"Dry", 0.0, {}},
               {"Double", -6.0,
                {{"a_delay", {{"time", 25}, {"feedback", 0}, {"mix", 1}, {"damping", 20000}}}, {"a_pitch", {{"cents", 8}}}, {"a_stereo", {{"width", 1.8}}}}}}});
    rack("Reverb Send", "Dry signal plus a fully wet reverb chain you can turn up or down.",
         Rack{{{"Dry", 0.0, {}}, {"Reverb", -8.0, {{"a_reverb", {{"room", 0.85}, {"damping", 0.35}, {"wet", 1}, {"dry", 0}, {"width", 1}, {"predelay", 30}}}}}}});

    // ---- Video: glitch presets (single devices, then chains) ---------------------------
    auto video = [&](size_t from) {
        for (size_t i = from; i < list.size(); ++i)
            list[i].audio = false;
    };
    const size_t firstVideo = list.size();
    single("chromatic_aberration", "Cheap Lens", "Strong radial fringes that grow fast towards the corners.",
           {{"amount", 18}, {"falloff", 2.5}});
    single("chromatic_aberration", "Breathing Prism", "A linear split that wobbles in and out, with green drifting too.",
           {{"mode", 1}, {"amount", 14}, {"angle", 20}, {"greenShift", 0.5}, {"wobble", 0.8}, {"wobbleSpeed", 1.5}});
    single("chromatic_aberration", "Off-Centre Lens", "Lens fringing around a point near the top left.",
           {{"amount", 22}, {"centerX", 0.25}, {"centerY", 0.3}, {"falloff", 1.5}});
    single("rgb_split", "Shaky Signal", "Channels jump around 12 times a second.", {{"redX", -8}, {"blueX", 8}, {"jitter", 25}, {"rate", 12}});
    single("rgb_split", "Vertical Ghosts", "Red up, blue down, wrapping around the frame.",
           {{"redX", 0}, {"redY", -40}, {"blueX", 0}, {"blueY", 40}, {"edge", 1}});
    single("pixel_sort", "Melting Highlights", "Bright areas pour down in long streaks.",
           {{"lower", 0.55}, {"upper", 1}, {"direction", 1}, {"reverse", 1}});
    single("pixel_sort", "Shattered Rows", "Mid tones sorted in short, random streaks that change every frame.",
           {{"lower", 0.2}, {"upper", 0.8}, {"breaks", 0.03}, {"animated", 1}});
    single("pixel_sort", "Hue Bands", "Colourful pixels sorted by hue.", {{"thresholdBy", 2}, {"lower", 0.35}, {"upper", 1}, {"sortBy", 1}});
    single("datamosh", "Heavy Melt", "Motion pushed twice as far, cleaned up every 4 seconds.", {{"amount", 0.97}, {"motionScale", 2}, {"autoClean", 4}});
    single("datamosh", "Bloom Burst", "Repeats the motion; a clean frame every 2 seconds.", {{"mode", 1}, {"autoClean", 2}});
    single("block_glitch", "Corrupt Stream", "Wide blocks with static and inverted colours.",
           {{"amount", 0.35}, {"aspect", 4}, {"noise", 0.25}, {"colorGlitch", 0.6}, {"colorMode", 2}});
    single("block_glitch", "Tall Tears", "Narrow blocks that jump up and down.", {{"amount", 0.3}, {"aspect", 0.4}, {"verticalShift", 1}, {"maxShift", 40}});
    single("slice_glitch", "Vertical Tear", "Columns slide up and down with colour fringes.", {{"direction", 1}, {"amount", 0.5}, {"rgbSplit", 10}});
    single("slice_glitch", "Signal Drop", "Torn slices with glitched colours and black gaps.",
           {{"amount", 0.6}, {"edge", 2}, {"colorGlitch", 0.5}, {"maxShift", 300}});
    single("jpeg_glitch", "Macroblock Hell", "16 px blocks at rock-bottom quality, block colours drifting.",
           {{"quality", 4}, {"blockScale", 1}, {"corruption", 0.2}, {"corruptWhat", 1}});
    single("jpeg_glitch", "Colour Smear", "Fine detail, but the colour is crushed into big blocks.", {{"quality", 40}, {"chromaQuality", 2}});
    single("databend", "Wrong Stride", "Read with the wrong line length: the picture skews and repeats.", {{"mode", 5}, {"stride", 9}});
    single("databend", "Byte Shift", "Colours rotate between channels and slide sideways.", {{"mode", 4}, {"delay", 4001}});
    single("databend", "Stuck Chunks", "Random chunks repeat the bytes before them.", {{"mode", 6}, {"feedback", 0.4}, {"chunk", 4000}});
    single("bitcrush", "8-bit Fold", "Few bits, bright colours folding back down.", {{"bits", 3}, {"gain", 1.4}, {"fold", 1}});
    single("bitcrush", "Toxic Channels", "Red 2 bits, green 4, blue 1.", {{"separate", 1}, {"bits", 2}, {"bitsG", 4}, {"bitsB", 1}, {"gain", 1.2}, {"wrap", 0}});
    single("pixel_stretch", "Scanner", "The smear line sweeps back and forth.", {{"sweep", 0.8}, {"sweepSpeed", 0.4}, {"length", 0.5}});
    single("pixel_stretch", "Torn Smear", "A jittery smear that stops half way.", {{"direction", 2}, {"jitter", 60}, {"rate", 8}, {"length", 0.5}});
    single("channel_swap", "Negative Green", "Channels unchanged, green inverted.", {{"order", 5}, {"invertG", 1}});
    single("time_split", "Wide Ghosts", "Blue now, green and red far behind.", {{"delay", 3}, {"order", 5}, {"curve", 2}});
    single("stutter", "Hiccups", "12 fps with random freezes.", {{"fps", 12}, {"freeze", 0.15}, {"maxFreeze", 10}});
    single("strobe", "Rainbow Strobe", "Half-strength flashes in rainbow colours.", {{"rate", 8}, {"duty", 0.25}, {"mode", 5}, {"opacity", 0.5}});
    single("strobe", "Random Flicker", "Black & white flashes that skip at random.", {{"rate", 12}, {"duty", 0.4}, {"mode", 6}, {"chance", 0.4}});
    single("block_glitch", "Glitch Box", "Corruption only inside a box in the middle.", {{"amount", 0.5}, {"fxMask", 1}, {"fxFeather", 0.02}});
    single("pixel_sort", "Sort the Shadows", "Sorting only in the dark areas.", {{"lower", 0}, {"upper", 1}, {"fxMask", 4}, {"fxThreshold", 0.35}});

    chain("Glitch", "Broken Stream", "Blocks, torn slices and compression artifacts together.",
          {{"jpeg_glitch", {{"quality", 12}, {"corruption", 0.1}}},
           {"block_glitch", {{"amount", 0.2}, {"noise", 0.2}}},
           {"slice_glitch", {{"amount", 0.3}, {"colorGlitch", 0.3}}}});
    chain("Glitch", "Datamosh Trip", "Melting motion with lens fringes and hue-sorted streaks.",
          {{"datamosh", {{"amount", 0.95}, {"autoClean", 3}}},
           {"chromatic_aberration", {{"amount", 12}, {"wobble", 0.5}}},
           {"pixel_sort", {{"thresholdBy", 1}, {"lower", 0.6}, {"upper", 0.9}, {"fxMix", 0.7}}}});
    chain("Glitch", "Panic Attack", "Stutter, strobe and jittery RGB split.",
          {{"stutter", {{"fps", 10}, {"freeze", 0.2}}},
           {"rgb_split", {{"jitter", 30}, {"rate", 15}}},
           {"strobe", {{"rate", 4}, {"duty", 0.15}, {"mode", 1}, {"chance", 0.6}}}});
    chain("Glitch", "Corrupted Tape", "Byte shift colours, stride tearing and bitcrush.",
          {{"databend", {{"mode", 4}, {"delay", 2501}, {"mix", 0.6}}},
           {"bitcrush", {{"bits", 4}, {"gain", 1.3}}},
           {"slice_glitch", {{"direction", 1}, {"amount", 0.25}}}});

    // ---- Video: colour grading ------------------------------------------------------------
    single("curves", "S-Curve Contrast", "Darker darks and brighter lights: punchier contrast.", {{"darks", -0.07}, {"lights", 0.07}});
    single("curves", "Faded Film", "Lifted, matte blacks and soft highlights.", {{"shadows", 0.12}, {"darks", 0.04}, {"highlights", -0.08}});
    single("curves", "Cross Process", "Slide film developed as negative: green-yellow highlights, blue shadows.",
           {{"darks", -0.05}, {"lights", 0.05}, {"redShadows", -0.05}, {"redHighlights", 0.08}, {"greenMidtones", 0.04},
            {"blueShadows", 0.12}, {"blueHighlights", -0.15}});
    single("curves", "Bright and Airy", "Open midtones and gentle highlights.", {{"midtones", 0.08}, {"lights", 0.05}, {"highlights", -0.03}});
    single("color_wheels", "Teal and Orange", "Shadows towards teal, highlights towards orange.",
           {{"liftHue", 190}, {"liftAmount", 0.35}, {"gainHue", 30}, {"gainAmount", 0.3}});
    single("color_wheels", "Warm Sunset", "Golden highlights, warm midtones.", {{"gammaHue", 35}, {"gammaAmount", 0.25}, {"gainHue", 40}, {"gainAmount", 0.3}});
    single("color_wheels", "Cool Moonlight", "Blue shadows and midtones, slightly darker.",
           {{"lift", -0.05}, {"liftHue", 220}, {"liftAmount", 0.3}, {"gammaHue", 215}, {"gammaAmount", 0.25}, {"gain", -0.05}});
    single("color_wheels", "Matrix Green", "Everything pushed towards green.", {{"liftHue", 120}, {"liftAmount", 0.25}, {"gammaHue", 110}, {"gammaAmount", 0.3}});
    chain("Color", "Blockbuster Grade", "Contrast curve plus teal shadows and orange highlights.",
          {{"curves", {{"darks", -0.06}, {"lights", 0.05}}}, {"color_wheels", {{"liftHue", 190}, {"liftAmount", 0.3}, {"gainHue", 30}, {"gainAmount", 0.25}}}});
    chain("Color", "Vintage Film", "Faded blacks, warm midtones, a little less saturation.",
          {{"curves", {{"shadows", 0.1}, {"highlights", -0.06}}}, {"color_wheels", {{"gammaHue", 40}, {"gammaAmount", 0.2}}},
           {"hsl", {{"saturation", -0.25}}}});
    video(firstVideo);
    return list;
}

QJsonObject presetToJson(const Preset& p)
{
    QJsonObject o;
    // Retain the disk identifier so saved presets work across the product rename.
    o[QStringLiteral("format")] = QStringLiteral("montage-preset");
    o[QStringLiteral("version")] = 1;
    o[QStringLiteral("name")] = p.name;
    o[QStringLiteral("audio")] = p.audio;
    o[QStringLiteral("effectType")] = p.effectType;
    QJsonArray chain;
    for (const EffectInstance& e : p.chain)
        chain.append(effectToJson(e));
    o[QStringLiteral("chain")] = chain;
    return o;
}

std::optional<Preset> presetFromJson(const QJsonObject& o, const QString& fileId)
{
    if (o[QStringLiteral("format")].toString() != QLatin1String("montage-preset"))
        return std::nullopt;
    Preset p;
    p.id = QStringLiteral("u:") + fileId;
    p.name = o[QStringLiteral("name")].toString();
    p.audio = o[QStringLiteral("audio")].toBool(true);
    p.user = true;
    p.effectType = o[QStringLiteral("effectType")].toString();
    p.category = QStringLiteral("User");
    for (const auto& v : o[QStringLiteral("chain")].toArray())
        p.chain.push_back(effectFromJson(v.toObject()));
    if (p.name.isEmpty() || p.chain.empty() || (!p.effectType.isEmpty() && (p.chain.size() != 1 || p.chain.front().type != p.effectType)))
        return std::nullopt;
    return p;
}

} // namespace

const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> list = buildFactory();
    return list;
}

EffectChain instantiatePreset(const Preset& preset, const std::function<int()>& allocId)
{
    EffectChain chain = preset.chain;
    reassignEffectIds(chain, allocId);
    return chain;
}

bool applyPresetTo(const Preset& preset, EffectInstance& device, const std::function<int()>& allocId)
{
    if (preset.isChain() || preset.chain.size() != 1 || preset.chain.front().type != device.type)
        return false;
    EffectChain copy = instantiatePreset(preset, allocId);
    device.params = copy.front().params;
    device.chains = copy.front().chains;
    return true;
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

PresetLibrary& PresetLibrary::instance()
{
    static PresetLibrary lib;
    return lib;
}

PresetLibrary::PresetLibrary()
{
    m_dir = brand::storageLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/presets");
    reload();
}

void PresetLibrary::setDirectory(const QString& dir)
{
    m_dir = dir;
    reload();
    emit changed();
}

void PresetLibrary::reload()
{
    m_user.clear();
    const QDir dir(m_dir);
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (auto p = presetFromJson(doc.object(), QFileInfo(file).completeBaseName()))
            m_user.push_back(std::move(*p));
    }
    std::stable_sort(m_user.begin(), m_user.end(), [](const Preset& a, const Preset& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
}

std::vector<Preset> PresetLibrary::all(bool audio) const
{
    std::vector<Preset> out;
    for (const Preset& p : factoryPresets())
        if (p.audio == audio)
            out.push_back(p);
    for (const Preset& p : m_user)
        if (p.audio == audio)
            out.push_back(p);
    return out;
}

std::vector<Preset> PresetLibrary::forEffect(const QString& effectType) const
{
    std::vector<Preset> out;
    for (const Preset& p : factoryPresets())
        if (p.effectType == effectType)
            out.push_back(p);
    for (const Preset& p : m_user)
        if (p.effectType == effectType)
            out.push_back(p);
    return out;
}

std::vector<Preset> PresetLibrary::chains(bool audio) const
{
    std::vector<Preset> out;
    for (const Preset& p : all(audio))
        if (p.isChain())
            out.push_back(p);
    return out;
}

std::optional<Preset> PresetLibrary::find(const QString& id) const
{
    for (const Preset& p : factoryPresets())
        if (p.id == id)
            return p;
    for (const Preset& p : m_user)
        if (p.id == id)
            return p;
    return std::nullopt;
}

std::optional<Preset> PresetLibrary::findUserByName(const QString& name, bool audio) const
{
    for (const Preset& p : m_user)
        if (p.audio == audio && p.name.compare(name, Qt::CaseInsensitive) == 0)
            return p;
    return std::nullopt;
}

QString PresetLibrary::save(const QString& name, bool audio, const QString& effectType, const EffectChain& chain, QString* error)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || chain.empty()) {
        if (error)
            *error = QStringLiteral("A preset needs a name and at least one device.");
        return {};
    }
    QDir().mkpath(m_dir);
    // Same name (and kind): overwrite that file; else a new unique file name.
    QString fileId;
    if (auto existing = findUserByName(trimmed, audio))
        fileId = existing->id.mid(2);
    else {
        const QString base = slug(trimmed) + (audio ? QStringLiteral("-audio") : QStringLiteral("-video"));
        fileId = base;
        for (int i = 2; QFile::exists(QDir(m_dir).filePath(fileId + QStringLiteral(".json"))); ++i)
            fileId = QStringLiteral("%1-%2").arg(base).arg(i);
    }
    Preset p;
    p.name = trimmed;
    p.audio = audio;
    p.effectType = effectType;
    p.chain = chain;
    reassignEffectIds(p.chain, [n = 1]() mutable { return n++; }); // stable placeholder ids in the file
    QFile f(QDir(m_dir).filePath(fileId + QStringLiteral(".json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(QJsonDocument(presetToJson(p)).toJson()) < 0) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(f.fileName());
        return {};
    }
    f.close();
    reload();
    emit changed();
    return QStringLiteral("u:") + fileId;
}

bool PresetLibrary::remove(const QString& id)
{
    if (!id.startsWith(QLatin1String("u:")))
        return false;
    const bool ok = QFile::remove(QDir(m_dir).filePath(id.mid(2) + QStringLiteral(".json")));
    reload();
    emit changed();
    return ok;
}
