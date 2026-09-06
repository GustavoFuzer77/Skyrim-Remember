#include "GuardProbe.h"
#include "RustBridge.h"
#include "Defines.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESFaction.h>
#include <SKSE/SKSE.h>

namespace {
// Verified in the local Skyrim.esm FACT/EDID records: GuardFactionWhiterun.
constexpr RE::FormID whiterunGuardFactionID = GuardFactionWhiterun;

class GuardProbe final : public RE::BSTEventSink<SKSE::CrosshairRefEvent> {
public:
  static GuardProbe &Get() {
    static GuardProbe instance;
    return instance;
  }

  bool Register() {
    engine.reset(skyrim_remember_create());
    const auto messaging = SKSE::GetMessagingInterface();
    return engine && messaging && messaging->RegisterListener(OnMessage);
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

    const auto registered = skyrim_remember_register(engine.get(), referenceID);
    if (registered != BridgeStatus::Created && registered != BridgeStatus::Ok) {
      SKSE::log::error("Register failed: ref={:08X} status={}", referenceID,
                       static_cast<std::uint32_t>(registered));
      return;
    }
    MemorySnapshot snapshot{};
    const auto read = skyrim_remember_get(engine.get(), referenceID, &snapshot);
    if (read != BridgeStatus::Ok) {
      SKSE::log::error("Read failed: ref={:08X} status={}", referenceID,
                       static_cast<std::uint32_t>(read));
      return;
    }
    SKSE::log::info("Guard memory: ref={:08X} created={} recognition={} "
                    "suspicion={} fear={} respect={}",
                    referenceID, registered == BridgeStatus::Created,
                    snapshot.recognition, snapshot.suspicion, snapshot.fear,
                    snapshot.respect);
  }
};
} // namespace

bool RegisterGuardProbe() { return GuardProbe::Get().Register(); }
