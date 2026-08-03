# FileFlow — desktop entry point.
#
# ADR-0010 requires the decode chain to build and run OFF-DEVICE so the simulator and
# replay harness exercise the production decoder. This file is the desktop half of that:
# it builds core/ with no Android toolchain present. If this build ever needs an Android
# header, the architecture has been violated -- see docs/adr/ADR-0010.
#
# The Android build consumes core/CMakeLists.txt via Gradle externalNativeBuild.
cmake_minimum_required(VERSION 3.22)
project(fileflow LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)          # ADR-0013: C++20, NOT C++23 (unreliable in NDK libc++)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FILEFLOW_BUILD_TESTS "Build unit tests" ON)
option(FILEFLOW_BUILD_SIM   "Build the offline channel simulator" ON)
option(FILEFLOW_BUILD_FUZZ  "Build libFuzzer targets (needs a toolchain shipping libFuzzer)" OFF)
option(FILEFLOW_WERROR      "Treat warnings as errors" ON)

add_library(fileflow_warnings INTERFACE)
target_compile_options(fileflow_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wcast-qual -Wold-style-cast -Wdouble-promotion
    -Wnon-virtual-dtor -Woverloaded-virtual

    # Suppressed deliberately, not out of convenience.
    #
    # -Wextra enables this, and under GCC it fires on every C++20 DESIGNATED initializer that
    # omits a member -- `{.pilot_pitch = 8, .marker_size = 4}` and friends, which this codebase
    # uses throughout. Omitting a member is the entire point of the feature: the omitted members
    # take their default member initializers, which every config struct here defines. So the
    # warning cannot indicate an uninitialised read in C++ (aggregate initialisation
    # value-initialises anything not named, NSDMI or not) and naming every field at every call
    # site would be verbose, brittle, and would silently stop tracking the defaults.
    #
    # Clang does not warn here, which is why this only surfaced when CI first ran on Linux
    # (finding F24). Keeping -Werror and suppressing the one false positive is better than
    # dropping -Werror.
    -Wno-missing-field-initializers

    $<$<BOOL:${FILEFLOW_WERROR}>:-Werror>
)

add_subdirectory(core)

# The replay harness gates on the same option as the simulator: both are host-side tools built
# on core/, and neither is part of the shipped decoder. Declared BEFORE sim/ because ffsim
# links it for --record.
if(FILEFLOW_BUILD_SIM)
    add_subdirectory(harness)
    add_subdirectory(sim)
endif()

if(FILEFLOW_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(core/tests)
endif()

if(FILEFLOW_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif()
