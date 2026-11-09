#pragma once

#include "Memory.h"

#include <SKSE/SKSE.h>

#include <cstdint>

namespace SkyrimRemember::Persistence {

consteval std::uint32_t MakeTag(char a, char b, char c, char d) {
  return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8U) | (static_cast<std::uint32_t>(c) << 16U) |
         (static_cast<std::uint32_t>(d) << 24U);
}

inline constexpr std::uint32_t SerializationID = MakeTag('S', 'R', 'M', 'R');
inline constexpr std::uint32_t MemoryRecordType = MakeTag('M', 'E', 'M', 'R');
// Version of the SKSE record envelope. The payload carries its own
// version, written and checked by MemoryStore.
inline constexpr std::uint32_t RecordEnvelopeVersion = 1;
inline constexpr std::uint32_t MaximumRecordBytes = 16U * 1024U * 1024U;

// What one Load call concluded about the save being opened.
enum class LoadOutcome {
  // The save predates this plugin; the store was cleared and is usable.
  NoData,
  // Data was found and restored.
  Restored,
  // Data was found but could not be trusted; the session must stay disabled.
  Rejected,
  // The save interface or the store was not there to work with.
  Unavailable
};

// Writes the whole store into the active SKSE co-save record.
bool Save(const MemoryStore *store, SKSE::SerializationInterface *serialization);

// Restores the store from the active SKSE co-save, remapping every saved
// FormID through the interface's own resolver.
LoadOutcome Load(MemoryStore *store, SKSE::SerializationInterface *serialization);

} // namespace SkyrimRemember::Persistence
