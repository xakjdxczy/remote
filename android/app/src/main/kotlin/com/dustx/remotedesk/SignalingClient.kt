package com.dustx.remotedesk

import android.util.Log
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject
import java.util.concurrent.TimeUnit

/** Thin WebSocket signaling client speaking the RemoteDesk protocol as a host. */
class SignalingClient(
    private val url: String,
    private val hostname: String,
    private val osName: String,
    private val cb: Callbacks,
) {
    interface Callbacks {
        fun onRegistered(deviceId: String, password: String)
        fun onSessionStart(msg: JSONObject)
        fun onSignal(msg: JSONObject)
        fun onSessionEnd(reason: String)
        fun onClosed()
    }

    private val client = OkHttpClient.Builder()
        .pingInterval(20, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .build()
    private var ws: WebSocket? = null
    val password = Ids.genPassword()

    fun connect() {
        val req = Request.Builder().url(url).build()
        ws = client.newWebSocket(req, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                val reg = JSONObject()
                    .put("type", "register")
                    .put("role", "host")
                    .put("hostname", hostname)
                    .put("os", osName)
                    .put("temp_password", password)
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

    private fun handle(text: String) {
        val msg = try { JSONObject(text) } catch (e: Exception) { return }
        when (msg.optString("type")) {
            "registered" -> cb.onRegistered(msg.optString("device_id"), msg.optString("temp_password"))
            "session_start" -> cb.onSessionStart(msg)
            "signal" -> cb.onSignal(msg)
            "session_end" -> cb.onSessionEnd(msg.optString("reason"))
        }
    }

    fun close() {
        try { ws?.close(1000, "bye") } catch (_: Exception) {}
    }

    companion object { private const val TAG = "RD.Signal" }
}
