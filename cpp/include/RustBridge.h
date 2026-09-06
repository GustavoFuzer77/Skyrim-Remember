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

inline constexpr std::uint32_t SkyrimRememberAbiVersion = 7;

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

struct MemorySnapshot {
  float recognition;
  float suspicion;
  float fear;
  float respect;
  double lastDecayGameDays;
};

struct MemoryDelta {
  float recognition;
  float suspicion;
  float fear;
  float respect;
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

static_assert(std::is_standard_layout_v<MemorySnapshot>);
static_assert(sizeof(MemorySnapshot) == 24 && alignof(MemorySnapshot) == 8);
static_assert(offsetof(MemorySnapshot, recognition) == 0);
static_assert(offsetof(MemorySnapshot, suspicion) == 4);
static_assert(offsetof(MemorySnapshot, fear) == 8);
static_assert(offsetof(MemorySnapshot, respect) == 12);
static_assert(offsetof(MemorySnapshot, lastDecayGameDays) == 16);
static_assert(sizeof(MemoryDelta) == 16 && alignof(MemoryDelta) == 4);
static_assert(sizeof(PersistenceStats) == 8);
static_assert(sizeof(AppearanceItem) == 8 && alignof(AppearanceItem) == 4);
static_assert(sizeof(AppearanceResult) == 40 && alignof(AppearanceResult) == 4);
static_assert(offsetof(AppearanceResult, outfitSimilarity) == 0);
static_assert(offsetof(AppearanceResult, appearanceMatched) == 12);
static_assert(offsetof(AppearanceResult, recognized) == 16);
static_assert(offsetof(AppearanceResult, linkedByContinuity) == 20);
static_assert(offsetof(AppearanceResult, knownAppearances) == 36);
static_assert(sizeof(BridgeStatus) == 4);

extern "C" {
// Callbacks must not throw or retain the message pointer.
using SkyrimRememberLogCallback = void (*)(const char *message) noexcept;

// Returns true after the Rust core has initialized successfully.
bool skyrim_remember_initialize(
    SkyrimRememberLogCallback log_callback) noexcept;

// ABI version allows both sides to reject incompatible future bridges.
std::uint32_t skyrim_remember_abi_version() noexcept;

// Contexts are owned by Rust. Never delete them from C++.
MemoryEngine *skyrim_remember_create() noexcept;

// No other call may be in flight when destroying the context.
void skyrim_remember_destroy(MemoryEngine *engine) noexcept;

BridgeStatus skyrim_remember_register(const MemoryEngine *engine,
                                      std::uint32_t reference_form_id,
                                      double game_days) noexcept;

// On errors output remains unchanged; querying does not create a memory.
BridgeStatus skyrim_remember_get(const MemoryEngine *engine,
                                 std::uint32_t reference_form_id,
                                 double game_days,
                                 MemorySnapshot *output) noexcept;

BridgeStatus skyrim_remember_apply_delta(const MemoryEngine *engine,
                                         std::uint32_t reference_form_id,
                                         double game_days, MemoryDelta delta,
                                         MemorySnapshot *output) noexcept;

BridgeStatus skyrim_remember_observe_appearance(
    const MemoryEngine *engine, std::uint32_t reference_form_id,
    double game_days, const AppearanceItem *items, std::size_t item_count,
    std::uint32_t face_visible, std::uint32_t visual_continuity,
    AppearanceResult *output) noexcept;

// Pure recognition query: never creates, learns, or refreshes an appearance.
BridgeStatus skyrim_remember_evaluate_appearance(
    const MemoryEngine *engine, std::uint32_t reference_form_id,
    const AppearanceItem *items, std::size_t item_count,
    std::uint32_t face_visible, std::uint32_t visual_continuity,
    AppearanceResult *output) noexcept;

BridgeStatus skyrim_remember_serialize(const MemoryEngine *engine,
                                       std::uint8_t *output,
                                       std::size_t capacity,
                                       std::size_t *written) noexcept;

using FormIdResolver = bool (*)(void *context, std::uint32_t old_form_id,
                                std::uint32_t *new_form_id) noexcept;

BridgeStatus skyrim_remember_deserialize(const MemoryEngine *engine,
                                         const std::uint8_t *data,
                                         std::size_t length,
                                         FormIdResolver resolver,
                                         void *resolver_context,
                                         PersistenceStats *output) noexcept;

BridgeStatus skyrim_remember_clear(const MemoryEngine *engine) noexcept;
}
