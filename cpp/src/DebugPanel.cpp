// The in-game debug panel, hosted by SKSE Menu Framework.
//
// This file is compiled without the project's precompiled header and must not
// include CommonLibSSE: the framework's header pulls in <windows.h>, which
// clashes with CommonLib's own Windows declarations. It only reads the
// DebugBoard, a snapshot the main thread fills, so drawing on the render thread
// never touches a Skyrim object.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "DebugPanel.h"
#include "DebugBoard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

// The framework header names RE::InputEvent in a callback type it never makes
// us use; a declaration is all it needs.
namespace RE {
class InputEvent;
} // namespace RE

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <SKSEMenuFramework.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace SkyrimRemember::DebugPanel {
namespace {

namespace ImGui = ImGuiMCP;

std::atomic<DebugBoard *> board{nullptr};

// The overlay only lists the nearest few, so it never covers the screen.
constexpr std::size_t overlayRows = 8;

const ImGui::ImVec4 calm{0.70F, 0.85F, 0.70F, 1.0F};
const ImGui::ImVec4 warning{1.00F, 0.80F, 0.30F, 1.0F};
const ImGui::ImVec4 danger{1.00F, 0.35F, 0.30F, 1.0F};
const ImGui::ImVec4 muted{0.60F, 0.60F, 0.60F, 1.0F};

float Level(const DebugRow &row, Axis axis) {
  return row.axes[AxisSlot(axis)];
}

ImGui::ImVec4 HostilityColor(HostilityVerdict verdict) {
  switch (verdict) {
  case HostilityVerdict::Attack:
  case HostilityVerdict::InCombat:
    return danger;
  case HostilityVerdict::CoolingDown:
    return warning;
  case HostilityVerdict::Calm:
    return calm;
  case HostilityVerdict::Disabled:
  case HostilityVerdict::NotRecognized:
    break;
  }
  return muted;
}

ImGui::ImVec4 SuspicionColor(float suspicion, float threshold) {
  if (suspicion >= threshold) {
    return danger;
  }
  return suspicion >= threshold * 0.5F ? warning : calm;
}

const char *Label(const DebugRow &row) {
  return row.name.empty() ? "(unnamed)" : row.name.c_str();
}

void HostilityCell(const DebugRow &row) {
  if (row.profile != NpcProfile::Guard) {
    ImGui::TextColored(muted, "-");
    return;
  }
  ImGui::TextColored(HostilityColor(row.hostility), "%s", HostilityVerdictName(row.hostility));
}

void RecognitionCell(const DebugRow &row) {
  if (row.recognized) {
    ImGui::TextColored(calm, "yes");
  } else {
    ImGui::TextColored(muted, "no");
  }
}

void DrawSettings(DebugBoard &target) {
  auto settings = target.Hostility();
  bool changed = false;
  changed |= ImGui::Checkbox("Guards attack a suspicious Player", &settings.enabled);
  changed |= ImGui::SliderFloat("Suspicion threshold", &settings.suspicionThreshold, 0.0F, 100.0F, "%.0f");
  int cooldown = static_cast<int>(settings.cooldown.count());
  if (ImGui::SliderInt("Cooldown per guard (s)", &cooldown, 0, 600)) {
    settings.cooldown = std::chrono::seconds{cooldown};
    changed = true;
  }
  if (changed) {
    target.SetHostility(settings);
  }
  ImGui::TextDisabled("Bandit armor raises a guard's suspicion to 40, hide armor to 15. Not saved: every session starts at 35 / 120 s.");

  bool overlay = target.OverlayVisible();
  if (ImGui::Checkbox("Show the live overlay over the HUD", &overlay)) {
    target.SetOverlayVisible(overlay);
  }
}

void DrawStatus(const DebugBoard &target) {
  const auto status = target.Status();
  ImGui::Text("Session: %s", status.playing ? "playing" : "not in game");
  ImGui::Text("Rules: %u impressions, %u of %u references bound to game data", status.impressions, status.boundReferences,
              status.references);
  if (status.boundReferences == 0) {
    ImGui::TextColored(warning, "No rule is bound to game data: impressions are off. See SkyrimRemember.log.");
  }
  ImGui::Text("Attacks started this session: %llu", static_cast<unsigned long long>(status.attacksStarted));
}

void DrawFullTable(const DebugBoard &target) {
  const auto rows = target.Rows();
  const auto threshold = target.Hostility().suspicionThreshold;
  if (rows.empty()) {
    ImGui::TextDisabled("No NPC has perceived the Player in the last %lld seconds.", static_cast<long long>(DebugBoard::rowRetention.count()));
    return;
  }
  constexpr auto flags = ImGui::ImGuiTableFlags_RowBg | ImGui::ImGuiTableFlags_Borders | ImGui::ImGuiTableFlags_SizingFixedFit | ImGui::ImGuiTableFlags_Resizable;
  if (!ImGui::BeginTable("##SkyrimRememberNpcs", 11, flags)) {
    return;
  }
  for (const char *header : {"NPC", "Ref", "Profile", "Dist.", "Recognizes", "Match", "Recognition", "Suspicion", "Fear", "Respect", "Hostility"}) {
    ImGui::TableSetupColumn(header);
  }
  ImGui::TableHeadersRow();
  for (const auto &row : rows) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s", Label(row));
    ImGui::TableNextColumn();
    ImGui::Text("%08X", row.referenceFormId);
    ImGui::TableNextColumn();
    ImGui::Text("%s", ProfileName(row.profile));
    ImGui::TableNextColumn();
    ImGui::Text("%.0f", row.distance);
    ImGui::TableNextColumn();
    RecognitionCell(row);
    ImGui::TableNextColumn();
    ImGui::Text("%.2f / %.2f", row.outfitSimilarity, row.recognitionThreshold);
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", Level(row, Axis::Recognition));
    ImGui::TableNextColumn();
    ImGui::TextColored(SuspicionColor(Level(row, Axis::Suspicion), threshold), "%.1f", Level(row, Axis::Suspicion));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", Level(row, Axis::Fear));
    ImGui::TableNextColumn();
    ImGui::Text("%.1f", Level(row, Axis::Respect));
    ImGui::TableNextColumn();
    HostilityCell(row);
  }
  ImGui::EndTable();
}

void __stdcall RenderPage() {
  auto *target = board.load();
  if (!target) {
    return;
  }
  ImGui::SeparatorText("Status");
  DrawStatus(*target);
  ImGui::SeparatorText("Guard hostility");
  DrawSettings(*target);
  ImGui::SeparatorText("NPCs perceiving the Player");
  DrawFullTable(*target);
}

void __stdcall RenderOverlay() {
  auto *target = board.load();
  if (!target || !target->OverlayVisible()) {
    return;
  }
  const auto rows = target->Rows();
  const auto threshold = target->Hostility().suspicionThreshold;

  constexpr auto windowFlags = ImGui::ImGuiWindowFlags_NoDecoration | ImGui::ImGuiWindowFlags_NoInputs | ImGui::ImGuiWindowFlags_AlwaysAutoResize |
                               ImGui::ImGuiWindowFlags_NoSavedSettings | ImGui::ImGuiWindowFlags_NoFocusOnAppearing | ImGui::ImGuiWindowFlags_NoNav;
  ImGui::SetNextWindowPos(ImGui::ImVec2{16.0F, 16.0F}, ImGui::ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.55F);
  if (ImGui::Begin("Skyrim Remember##Overlay", nullptr, windowFlags)) {
    ImGui::Text("Skyrim Remember");
    if (rows.empty()) {
      ImGui::TextDisabled("nobody is watching");
    } else {
      constexpr auto tableFlags = ImGui::ImGuiTableFlags_RowBg | ImGui::ImGuiTableFlags_SizingFixedFit;
      if (ImGui::BeginTable("##SkyrimRememberOverlay", 6, tableFlags)) {
        for (const char *header : {"NPC", "Dist.", "Knows", "Susp.", "Fear", "Hostility"}) {
          ImGui::TableSetupColumn(header);
        }
        ImGui::TableHeadersRow();
        const auto shown = std::min(rows.size(), overlayRows);
        for (std::size_t index = 0; index < shown; ++index) {
          const auto &row = rows[index];
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Text("%s", Label(row));
          ImGui::TableNextColumn();
          ImGui::Text("%.0f", row.distance);
          ImGui::TableNextColumn();
          RecognitionCell(row);
          ImGui::TableNextColumn();
          ImGui::TextColored(SuspicionColor(Level(row, Axis::Suspicion), threshold), "%.0f", Level(row, Axis::Suspicion));
          ImGui::TableNextColumn();
          ImGui::Text("%.0f", Level(row, Axis::Fear));
          ImGui::TableNextColumn();
          HostilityCell(row);
        }
        ImGui::EndTable();
      }
      if (rows.size() > overlayRows) {
        ImGui::TextDisabled("+%zu more in the Mod Control Panel", rows.size() - overlayRows);
      }
    }
  }
  ImGui::End();
}

} // namespace

Registration Register(DebugBoard *target) {
  if (!SKSEMenuFramework::IsInstalled()) {
    return Registration::FrameworkMissing;
  }
  board.store(target);
  SKSEMenuFramework::SetSection("Skyrim Remember");
  SKSEMenuFramework::AddSectionItem("Memory debugger", RenderPage);
  // Owned by the framework for the rest of the process, like the page above.
  SKSEMenuFramework::AddHudElement(RenderOverlay);
  return Registration::Registered;
}

} // namespace SkyrimRemember::DebugPanel
