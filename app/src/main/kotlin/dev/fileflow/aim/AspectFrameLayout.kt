package dev.fileflow.aim

import android.content.Context
import android.util.Log
import android.view.SurfaceView
import android.widget.FrameLayout

/**
 * A container that keeps its child at a fixed aspect ratio, letterboxing rather than stretching.
 *
 * ## Why this exists
 *
 * The camera preview was a `SurfaceView` at `MATCH_PARENT` in both axes. A rear camera delivers
 * LANDSCAPE frames (e.g. 2688×1512) and the phone is held PORTRAIT, so the stream was being squashed
 * into a tall box — reported as "stretched up and down and impossible to line up", which is exactly
 * what it was (F38).
 *
 * That is worse than ugly. Aiming is a visual feedback loop: the operator moves the phone and judges
 * the result. If the preview's proportions do not match reality, the judgement is wrong — a screen
 * that looks centred is not centred, and one that looks square-on is not. **A distorted preview is
 * an actively misleading instrument**, and the operator was being asked to line up a rig with it.
 *
 * ## Why the ratio is expressed in display orientation
 *
 * [setAspect] takes the camera's frame dimensions and its `SENSOR_ORIENTATION`, and swaps the axes
 * on a quarter-turn. Callers therefore pass what the camera reports, not a pre-rotated guess — the
 * rotation logic lives in one place rather than at each call site, which is where it would drift.
 */
class AspectFrameLayout(context: Context) : FrameLayout(context) {

    /** Width ÷ height, in DISPLAY orientation. 0 means "no constraint yet". */
    private var aspect = 0f

    /**
     * @param frameW camera frame width, in sensor orientation
     * @param frameH camera frame height, in sensor orientation
     * @param sensorRotation `SENSOR_ORIENTATION`, clockwise degrees
     */
    fun setAspect(frameW: Int, frameH: Int, sensorRotation: Int) {
        if (frameW <= 0 || frameH <= 0) return
        val quarter = sensorRotation == 90 || sensorRotation == 270
        val w = if (quarter) frameH else frameW
        val h = if (quarter) frameW else frameH
        val a = w.toFloat() / h.toFloat()
        if (kotlin.math.abs(a - aspect) > 1e-4f) {
            aspect = a
            Log.i(TAG, "aspect := %.4f from camera %dx%d rot %d (display %dx%d)"
                .format(a, frameW, frameH, sensorRotation, w, h))
            // Pin the child surface's BUFFER to the camera's display-oriented geometry.
            //
            // Sizing this container correctly is not sufficient on its own: a `SurfaceView` scales
            // whatever buffer it holds to fill its bounds, and the buffer was created at the view's
            // original (full-portrait) size before any frame had arrived to reveal the camera's
            // aspect. Fixing the buffer makes the two agree, so the stream is letterboxed rather
            // than stretched even on the first layout pass.
            for (i in 0 until childCount) {
                (getChildAt(i) as? SurfaceView)?.holder?.setFixedSize(w, h)
            }
            requestLayout()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        if (aspect <= 0f) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            return
        }
        val availW = MeasureSpec.getSize(widthMeasureSpec)
        val availH = MeasureSpec.getSize(heightMeasureSpec)
        if (availW <= 0 || availH <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            return
        }

        // Fit INSIDE the available space rather than filling it. Cropping would hide part of the
        // camera's view, and the one thing the operator most needs to see is whether the screen is
        // running off an edge — which is precisely what would be cropped away.
        var w = availW
        var h = (w / aspect).toInt()
        if (h > availH) {
            h = availH
            w = (h * aspect).toInt()
        }
        super.onMeasure(
            MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
            MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY),
        )
    }

    companion object {
        private const val TAG = "FileFlow.Aim"
    }
}
