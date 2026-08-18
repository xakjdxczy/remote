package com.dustx.remotedesk

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import org.webrtc.Camera2Capturer
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.MediaConstraints
import org.webrtc.PeerConnection
import org.webrtc.SurfaceTextureHelper
import org.webrtc.AudioTrack
import org.webrtc.VideoTrack

class CameraLinkService : Service() {
    private var capturer: CameraVideoCapturer? = null
    private var helper: SurfaceTextureHelper? = null
    private var videoTrack: VideoTrack? = null
    private var audioTrack: AudioTrack? = null
    private var rtc: CameraWebRtc? = null
    private var usingFront = false
    private var muted = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_SWITCH -> {
                try { capturer?.switchCamera(null) } catch (e: Exception) {
                    Log.w(TAG, "switch: ${e.message}")
                }
                return START_STICKY
            }
            ACTION_MUTE -> {
                muted = !muted
                audioTrack?.setEnabled(!muted)
                HostState.set(status = if (muted) "已静音" else "麦克风已开")
                return START_STICKY
            }
        }
        startAsForeground()
        val host = intent?.getStringExtra(EXTRA_HOST)?.trim().orEmpty()
        val token = intent?.getStringExtra(EXTRA_TOKEN)?.trim().orEmpty()
        val usb = intent?.getBooleanExtra(EXTRA_USB, false) == true
        val front = intent?.getBooleanExtra(EXTRA_FRONT, false) == true
        val height = intent?.getIntExtra(EXTRA_HEIGHT, 720)?.takeIf { it >= 720 } ?: 720
        usingFront = front
        Thread { startCapture(host, token, usb, front, height) }.start()
        return START_STICKY
    }

    private fun startCapture(host: String, token: String, usb: Boolean, front: Boolean, height: Int) {
        try {
            val factory = WebRtcHost.ensureFactory(applicationContext)
            val enumerator = Camera2Enumerator(applicationContext)
            val name = enumerator.deviceNames.firstOrNull { enumerator.isFrontFacing(it) == front }
                ?: enumerator.deviceNames.firstOrNull()
            if (name == null) {
                HostState.set(status = "没有可用摄像头")
                stopSelf()
                return
            }
            val events = object : CameraVideoCapturer.CameraEventsHandler {
                override fun onCameraError(s: String) { HostState.set(status = "相机错误：$s") }
                override fun onCameraDisconnected() {}
                override fun onCameraFreezed(s: String) {}
                override fun onCameraOpening(s: String) {}
                override fun onFirstFrameAvailable() {}
                override fun onCameraClosed() {}
            }
            val cap = Camera2Capturer(applicationContext, name, events)
            capturer = cap
            helper = SurfaceTextureHelper.create("dust-cam", WebRtcHost.egl().eglBaseContext)
            val source = factory.createVideoSource(false)
            cap.initialize(helper, applicationContext, source.capturerObserver)
            val width = if (height >= 1080) 1920 else 1280
            cap.startCapture(width, height.coerceAtMost(1080), 30)
            val vTrack = factory.createVideoTrack("cam0", source)
            videoTrack = vTrack
            val aTrack = factory.createAudioTrack("aud0", factory.createAudioSource(MediaConstraints()))
            audioTrack = aTrack
            val ice = HostState.iceServers.ifEmpty { defaultIce() }
            val ws = buildWs(host, usb)
            val peer = CameraWebRtc(applicationContext, ice, vTrack, aTrack) { msg ->
                HostState.set(status = msg)
            }
            rtc = peer
            instance = this
            peer.connect(ws, token)
        } catch (e: Exception) {
            Log.e(TAG, "start", e)
            HostState.set(status = "启动失败：${e.message}")
            stopSelf()
        }
    }

    private fun buildWs(host: String, usb: Boolean): String {
        val raw = host.ifBlank { if (usb) "127.0.0.1" else DeviceStore.camHost(this) }
        if (raw.startsWith("ws://") || raw.startsWith("wss://")) return raw
        val hostPort = if (":" in raw) raw else "$raw:8080"
        return "ws://$hostPort/cam/ws"
    }

    private fun defaultIce(): List<PeerConnection.IceServer> =
        listOf(
            PeerConnection.IceServer.builder(
                listOf(
                    "stun:stun.l.google.com:19302",
                    "stun:stun.qq.com:3478",
                    "stun:stun.miwifi.com:3478",
                )
            ).createIceServer()
        )

    private fun startAsForeground() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "尘埃摄像头", NotificationManager.IMPORTANCE_LOW)
            )
        }
        val n = NotificationCompat.Builder(this, CHANNEL)
            .setContentTitle("尘埃X")
            .setContentText("正在作为电脑摄像头")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(
                42,
                n,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE,
            )
        } else {
            startForeground(42, n)
        }
    }

    override fun onDestroy() {
        instance = null
        try { rtc?.close() } catch (_: Exception) {}
        try { capturer?.stopCapture() } catch (_: Exception) {}
        try { capturer?.dispose() } catch (_: Exception) {}
        try { videoTrack?.dispose() } catch (_: Exception) {}
        try { audioTrack?.dispose() } catch (_: Exception) {}
        try { helper?.dispose() } catch (_: Exception) {}
        rtc = null
        capturer = null
        videoTrack = null
        audioTrack = null
        helper = null
        super.onDestroy()
    }

    companion object {
        const val CHANNEL = "dust_cam"
        const val EXTRA_HOST = "host"
        const val EXTRA_TOKEN = "token"
        const val EXTRA_USB = "usb"
        const val EXTRA_FRONT = "front"
        const val EXTRA_HEIGHT = "height"
        const val ACTION_STOP = "dust.cam.stop"
        const val ACTION_SWITCH = "dust.cam.switch"
        const val ACTION_MUTE = "dust.cam.mute"
        @Volatile var instance: CameraLinkService? = null
            private set
        private const val TAG = "RD.CamSvc"
    }
}
