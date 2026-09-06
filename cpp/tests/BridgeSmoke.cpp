#include "RustBridge.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

bool logged = false;

void Log(const char *message) noexcept {
  logged = message && std::strcmp(message, "Rust bridge initialized") == 0;
}

bool ResolveForm(void *, std::uint32_t oldFormID,
                 std::uint32_t *newFormID) noexcept {
  if (!newFormID) {
    return false;
  }
  *newFormID = oldFormID;
  return true;
}

} // namespace

// Check real C++ -> Rust calls in Release too (assert would be disabled).
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition);   \
      return 1;                                                                \
    }                                                                          \
  } while (false)

int main() {

  CHECK(skyrim_remember_abi_version() == 3);

  CHECK(skyrim_remember_initialize(Log));

  CHECK(logged);

  using Context =
      std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>;

  Context engine(skyrim_remember_create(), skyrim_remember_destroy);

  CHECK(engine);

  MemorySnapshot snapshot{1.0f, 2.0f, 3.0f, 4.0f, 5.0};

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(snapshot.recognition == 1.0f && snapshot.respect == 4.0f);

  CHECK(skyrim_remember_register(engine.get(), 0, 10.0) ==
        BridgeStatus::InvalidId);
  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) ==
        BridgeStatus::Created);

  MemoryDelta delta{0.0f, 0.0f, 30.0f, 50.0f};
  CHECK(skyrim_remember_apply_delta(engine.get(), 123, 10.0, delta,
                                    &snapshot) == BridgeStatus::Ok);
  CHECK(snapshot.fear == 30.0f && snapshot.respect == 50.0f);

  std::size_t length = 0;
  CHECK(skyrim_remember_serialize(engine.get(), nullptr, 0, &length) ==
        BridgeStatus::Ok);
  CHECK(length > 0);
  std::vector<std::uint8_t> bytes(length);
  CHECK(skyrim_remember_serialize(engine.get(), bytes.data(), bytes.size(),
                                  &length) == BridgeStatus::Ok);

  CHECK(skyrim_remember_clear(engine.get()) == BridgeStatus::Ok);
  PersistenceStats stats{};
  CHECK(skyrim_remember_deserialize(engine.get(), bytes.data(), bytes.size(),
                                    ResolveForm, nullptr,
                                    &stats) == BridgeStatus::Ok);
  CHECK(stats.loaded == 1 && stats.unresolved == 0);
  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::Ok);
  CHECK(snapshot.fear == 30.0f && snapshot.respect == 50.0f);
  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) == BridgeStatus::Ok);
  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::Ok);

  CHECK(snapshot.recognition == 0.0f && snapshot.suspicion == 0.0f &&
        snapshot.fear == 30.0f && snapshot.respect == 50.0f);

  CHECK(skyrim_remember_get(engine.get(), 456, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, nullptr) ==
        BridgeStatus::InvalidArgument);

  CHECK(skyrim_remember_clear(engine.get()) == BridgeStatus::Ok);

  CHECK(skyrim_remember_get(engine.get(), 123, 10.0, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_register(engine.get(), 123, 10.0) ==
        BridgeStatus::Created);

  std::puts("C++/Rust bridge smoke test passed");
}
