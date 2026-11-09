#include "Persistence.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace SkyrimRemember::Persistence {

bool Save(const MemoryStore *store, SKSE::SerializationInterface *serialization) {
  if (!serialization || !store) {
    SKSE::log::error("Persistence save skipped: unavailable interface");
    return false;
  }
  const auto bytes = store->Serialize();
  if (bytes.empty() || bytes.size() > MaximumRecordBytes) {
    SKSE::log::error("Persistence payload unusable: bytes={}", bytes.size());
    return false;
  }
  if (!serialization->WriteRecord(MemoryRecordType, RecordEnvelopeVersion, bytes.data(), static_cast<std::uint32_t>(bytes.size()))) {
    SKSE::log::error("Persistence record write failed");
    return false;
  }
  SKSE::log::info("Memory saved: bytes={}", bytes.size());
  return true;
}

LoadOutcome Load(MemoryStore *store, SKSE::SerializationInterface *serialization) {
  if (!serialization || !store) {
    SKSE::log::error("Persistence load skipped: unavailable interface");
    return LoadOutcome::Unavailable;
  }

  std::uint32_t type = 0;
  std::uint32_t version = 0;
  std::uint32_t length = 0;
  bool found = false;
  bool restored = false;
  while (serialization->GetNextRecordInfo(type, version, length)) {
    if (type != MemoryRecordType) {
      SKSE::log::warn("Ignoring unknown persistence record: type={:08X}", type);
      continue;
    }
    found = true;
    if (version != RecordEnvelopeVersion || length == 0 || length > MaximumRecordBytes) {
      SKSE::log::error("Persistence record rejected: version={} bytes={}", version, length);
      continue;
    }
    std::vector<std::uint8_t> bytes(length);
    const auto read = serialization->ReadRecordData(bytes.data(), length);
    if (read != length) {
      SKSE::log::error("Persistence record truncated: bytes={}/{}", read, length);
      continue;
    }
    PersistenceStats stats;
    const auto resolve = [serialization](std::uint32_t oldFormID) -> std::optional<std::uint32_t> {
      RE::FormID resolved = 0;
      if (!serialization->ResolveFormID(oldFormID, resolved)) {
        return std::nullopt;
      }
      return resolved;
    };
    const auto status = store->Deserialize(bytes, resolve, stats);
    if (status != MemoryStatus::Ok) {
      SKSE::log::error("Memory restore failed: status={}", static_cast<std::uint32_t>(status));
      continue;
    }
    SKSE::log::info("Memory restored: loaded={} unresolvedForms={} bytes={}", stats.loaded, stats.unresolved, bytes.size());
    restored = true;
  }

  if (restored) {
    return LoadOutcome::Restored;
  }
  if (found) {
    SKSE::log::critical("Memory restore rejected; Skyrim Remember is disabled "
                        "for this loaded session");
    return LoadOutcome::Rejected;
  }
  store->Clear();
  SKSE::log::info("No Skyrim Remember data found in this save");
  return LoadOutcome::NoData;
}

} // namespace SkyrimRemember::Persistence
