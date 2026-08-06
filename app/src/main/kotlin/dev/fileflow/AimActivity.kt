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
import android.view.Gravity
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
import dev.fileflow.aim.CameraPreviewView
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
    private var previewView: CameraPreviewView? = null
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
        val preview = CameraPreviewView(this)
        // The preview is LETTERBOXED, not stretched.
        //
        // A rear camera delivers landscape frames and the phone is held portrait, so filling the
        // view squashed the stream vertically. Aiming is a visual feedback loop -- the operator
        // moves the phone and judges the result -- so a preview with the wrong proportions is an
        // actively misleading instrument, not merely an ugly one: a screen that looks centred is
        // not centred (F38).
        // Both views fill the screen; the PREVIEW letterboxes its own image via its transform, and
        // the overlay is told the resulting content rectangle so it draws onto the image rather than
        // beside it.
        //
        // The container that used to letterbox the preview is gone. It could size a box but never
        // turn the pixels, so a landscape sensor stream stayed squashed inside a portrait box no
        // matter what shape the box was -- which is why the operator reported the same stretch after
        // two separate "fixes" (F42).
        setContentView(FrameLayout(this).apply {
            setBackgroundColor(android.graphics.Color.BLACK)
            addView(preview, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
            addView(v, FrameLayout.LayoutParams(MATCH_PARENT, MATCH_PARENT))
        })
        preview.onSurfaceReady = { surface ->
            if (!started) { started = true; startAnalysing(surface) }
        }
        previewView = preview
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
            // A focus override, so focus can be SWEPT against the metric that actually matters.
            //
            // The camera's own autofocus optimises contrast for a general scene. What decides
            // whether this channel works is the separation between the two luminance levels, and
            // those are not the same objective: AF converged on a value that looked plausible and
            // gave a level separation of 5.1 against a threshold of 12, i.e. unreadable. Sweeping
            // this and watching the reported separation finds the focus this task wants rather than
            // the one a photograph wants.
            val focusOverride = intent.getFloatExtra("focusDiopters", -1f)
            val outcome = rec.record(
                focusDiopters = focusOverride,
                previewSurface = previewSurface,
                bundleDir = "${filesDir.absolutePath}/aim-discard",
                frameCount = 0,                 // unbounded: runs until the activity stops it
                targetFps = 30,
                maxWidth = maxWidth,
                notes = "aiming session — not a dataset",
                writeFrames = false,            // nothing is recorded; this is guidance only
                rig = CameraRecorder.RigMetadata(gridCols = cols, gridRows = rows),
                onFrame = { buf, w, h, stride ->
                    // Known only once the camera is open, so it is picked up here rather than at
                    // construction. Idempotent and cheap; the view repaints only when it changes.
                    view?.let { if (it.sensorRotation != rec.sensorOrientation) {
                        it.sensorRotation = rec.sensorOrientation
                    } }
                    // Frame dimensions are known only once frames arrive, so the preview's shape is
                    // corrected here rather than guessed at layout time.
                    previewView?.let { pv ->
                        runOnUiThread {
                            // Rotation AND proportions, in one place: a SurfaceView could do neither
                            // (F42).
                            pv.setCameraGeometry(w, h, rec.sensorOrientation)
                            view?.contentRect = pv.contentRect()
                        }
                    }
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
                                    // Parenthesised. Without them `.format` binds to the SECOND
                                    // literal alone, so the arguments line up against "sep %.0f |
                                    // %s" and the verdict is handed to a float specifier -- which
                                    // threw IllegalFormatConversionException on the camera thread
                                    // and killed the app on every launch.
                                    ("%s | px/cell %.1f rot %.0f lit %.3f mid %.2f mean %.0f " +
                                     "sep %.0f | %s")
                                        .format(
                                            aim.verdict, aim.pxPerCell, aim.rotationDeg,
                                            aim.litFraction, aim.midFraction, aim.meanLuminance,
                                            aim.levelSeparation, aim.guidance,
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
