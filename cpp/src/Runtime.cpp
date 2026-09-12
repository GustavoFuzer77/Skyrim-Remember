#include "Runtime.h"
#include "EquipmentBatch.h"
#include "GuardMemoryAdapter.h"
#include "NpcObservationService.h"
#include "NpcProfile.h"
#include "ObservationTuning.h"
#include "Persistence.h"
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

// Owns the session: the Rust engine, the scan, the guard adapter (and futures adapters) and the
// equipment batch. Everything below runs on Skyrim's main thread.
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

    adapter.Configure(engine.get());

    // Runtime is a function-local static, so it outlives every task the batch
    // and the observation service schedule.
    equipment.Configure(tasks, [this](std::uint64_t batchedEvents) {
      this->FlushEquipmentBatch(batchedEvents);
    });

    observations.Configure(tasks, [this](const NpcObservationContext &context, const PerceptionOutcome &outcome, NpcObservationService::Trigger trigger) {
      this->adapter.OnObserved(context, outcome, trigger);
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
  bool classified = false;
  bool playing = false;
  bool sessionReady = false;

  using EnginePtr = std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>; // <tipo do pointer, tipo do deleter>
  EnginePtr engine{nullptr, skyrim_remember_destroy};

#pragma region adapters

  GuardMemoryAdapter adapter;

  // add others adapters here (citizens, generics, followers, etc)

#pragma endregion

  // Declared last on purpose: its destructor stops and joins the timer thread
  // before the adapters and the engine its tasks reach into are destroyed.
  NpcObservationService observations;

  void SetPlaying(bool value) {
    playing = value;
    adapter.SetSessionActive(value);
    observations.SetActive(value);
  }

  void FlushEquipmentBatch(std::uint64_t batchedEvents) {

    if (!playing) {
      return;
    }
    // An outfit change is a discrete event: every guard watching right now must
    // see it, so this one sweep ignores the budget and the cooldown.
    if (adapter.BeginAppearanceTransition(batchedEvents)) {
      observations.ScanNow(NpcObservationService::Trigger::AppearanceChanged);
    }
    adapter.EndAppearanceTransition();
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
        adapter.CaptureBaseline();
      }
      break;
    case SKSE::MessagingInterface::kPostLoadGame:
      // SKSE encodes the load success bool in data itself, not a bool*.
      SetPlaying(sessionReady && message.data != nullptr);
      if (playing) {
        adapter.CaptureBaseline();
      }
      break;
    default:
      break;
    }
  }

  void AttachEventSinks() {
    if (!classified) {
      classified = InitializeClassification();
    }
    if (!classified) {
      SKSE::log::error("Skyrim Remember disabled: classification unavailable");
      return;
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
    adapter.Reset();
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
