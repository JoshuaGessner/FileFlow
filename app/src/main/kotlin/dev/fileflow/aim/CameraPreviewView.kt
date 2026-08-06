package dev.fileflow.aim

import android.content.Context
import android.graphics.Matrix
import android.graphics.RectF
import android.graphics.SurfaceTexture
import android.util.Log
import android.view.Surface
import android.view.TextureView

/**
 * A camera preview that is both correctly ORIENTED and correctly PROPORTIONED.
 *
 * ## Why a TextureView and not a SurfaceView
 *
 * A `SurfaceView` cannot rotate its content. The camera writes frames in SENSOR orientation — on a
 * rear camera that is landscape, because `SENSOR_ORIENTATION` is 90 — and a `SurfaceView` simply
 * stretches whatever buffer it holds to fill its bounds. Putting a 1920×960 landscape stream into a
 * portrait box therefore squashes it vertically, and **no amount of resizing the box can fix that**,
 * because the pixels themselves are never turned.
 *
 * Two earlier attempts failed for exactly this reason: letterboxing the container, then pinning the
 * surface's buffer size. Both changed the box and neither rotated the image, so the operator
 * reported a stretched preview all three times and was right all three times (F38, F41, F42).
 *
 * A `TextureView` renders through a transform matrix, so it can rotate and scale together — which is
 * the only arrangement that yields an image matching what the eye sees.
 *
 * ## Fit, never fill
 *
 * The transform scales to FIT, leaving letterbox bars, rather than filling and cropping. The single
 * most important thing an operator needs to see is whether the transmitting screen is running off an
 * edge of the camera's view (F33) — and that is precisely the part cropping would hide.
 */
class CameraPreviewView(context: Context) : TextureView(context) {

    private var bufferW = 0
    private var bufferH = 0
    private var sensorRotation = 0

    /** Called once the underlying texture exists and a camera can be pointed at it. */
    var onSurfaceReady: ((Surface) -> Unit)? = null

    init {
        surfaceTextureListener = object : SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(st: SurfaceTexture, w: Int, h: Int) {
                applyTransform()
                onSurfaceReady?.invoke(Surface(st))
            }

            override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, w: Int, h: Int) {
                applyTransform()
            }

            override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean = true
            override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
        }
    }

    /**
     * Tell the view the camera's frame geometry, in SENSOR orientation, and how far that is rotated
     * from display orientation. Known only once frames arrive, so this is called from the frame
     * callback rather than at construction.
     */
    fun setCameraGeometry(frameW: Int, frameH: Int, rotationDeg: Int) {
        if (frameW <= 0 || frameH <= 0) return
        if (frameW == bufferW && frameH == bufferH && rotationDeg == sensorRotation) return
        bufferW = frameW
        bufferH = frameH
        sensorRotation = ((rotationDeg % 360) + 360) % 360
        Log.i(TAG, "preview geometry: camera ${frameW}x$frameH rot $sensorRotation")
        // The texture is written from the camera thread, so make sure the transform lands on the UI
        // thread that owns the view.
        post { applyTransform() }
    }

    /** The rectangle, in this view's coordinates, that the image actually occupies after fitting. */
    fun contentRect(): RectF {
        val vw = width.toFloat()
        val vh = height.toFloat()
        if (bufferW <= 0 || bufferH <= 0 || vw <= 0f || vh <= 0f) return RectF(0f, 0f, vw, vh)
        // Same rotation decision as the transform, or the overlay registers to a rectangle the
        // image does not occupy.
        val rot = displayRotationDeg()
        val quarter = rot == 90 || rot == 270
        val dw = (if (quarter) bufferH else bufferW).toFloat()
        val dh = (if (quarter) bufferW else bufferH).toFloat()
        val scale = minOf(vw / dw, vh / dh)
        val outW = dw * scale
        val outH = dh * scale
        val left = (vw - outW) / 2f
        val top = (vh - outH) / 2f
        return RectF(left, top, left + outW, top + outH)
    }

    /**
     * Degrees to rotate the buffer CLOCKWISE to bring it upright. Always upright: there is no
     * device-rotation term and no operator override.
     *
     * `SENSOR_ORIENTATION` is how far the sensor is mounted clockwise relative to the device's
     * natural orientation, so exactly that much rotation undoes it. The sign is the part worth
     * stating: rotating by MINUS it leaves the image lying on its side, which is what the first
     * version did (F42).
     *
     * An earlier version also subtracted the display's rotation, so that turning the phone sideways
     * would keep the preview upright. That term is gone, for two reasons:
     *
     *  - Every activity is `screenOrientation="portrait"` in the manifest, so the term is zero by
     *    construction. What a portrait-locked activity reports for `display.rotation` when the
     *    device is physically turned is not consistent across vendors, so reading it introduced a
     *    device-dependent quarter-turn in exchange for nothing.
     *  - [AimView] rotates its overlay by `sensorRotation` ALONE. Any term present here and absent
     *    there desynchronises the guidance boxes from the image they are drawn over. Making both
     *    the same expression means they cannot disagree.
     *
     * Aiming is done by lining the two phones up in a fixed portrait attitude, so an upright lock is
     * what the task actually wants (operator instruction, 2026-08-06).
     */
    private fun displayRotationDeg(): Int = sensorRotation

    private fun applyTransform() {
        val vw = width.toFloat()
        val vh = height.toFloat()
        if (bufferW <= 0 || bufferH <= 0 || vw <= 0f || vh <= 0f) return

        val rot = displayRotationDeg()
        val quarter = rot == 90 || rot == 270

        // A TextureView first stretches the buffer to fill the view, then applies this matrix. So the
        // matrix has to undo that stretch, rotate, and rescale to fit -- all about the view's centre.
        val cx = vw / 2f
        val cy = vh / 2f
        val m = Matrix()

        // 1. Undo the fill-stretch by mapping the view rect onto a centred rect of the buffer's own
        //    proportions.
        val viewRect = RectF(0f, 0f, vw, vh)
        val bufRect = RectF(0f, 0f, bufferW.toFloat(), bufferH.toFloat())
        bufRect.offset(cx - bufRect.centerX(), cy - bufRect.centerY())
        m.setRectToRect(viewRect, bufRect, Matrix.ScaleToFit.FILL)

        // 2. Rotate CLOCKWISE to undo the sensor's mounting.
        m.postRotate(rot.toFloat(), cx, cy)

        // 3. Scale to FIT. One uniform factor: separate x and y factors are exactly the squash this
        //    class exists to remove.
        val dw = (if (quarter) bufferH else bufferW).toFloat()
        val dh = (if (quarter) bufferW else bufferH).toFloat()
        val scale = minOf(vw / dw, vh / dh)
        m.postScale(scale, scale, cx, cy)

        Log.i(TAG, "transform: buffer ${bufferW}x$bufferH sensor $sensorRotation " +
                   "device ${display?.rotation} -> rotate $rot, scale %.3f, view ${vw.toInt()}x${vh.toInt()}"
                       .format(scale))
        setTransform(m)
    }

    companion object {
        private const val TAG = "FileFlow.Aim"
    }
}
