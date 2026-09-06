#include "RustBridge.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

bool logged = false;

void Log(const char *message) noexcept {
  logged = message && std::strcmp(message, "Rust bridge initialized") == 0;
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

  CHECK(skyrim_remember_abi_version() == 2);

  CHECK(skyrim_remember_initialize(Log));

  CHECK(logged);

  using Context =
      std::unique_ptr<MemoryEngine, decltype(&skyrim_remember_destroy)>;

  Context engine(skyrim_remember_create(), skyrim_remember_destroy);

  CHECK(engine);

  MemorySnapshot snapshot{1.0f, 2.0f, 3.0f, 4.0f};

  CHECK(skyrim_remember_get(engine.get(), 123, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(snapshot.recognition == 1.0f && snapshot.respect == 4.0f);

  CHECK(skyrim_remember_register(engine.get(), 0) == BridgeStatus::InvalidId);
  CHECK(skyrim_remember_register(engine.get(), 123) == BridgeStatus::Created);
  CHECK(skyrim_remember_register(engine.get(), 123) == BridgeStatus::Ok);
  CHECK(skyrim_remember_get(engine.get(), 123, &snapshot) == BridgeStatus::Ok);

  CHECK(snapshot.recognition == 0.0f && snapshot.suspicion == 0.0f &&
        snapshot.fear == 0.0f && snapshot.respect == 0.0f);

  CHECK(skyrim_remember_get(engine.get(), 456, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_get(engine.get(), 123, nullptr) ==
        BridgeStatus::InvalidArgument);

  CHECK(skyrim_remember_clear(engine.get()) == BridgeStatus::Ok);

  CHECK(skyrim_remember_get(engine.get(), 123, &snapshot) ==
        BridgeStatus::NotFound);

  CHECK(skyrim_remember_register(engine.get(), 123) == BridgeStatus::Created);

  std::puts("C++/Rust bridge smoke test passed");
}
