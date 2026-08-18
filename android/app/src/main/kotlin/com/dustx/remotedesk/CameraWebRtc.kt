package com.dustx.remotedesk

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import org.webrtc.AudioTrack
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.RtpReceiver
import org.webrtc.SessionDescription
import org.webrtc.VideoTrack
import java.util.concurrent.TimeUnit

/** Phone answers the desktop offer and sends camera + mic tracks. */
class CameraWebRtc(
    private val appContext: Context,
    private val iceServers: List<PeerConnection.IceServer>,
    private val videoTrack: VideoTrack,
    private val audioTrack: AudioTrack,
    private val onStatus: (String) -> Unit,
) {
    private val main = Handler(Looper.getMainLooper())
    private var socket: WebSocket? = null
    private var pc: PeerConnection? = null
    private var answerSent = false

    fun connect(wsUrl: String, token: String) {
        close()
        onStatus("正在连接电脑…")
        val client = OkHttpClient.Builder().readTimeout(0, TimeUnit.MILLISECONDS).build()
        socket = client.newWebSocket(
            Request.Builder().url(wsUrl).build(),
            object : WebSocketListener() {
                override fun onOpen(webSocket: WebSocket, response: Response) {
                    webSocket.send(
                        JSONObject()
                            .put("type", "hello")
                            .put("role", "phone")
                            .put("token", token)
                            .toString()
                    )
                }

                override fun onMessage(webSocket: WebSocket, text: String) {
                    val msg = try {
                        JSONObject(text)
                    } catch (_: Exception) {
                        return
                    }
                    when (msg.optString("type")) {
                        "error" -> onStatus(msg.optString("message", "配对失败"))
                        "hello_ok" -> onStatus("已配对，等待电脑拉流")
                        "ready" -> onStatus("通道就绪")
                        "signal" -> if (msg.optString("kind") == "offer") {
                            handleOffer(msg.optJSONObject("sdp")?.optString("sdp").orEmpty())
                        }
                        "peer_left" -> onStatus("电脑已断开")
                    }
                }

                override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                    onStatus("连接失败：${t.message ?: "network"}")
                }

                override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                    onStatus("通道已关闭")
                }
            },
        )
    }

    private fun handleOffer(sdp: String) {
        if (sdp.isBlank()) return
        answerSent = false
        val factory = WebRtcHost.ensureFactory(appContext)
        val config = PeerConnection.RTCConfiguration(iceServers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        try { pc?.close() } catch (_: Exception) {}
        val peer = factory.createPeerConnection(config, pcObserver) ?: return
        pc = peer
        try {
            peer.addTrack(videoTrack, listOf("dust-cam"))
            peer.addTrack(audioTrack, listOf("dust-cam"))
        } catch (e: Exception) {
            Log.w(TAG, "addTrack: ${e.message}")
        }
        try {
            val caps = WebRtcHost.peerFactory().getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
            val ordered = caps.codecs.sortedByDescending { it.name.equals("H264", true) }
            peer.transceivers
                .firstOrNull { it.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO }
                ?.setCodecPreferences(ordered)
        } catch (e: Exception) {
            Log.w(TAG, "codec: ${e.message}")
        }
        peer.setRemoteDescription(object : SimpleSdp() {
            override fun onSetSuccess() {
                peer.createAnswer(object : SimpleSdp() {
                    override fun onCreateSuccess(desc: SessionDescription) {
                        peer.setLocalDescription(SimpleSdp(), desc)
                        main.postDelayed({ trySendAnswer() }, 2000)
                    }
                }, MediaConstraints())
            }
        }, SessionDescription(SessionDescription.Type.OFFER, sdp))
    }

    private fun trySendAnswer() {
        val local = pc?.localDescription ?: return
        if (answerSent) return
        answerSent = true
        socket?.send(
            JSONObject()
                .put("type", "signal")
                .put("kind", "answer")
                .put("sdp", JSONObject().put("type", "answer").put("sdp", local.description))
                .toString()
        )
        onStatus("已发送应答")
    }

    fun close() {
        try { socket?.close(1000, "bye") } catch (_: Exception) {}
        try { pc?.close() } catch (_: Exception) {}
        socket = null
        pc = null
        answerSent = false
    }

    private val pcObserver = object : PeerConnection.Observer {
        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
            if (state == PeerConnection.IceGatheringState.COMPLETE) main.post { trySendAnswer() }
        }
        override fun onIceCandidate(c: IceCandidate?) {}
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>?) {}
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState?) {
            Log.i(TAG, "ICE $s")
        }
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onConnectionChange(s: PeerConnection.PeerConnectionState?) {
            if (s == PeerConnection.PeerConnectionState.CONNECTED) onStatus("已连接到电脑")
        }
        override fun onAddStream(s: MediaStream?) {}
        override fun onRemoveStream(s: MediaStream?) {}
        override fun onDataChannel(c: org.webrtc.DataChannel?) {}
        override fun onRenegotiationNeeded() {}
        override fun onAddTrack(r: RtpReceiver?, s: Array<out MediaStream>?) {}
    }

    companion object {
        private const val TAG = "RD.CamRTC"
    }
}
