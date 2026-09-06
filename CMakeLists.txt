cmake_minimum_required(VERSION 3.25)

project(
    SkyrimRemember
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CommonLibSSE CONFIG REQUIRED)

# CommonLibSSE NG 3.7.0 in the colorglass registry ships source that MSVC
# accepts until template instantiation while clang-cl correctly rejects it.
# The maintained upstream has already fixed each of these. Patch only the
# build-local vcpkg copy, and fail naturally if any unrelated error remains.
#
# Each call rewrites one exact string in one header, and is a no-op once the
# header no longer contains it, so re-running CMake and upgrading the port are
# both safe. Add a call here rather than another copy of this block.
find_path(COMMONLIBSSE_INCLUDE_DIR "RE/B/BSTPointerAndFlags.h" REQUIRED)

function(patch_commonlibsse_header RELATIVE_HEADER FROM TO DESCRIPTION)
    set(HEADER "${COMMONLIBSSE_INCLUDE_DIR}/${RELATIVE_HEADER}")
    file(READ "${HEADER}" CONTENT)
    string(FIND "${CONTENT}" "${FROM}" TYPO_POSITION)
    if(NOT TYPO_POSITION EQUAL -1)
        string(REPLACE "${FROM}" "${TO}" CONTENT "${CONTENT}")
        file(WRITE "${HEADER}" "${CONTENT}")
        message(STATUS "Applied the CommonLibSSE NG ${DESCRIPTION} clang-cl fix")
    endif()
endfunction()

# A one-character typo: the member is named _storage.
patch_commonlibsse_header("RE/B/BSTPointerAndFlags.h"
    "a_rhs.storage.address = 0;"
    "a_rhs._storage.address = 0;"
    "BSTPointerAndFlags")

# Stale Scaleform container method casing, hit when UI event headers are
# instantiated.
patch_commonlibsse_header("RE/G/GHashSetBase.h"
    "a_src.Begin(); it != a_src.End();"
    "a_src.begin(); it != a_src.end();"
    "Scaleform")

set(RUST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
set(RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
set(RUST_LIBRARY "${RUST_TARGET_DIR}/release/skyrim_remember_core.lib")
file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/src/*.rs")

add_custom_command(
    OUTPUT "${RUST_LIBRARY}"
    COMMAND cargo build
        --manifest-path "${RUST_MANIFEST}"
        --target-dir "${RUST_TARGET_DIR}"
        --release
        --locked
    DEPENDS
        "${RUST_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
        ${RUST_SOURCES}
    COMMENT "Building the Rust memory core"
    VERBATIM
)

add_custom_target(skyrim_remember_rust_build DEPENDS "${RUST_LIBRARY}")

add_library(skyrim_remember_rust STATIC IMPORTED GLOBAL)
set_target_properties(skyrim_remember_rust PROPERTIES
    IMPORTED_LOCATION "${RUST_LIBRARY}"
)
add_dependencies(skyrim_remember_rust skyrim_remember_rust_build)

# Transitive native dependencies of the Rust standard library on Windows MSVC.
set_property(TARGET skyrim_remember_rust PROPERTY INTERFACE_LINK_LIBRARIES
    "ntdll;userenv;ws2_32;dbghelp")

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "Skyrim Remember"
    AUTHOR "fuzer"
    VERSION ${PROJECT_VERSION}
    SOURCES
        cpp/include/Appearance.h
        cpp/include/Defines.h
        cpp/include/EquipmentBatch.h
        cpp/include/GuardProbe.h
        cpp/include/Persistence.h
        cpp/include/PlayerAppearance.h
        cpp/include/RustBridge.h
        cpp/src/Appearance.cpp
        cpp/src/EquipmentBatch.cpp
        cpp/src/GuardProbe.cpp
        cpp/src/Persistence.cpp
        cpp/src/Plugin.cpp
        cpp/src/PlayerAppearance.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE cpp/include)
target_link_libraries(${PROJECT_NAME} PRIVATE skyrim_remember_rust)
target_precompile_headers(${PROJECT_NAME} PRIVATE cpp/include/PCH.h)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /EHsc)
endif()

add_custom_target(rust_tests
    COMMAND cargo test --manifest-path "${RUST_MANIFEST}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Running Rust unit tests"
    VERBATIM
)

include(CTest)
if(BUILD_TESTING)
    # Appearance.cpp is deliberately free of Skyrim dependencies so that the
    # outfit merge and the face rule can be tested without the game.
    add_executable(BridgeSmoke cpp/tests/BridgeSmoke.cpp cpp/src/Appearance.cpp)
    target_include_directories(BridgeSmoke PRIVATE cpp/include)
    target_link_libraries(BridgeSmoke PRIVATE skyrim_remember_rust)
    add_test(NAME bridge_smoke COMMAND BridgeSmoke)
endif()
