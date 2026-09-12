#include "Runtime.h"
#include "AppearanceMemoryService.h"
#include "EquipmentBatch.h"
#include "ImpressionRules.h"
#include "NpcObservationService.h"
#include "NpcProfile.h"
#include "ObservationTuning.h"
#include "Persistence.h"
#include "ProfileReactionRouter.h"
#include "RustBridge.h"

#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/S/ScriptEventSourceHolder.h>
#include <RE/T/TESEquipEvent.h>
#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

#include <chrono>
#include <cstdint>
#include <memory>

namespace SkyrimRemember {

namespace {

void LogCallbackFailure(const char *message) noexcept {
  try {
    SKSE::log::error("{}", message);
  } catch (...) {
    OutputDebugString("Skyrim Remember: lifecycle callback failed\n");
  }
}

// Owns the session: the Rust engine, universal appearance memory, profile
// reactions, the scan and the equipment batch. Everything below runs on
// Skyrim's main thread.
class Runtime final : public RE::BSTEventSink<RE::TESEquipEvent>, public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
  static Runtime &Get() {
    static Runtime instance;
    return instance;
  }

  bool Register() {

    const auto messaging = SKSE::GetMessagingInterface();
    const auto serialization = SKSE::GetSerializationInterface();
    const auto tasks = SKSE::GetTaskInterface();

    engine.reset(skyrim_remember_create());

    if (!engine || !messaging || !serialization || !tasks) {
      return false;
    }

    if (!messaging->RegisterListener(OnMessage)) {
      return false;
    }

    appearanceMemory.Configure(engine.get());

    // Runtime is a function-local static, so it outlives every task the batch
    // and the observation service schedule.
    equipment.Configure(tasks, [this](std::uint64_t batchedEvents) {
      this->FlushEquipmentBatch(batchedEvents);
    });

    observations.Configure(
        tasks,
        [this](NpcObservationService::Trigger) {
          this->appearanceMemory.BeginObservationPass();
          this->reactions.BeginPass();
        },
        [this](RE::Actor &actor, const NpcObservationContext &context, const PerceptionOutcome &outcome, NpcObservationService::Trigger trigger) {
          const auto identity = this->appearanceMemory.Observe(context, outcome, trigger == NpcObservationService::Trigger::AppearanceChanged);
          if (identity) {
            this->reactions.OnObserved(actor, context, outcome, *identity);
          }
        });

    serialization->SetUniqueID(Persistence::SerializationID);
    serialization->SetSaveCallback(OnSave);
    serialization->SetLoadCallback(OnLoad);
    serialization->SetRevertCallback(OnRevert);

    return true;
  }

  RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent *event, RE::BSTEventSource<RE::TESEquipEvent> *) override {
    try {
      auto *player = RE::PlayerCharacter::GetSingleton();

      if (event && player && playing && event->actor.get() == player) {
        equipment.RecordChange();
      }

    } catch (...) {
      SKSE::log::error("Equipment event failed with an exception");
    }

    return RE::BSEventNotifyControl::kContinue;
  }

  RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent *event, RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override {
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
  EquipmentBatch equipment;
  bool equipmentAttached = false;
  bool menuAttached = false;
  bool rulesLoaded = false;
  bool playing = false;
  bool sessionReady = false;

  using EnginePtr = std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>; // <tipo do pointer, tipo do deleter>
  EnginePtr engine{nullptr, skyrim_remember_destroy};

  AppearanceMemoryService appearanceMemory;
  ProfileReactionRouter reactions;

  // Declared last on purpose: its destructor stops and joins the timer thread
  // before the adapters and the engine its tasks reach into are destroyed.
  NpcObservationService observations;

  void SetPlaying(bool value) {
    playing = value;
    appearanceMemory.SetSessionActive(value);
    observations.SetActive(value);
  }

  void FlushEquipmentBatch(std::uint64_t batchedEvents) {

    if (!playing) {
      return;
    }
    // An outfit change is a discrete event: every NPC watching right now must
    // see it, so this one sweep ignores the budget and the cooldown.
    if (!appearanceMemory.BeginAppearanceTransition(batchedEvents)) {
      appearanceMemory.EndAppearanceTransition();
      return;
    }
    try {
      observations.ScanNow(NpcObservationService::Trigger::AppearanceChanged);
    } catch (...) {
      appearanceMemory.EndAppearanceTransition();
      throw;
    }
    appearanceMemory.EndAppearanceTransition();
  }

  static void Disable(const char *reason) {
    Get().SetPlaying(false);
    try {
      SKSE::log::critical("{}", reason);
    } catch (...) {
      OutputDebugString("Skyrim Remember: runtime disabled\n");
    }
  }

  static void OnSave(SKSE::SerializationInterface *serialization) noexcept {
    try {
      auto &runtime = Get();
      if (!runtime.sessionReady || !runtime.playing) {
        SKSE::log::error("Persistence save skipped: no gameplay session");
        return;
      }
      Persistence::Save(runtime.engine.get(), serialization);
    } catch (...) {
      LogCallbackFailure("Persistence save callback failed with an exception");
    }
  }

  static void OnLoad(SKSE::SerializationInterface *serialization) noexcept {
    try {
      Get().Load(serialization);
    } catch (...) {
      LogCallbackFailure("Persistence load callback failed with an exception");
    }
  }

  static void OnRevert(SKSE::SerializationInterface *) noexcept {
    try {
      Get().PrepareForLoad("serialization-revert");
    } catch (...) {
      LogCallbackFailure("Persistence revert callback failed with an exception");
    }
  }

  static void OnMessage(SKSE::MessagingInterface::Message *message) {
    if (!message) {
      return;
    }
    try {
      Get().HandleMessage(*message);
    } catch (...) {
      Disable("Lifecycle callback failed; Skyrim Remember disabled");
    }
  }

  void HandleMessage(const SKSE::MessagingInterface::Message &message) {
    switch (message.type) {
    case SKSE::MessagingInterface::kDataLoaded:
      AttachEventSinks();
      break;
    case SKSE::MessagingInterface::kPreLoadGame:
      PrepareForLoad("pre-load");
      break;
    case SKSE::MessagingInterface::kNewGame:
      Reset("new-game");
      SetPlaying(sessionReady);
      if (playing) {
        appearanceMemory.CaptureBaseline();
      }
      break;
    case SKSE::MessagingInterface::kPostLoadGame:
      // SKSE encodes the load success bool in data itself, not a bool*.
      SetPlaying(sessionReady && message.data != nullptr);
      if (playing) {
        appearanceMemory.CaptureBaseline();
      }
      break;
    default:
      break;
    }
  }

  void AttachEventSinks() {
    if (!rulesLoaded) {
      // Rules outlive every load and new game: they describe the installed mods,
      // not a save. Loading them once here is what keeps a reload cheap.
      const auto rules = ImpressionRules::Load(engine.get());
      appearanceMemory.SetImpressionsEnabled(rules.boundReferences > 0);
      rulesLoaded = true;
    }
    auto *scriptEvents = RE::ScriptEventSourceHolder::GetSingleton();
    if (!scriptEvents) {
      SKSE::log::error("Skyrim Remember disabled: no script event source");
      return;
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
    SKSE::log::info("Skyrim Remember ready: observation starts at the NPC");
    // Print the knobs the metrics have to be read against, so a log pasted
    // into a report is self-describing.
    SKSE::log::info("Observation tuning: scanIntervalMs={} budgetPerSlice={} "
                    "cooldownMs={} maxDistance={} metricsIntervalMs={} "
                    "slowSliceUs={}",
                    ObservationTuning::scanInterval.count(),
                    ObservationTuning::actorBudgetPerScan,
                    std::chrono::duration_cast<std::chrono::milliseconds>(ObservationTuning::observerCooldown).count(),
                    ObservationTuning::maximumObservationDistance,
                    std::chrono::duration_cast<std::chrono::milliseconds>(ObservationTuning::metricsInterval).count(),
                    ObservationTuning::slowSliceThreshold.count());
  }

  void Reset(const char *reason) {
    PrepareForLoad(reason);
    const auto status = skyrim_remember_clear(engine.get());
    sessionReady = status == BridgeStatus::Ok;
    SKSE::log::info("Memory reset: reason={} status={}", reason, static_cast<std::uint32_t>(status));
  }

  void PrepareForLoad(const char *reason) {
    SetPlaying(false);
    sessionReady = false;
    equipment.Reset();
    observations.Reset();
    appearanceMemory.Reset();
    reactions.Reset();
    SKSE::log::info("Memory load preparation: reason={}", reason);
  }

  void Load(SKSE::SerializationInterface *serialization) {
    sessionReady = false;
    const auto outcome = Persistence::Load(engine.get(), serialization);
    sessionReady = outcome == Persistence::LoadOutcome::Restored || outcome == Persistence::LoadOutcome::NoData;
  }
};

} // namespace

bool RegisterRuntime() {
  return Runtime::Get().Register();
}

} // namespace SkyrimRemember
