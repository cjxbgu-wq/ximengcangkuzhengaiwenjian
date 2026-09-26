package com.nvshen.chmp4

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.graphics.PixelFormat
import android.os.IBinder
import android.view.Gravity
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import android.widget.ImageButton
import android.widget.TextView
import androidx.core.app.NotificationCompat

class FloatingWindowService : Service() {

    private lateinit var wm: WindowManager
    private var floatView: View? = null
    private var lastX = 0f
    private var lastY = 0f
    private var initX = 0
    private var initY = 0

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForeground(2, buildNotification())
        showFloatingWindow()
    }

    private fun buildNotification(): Notification {
        val pi = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        return NotificationCompat.Builder(this, App.CHANNEL_ID)
            .setContentTitle("虚拟摄像头")
            .setContentText("悬浮控制窗口运行中")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pi)
            .build()
    }

    private fun showFloatingWindow() {
        wm = getSystemService(WINDOW_SERVICE) as WindowManager

        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = 100; y = 300
        }

        floatView = LayoutInflater.from(this).inflate(R.layout.layout_floating, null)

        val tvState  = floatView!!.findViewById<TextView>(R.id.tvFloatState)
        val btnClose = floatView!!.findViewById<ImageButton>(R.id.btnFloatClose)
        val btnMirror = floatView!!.findViewById<ImageButton>(R.id.btnFloatMirror)

        tvState.text = "VCam ●"

        btnClose.setOnClickListener {
            stopSelf()
            // Also signal main to stop
            sendBroadcast(Intent("com.nvshen.chmp4.STOP_VCAM"))
        }

        btnMirror.setOnClickListener {
            sendBroadcast(Intent("com.nvshen.chmp4.TOGGLE_MIRROR"))
        }

        // Drag to move
        floatView!!.setOnTouchListener { v, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastX = event.rawX; lastY = event.rawY
                    initX = params.x;   initY = params.y
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    params.x = initX + (event.rawX - lastX).toInt()
                    params.y = initY + (event.rawY - lastY).toInt()
                    wm.updateViewLayout(floatView, params)
                    true
                }
                else -> false
            }
        }

        wm.addView(floatView, params)
    }

    override fun onDestroy() {
        floatView?.let { wm.removeView(it) }
        floatView = null
        super.onDestroy()
    }
}
