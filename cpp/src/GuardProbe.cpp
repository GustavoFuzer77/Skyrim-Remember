#include "GuardProbe.h"
#include "Appearance.h"
#include "Defines.h"
#include "EquipmentBatch.h"
#include "Persistence.h"
#include "PlayerAppearance.h"
#include "RustBridge.h"

#include <RE/A/Actor.h>
#include <RE/C/Calendar.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESEquipEvent.h>
#include <RE/T/TESFaction.h>
#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

#include <cstdint>
#include <optional>

namespace {
using SkyrimRemember::CapturedAppearance;
using SkyrimRemember::CapturePlayerAppearance;
using SkyrimRemember::EquipmentBatch;
namespace Persistence = SkyrimRemember::Persistence;

// Verified in the local Skyrim.esm FACT/EDID records: GuardFactionWhiterun.
constexpr RE::FormID whiterunGuardFactionID =
    SkyrimRemember::Forms::WhiterunGuardFactionLocalID;

constexpr float maximumObservationDistance = 4096.0F;

void LogAppearance(RE::FormID observer, const AppearanceResult &result) {
  SKSE::log::info(
      "Appearance: observer={:08X} appearanceMatched={} recognized={} "
      "linked={} learned={} faceVisible={} "
      "outfitSimilarity={} confidence={} threshold={} items={} known={}",
      observer, result.appearanceMatched != 0, result.recognized != 0,
      result.linkedByContinuity != 0, result.learned != 0,
      result.faceVisible != 0, result.outfitSimilarity,
      result.identityConfidence, result.recognitionThreshold, result.itemCount,
      result.knownAppearances);
}

class GuardProbe final : public RE::BSTEventSink<SKSE::CrosshairRefEvent>,
                         public RE::BSTEventSink<RE::TESEquipEvent>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
  static GuardProbe &Get() {
    static GuardProbe instance;
    return instance;
  }

  bool Register() {
    engine.reset(skyrim_remember_create());
    const auto messaging = SKSE::GetMessagingInterface();
    const auto serialization = SKSE::GetSerializationInterface();
    const auto tasks = SKSE::GetTaskInterface();
    if (!engine || !messaging || !serialization || !tasks) {
      return false;
    }
    if (!messaging->RegisterListener(OnMessage)) {
      return false;
    }
    // The probe is a function-local static, so it outlives every task it
    // schedules through the batch.
    equipment.Configure(tasks, [this](std::uint64_t batchedEvents) {
      HandleEquipmentUpdate(batchedEvents);
    });
    serialization->SetUniqueID(Persistence::SerializationID);
    serialization->SetSaveCallback(OnSave);
    serialization->SetLoadCallback(OnLoad);
    serialization->SetRevertCallback(OnRevert);
    return true;
  }

  RE::BSEventNotifyControl
  ProcessEvent(const SKSE::CrosshairRefEvent *event,
               RE::BSTEventSource<SKSE::CrosshairRefEvent> *) override {
    try {
      Inspect(event);
    } catch (...) {
      Disable("Guard probe disabled after an exception");
    }
    return RE::BSEventNotifyControl::kContinue;
  }

  RE::BSEventNotifyControl
  ProcessEvent(const RE::TESEquipEvent *event,
               RE::BSTEventSource<RE::TESEquipEvent> *) override {
    try {
      QueueEquipmentUpdate(event);
    } catch (...) {
      SKSE::log::error("Equipment event failed with an exception");
    }
    return RE::BSEventNotifyControl::kContinue;
  }

  RE::BSEventNotifyControl
  ProcessEvent(const RE::MenuOpenCloseEvent *event,
               RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override {
    try {
      if (event && !event->opening) {
        equipment.MenuClosed();
      }
    } catch (...) {
      SKSE::log::error("Menu event failed with an exception");
    }
    return RE::BSEventNotifyControl::kContinue;
  }

private:
  // SKSE messages, game events, and queued tasks access the gameplay fields
  // below only on Skyrim's main thread.
  using EnginePtr =
      std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>;
  EnginePtr engine{nullptr, skyrim_remember_destroy};
  EquipmentBatch equipment;
  RE::TESFaction *faction = nullptr;
  RE::FormID lastReference = 0;
  std::optional<CapturedAppearance> lastPlayerAppearance;
  bool attached = false;
  bool equipmentAttached = false;
  bool menuAttached = false;
  bool playing = false;
  bool sessionReady = false;

  // One appearance question for the Rust core. `learn` chooses between the
  // pure query and the observation that also updates the NPC's memory, so the
  // difference is visible at every call site.
  struct AppearanceQuery {
    RE::FormID observer = 0;
    const CapturedAppearance *appearance = nullptr;
    bool visualContinuity = false;
    bool learn = false;
    double gameDays = 0.0;
  };

  static void Disable(const char *reason) {
    Get().playing = false;
    try {
      SKSE::log::critical("{}", reason);
    } catch (...) {
      OutputDebugString("Skyrim Remember: guard probe disabled\n");
    }
  }

  static void OnSave(SKSE::SerializationInterface *serialization) noexcept {
    try {
      auto &probe = Get();
      if (!probe.sessionReady || !probe.playing) {
        SKSE::log::error("Persistence save skipped: no gameplay session");
        return;
      }
      Persistence::Save(probe.engine.get(), serialization);
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
      Get().PrepareForLoad("serialization-revert");
    } catch (...) {
      SKSE::log::error("Persistence revert callback failed with an exception");
    }
  }

  static void OnMessage(SKSE::MessagingInterface::Message *message) {
    if (!message) {
      return;
    }
    try {
      Get().HandleMessage(*message);
    } catch (...) {
      Disable("Lifecycle callback failed; guard probe disabled");
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
      auto *scriptEvents = RE::ScriptEventSourceHolder::GetSingleton();
      if (!faction || !events || !scriptEvents) {
        SKSE::log::error("Guard probe disabled: missing Whiterun faction or "
                         "event source");
        return;
      }
      if (!attached) {
        events->AddEventSink(this);
        attached = true;
      }
      if (!equipmentAttached) {
        scriptEvents->GetEventSource<RE::TESEquipEvent>()->AddEventSink(this);
        equipmentAttached = true;
      }
      auto *ui = RE::UI::GetSingleton();
      if (!ui) {
        SKSE::log::error("Equipment batching disabled: UI unavailable");
        return;
      }
      if (!menuAttached) {
        ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
        menuAttached = true;
      }
      SKSE::log::info("Guard probe ready: GuardFactionWhiterun={:08X}",
                      faction->GetFormID());
      break;
    }
    case SKSE::MessagingInterface::kPreLoadGame:
      PrepareForLoad("pre-load");
      break;
    case SKSE::MessagingInterface::kNewGame:
      Reset("new-game");
      playing = sessionReady;
      if (playing) {
        lastPlayerAppearance = CapturePlayerAppearance();
      }
      break;
    case SKSE::MessagingInterface::kPostLoadGame:
      // SKSE encodes the load success bool in data itself, not a bool*.
      playing = sessionReady && message.data != nullptr;
      lastReference = 0;
      if (playing) {
        lastPlayerAppearance = CapturePlayerAppearance();
      }
      break;
    default:
      break;
    }
  }

  void Reset(const char *reason) {
    PrepareForLoad(reason);
    const auto status = skyrim_remember_clear(engine.get());
    sessionReady = status == BridgeStatus::Ok;
    SKSE::log::info("Memory reset: reason={} status={}", reason,
                    static_cast<std::uint32_t>(status));
  }

  void PrepareForLoad(const char *reason) {
    playing = false;
    sessionReady = false;
    lastReference = 0;
    lastPlayerAppearance.reset();
    equipment.Reset();
    SKSE::log::info("Memory load preparation: reason={}", reason);
  }

  void Load(SKSE::SerializationInterface *serialization) {
    sessionReady = false;
    const auto outcome = Persistence::Load(engine.get(), serialization);
    sessionReady = outcome == Persistence::LoadOutcome::Restored ||
                   outcome == Persistence::LoadOutcome::NoData;
  }

  static std::optional<double> CurrentGameDays() {
    const auto *calendar = RE::Calendar::GetSingleton();
    if (!calendar) {
      SKSE::log::error("Game time unavailable: Calendar singleton missing");
      return std::nullopt;
    }
    const auto gameDays = calendar->GetDaysPassed();
    if (!std::isfinite(gameDays) || gameDays < 0.0F) {
      SKSE::log::error("Game time unavailable: invalid GameDaysPassed={}",
                       gameDays);
      return std::nullopt;
    }
    return static_cast<double>(gameDays);
  }

  bool IsEligibleGuard(RE::Actor &actor) const {
    return &actor != RE::PlayerCharacter::GetSingleton() && !actor.IsDead() &&
           actor.IsGuard() && actor.IsInFaction(faction);
  }

  static bool IsInObservationRange(RE::Actor &actor,
                                   RE::PlayerCharacter &player) {
    const auto *actorWorld = actor.GetWorldspace();
    const auto *playerWorld = player.GetWorldspace();
    if (actorWorld != playerWorld) {
      return false;
    }
    if (!actorWorld && actor.GetParentCell() != player.GetParentCell()) {
      return false;
    }
    const auto distance = actor.GetPosition().GetDistance(player.GetPosition());
    return std::isfinite(distance) && distance <= maximumObservationDistance;
  }

  BridgeStatus QueryAppearance(const AppearanceQuery &query,
                               AppearanceResult &result) const {
    const auto &items = query.appearance->items;
    const auto *data = items.empty() ? nullptr : items.data();
    const auto faceVisible = query.appearance->faceVisible ? 1U : 0U;
    const auto continuity = query.visualContinuity ? 1U : 0U;
    if (query.learn) {
      return skyrim_remember_observe_appearance(engine.get(), query.observer,
                                                query.gameDays, data,
                                                items.size(), faceVisible,
                                                continuity, &result);
    }
    return skyrim_remember_evaluate_appearance(engine.get(), query.observer,
                                               data, items.size(), faceVisible,
                                               continuity, &result);
  }

  void QueueEquipmentUpdate(const RE::TESEquipEvent *event) {
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!event || !player || event->actor.get() != player || !playing) {
      return;
    }
    equipment.RecordChange();
  }

  void HandleEquipmentUpdate(std::uint64_t batchedEvents) {
    if (!playing || !faction || !engine) {
      return;
    }
    auto *player = RE::PlayerCharacter::GetSingleton();
    auto *processLists = RE::ProcessLists::GetSingleton();
    if (!player || !processLists) {
      return;
    }

    auto current = CapturePlayerAppearance();
    if (!lastPlayerAppearance) {
      lastPlayerAppearance = std::move(current);
      return;
    }
    auto previous = std::move(*lastPlayerAppearance);
    lastPlayerAppearance = current;
    if (previous == current) {
      return;
    }

    const auto currentGameDays = CurrentGameDays();
    if (!currentGameDays) {
      return;
    }
    const auto gameDays = *currentGameDays;
    processLists->ForEachHighActor([&](RE::Actor &actor) {
      if (!IsEligibleGuard(actor) || !actor.Is3DLoaded() ||
          !IsInObservationRange(actor, *player)) {
        return RE::BSContainer::ForEachResult::kContinue;
      }
      const auto detectionLevel = actor.RequestDetectionLevel(player);
      bool lineOfSightArgument = false;
      const auto hasLineOfSight =
          detectionLevel > 0 &&
          actor.HasLineOfSight(player, lineOfSightArgument);
      if (!hasLineOfSight) {
        return RE::BSContainer::ForEachResult::kContinue;
      }

      const auto observer = actor.GetFormID();
      const auto registered =
          skyrim_remember_register(engine.get(), observer, gameDays);
      if (registered != BridgeStatus::Created &&
          registered != BridgeStatus::Ok) {
        SKSE::log::error(
            "Equipment observer registration failed: ref={:08X} status={}",
            observer, static_cast<std::uint32_t>(registered));
        return RE::BSContainer::ForEachResult::kContinue;
      }

      // Did this guard recognize the Player a moment ago, before the change?
      AppearanceResult before{};
      auto status = QueryAppearance({.observer = observer,
                                     .appearance = &previous,
                                     .visualContinuity = false,
                                     .learn = false},
                                    before);
      if (status != BridgeStatus::Ok) {
        SKSE::log::error(
            "Previous appearance evaluation failed: ref={:08X} status={}",
            observer, static_cast<std::uint32_t>(status));
        return RE::BSContainer::ForEachResult::kContinue;
      }

      // A guard that was watching a recognized Player keeps the identity
      // across the change, so the new outfit is learned as the Player's.
      AppearanceResult after{};
      status = QueryAppearance({.observer = observer,
                                .appearance = &current,
                                .visualContinuity = before.recognized != 0,
                                .learn = true,
                                .gameDays = gameDays},
                               after);
      if (status != BridgeStatus::Ok) {
        SKSE::log::error(
            "Changed appearance inspection failed: ref={:08X} status={}",
            observer, static_cast<std::uint32_t>(status));
        return RE::BSContainer::ForEachResult::kContinue;
      }

      const auto distance =
          actor.GetPosition().GetDistance(player->GetPosition());
      SKSE::log::info("Appearance transition: observer={:08X} "
                      "batchedEvents={} detection={} "
                      "lineOfSight=true distance={} "
                      "previousRecognized={} continuity={} recognized={} "
                      "linked={} learned={} faceVisible={} known={}",
                      observer, batchedEvents, detectionLevel, distance,
                      before.recognized != 0, before.recognized != 0,
                      after.recognized != 0, after.linkedByContinuity != 0,
                      after.learned != 0, after.faceVisible != 0,
                      after.knownAppearances);
      return RE::BSContainer::ForEachResult::kContinue;
    });
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
    if (!IsEligibleGuard(*actor)) {
      SKSE::log::info("Guard probe ignored: ref={:08X} (outside scope)",
                      referenceID);
      return;
    }

    const auto currentGameDays = CurrentGameDays();
    if (!currentGameDays) {
      return;
    }
    const auto gameDays = *currentGameDays;
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
    // Looking at a guard through the crosshair is deliberate contact: the
    // guard is given the chance to memorize what the Player looks like now.
    const auto appearance = CapturePlayerAppearance();
    AppearanceResult appearanceResult{};
    const auto appearanceStatus = QueryAppearance({.observer = referenceID,
                                                   .appearance = &appearance,
                                                   .visualContinuity = false,
                                                   .learn = true,
                                                   .gameDays = gameDays},
                                                  appearanceResult);
    if (appearanceStatus != BridgeStatus::Ok) {
      SKSE::log::error("Appearance inspection failed: ref={:08X} status={}",
                       referenceID,
                       static_cast<std::uint32_t>(appearanceStatus));
      return;
    }
    SKSE::log::info("Guard memory: ref={:08X} created={} recognition={} "
                    "suspicion={} fear={} respect={} gameDays={} lastDecay={}",
                    referenceID, registered == BridgeStatus::Created,
                    snapshot.recognition, snapshot.suspicion, snapshot.fear,
                    snapshot.respect, gameDays, snapshot.lastDecayGameDays);
    LogAppearance(referenceID, appearanceResult);
    if (appearanceResult.learned != 0) {
      for (const auto &item : appearance.items) {
        SKSE::log::info(
            "Appearance equipment: observer={:08X} item={:08X} slots={:08X}",
            referenceID, item.formId, item.slotMask);
      }
    }
  }
};
} // namespace

bool RegisterGuardProbe() { return GuardProbe::Get().Register(); }
