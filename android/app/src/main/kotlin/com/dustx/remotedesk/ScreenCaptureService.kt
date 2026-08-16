package com.dustx.remotedesk

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
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
import java.io.ByteArrayOutputStream

/** Foreground service: screen capture via MediaProjection + signaling + WebRTC. */
class ScreenCaptureService : Service(), SignalingClient.Callbacks {

    private var projection: MediaProjection? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var imageReader: ImageReader? = null
    private var captureThread: HandlerThread? = null
    private var captureHandler: Handler? = null

    private var signaling: SignalingClient? = null
    private var host: WebRtcHost? = null

    private var deviceW = 0
    private var deviceH = 0
    private var capW = 0
    private var capH = 0
    private var densityDpi = 320
    private var lastSent = 0L
    private val frameIntervalMs = 80L // ~12fps
    private val quality = 45

    private val stunUrls = listOf(
        "stun:stun.l.google.com:19302",
        "stun:stun.qq.com:3478",
        "stun:stun.miwifi.com:3478",
        "stun:stun.cloudflare.com:3478",
    )
    @Volatile private var iceServers: List<PeerConnection.IceServer> = defaultIceServers()

    private fun defaultIceServers(): List<PeerConnection.IceServer> =
        listOf(PeerConnection.IceServer.builder(stunUrls).createIceServer())

    /** Fetch ice_servers (incl. optional TURN) from the signaling server's /api/config. */
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

        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
        val data = intent.getParcelableExtra<Intent>(EXTRA_DATA)
        val url = intent.getStringExtra(EXTRA_URL) ?: DEFAULT_URL
        if (data == null) { stopSelf(); return START_NOT_STICKY }

        computeDimensions()
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mpm.getMediaProjection(resultCode, data)
        projection?.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() { stopSelf() }
        }, Handler(mainLooper))

        Thread { iceServers = fetchIceServers(url) }.start()
        startCapture()
        startSignaling(url)
        HostState.set(status = "正在连接信令…")
        return START_STICKY
    }

    private fun computeDimensions() {
        val dm: DisplayMetrics = resources.displayMetrics
        deviceW = dm.widthPixels
        deviceH = dm.heightPixels
        densityDpi = dm.densityDpi
        val maxW = 720
        if (deviceW <= maxW) {
            capW = deviceW; capH = deviceH
        } else {
            capW = maxW
            capH = (deviceH.toLong() * maxW / deviceW).toInt()
        }
        if (capW % 2 == 1) capW -= 1
        if (capH % 2 == 1) capH -= 1
    }

    private fun startCapture() {
        captureThread = HandlerThread("rd-capture").also { it.start() }
        captureHandler = Handler(captureThread!!.looper)
        imageReader = ImageReader.newInstance(capW, capH, PixelFormat.RGBA_8888, 2)
        virtualDisplay = projection?.createVirtualDisplay(
            "rd-cap", capW, capH, densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            imageReader!!.surface, null, captureHandler
        )
        imageReader!!.setOnImageAvailableListener({ reader -> onFrame(reader) }, captureHandler)
    }

    private fun onFrame(reader: ImageReader) {
        val image = reader.acquireLatestImage() ?: return
        try {
            val now = System.currentTimeMillis()
            val h = host
            if (h == null || !h.isOpen || now - lastSent < frameIntervalMs) return
            lastSent = now
            val plane = image.planes[0]
            val buffer = plane.buffer
            val pixelStride = plane.pixelStride
            val rowStride = plane.rowStride
            val rowPadding = rowStride - pixelStride * capW
            val bmpW = capW + rowPadding / pixelStride
            val bmp = Bitmap.createBitmap(bmpW, capH, Bitmap.Config.ARGB_8888)
            bmp.copyPixelsFromBuffer(buffer)
            val out = if (bmpW != capW) Bitmap.createBitmap(bmp, 0, 0, capW, capH) else bmp
            val baos = ByteArrayOutputStream()
            out.compress(Bitmap.CompressFormat.JPEG, quality, baos)
            if (out !== bmp) out.recycle()
            bmp.recycle()
            h.sendFrame(Protocol.packFrame(baos.toByteArray(), capW, capH, now))
        } catch (e: Exception) {
            Log.w(TAG, "frame error: ${e.message}")
        } finally {
            image.close()
        }
    }

    private fun startSignaling(url: String) {
        val sig = SignalingClient(url, Build.MODEL ?: "Android", "Android ${Build.VERSION.RELEASE}", this)
        signaling = sig
        sig.connect()
    }

    // ---- SignalingClient.Callbacks --------------------------------------
    override fun onRegistered(deviceId: String, password: String) {
        HostState.set(status = "在线，等待连接", deviceId = deviceId, password = password)
    }

    override fun onSessionStart(msg: JSONObject) {
        host?.close()
        val h = WebRtcHost(applicationContext, signaling!!, iceServers, capW, capH, deviceW, deviceH)
        host = h
        h.start()
        HostState.set(status = "有人接入，正在建立 P2P…")
    }

    override fun onSignal(msg: JSONObject) {
        host?.onSignal(msg)
    }

    override fun onSessionEnd(reason: String) {
        host?.close(); host = null
        HostState.set(status = "在线，等待连接")
    }

    override fun onClosed() {
        HostState.set(status = "信令断开")
    }

    private fun startForegroundInternal() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL, "远程被控", NotificationManager.IMPORTANCE_LOW)
            mgr.createNotificationChannel(ch)
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
        try { signaling?.close() } catch (_: Exception) {}
        try { host?.close() } catch (_: Exception) {}
        try { virtualDisplay?.release() } catch (_: Exception) {}
        try { imageReader?.close() } catch (_: Exception) {}
        try { projection?.stop() } catch (_: Exception) {}
        captureThread?.quitSafely()
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
