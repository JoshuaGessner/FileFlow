package dev.fileflow.capture

import android.media.Image
import java.nio.ByteBuffer

/**
 * JNI bridge to the capture-bundle writer (C05 recording mode, C17 format).
 *
 * ADR-0014: this writes through `harness::CaptureWriter`, the SAME writer the simulator uses and
 * the one F17 proved replay is bit-identical to. It is not reimplemented in Kotlin, because that
 * would make F17's guarantee apply to the wrong code and the first real capture would arrive on
 * an unproven path.
 *
 * ⚠ FIELD NAMES ARE PART OF THE ABI — see the note in `NativeProbe.kt`.
 *
 * ON PER-FRAME JNI. ADR-0003 forbids per-frame *data* crossing the boundary. [writeFrame] passes a
 * direct `ByteBuffer`, so native code reads the camera's own memory and no copy occurs; a
 * non-direct buffer is refused with an error rather than silently copied.
 */

/**
 * Everything CAPTURE-HARNESS.md requires alongside a capture.
 *
 * Unrecorded fields keep their sentinels and are serialised as unset, so a half-labelled dataset
 * cannot quietly become a cited result. A capture whose distance was never measured must not
 * claim 30 cm.
 */
class CaptureMetadata(
    @JvmField val senderModel: String = "",
    @JvmField val receiverModel: String = "",
    @JvmField val osBuild: String = "",
    @JvmField val appCommit: String = "",
    @JvmField val notes: String = "",

    @JvmField val displayMode: String = "",
    @JvmField val gridCols: Int = 0,
    @JvmField val gridRows: Int = 0,
    @JvmField val modulationProfile: String = "",
    @JvmField val screenBrightness: Double = -1.0,

    @JvmField val cameraId: String = "",
    @JvmField val width: Int = 0,
    @JvmField val height: Int = 0,
    @JvmField val fps: Double = 0.0,
    @JvmField val exposureNs: Double = -1.0,
    @JvmField val iso: Double = -1.0,
    @JvmField val focusDistance: Double = -1.0,
    @JvmField val whiteBalance: String = "",

    @JvmField val distanceCm: Double = -1.0,
    @JvmField val angleDeg: Double = -1.0,
    @JvmField val ambientLux: Double = -1.0,
    @JvmField val motionCondition: String = "",

    @JvmField val sourcePayloadSha256: String = "",
    @JvmField val sourcePayloadBytes: Long = 0L,
)

/**
 * A recording session. `use {}`-friendly: [close] releases the native handle.
 *
 * Not thread-safe. Feed it from the single camera callback thread, which is where the frames
 * arrive anyway (C05: that thread hands off immediately and does no processing — writing a frame
 * to disk is I/O, so a real receiver should hand off rather than write inline. For a pure
 * *recording* run there is no decode to compete with, which is why the recorder is usable
 * directly and the live receiver will not be).
 */
class Recorder private constructor(private var handle: Long) : AutoCloseable {

    companion object {
        init {
            System.loadLibrary("fileflow")
        }

        /** Opens a bundle directory. Throws [IllegalStateException] if it cannot be created. */
        fun open(bundleDir: String, meta: CaptureMetadata): Recorder =
            Recorder(NativeRecorder.open(bundleDir, meta))

        /** Turns a native error ordinal into a name a bug report can carry. */
        fun errorName(code: Int): String = NativeRecorder.errorName(code)
    }

    /**
     * Writes one frame from a `YUV_420_888` image's Y plane.
     *
     * Returns 0 on success or a `fileflow::Error` ordinal. A failed frame is a **data point, not
     * an exception**: a dropped frame is normal on this channel and the fountain layer is built
     * for it, so the caller records the gap and continues.
     */
    fun writeFrame(image: Image): Int {
        val h = handle
        if (h == 0L) return -1
        val plane = image.planes[0]
        // Stride is passed through, never assumed equal to width — the Y plane is very often
        // padded, and the native writer packs rows tightly so the driver's stride never reaches
        // the file.
        return NativeRecorder.writeFrame(
            h, plane.buffer, image.width, image.height, plane.rowStride,
        )
    }

    /** Direct-buffer overload for callers that already hold the plane. */
    fun writeFrame(y: ByteBuffer, width: Int, height: Int, rowStride: Int): Int {
        val h = handle
        if (h == 0L) return -1
        return NativeRecorder.writeFrame(h, y, width, height, rowStride)
    }

    val framesWritten: Int
        get() = if (handle != 0L) NativeRecorder.framesWritten(handle) else -1

    /** Flushes metadata with the final frame count. **A bundle is not valid until this runs.** */
    fun finish(): Int {
        val h = handle
        if (h == 0L) return -1
        return NativeRecorder.finish(h)
    }

    override fun close() {
        val h = handle
        handle = 0L
        if (h != 0L) NativeRecorder.close(h)
    }
}

/** Raw external declarations. Use [Recorder] rather than calling these directly. */
internal object NativeRecorder {
    @JvmStatic external fun open(bundleDir: String, meta: CaptureMetadata): Long
    @JvmStatic external fun writeFrame(
        handle: Long, yPlane: ByteBuffer, width: Int, height: Int, rowStride: Int,
    ): Int
    @JvmStatic external fun finish(handle: Long): Int
    @JvmStatic external fun framesWritten(handle: Long): Int
    @JvmStatic external fun close(handle: Long)
    @JvmStatic external fun errorName(code: Int): String
}
