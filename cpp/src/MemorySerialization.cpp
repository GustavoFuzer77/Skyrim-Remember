#include "Memory.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

// The payload of the SKSE co-save record, byte-compatible with what the Rust
// core wrote (bincode 1.x, fixed-width little-endian integers), so a save made
// before the core moved to C++ still loads.
//
//   u32 version
//   u64 count, then per NPC:
//     u32 reference FormID
//     f32 axes[axisCapacity]
//     f64 last decay game days
//     u8 hasRecognition, [f64 last recognition game days]
//     u64 count, then per known outfit:
//       u64 count, then per item: u32 FormID, u32 slot mask
//       u8 face visible, u8 identity known, f64 last seen game days
//
// Only the current version is read. Reading an older one would be a separate
// decoder per version; add one here if a released save ever needs it.
namespace SkyrimRemember {
namespace {

static_assert(std::endian::native == std::endian::little);

constexpr std::uint32_t payloadVersion = 5;

class Writer {
public:
  template <typename T>
  void Put(T value) {
    static_assert(std::is_arithmetic_v<T>);
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
  }

  void PutBool(bool value) {
    Put<std::uint8_t>(value ? 1 : 0);
  }

  void PutCount(std::size_t value) {
    Put<std::uint64_t>(value);
  }

  std::vector<std::uint8_t> Take() {
    return std::move(bytes);
  }

private:
  std::vector<std::uint8_t> bytes;
};

// Reads until the first shortfall, after which every read fails.
class Reader {
public:
  explicit Reader(std::span<const std::uint8_t> data) : data(data) {}

  template <typename T>
  T Get() {
    static_assert(std::is_arithmetic_v<T>);
    T value{};
    if (failed || data.size() - position < sizeof(T)) {
      failed = true;
      return value;
    }
    std::memcpy(&value, data.data() + position, sizeof(T));
    position += sizeof(T);
    return value;
  }

  bool GetBool() {
    const auto value = Get<std::uint8_t>();
    if (value > 1) {
      failed = true;
    }
    return value == 1;
  }

  std::uint64_t GetCount() {
    return Get<std::uint64_t>();
  }

  bool Failed() const {
    return failed;
  }

private:
  std::span<const std::uint8_t> data;
  std::size_t position = 0;
  bool failed = false;
};

struct PersistedMemory {
  std::uint32_t referenceFormId = 0;
  NpcMemory memory;
  std::vector<KnownAppearance> appearances;
};

NpcMemory ReadMemory(Reader &reader) {
  NpcMemory memory;
  for (auto &axis : memory.axes) {
    axis = reader.Get<float>();
  }
  memory.lastDecayGameDays = reader.Get<double>();
  if (reader.GetBool()) {
    memory.lastRecognitionGameDays = reader.Get<double>();
  }
  return memory;
}

KnownAppearance ReadAppearance(Reader &reader) {
  KnownAppearance appearance;
  const auto itemCount = reader.GetCount();
  // Not reserved up front: the count comes from untrusted bytes, and the loop
  // stops at the first read past the end.
  for (std::uint64_t index = 0; index < itemCount && !reader.Failed(); ++index) {
    AppearanceItem item;
    item.formId = reader.Get<std::uint32_t>();
    item.slotMask = reader.Get<std::uint32_t>();
    appearance.items.push_back(item);
  }
  appearance.faceVisible = reader.GetBool();
  appearance.identityKnown = reader.GetBool();
  appearance.lastSeenGameDays = reader.Get<double>();
  return appearance;
}

// Rejects appearances that could not have been produced by the game.
bool AppearanceIsValid(const KnownAppearance &appearance, std::size_t maxItems) {
  return appearance.items.size() <= maxItems &&
         std::ranges::all_of(appearance.items, [](const AppearanceItem &item) { return item.formId != 0 && item.slotMask != 0; }) &&
         std::isfinite(appearance.lastSeenGameDays) && appearance.lastSeenGameDays >= 0.0;
}

} // namespace

std::vector<std::uint8_t> MemoryStore::Serialize() const {
  const std::scoped_lock lock(mutex);
  std::vector<std::uint32_t> ids;
  ids.reserve(memories.size());
  for (const auto &entry : memories) {
    ids.push_back(entry.first);
  }
  // Sorted, so the same store always produces the same bytes.
  std::ranges::sort(ids);

  Writer writer;
  writer.Put<std::uint32_t>(payloadVersion);
  writer.PutCount(ids.size());
  for (const auto id : ids) {
    const auto &memory = memories.at(id);
    writer.Put(id);
    for (const auto axis : memory.axes) {
      writer.Put(axis);
    }
    writer.Put(memory.lastDecayGameDays);
    writer.PutBool(memory.lastRecognitionGameDays.has_value());
    if (memory.lastRecognitionGameDays) {
      writer.Put(*memory.lastRecognitionGameDays);
    }
    const auto known = appearances.find(id);
    const auto count = known == appearances.end() ? 0 : known->second.size();
    writer.PutCount(count);
    if (count == 0) {
      continue;
    }
    for (const auto &appearance : known->second) {
      writer.PutCount(appearance.items.size());
      for (const auto &item : appearance.items) {
        writer.Put(item.formId);
        writer.Put(item.slotMask);
      }
      writer.PutBool(appearance.faceVisible);
      writer.PutBool(appearance.identityKnown);
      writer.Put(appearance.lastSeenGameDays);
    }
  }
  return writer.Take();
}

MemoryStatus MemoryStore::Deserialize(std::span<const std::uint8_t> bytes, const FormIdResolver &resolver, PersistenceStats &stats) {
  if (!resolver) {
    return MemoryStatus::InvalidArgument;
  }

  // The resolver calls back into the host, so the lock is not held while it
  // runs: everything is decoded and resolved into locals first, and only the
  // final swap needs the store.
  Reader reader(bytes);
  const auto version = reader.Get<std::uint32_t>();
  if (reader.Failed()) {
    return MemoryStatus::InvalidData;
  }
  if (version != payloadVersion) {
    return MemoryStatus::UnsupportedVersion;
  }

  std::vector<PersistedMemory> persisted;
  const auto memoryCount = reader.GetCount();
  for (std::uint64_t index = 0; index < memoryCount && !reader.Failed(); ++index) {
    PersistedMemory entry;
    entry.referenceFormId = reader.Get<std::uint32_t>();
    entry.memory = ReadMemory(reader);
    const auto appearanceCount = reader.GetCount();
    for (std::uint64_t outfit = 0; outfit < appearanceCount && !reader.Failed(); ++outfit) {
      entry.appearances.push_back(ReadAppearance(reader));
    }
    persisted.push_back(std::move(entry));
  }
  if (reader.Failed()) {
    return MemoryStatus::InvalidData;
  }

  const auto &limits = tuning.limits;
  std::unordered_map<std::uint32_t, NpcMemory> restoredMemories;
  std::unordered_map<std::uint32_t, std::vector<KnownAppearance>> restoredAppearances;
  PersistenceStats restoredStats;

  for (auto &entry : persisted) {
    if (entry.referenceFormId == 0 || !entry.memory.IsValid() || entry.appearances.size() > limits.maxKnownAppearances ||
        !std::ranges::all_of(entry.appearances, [&](const KnownAppearance &appearance) { return AppearanceIsValid(appearance, limits.maxAppearanceItems); })) {
      return MemoryStatus::InvalidData;
    }

    const auto resolved = resolver(entry.referenceFormId);
    if (!resolved || *resolved == 0) {
      ++restoredStats.unresolved;
      continue;
    }

    std::vector<KnownAppearance> appearancesOfNpc;
    for (auto &appearance : entry.appearances) {
      std::vector<AppearanceItem> items;
      for (const auto &item : appearance.items) {
        if (const auto resolvedItem = resolver(item.formId)) {
          items.push_back({*resolvedItem, item.slotMask});
        } else {
          ++restoredStats.unresolved;
        }
      }
      // Normalizing here rather than trusting the saved bytes is what lets an
      // outfit saved under older rules — technical slots included, slots
      // unmerged — compare equal to the same outfit captured today.
      NormalizeItems(items);
      if (appearance.faceVisible || !items.empty()) {
        appearancesOfNpc.push_back({std::move(items), appearance.faceVisible, appearance.identityKnown, appearance.lastSeenGameDays});
      }
    }

    restoredMemories[*resolved] = entry.memory;
    if (!appearancesOfNpc.empty()) {
      restoredAppearances[*resolved] = std::move(appearancesOfNpc);
    }
    ++restoredStats.loaded;
  }

  const std::scoped_lock lock(mutex);
  memories = std::move(restoredMemories);
  appearances = std::move(restoredAppearances);
  stats = restoredStats;
  return MemoryStatus::Ok;
}

} // namespace SkyrimRemember
