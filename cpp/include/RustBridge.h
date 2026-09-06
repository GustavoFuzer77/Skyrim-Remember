#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

struct MemoryEngine;

enum class BridgeStatus : std::uint32_t {
  Ok = 0,
  Created = 1,
  InvalidId = 2,
  NotFound = 3,
  InvalidArgument = 4,
  Unavailable = 5
};

struct MemorySnapshot {
  float recognition;
  float suspicion;
  float fear;
  float respect;
};

static_assert(std::is_standard_layout_v<MemorySnapshot>);
static_assert(sizeof(MemorySnapshot) == 16 && alignof(MemorySnapshot) == 4);
static_assert(offsetof(MemorySnapshot, recognition) == 0);
static_assert(offsetof(MemorySnapshot, suspicion) == 4);
static_assert(offsetof(MemorySnapshot, fear) == 8);
static_assert(offsetof(MemorySnapshot, respect) == 12);
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
                                      std::uint32_t reference_form_id) noexcept;

// On errors output remains unchanged; querying does not create a memory.
BridgeStatus skyrim_remember_get(const MemoryEngine *engine,
                                 std::uint32_t reference_form_id,
                                 MemorySnapshot *output) noexcept;

BridgeStatus skyrim_remember_clear(const MemoryEngine *engine) noexcept;
}
