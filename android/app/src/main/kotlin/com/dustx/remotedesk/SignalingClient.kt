package com.dustx.remotedesk

import android.content.Context
import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/** WebSocket signaling: every Android client is both host and viewer. */
class SignalingClient(
    private val appContext: Context,
    private val url: String,
    private val hostname: String,
    private val osName: String,
    private val cb: Callbacks,
) {
    interface Callbacks {
        fun onRegistered(deviceId: String, password: String)
        fun onIncomingCall(msg: JSONObject)
        fun onCallPending(msg: JSONObject)
        fun onSessionStart(msg: JSONObject)
        fun onSignal(msg: JSONObject)
        fun onSessionEnd(reason: String)
        fun onPassword(password: String)
        fun onClosed()
    }

    private val client = OkHttpClient.Builder()
        .pingInterval(20, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .build()
    private var ws: WebSocket? = null

    fun connect() {
        val req = Request.Builder().url(url).build()
        ws = client.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                val reg = JSONObject()
                    .put("type", "register")
                    .put("role", "host")
                    .put("hostname", hostname)
                    .put("os", osName)
                    .put("device_id", DeviceStore.deviceId(appContext))
                    .put("temp_password", DeviceStore.password(appContext))
                webSocket.send(reg.toString())
            }

            override fun onMessage(webSocket: WebSocket, text: String) = handle(text)

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) = cb.onClosed()

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                Log.w(TAG, "ws failure: ${t.message}")
                cb.onClosed()
            }
        })
    }

    fun send(obj: JSONObject) {
        ws?.send(obj.toString())
    }

    fun connectTo(deviceId: String, password: String, name: String) {
        send(
            JSONObject()
                .put("type", "connect")
                .put("device_id", deviceId)
                .put("password", password)
                .put("name", name)
        )
    }

    fun answerCall(sessionId: String, ok: Boolean) {
        send(JSONObject().put("type", "auth_result").put("session_id", sessionId).put("ok", ok))
    }

    fun refreshPassword() {
        send(JSONObject().put("type", "refresh_password"))
    }

    fun hangup(reason: String = "hangup") {
        send(JSONObject().put("type", "hangup").put("reason", reason))
    }

    private fun handle(text: String) {
        val msg = try { JSONObject(text) } catch (e: Exception) { return }
        when (msg.optString("type")) {
            "registered" -> {
                val id = msg.optString("device_id")
                val pw = msg.optString("temp_password")
                DeviceStore.save(appContext, id, pw)
                cb.onRegistered(id, pw)
            }
            "password" -> {
                val pw = msg.optString("temp_password")
                DeviceStore.save(appContext, DeviceStore.deviceId(appContext), pw)
                cb.onPassword(pw)
            }
            "incoming_call" -> cb.onIncomingCall(msg)
            "call_pending" -> cb.onCallPending(msg)
            "session_start" -> cb.onSessionStart(msg)
            "signal" -> cb.onSignal(msg)
            "session_end" -> cb.onSessionEnd(msg.optString("reason"))
            "auth_failed" -> cb.onSessionEnd(msg.optString("message").ifEmpty { "auth_failed" })
        }
    }

    fun close() {
        try { ws?.close(1000, "bye") } catch (_: Exception) {}
    }

    companion object { private const val TAG = "RD.Signal" }
}
