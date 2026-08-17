package com.dustx.remotedesk

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import org.webrtc.PeerConnection
import org.webrtc.RTCStatsCollectorCallback
import org.webrtc.ScreenCapturerAndroid
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoSource
import org.webrtc.VideoTrack

/**
 * Foreground service: captures the screen as a WebRTC video track
 * (ScreenCapturerAndroid + hardware encoder) and runs signaling + the peer.
 * Traffic (speed / session / historical) and latency come from getStats().
 */
class ScreenCaptureService : Service(), SignalingClient.Callbacks {

    private var capturer: ScreenCapturerAndroid? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var surfaceHelper: SurfaceTextureHelper? = null

    private var signaling: SignalingClient? = null
    private var host: WebRtcHost? = null

    private var statsThread: HandlerThread? = null
    private var statsHandler: Handler? = null

    private var deviceW = 0
    private var deviceH = 0
    private var capW = 0
    private var capH = 0
    private val fps = 30

    private var lastSent = 0L
    private var lastAt = 0L
    private var totalPersisted = 0L

    private val stunUrls = listOf(
        "stun:stun.l.google.com:19302",
        "stun:stun.qq.com:3478",
        "stun:stun.miwifi.com:3478",
        "stun:stun.cloudflare.com:3478",
    )
    @Volatile private var iceServers: List<PeerConnection.IceServer> = defaultIceServers()

    private fun defaultIceServers(): List<PeerConnection.IceServer> =
        listOf(PeerConnection.IceServer.builder(stunUrls).createIceServer())

    private fun fetchIceServers(wsUrl: String): List<PeerConnection.IceServer> {
        return try {
            val base = wsUrl.replace("wss://", "https://").replace("ws://", "http://").removeSuffix("/ws")
            val body = OkHttpClient().newCall(Request.Builder().url("$base/api/config").build())
                .execute().use { it.body?.string() } ?: return defaultIceServers()
            val arr = JSONObject(body).optJSONArray("ice_servers") ?: return defaultIceServers()
            val out = ArrayList<PeerConnection.IceServer>()
            for (i in 0 until arr.length()) {
                val s = arr.getJSONObject(i)
                val urls = ArrayList<String>()
                when (val u = s.opt("urls")) {
                    is JSONArray -> for (j in 0 until u.length()) urls.add(u.getString(j))
                    is String -> urls.add(u)
                }
                if (urls.isEmpty()) continue
                val b = PeerConnection.IceServer.builder(urls)
                s.optString("username").takeIf { it.isNotEmpty() }?.let { b.setUsername(it) }
                s.optString("credential").takeIf { it.isNotEmpty() }?.let { b.setPassword(it) }
                out.add(b.createIceServer())
            }
            if (out.isEmpty()) defaultIceServers() else out
        } catch (e: Exception) {
            Log.w(TAG, "fetch ice servers failed: ${e.message}")
            defaultIceServers()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent == null) { stopSelf(); return START_NOT_STICKY }
        startForegroundInternal()

        val data = intent.getParcelableExtra<Intent>(EXTRA_DATA)
        val url = intent.getStringExtra(EXTRA_URL) ?: DEFAULT_URL
        if (data == null) { stopSelf(); return START_NOT_STICKY }

        totalPersisted = getSharedPreferences("rd_traffic", Context.MODE_PRIVATE).getLong("total_bytes", 0L)
        computeDimensions()

        Thread { iceServers = fetchIceServers(url) }.start()
        startVideoCapture(data)
        startSignaling(url)
        startStatsLoop()
        HostState.set(status = "正在连接信令…")
        return START_STICKY
    }

    private fun computeDimensions() {
        val dm: DisplayMetrics = resources.displayMetrics
        deviceW = dm.widthPixels
        deviceH = dm.heightPixels
        // 540p keeps latency/bandwidth low while staying legible for control.
        val maxW = 540
        if (deviceW <= maxW) { capW = deviceW; capH = deviceH }
        else { capW = maxW; capH = (deviceH.toLong() * maxW / deviceW).toInt() }
        if (capW % 2 == 1) capW -= 1
        if (capH % 2 == 1) capH -= 1
    }

    private fun startVideoCapture(data: Intent) {
        val factory = WebRtcHost.ensureFactory(applicationContext)
        val egl = WebRtcHost.egl()
        val cap = ScreenCapturerAndroid(data, object : MediaProjection.Callback() {
            override fun onStop() { stopSelf() }
        })
        capturer = cap
        val src = factory.createVideoSource(true) // isScreencast
        videoSource = src
        // Lock the output format so the encoder keeps capW x capH @ fps (smoother).
        src.adaptOutputFormat(capW, capH, fps)
        surfaceHelper = SurfaceTextureHelper.create("rd-capture", egl.eglBaseContext)
        cap.initialize(surfaceHelper, applicationContext, src.capturerObserver)
        cap.startCapture(capW, capH, fps)
        videoTrack = factory.createVideoTrack("screen", src)
    }

    private fun startSignaling(url: String) {
        val sig = SignalingClient(url, Build.MODEL ?: "Android", "Android ${Build.VERSION.RELEASE}", this)
        signaling = sig
        sig.connect()
    }

    private fun startStatsLoop() {
        statsThread = HandlerThread("rd-stats").also { it.start() }
        statsHandler = Handler(statsThread!!.looper)
        statsHandler?.postDelayed(object : Runnable {
            override fun run() {
                val h = host
                if (h != null) h.stats(statsCallback)
                statsHandler?.postDelayed(this, 1500)
            }
        }, 1500)
    }

    private val statsCallback = RTCStatsCollectorCallback { report ->
        var sent = 0L
        var rttMs = -1
        for (s in report.statsMap.values) {
            when (s.type) {
                "outbound-rtp", "data-channel" -> {
                    (s.members["bytesSent"] as? Number)?.let { sent += it.toLong() }
                }
                "candidate-pair" -> {
                    val nominated = (s.members["nominated"] as? Boolean) ?: false
                    if (nominated) {
                        (s.members["currentRoundTripTime"] as? Number)?.let { rttMs = (it.toDouble() * 1000).toInt() }
                    }
                }
            }
        }
        val now = System.currentTimeMillis()
        if (lastSent > 0 && sent >= lastSent) {
            val dt = (now - lastAt) / 1000.0
            val delta = sent - lastSent
            totalPersisted += delta
            getSharedPreferences("rd_traffic", Context.MODE_PRIVATE).edit()
                .putLong("total_bytes", totalPersisted).apply()
            val speed = if (dt > 0) (delta / dt) else 0.0
            HostState.setStats(fmtSpeed(speed), sent, totalPersisted, rttMs)
        }
        lastSent = sent
        lastAt = now
    }

    private fun fmtSpeed(bytesPerSec: Double): String {
        val bits = bytesPerSec * 8
        return if (bits < 1e6) String.format("%.0f Kbps", bits / 1e3) else String.format("%.2f Mbps", bits / 1e6)
    }

    // ---- SignalingClient.Callbacks --------------------------------------
    override fun onRegistered(deviceId: String, password: String) {
        HostState.set(status = "在线，等待连接", deviceId = deviceId, password = password)
    }

    override fun onSessionStart(msg: JSONObject) {
        host?.close()
        lastSent = 0
        val h = WebRtcHost(applicationContext, signaling!!, iceServers, videoTrack, capW, capH, deviceW, deviceH)
        host = h
        h.start()
        HostState.set(status = "有人接入，正在建立 P2P…")
    }

    override fun onSignal(msg: JSONObject) { host?.onSignal(msg) }

    override fun onSessionEnd(reason: String) {
        host?.close(); host = null
        HostState.set(status = "在线，等待连接")
    }

    override fun onClosed() { HostState.set(status = "信令断开") }

    private fun startForegroundInternal() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(NotificationChannel(CHANNEL, "远程被控", NotificationManager.IMPORTANCE_LOW))
        }
        val notif: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("尘埃X 远程被控运行中")
            .setContentText("本机屏幕可被授权的控制端查看/操作")
            .setSmallIcon(R.drawable.ic_logo)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    override fun onDestroy() {
        try { statsThread?.quitSafely() } catch (_: Exception) {}
        try { signaling?.close() } catch (_: Exception) {}
        try { host?.close() } catch (_: Exception) {}
        try { capturer?.stopCapture() } catch (_: Exception) {}
        try { capturer?.dispose() } catch (_: Exception) {}
        try { videoSource?.dispose() } catch (_: Exception) {}
        try { surfaceHelper?.dispose() } catch (_: Exception) {}
        HostState.set(status = "未连接", deviceId = "", password = "")
        super.onDestroy()
    }

    companion object {
        private const val TAG = "RD.Capture"
        private const val CHANNEL = "rd_capture"
        private const val NOTIF_ID = 1001
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_DATA = "data"
        const val EXTRA_URL = "url"
        const val DEFAULT_URL = "wss://117.72.108.246/ws"
    }
}
