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
import android.os.Looper
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
 * (ScreenCapturerAndroid + hardware encoder). Signaling stays in KeepAliveService
 * so going to the background does not drop the remote-code socket.
 * Traffic (speed / session / historical) and latency come from getStats().
 */
class ScreenCaptureService : Service() {

    private var capturer: ScreenCapturerAndroid? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var surfaceHelper: SurfaceTextureHelper? = null

    private var host: WebRtcHost? = null
    var screenTrack: org.webrtc.VideoTrack? = null
        private set

    private var statsThread: HandlerThread? = null
    private var statsHandler: Handler? = null
    private val mainHandler = Handler(Looper.getMainLooper())
    private var sceneWatch: SceneWatchdog? = null
    private var bursting = false

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

        instance = this
        Thread {
            iceServers = fetchIceServers(url)
            HostState.iceServers = iceServers
        }.start()
        startVideoCapture(data)
        if (HostState.signaling == null) KeepAliveService.start(this, url)
        startStatsLoop()
        if (HostState.pendingHostAttach) {
            HostState.pendingHostAttach = false
            attachHost()
        }
        HostState.set(status = "录屏已就绪，等待连接")
        return START_STICKY
    }

    fun attachHost() {
        host?.close()
        lastSent = 0
        val track = videoTrack
        screenTrack = track
        val h = WebRtcHost(
            applicationContext, HostState.signaling ?: return,
            if (HostState.iceServers.isNotEmpty()) HostState.iceServers else iceServers,
            track, capW, capH, deviceW, deviceH,
            onNeedKeyframe = { burstThenRestore() },
        )
        host = h
        HostState.hostPeer = h
        h.start()
        HostState.set(status = "有人接入，正在建立 P2P…")
    }

    private fun computeDimensions() {
        val dm: DisplayMetrics = resources.displayMetrics
        deviceW = dm.widthPixels
        deviceH = dm.heightPixels
        // 540p: keeps frames + keyframes small enough to fit a mobile uplink so
        // complex/animated screens don't burst past the link and collapse. Data
        // showed 720p interactive bursts caused heavy packet loss + keyframe
        // death-spiral; 540p recovers fast. Smoothness kept via 30fps + H.264.
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
        screenTrack = videoTrack
        val watch = SceneWatchdog { mainHandler.post { burstThenRestore() } }
        sceneWatch = watch
        videoTrack?.addSink(watch)
    }

    /** Drop to a budget-sized I-frame, then step sharpness back. */
    fun burstThenRestore() {
        if (bursting) return
        bursting = true
        val bps = host?.currentBitrateBps ?: WebRtcHost.TURN_MAX_BPS
        val (bw, bh, scale) = Latency.sceneScaleSize(capW, capH, Latency.keyframeBudgetBytes(bps))
        val useW = if (scale < 0.98f) bw else maxOf(160, capW / 2).let { it - it % 2 }
        val useH = if (scale < 0.98f) bh else maxOf(90, capH / 2).let { it - it % 2 }
        applyCaptureSize(useW, useH)
        host?.notifySceneChange()
        val midW = Latency.evenDim((useW + capW) / 2)
        val midH = Latency.evenDim((useH + capH) / 2)
        mainHandler.postDelayed({ applyCaptureSize(midW, midH) }, Latency.SCENE_STEP_MS)
        mainHandler.postDelayed({
            applyCaptureSize(capW, capH)
            bursting = false
        }, Latency.SCENE_RESTORE_MS)
    }

    private fun applyCaptureSize(w: Int, h: Int) {
        try { videoSource?.adaptOutputFormat(w, h, fps) } catch (e: Exception) {
            Log.w(TAG, "adapt $w x $h: ${e.message}")
        }
        try { capturer?.changeCaptureFormat(w, h, fps) } catch (e: Exception) {
            Log.w(TAG, "recapture $w x $h: ${e.message}")
        }
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
        val codecs = HashMap<String, String>()
        var outCodec: String? = null
        var proto: String? = null
        var pairId: String? = null
        for (s in report.statsMap.values) {
            if (s.type == "codec") {
                (s.members["mimeType"] as? String)?.let { codecs[s.id] = it.substringAfter("/") }
            }
            if (s.type == "transport") {
                pairId = s.members["selectedCandidatePairId"] as? String
            }
        }
        for (s in report.statsMap.values) {
            when (s.type) {
                "outbound-rtp", "data-channel" -> {
                    (s.members["bytesSent"] as? Number)?.let { sent += it.toLong() }
                    if (s.type == "outbound-rtp") {
                        val cid = s.members["codecId"] as? String
                        if (cid != null) outCodec = codecs[cid]
                    }
                }
                "candidate-pair" -> {
                    val nominated = (s.members["nominated"] as? Boolean) ?: false
                    if (nominated || s.id == pairId) {
                        (s.members["currentRoundTripTime"] as? Number)?.let { rttMs = (it.toDouble() * 1000).toInt() }
                    }
                }
                "local-candidate", "remote-candidate" -> {
                    val p = (s.members["relayProtocol"] as? String) ?: (s.members["protocol"] as? String)
                    if (p != null) proto = p.uppercase()
                }
            }
        }
        if (outCodec != null || proto != null) {
            HostState.setMedia(codec = outCodec?.let { "编码 $it" }, proto = proto)
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
        sceneWatch?.let { try { videoTrack?.removeSink(it) } catch (_: Exception) {} }
        sceneWatch = null
        mainHandler.removeCallbacksAndMessages(null)
        bursting = false
        try { host?.close() } catch (_: Exception) {}
        try { capturer?.stopCapture() } catch (_: Exception) {}
        try { capturer?.dispose() } catch (_: Exception) {}
        try { videoSource?.dispose() } catch (_: Exception) {}
        try { surfaceHelper?.dispose() } catch (_: Exception) {}
        if (instance === this) instance = null
        HostState.hostPeer = null
        HostState.set(status = "在线，等待连接")
        super.onDestroy()
    }

    companion object {
        @Volatile var instance: ScreenCaptureService? = null
        private const val TAG = "RD.Capture"
        private const val CHANNEL = "rd_capture"
        private const val NOTIF_ID = 1001
        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_DATA = "data"
        const val EXTRA_URL = "url"
        const val DEFAULT_URL = "wss://loessx.com/ws"
    }
}
