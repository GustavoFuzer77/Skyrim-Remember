#include "Runtime.h"
#include "RustBridge.h"

#include <SKSE/API.h>
#include <SKSE/Logger.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace {

bool InitializeLogging() {
  try {
    auto logDirectory = SKSE::log::log_directory();

    if (!logDirectory) {
      return false;
    }

    *logDirectory /= "SkyrimRemember.log";

    auto logger = spdlog::basic_logger_mt("SkyrimRemember", logDirectory->string(), true);

    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);

    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    return true;
  } catch (const spdlog::spdlog_ex &) {
    return false;
  } catch (...) {
    return false;
  }
}

void RustLogCallback(const char *message) noexcept {
  try {
    SKSE::log::info("[Rust] {}", message != nullptr ? message : "<null>");
  } catch (...) {
    OutputDebugString("Skyrim Remember: Rust log callback failed\n");
  }
}

} // namespace

// SKSEPluginLoad expects a SKSE::LoadInterface entry point to load the plugin.
SKSEPluginLoad(const SKSE::LoadInterface *skse) {
  try {

    if (!InitializeLogging()) {
      return false;
    }

    SKSE::Init(skse);
    SKSE::log::info("Skyrim Remember SKSE plugin loaded");

    const auto actualAbiVersion = skyrim_remember_abi_version();

    if (actualAbiVersion != SkyrimRememberAbiVersion) {
      SKSE::log::critical("Rust bridge ABI mismatch: expected {}, received {}", SkyrimRememberAbiVersion, actualAbiVersion);
      return false;
    }

    if (!skyrim_remember_initialize(&RustLogCallback)) {
      SKSE::log::critical("Rust bridge initialization failed");
      return false;
    }

    SKSE::log::info("C++ -> Rust bridge initialized successfully");
    // Register callbacks last: after registration succeeds the plugin must stay
    // loaded.
    return SkyrimRemember::RegisterRuntime();
  } catch (...) {
    OutputDebugString("Skyrim Remember: plugin initialization failed\n");
    return false;
  }
}
