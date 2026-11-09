#pragma once

namespace SkyrimRemember {

// Creates the session's memory store and registers the SKSE listeners.
//
// Replaces the old GuardProbe: the lifecycle lives here, classification in
// NpcProfile, perception in NpcObservation, discovery in
// NpcObservationService, and universal recognition in AppearanceMemoryService.
bool RegisterRuntime();

} // namespace SkyrimRemember
