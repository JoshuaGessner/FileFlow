// Fuzz the RS-protected header path end to end: correction plus parsing.
// Malformed FEC input is an explicit threat (THREAT-MODEL T4) -- a degenerate codeword must
// not send the Berlekamp-Massey or Chien search into pathological behaviour.
#include <fileflow/frame.h>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const fileflow::HeaderCodec codec(32);
    if (size != codec.coded_size()) return 0;

    std::vector<std::uint8_t> buf(data, data + size);
    auto r = codec.Decode(buf);
    if (r.ok()) {
        const auto& h = r.value();
        if (h.payload_bytes > fileflow::FrameHeader::kMaxPayloadBytes) __builtin_trap();
    }
    return 0;
}
