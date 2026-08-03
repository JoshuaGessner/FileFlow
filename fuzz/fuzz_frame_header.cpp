// Fuzz the frame header parser — the first thing an attacker's photons reach.
#include <fileflow/frame.h>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto r = fileflow::FrameHeader::DecodePlain(std::span(data, size));
    if (r.ok()) {
        // If the parser accepted it, every documented bound must actually hold. A field
        // that escapes its bound here would be used to size buffers downstream.
        const auto& h = r.value();
        if (h.payload_bytes > fileflow::FrameHeader::kMaxPayloadBytes) __builtin_trap();
        if (h.version > fileflow::kProtocolVersion) __builtin_trap();
        if (static_cast<std::uint8_t>(h.profile) >
            static_cast<std::uint8_t>(fileflow::ModulationProfile::kM4MixedFrame)) {
            __builtin_trap();
        }
        // Re-encoding an accepted header must reproduce it exactly.
        auto again = fileflow::FrameHeader::DecodePlain(h.EncodePlain());
        if (!again.ok()) __builtin_trap();
        if (again.value().sequence != h.sequence) __builtin_trap();
        if (again.value().payload_crc != h.payload_crc) __builtin_trap();
    }
    return 0;
}
