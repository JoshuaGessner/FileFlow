// Fuzz the session manifest parser. This one drives ALLOCATION SIZES (block counts,
// symbol sizes, file length), so a bound that leaks here is a heap-corruption primitive.
#include <fileflow/transfer.h>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto r = fileflow::FileManifest::Decode(std::span(data, size));
    if (!r.ok()) return 0;

    const auto& m = r.value();

    // Accepted manifests must satisfy every bound BEFORE anything is sized from them.
    if (m.file_size > fileflow::FileManifest::kMaxFileSize) __builtin_trap();
    if (m.block_count == 0 || m.block_count > fileflow::FileManifest::kMaxBlockCount) {
        __builtin_trap();
    }
    if (m.symbol_size == 0 || m.symbol_size > fileflow::FountainParams::kMaxSymbolSize) {
        __builtin_trap();
    }
    if (m.file_name.size() > fileflow::FileManifest::kMaxNameLength) __builtin_trap();

    // The sanitised name must never be usable as a path.
    if (m.file_name.find('/') != std::string::npos) __builtin_trap();
    if (m.file_name.find('\\') != std::string::npos) __builtin_trap();
    if (m.file_name == "." || m.file_name == "..") __builtin_trap();

    // Constructing a receiver from an accepted manifest must not blow up: this is the
    // allocation path a hostile transmitter is actually aiming at (THREAT-MODEL T2).
    // Skipped for very large declared blocks so the fuzzer is not just testing malloc.
    if (m.block_count <= 64 && m.block_symbols <= 1024 && m.symbol_size <= 4096) {
        auto rx = fileflow::FileReceiver::Create(m);
        (void)rx;
    }
    return 0;
}
