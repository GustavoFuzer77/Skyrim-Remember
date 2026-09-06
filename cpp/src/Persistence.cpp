#include "Persistence.h"

#include <cstddef>
#include <vector>

namespace SkyrimRemember::Persistence {

bool ResolveFormID(void *context, std::uint32_t oldFormID,
                   std::uint32_t *newFormID) noexcept {
  if (!context || !newFormID) {
    return false;
  }
  auto *serialization = static_cast<SKSE::SerializationInterface *>(context);
  RE::FormID resolved = 0;
  if (!serialization->ResolveFormID(oldFormID, resolved)) {
    return false;
  }
  *newFormID = resolved;
  return true;
}

bool Save(const MemoryEngine *engine,
          SKSE::SerializationInterface *serialization) {
  if (!serialization || !engine) {
    SKSE::log::error("Persistence save skipped: unavailable interface");
    return false;
  }
  std::size_t bytesRequired = 0;
  auto status = skyrim_remember_serialize(engine, nullptr, 0, &bytesRequired);
  if (status != BridgeStatus::Ok || bytesRequired == 0 ||
      bytesRequired > MaximumRecordBytes) {
    SKSE::log::error("Persistence size query failed: status={} bytes={}",
                     static_cast<std::uint32_t>(status), bytesRequired);
    return false;
  }
  std::vector<std::uint8_t> bytes(bytesRequired);
  std::size_t bytesWritten = 0;
  status =
      skyrim_remember_serialize(engine, bytes.data(), bytes.size(), &bytesWritten);
  if (status != BridgeStatus::Ok || bytesWritten != bytes.size()) {
    SKSE::log::error("Persistence serialization failed: status={} bytes={}/{}",
                     static_cast<std::uint32_t>(status), bytesWritten,
                     bytes.size());
    return false;
  }
  if (!serialization->WriteRecord(MemoryRecordType, RecordEnvelopeVersion,
                                  bytes.data(),
                                  static_cast<std::uint32_t>(bytes.size()))) {
    SKSE::log::error("Persistence record write failed");
    return false;
  }
  SKSE::log::info("Memory saved: bytes={}", bytes.size());
  return true;
}

LoadOutcome Load(const MemoryEngine *engine,
                 SKSE::SerializationInterface *serialization) {
  if (!serialization || !engine) {
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
    if (version != RecordEnvelopeVersion || length == 0 ||
        length > MaximumRecordBytes) {
      SKSE::log::error("Persistence record rejected: version={} bytes={}",
                       version, length);
      continue;
    }
    std::vector<std::uint8_t> bytes(length);
    const auto read = serialization->ReadRecordData(bytes.data(), length);
    if (read != length) {
      SKSE::log::error("Persistence record truncated: bytes={}/{}", read,
                       length);
      continue;
    }
    PersistenceStats stats{};
    const auto status =
        skyrim_remember_deserialize(engine, bytes.data(), bytes.size(),
                                    ResolveFormID, serialization, &stats);
    if (status != BridgeStatus::Ok) {
      SKSE::log::error("Memory restore failed: status={}",
                       static_cast<std::uint32_t>(status));
      continue;
    }
    SKSE::log::info("Memory restored: loaded={} unresolvedForms={} bytes={}",
                    stats.loaded, stats.unresolved, bytes.size());
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
  const auto cleared = skyrim_remember_clear(engine);
  SKSE::log::info("No Skyrim Remember data found in this save: clearStatus={}",
                  static_cast<std::uint32_t>(cleared));
  return cleared == BridgeStatus::Ok ? LoadOutcome::NoData
                                     : LoadOutcome::Unavailable;
}

} // namespace SkyrimRemember::Persistence
