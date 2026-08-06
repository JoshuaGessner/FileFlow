package dev.fileflow

import android.Manifest
import android.content.pm.PackageManager
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.fileflow.aim.AimAnalyser
import dev.fileflow.aim.AimVerdict
import dev.fileflow.aim.AimView
import dev.fileflow.capture.CameraRecorder

/**
 * Live aiming guidance (feature UI-02): point the receiver at the transmitter, with feedback.
 *
 * ```
 * adb shell am start -n dev.fileflow/.AimActivity --ei gridCols 120 --ei gridRows 260
 * ```
 *
 * ## Why a schematic and not a camera preview
 *
 * It draws the *analysis* — where the screen sits inside the frame, which edge is clipping, how big
 * it is — rather than the camera image. That is a deliberate choice, not a shortcut:
 *
 *  - It shows the thing that actually decides success. Five hardware failures were all framing, and
 *    every one is obvious in a schematic and easy to miss in a live image (F33). "The bright rectangle
 *    is touching the top edge" is precisely the fact a user needs.
 *  - It needs no YUV-to-RGB conversion of a multi-megapixel frame per displayed frame, so the
 *    analysis budget goes on analysis.
 *  - It cannot mislead by looking plausible. A preview of a badly overexposed screen looks like a
 *    bright screen; the schematic says "the dark cells are washing out".
 *
 * ## What it does NOT do
 *
 * It does not decode. A green verdict means the geometry is workable, not that a transfer will
 * succeed — `Fd`, `Pc` and the density limit are all still unmeasured on real hardware.
 */
class AimActivity : AppCompatActivity() {

    private var recorder: CameraRecorder? = null
    private var view: AimView? = null
    private var worker: Thread? = null

    // Held as fields because the camera cannot be opened until the preview surface exists, so setup
    // is split across `onCreate` and the surface callback and these outlive the first of them.
    private var gridCols = 120
    private var gridRows = 260
    private var captureMaxWidth = 1920
    private var previewSurface: android.view.Surface? = null
    private var started = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The receiver's own screen must stay awake, and camera access is refused to a background
        // process — an activity launched onto a locked device is exactly that (F33).
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        gridCols = intent.getIntExtra("gridCols", 120)
        gridRows = intent.getIntExtra("gridRows", 260)
        captureMaxWidth = intent.getIntExtra("maxWidth", 1920)

        val v = AimView(this, gridCols, gridRows)
        view = v
        // Live preview behind the analysis overlay. Without it the screen shows what is wrong but
        // not where the camera is pointing, which is not something an operator can aim with (F37).
        val preview = SurfaceView(this)
        setContentView(FrameLayout(this).apply {
            addView(preview, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            addView(v, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        })
        preview.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (!started) { started = true; startAnalysing(holder.surface) }
            }
            override fun surfaceChanged(h: SurfaceHolder, fmt: Int, w: Int, ht: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) {}
        })
    }

    private fun startAnalysing(surface: android.view.Surface) {
        previewSurface = surface
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA)
            view?.setMessage("Waiting for camera permission…")
            return
        }
        start(gridCols, gridRows, captureMaxWidth)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_CAMERA &&
            grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED
        ) {
            start(
                intent.getIntExtra("gridCols", 120),
                intent.getIntExtra("gridRows", 260),
                intent.getIntExtra("maxWidth", 1920),
            )
        } else {
            view?.setMessage("Camera permission denied — nothing can be measured without it.")
        }
    }

    private fun start(cols: Int, rows: Int, maxWidth: Int) {
        val analyser = AimAnalyser()
        val rec = CameraRecorder(this)
        recorder = rec

        worker = Thread {
            // Analysis is throttled rather than run on every frame: guidance a user can act on does
            // not need 60 Hz, and a full-frame pass at 60 Hz would compete with the capture it is
            // meant to be advising about.
            var lastAnalysisMs = 0L
            var lastVerdict: AimVerdict? = null
            val outcome = rec.record(
                previewSurface = previewSurface,
                bundleDir = "${filesDir.absolutePath}/aim-discard",
                frameCount = 0,                 // unbounded: runs until the activity stops it
                targetFps = 30,
                maxWidth = maxWidth,
                notes = "aiming session — not a dataset",
                writeFrames = false,            // nothing is recorded; this is guidance only
                rig = CameraRecorder.RigMetadata(gridCols = cols, gridRows = rows),
                onFrame = { buf, w, h, stride ->
                    val now = System.currentTimeMillis()
                    if (now - lastAnalysisMs >= ANALYSE_INTERVAL_MS) {
                        lastAnalysisMs = now
                        val aim = analyser.analyse(buf, w, h, stride, cols, rows)
                        if (aim != null) {
                            // Logged on CHANGE only. A line per analysed frame would bury the
                            // transitions, and the transitions are the diagnostic: what the aim was
                            // when it stopped being workable is the question a field report asks.
                            if (aim.verdict != lastVerdict) {
                                lastVerdict = aim.verdict
                                Log.i(
                                    TAG,
                                    "%s | px/cell %.1f rot %.0f lit %.3f mid %.2f mean %.0f | %s"
                                        .format(
                                            aim.verdict, aim.pxPerCell, aim.rotationDeg,
                                            aim.litFraction, aim.midFraction, aim.meanLuminance,
                                            aim.guidance,
                                        ),
                                )
                            }
                            runOnUiThread { view?.update(aim, w, h) }
                        }
                    }
                },
            )
            if (outcome.error != null) {
                Log.e(TAG, "aim session ended: ${outcome.error}")
                runOnUiThread { view?.setMessage("Camera error: ${outcome.error}") }
            }
        }.also { it.start() }
    }

    override fun onStop() {
        super.onStop()
        recorder?.cancel()
    }

    override fun onDestroy() {
        super.onDestroy()
        recorder?.cancel()
        worker?.join(2000)
    }

    companion object {
        private const val TAG = "FileFlow.Aim"
        private const val REQ_CAMERA = 2
        private const val ANALYSE_INTERVAL_MS = 150L  // ~6 verdicts a second
    }
}
