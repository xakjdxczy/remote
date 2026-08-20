package com.dustx.remotedesk

import android.Manifest
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.content.pm.PackageManager
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import org.json.JSONObject

class MainActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_TAB = "tab"
        const val TAB_REMOTE = "remote"
        const val TAB_CAMERA = "camera"
    }

    private lateinit var serverUrl: EditText
    private lateinit var statusView: TextView
    private lateinit var deviceIdView: TextView
    private lateinit var passwordView: TextView
    private lateinit var remoteId: EditText
    private lateinit var remotePass: EditText
    private lateinit var refreshSpinner: Spinner
    private lateinit var tabRemote: TextView
    private lateinit var tabCamera: TextView
    private lateinit var panelRemote: ScrollView
    private lateinit var panelCamera: ScrollView
    private lateinit var camHost: EditText
    private lateinit var camToken: EditText
    private lateinit var camStatus: TextView
    private lateinit var camUsb: RadioButton
    private lateinit var camPairCard: LinearLayout
    private lateinit var camModeHint: TextView
    private var camFront = false
    private var cameraTab = false
    private val main = Handler(Looper.getMainLooper())
    private var pendingAcceptId: String? = null
    private var pwTimer: Runnable? = null
    private var shownIncomingSid: String? = null
    private var incomingDialog: AlertDialog? = null

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK && result.data != null) {
                val svc = Intent(this, ScreenCaptureService::class.java).apply {
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, result.resultCode)
                    putExtra(ScreenCaptureService.EXTRA_DATA, result.data)
                    putExtra(ScreenCaptureService.EXTRA_URL, serverUrl.text.toString().trim())
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc) else startService(svc)
                pendingAcceptId?.let { id ->
                    main.postDelayed({
                        HostState.signaling?.answerCall(id, true)
                        pendingAcceptId = null
                    }, 800)
                }
            } else {
                pendingAcceptId?.let { HostState.signaling?.answerCall(it, false) }
                pendingAcceptId = null
            }
        }

    private val notifLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { /* proceed */ }

    private val camPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { granted ->
        if (granted.values.all { it }) startCameraLink() else camStatus.text = "需要相机和麦克风权限"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<TextView>(R.id.app_version).text =
            "版本 v${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"

        serverUrl = findViewById(R.id.server_url)
        statusView = findViewById(R.id.status)
        deviceIdView = findViewById(R.id.device_id)
        passwordView = findViewById(R.id.password)
        remoteId = findViewById(R.id.remote_id)
        remotePass = findViewById(R.id.remote_pass)
        refreshSpinner = findViewById(R.id.pw_refresh)

        val savedUrl = DeviceStore.serverUrl(this)
        if (savedUrl.isNotBlank()) serverUrl.setText(savedUrl)

        val labels = listOf("不自动刷新（仅手动）", "每 10 分钟", "每 1 小时", "每 24 小时")
        val values = listOf(0, 600, 3600, 86400)
        refreshSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)
        refreshSpinner.setSelection(values.indexOf(DeviceStore.refreshSec(this)).coerceAtLeast(0))

        bindTabs()
        bindCamera()

        findViewById<Button>(R.id.btn_start).setOnClickListener { startProjection() }
        findViewById<Button>(R.id.btn_stop).setOnClickListener {
            stopService(Intent(this, ScreenCaptureService::class.java))
        }
        findViewById<Button>(R.id.btn_offline).setOnClickListener { goOffline() }
        findViewById<Button>(R.id.btn_battery).setOnClickListener { requestBatteryExemption(force = true) }
        findViewById<Button>(R.id.btn_accessibility).setOnClickListener {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }
        findViewById<Button>(R.id.btn_refresh_pw).setOnClickListener {
            HostState.signaling?.refreshPassword()
        }
        findViewById<Button>(R.id.btn_connect).setOnClickListener { connectToPeer() }
        refreshSpinner.setOnItemSelectedListener(object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: android.widget.AdapterView<*>?, v: android.view.View?, pos: Int, id: Long) {
                DeviceStore.setRefreshSec(this@MainActivity, values[pos])
                schedulePasswordRefresh()
            }
            override fun onNothingSelected(p: android.widget.AdapterView<*>?) {}
        })

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            notifLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        HostState.listener = { runOnUiThread { render() } }
        HostState.incomingListener = { msg -> runOnUiThread { showIncoming(msg) } }
        goOnline()
        requestBatteryExemption(force = false)
        schedulePasswordRefresh()
        handleIncomingIntent(intent)
        render()
    }

    private fun bindTabs() {
        tabRemote = findViewById(R.id.tab_remote)
        tabCamera = findViewById(R.id.tab_camera)
        panelRemote = findViewById(R.id.panel_remote)
        panelCamera = findViewById(R.id.panel_camera)
        tabRemote.setOnClickListener { showTab(false) }
        tabCamera.setOnClickListener { showTab(true) }
        showTab(false)
    }

    private fun showTab(camera: Boolean) {
        cameraTab = camera
        tabRemote.isSelected = !camera
        tabCamera.isSelected = camera
        panelRemote.visibility = if (camera) View.GONE else View.VISIBLE
        panelCamera.visibility = if (camera) View.VISIBLE else View.GONE
    }

    private fun bindCamera() {
        camHost = findViewById(R.id.cam_host)
        camToken = findViewById(R.id.cam_token)
        camStatus = findViewById(R.id.cam_status)
        camUsb = findViewById(R.id.cam_usb)
        camPairCard = findViewById(R.id.cam_pair_card)
        camModeHint = findViewById(R.id.cam_mode_hint)
        camHost.setText(DeviceStore.camHost(this))
        findViewById<RadioButton>(R.id.cam_wifi).setOnCheckedChangeListener { _, on ->
            if (on) {
                if (camHost.text.toString().startsWith("127.0.0.1")) camHost.setText(DeviceStore.camHost(this))
                renderCamTransport()
            }
        }
        camUsb.setOnCheckedChangeListener { _, on ->
            if (on) {
                camHost.setText("127.0.0.1")
                renderCamTransport()
            }
        }
        renderCamTransport()
        findViewById<Button>(R.id.cam_start).setOnClickListener { ensureCamPerms() }
        findViewById<Button>(R.id.cam_stop).setOnClickListener {
            stopService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_STOP))
            HostState.set(status = "已停止摄像头模式")
        }
        findViewById<Button>(R.id.cam_switch).setOnClickListener {
            camFront = !camFront
            startService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_SWITCH))
        }
        findViewById<Button>(R.id.cam_mute).setOnClickListener {
            startService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_MUTE))
        }
    }

    private fun renderCamTransport() {
        val usbOn = camUsb.isChecked
        camPairCard.visibility = if (usbOn) View.GONE else View.VISIBLE
        camModeHint.text = if (usbOn) {
            "电脑点「准备 USB」后会自动配对，这里不用填地址和配对码。点下方开始即可。"
        } else {
            "填电脑局域网 IP 和配对码。USB 网络共享也走这里，填电脑在那条 USB 网上的 IP。"
        }
    }

    private fun applyCamPair(intent: Intent?) {
        val uri = intent?.data ?: return
        if (uri.scheme != "dustcam") return
        showTab(true)
        val port = if (uri.port > 0) uri.port else 18790
        val hostPart = uri.host ?: return
        camHost.setText("$hostPart:$port")
        val code = uri.path?.trim('/') ?: ""
        if (code.isNotEmpty()) camToken.setText(code)
        if (uri.getBooleanQueryParameter("usb", false)) {
            camUsb.isChecked = true
            camStatus.text = "电脑已配对，点开始即可"
        } else {
            camStatus.text = "已填入配对信息"
        }
        renderCamTransport()
    }

    private fun ensureCamPerms() {
        val need = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            need.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        val missing = need.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startCameraLink() else camPermLauncher.launch(missing.toTypedArray())
    }

    private fun startCameraLink() {
        KeepAliveService.stop(this)
        stopService(Intent(this, ScreenCaptureService::class.java))
        val h = if (camUsb.isChecked) {
            camHost.text.toString().trim().ifBlank { "127.0.0.1" }
        } else {
            camHost.text.toString().trim()
        }
        val t = camToken.text.toString().trim()
        if (t.length != 6) {
            camStatus.text = if (camUsb.isChecked) "请先在电脑点「准备 USB」" else "请填写电脑上的 6 位配对码"
            return
        }
        if (!camUsb.isChecked) DeviceStore.setCamHost(this, h)
        val svc = Intent(this, CameraLinkService::class.java)
            .putExtra(CameraLinkService.EXTRA_HOST, h)
            .putExtra(CameraLinkService.EXTRA_TOKEN, t)
            .putExtra(CameraLinkService.EXTRA_USB, camUsb.isChecked)
            .putExtra(CameraLinkService.EXTRA_FRONT, camFront)
            .putExtra(CameraLinkService.EXTRA_HEIGHT, 720)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc) else startService(svc)
        camStatus.text = "正在启动…"
    }

    private fun goOnline() {
        val url = serverUrl.text.toString().trim().ifEmpty { DeviceStore.serverUrl(this) }
        DeviceStore.setServerUrl(this, url)
        Thread {
            HostState.iceServers = fetchIce(url)
        }.start()
        KeepAliveService.start(this, url)
    }

    private fun goOffline() {
        stopService(Intent(this, ScreenCaptureService::class.java))
        KeepAliveService.stop(this)
        HostState.set(status = "已下线")
    }

    private fun requestBatteryExemption(force: Boolean) {
        if (!force && DeviceStore.askedBattery(this)) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) {
            if (force) HostState.set(status = "已允许忽略电池优化，后台更稳")
            return
        }
        DeviceStore.setAskedBattery(this)
        try {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                    .setData(Uri.parse("package:$packageName"))
            )
        } catch (_: Exception) {
            try {
                startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
            } catch (_: Exception) {}
        }
    }

    private fun fetchIce(wsUrl: String): List<org.webrtc.PeerConnection.IceServer> {
        return try {
            val base = wsUrl.replace("wss://", "https://").replace("ws://", "http://").removeSuffix("/ws")
            val body = okhttp3.OkHttpClient().newCall(okhttp3.Request.Builder().url("$base/api/config").build())
                .execute().use { it.body?.string() } ?: return emptyList()
            val arr = JSONObject(body).optJSONArray("ice_servers") ?: return emptyList()
            val out = ArrayList<org.webrtc.PeerConnection.IceServer>()
            for (i in 0 until arr.length()) {
                val s = arr.getJSONObject(i)
                val urls = ArrayList<String>()
                when (val u = s.opt("urls")) {
                    is org.json.JSONArray -> for (j in 0 until u.length()) urls.add(u.getString(j))
                    is String -> urls.add(u)
                }
                if (urls.isEmpty()) continue
                val b = org.webrtc.PeerConnection.IceServer.builder(urls)
                s.optString("username").takeIf { it.isNotEmpty() }?.let { b.setUsername(it) }
                s.optString("credential").takeIf { it.isNotEmpty() }?.let { b.setPassword(it) }
                out.add(b.createIceServer())
            }
            out
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun startProjection() {
        goOnline()
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(mpm.createScreenCaptureIntent())
    }

    private fun connectToPeer() {
        val id = remoteId.text.toString()
        val pw = remotePass.text.toString()
        if (id.isBlank() || pw.isBlank()) {
            HostState.set(status = "请填写对方远程码和密码")
            return
        }
        val digits = id.filter { it.isDigit() }
        if (digits == HostState.myDeviceId) {
            HostState.set(status = "不能连接自己的远程码")
            return
        }
        goOnline()
        main.postDelayed({
            HostState.signaling?.connectTo(id, pw, Build.MODEL ?: "Android")
            HostState.set(status = "正在发起连接…")
        }, 400)
    }

    private fun showIncoming(msg: JSONObject) {
        val sid = msg.optString("session_id")
        if (sid.isNotEmpty() && sid == shownIncomingSid && incomingDialog?.isShowing == true) return
        shownIncomingSid = sid
        incomingDialog?.dismiss()
        val peer = msg.optString("viewer_id_display").ifEmpty { msg.optString("viewer_name") }
        incomingDialog = AlertDialog.Builder(this)
            .setTitle("远程协助请求")
            .setMessage("对方远程码：$peer\n同意后将共享本机屏幕。")
            .setPositiveButton("同意") { _, _ ->
                HostState.incoming = null
                pendingAcceptId = sid
                if (ScreenCaptureService.instance != null) {
                    HostState.signaling?.answerCall(sid, true)
                    pendingAcceptId = null
                } else {
                    startProjection()
                }
            }
            .setNegativeButton("拒绝") { _, _ ->
                HostState.incoming = null
                HostState.signaling?.answerCall(sid, false)
            }
            .setCancelable(false)
            .show()
    }

    private fun handleIncomingIntent(intent: Intent?) {
        if (intent?.getStringExtra(EXTRA_TAB) == TAB_CAMERA || intent?.data?.scheme == "dustcam") {
            showTab(true)
        }
        applyCamPair(intent)
        if (intent?.getBooleanExtra(KeepAliveService.EXTRA_INCOMING, false) == true) {
            showTab(false)
            HostState.incoming?.let { showIncoming(it) }
        }
    }

    private fun schedulePasswordRefresh() {
        pwTimer?.let { main.removeCallbacks(it) }
        val sec = DeviceStore.refreshSec(this)
        if (sec <= 0) return
        val task = object : Runnable {
            override fun run() {
                HostState.signaling?.refreshPassword()
                main.postDelayed(this, sec * 1000L)
            }
        }
        pwTimer = task
        main.postDelayed(task, sec * 1000L)
    }

    private fun render() {
        statusView.text = HostState.status
        deviceIdView.text = Ids.formatId(HostState.deviceId)
        passwordView.text = HostState.password?.ifEmpty { "——————" } ?: "——————"
        findViewById<TextView>(R.id.stat_method).text = "连接方式：${HostState.connMethod ?: "--"}"
        findViewById<TextView>(R.id.stat_peer).text = "对方远程码：${HostState.peerId}"
        findViewById<TextView>(R.id.stat_codec).text = "编解码：${HostState.codec}"
        findViewById<TextView>(R.id.stat_proto).text = "传输：${HostState.proto}"
        findViewById<TextView>(R.id.stat_speed).text = "实时网速：${HostState.netSpeed}"
        findViewById<TextView>(R.id.stat_session).text = "本次流量：${HostState.fmtBytes(HostState.sessionBytes)}"
        findViewById<TextView>(R.id.stat_total).text = "历史流量：${HostState.fmtBytes(HostState.totalBytes)}"
        findViewById<TextView>(R.id.stat_latency).text =
            "延迟：" + (if (HostState.latencyMs >= 0) "${HostState.latencyMs} ms" else "-- ms")
        val access = InputAccessibilityService.isConfigured(this)
        findViewById<TextView>(R.id.stat_access).apply {
            text = "远程操作(无障碍)：" + if (access) "已开启" else "未开启（点上方按钮开启才能被远程点击）"
            setTextColor(if (access) 0xFF22E6C8.toInt() else 0xFFFF6B8B.toInt())
        }
        if (::camStatus.isInitialized) camStatus.text = HostState.status
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleIncomingIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        HostState.uiResumed = true
        HostState.listener = { runOnUiThread { render() } }
        HostState.incomingListener = { msg -> runOnUiThread { showIncoming(msg) } }
        HostState.incoming?.let { showIncoming(it) }
        render()
    }

    override fun onPause() {
        HostState.uiResumed = false
        super.onPause()
    }

    override fun onDestroy() {
        pwTimer?.let { main.removeCallbacks(it) }
        if (HostState.listener != null) HostState.listener = null
        if (HostState.incomingListener != null) HostState.incomingListener = null
        super.onDestroy()
    }
}
