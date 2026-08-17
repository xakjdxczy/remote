package com.dustx.remotedesk

import android.Manifest
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    private lateinit var serverUrl: EditText
    private lateinit var statusView: TextView
    private lateinit var deviceIdView: TextView
    private lateinit var passwordView: TextView

    private val projectionLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
            if (result.resultCode == RESULT_OK && result.data != null) {
                val svc = Intent(this, ScreenCaptureService::class.java).apply {
                    putExtra(ScreenCaptureService.EXTRA_RESULT_CODE, result.resultCode)
                    putExtra(ScreenCaptureService.EXTRA_DATA, result.data)
                    putExtra(ScreenCaptureService.EXTRA_URL, serverUrl.text.toString().trim())
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(svc) else startService(svc)
            }
        }

    private val notifLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { /* proceed regardless */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        findViewById<TextView>(R.id.app_version).text =
            "版本 v${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})"

        serverUrl = findViewById(R.id.server_url)
        statusView = findViewById(R.id.status)
        deviceIdView = findViewById(R.id.device_id)
        passwordView = findViewById(R.id.password)

        findViewById<Button>(R.id.btn_start).setOnClickListener { startProjection() }
        findViewById<Button>(R.id.btn_stop).setOnClickListener {
            stopService(Intent(this, ScreenCaptureService::class.java))
        }
        findViewById<Button>(R.id.btn_accessibility).setOnClickListener {
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            notifLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        HostState.listener = { runOnUiThread { render() } }
        render()
    }

    private fun startProjection() {
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(mpm.createScreenCaptureIntent())
    }

    private fun render() {
        statusView.text = HostState.status
        deviceIdView.text = Ids.formatId(HostState.deviceId)
        passwordView.text = HostState.password?.ifEmpty { "——————" } ?: "——————"
        findViewById<TextView>(R.id.stat_method).text = "连接方式：${HostState.connMethod ?: "--"}"
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
    }

    override fun onResume() {
        super.onResume()
        render()
    }

    override fun onDestroy() {
        if (HostState.listener != null) HostState.listener = null
        super.onDestroy()
    }
}
