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
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpReceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import java.nio.ByteBuffer
import kotlin.math.abs

/**
 * Native WebRTC host peer. Mirrors the Python aiortc host: it answers the
 * viewer's offer, receives the "session" DataChannel, streams JPEG frames on it
 * and applies input events. Non-trickle ICE (candidates are gathered then the
 * full SDP is sent), matching the web viewer.
 */
class WebRtcHost(
    private val appContext: Context,
    private val signaling: SignalingClient,
    private val iceUrls: List<String>,
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
        ensureFactory(appContext)
        val servers = iceUrls.map { PeerConnection.IceServer.builder(it).createIceServer() }
        val config = PeerConnection.RTCConfiguration(servers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        }
        pc = factory!!.createPeerConnection(config, pcObserver)
    }

    fun onSignal(msg: JSONObject) {
        if (msg.optString("kind") == "offer") {
            val sdp = msg.optJSONObject("sdp") ?: return
            handleOffer(sdp.optString("sdp"))
        }
    }

    private fun handleOffer(sdp: String) {
        val peer = pc ?: return
        peer.setRemoteDescription(object : SimpleSdp() {
            override fun onSetSuccess() {
                peer.createAnswer(object : SimpleSdp() {
                    override fun onCreateSuccess(desc: SessionDescription) {
                        peer.setLocalDescription(SimpleSdp(), desc)
                        // Fallback: send answer even if gathering does not report COMPLETE.
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

    fun sendFrame(bytes: ByteArray) {
        val channel = dc ?: return
        if (channel.state() != DataChannel.State.OPEN) return
        channel.send(DataChannel.Buffer(ByteBuffer.wrap(bytes), true))
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
                HostState.set(status = "P2P 直连中（对方正在控制）")
                sendJson(
                    JSONObject().put("type", "screen_info")
                        .put("width", reportedW).put("height", reportedH).put("backend", "android")
                )
            }
        }
        override fun onMessage(buffer: DataChannel.Buffer) {
            if (buffer.binary) return // file chunks not handled on mobile host
            val arr = ByteArray(buffer.data.remaining())
            buffer.data.get(arr)
            val msg = try { JSONObject(String(arr, Charsets.UTF_8)) } catch (e: Exception) { return }
            when (msg.optString("type")) {
                "input" -> handleInput(msg)
                "ping" -> sendJson(JSONObject().put("type", "pong").put("t", msg.opt("t")))
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
        when (msg.optString("event")) {
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

        @Synchronized
        private fun ensureFactory(context: Context) {
            if (factory != null) return
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(context.applicationContext)
                    .createInitializationOptions()
            )
            factory = PeerConnectionFactory.builder().createPeerConnectionFactory()
        }
    }
}

/** SdpObserver with no-op defaults so subclasses override only what they need. */
open class SimpleSdp : SdpObserver {
    override fun onCreateSuccess(desc: SessionDescription) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(error: String?) {}
    override fun onSetFailure(error: String?) {}
}
