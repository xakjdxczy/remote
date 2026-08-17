package com.dustx.remotedesk

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONObject
import org.webrtc.DataChannel
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.RtpReceiver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceViewRenderer
import org.webrtc.VideoTrack
import java.nio.ByteBuffer

/** Viewer-side peer: creates the offer and renders the remote screen. */
class WebRtcViewer(
    private val appContext: Context,
    private val signaling: SignalingClient,
    private val iceServers: List<PeerConnection.IceServer>,
    private val renderer: SurfaceViewRenderer,
) {
    private var pc: PeerConnection? = null
    private var dc: DataChannel? = null
    private var offerSent = false
    private val main = Handler(Looper.getMainLooper())

    fun start() {
        WebRtcHost.ensureFactory(appContext)
        val egl = WebRtcHost.egl()
        try {
            renderer.init(egl.eglBaseContext, null)
            renderer.setMirror(false)
        } catch (_: Exception) { /* already inited */ }
        val config = PeerConnection.RTCConfiguration(iceServers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        pc = WebRtcHost.peerFactory().createPeerConnection(config, observer)
        val peer = pc ?: return
        dc = peer.createDataChannel("session", DataChannel.Init())
        dc?.registerObserver(dcObserver)
        peer.addTransceiver(
            MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO,
            org.webrtc.RtpTransceiver.RtpTransceiverInit(org.webrtc.RtpTransceiver.RtpTransceiverDirection.RECV_ONLY),
        )
        try {
            val caps = WebRtcHost.peerFactory().getRtpReceiverCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
            val ordered = caps.codecs.sortedByDescending { it.name.equals("H264", true) }
            peer.transceivers.firstOrNull { it.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO }
                ?.setCodecPreferences(ordered)
        } catch (_: Exception) {}
        peer.createOffer(object : SimpleSdp() {
            override fun onCreateSuccess(desc: SessionDescription) {
                peer.setLocalDescription(SimpleSdp(), desc)
                main.postDelayed({ trySendOffer() }, 2500)
            }
        }, MediaConstraints())
    }

    fun onSignal(msg: JSONObject) {
        if (msg.optString("kind") != "answer") return
        val sdp = msg.optJSONObject("sdp") ?: return
        pc?.setRemoteDescription(SimpleSdp(), SessionDescription(SessionDescription.Type.ANSWER, sdp.optString("sdp")))
    }

    private fun trySendOffer() {
        val local = pc?.localDescription ?: return
        if (offerSent) return
        offerSent = true
        signaling.send(
            JSONObject()
                .put("type", "signal")
                .put("kind", "offer")
                .put("sdp", JSONObject().put("type", "offer").put("sdp", local.description))
        )
        Log.i(TAG, "offer sent")
    }

    fun close() {
        try { dc?.close() } catch (_: Exception) {}
        try { pc?.close() } catch (_: Exception) {}
        try { renderer.release() } catch (_: Exception) {}
        dc = null; pc = null; offerSent = false
    }

    private val observer = object : PeerConnection.Observer {
        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
            if (state == PeerConnection.IceGatheringState.COMPLETE) main.post { trySendOffer() }
        }
        override fun onAddTrack(receiver: RtpReceiver?, streams: Array<out MediaStream>?) {
            val track = receiver?.track() as? VideoTrack ?: return
            main.post { track.addSink(renderer) }
        }
        override fun onIceCandidate(c: IceCandidate?) {}
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>?) {}
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState?) {}
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onAddStream(s: MediaStream?) {}
        override fun onRemoveStream(s: MediaStream?) {}
        override fun onDataChannel(c: DataChannel?) {}
        override fun onRenegotiationNeeded() {}
    }

    private val dcObserver = object : DataChannel.Observer {
        override fun onBufferedAmountChange(previousAmount: Long) {}
        override fun onStateChange() {
            if (dc?.state() == DataChannel.State.OPEN) HostState.set(status = "正在控制对方")
        }
        override fun onMessage(buffer: DataChannel.Buffer) {
            if (buffer.binary) return
            val arr = ByteArray(buffer.data.remaining())
            buffer.data.get(arr)
            val msg = try { JSONObject(String(arr, Charsets.UTF_8)) } catch (_: Exception) { return }
            if (msg.optString("type") == "ping") {
                val data = JSONObject().put("type", "pong").put("t", msg.opt("t")).toString().toByteArray()
                dc?.send(DataChannel.Buffer(ByteBuffer.wrap(data), false))
            }
        }
    }

    companion object { private const val TAG = "RD.Viewer" }
}
