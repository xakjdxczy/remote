package com.dustx.remotedesk

import android.content.Intent
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity

/** Old camera page / dustcam:// entry: jump into the camera tab. */
class CameraLinkActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        startActivity(
            Intent(this, MainActivity::class.java).apply {
                data = intent.data
                putExtras(intent)
                putExtra(MainActivity.EXTRA_TAB, MainActivity.TAB_CAMERA)
                addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
            }
        )
        finish()
    }
}
