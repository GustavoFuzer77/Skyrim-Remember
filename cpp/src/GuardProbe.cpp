#include "GuardProbe.h"
#include "Defines.h"
#include "RustBridge.h"

#include <RE/A/Actor.h>
#include <RE/C/Calendar.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFaction.h>
#include <SKSE/SKSE.h>

namespace {
// Verified in the local Skyrim.esm FACT/EDID records: GuardFactionWhiterun.
constexpr RE::FormID whiterunGuardFactionID = GuardFactionWhiterun;

consteval std::uint32_t MakeTag(char a, char b, char c, char d) {
  return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8U) |
         (static_cast<std::uint32_t>(c) << 16U) |
         (static_cast<std::uint32_t>(d) << 24U);
}

constexpr std::uint32_t serializationID = MakeTag('S', 'R', 'M', 'R');
constexpr std::uint32_t memoryRecordType = MakeTag('M', 'E', 'M', 'R');
constexpr std::uint32_t memoryRecordVersion = 1;
constexpr std::uint32_t maximumRecordBytes = 16U * 1024U * 1024U;

class GuardProbe final : public RE::BSTEventSink<SKSE::CrosshairRefEvent> {
public:
  static GuardProbe &Get() {
    static GuardProbe instance;
    return instance;
  }

  bool Register() {
    engine.reset(skyrim_remember_create());
    const auto messaging = SKSE::GetMessagingInterface();
    const auto serialization = SKSE::GetSerializationInterface();
    if (!engine || !messaging || !serialization) {
      return false;
    }
    serialization->SetUniqueID(serializationID);
    serialization->SetSaveCallback(OnSave);
    serialization->SetLoadCallback(OnLoad);
    serialization->SetRevertCallback(OnRevert);
    return messaging->RegisterListener(OnMessage);
  }

  RE::BSEventNotifyControl
  ProcessEvent(const SKSE::CrosshairRefEvent *event,
               RE::BSTEventSource<SKSE::CrosshairRefEvent> *) override {
    try {
      Inspect(event);
    } catch (...) {
      playing = false;
      OutputDebugString(
          "Skyrim Remember: guard probe disabled after an exception\n");
    }
    return RE::BSEventNotifyControl::kContinue;
  }

private:
  using EnginePtr =
      std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>;
  EnginePtr engine{nullptr, skyrim_remember_destroy};
  RE::TESFaction *faction = nullptr;
  RE::FormID lastReference = 0;
  bool attached = false;
  bool playing = false;
  bool sessionReady = false;

  static void OnSave(SKSE::SerializationInterface *serialization) noexcept {
    try {
      Get().Save(serialization);
    } catch (...) {
      SKSE::log::error("Persistence save callback failed with an exception");
    }
  }

  static void OnLoad(SKSE::SerializationInterface *serialization) noexcept {
    try {
      Get().Load(serialization);
    } catch (...) {
      SKSE::log::error("Persistence load callback failed with an exception");
    }
  }

  static void OnRevert(SKSE::SerializationInterface *) noexcept {
    try {
      Get().Reset("serialization-revert");
    } catch (...) {
      SKSE::log::error("Persistence revert callback failed with an exception");
    }
  }

  static bool ResolveFormID(void *context, std::uint32_t oldFormID,
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

  static void OnMessage(SKSE::MessagingInterface::Message *message) {
    if (!message) {
      return;
    }
    try {
      Get().HandleMessage(*message);
    } catch (...) {
      Get().playing = false;
      OutputDebugString("Skyrim Remember: lifecycle callback failed\n");
    }
  }

  void HandleMessage(const SKSE::MessagingInterface::Message &message) {
    switch (message.type) {
    case SKSE::MessagingInterface::kDataLoaded: {
      auto *data = RE::TESDataHandler::GetSingleton();
      faction = data ? data->LookupForm<RE::TESFaction>(whiterunGuardFactionID,
                                                        "Skyrim.esm")
                     : nullptr;
      auto *events = SKSE::GetCrosshairRefEventSource();
      if (!faction || !events) {
        SKSE::log::error("Guard probe disabled: missing Whiterun faction or "
                         "crosshair source");
        return;
      }
      if (!attached) {
        events->AddEventSink(this);
        attached = true;
      }
      SKSE::log::info("Guard probe ready: GuardFactionWhiterun={:08X}",
                      faction->GetFormID());
      break;
    }
    case SKSE::MessagingInterface::kPreLoadGame:
      Reset("pre-load");
      break;
    case SKSE::MessagingInterface::kNewGame:
      Reset("new-game");
      playing = sessionReady;
      break;
    case SKSE::MessagingInterface::kPostLoadGame:
      // SKSE encodes the load success bool in data itself, not a bool*.
      playing = sessionReady && message.data != nullptr;
      lastReference = 0;
      break;
    default:
      break;
    }
  }

  void Reset(const char *reason) {
    playing = false;
    lastReference = 0;
    const auto status = skyrim_remember_clear(engine.get());
    sessionReady = status == BridgeStatus::Ok;
    SKSE::log::info("Memory reset: reason={} status={}", reason,
                    static_cast<std::uint32_t>(status));
  }

  void Save(SKSE::SerializationInterface *serialization) {
    if (!serialization || !engine) {
      SKSE::log::error("Persistence save skipped: unavailable interface");
      return;
    }
    std::size_t bytesRequired = 0;
    auto status =
        skyrim_remember_serialize(engine.get(), nullptr, 0, &bytesRequired);
    if (status != BridgeStatus::Ok || bytesRequired == 0 ||
        bytesRequired > maximumRecordBytes) {
      SKSE::log::error("Persistence size query failed: status={} bytes={}",
                       static_cast<std::uint32_t>(status), bytesRequired);
      return;
    }
    std::vector<std::uint8_t> bytes(bytesRequired);
    std::size_t bytesWritten = 0;
    status = skyrim_remember_serialize(engine.get(), bytes.data(), bytes.size(),
                                       &bytesWritten);
    if (status != BridgeStatus::Ok || bytesWritten != bytes.size()) {
      SKSE::log::error(
          "Persistence serialization failed: status={} bytes={}/{}",
          static_cast<std::uint32_t>(status), bytesWritten, bytes.size());
      return;
    }
    if (!serialization->WriteRecord(memoryRecordType, memoryRecordVersion,
                                    bytes.data(),
                                    static_cast<std::uint32_t>(bytes.size()))) {
      SKSE::log::error("Persistence record write failed");
      return;
    }
    SKSE::log::info("Memory saved: bytes={}", bytes.size());
  }

  void Load(SKSE::SerializationInterface *serialization) {
    if (!serialization || !engine) {
      SKSE::log::error("Persistence load skipped: unavailable interface");
      return;
    }
    const auto cleared = skyrim_remember_clear(engine.get());
    if (cleared != BridgeStatus::Ok) {
      SKSE::log::error("Persistence load could not clear memory: status={}",
                       static_cast<std::uint32_t>(cleared));
      return;
    }

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    bool found = false;
    while (serialization->GetNextRecordInfo(type, version, length)) {
      if (type != memoryRecordType) {
        SKSE::log::warn("Ignoring unknown persistence record: type={:08X}",
                        type);
        continue;
      }
      found = true;
      if (version != memoryRecordVersion || length == 0 ||
          length > maximumRecordBytes) {
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
          skyrim_remember_deserialize(engine.get(), bytes.data(), bytes.size(),
                                      ResolveFormID, serialization, &stats);
      if (status != BridgeStatus::Ok) {
        SKSE::log::error("Memory restore failed: status={}",
                         static_cast<std::uint32_t>(status));
        continue;
      }
      SKSE::log::info("Memory restored: loaded={} unresolved={} bytes={}",
                      stats.loaded, stats.unresolved, bytes.size());
    }
    if (!found) {
      SKSE::log::info("No Skyrim Remember data found in this save");
    }
  }

  static double CurrentGameDays() {
    const auto *calendar = RE::Calendar::GetSingleton();
    const auto gameDays = calendar ? calendar->GetDaysPassed() : 0.0F;
    return std::isfinite(gameDays) && gameDays >= 0.0F
               ? static_cast<double>(gameDays)
               : 0.0;
  }

  void Inspect(const SKSE::CrosshairRefEvent *event) {
    if (!playing || !faction || !event) {
      return;
    }
    auto *reference = event->crosshairRef.get();
    const auto referenceID = reference ? reference->GetFormID() : 0;
    if (referenceID == lastReference) {
      return;
    }
    lastReference = referenceID;
    auto *actor = reference ? reference->As<RE::Actor>() : nullptr;
    if (!actor) {
      return;
    }
    const bool eligible = actor != RE::PlayerCharacter::GetSingleton() &&
                          !actor->IsDead() && actor->IsGuard() &&
                          actor->IsInFaction(faction);
    if (!eligible) {
      SKSE::log::info("Guard probe ignored: ref={:08X} (outside scope)",
                      referenceID);
      return;
    }

    const auto gameDays = CurrentGameDays();
    const auto registered =
        skyrim_remember_register(engine.get(), referenceID, gameDays);
    if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
      SKSE::log::error("Register failed: ref={:08X} status={}", referenceID,
                       static_cast<std::uint32_t>(registered));
      return;
    }
    MemorySnapshot snapshot{};
    const auto read =
        skyrim_remember_get(engine.get(), referenceID, gameDays, &snapshot);
    if (read != BridgeStatus::Ok) {
      SKSE::log::error("Read failed: ref={:08X} status={}", referenceID,
                       static_cast<std::uint32_t>(read));
      return;
    }
    SKSE::log::info("Guard memory: ref={:08X} created={} recognition={} "
                    "suspicion={} fear={} respect={} gameDays={} lastDecay={}",
                    referenceID, registered == BridgeStatus::Created,
                    snapshot.recognition, snapshot.suspicion, snapshot.fear,
                    snapshot.respect, gameDays, snapshot.lastDecayGameDays);
  }
};
} // namespace

bool RegisterGuardProbe() { return GuardProbe::Get().Register(); }
