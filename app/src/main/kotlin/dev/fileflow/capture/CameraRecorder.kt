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

    /**
     * Set to stop an unbounded run. Checked on the camera callback thread.
     *
     * `@Volatile` rather than a lock: it is written from the UI thread and read from the camera
     * thread, and a torn read of a boolean is not possible — the only requirement is visibility.
     */
    @Volatile
    private var cancelled = false

    /** Ends a run started with `frameCount <= 0`. Safe to call from any thread. */
    fun cancel() {
        cancelled = true
    }

    /**
     * Facts about the transmitter and the physical rig that the receiver cannot observe.
     *
     * Defaults are the format's **sentinels**, not plausible values: a capture whose distance was
     * never measured must not claim 30 cm (C17).
     */
    data class RigMetadata(
        val senderModel: String = "",
        val displayMode: String = "",
        val gridCols: Int = 0,
        val gridRows: Int = 0,
        val modulationProfile: String = "",
        val screenBrightness: Double = -1.0,
        val distanceCm: Double = -1.0,
        val angleDeg: Double = -1.0,
        val ambientLux: Double = -1.0,
        val motionCondition: String = "",
        val payloadSha256: String = "",
        val payloadBytes: Long = 0L,
    )

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
        /** `LENS_INFO_MINIMUM_FOCUS_DISTANCE` in diopters — the closest the lens can focus. */
        val minFocusDiopters: Float,
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
         * Focus, in reciprocal metres. **Negative means continuous autofocus.**
         *
         * 5.0 focuses at 20 cm, 3.33 at 30 cm. There is no safe fixed default: infinity is correct
         * for a distant target and catastrophic for a screen at arm's length (F32).
         */
        focusDiopters: Float = -1f,
        /**
         * Exposure in nanoseconds, and ISO. Both **parameters, not constants**, because the right
         * values depend entirely on what is being photographed and EXP-004/EXP-005 exist to choose
         * them.
         *
         * The first values here — a quarter of the frame period at ISO 400 — were reasonable for a
         * general scene and badly wrong for this one: a full-brightness OLED at close range came
         * back at 66% bright against 9% dark, washed out well past the point where a threshold can
         * separate two levels (F33). Pointing a camera at a light source is not the average case,
         * and the average-case defaults flattered nothing.
         *
         * 0 means "derive from the frame period" (the old behaviour), so a sweep can vary one axis
         * at a time.
         */
        exposureNs: Long = 0L,
        iso: Int = DEFAULT_ISO,
        /**
         * What the TRANSMITTER is doing, and how the rig is arranged.
         *
         * None of this is discoverable from the receiver — the camera cannot know the grid it is
         * looking at, and it certainly cannot know the distance. `ffreplay` REFUSES a bundle whose
         * grid is unset (F29), so without this a capture is not decodable; and CAPTURE-HARNESS
         * requires the rig figures because a capture whose conditions were not recorded is not
         * evidence. Unset fields keep their sentinels rather than plausible defaults.
         */
        rig: RigMetadata = RigMetadata(),
        /**
         * When false, frames are counted and timed but never written.
         *
         * This is the A/B that makes a low delivered rate ATTRIBUTABLE. Writing a 1920x1440 Y
         * plane is 2.76 MB, so 60 fps is ~166 MB/s of I/O; if that saturates, unreturned
         * `ImageReader` buffers throttle the camera and the result is indistinguishable from a
         * sensor that cannot hit the rate. Run both arms and the difference names the culprit.
         */
        writeFrames: Boolean = true,
        /**
         * Called for every delivered frame, on the CAMERA THREAD, with a view of the driver's own Y
         * plane. Must return promptly: the buffer is released as soon as it returns, and holding it
         * starves the pipeline in a way that looks exactly like a camera that cannot hit its rate.
         *
         * This exists so aiming analysis shares ONE session and one set of manual settings with
         * recording. A separate preview path would duplicate the request configuration, and the two
         * copies would drift — which is the mistake ADR-0014 was written about, in a different place.
         */
        onFrame: ((java.nio.ByteBuffer, Int, Int, Int) -> Unit)? = null,
    ): Outcome {
        // `rig` carries the grid, and the grid decides which capture mode maximises px/cell, so it
        // must be known before the mode is chosen.
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

        val size = pickSize(map, targetFps, maxWidth, rig.gridCols, rig.gridRows)
            ?: return failure("no YUV_420_888 size at <= $maxWidth px wide", cameraId)

        var thread: HandlerThread? = null
        var reader: ImageReader? = null
        var device: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var recorder: Recorder? = null

        val timestamps = ArrayList<Long>(maxOf(frameCount, 64))
        var reportedExposure = -1L
        var reportedIso = -1
        var reportedFocus = -1f
        val minFocus = chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: -1f
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
                senderModel = rig.senderModel,
                receiverModel = "${android.os.Build.MANUFACTURER} ${android.os.Build.MODEL}",
                osBuild = android.os.Build.FINGERPRINT,
                appCommit = "see git; app ${versionName()}",
                notes = notes,
                displayMode = rig.displayMode,
                gridCols = rig.gridCols,
                gridRows = rig.gridRows,
                modulationProfile = rig.modulationProfile,
                screenBrightness = rig.screenBrightness,
                cameraId = cameraId,
                width = size.width,
                height = size.height,
                fps = targetFps.toDouble(),
                distanceCm = rig.distanceCm,
                angleDeg = rig.angleDeg,
                ambientLux = rig.ambientLux,
                motionCondition = rig.motionCondition,
                sourcePayloadSha256 = rig.payloadSha256,
                sourcePayloadBytes = rig.payloadBytes,
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
            // `frameCount <= 0` means run until cancelled, which is what the aiming UI wants: it
            // needs a steady stream of verdicts, not a fixed-size dataset.
            val unbounded = frameCount <= 0
            reader.setOnImageAvailableListener({ r ->
                val image = r.acquireNextImage() ?: return@setOnImageAvailableListener
                try {
                    if (cancelled) {
                        done.countDown()
                    } else if (unbounded || timestamps.size < frameCount) {
                        onFrame?.let { cb ->
                            val plane = image.planes[0]
                            cb(plane.buffer, image.width, image.height, plane.rowStride)
                        }
                        val code = if (writeFrames) rec.writeFrame(image) else 0
                        if (code == 0) {
                            timestamps.add(image.timestamp)
                        } else if (error == null) {
                            error = "writeFrame: ${Recorder.errorName(code)}"
                        }
                        if (!unbounded && timestamps.size >= frameCount) done.countDown()
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
                // Focus is the one manual setting that CANNOT have a fixed sensible default.
                //
                // The first two-device run captured with `LENS_FOCUS_DISTANCE = 0`, i.e. INFINITY,
                // against a transmitting screen roughly 20 cm away — so every frame was recorded
                // out of focus, and a failed decode would have looked like the grid being
                // unresolvable rather than the lens being pointed at nothing (F32). Defocus and
                // insufficient resolution both destroy high-spatial-frequency cell structure, and
                // they are indistinguishable in a decode log.
                //
                // `focusDiopters < 0` means "let the camera decide": continuous AF runs even with
                // manual exposure, since AF and AE are independent. That is the robust default,
                // because it needs no knowledge of the rig geometry. A non-negative value is
                // reciprocal metres (5.0 = 20 cm) and locks focus for the session, which is what a
                // repeatable measurement wants once the distance IS known.
                if (focusDiopters < 0f) {
                    set(
                        CaptureRequest.CONTROL_AF_MODE,
                        CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE,
                    )
                } else {
                    set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                }
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
                    val exposure =
                        if (exposureNs > 0L) exposureNs else (1_000_000_000L / targetFps) / 4
                    set(CaptureRequest.SENSOR_EXPOSURE_TIME, exposure)
                    set(CaptureRequest.SENSOR_SENSITIVITY, iso)
                    if (focusDiopters >= 0f) {
                        // Clamped to what the lens can actually do. Requesting 10 diopters on a lens
                        // whose minimum focus distance is 5 would otherwise be silently ignored, and
                        // the read-back below is what proves which happened.
                        val maxDiopters =
                            chars.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                        val want = if (maxDiopters > 0f) focusDiopters.coerceAtMost(maxDiopters)
                                   else focusDiopters
                        set(CaptureRequest.LENS_FOCUS_DISTANCE, want)
                    }

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

            if (unbounded) {
                // No deadline: an aiming session lasts as long as the user needs. Polled rather than
                // awaited without limit so a cancel is never missed if it lands between checks.
                while (!cancelled) {
                    if (done.await(200, TimeUnit.MILLISECONDS)) break
                }
            } else {
                // Generous: a cold camera start plus AE settling can take a second or two, and the
                // point is to fail with data rather than to fail fast.
                val timeoutSec = 20L + frameCount / maxOf(targetFps, 1)
                if (!done.await(timeoutSec, TimeUnit.SECONDS)) {
                    error = "timed out after ${timestamps.size}/$frameCount frames"
                }
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
                minFocusDiopters = minFocus,
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
     * The `YUV_420_888` size that maximises **resolvable pixels per cell** for the target grid.
     *
     * NOT largest-by-area, which is what this did first and which is subtly wrong. Asked for a mode
     * at or below 1920 wide, largest-area picked a **1920×1920** square sensor mode — and a square
     * frame wastes most of its pixels on a portrait screen, delivering no more px/cell than
     * 1920×1080 while costing 78% more bandwidth (F33).
     *
     * The criterion that actually matters: the screen is a rectangle of aspect `cols/rows`, and it
     * must fit inside the frame **with margin** or the boundary ring is clipped and localisation
     * fails outright. Given a mode `W×H` and a margin fraction `m`, the largest the screen's long
     * axis can be is `m·min(W, H/r)` — so px/cell is that over `rows`. Maximising it weighs
     * resolution and aspect together, which area alone cannot.
     *
     * Both orientations are considered, because the receiver can be rotated and the sensor's long
     * axis may carry either screen axis.
     */
    private fun pickSize(
        map: StreamConfigurationMap,
        targetFps: Int,
        maxWidth: Int,
        cols: Int,
        rows: Int,
    ): Size? {
        val minDurationNs = 1_000_000_000L / targetFps
        val candidates = map.getOutputSizes(ImageFormat.YUV_420_888)
            ?.filter { it.width <= maxWidth }
            ?.filter { map.getOutputMinFrameDuration(ImageFormat.YUV_420_888, it) <= minDurationNs }
            ?: return null
        if (cols <= 0 || rows <= 0) return candidates.maxByOrNull { it.width.toLong() * it.height }

        val r = cols.toDouble() / rows       // screen short : long
        val margin = 0.85                    // must leave a visible ring of background on all sides
        fun score(s: Size): Double {
            val a = margin * minOf(s.width.toDouble(), s.height / r)
            val b = margin * minOf(s.height.toDouble(), s.width / r)
            return maxOf(a, b) / rows
        }
        return candidates.maxByOrNull { score(it) }
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
        reportedIso = -1, reportedFocusDistance = -1f, minFocusDiopters = -1f, aeMode = -1,
        reportedFrameDurationNs = -1L, edgeMode = -1,
        noiseReductionMode = -1, manualRequested = false, error = why, framesWereWritten = true,
    )

    companion object {
        private const val TAG = "FileFlow.Capture"
        private const val IMAGE_BUFFERS = 6

        /**
         * Low, deliberately. The subject is an emissive display at full brightness, not a room.
         * ISO 400 overexposed it badly (F33), and overexposure destroys the dark level the
         * photometric field needs to separate two states.
         */
        const val DEFAULT_ISO = 60
    }
}
