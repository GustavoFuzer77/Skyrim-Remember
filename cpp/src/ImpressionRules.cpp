#include "ImpressionRules.h"

#include <RE/B/BGSKeyword.h>
#include <RE/T/TESDataHandler.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace SkyrimRemember::ImpressionRules {
namespace {

std::vector<std::filesystem::path> FindRuleFiles() {
  std::error_code error;
  const std::filesystem::path root{directory};
  if (!std::filesystem::is_directory(root, error)) {
    SKSE::log::info("Impression rules: no {} folder, no rules loaded", directory);
    return {};
  }
  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file(error) && name.ends_with(fileSuffix)) {
      files.push_back(entry.path());
    }
  }
  if (error) {
    SKSE::log::error("Impression rules: cannot list {}: {}", directory, error.message());
  }
  // Name order makes load order, and so temperament overrides, predictable.
  std::ranges::sort(files);
  return files;
}

bool LoadFile(const MemoryEngine *engine, const std::filesystem::path &path) {
  const auto name = path.filename().string();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SKSE::log::error("Impression rules rejected: file={} reason=cannot open", name);
    return false;
  }
  const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) {
    SKSE::log::error("Impression rules rejected: file={} reason=unreadable", name);
    return false;
  }
  if (bytes.empty()) {
    SKSE::log::error("Impression rules rejected: file={} reason=empty", name);
    return false;
  }

  std::array<std::uint8_t, 512> reason{};
  const auto status = skyrim_remember_load_rules(engine, bytes.data(), bytes.size(), reason.data(), reason.size());
  if (status != BridgeStatus::Ok) {
    SKSE::log::error("Impression rules rejected: file={} status={} reason={}",
                     name,
                     static_cast<std::uint32_t>(status),
                     std::string_view{static_cast<const char *>(static_cast<void *>(reason.data()))});
    return false;
  }
  SKSE::log::info("Impression rules loaded: file={}", name);
  return true;
}

// Keywords keep their EditorID at runtime, so one pass indexes them all instead
// of scanning every keyword once per reference.
std::unordered_map<std::string_view, RE::FormID> IndexKeywords(RE::TESDataHandler &data) {
  std::unordered_map<std::string_view, RE::FormID> keywords;
  for (const auto *keyword : data.GetFormArray<RE::BGSKeyword>()) {
    const auto *editorId = keyword ? keyword->GetFormEditorID() : nullptr;
    if (editorId && *editorId != '\0') {
      keywords.emplace(editorId, keyword->GetFormID());
    }
  }
  return keywords;
}

void BindReferences(const MemoryEngine *engine, std::uint32_t count) {
  auto *data = RE::TESDataHandler::GetSingleton();
  if (!data) {
    SKSE::log::error("Impression rules unbound: TESDataHandler unavailable");
    return;
  }
  const auto keywords = IndexKeywords(*data);

  std::vector<std::uint8_t> name(128);
  for (std::uint32_t index = 0; index < count; ++index) {
    RuleReferenceInfo info{};
    auto status = skyrim_remember_rule_reference(engine, index, &info, name.data(), name.size());
    if (status == BridgeStatus::BufferTooSmall) {
      name.resize(info.nameLength);
      status = skyrim_remember_rule_reference(engine, index, &info, name.data(), name.size());
    }
    if (status != BridgeStatus::Ok) {
      SKSE::log::error("Impression rule reference unreadable: index={} status={}", index, static_cast<std::uint32_t>(status));
      continue;
    }
    const std::string_view text{static_cast<const char *>(static_cast<void *>(name.data())), info.nameLength};

    RE::FormID resolved = 0;
    if (info.kind == RuleReferenceKind::Keyword) {
      if (const auto found = keywords.find(text); found != keywords.end()) {
        resolved = found->second;
      } else {
        SKSE::log::warn("Impression rule keyword not found: {}", text);
      }
    } else {
      resolved = data->LookupFormID(info.localFormId, text);
      if (resolved == 0) {
        SKSE::log::warn("Impression rule form not found: 0x{:06X}~{}", info.localFormId, text);
      }
    }
    if (resolved != 0) {
      skyrim_remember_bind_rule_reference(engine, index, resolved);
    }
  }
}

} // namespace

RulesSummary Load(const MemoryEngine *engine) {
  RulesSummary summary{};
  if (!engine) {
    return summary;
  }
  std::size_t loaded = 0;
  for (const auto &path : FindRuleFiles()) {
    if (LoadFile(engine, path)) {
      ++loaded;
    }
  }
  if (skyrim_remember_rules_summary(engine, &summary) != BridgeStatus::Ok) {
    SKSE::log::error("Impression rules unavailable: summary failed");
    return {};
  }
  BindReferences(engine, summary.references);
  skyrim_remember_rules_summary(engine, &summary);
  SKSE::log::info(
      "Impression rules ready: files={} impressions={} references={} bound={}", loaded, summary.impressions, summary.references, summary.boundReferences);
  return summary;
}

} // namespace SkyrimRemember::ImpressionRules
