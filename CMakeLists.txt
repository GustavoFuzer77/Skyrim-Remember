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

find_package(nlohmann_json CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "Skyrim Remember"
    AUTHOR "fuzer"
    VERSION ${PROJECT_VERSION}
    SOURCES
        cpp/include/Appearance.h
        cpp/include/AppearanceMemoryService.h
        cpp/include/EquipmentBatch.h
        cpp/include/GuardNotices.h
        cpp/include/GuardReactionAdapter.h
        cpp/include/ImpressionRules.h
        cpp/include/NpcObservation.h
        cpp/include/NpcObservationService.h
        cpp/include/NpcProfile.h
        cpp/include/ObservationTuning.h
        cpp/include/Memory.h
        cpp/include/MemoryTuning.h
        cpp/include/Persistence.h
        cpp/include/PlayerAppearance.h
        cpp/include/ProfileReactionRouter.h
        cpp/include/Runtime.h
        cpp/include/RuleBook.h
        cpp/src/Appearance.cpp
        cpp/src/AppearanceMemoryService.cpp
        cpp/src/EquipmentBatch.cpp
        cpp/src/GuardNotices.cpp
        cpp/src/GuardReactionAdapter.cpp
        cpp/src/ImpressionRules.cpp
        cpp/src/Memory.cpp
        cpp/src/MemorySerialization.cpp
        cpp/src/NpcClassification.cpp
        cpp/src/NpcObservation.cpp
        cpp/src/NpcObservationService.cpp
        cpp/src/NpcProfile.cpp
        cpp/src/Persistence.cpp
        cpp/src/Plugin.cpp
        cpp/src/PlayerAppearance.cpp
        cpp/src/ProfileReactionRouter.cpp
        cpp/src/RuleBook.cpp
        cpp/src/Runtime.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE cpp/include)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_precompile_headers(${PROJECT_NAME} PRIVATE cpp/include/PCH.h)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /EHsc)
endif()

include(CTest)
if(BUILD_TESTING)
    # These sources are deliberately free of Skyrim dependencies so that the
    # memory model, the outfit merge, the face rule and the profile policy can
    # be tested without the game installed.
    add_executable(MemoryTests
        cpp/tests/MemoryTests.cpp
        cpp/src/Appearance.cpp
        cpp/src/GuardNotices.cpp
        cpp/src/Memory.cpp
        cpp/src/MemorySerialization.cpp
        cpp/src/NpcProfile.cpp
        cpp/src/RuleBook.cpp)
    target_include_directories(MemoryTests PRIVATE cpp/include)
    target_link_libraries(MemoryTests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(MemoryTests PRIVATE
        SHIPPED_RULES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/data/SKSE/Plugins/SkyrimRemember/SkyrimRemember_Rules.json")
    if(MSVC)
        target_compile_options(MemoryTests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME memory_tests COMMAND MemoryTests)
endif()
