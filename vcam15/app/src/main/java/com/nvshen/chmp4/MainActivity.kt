package com.nvshen.chmp4

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    companion object {
        private const val REQ_PICK_VIDEO  = 1001
        private const val REQ_PERMISSIONS = 1002
    }

    private lateinit var vcam: VCamManager

    // UI references (set in onCreate via findViewById)
    private lateinit var tvStatus:     TextView
    private lateinit var tvSource:     TextView
    private lateinit var btnStart:     Button
    private lateinit var btnStop:      Button
    private lateinit var btnPickFile:  Button
    private lateinit var btnRtmp:      Button
    private lateinit var btnFloat:     Button
    private lateinit var switchMirror: Switch

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        vcam = VCamManager(this)

        tvStatus    = findViewById(R.id.tvStatus)
        tvSource    = findViewById(R.id.tvSource)
        btnStart    = findViewById(R.id.btnStart)
        btnStop     = findViewById(R.id.btnStop)
        btnPickFile = findViewById(R.id.btnPickFile)
        btnRtmp     = findViewById(R.id.btnRtmp)
        btnFloat    = findViewById(R.id.btnFloat)
        switchMirror = findViewById(R.id.switchMirror)

        btnStop.isEnabled = false

        btnStart.setOnClickListener { startVCam() }
        btnStop.setOnClickListener  { stopVCam()  }

        btnPickFile.setOnClickListener {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                type = "video/*"
                addCategory(Intent.CATEGORY_OPENABLE)
            }
            startActivityForResult(intent, REQ_PICK_VIDEO)
        }

        btnRtmp.setOnClickListener { showRtmpDialog() }

        switchMirror.setOnCheckedChangeListener { _, checked ->
            vcam.setMirror(checked)
        }

        btnFloat.setOnClickListener {
            if (android.provider.Settings.canDrawOverlays(this)) {
                startService(Intent(this, FloatingWindowService::class.java))
                Toast.makeText(this, "悬浮窗已启动", Toast.LENGTH_SHORT).show()
            } else {
                val intent = Intent(android.provider.Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:$packageName"))
                startActivity(intent)
                Toast.makeText(this, "请授予悬浮窗权限", Toast.LENGTH_LONG).show()
            }
        }

        checkPermissions()
    }

    private fun checkPermissions() {
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_MEDIA_VIDEO)
                != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.READ_MEDIA_VIDEO)
        } else {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED)
                perms.add(Manifest.permission.READ_EXTERNAL_STORAGE)
        }
        if (perms.isNotEmpty())
            ActivityCompat.requestPermissions(this, perms.toTypedArray(), REQ_PERMISSIONS)
    }

    private fun startVCam() {
        if (vcam.currentSource == VCamManager.Source.NONE) {
            Toast.makeText(this, "请先选择视频来源", Toast.LENGTH_SHORT).show()
            return
        }
        lifecycleScope.launch {
            val ok = vcam.start()
            runOnUiThread {
                if (ok) {
                    tvStatus.text = "状态: 运行中 ✓"
                    btnStart.isEnabled = false
                    btnStop.isEnabled  = true
                } else {
                    tvStatus.text = "状态: 启动失败"
                    Toast.makeText(this@MainActivity, "启动失败，请确认已授予Root权限", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    private fun stopVCam() {
        vcam.stop()
        tvStatus.text = "状态: 已停止"
        btnStart.isEnabled = true
        btnStop.isEnabled  = false
        stopService(Intent(this, FloatingWindowService::class.java))
    }

    private fun showRtmpDialog() {
        val input = EditText(this).apply {
            hint = "rtmp://服务器地址/直播间"
            setSingleLine()
        }
        AlertDialog.Builder(this)
            .setTitle("RTMP输入流")
            .setMessage("输入RTMP推流地址（拉流地址）")
            .setView(input)
            .setPositiveButton("确定") { _, _ ->
                val url = input.text.toString().trim()
                if (url.startsWith("rtmp://") || url.startsWith("rtmps://")) {
                    vcam.startRtmp(url)
                    tvSource.text = "来源: RTMP\n$url"
                } else {
                    Toast.makeText(this, "请输入有效的RTMP地址", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_PICK_VIDEO && resultCode == Activity.RESULT_OK) {
            val uri = data?.data ?: return
            val path = getRealPath(uri) ?: uri.toString()
            vcam.startVideoFile(path)
            val name = getFileName(uri) ?: path
            tvSource.text = "来源: 本地视频\n$name"
        }
    }

    private fun getRealPath(uri: Uri): String? {
        return try {
            contentResolver.openFileDescriptor(uri, "r") ?: return null
            // For SAF URIs, create a cached copy
            val name = getFileName(uri) ?: "video.mp4"
            val cacheFile = java.io.File(cacheDir, name)
            contentResolver.openInputStream(uri)?.use { inp ->
                cacheFile.outputStream().use { out -> inp.copyTo(out) }
            }
            cacheFile.absolutePath
        } catch (e: Exception) {
            null
        }
    }

    private fun getFileName(uri: Uri): String? {
        var name: String? = null
        contentResolver.query(uri, null, null, null, null)?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (cursor.moveToFirst() && idx >= 0) name = cursor.getString(idx)
        }
        return name
    }

    override fun onDestroy() {
        super.onDestroy()
        if (isFinishing) vcam.stop()
    }
}
