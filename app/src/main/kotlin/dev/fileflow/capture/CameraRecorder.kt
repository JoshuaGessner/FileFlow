package dev.fileflow.capture

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * The CPU capture path of C05: opens the camera with locked settings and writes every Y plane
 * into a capture bundle, measuring what actually arrived.
 *
 * WHAT THIS IS FOR. The probe (C02) can only report what the camera *advertises*. This is the
 * first thing that finds out what it *delivers* — the verification half of EXP-007 — and it is the
 * step that makes every analysis tool in the repo apply to real data, because the bundle it writes
 * is the format `ffreplay` already consumes and F17 proved bit-identical to live decode.
 *
 * WHY THE CPU PATH FIRST. `YUV_420_888` through `ImageReader` tops out at 60 fps on the reference
 * device (F27). The ≥120 fps arm needs a constrained high-speed session driven from a
 * `SurfaceTexture`, because such a session **rejects `ImageReader`** `[FACT]` — a different code
 * path, and one whose value depends on the distinctness question this run answers first.
 *
 * WHAT IT DELIBERATELY MEASURES, beyond writing frames:
 *  - **delivered rate**, from sensor timestamps, not from what was requested
 *  - **duplicate fraction**, byte-identical frames (see [Recorder.duplicateFrames])
 *  - **whether the manual settings were honoured**, read back from `CaptureResult` rather than
 *    trusted from the request — RISK-011's whole point
 *
 * Nothing here interprets those numbers. It records them; the decision belongs to C02's policy.
 */
class CameraRecorder(private val context: Context) {

    /** Everything one run produced. All of it observed, none of it requested. */
    data class Outcome(
        val cameraId: String,
        val size: Size,
        val requestedFps: Int,
        val framesWritten: Int,
        val duplicateFrames: Int,
        val timestampsNs: LongArray,
        /** Exposure/ISO/AF as the camera REPORTED them, not as we asked. */
        val reportedExposureNs: Long,
        val reportedIso: Int,
        val reportedFocusDistance: Float,
        val aeMode: Int,
        /** `SENSOR_FRAME_DURATION` as reported. The rate ceiling under manual sensor mode. */
        val reportedFrameDurationNs: Long,
        val edgeMode: Int,
        val noiseReductionMode: Int,
        val manualRequested: Boolean,
        val error: String?,
        /**
         * False when the run deliberately skipped disk writes to isolate camera rate from writer
         * throughput. When false, [duplicateFrames] was **not measured** and must not be read as
         * zero — duplicate detection lives in the writer.
         */
        val framesWereWritten: Boolean,
    ) {
        /**
         * Delivered rate from the FIRST to the LAST sensor timestamp.
         *
         * Uses sensor timestamps rather than wall clock so writer stalls do not flatter or
         * penalise the camera, and spans the whole run rather than averaging per-frame deltas so a
         * single long gap cannot hide behind many short ones.
         */
        val deliveredFps: Double
            get() {
                if (timestampsNs.size < 2) return 0.0
                val span = timestampsNs.last() - timestampsNs.first()
                if (span <= 0L) return 0.0
                return (timestampsNs.size - 1) * 1_000_000_000.0 / span
            }

        /** Null when the run did not write frames, because then it was never measured. */
        val duplicateFraction: Double?
            get() = when {
                !framesWereWritten -> null
                framesWritten > 0 -> duplicateFrames.toDouble() / framesWritten
                else -> null
            }

        /**
         * Distinct frames per second — the rate that actually matters, and the one the goodput
         * model's `Pc` depends on. A session delivering 240 buffers/s of which half repeat has an
         * effective rate of 120, and quoting 240 would be the unlabelled-number defect ADR-0012
         * exists to prevent.
         */
        val distinctFps: Double?
            get() = duplicateFraction?.let { deliveredFps * (1.0 - it) }

        /** Largest gap between consecutive frames, in units of the nominal frame period. */
        val worstGapFrames: Double
            get() {
                if (timestampsNs.size < 2 || requestedFps <= 0) return 0.0
                val period = 1_000_000_000.0 / requestedFps
                var worst = 0.0
                for (i in 1 until timestampsNs.size) {
                    val d = (timestampsNs[i] - timestampsNs[i - 1]) / period
                    if (d > worst) worst = d
                }
                return worst
            }
    }

    /**
     * Records [frameCount] frames into [bundleDir].
     *
     * Blocking, and meant to be called off the main thread. Returns an [Outcome] even on failure —
     * a run that captured 12 frames and then died tells us more than an exception does, and losing
     * the 12 frames' worth of timing would waste the trip to the device.
     */
    @Suppress("LongMethod")
    fun record(
        bundleDir: String,
        frameCount: Int,
        targetFps: Int,
        maxWidth: Int,
        notes: String,
        /**
         * When false, frames are counted and timed but never written.
         *
         * This is the A/B that makes a low delivered rate ATTRIBUTABLE. Writing a 1920x1440 Y
         * plane is 2.76 MB, so 60 fps is ~166 MB/s of I/O; if that saturates, unreturned
         * `ImageReader` buffers throttle the camera and the result is indistinguishable from a
         * sensor that cannot hit the rate. Run both arms and the difference names the culprit.
         */
        writeFrames: Boolean = true,
    ): Outcome {
        val mgr = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager

        // Back camera. The front camera is not a candidate: it is lower resolution on every
        // device we care about, and a receiver is held facing the transmitting screen.
        val cameraId = mgr.cameraIdList.firstOrNull { id ->
            mgr.getCameraCharacteristics(id)
                .get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK
        } ?: mgr.cameraIdList.firstOrNull()
            ?: return failure("no camera on this device")

        val chars = mgr.getCameraCharacteristics(cameraId)
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?: return failure("no stream configuration map", cameraId)

        val size = pickSize(map, targetFps, maxWidth)
            ?: return failure("no YUV_420_888 size at <= $maxWidth px wide", cameraId)

        var thread: HandlerThread? = null
        var reader: ImageReader? = null
        var device: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var recorder: Recorder? = null

        val timestamps = ArrayList<Long>(frameCount)
        var reportedExposure = -1L
        var reportedIso = -1
        var reportedFocus = -1f
        var aeMode = -1
        var reportedFrameDuration = -1L
        var edgeMode = -1
        var nrMode = -1
        var manualRequested = false
        var error: String? = null

        try {
            thread = HandlerThread("ff-capture").apply { start() }
            val handler = Handler(thread.looper)

            val meta = CaptureMetadata(
                receiverModel = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}",
                osBuild = android.os.Build.FINGERPRINT,
                appCommit = "see git; app ${versionName()}",
                notes = notes,
                cameraId = cameraId,
                width = size.width,
                height = size.height,
                fps = targetFps.toDouble(),
                motionCondition = "handheld/unspecified",
            )
            recorder = Recorder.open(bundleDir, meta)

            // maxImages: enough to absorb a writer hiccup without the camera pipeline stalling,
            // small enough that a leak surfaces fast. C05: return buffers promptly or the pipeline
            // stops — every image is closed in the listener below.
            reader = ImageReader.newInstance(
                size.width, size.height, ImageFormat.YUV_420_888, IMAGE_BUFFERS,
            )

            val done = CountDownLatch(1)
            val rec = recorder
            reader.setOnImageAvailableListener({ r ->
                val image = r.acquireNextImage() ?: return@setOnImageAvailableListener
                try {
                    if (timestamps.size < frameCount) {
                        val code = if (writeFrames) rec.writeFrame(image) else 0
                        if (code == 0) {
                            timestamps.add(image.timestamp)
                        } else if (error == null) {
                            error = "writeFrame: ${Recorder.errorName(code)}"
                        }
                        if (timestamps.size >= frameCount) done.countDown()
                    }
                } finally {
                    // Always. A held image starves the pipeline, and a starved pipeline looks
                    // exactly like a camera that cannot hit its rate.
                    image.close()
                }
            }, handler)

            device = openCamera(mgr, cameraId, handler)
                ?: return failure("could not open camera $cameraId", cameraId, size, targetFps)

            val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(reader.surface)
                set(
                    CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,
                    android.util.Range(targetFps, targetFps),
                )
                // Everything below exists so consecutive frames are photometrically comparable.
                // Auto-anything re-tunes between frames, which moves the dark/bright references
                // the photometric field (C09) is trying to estimate.
                set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_OFF)
                // OQ-016: vendor sharpening and denoise destroy high-spatial-frequency cell
                // structure — the exact signal we are trying to read. Requested off; whether the
                // request is honoured is read back below, not assumed.
                set(CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_OFF)
                set(CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_OFF)

                if (chars.hasCapability(
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
                    )
                ) {
                    manualRequested = true
                    set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    // Exposure one quarter of the frame period: short enough to limit temporal
                    // mixing across display states (the Pc term), long enough to keep SNR usable.
                    // EXP-004 picks the real value; this is a defensible starting point, not one.
                    val exposure = (1_000_000_000L / targetFps) / 4
                    set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposure)
                    set(CaptureRequest.SENSOR_SENSITIVITY, START_ISO)
                    set(CaptureRequest.LENS_FOCUS_DISTANCE, 0f)  // 0 = infinity

                    // THE frame-rate control once AE is off, and the reason the first run of this
                    // recorder delivered 31 fps against a requested 60 (finding F28).
                    //
                    // `CONTROL_AE_TARGET_FPS_RANGE` is an AUTO-EXPOSURE control: with
                    // CONTROL_AE_MODE_OFF the auto-exposure routine is not running, so nothing
                    // consumes the range and the frame period falls back to whatever the request
                    // template carries. Under manual sensor control the period is set explicitly
                    // by SENSOR_FRAME_DURATION, and omitting it does not fail loudly -- it just
                    // silently produces the wrong rate, which is indistinguishable from a device
                    // that cannot hit the rate.
                    set(CaptureRequest.SENSOR_FRAME_DURATION, 1_000_000_000L / targetFps)
                } else {
                    set(CaptureRequest.CONTROL_AE_LOCK, true)
                }
            }.build()

            session = createSession(device, reader.surface, handler)
                ?: return failure("could not configure a capture session", cameraId, size, targetFps)

            session.setRepeatingRequest(
                request,
                object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(
                        s: CameraCaptureSession,
                        r: CaptureRequest,
                        result: TotalCaptureResult,
                    ) {
                        // Read back what the camera DID, not what we asked for. RISK-011: a
                        // device advertising MANUAL_SENSOR may ignore the settings, and the only
                        // way to know is to compare the result against the request.
                        result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { reportedExposure = it }
                        result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { reportedIso = it }
                        result.get(CaptureResult.LENS_FOCUS_DISTANCE)?.let { reportedFocus = it }
                        result.get(CaptureResult.CONTROL_AE_MODE)?.let { aeMode = it }
                        result.get(CaptureResult.SENSOR_FRAME_DURATION)?.let {
                            reportedFrameDuration = it
                        }
                        result.get(CaptureResult.EDGE_MODE)?.let { edgeMode = it }
                        result.get(CaptureResult.NOISE_REDUCTION_MODE)?.let { nrMode = it }
                    }
                },
                handler,
            )

            // Generous: a cold camera start plus AE settling can take a second or two, and the
            // point is to fail with data rather than to fail fast.
            val timeoutSec = 20L + frameCount / maxOf(targetFps, 1)
            if (!done.await(timeoutSec, TimeUnit.SECONDS)) {
                error = "timed out after ${timestamps.size}/$frameCount frames"
            }
        } catch (e: Exception) {
            error = "${e::class.simpleName}: ${e.message}"
            Log.e(TAG, "capture failed", e)
        } finally {
            runCatching { session?.stopRepeating() }
            runCatching { session?.close() }
            runCatching { device?.close() }
            runCatching { reader?.close() }
            val code = recorder?.finish() ?: 0
            if (code != 0 && error == null) error = "finish: ${Recorder.errorName(code)}"
            val dupes = recorder?.duplicateFrames ?: -1
            val written = recorder?.framesWritten ?: -1
            runCatching { recorder?.close() }
            runCatching { thread?.quitSafely() }

            return Outcome(
                framesWereWritten = writeFrames,
                cameraId = cameraId,
                size = size,
                requestedFps = targetFps,
                framesWritten = if (writeFrames) written else timestamps.size,
                duplicateFrames = dupes,
                timestampsNs = timestamps.toLongArray(),
                reportedExposureNs = reportedExposure,
                reportedIso = reportedIso,
                reportedFocusDistance = reportedFocus,
                aeMode = aeMode,
                reportedFrameDurationNs = reportedFrameDuration,
                edgeMode = edgeMode,
                noiseReductionMode = nrMode,
                manualRequested = manualRequested,
                error = error,
            )
        }
    }

    // ------------------------------------------------------------------ helpers

    private fun CameraCharacteristics.hasCapability(cap: Int): Boolean =
        get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.contains(cap) == true

    /**
     * Largest `YUV_420_888` size at or below [maxWidth] whose minimum frame duration supports
     * [targetFps].
     *
     * Largest-that-fits rather than largest available: pixels per cell is what bounds resolvable
     * grid density, so throwing resolution away costs density directly — but a size the sensor
     * cannot clock at the target rate costs `Fd`, which is worse.
     */
    private fun pickSize(map: StreamConfigurationMap, targetFps: Int, maxWidth: Int): Size? {
        val minDurationNs = 1_000_000_000L / targetFps
        return map.getOutputSizes(ImageFormat.YUV_420_888)
            ?.filter { it.width <= maxWidth }
            ?.filter { map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, it) <= minDurationNs }
            ?.maxByOrNull { it.width.toLong() * it.height }
    }

    private fun openCamera(mgr: CameraManager, id: String, handler: Handler): CameraDevice? {
        var device: CameraDevice? = null
        val latch = CountDownLatch(1)
        mgr.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(c: CameraDevice) { device = c; latch.countDown() }
            override fun onDisconnected(c: CameraDevice) { c.close(); latch.countDown() }
            override fun onError(c: CameraDevice, e: Int) {
                Log.e(TAG, "camera error $e")
                c.close()
                latch.countDown()
            }
        }, handler)
        latch.await(10, TimeUnit.SECONDS)
        return device
    }

    @Suppress("DEPRECATION")  // SessionConfiguration is API 28+, but takes an Executor we do not
    // want here: the capture callbacks must land on the camera Handler thread, and the deprecated
    // overload expresses that directly.
    private fun createSession(
        device: CameraDevice,
        surface: android.view.Surface,
        handler: Handler,
    ): CameraCaptureSession? {
        var session: CameraCaptureSession? = null
        val latch = CountDownLatch(1)
        device.createCaptureSession(
            listOf(surface),
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) { session = s; latch.countDown() }
                override fun onConfigureFailed(s: CameraCaptureSession) {
                    Log.e(TAG, "session configure failed")
                    latch.countDown()
                }
            },
            handler,
        )
        latch.await(10, TimeUnit.SECONDS)
        return session
    }

    private fun versionName(): String = runCatching {
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
    }.getOrElse { "?" }

    private fun failure(
        why: String,
        cameraId: String = "",
        size: Size = Size(0, 0),
        fps: Int = 0,
    ) = Outcome(
        cameraId = cameraId, size = size, requestedFps = fps, framesWritten = 0,
        duplicateFrames = 0, timestampsNs = LongArray(0), reportedExposureNs = -1,
        reportedIso = -1, reportedFocusDistance = -1f, aeMode = -1,
        reportedFrameDurationNs = -1L, edgeMode = -1,
        noiseReductionMode = -1, manualRequested = false, error = why, framesWereWritten = true,
    )

    companion object {
        private const val TAG = "FileFlow.Capture"
        private const val IMAGE_BUFFERS = 6
        private const val START_ISO = 400
    }
}
