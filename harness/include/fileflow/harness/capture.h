// Recorded-frame capture bundles and replay (component C17).
//
// THE POINT: a replayed capture must go through the identical decode chain the live receiver
// uses (FramePipeline), so that a bug reproducing in replay is a real bug and a change that
// improves replayed goodput is a real improvement. Anything less and recorded datasets are
// decoration.
//
// This is the piece that unblocks EXP-001. The grid sweep established that the density cliff
// cannot be found in simulation, because the renderer models neither display subpixel
// structure nor sensor MTF nor moire (findings F14 retracted, F16). Real captures replayed
// deterministically are the only way to measure it.
//
// FORMAT — deliberately boring:
//
//   <bundle>/capture.meta        key: value text, one per line
//   <bundle>/frames/000000.gray  raw 8-bit luminance, width*height bytes, no header
//   <bundle>/frames/000001.gray
//
// Raw 8-bit greyscale because that is EXACTLY what the camera hands us: the Y plane of
// YUV_420_888 is a full-resolution 8-bit luminance image (docs/research/android-camera-pipeline.md).
// No decoder, no third-party dependency, no lossy compression to destroy the high-spatial-
// frequency cell structure we are trying to measure. Text metadata because it diffs cleanly in
// git and can be corrected by hand when a rig measurement was mis-typed.
#pragma once

#include <fileflow/capture_source.h>
#include <fileflow/image.h>
#include <fileflow/pipeline.h>
#include <fileflow/result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fileflow::harness {

// Everything docs/testing/CAPTURE-HARNESS.md requires alongside a capture.
//
// Unset fields are empty or zero, and that is VISIBLE rather than defaulted to something
// plausible: a capture whose distance was never recorded must not silently claim 30 cm.
struct CaptureMetadata {
    // --- Provenance ---
    std::string sender_model;
    std::string receiver_model;
    std::string os_build;
    std::string app_commit;
    std::string notes;

    // --- Transmitter ---
    std::string display_mode;      // e.g. "1080x2400@120"
    std::uint32_t grid_cols = 0;
    std::uint32_t grid_rows = 0;
    std::string modulation_profile;  // e.g. "M0"
    double screen_brightness = -1.0;  // 0..1; negative = not recorded

    // --- Receiver camera ---
    std::string camera_id;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double fps = 0.0;
    double exposure_ns = -1.0;
    double iso = -1.0;
    double focus_distance = -1.0;
    std::string white_balance;

    // --- Rig ---
    double distance_cm = -1.0;
    double angle_deg = -1.0;
    double ambient_lux = -1.0;
    std::string motion_condition;  // "rigid" | "handheld" | ...

    // --- Expected result ---
    std::string source_payload_sha256;
    std::uint64_t source_payload_bytes = 0;
    std::uint32_t frame_count = 0;

    [[nodiscard]] Status Validate() const noexcept;

    // Round-trips through the text format. Serialise emits every field, including unset ones,
    // so a bundle always shows what was NOT recorded.
    [[nodiscard]] std::string Serialise() const;
    [[nodiscard]] static Result<CaptureMetadata> Parse(std::string_view text);

    // Fields that must be filled before a capture is usable as evidence. Returns the missing
    // ones by name rather than a bare bool -- an incomplete capture should say what it needs.
    [[nodiscard]] std::vector<std::string> MissingRequiredFields() const;
};

// Writes a bundle. Used by the simulator to produce synthetic datasets, and by the Android
// recorder later; both must produce byte-identical layouts so replay cannot tell them apart.
class CaptureWriter {
  public:
    static Result<CaptureWriter> Create(const std::string& bundle_dir, CaptureMetadata meta);

    // Appends one frame. `img` must match the metadata's width/height.
    [[nodiscard]] Status WriteFrame(const ImageView8& img);

    // Flushes metadata with the final frame count. A bundle is not valid until this runs.
    [[nodiscard]] Status Finish();

    [[nodiscard]] std::uint32_t frames_written() const noexcept { return frames_; }

  private:
    CaptureWriter(std::string dir, CaptureMetadata meta)
        : dir_(std::move(dir)), meta_(std::move(meta)) {}

    std::string dir_;
    CaptureMetadata meta_;
    std::uint32_t frames_ = 0;
    bool finished_ = false;
};

// Replays a bundle through the real decode chain.
//
// Implements CaptureSource, so every consumer that works against the simulator works against
// recorded frames unchanged -- which is the whole reason CaptureSource exists.
class ReplaySource final : public CaptureSource {
  public:
    static Result<ReplaySource> Create(const std::string& bundle_dir, const FrameLayout& layout,
                                       PipelineConfig cfg = {});

    [[nodiscard]] GridGeometry geometry() const override { return layout_->geometry(); }
    [[nodiscard]] std::optional<CapturedFrame> Next() override;
    [[nodiscard]] std::uint64_t frames_emitted() const override {
        return pipeline_.diagnostics().frames_decoded;
    }
    [[nodiscard]] std::uint64_t frames_dropped() const override {
        return pipeline_.diagnostics().failures();
    }

    [[nodiscard]] const CaptureMetadata& metadata() const noexcept { return meta_; }
    [[nodiscard]] const FramePipeline& pipeline() const noexcept { return pipeline_; }

    // Rewind so the same bundle can be replayed under different configurations -- the A/B
    // that makes recorded datasets worth keeping.
    void Rewind() noexcept;

  private:
    ReplaySource(std::string dir, CaptureMetadata meta, const FrameLayout& layout,
                 FramePipeline pipeline);

    std::string dir_;
    CaptureMetadata meta_;
    const FrameLayout* layout_;
    FramePipeline pipeline_;
    std::vector<std::uint8_t> buffer_;  // reused across frames; no per-frame allocation
    std::uint32_t next_ = 0;
};

// Reads a raw .gray frame into `out`, sized width*height. Exposed for tools and tests.
[[nodiscard]] Status ReadGrayFrame(const std::string& path, std::uint32_t width,
                                   std::uint32_t height, std::vector<std::uint8_t>* out);

}  // namespace fileflow::harness
