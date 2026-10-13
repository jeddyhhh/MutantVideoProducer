#pragma once

// What an effects panel is showing: an effect chain (event, track, video
// output, audio master: the device chain view) or an event's transition,
// generated media or pan/crop (the effect editor window).
struct FxTarget {
    enum class Kind { None, Clip, Track, Output, TransitionIn, TransitionOut, Generator, PanCrop, AudioMaster, Live };
    Kind kind = Kind::None;
    int id = 0; // clip id (Clip, Transition*, Generator, PanCrop), track id (Track), Live: 0 = Live FX, 1..16 = pad

    bool isChain() const
    {
        return kind == Kind::Clip || kind == Kind::Track || kind == Kind::Output || kind == Kind::AudioMaster || kind == Kind::Live;
    }
    bool operator==(const FxTarget&) const = default;
};
