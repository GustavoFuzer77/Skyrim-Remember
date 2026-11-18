Scriptname SkyrimRemember Hidden
{What NPCs remember about the Player, provided by the Skyrim Remember SKSE plugin.

Axes are named: recognition, suspicion, fear, respect, approval, loyalty,
resentment, trust. Levels go from 0 to 100 and fade over game time.}

; The current level of one axis in akNpc's memory of the Player.
; 0 when akNpc has no memory yet, -1 for an unknown axis or a None actor.
Float Function GetAxis(Actor akNpc, String asAxis) Global Native

; True once akNpc has perceived the Player, or a script gave it a memory.
Bool Function HasMemory(Actor akNpc) Global Native

; Adds afDelta to one axis, clamped to 0..100, creating the memory if needed.
; Use it to feed events the plugin does not see: a favor, a threat, a theft.
; Returns the new level, or -1 on bad arguments.
Float Function ModAxis(Actor akNpc, String asAxis, Float afDelta) Global Native
