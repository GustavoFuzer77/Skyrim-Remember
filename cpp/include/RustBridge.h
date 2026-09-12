#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Hand-written mirror of the `#[no_mangle]` surface in rust/src/bridge.rs and
// rust/src/lib.rs. Three independent checks keep the two sides honest:
//   * the static_asserts below pin this side's layout;
//   * `bridge::tests::abi_layout_matches_cpp_contract` pins the Rust side's;
//   * BridgeSmoke compares the two version constants at run time.
// Bump the version on both sides for any signature, layout, or field-meaning
// change.
struct MemoryEngine;

inline constexpr std::uint32_t SkyrimRememberAbiVersion = 9;

// How many axis slots every memory carries. Deliberately larger than the number
// of axes named below: the spare slots are what let a new axis be named without
// resizing any struct here or bumping the persisted payload. An older save
// loads an axis it never knew at zero, which is the correct value.
inline constexpr std::size_t SkyrimRememberAxisCapacity = 16;

// One dimension of what an NPC feels about the Player. The slot numbers are
// part of the ABI: Rust indexes the same array by the same values, so
// reordering these would silently reinterpret every saved memory.
enum class Axis : std::uint32_t {
  Recognition = 0,
  Suspicion = 1,
  Fear = 2,
  Respect = 3,
  // Reserved for the Follower profile; nothing writes these yet.
  Approval = 4,
  Loyalty = 5,
  Resentment = 6,
  Trust = 7
};

constexpr std::size_t AxisSlot(Axis axis) {
  return static_cast<std::size_t>(axis);
}

enum class BridgeStatus : std::uint32_t {
  Ok = 0,
  Created = 1,
  InvalidId = 2,
  NotFound = 3,
  InvalidArgument = 4,
  Unavailable = 5,
  BufferTooSmall = 6,
  InvalidData = 7,
  UnsupportedVersion = 8
};

// Indexed with AxisSlot(Axis::...). Slots past the named axes read as zero.
struct MemorySnapshot {
  float axes[SkyrimRememberAxisCapacity];
  double lastDecayGameDays;
};

// One change per axis slot. A slot left at zero leaves that axis alone.
struct MemoryDelta {
  float axes[SkyrimRememberAxisCapacity];
};

struct PersistenceStats {
  std::uint32_t loaded;
  std::uint32_t unresolved;
};

struct AppearanceItem {
  std::uint32_t formId;
  std::uint32_t slotMask;

  bool operator==(const AppearanceItem &) const = default;
};

struct AppearanceResult {
  float outfitSimilarity;
  float identityConfidence;
  float recognitionThreshold;
  std::uint32_t appearanceMatched;
  std::uint32_t recognized;
  std::uint32_t linkedByContinuity;
  std::uint32_t learned;
  std::uint32_t faceVisible;
  std::uint32_t itemCount;
  std::uint32_t knownAppearances;
};

// Hard cap on the keywords passed with one impression, mirroring
// MAX_WORN_KEYWORDS in rust/src/rules.rs.
inline constexpr std::size_t SkyrimRememberMaxWornKeywords = 512;

// What one outfit did to one NPC's axes.
struct ImpressionResult {
  // The level each slot was raised to; indexed with AxisSlot(Axis::...).
  float targets[SkyrimRememberAxisCapacity];
  // One bit per slot that rose noticeably: 1 << AxisSlot(axis).
  std::uint32_t raisedAxes;
  std::uint32_t matchedRules;
};

enum class RuleReferenceKind : std::uint32_t { Keyword = 0, Form = 1 };

// One keyword or Form a rule file mentions. The name — a keyword EditorID or a
// plugin file name — is copied into a separate buffer and is not NUL-terminated.
struct RuleReferenceInfo {
  RuleReferenceKind kind;
  // Local FormID inside the plugin for a Form; zero for a keyword.
  std::uint32_t localFormId;
  std::uint32_t nameLength;
};

struct RulesSummary {
  std::uint32_t impressions;
  std::uint32_t references;
  std::uint32_t boundReferences;
};

static_assert(std::is_standard_layout_v<MemorySnapshot>);
static_assert(sizeof(MemorySnapshot) == 72 && alignof(MemorySnapshot) == 8);
static_assert(offsetof(MemorySnapshot, axes) == 0);
static_assert(offsetof(MemorySnapshot, lastDecayGameDays) == 64);
static_assert(sizeof(MemoryDelta) == 64 && alignof(MemoryDelta) == 4);
static_assert(AxisSlot(Axis::Recognition) == 0);
static_assert(AxisSlot(Axis::Suspicion) == 1);
static_assert(AxisSlot(Axis::Fear) == 2);
static_assert(AxisSlot(Axis::Respect) == 3);
static_assert(AxisSlot(Axis::Approval) == 4);
static_assert(AxisSlot(Axis::Loyalty) == 5);
static_assert(AxisSlot(Axis::Resentment) == 6);
static_assert(AxisSlot(Axis::Trust) == 7);
static_assert(AxisSlot(Axis::Trust) < SkyrimRememberAxisCapacity);
static_assert(sizeof(PersistenceStats) == 8);
static_assert(sizeof(AppearanceItem) == 8 && alignof(AppearanceItem) == 4);
static_assert(sizeof(AppearanceResult) == 40 && alignof(AppearanceResult) == 4);
static_assert(offsetof(AppearanceResult, outfitSimilarity) == 0);
static_assert(offsetof(AppearanceResult, appearanceMatched) == 12);
static_assert(offsetof(AppearanceResult, recognized) == 16);
static_assert(offsetof(AppearanceResult, linkedByContinuity) == 20);
static_assert(offsetof(AppearanceResult, knownAppearances) == 36);
static_assert(sizeof(BridgeStatus) == 4);
static_assert(sizeof(ImpressionResult) == 72 && alignof(ImpressionResult) == 4);
static_assert(offsetof(ImpressionResult, raisedAxes) == 64);
static_assert(offsetof(ImpressionResult, matchedRules) == 68);
static_assert(sizeof(RuleReferenceInfo) == 12);
static_assert(offsetof(RuleReferenceInfo, nameLength) == 8);
static_assert(sizeof(RulesSummary) == 12);

extern "C" {
// Callbacks must not throw or retain the message pointer.
using SkyrimRememberLogCallback = void (*)(const char *message) noexcept;

// Returns true after the Rust core has initialized successfully.
bool skyrim_remember_initialize(SkyrimRememberLogCallback log_callback) noexcept;

// ABI version allows both sides to reject incompatible future bridges.
std::uint32_t skyrim_remember_abi_version() noexcept;

// Contexts are owned by Rust. Never delete them from C++.
MemoryEngine *skyrim_remember_create() noexcept;

// No other call may be in flight when destroying the context.
void skyrim_remember_destroy(MemoryEngine *engine) noexcept;

BridgeStatus skyrim_remember_register(const MemoryEngine *engine, std::uint32_t reference_form_id, double game_days) noexcept;

// On errors output remains unchanged; querying does not create a memory.
BridgeStatus skyrim_remember_get(const MemoryEngine *engine, std::uint32_t reference_form_id, double game_days, MemorySnapshot *output) noexcept;

BridgeStatus skyrim_remember_apply_delta(const MemoryEngine *engine, std::uint32_t reference_form_id, double game_days, MemoryDelta delta,
                                         MemorySnapshot *output) noexcept;

BridgeStatus skyrim_remember_observe_appearance(const MemoryEngine *engine, std::uint32_t reference_form_id, double game_days, const AppearanceItem *items,
                                                std::size_t item_count, std::uint32_t face_visible, std::uint32_t visual_continuity,
                                                AppearanceResult *output) noexcept;

// Pure recognition query: never creates, learns, or refreshes an appearance.
BridgeStatus skyrim_remember_evaluate_appearance(const MemoryEngine *engine, std::uint32_t reference_form_id, const AppearanceItem *items,
                                                 std::size_t item_count, std::uint32_t face_visible, std::uint32_t visual_continuity,
                                                 AppearanceResult *output) noexcept;

BridgeStatus skyrim_remember_serialize(const MemoryEngine *engine, std::uint8_t *output, std::size_t capacity, std::size_t *written) noexcept;

using FormIdResolver = bool (*)(void *context, std::uint32_t old_form_id, std::uint32_t *new_form_id) noexcept;

BridgeStatus skyrim_remember_deserialize(const MemoryEngine *engine, const std::uint8_t *data, std::size_t length, FormIdResolver resolver,
                                         void *resolver_context, PersistenceStats *output) noexcept;

BridgeStatus skyrim_remember_clear(const MemoryEngine *engine) noexcept;

// Adds one JSON rule file. InvalidData rejects the whole file and, when an error
// buffer is given, writes a NUL-terminated (possibly truncated) reason.
BridgeStatus skyrim_remember_load_rules(const MemoryEngine *engine, const std::uint8_t *data, std::size_t length, std::uint8_t *error,
                                        std::size_t error_capacity) noexcept;

BridgeStatus skyrim_remember_rules_summary(const MemoryEngine *engine, RulesSummary *output) noexcept;

// NotFound past the last reference. BufferTooSmall still fills output, so the
// caller can size the name buffer from nameLength and ask again.
BridgeStatus skyrim_remember_rule_reference(const MemoryEngine *engine, std::uint32_t index, RuleReferenceInfo *output, std::uint8_t *name,
                                            std::size_t name_capacity) noexcept;

// Records the runtime FormID a reference resolved to; zero marks it unresolved.
BridgeStatus skyrim_remember_bind_rule_reference(const MemoryEngine *engine, std::uint32_t index, std::uint32_t runtime_form_id) noexcept;

// Raises one NPC's axes to what the worn items and keywords imply for its
// profile. Never lowers anything. Call only for an NPC that recognized the
// Player: the outfit of a stranger it could not identify is not held against
// the Player.
BridgeStatus skyrim_remember_observe_impression(const MemoryEngine *engine, std::uint32_t reference_form_id, double game_days, std::uint32_t profile,
                                                const AppearanceItem *items, std::size_t item_count, const std::uint32_t *keywords, std::size_t keyword_count,
                                                ImpressionResult *output) noexcept;
}
