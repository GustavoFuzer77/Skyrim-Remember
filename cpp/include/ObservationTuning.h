#pragma once

#include <chrono>
#include <cstddef>

// Every number that governs how often and how hard the observation scan works.
// These are starting values chosen before measuring in game; the scan logs the
// aggregate metrics needed to calibrate them.
namespace SkyrimRemember::ObservationTuning {

// Farthest an NPC may be from the Player and still observe it.
inline constexpr float maximumObservationDistance = 4096.0F;

// Real time between two scan slices. Real time, not game time: this governs
// CPU cost, while game time governs memory and decay.
inline constexpr std::chrono::milliseconds scanInterval{500};

// Most actors whose expensive checks — detection level and line of sight — may
// run in one slice. Actors whose profile is disabled do not spend this budget,
// because classifying them is cheap.
inline constexpr std::size_t actorBudgetPerScan = 16;

// Real time before the same observer is examined again. An appearance change
// pierces it: a discrete event must reach every watcher at once.
inline constexpr std::chrono::seconds observerCooldown{2};

// How often the aggregated scan metrics reach the log. One line per interval,
// never one line per actor.
inline constexpr std::chrono::seconds metricsInterval{10};

// Observers not seen for this long are dropped from the cooldown table, so a
// long session does not accumulate an entry per actor ever encountered.
inline constexpr std::chrono::seconds observerRetention{60};

} // namespace SkyrimRemember::ObservationTuning
