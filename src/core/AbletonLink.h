#pragma once

#include <memory>

// Ableton Link: the tempo and beat shared with music apps on the same network
// (Ableton Live, Traktor, Bitwig, phone apps...). Every app in the session
// plays at one tempo with its bars lined up; any of them can change the tempo.
// Without Link in the build (deps/link) available() is false and it does nothing.
class AbletonLink {
public:
    AbletonLink();
    ~AbletonLink();
    AbletonLink(const AbletonLink&) = delete;
    AbletonLink& operator=(const AbletonLink&) = delete;

    static bool available();
    void setEnabled(bool on);
    bool enabled() const;
    int peers() const; // the other apps in the session
    double tempo() const;
    // The session's beat now (bars of 4 line up across apps: beat 0, 4, 8... is a 1).
    double beat() const;
    void setTempo(double bpm);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
