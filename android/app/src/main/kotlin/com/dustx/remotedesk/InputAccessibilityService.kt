package com.dustx.remotedesk

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.util.Log
import android.view.accessibility.AccessibilityEvent

/**
 * Injects taps / swipes coming from the remote viewer. Android only lets an
 * AccessibilityService with canPerformGestures synthesize touch input, so remote
 * control requires the user to enable this service once in Settings.
 */
class InputAccessibilityService : AccessibilityService() {
    override fun onServiceConnected() {
        instance = this
        Log.i(TAG, "accessibility connected")
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}
    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "RD.Access"
        @Volatile var instance: InputAccessibilityService? = null

        val isEnabled: Boolean get() = instance != null

        fun tap(x: Int, y: Int) {
            val svc = instance ?: return
            val path = Path().apply { moveTo(x.toFloat(), y.toFloat()) }
            val stroke = GestureDescription.StrokeDescription(path, 0, 60)
            svc.dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)
        }

        fun swipe(x1: Int, y1: Int, x2: Int, y2: Int, durationMs: Long = 220) {
            val svc = instance ?: return
            val path = Path().apply {
                moveTo(x1.toFloat(), y1.toFloat())
                lineTo(x2.toFloat(), y2.toFloat())
            }
            val stroke = GestureDescription.StrokeDescription(path, 0, durationMs.coerceIn(20, 2000))
            svc.dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)
        }
    }
}
