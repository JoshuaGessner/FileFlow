// Fuzz the Reed-Solomon decoder directly with arbitrary codewords.
//
// The decoder runs iterative algorithms (Berlekamp-Massey, Chien search, Forney) over
// attacker-influenced data. It must terminate, stay in bounds, and -- critically -- never
// claim success while producing wrong output (THREAT-MODEL T5, miscorrection).
#include <fileflow/gf256.h>

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) return 0;

    // First byte selects the parity count, so the fuzzer explores several code shapes.
    const std::size_t nsym = 2 + (data[0] % 32);
    std::vector<std::uint8_t> cw(data + 1, data + size);
    if (cw.size() < nsym + 1 || cw.size() > 255) return 0;

    const fileflow::ReedSolomon rs(nsym);
    const std::vector<std::uint8_t> before = cw;

    auto r = rs.Decode(cw);
    if (r.ok()) {
        // A "successful" decode must yield a codeword with all-zero syndromes. Decode()
        // re-verifies internally; re-encoding the corrected message must reproduce it.
        const std::size_t k = cw.size() - nsym;
        std::vector<std::uint8_t> msg(cw.begin(), cw.begin() + static_cast<std::ptrdiff_t>(k));
        auto re = rs.Encode(msg);
        if (!re.ok()) __builtin_trap();
        if (re.value() != cw) __builtin_trap();

        // It must not have claimed more corrections than the code can make.
        if (r.value() > rs.correctable()) __builtin_trap();
    }
    (void)before;
    return 0;
}
