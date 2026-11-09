#include "Memory.h"
#include "Appearance.h"

#include <algorithm>
#include <cmath>
#include <compare>

namespace SkyrimRemember {
namespace {

// Biped slot groups. Bit numbers are CommonLibSSE's BipedObjectSlot indices,
// not Creation Kit slot numbers.
constexpr std::uint32_t headSlots = (1U << 0) | (1U << 1) | (1U << 12) | (1U << 13) | (1U << 14) | (1U << 25);
constexpr std::uint32_t bodySlots = (1U << 2) | (1U << 16) | (1U << 17) | (1U << 19) | (1U << 22) | (1U << 26) | (1U << 27);
constexpr std::uint32_t handSlots = (1U << 3) | (1U << 4) | (1U << 28) | (1U << 29);
constexpr std::uint32_t feetSlots = (1U << 7) | (1U << 8) | (1U << 23) | (1U << 24);
constexpr std::uint32_t shieldSlots = 1U << 9;
constexpr std::uint32_t accessorySlots = (1U << 5) | (1U << 6) | (1U << 10) | (1U << 11) | (1U << 15) | (1U << 18) | (1U << 30) | (1U << 31);

struct CategoryMask {
  AppearanceCategory category;
  std::uint32_t mask;
};

constexpr std::array categories = {
    CategoryMask{AppearanceCategory::Head, headSlots},
    CategoryMask{AppearanceCategory::Body, bodySlots},
    CategoryMask{AppearanceCategory::Hands, handSlots},
    CategoryMask{AppearanceCategory::Feet, feetSlots},
    CategoryMask{AppearanceCategory::Shield, shieldSlots},
    CategoryMask{AppearanceCategory::Accessories, accessorySlots},
};

bool ValidGameDays(double gameDays) {
  return std::isfinite(gameDays) && gameDays >= 0.0;
}

std::uint32_t Count(std::size_t value) {
  return static_cast<std::uint32_t>(std::min<std::size_t>(value, UINT32_MAX));
}

float BoundedAdd(float value, float delta) {
  return std::clamp(value + delta, 0.0F, maxMemoryValue);
}

float Decay(float value, double elapsedHours, double halfLifeHours) {
  // A slot with no configured half-life does not fade. Zeroing it instead would
  // silently erase a newly added axis whose tuning was forgotten, which is
  // exactly the mistake the reserve slots invite.
  if (value <= 0.0F || !std::isfinite(halfLifeHours) || halfLifeHours <= 0.0) {
    return std::max(value, 0.0F);
  }
  return static_cast<float>(static_cast<double>(value) * std::pow(0.5, elapsedHours / halfLifeHours));
}

void ApplyDecay(NpcMemory &memory, double currentGameDays, const DecayHalfLives &halfLives) {
  if (!std::isfinite(currentGameDays) || currentGameDays <= memory.lastDecayGameDays) {
    return;
  }
  const auto elapsedHours = (currentGameDays - memory.lastDecayGameDays) * 24.0;
  for (std::size_t slot = 0; slot < axisCapacity; ++slot) {
    memory.axes[slot] = Decay(memory.axes[slot], elapsedHours, halfLives.hours[slot]);
  }
  memory.lastDecayGameDays = currentGameDays;
}

bool RecognitionIsDue(const NpcMemory &memory, double currentGameDays, double intervalGameDays) {
  return !memory.lastRecognitionGameDays || currentGameDays - *memory.lastRecognitionGameDays >= intervalGameDays;
}

void ReinforceRecognition(NpcMemory &memory, double gameDays, float gain) {
  auto &recognition = memory.axes[AxisSlot(Axis::Recognition)];
  recognition = BoundedAdd(recognition, gain);
  memory.lastRecognitionGameDays = gameDays;
}

MemorySnapshot ToSnapshot(const NpcMemory &memory) {
  return {memory.axes, memory.lastDecayGameDays};
}

AppearanceResult WithFace(AppearanceResult result, bool faceVisible) {
  result.faceVisible = faceVisible;
  return result;
}

} // namespace

bool NpcMemory::IsValid() const {
  const auto validTime = [](double value) { return std::isfinite(value) && value >= 0.0; };
  return std::ranges::all_of(axes, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= maxMemoryValue; }) && validTime(lastDecayGameDays) &&
         (!lastRecognitionGameDays || validTime(*lastRecognitionGameDays));
}

void NormalizeItems(std::vector<AppearanceItem> &items) {
  std::erase_if(items, [](AppearanceItem &item) {
    item.slotMask &= ~technicalSlots;
    return item.formId == 0 || item.slotMask == 0;
  });
  std::ranges::sort(items, {}, &AppearanceItem::formId);
  std::vector<AppearanceItem> normalized;
  normalized.reserve(items.size());
  for (const auto &item : items) {
    if (!normalized.empty() && normalized.back().formId == item.formId) {
      normalized.back().slotMask |= item.slotMask;
    } else {
      normalized.push_back(item);
    }
  }
  items = std::move(normalized);
}

float OutfitSimilarity(std::span<const AppearanceItem> left, std::span<const AppearanceItem> right, const AppearanceWeights &weights) {
  const auto idsIn = [](std::span<const AppearanceItem> items, std::uint32_t mask) {
    std::vector<std::uint32_t> ids;
    for (const auto &item : items) {
      if ((item.slotMask & mask) != 0) {
        ids.push_back(item.formId);
      }
    }
    return ids;
  };

  float score = 0.0F;
  float applicableWeight = 0.0F;
  for (const auto &[category, mask] : categories) {
    const auto leftIds = idsIn(left, mask);
    const auto rightIds = idsIn(right, mask);
    if (leftIds.empty() && rightIds.empty()) {
      continue;
    }
    const auto weight = weights.Get(category);
    applicableWeight += weight;
    const auto intersection =
        static_cast<float>(std::ranges::count_if(leftIds, [&rightIds](std::uint32_t id) { return std::ranges::find(rightIds, id) != rightIds.end(); }));
    const auto unionSize = static_cast<float>(leftIds.size() + rightIds.size()) - intersection;
    if (unionSize > 0.0F) {
      score += weight * (intersection / unionSize);
    }
  }
  if (applicableWeight > 0.0F) {
    return score / applicableWeight;
  }
  return std::ranges::equal(left, right) ? 1.0F : 0.0F;
}

MemoryStore::MemoryStore(Tuning tuning) : tuning(tuning) {}

NpcMemory *MemoryStore::Touch(std::uint32_t referenceFormId, double gameDays) {
  const auto found = memories.find(referenceFormId);
  if (found == memories.end()) {
    return nullptr;
  }
  ApplyDecay(found->second, gameDays, tuning.decay);
  return &found->second;
}

NpcMemory &MemoryStore::TouchOrCreate(std::uint32_t referenceFormId, double gameDays) {
  auto [found, inserted] = memories.try_emplace(referenceFormId);
  if (inserted) {
    found->second.lastDecayGameDays = gameDays;
  }
  ApplyDecay(found->second, gameDays, tuning.decay);
  return found->second;
}

MemoryStatus MemoryStore::Register(std::uint32_t referenceFormId, double gameDays) {
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  if (!ValidGameDays(gameDays)) {
    return MemoryStatus::InvalidArgument;
  }
  const std::scoped_lock lock(mutex);
  const auto created = !memories.contains(referenceFormId);
  TouchOrCreate(referenceFormId, gameDays);
  return created ? MemoryStatus::Created : MemoryStatus::Ok;
}

MemoryStatus MemoryStore::Get(std::uint32_t referenceFormId, double gameDays, MemorySnapshot &output) {
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  if (!ValidGameDays(gameDays)) {
    return MemoryStatus::InvalidArgument;
  }
  const std::scoped_lock lock(mutex);
  const auto *memory = Touch(referenceFormId, gameDays);
  if (!memory) {
    return MemoryStatus::NotFound;
  }
  output = ToSnapshot(*memory);
  return MemoryStatus::Ok;
}

MemoryStatus MemoryStore::ApplyDelta(std::uint32_t referenceFormId, double gameDays, const MemoryDelta &delta, MemorySnapshot &output) {
  if (!ValidGameDays(gameDays) || !std::ranges::all_of(delta.axes, [](float value) { return std::isfinite(value); })) {
    return MemoryStatus::InvalidArgument;
  }
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  const std::scoped_lock lock(mutex);
  auto &memory = TouchOrCreate(referenceFormId, gameDays);
  for (std::size_t slot = 0; slot < axisCapacity; ++slot) {
    memory.axes[slot] = BoundedAdd(memory.axes[slot], delta.axes[slot]);
  }
  output = ToSnapshot(memory);
  return MemoryStatus::Ok;
}

std::optional<AppearanceResult> MemoryStore::EvaluateNormalized(std::uint32_t referenceFormId, std::span<const AppearanceItem> items, bool faceVisible,
                                                                bool visualContinuity) const {
  const auto found = memories.find(referenceFormId);
  if (found == memories.end()) {
    return std::nullopt;
  }
  const auto threshold = tuning.recognition.Threshold(found->second.axes[AxisSlot(Axis::Recognition)]);
  const auto knownEntry = appearances.find(referenceFormId);
  const std::span<const KnownAppearance> known = knownEntry == appearances.end() ? std::span<const KnownAppearance>{} : std::span{knownEntry->second};

  float bestSimilarity = 0.0F;
  float bestKnownIdentitySimilarity = 0.0F;
  for (const auto &previous : known) {
    const auto similarity = OutfitSimilarity(previous.items, items, tuning.weights);
    bestSimilarity = std::max(bestSimilarity, similarity);
    if (previous.identityKnown) {
      bestKnownIdentitySimilarity = std::max(bestKnownIdentitySimilarity, similarity);
    }
  }
  const auto identityConfidence = (faceVisible || visualContinuity) ? 1.0F : bestKnownIdentitySimilarity;

  AppearanceResult result;
  result.outfitSimilarity = bestSimilarity;
  result.identityConfidence = identityConfidence;
  result.recognitionThreshold = threshold;
  result.appearanceMatched = !known.empty() && bestSimilarity >= threshold;
  result.recognized = identityConfidence >= threshold;
  result.linkedByContinuity = visualContinuity && !faceVisible && bestKnownIdentitySimilarity < threshold;
  result.learned = false;
  result.itemCount = Count(items.size());
  result.knownAppearances = Count(known.size());
  return result;
}

MemoryStatus MemoryStore::EvaluateAppearance(std::uint32_t referenceFormId, std::span<const AppearanceItem> items, bool faceVisible, bool visualContinuity,
                                             AppearanceResult &output) {
  if (items.size() > maxAppearanceItems) {
    return MemoryStatus::InvalidArgument;
  }
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  std::vector<AppearanceItem> normalized(items.begin(), items.end());
  NormalizeItems(normalized);

  const std::scoped_lock lock(mutex);
  if (!memories.contains(referenceFormId)) {
    return MemoryStatus::NotFound;
  }
  if (normalized.size() > tuning.limits.maxAppearanceItems) {
    return MemoryStatus::InvalidArgument;
  }
  const auto verdict = EvaluateNormalized(referenceFormId, normalized, faceVisible, visualContinuity);
  output = WithFace(*verdict, faceVisible);
  return MemoryStatus::Ok;
}

MemoryStatus MemoryStore::ObserveAppearance(std::uint32_t referenceFormId, double gameDays, std::span<const AppearanceItem> items, bool faceVisible,
                                            bool visualContinuity, AppearanceResult &output) {
  if (!ValidGameDays(gameDays) || items.size() > maxAppearanceItems) {
    return MemoryStatus::InvalidArgument;
  }
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  std::vector<AppearanceItem> normalized(items.begin(), items.end());
  NormalizeItems(normalized);

  const std::scoped_lock lock(mutex);
  const auto memoryEntry = memories.find(referenceFormId);
  if (memoryEntry == memories.end()) {
    return MemoryStatus::NotFound;
  }
  if (normalized.size() > tuning.limits.maxAppearanceItems) {
    return MemoryStatus::InvalidArgument;
  }
  auto verdict = *EvaluateNormalized(referenceFormId, normalized, faceVisible, visualContinuity);

  const auto &observation = tuning.observation;
  const auto reinforceRecognition = faceVisible && RecognitionIsDue(memoryEntry->second, gameDays, observation.recognitionIntervalGameDays);
  auto &known = appearances[referenceFormId];

  bool learned = false;
  if (known.empty()) {
    // The first outfit an NPC sees is always learned.
    known.push_back({normalized, faceVisible, verdict.recognized, gameDays});
    learned = true;
  } else {
    // Seeing the Player's face can link a previously anonymous outfit to the
    // Player, but only when the clothing evidence is strong enough.
    if (faceVisible) {
      for (auto &previous : known) {
        if (!previous.identityKnown && OutfitSimilarity(previous.items, normalized, tuning.weights) >= verdict.recognitionThreshold) {
          previous.identityKnown = true;
        }
      }
    }
    const auto exact = std::ranges::find_if(known, [&](const KnownAppearance &previous) { return previous.faceVisible == faceVisible && previous.items == normalized; });
    if (verdict.recognized) {
      if (exact != known.end()) {
        // Seeing a known outfit again is not news. Only move the freshness
        // stamp once the refresh interval has really passed, so repeated
        // polling is idempotent.
        if (gameDays - exact->lastSeenGameDays >= observation.refreshIntervalGameDays) {
          exact->lastSeenGameDays = gameDays;
        }
        exact->identityKnown = true;
      } else {
        if (known.size() >= tuning.limits.maxKnownAppearances) {
          const auto oldest = std::ranges::min_element(known, [](const KnownAppearance &left, const KnownAppearance &right) {
            return std::strong_order(left.lastSeenGameDays, right.lastSeenGameDays) < 0;
          });
          known.erase(oldest);
        }
        known.push_back({normalized, faceVisible, true, gameDays});
        learned = true;
      }
    }
  }

  verdict.learned = learned;
  verdict.knownAppearances = Count(known.size());
  if (reinforceRecognition) {
    ReinforceRecognition(memoryEntry->second, gameDays, observation.recognitionGain);
  }
  output = WithFace(verdict, faceVisible);
  return MemoryStatus::Ok;
}

MemoryStatus MemoryStore::ObserveImpression(std::uint32_t referenceFormId, double gameDays, NpcProfile profile, std::span<const AppearanceItem> items,
                                            std::span<const std::uint32_t> keywords, ImpressionResult &output) {
  if (!ValidGameDays(gameDays) || items.size() > maxAppearanceItems || keywords.size() > maxWornKeywords || static_cast<std::size_t>(profile) >= npcProfileCount) {
    return MemoryStatus::InvalidArgument;
  }
  if (referenceFormId == 0) {
    return MemoryStatus::InvalidId;
  }
  std::vector<std::uint32_t> worn;
  worn.reserve(items.size() + keywords.size());
  for (const auto &item : items) {
    worn.push_back(item.formId);
  }
  worn.insert(worn.end(), keywords.begin(), keywords.end());

  const std::scoped_lock lock(mutex);
  const auto outcome = rules.Evaluate(profile, worn);
  auto *memory = Touch(referenceFormId, gameDays);
  if (!memory) {
    return MemoryStatus::NotFound;
  }
  // Raise, never lower: a stronger feeling from an earlier event survives an
  // outfit that implies less.
  std::uint32_t raisedAxes = 0;
  for (std::size_t slot = 0; slot < axisCapacity; ++slot) {
    const auto target = outcome.targets[slot];
    auto &value = memory->axes[slot];
    if (target > value) {
      if (target - value >= noticeableRise) {
        raisedAxes |= 1U << slot;
      }
      value = target;
    }
  }
  output = {outcome.targets, raisedAxes, outcome.matchedRules};
  return MemoryStatus::Ok;
}

std::optional<std::string> MemoryStore::LoadRules(std::string_view json) {
  if (json.empty() || json.size() > maxRuleFileBytes) {
    return "rule file is empty or larger than the allowed size";
  }
  const std::scoped_lock lock(mutex);
  return rules.LoadJson(json);
}

RulesSummary MemoryStore::GetRulesSummary() const {
  const std::scoped_lock lock(mutex);
  return {Count(rules.ImpressionCount()), Count(rules.References().size()), Count(rules.BoundCount())};
}

std::optional<RuleReference> MemoryStore::GetRuleReference(std::uint32_t index) const {
  const std::scoped_lock lock(mutex);
  if (index >= rules.References().size()) {
    return std::nullopt;
  }
  return rules.References()[index];
}

MemoryStatus MemoryStore::BindRuleReference(std::uint32_t index, std::uint32_t runtimeFormId) {
  const std::scoped_lock lock(mutex);
  return rules.Bind(index, runtimeFormId) ? MemoryStatus::Ok : MemoryStatus::NotFound;
}

void MemoryStore::Clear() {
  const std::scoped_lock lock(mutex);
  memories.clear();
  appearances.clear();
}

std::size_t MemoryStore::Size() const {
  const std::scoped_lock lock(mutex);
  return memories.size();
}

std::optional<NpcMemory> MemoryStore::Find(std::uint32_t referenceFormId) const {
  const std::scoped_lock lock(mutex);
  const auto found = memories.find(referenceFormId);
  return found == memories.end() ? std::nullopt : std::optional{found->second};
}

std::vector<KnownAppearance> MemoryStore::KnownAppearances(std::uint32_t referenceFormId) const {
  const std::scoped_lock lock(mutex);
  const auto found = appearances.find(referenceFormId);
  return found == appearances.end() ? std::vector<KnownAppearance>{} : found->second;
}

} // namespace SkyrimRemember
