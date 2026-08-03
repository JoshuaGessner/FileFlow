#include <fileflow/harness/capture.h>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fileflow::harness {
namespace {

namespace fs = std::filesystem;

std::string FramePath(const std::string& dir, std::uint32_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%06u.gray", index);
    return (fs::path(dir) / "frames" / buf).string();
}

std::string MetaPath(const std::string& dir) {
    return (fs::path(dir) / "capture.meta").string();
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

double ParseDouble(std::string_view v, double fallback) {
    double out = fallback;
    const auto* first = v.data();
    const auto* last = v.data() + v.size();
    if (std::from_chars(first, last, out).ec != std::errc{}) return fallback;
    return out;
}

std::uint64_t ParseUint(std::string_view v, std::uint64_t fallback) {
    std::uint64_t out = fallback;
    const auto* first = v.data();
    const auto* last = v.data() + v.size();
    if (std::from_chars(first, last, out).ec != std::errc{}) return fallback;
    return out;
}

void Emit(std::ostringstream& o, const char* key, const std::string& v) {
    o << key << ": " << v << "\n";
}
void Emit(std::ostringstream& o, const char* key, double v) {
    o << key << ": " << v << "\n";
}
void Emit(std::ostringstream& o, const char* key, std::uint64_t v) {
    o << key << ": " << v << "\n";
}

}  // namespace

// ---------------------------------------------------------------- metadata

Status CaptureMetadata::Validate() const noexcept {
    if (width == 0 || height == 0) return Error::kValueOutOfRange;
    // A bundle large enough to exhaust memory on open is a real hazard even for our own
    // files: 64 MP is far beyond any phone sensor and keeps a frame under ~64 MB.
    constexpr std::uint64_t kMaxPixels = 64ull * 1000 * 1000;
    if (static_cast<std::uint64_t>(width) * height > kMaxPixels) return Error::kValueOutOfRange;
    if (grid_cols > GridGeometry::kMaxCols || grid_rows > GridGeometry::kMaxRows) {
        return Error::kValueOutOfRange;
    }
    return Status::Ok();
}

std::vector<std::string> CaptureMetadata::MissingRequiredFields() const {
    std::vector<std::string> missing;
    if (sender_model.empty()) missing.emplace_back("sender_model");
    if (receiver_model.empty()) missing.emplace_back("receiver_model");
    if (app_commit.empty()) missing.emplace_back("app_commit");
    if (grid_cols == 0 || grid_rows == 0) missing.emplace_back("grid_cols/grid_rows");
    if (width == 0 || height == 0) missing.emplace_back("width/height");
    if (distance_cm < 0.0) missing.emplace_back("distance_cm");
    if (source_payload_sha256.empty()) missing.emplace_back("source_payload_sha256");
    return missing;
}

std::string CaptureMetadata::Serialise() const {
    std::ostringstream o;
    o << "# FileFlow capture bundle metadata\n";
    o << "# Unset numeric fields are negative and unset strings are blank, ON PURPOSE:\n";
    o << "# a capture must show what was not recorded rather than imply a default.\n";
    Emit(o, "sender_model", sender_model);
    Emit(o, "receiver_model", receiver_model);
    Emit(o, "os_build", os_build);
    Emit(o, "app_commit", app_commit);
    Emit(o, "notes", notes);
    Emit(o, "display_mode", display_mode);
    Emit(o, "grid_cols", static_cast<std::uint64_t>(grid_cols));
    Emit(o, "grid_rows", static_cast<std::uint64_t>(grid_rows));
    Emit(o, "modulation_profile", modulation_profile);
    Emit(o, "screen_brightness", screen_brightness);
    Emit(o, "camera_id", camera_id);
    Emit(o, "width", static_cast<std::uint64_t>(width));
    Emit(o, "height", static_cast<std::uint64_t>(height));
    Emit(o, "fps", fps);
    Emit(o, "exposure_ns", exposure_ns);
    Emit(o, "iso", iso);
    Emit(o, "focus_distance", focus_distance);
    Emit(o, "white_balance", white_balance);
    Emit(o, "distance_cm", distance_cm);
    Emit(o, "angle_deg", angle_deg);
    Emit(o, "ambient_lux", ambient_lux);
    Emit(o, "motion_condition", motion_condition);
    Emit(o, "source_payload_sha256", source_payload_sha256);
    Emit(o, "source_payload_bytes", source_payload_bytes);
    Emit(o, "frame_count", static_cast<std::uint64_t>(frame_count));
    return o.str();
}

Result<CaptureMetadata> CaptureMetadata::Parse(std::string_view text) {
    CaptureMetadata m;
    std::size_t pos = 0;
    std::size_t lines = 0;

    // Bounded even though these are our own files. A parser that trusts its input because of
    // where the input came from is a parser that will be handed something else eventually.
    constexpr std::size_t kMaxLines = 4096;
    constexpr std::size_t kMaxValue = 4096;

    while (pos < text.size() && lines < kMaxLines) {
        ++lines;
        const std::size_t nl = text.find('\n', pos);
        std::string_view line =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;

        line = Trim(line);
        if (line.empty() || line.front() == '#') continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;  // tolerate junk lines
        const std::string_view key = Trim(line.substr(0, colon));
        std::string_view val = Trim(line.substr(colon + 1));
        if (val.size() > kMaxValue) val = val.substr(0, kMaxValue);

        const auto s = [&](std::string& dst) { dst.assign(val); };

        if (key == "sender_model") s(m.sender_model);
        else if (key == "receiver_model") s(m.receiver_model);
        else if (key == "os_build") s(m.os_build);
        else if (key == "app_commit") s(m.app_commit);
        else if (key == "notes") s(m.notes);
        else if (key == "display_mode") s(m.display_mode);
        else if (key == "modulation_profile") s(m.modulation_profile);
        else if (key == "camera_id") s(m.camera_id);
        else if (key == "white_balance") s(m.white_balance);
        else if (key == "motion_condition") s(m.motion_condition);
        else if (key == "source_payload_sha256") s(m.source_payload_sha256);
        else if (key == "grid_cols") m.grid_cols = static_cast<std::uint32_t>(ParseUint(val, 0));
        else if (key == "grid_rows") m.grid_rows = static_cast<std::uint32_t>(ParseUint(val, 0));
        else if (key == "width") m.width = static_cast<std::uint32_t>(ParseUint(val, 0));
        else if (key == "height") m.height = static_cast<std::uint32_t>(ParseUint(val, 0));
        else if (key == "frame_count") {
            m.frame_count = static_cast<std::uint32_t>(ParseUint(val, 0));
        } else if (key == "source_payload_bytes") {
            m.source_payload_bytes = ParseUint(val, 0);
        } else if (key == "screen_brightness") m.screen_brightness = ParseDouble(val, -1.0);
        else if (key == "fps") m.fps = ParseDouble(val, 0.0);
        else if (key == "exposure_ns") m.exposure_ns = ParseDouble(val, -1.0);
        else if (key == "iso") m.iso = ParseDouble(val, -1.0);
        else if (key == "focus_distance") m.focus_distance = ParseDouble(val, -1.0);
        else if (key == "distance_cm") m.distance_cm = ParseDouble(val, -1.0);
        else if (key == "angle_deg") m.angle_deg = ParseDouble(val, -1.0);
        else if (key == "ambient_lux") m.ambient_lux = ParseDouble(val, -1.0);
        // Unknown keys are ignored, so a newer recorder's extra fields do not break an older
        // replay tool. Forward compatibility costs nothing here.
    }

    FF_TRY(m.Validate());
    return m;
}

// ---------------------------------------------------------------- reading frames

Status ReadGrayFrame(const std::string& path, std::uint32_t width, std::uint32_t height,
                     std::vector<std::uint8_t>* out) {
    if (out == nullptr) return Error::kInternal;
    const std::uint64_t want = static_cast<std::uint64_t>(width) * height;
    if (want == 0) return Error::kValueOutOfRange;

    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return Error::kIoError;
    // Exact-size check BEFORE allocating: a truncated or padded frame is a corrupt bundle,
    // and silently accepting either would shift every subsequent pixel.
    if (size != want) return Error::kLengthMismatch;

    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::kIoError;
    out->resize(static_cast<std::size_t>(want));
    f.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(want));
    if (!f) return Error::kIoError;
    return Status::Ok();
}

// ---------------------------------------------------------------- writer

Result<CaptureWriter> CaptureWriter::Create(const std::string& bundle_dir,
                                            CaptureMetadata meta) {
    FF_TRY(meta.Validate());
    std::error_code ec;
    fs::create_directories(fs::path(bundle_dir) / "frames", ec);
    if (ec) return Error::kIoError;
    return CaptureWriter(bundle_dir, std::move(meta));
}

Status CaptureWriter::WriteFrame(const ImageView8& img) {
    if (finished_) return Error::kInternal;
    if (img.empty()) return Error::kValueOutOfRange;
    if (static_cast<std::uint32_t>(img.width()) != meta_.width ||
        static_cast<std::uint32_t>(img.height()) != meta_.height) {
        return Error::kLengthMismatch;
    }

    std::ofstream f(FramePath(dir_, frames_), std::ios::binary);
    if (!f) return Error::kIoError;

    // Written row by row so a strided source (the camera's Y plane always is) lands as a
    // tightly packed frame. Writing the raw buffer would embed the driver's stride and make
    // the bundle unreadable anywhere else.
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const char v = static_cast<char>(img.At(x, y));
            f.write(&v, 1);
        }
    }
    if (!f) return Error::kIoError;
    ++frames_;
    return Status::Ok();
}

Status CaptureWriter::Finish() {
    if (finished_) return Error::kInternal;
    meta_.frame_count = frames_;
    std::ofstream f(MetaPath(dir_));
    if (!f) return Error::kIoError;
    const std::string text = meta_.Serialise();
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return Error::kIoError;
    finished_ = true;
    return Status::Ok();
}

// ---------------------------------------------------------------- replay

ReplaySource::ReplaySource(std::string dir, CaptureMetadata meta, const FrameLayout& layout,
                           FramePipeline pipeline)
    : dir_(std::move(dir)),
      meta_(std::move(meta)),
      layout_(&layout),
      pipeline_(std::move(pipeline)) {}

Result<ReplaySource> ReplaySource::Create(const std::string& bundle_dir,
                                          const FrameLayout& layout, PipelineConfig cfg) {
    std::ifstream f(MetaPath(bundle_dir));
    if (!f) return Error::kIoError;
    std::ostringstream ss;
    ss << f.rdbuf();

    FF_ASSIGN_OR_RETURN(auto meta, CaptureMetadata::Parse(ss.str()));

    // A bundle recorded at a different grid cannot be decoded against this layout. Catching it
    // here beats a frame of garbage samples and a mystified reader.
    if (meta.grid_cols != 0 && meta.grid_rows != 0) {
        if (meta.grid_cols != layout.geometry().cols ||
            meta.grid_rows != layout.geometry().rows) {
            return Error::kGridMismatch;
        }
    }

    FF_ASSIGN_OR_RETURN(auto pipeline, FramePipeline::Create(layout, cfg));
    return ReplaySource(bundle_dir, std::move(meta), layout, std::move(pipeline));
}

void ReplaySource::Rewind() noexcept {
    next_ = 0;
    pipeline_.Reset();
}

std::optional<CapturedFrame> ReplaySource::Next() {
    if (next_ >= meta_.frame_count) return std::nullopt;

    const std::uint32_t idx = next_++;
    if (!ReadGrayFrame(FramePath(dir_, idx), meta_.width, meta_.height, &buffer_).ok()) {
        // A missing or corrupt frame file is a gap in the recording, not the end of it. Report
        // it as a dropped frame and keep going -- exactly as a live receiver treats a dropped
        // camera frame -- so one bad file does not truncate an otherwise good dataset.
        CapturedFrame out;
        out.index = idx;
        out.cell_samples.assign(static_cast<std::size_t>(layout_->geometry().cells()),
                                std::nan(""));
        return out;
    }

    const ImageView8 view(buffer_.data(), static_cast<int>(meta_.width),
                          static_cast<int>(meta_.height), static_cast<int>(meta_.width));

    // Nominal timing from the recorded fps. Real per-frame sensor timestamps belong in the
    // metadata once the Android recorder exists; until then this is clearly nominal.
    const double period_ns = meta_.fps > 0.0 ? 1e9 / meta_.fps : 16'666'667.0;
    return pipeline_.Process(view, idx, static_cast<std::int64_t>(idx * period_ns));
}

}  // namespace fileflow::harness
