package com.dustx.remotedesk

import org.webrtc.VideoFrame
import org.webrtc.VideoSink

/**
 * Samples the Y plane and fires [onSceneChange] when the picture jumps
 * (app / desktop switch). Cheap: 32×18 luma, at most ~11 Hz.
 */
class SceneWatchdog(private val onSceneChange: () -> Unit) : VideoSink {
    private var prev: IntArray? = null
    private var lastCheck = 0L
    private var coolUntil = 0L

    override fun onFrame(frame: VideoFrame) {
        val now = System.currentTimeMillis()
        if (now < coolUntil || now - lastCheck < 90) return
        lastCheck = now
        val i420 = try { frame.buffer.toI420() } catch (_: Exception) { null } ?: return
        try {
            val sample = sampleY(i420, Latency.LUMA_W, Latency.LUMA_H)
            val last = prev
            prev = sample
            if (last != null && Latency.lumaDiff(last, sample) >= Latency.SCENE_DIFF) {
                coolUntil = now + Latency.SCENE_RESTORE_MS
                onSceneChange()
            }
        } finally {
            i420.release()
        }
    }

    private fun sampleY(i420: VideoFrame.I420Buffer, sw: Int, sh: Int): IntArray {
        val w = i420.width
        val h = i420.height
        val y = i420.dataY.duplicate()
        val stride = i420.strideY
        val out = IntArray(sw * sh)
        var i = 0
        for (row in 0 until sh) {
            val srcRow = (row * h / sh) * stride
            for (col in 0 until sw) {
                val srcX = col * w / sw
                out[i++] = y.get(srcRow + srcX).toInt() and 0xFF
            }
        }
        return out
    }
}
