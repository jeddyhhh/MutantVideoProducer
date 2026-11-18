#ifdef MVP_LINK
#include <ableton/Link.hpp> // (first: asio wants the Windows socket headers before anything else)
#endif

#include "core/AbletonLink.h"

#ifdef MVP_LINK

namespace {
constexpr double kQuantum = 4; // a bar of four beats
}

struct AbletonLink::Impl {
    ableton::Link link{120.0};
};

AbletonLink::AbletonLink() : d(std::make_unique<Impl>()) {}
AbletonLink::~AbletonLink() = default;
bool AbletonLink::available() { return true; }
void AbletonLink::setEnabled(bool on) { d->link.enable(on); }
bool AbletonLink::enabled() const { return d->link.isEnabled(); }
int AbletonLink::peers() const { return int(d->link.numPeers()); }
double AbletonLink::tempo() const { return d->link.captureAppSessionState().tempo(); }

double AbletonLink::beat() const
{
    return d->link.captureAppSessionState().beatAtTime(d->link.clock().micros(), kQuantum);
}

void AbletonLink::setTempo(double bpm)
{
    auto state = d->link.captureAppSessionState();
    state.setTempo(bpm, d->link.clock().micros());
    d->link.commitAppSessionState(state);
}

#else

struct AbletonLink::Impl {};
AbletonLink::AbletonLink() = default;
AbletonLink::~AbletonLink() = default;
bool AbletonLink::available() { return false; }
void AbletonLink::setEnabled(bool) {}
bool AbletonLink::enabled() const { return false; }
int AbletonLink::peers() const { return 0; }
double AbletonLink::tempo() const { return 0; }
double AbletonLink::beat() const { return 0; }
void AbletonLink::setTempo(double) {}

#endif
