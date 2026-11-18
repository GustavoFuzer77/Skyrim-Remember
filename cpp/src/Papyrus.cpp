#include "Papyrus.h"
#include "Memory.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>

// Script calls arrive on the Papyrus VM's threads, not on the main thread.
// Everything below only reads the Actor's FormID and the calendar, and talks to
// the memory store, which locks for itself.
namespace SkyrimRemember::Papyrus {
namespace {

MemoryStore *memory = nullptr;

// Papyrus strings compare without case, so "Suspicion" works as well as
// "suspicion".
std::optional<Axis> AxisFromScript(std::string_view name) {
  for (const auto axis : namedAxes) {
    const std::string_view known = AxisName(axis);
    if (known.size() == name.size() &&
        std::equal(known.begin(), known.end(), name.begin(), [](char left, char right) { return left == std::tolower(static_cast<unsigned char>(right)); })) {
      return axis;
    }
  }
  return std::nullopt;
}

std::optional<double> GameDays() {
  const auto *calendar = RE::Calendar::GetSingleton();
  if (!calendar) {
    return std::nullopt;
  }
  const auto days = calendar->GetDaysPassed();
  return std::isfinite(days) && days >= 0.0F ? std::optional{static_cast<double>(days)} : std::nullopt;
}

// The current level of one axis, or -1 when the arguments make no sense. An
// NPC with no memory of the Player feels nothing: 0.
float GetAxis(RE::StaticFunctionTag *, RE::Actor *npc, RE::BSFixedString axisName) {
  const auto axis = AxisFromScript(axisName.c_str());
  const auto days = GameDays();
  if (!memory || !npc || !axis || !days) {
    return -1.0F;
  }
  MemorySnapshot snapshot;
  const auto status = memory->Get(npc->GetFormID(), *days, snapshot);
  if (status == MemoryStatus::NotFound) {
    return 0.0F;
  }
  return status == MemoryStatus::Ok ? snapshot.axes[AxisSlot(*axis)] : -1.0F;
}

bool HasMemory(RE::StaticFunctionTag *, RE::Actor *npc) {
  return memory && npc && memory->Find(npc->GetFormID()).has_value();
}

// Lets a script feed an event into an NPC's memory: being helped, threatened,
// robbed. Creates the memory if there is none. Returns the new level, or -1.
float ModAxis(RE::StaticFunctionTag *, RE::Actor *npc, RE::BSFixedString axisName, float delta) {
  const auto axis = AxisFromScript(axisName.c_str());
  const auto days = GameDays();
  if (!memory || !npc || !axis || !days || !std::isfinite(delta)) {
    return -1.0F;
  }
  MemoryDelta change{};
  change.axes[AxisSlot(*axis)] = delta;
  MemorySnapshot snapshot;
  if (memory->ApplyDelta(npc->GetFormID(), *days, change, snapshot) != MemoryStatus::Ok) {
    return -1.0F;
  }
  SKSE::log::info("Papyrus ModAxis: npc={:08X} axis={} delta={} level={}", npc->GetFormID(), AxisName(*axis), delta, snapshot.axes[AxisSlot(*axis)]);
  return snapshot.axes[AxisSlot(*axis)];
}

bool RegisterFunctions(RE::BSScript::IVirtualMachine *vm) {
  if (!vm) {
    return false;
  }
  vm->RegisterFunction("GetAxis", scriptClass, GetAxis);
  vm->RegisterFunction("HasMemory", scriptClass, HasMemory);
  vm->RegisterFunction("ModAxis", scriptClass, ModAxis);
  SKSE::log::info("Papyrus functions registered: class={}", scriptClass);
  return true;
}

} // namespace

bool Register(MemoryStore *store) {
  memory = store;
  const auto *papyrus = SKSE::GetPapyrusInterface();
  return papyrus && papyrus->Register(RegisterFunctions);
}

bool StartCombatWithPlayer(RE::Actor &actor) {
  auto *vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
  auto *player = RE::PlayerCharacter::GetSingleton();
  if (!vm || !player) {
    return false;
  }
  auto *policy = vm->GetObjectHandlePolicy();
  if (!policy) {
    return false;
  }
  const auto handle = policy->GetHandleForObject(RE::Actor::FORMTYPE, &actor);
  if (handle == policy->EmptyHandle()) {
    return false;
  }
  RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback;
  auto *arguments = RE::MakeFunctionArguments(static_cast<RE::Actor *>(player));
  return vm->DispatchMethodCall(handle, "Actor", "StartCombat", arguments, noCallback);
}

} // namespace SkyrimRemember::Papyrus
