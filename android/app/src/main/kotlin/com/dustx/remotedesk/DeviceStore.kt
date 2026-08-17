package com.dustx.remotedesk

import android.content.Context

/** Persistent ToDesk-style remote code + password on this device. */
object DeviceStore {
    private fun prefs(ctx: Context) = ctx.applicationContext.getSharedPreferences("rd_device", Context.MODE_PRIVATE)

    fun deviceId(ctx: Context): String = prefs(ctx).getString("device_id", "") ?: ""

    fun password(ctx: Context): String {
        val saved = prefs(ctx).getString("password", "") ?: ""
        if (saved.isNotEmpty()) return saved
        val fresh = Ids.genPassword()
        prefs(ctx).edit().putString("password", fresh).apply()
        return fresh
    }

    fun save(ctx: Context, deviceId: String, password: String) {
        prefs(ctx).edit().putString("device_id", deviceId).putString("password", password).apply()
    }

    fun refreshSec(ctx: Context): Int = prefs(ctx).getInt("refresh_sec", 0)

    fun setRefreshSec(ctx: Context, sec: Int) {
        prefs(ctx).edit().putInt("refresh_sec", sec).apply()
    }
}
