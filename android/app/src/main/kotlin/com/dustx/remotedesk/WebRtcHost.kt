package com.dustx.remotedesk

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONObject
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RTCStatsCollectorCallback
import org.webrtc.RtpParameters
import org.webrtc.RtpReceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.VideoTrack
import java.nio.ByteBuffer
import kotlin.math.abs

/**
 * Native WebRTC host peer. Answers the viewer's offer, attaches the screen as a
 * real **WebRTC video track** (VP8/H264, hardware-encoded), receives the
 * "session" DataChannel and applies input events. Non-trickle ICE.
 */
class WebRtcHost(
    private val appContext: Context,
    private val signaling: SignalingClient,
    private val iceServers: List<PeerConnection.IceServer>,
    private val videoTrack: VideoTrack?,
    private val reportedW: Int,
    private val reportedH: Int,
    private val deviceW: Int,
    private val deviceH: Int,
) {
    private var pc: PeerConnection? = null
    private var dc: DataChannel? = null
    private var answerSent = false
    private var pressX = 0
    private var pressY = 0
    private val main = Handler(Looper.getMainLooper())

    val isOpen: Boolean get() = dc?.state() == DataChannel.State.OPEN

    fun start() {
        val config = PeerConnection.RTCConfiguration(iceServers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        pc = ensureFactory(appContext).createPeerConnection(config, pcObserver)
    }

    fun onSignal(msg: JSONObject) {
        if (msg.optString("kind") == "offer") {
            val sdp = msg.optJSONObject("sdp") ?: return
            handleOffer(sdp.optString("sdp"))
        }
    }

    fun stats(cb: RTCStatsCollectorCallback) {
        pc?.getStats(cb)
    }

    private fun handleOffer(sdp: String) {
        val peer = pc ?: return
        peer.setRemoteDescription(object : SimpleSdp() {
            override fun onSetSuccess() {
                if (videoTrack != null) {
                    try {
                        val sender = peer.addTrack(videoTrack, listOf("rd-stream"))
                        // Prioritise smoothness: keep framerate, allow a high bitrate ceiling.
                        val p = sender.parameters
                        p.degradationPreference = RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE
                        if (p.encodings.isNotEmpty()) {
                            p.encodings[0].maxFramerate = 30
                            p.encodings[0].minBitrateBps = 1_000_000
                            p.encodings[0].maxBitrateBps = 4_000_000
                        }
                        sender.parameters = p
                    } catch (e: Exception) {
                        Log.w(TAG, "addTrack/params: ${e.message}")
                    }
                    // Prefer hardware H.264 (usually smoother/cheaper than SW VP8).
                    try {
                        val caps = peerFactory().getRtpSenderCapabilities(MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO)
                        val ordered = caps.codecs.sortedByDescending { it.name.equals("H264", true) }
                        peer.transceivers
                            .firstOrNull { it.mediaType == MediaStreamTrack.MediaType.MEDIA_TYPE_VIDEO }
                            ?.setCodecPreferences(ordered)
                    } catch (e: Exception) {
                        Log.w(TAG, "codec pref: ${e.message}")
                    }
                }
                peer.createAnswer(object : SimpleSdp() {
                    override fun onCreateSuccess(desc: SessionDescription) {
                        peer.setLocalDescription(SimpleSdp(), desc)
                        main.postDelayed({ trySendAnswer() }, 2500)
                    }
                }, MediaConstraints())
            }
        }, SessionDescription(SessionDescription.Type.OFFER, sdp))
    }

    private fun trySendAnswer() {
        val local = pc?.localDescription ?: return
        if (answerSent) return
        answerSent = true
        val out = JSONObject()
            .put("type", "signal")
            .put("kind", "answer")
            .put("sdp", JSONObject().put("type", "answer").put("sdp", local.description))
        signaling.send(out)
        Log.i(TAG, "answer sent")
    }

    private fun sendJson(obj: JSONObject) {
        val channel = dc ?: return
        val data = obj.toString().toByteArray(Charsets.UTF_8)
        channel.send(DataChannel.Buffer(ByteBuffer.wrap(data), false))
    }

    fun close() {
        try { dc?.close() } catch (_: Exception) {}
        try { pc?.close() } catch (_: Exception) {}
        dc = null; pc = null; answerSent = false
    }

    // ---- observers -------------------------------------------------------
    private val pcObserver = object : PeerConnection.Observer {
        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState) {
            if (state == PeerConnection.IceGatheringState.COMPLETE) main.post { trySendAnswer() }
        }
        override fun onDataChannel(channel: DataChannel) {
            dc = channel
            channel.registerObserver(dcObserver)
        }
        override fun onIceCandidate(c: IceCandidate?) {}
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>?) {}
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(s: PeerConnection.IceConnectionState?) {
            Log.i(TAG, "ICE $s")
        }
        override fun onIceConnectionReceivingChange(b: Boolean) {}
        override fun onConnectionChange(s: PeerConnection.PeerConnectionState?) {
            if (s == PeerConnection.PeerConnectionState.CONNECTED) HostState.set(status = "P2P 已连接")
        }
        override fun onAddStream(s: MediaStream?) {}
        override fun onRemoveStream(s: MediaStream?) {}
        override fun onRenegotiationNeeded() {}
        override fun onAddTrack(r: RtpReceiver?, s: Array<out MediaStream>?) {}
    }

    private val dcObserver = object : DataChannel.Observer {
        override fun onBufferedAmountChange(previousAmount: Long) {}
        override fun onStateChange() {
            if (dc?.state() == DataChannel.State.OPEN) {
                HostState.set(status = "被控中")
                sendJson(
                    JSONObject().put("type", "screen_info")
                        .put("width", reportedW).put("height", reportedH).put("backend", "android")
                )
            }
        }
        override fun onMessage(buffer: DataChannel.Buffer) {
            if (buffer.binary) return
            val arr = ByteArray(buffer.data.remaining())
            buffer.data.get(arr)
            val msg = try { JSONObject(String(arr, Charsets.UTF_8)) } catch (e: Exception) { return }
            when (msg.optString("type")) {
                "input" -> handleInput(msg)
                "nav" -> InputAccessibilityService.global(msg.optString("action"))
                "ping" -> sendJson(JSONObject().put("type", "pong").put("t", msg.opt("t")))
                "conn_info" -> {
                    HostState.connMethod = if (msg.optString("method") == "relay") "TURN 中继" else "P2P 直连"
                    HostState.set()
                }
            }
        }
    }

    private fun toDevice(x: Int, y: Int): Pair<Int, Int> {
        val dx = (x.toLong() * deviceW / reportedW).toInt()
        val dy = (y.toLong() * deviceH / reportedH).toInt()
        return dx to dy
    }

    private fun handleInput(msg: JSONObject) {
        val (dx, dy) = toDevice(msg.optInt("x"), msg.optInt("y"))
        val ev = msg.optString("event")
        Log.d(TAG, "input $ev dev=($dx,$dy) accessibility=${InputAccessibilityService.isEnabled}")
        when (ev) {
            "down" -> { pressX = dx; pressY = dy }
            "up" -> {
                val slop = (20L * deviceW / reportedW).toInt().coerceAtLeast(12)
                if (abs(dx - pressX) <= slop && abs(dy - pressY) <= slop) {
                    InputAccessibilityService.tap(dx, dy)
                } else {
                    InputAccessibilityService.swipe(pressX, pressY, dx, dy)
                }
            }
            "scroll" -> {
                val delta = deviceH / 4
                val dir = if (msg.optString("button") == "up") 1 else -1
                InputAccessibilityService.swipe(dx, dy, dx, dy - delta * dir, 180)
            }
        }
    }

    companion object {
        private const val TAG = "RD.WebRTC"
        @Volatile private var factory: PeerConnectionFactory? = null
        @Volatile private var eglBase: EglBase? = null

        @Synchronized
        fun ensureFactory(context: Context): PeerConnectionFactory {
            var f = factory
            if (f == null) {
                PeerConnectionFactory.initialize(
                    PeerConnectionFactory.InitializationOptions.builder(context.applicationContext)
                        .createInitializationOptions()
                )
                val egl = EglBase.create()
                eglBase = egl
                val enc = DefaultVideoEncoderFactory(egl.eglBaseContext, true, true)
                val dec = DefaultVideoDecoderFactory(egl.eglBaseContext)
                f = PeerConnectionFactory.builder()
                    .setVideoEncoderFactory(enc)
                    .setVideoDecoderFactory(dec)
                    .createPeerConnectionFactory()
                factory = f
            }
            return f!!
        }

        fun egl(): EglBase = eglBase!!
        fun peerFactory(): PeerConnectionFactory = factory!!
    }
}

/** SdpObserver with no-op defaults so subclasses override only what they need. */
open class SimpleSdp : SdpObserver {
    override fun onCreateSuccess(desc: SessionDescription) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(error: String?) {}
    override fun onSetFailure(error: String?) {}
}
