package com.nvshen.chmp4

import android.content.Context
import java.security.MessageDigest

/**
 * CDKey license manager.
 * Keys are validated client-side using SHA-256.
 * Valid keys: SHA256("VCAM15:" + key) must start with one of the valid prefixes.
 *
 * Pre-generated valid keys (give these to your friends):
 *   VCAM-A3F9-D28B-E501
 *   VCAM-B7C2-F410-9A3E
 *   VCAM-C5E8-2D71-B6F4
 *   VCAM-D1A6-E934-C28B
 *   VCAM-E04F-7B15-A39D
 *   VCAM-F293-1C86-D740
 *   VCAM-DEMO-TEST-0001
 *   VCAM-DEMO-TEST-0002
 */
object LicenseManager {

    private const val PREF_NAME = "vcam_license"
    private const val PREF_KEY  = "activated_key"
    private const val SALT      = "VCAM15:"

    // SHA-256("VCAM15:" + KEY_UPPERCASE) for each valid key
    // Keys:  VCAM-A3F9-D28B-E501  /  VCAM-B7C2-F410-9A3E  /  VCAM-C5E8-2D71-B6F4
    //        VCAM-D1A6-E934-C28B  /  VCAM-E04F-7B15-A39D  /  VCAM-F293-1C86-D740
    //        VCAM-DEMO-TEST-0001  /  VCAM-DEMO-TEST-0002
    private val VALID_HASHES = setOf(
        "8cce5d4932313eed184450ed6b368282b9d91dd205f5cef1328bdf541e8370da",
        "e720ffd69e9ab3fc84a22285722d8f17ea5e23645b3c801ab722d0f257bb2fda",
        "cd8d94be3b841f5a910fa24bfdec668e621fc0bf1d95b5f672a48b59cf45a8e2",
        "121dac5a138ef22f980914ca43f99fe1801dea62453bb736b6bacac6dd58d3bb",
        "9d44192e3df3dc171bb1c74a9e6127213f6467d94ad63e7ec7ac1b1d19ea4009",
        "b49af0ae117b2696b57cce6d01c62265fef368b91a1ca25c89779a71ff38a9c1",
        "2f9ffb9a4ecfaa2a3bf8a4dfb35d36ad1aa75b02001adac4449d79f8dfce04a3",
        "d190808fbec126d1fa7c2c4fec172ddd3f331a95cc8c56ea2176866ef9003773",
    )

    private fun sha256hex(input: String): String {
        val digest = MessageDigest.getInstance("SHA-256")
        val bytes  = digest.digest(input.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it) }
    }

    fun isActivated(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
        val saved = prefs.getString(PREF_KEY, null) ?: return false
        return isValidKey(saved)
    }

    fun activate(context: Context, key: String): Boolean {
        val normalized = key.trim().uppercase()
        if (!isValidKey(normalized)) return false
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_KEY, normalized)
            .apply()
        return true
    }

    fun isValidKey(key: String): Boolean {
        val hash = sha256hex("$SALT${key.trim().uppercase()}")
        // Check against known valid hashes
        if (hash in VALID_HASHES) return true
        // Also accept: any key whose hash starts with "vcam" (development mode — remove in production)
        return false
    }

    fun deactivate(context: Context) {
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
            .edit().remove(PREF_KEY).apply()
    }
}
