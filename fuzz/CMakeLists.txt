# libFuzzer targets for the untrusted-input surface.
#
# NOTE: Apple's clang does NOT ship libclang_rt.fuzzer, so these do not build on macOS with
# the system toolchain. They are opt-in (FILEFLOW_BUILD_FUZZ, default OFF) and run on Linux
# in CI. Use Homebrew LLVM locally if you want to fuzz on a Mac.
#
# The project runs plain C++20 with no memory-safety hedge, so fuzzing plus sanitisers
# carry the full weight on this surface (docs/security/THREAT-MODEL.md).

function(fileflow_fuzz_target name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE fileflow_core)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    set_target_properties(${name} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endfunction()

# Priority order matches docs/architecture/ENGINEERING-PRACTICES.md section 5.
fileflow_fuzz_target(fuzz_frame_header   fuzz_frame_header.cpp)
fileflow_fuzz_target(fuzz_manifest       fuzz_manifest.cpp)
fileflow_fuzz_target(fuzz_header_codec   fuzz_header_codec.cpp)
fileflow_fuzz_target(fuzz_reed_solomon   fuzz_reed_solomon.cpp)
# The camera is attacker-controlled and the detector runs before any checksum exists, so this
# is the receiver's true front door (THREAT-MODEL T1).
fileflow_fuzz_target(fuzz_screen_detect  fuzz_screen_detect.cpp)
