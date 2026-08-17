package com.dustx.remotedesk

import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.webrtc.SurfaceViewRenderer

class ViewerActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_viewer)
        val renderer = findViewById<SurfaceViewRenderer>(R.id.remote_video)
        findViewById<TextView>(R.id.viewer_meta).text = "对方 ${HostState.peerId}  ·  ${HostState.codec}  ·  ${HostState.proto}"
        findViewById<Button>(R.id.btn_hangup).setOnClickListener {
            HostState.signaling?.hangup("viewer_hangup")
            finish()
        }
        val sig = HostState.signaling
        if (sig == null) {
            finish()
            return
        }
        val viewer = WebRtcViewer(applicationContext, sig, HostState.iceServers, renderer)
        HostState.viewer = viewer
        viewer.start()
        HostState.listener = {
            runOnUiThread {
                findViewById<TextView>(R.id.viewer_meta).text =
                    "对方 ${HostState.peerId}  ·  ${HostState.codec}  ·  ${HostState.proto}"
                if (HostState.status.startsWith("在线 ·") || HostState.status == "已下线") finish()
            }
        }
    }

    override fun onDestroy() {
        HostState.viewer?.close()
        HostState.viewer = null
        if (HostState.listener != null) HostState.listener = null
        super.onDestroy()
    }
}
