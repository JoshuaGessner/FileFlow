#include <fileflow/hash.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace fileflow;

namespace {
std::span<const std::uint8_t> Bytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}
}  // namespace

// Known-answer tests against the FIPS 180-4 examples. SHA-256 is the integrity anchor for
// the whole system (G3) -- if it is wrong, every "verified" transfer is a lie.
TEST(Sha256, EmptyInput) {
    EXPECT_EQ(ToHex(Sha256::Of({})),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
    EXPECT_EQ(ToHex(Sha256::Of(Bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, TwoBlockMessage) {
    const std::string in = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(ToHex(Sha256::Of(Bytes(in))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionA) {
    std::vector<std::uint8_t> in(1000000, 'a');
    EXPECT_EQ(ToHex(Sha256::Of(in)),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, IncrementalMatchesOneShot) {
    std::vector<std::uint8_t> in(5000);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<std::uint8_t>(i * 7 + 3);

    const auto one_shot = Sha256::Of(in);

    // Feed in awkward chunk sizes to exercise the buffering path.
    Sha256 h;
    std::size_t off = 0;
    for (std::size_t chunk : {1u, 63u, 64u, 65u, 127u, 1u, 1000u}) {
        const std::size_t n = std::min(chunk, in.size() - off);
        h.Update(std::span<const std::uint8_t>(in.data() + off, n));
        off += n;
    }
    h.Update(std::span<const std::uint8_t>(in.data() + off, in.size() - off));
    EXPECT_EQ(h.Finish(), one_shot);
}

TEST(Sha256, SingleBitFlipChangesDigest) {
    std::vector<std::uint8_t> a(256, 0x5A);
    std::vector<std::uint8_t> b = a;
    b[128] ^= 0x01;
    EXPECT_NE(Sha256::Of(a), Sha256::Of(b));
}

TEST(Crc32, KnownAnswers) {
    EXPECT_EQ(Crc32(Bytes("")), 0x00000000U);
    EXPECT_EQ(Crc32(Bytes("123456789")), 0xCBF43926U);
    EXPECT_EQ(Crc32(Bytes("The quick brown fox jumps over the lazy dog")), 0x414FA339U);
}

TEST(Crc32, DetectsSingleBitFlip) {
    std::vector<std::uint8_t> a(64, 0x11);
    std::vector<std::uint8_t> b = a;
    b[31] ^= 0x08;
    EXPECT_NE(Crc32(a), Crc32(b));
}
