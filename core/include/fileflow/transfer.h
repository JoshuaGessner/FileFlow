// Session and file layers: manifest, block partitioning, reassembly, hash verification.
//
// THE GUARANTEE (G3): a completed transfer means the receiver holds a file whose SHA-256
// matches the sender's. Partial, corrupt or silently truncated output is a defect of the
// highest severity, and a hash mismatch delivers NOTHING (docs/security/THREAT-MODEL.md T5).
#pragma once

#include <fileflow/fountain.h>
#include <fileflow/frame.h>
#include <fileflow/hash.h>
#include <fileflow/result.h>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace fileflow {

struct FileManifest {
    std::uint64_t file_size = 0;
    Sha256::Digest sha256{};
    std::string file_name;      // a DISPLAY HINT, never a path (INPUT-VALIDATION)
    std::uint32_t block_count = 0;
    std::uint32_t block_symbols = 0;   // k per block
    std::uint32_t symbol_size = 0;
    std::uint32_t session_id = 0;

    // Hard bounds, all enforced before any allocation derived from them.
    static constexpr std::uint64_t kMaxFileSize = 4ULL * 1024 * 1024 * 1024;
    static constexpr std::uint32_t kMaxBlockCount = 1u << 24;
    static constexpr std::size_t kMaxNameLength = 255;

    [[nodiscard]] Status Validate() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> Encode() const;
    [[nodiscard]] static Result<FileManifest> Decode(std::span<const std::uint8_t> in);
};

// Strips a received name down to something safe to use. Sanitises rather than merely
// validating: rejecting is fine too, but we must never pass an attacker's string through
// to the filesystem (THREAT-MODEL T3).
[[nodiscard]] std::string SanitiseFileName(std::string_view raw);

// ---------------------------------------------------------------- transmitter

class FileTransmitter {
  public:
    // `payload` is the original file bytes. Block size is chosen so the fountain decoder's
    // working set stays bounded regardless of file size -- a 100 MB transfer must not need
    // 100 MB of decoder memory.
    static Result<FileTransmitter> Create(std::span<const std::uint8_t> payload,
                                          std::string file_name, std::uint32_t session_id,
                                          std::uint32_t symbol_size = 512,
                                          std::uint32_t block_symbols = 64);

    [[nodiscard]] const FileManifest& manifest() const noexcept { return manifest_; }

    // Produces the next frame payload in the transmission schedule, round-robining over
    // blocks so a receiver joining late makes progress on every block rather than waiting
    // for the schedule to come around.
    struct FramePayload {
        FrameHeader header;
        std::vector<std::uint8_t> data;
    };
    [[nodiscard]] FramePayload NextFrame();

    [[nodiscard]] std::uint64_t frames_generated() const noexcept { return counter_; }

  private:
    FileTransmitter(FileManifest m, std::vector<FountainEncoder> enc)
        : manifest_(std::move(m)), encoders_(std::move(enc)) {}

    FileManifest manifest_;
    std::vector<FountainEncoder> encoders_;
    std::uint64_t counter_ = 0;
};

// ---------------------------------------------------------------- receiver

class FileReceiver {
  public:
    static Result<FileReceiver> Create(const FileManifest& m);

    // Feed one successfully decoded frame. Duplicates and out-of-order arrivals are
    // tolerated by construction. Returns true once every block is recovered.
    bool Ingest(const FrameHeader& h, std::span<const std::uint8_t> data);

    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::uint32_t blocks_complete() const noexcept;
    [[nodiscard]] double progress() const noexcept;

    // Total symbols ingested across all blocks, and the resulting reception overhead --
    // the Rfountain term the goodput model needs and EXP-012 measures.
    [[nodiscard]] std::uint64_t symbols_ingested() const noexcept { return ingested_; }
    [[nodiscard]] double overhead() const noexcept;

    // Reassembles and VERIFIES. A hash mismatch is a hard failure that returns
    // kHashMismatch and yields no data -- never a partial or unverified result.
    [[nodiscard]] Result<std::vector<std::uint8_t>> Finish();

  private:
    explicit FileReceiver(const FileManifest& m) : manifest_(m) {}

    FileManifest manifest_;
    std::vector<FountainDecoder> decoders_;
    std::uint64_t ingested_ = 0;
};

}  // namespace fileflow
