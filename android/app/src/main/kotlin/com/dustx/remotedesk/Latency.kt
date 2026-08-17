package com.dustx.remotedesk

import kotlin.math.abs
import kotlin.math.sqrt

/** Mirrors ``remote.latency`` so Android sizes switch I-frames the same way. */
object Latency {
    const val KEYFRAME_BUDGET_MS = 100
    const val KEYFRAME_BPP = 0.75
    const val SCENE_DIFF = 0.16f
    const val SCENE_STEP_MS = 200L
    const val SCENE_RESTORE_MS = 350L
    const val LUMA_W = 32
    const val LUMA_H = 18

    fun keyframeBudgetBytes(bitrateBps: Int): Int =
        maxOf(1024, bitrateBps * KEYFRAME_BUDGET_MS / 1000 / 8)

    fun evenDim(n: Int): Int {
        val v = maxOf(2, n)
        return v - (v % 2)
    }

    fun sceneScaleSize(width: Int, height: Int, budgetBytes: Int): Triple<Int, Int, Float> {
        val raw = width * height * KEYFRAME_BPP / 8.0
        if (raw <= budgetBytes) return Triple(evenDim(width), evenDim(height), 1f)
        val scale = sqrt(budgetBytes / raw).toFloat().coerceIn(0.35f, 1f)
        return Triple(evenDim((width * scale).toInt()), evenDim((height * scale).toInt()), scale)
    }

    fun lumaDiff(prev: IntArray, curr: IntArray): Float {
        if (prev.size != curr.size || prev.isEmpty()) return 1f
        var acc = 0
        for (i in prev.indices) acc += abs(prev[i] - curr[i])
        return acc / (prev.size * 255f)
    }
}
