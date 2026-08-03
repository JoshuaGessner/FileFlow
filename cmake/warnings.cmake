# The shared warning set, in one place.
#
# Extracted from the root CMakeLists so the Android entry point (platform/android/CMakeLists.txt)
# can add core/ and harness/ without re-declaring the flags. Two copies of this list would drift,
# and the drift would be silent: the Android build would quietly enforce less than CI does, which
# is the configuration nobody tests until it matters.
#
# ADR-0013 fixes C++20 and -Werror. Deviations belong in an ADR, not here.

if(TARGET fileflow_warnings)
    return()
endif()

option(FILEFLOW_WERROR "Treat warnings as errors" ON)

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
