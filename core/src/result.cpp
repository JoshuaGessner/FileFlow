#include <fileflow/result.h>

namespace fileflow {

std::string_view ErrorName(Error e) noexcept {
    switch (e) {
        case Error::kNone: return "none";
        case Error::kTruncated: return "truncated";
        case Error::kSizeOverflow: return "size_overflow";
        case Error::kValueOutOfRange: return "value_out_of_range";
        case Error::kLengthMismatch: return "length_mismatch";
        case Error::kTooManyExtensions: return "too_many_extensions";
        case Error::kBadMagic: return "bad_magic";
        case Error::kUnsupportedVersion: return "unsupported_version";
        case Error::kHeaderCrcMismatch: return "header_crc_mismatch";
        case Error::kUnknownProfile: return "unknown_profile";
        case Error::kGridMismatch: return "grid_mismatch";
        case Error::kUncorrectable: return "uncorrectable";
        case Error::kFountainIncomplete: return "fountain_incomplete";
        case Error::kDegenerateParameters: return "degenerate_parameters";
        case Error::kIterationLimit: return "iteration_limit";
        case Error::kMarkersNotFound: return "markers_not_found";
        case Error::kDegenerateHomography: return "degenerate_homography";
        case Error::kHashMismatch: return "hash_mismatch";
        case Error::kPayloadTooLarge: return "payload_too_large";
        case Error::kIoError: return "io_error";
        case Error::kInternal: return "internal";
    }
    return "unknown";
}

}  // namespace fileflow
