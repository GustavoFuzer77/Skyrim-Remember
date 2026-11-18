#pragma once

namespace SkyrimRemember {

class DebugBoard;

namespace DebugPanel {

// Outcome of trying to attach the panel to SKSE Menu Framework.
enum class Registration { Registered, FrameworkMissing };

// Adds the "Skyrim Remember" page to SKSE Menu Framework's Mod Control Panel
// and the live overlay drawn over the HUD. Without the framework installed this
// does nothing and the mod runs exactly as before.
//
// Call once, after every SKSE plugin is loaded (kDataLoaded). The board must
// outlive the game session: the panel reads it on the render thread.
Registration Register(DebugBoard *board);

} // namespace DebugPanel
} // namespace SkyrimRemember
