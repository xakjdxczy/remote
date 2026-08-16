package com.dustx.remotedesk

import java.security.SecureRandom

/** Shared, observable host state surfaced to the UI. */
object HostState {
    @Volatile var status: String = "未连接"
    @Volatile var deviceId: String? = null
    @Volatile var password: String? = null

    @Volatile var listener: (() -> Unit)? = null

    fun set(status: String? = null, deviceId: String? = null, password: String? = null) {
        if (status != null) this.status = status
        if (deviceId != null) this.deviceId = deviceId
        if (password != null) this.password = password
        listener?.invoke()
    }
}

object Ids {
    private const val PW = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789"
    private val rnd = SecureRandom()

    fun genPassword(len: Int = 8): String {
        val sb = StringBuilder(len)
        repeat(len) { sb.append(PW[rnd.nextInt(PW.length)]) }
        return sb.toString()
    }

    fun formatId(id: String?): String {
        val d = (id ?: "").filter { it.isDigit() }
        return if (d.length == 9) "${d.substring(0, 3)} ${d.substring(3, 6)} ${d.substring(6, 9)}" else (id ?: "— — —")
    }
}
