package com.dustx.remotedesk

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.RadioButton
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat

class CameraLinkActivity : AppCompatActivity() {
    private lateinit var host: EditText
    private lateinit var token: EditText
    private lateinit var status: TextView
    private lateinit var usb: RadioButton
    private var wantFront = false

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { granted ->
        if (granted.values.all { it }) startLink() else status.text = "需要相机和麦克风权限"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_camera_link)
        host = findViewById(R.id.cam_host)
        token = findViewById(R.id.cam_token)
        status = findViewById(R.id.cam_status)
        usb = findViewById(R.id.cam_usb)
        host.setText(DeviceStore.camHost(this))
        applyPairIntent(intent)
        findViewById<RadioButton>(R.id.cam_wifi).setOnCheckedChangeListener { _, on ->
            if (on && host.text.isNullOrBlank()) host.setText(DeviceStore.camHost(this))
        }
        usb.setOnCheckedChangeListener { _, on ->
            if (on && host.text.isNullOrBlank()) host.setText("127.0.0.1")
        }
        findViewById<Button>(R.id.cam_start).setOnClickListener { ensurePerms() }
        findViewById<Button>(R.id.cam_stop).setOnClickListener {
            stopService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_STOP))
            HostState.set(status = "已停止摄像头模式")
        }
        findViewById<Button>(R.id.cam_switch).setOnClickListener {
            wantFront = !wantFront
            startService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_SWITCH))
        }
        findViewById<Button>(R.id.cam_mute).setOnClickListener {
            startService(Intent(this, CameraLinkService::class.java).setAction(CameraLinkService.ACTION_MUTE))
        }
        HostState.listener = { runOnUiThread { status.text = HostState.status } }
        status.text = HostState.status
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        applyPairIntent(intent)
    }

    private fun applyPairIntent(intent: Intent?) {
        val uri = intent?.data ?: return
        if (uri.scheme != "dustcam") return
        val port = if (uri.port > 0) uri.port else 8080
        val hostPart = uri.host ?: return
        host.setText("$hostPart:$port")
        val code = uri.path?.trim('/') ?: ""
        if (code.isNotEmpty()) token.setText(code)
        if (uri.getBooleanQueryParameter("usb", false)) {
            usb.isChecked = true
        }
        status.text = "已从扫码填入配对信息"
    }

    private fun ensurePerms() {
        val need = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            need.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        val missing = need.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startLink() else permLauncher.launch(missing.toTypedArray())
    }

    private fun startLink() {
        KeepAliveService.stop(this)
        stopService(Intent(this, ScreenCaptureService::class.java))
        val h = host.text.toString().trim()
        val t = token.text.toString().trim()
        if (t.length != 6) {
            status.text = "请填写电脑上的 6 位配对码"
            return
        }
        DeviceStore.setCamHost(this, h)
        val svc = Intent(this, CameraLinkService::class.java)
            .putExtra(CameraLinkService.EXTRA_HOST, h)
            .putExtra(CameraLinkService.EXTRA_TOKEN, t)
            .putExtra(CameraLinkService.EXTRA_USB, usb.isChecked)
            .putExtra(CameraLinkService.EXTRA_FRONT, wantFront)
            .putExtra(CameraLinkService.EXTRA_HEIGHT, 720)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc) else startService(svc)
        status.text = "正在启动…"
    }

    override fun onDestroy() {
        if (HostState.listener != null) HostState.listener = null
        super.onDestroy()
    }
}
