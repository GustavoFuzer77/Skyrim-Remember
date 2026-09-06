#pragma once

namespace SkyrimRemember {

// Creates the session's Rust context and registers the SKSE listeners.
//
// Replaces the old GuardProbe: the lifecycle lives here, classification in
// NpcProfile, perception in NpcObservation, discovery in
// NpcObservationService, and the Rust calls in GuardMemoryAdapter.
bool RegisterRuntime();

} // namespace SkyrimRemember
