package com.nvshen.chmp4

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.widget.EditText
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity

class SplashActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_splash)

        if (LicenseManager.isActivated(this)) {
            goToMain()
        } else {
            showActivationDialog()
        }
    }

    private fun showActivationDialog() {
        val input = EditText(this).apply {
            hint = "VCAM-XXXX-XXXX-XXXX"
            setSingleLine()
        }

        AlertDialog.Builder(this)
            .setTitle("激活虚拟摄像头")
            .setMessage("请输入激活码 (CDKey) 以使用本软件\n\n测试码: VCAM-DEMO-TEST-0001")
            .setView(input)
            .setCancelable(false)
            .setPositiveButton("激活") { _, _ ->
                val key = input.text.toString().trim()
                if (LicenseManager.activate(this, key)) {
                    Toast.makeText(this, "激活成功！", Toast.LENGTH_SHORT).show()
                    goToMain()
                } else {
                    Toast.makeText(this, "激活码无效，请重试", Toast.LENGTH_LONG).show()
                    showActivationDialog()
                }
            }
            .setNegativeButton("退出") { _, _ ->
                finish()
            }
            .show()
    }

    private fun goToMain() {
        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }
}
