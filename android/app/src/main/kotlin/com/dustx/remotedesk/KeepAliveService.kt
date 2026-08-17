package com.dustx.remotedesk

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import org.json.JSONObject

/**
 * Stays in the foreground so signaling survives the app going to the background.
 * Owns the WebSocket, application pings, and reconnect — MainActivity is UI only.
 */
class KeepAliveService : Service(), SignalingClient.Callbacks {

    private var worker: HandlerThread? = null
    private var handler: Handler? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var backoffMs = 1_000L
    private var networkCb: ConnectivityManager.NetworkCallback? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        userStopped = false
        intent?.getStringExtra(EXTRA_URL)?.takeIf { it.isNotBlank() }?.let {
            DeviceStore.setServerUrl(this, it)
        }
        instance = this
        startForegroundInternal()
        acquireWakeLock()
        ensureWorker()
        connectNow()
        startPingLoop()
        watchNetwork()
        return START_STICKY
    }

    private fun ensureWorker() {
        if (worker != null) return
        worker = HandlerThread("rd-keepalive").also { it.start() }
        handler = Handler(worker!!.looper)
    }

    private fun connectNow() {
        if (userStopped) return
        val existing = HostState.signaling
        if (existing != null && existing.connected) return
        if (existing != null) {
            HostState.set(status = "正在连接信令…")
            existing.connect()
            return
        }
        val url = DeviceStore.serverUrl(this)
        val sig = SignalingClient(
            applicationContext, url, Build.MODEL ?: "Android",
            "Android ${Build.VERSION.RELEASE}", this,
        )
        HostState.signaling = sig
        HostState.set(status = "正在连接信令…")
        sig.connect()
    }

    private fun startPingLoop() {
        handler?.removeCallbacks(pingTask)
        handler?.post(pingTask)
    }

    private val pingTask = object : Runnable {
        override fun run() {
            HostState.signaling?.ping()
            handler?.postDelayed(this, PING_MS)
        }
    }

    private fun scheduleReconnect() {
        if (userStopped) return
        handler?.removeCallbacks(reconnectTask)
        handler?.postDelayed(reconnectTask, backoffMs)
        backoffMs = (backoffMs * 2).coerceAtMost(15_000L)
    }

    private val reconnectTask = Runnable {
        if (userStopped) return@Runnable
        Log.i(TAG, "reconnecting signaling")
        connectNow()
    }

    private fun watchNetwork() {
        if (networkCb != null) return
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (HostState.signaling?.connected != true) {
                    backoffMs = 1_000L
                    handler?.post { connectNow() }
                }
            }
        }
        networkCb = cb
        try { cm.registerDefaultNetworkCallback(cb) } catch (e: Exception) {
            Log.w(TAG, "net callback: ${e.message}")
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dustx:signal").also {
            it.setReferenceCounted(false)
            it.acquire()
        }
    }

    private fun startForegroundInternal() {
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "远程在线", NotificationManager.IMPORTANCE_LOW)
            )
            mgr.createNotificationChannel(
                NotificationChannel(CALL_CHANNEL, "远程来电", NotificationManager.IMPORTANCE_HIGH)
            )
        }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val notif = Notification.Builder(this, CHANNEL)
            .setContentTitle("尘埃X 远程在线")
            .setContentText("切到后台也会保持连接，等待远程协助")
            .setSmallIcon(R.drawable.ic_logo)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(
                NOTIF_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC or ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE,
            )
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun updateOnlineNotification(deviceId: String) {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val notif = Notification.Builder(this, CHANNEL)
            .setContentTitle("尘埃X 远程在线")
            .setContentText("远程码 ${Ids.formatId(deviceId)} · 后台保持连接")
            .setSmallIcon(R.drawable.ic_logo)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIF_ID, notif)
    }

    private fun notifyIncoming(msg: JSONObject) {
        val peer = msg.optString("viewer_id_display").ifEmpty { msg.optString("viewer_name") }
        val open = PendingIntent.getActivity(
            this, 1, Intent(this, MainActivity::class.java).putExtra(EXTRA_INCOMING, true),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
        )
        val mgr = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val notif = Notification.Builder(this, CALL_CHANNEL)
            .setContentTitle("远程协助请求")
            .setContentText("对方远程码 $peer")
            .setSmallIcon(R.drawable.ic_logo)
            .setContentIntent(open)
            .setAutoCancel(true)
            .build()
        mgr.notify(CALL_NOTIF_ID, notif)
    }

    private fun cancelIncomingNotification() {
        try {
            (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(CALL_NOTIF_ID)
        } catch (_: Exception) {}
    }

    override fun onRegistered(deviceId: String, password: String) {
        backoffMs = 1_000L
        HostState.myDeviceId = deviceId
        HostState.set(status = "在线", deviceId = deviceId, password = password)
        updateOnlineNotification(deviceId)
    }

    override fun onIncomingCall(msg: JSONObject) {
        HostState.incoming = msg
        HostState.set(status = "来电 ${msg.optString("viewer_id_display")}")
        if (HostState.uiResumed) {
            HostState.incomingListener?.invoke(msg)
        } else {
            notifyIncoming(msg)
        }
    }

    override fun onCallPending(msg: JSONObject) {
        HostState.set(status = "等待对方同意…")
    }

    override fun onSessionStart(msg: JSONObject) {
        cancelIncomingNotification()
        HostState.incoming = null
        val myId = HostState.myDeviceId
        val iAmHost = myId.isNotEmpty() && msg.optString("host_id") == myId
        if (iAmHost) {
            HostState.setMedia(peerId = msg.optString("viewer_id_display").ifEmpty { msg.optString("viewer_name") })
            val cap = ScreenCaptureService.instance
            if (cap != null) {
                HostState.pendingHostAttach = false
                cap.attachHost()
            } else {
                HostState.pendingHostAttach = true
            }
        } else {
            HostState.setMedia(peerId = msg.optString("host_id_display").ifEmpty { msg.optString("hostname") })
            val i = Intent(this, ViewerActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            startActivity(i)
        }
    }

    override fun onSignal(msg: JSONObject) {
        HostState.hostPeer?.onSignal(msg)
        HostState.viewer?.onSignal(msg)
        ScreenCaptureService.instance?.let { /* host peer already notified */ }
    }

    override fun onSessionEnd(reason: String) {
        cancelIncomingNotification()
        HostState.incoming = null
        HostState.pendingHostAttach = false
        HostState.hostPeer?.close()
        HostState.hostPeer = null
        HostState.viewer?.close()
        HostState.viewer = null
        val text = when (reason) {
            "rejected" -> "对方拒绝了连接"
            "timeout" -> "对方未在时限内同意"
            "wrong password" -> "密码错误"
            "device offline" -> "设备不在线"
            "device busy" -> "设备正忙"
            "cannot connect to self" -> "不能连接自己"
            else -> "会话结束：$reason"
        }
        HostState.set(status = if (HostState.myDeviceId.isNotEmpty()) "在线 · $text" else text)
    }

    override fun onPassword(password: String) {
        HostState.set(password = password)
    }

    override fun onClosed() {
        if (userStopped) {
            HostState.signaling = null
            HostState.set(status = "已下线")
            return
        }
        HostState.set(status = "信令断开，正在重连…")
        scheduleReconnect()
    }

    override fun onDestroy() {
        userStopped = true
        handler?.removeCallbacksAndMessages(null)
        try { worker?.quitSafely() } catch (_: Exception) {}
        try { HostState.signaling?.close() } catch (_: Exception) {}
        HostState.signaling = null
        networkCb?.let {
            try {
                (getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).unregisterNetworkCallback(it)
            } catch (_: Exception) {}
        }
        networkCb = null
        try { if (wakeLock?.isHeld == true) wakeLock?.release() } catch (_: Exception) {}
        if (instance === this) instance = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "RD.KeepAlive"
        private const val CHANNEL = "rd_online"
        private const val CALL_CHANNEL = "rd_call"
        private const val NOTIF_ID = 1002
        private const val CALL_NOTIF_ID = 1003
        private const val PING_MS = 12_000L
        const val EXTRA_URL = "url"
        const val EXTRA_INCOMING = "incoming"
        @Volatile var instance: KeepAliveService? = null
        @Volatile var userStopped: Boolean = false

        fun start(ctx: Context, url: String) {
            DeviceStore.setServerUrl(ctx, url)
            val i = Intent(ctx, KeepAliveService::class.java).putExtra(EXTRA_URL, url)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
            else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            userStopped = true
            ctx.stopService(Intent(ctx, KeepAliveService::class.java))
        }
    }
}
