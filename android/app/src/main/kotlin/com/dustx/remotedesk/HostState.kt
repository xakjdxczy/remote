package com.dustx.remotedesk

import java.security.SecureRandom

/** Shared, observable host state surfaced to the UI. */
object HostState {
    @Volatile var status: String = "未连接"
    @Volatile var deviceId: String? = null
    @Volatile var password: String? = null
    @Volatile var connMethod: String? = null

    // live traffic/latency stats
    @Volatile var netSpeed: String = "--"
    @Volatile var sessionBytes: Long = 0
    @Volatile var totalBytes: Long = 0
    @Volatile var latencyMs: Int = -1
    @Volatile var peerId: String = "--"
    @Volatile var codec: String = "--"
    @Volatile var proto: String = "--"
    @Volatile var incoming: org.json.JSONObject? = null
    @Volatile var incomingListener: ((org.json.JSONObject) -> Unit)? = null

    @Volatile var signaling: SignalingClient? = null
    @Volatile var hostPeer: WebRtcHost? = null
    @Volatile var viewer: WebRtcViewer? = null
    @Volatile var iceServers: List<org.webrtc.PeerConnection.IceServer> = emptyList()
    @Volatile var myDeviceId: String = ""
    @Volatile var uiResumed: Boolean = false
    @Volatile var pendingHostAttach: Boolean = false

    @Volatile var listener: (() -> Unit)? = null

    fun set(status: String? = null, deviceId: String? = null, password: String? = null) {
        if (status != null) this.status = status
        if (deviceId != null) this.deviceId = deviceId
        if (password != null) this.password = password
        listener?.invoke()
    }

    fun setStats(speed: String, session: Long, total: Long, latency: Int) {
        netSpeed = speed
        sessionBytes = session
        totalBytes = total
        latencyMs = latency
        listener?.invoke()
    }

    fun setMedia(codec: String? = null, proto: String? = null, peerId: String? = null) {
        if (codec != null) this.codec = codec
        if (proto != null) this.proto = proto
        if (peerId != null) this.peerId = peerId
        listener?.invoke()
    }

    fun fmtBytes(n: Long): String = when {
        n < 1024 -> "$n B"
        n < 1024 * 1024 -> String.format("%.1f KB", n / 1024.0)
        n < 1024L * 1024 * 1024 -> String.format("%.1f MB", n / 1024.0 / 1024)
        else -> String.format("%.2f GB", n / 1024.0 / 1024 / 1024)
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
