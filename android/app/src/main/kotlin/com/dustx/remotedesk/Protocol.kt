package com.dustx.remotedesk

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Wire protocol shared with the Python host / web viewer.
 *  Binary frame: type(u8=1) + width(u16) + height(u16) + ts_ms(u64) + jpeg  (big-endian).
 */
object Protocol {
    const val FRAME: Byte = 1

    fun packFrame(jpeg: ByteArray, width: Int, height: Int, tsMs: Long): ByteArray {
        val buf = ByteBuffer.allocate(13 + jpeg.size).order(ByteOrder.BIG_ENDIAN)
        buf.put(FRAME)
        buf.putShort(width.toShort())
        buf.putShort(height.toShort())
        buf.putLong(tsMs)
        buf.put(jpeg)
        return buf.array()
    }
}
