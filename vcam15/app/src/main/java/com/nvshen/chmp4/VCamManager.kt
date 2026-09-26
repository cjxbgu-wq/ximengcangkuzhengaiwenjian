package com.nvshen.chmp4

import android.content.Context
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaPlayer
import android.net.Uri
import android.util.Log
import kotlinx.coroutines.*
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer

class VCamManager(private val context: Context) {

    companion object {
        private const val TAG = "VCamManager"
        private const val FRAME_W = 1280
        private const val FRAME_H = 720

        init { System.loadLibrary("vcam") }

        @JvmStatic external fun nativeInit(width: Int, height: Int): Boolean
        @JvmStatic external fun nativeSetEnabled(enabled: Boolean)
        @JvmStatic external fun nativeSetMirror(mirror: Boolean)
        @JvmStatic external fun nativeWriteFrame(data: ByteArray, width: Int, height: Int): Boolean
        @JvmStatic external fun nativeDestroy()
        @JvmStatic external fun nativeGetReadSeq(): Int
        @JvmStatic external fun nativeGetWriteSeq(): Int
    }

    enum class Source { NONE, VIDEO_FILE, RTMP }

    var currentSource = Source.NONE
    var vcamActive = false
        private set

    private var decodeJob: Job? = null
    private var mediaPlayer: MediaPlayer? = null
    private val scope = CoroutineScope(Dispatchers.Default + SupervisorJob())

    // ── Start / Stop ────────────────────────────────────────────────────────
    fun start(): Boolean {
        if (!nativeInit(FRAME_W, FRAME_H)) {
            Log.e(TAG, "nativeInit failed — shared memory creation error")
            return false
        }
        // Inject hook library into cameraserver
        val ok = runInjector()
        if (!ok) Log.w(TAG, "Injector failed — hook may not be installed")
        nativeSetEnabled(true)
        vcamActive = true
        Log.i(TAG, "VCam started (injector=$ok)")
        return true
    }

    fun stop() {
        nativeSetEnabled(false)
        stopSource()
        vcamActive = false
        nativeDestroy()
        Log.i(TAG, "VCam stopped")
    }

    fun setMirror(mirror: Boolean) = nativeSetMirror(mirror)

    // ── Source selection ────────────────────────────────────────────────────
    fun startVideoFile(path: String) {
        stopSource()
        currentSource = Source.VIDEO_FILE
        decodeJob = scope.launch { decodeVideoLoop(path) }
    }

    fun startRtmp(url: String) {
        stopSource()
        currentSource = Source.RTMP
        decodeJob = scope.launch { playRtmpLoop(url) }
    }

    fun stopSource() {
        decodeJob?.cancel()
        decodeJob = null
        try { mediaPlayer?.stop(); mediaPlayer?.release() } catch (_: Exception) {}
        mediaPlayer = null
        currentSource = Source.NONE
    }

    // ── Local video decode loop (MediaCodec buffer mode) ────────────────────
    private suspend fun decodeVideoLoop(path: String) {
        while (currentCoroutineContext().isActive) {
            try {
                decodeVideoOnce(path)
                delay(100) // brief pause before looping
            } catch (e: CancellationException) {
                break
            } catch (e: Exception) {
                Log.e(TAG, "Decode error: ${e.message}")
                delay(2000)
            }
        }
    }

    private suspend fun decodeVideoOnce(path: String) {
        val extractor = MediaExtractor()
        extractor.setDataSource(path)

        var videoTrack = -1
        var videoFormat: MediaFormat? = null
        for (i in 0 until extractor.trackCount) {
            val fmt = extractor.getTrackFormat(i)
            if (fmt.getString(MediaFormat.KEY_MIME)?.startsWith("video/") == true) {
                videoTrack = i
                videoFormat = fmt
                break
            }
        }
        if (videoTrack < 0 || videoFormat == null) {
            Log.e(TAG, "No video track: $path")
            extractor.release()
            return
        }

        extractor.selectTrack(videoTrack)
        val mime = videoFormat.getString(MediaFormat.KEY_MIME)!!
        val srcW = videoFormat.getInteger(MediaFormat.KEY_WIDTH)
        val srcH = videoFormat.getInteger(MediaFormat.KEY_HEIGHT)

        // Buffer mode: no surface, decoder returns raw YUV data
        val decoder = MediaCodec.createDecoderByType(mime)
        decoder.configure(videoFormat, null, null, 0)
        decoder.start()

        val info        = MediaCodec.BufferInfo()
        var inputDone   = false
        var frameW      = srcW
        var frameH      = srcH
        var stride      = srcW
        var sliceH      = srcH
        var colorFmt    = 0

        // ~30 fps pacing
        val framePaceMs = 33L

        try {
            while (currentCoroutineContext().isActive) {
                // Feed input buffers
                if (!inputDone) {
                    val inIdx = decoder.dequeueInputBuffer(5_000)
                    if (inIdx >= 0) {
                        val inBuf = decoder.getInputBuffer(inIdx)!!
                        inBuf.clear()
                        val size = extractor.readSampleData(inBuf, 0)
                        if (size < 0) {
                            decoder.queueInputBuffer(inIdx, 0, 0, 0,
                                MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            decoder.queueInputBuffer(inIdx, 0, size,
                                extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }

                // Get output buffers
                val outIdx = decoder.dequeueOutputBuffer(info, 5_000)
                when {
                    outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val fmt = decoder.outputFormat
                        frameW  = fmt.getInteger(MediaFormat.KEY_WIDTH)
                        frameH  = fmt.getInteger(MediaFormat.KEY_HEIGHT)
                        stride  = try { fmt.getInteger(MediaFormat.KEY_STRIDE) } catch (_: Exception) { frameW }
                        sliceH  = try { fmt.getInteger(MediaFormat.KEY_SLICE_HEIGHT) } catch (_: Exception) { frameH }
                        colorFmt = try { fmt.getInteger(MediaFormat.KEY_COLOR_FORMAT) } catch (_: Exception) { 0 }
                        Log.d(TAG, "Output format: ${frameW}x${frameH} stride=$stride sliceH=$sliceH fmt=$colorFmt")
                    }
                    outIdx >= 0 -> {
                        val outBuf = decoder.getOutputBuffer(outIdx)
                        if (outBuf != null && frameW > 0 && frameH > 0) {
                            val rgba = yuv2rgba(outBuf, frameW, frameH, stride, sliceH)
                            nativeWriteFrame(rgba, frameW, frameH)
                        }
                        decoder.releaseOutputBuffer(outIdx, false)

                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) {
                            // Seek to beginning for loop
                            extractor.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC)
                            decoder.flush()
                            inputDone = false
                        }
                    }
                }
                delay(framePaceMs)
            }
        } finally {
            runCatching { decoder.stop(); decoder.release() }
            extractor.release()
        }
    }

    // Simplified NV12/NV21/YUV420 → RGBA conversion
    // Works for the most common decoder output formats on Android
    private fun yuv2rgba(buf: ByteBuffer, w: Int, h: Int, stride: Int, sliceH: Int): ByteArray {
        buf.rewind()
        val rgba = ByteArray(w * h * 4)
        val ySize  = stride * sliceH
        val uvOff  = ySize  // U/V planes start here for NV12; exact offset depends on format

        var idx = 0
        for (row in 0 until h) {
            val yRowOff = row * stride
            val uvRowOff = uvOff + (row / 2) * stride
            for (col in 0 until w) {
                val yPos  = yRowOff  + col
                val uvPos = uvRowOff + (col and 1.inv()) // align to 2

                val yByte  = if (yPos  < buf.limit()) buf.get(yPos).toInt()  and 0xFF else 0
                val uByte  = if (uvPos < buf.limit()) buf.get(uvPos).toInt() and 0xFF else 128
                val vByte  = if (uvPos + 1 < buf.limit()) buf.get(uvPos + 1).toInt() and 0xFF else 128

                val Y = yByte
                val U = uByte - 128
                val V = vByte - 128

                val r = (Y + 1.402f  * V).toInt().coerceIn(0, 255)
                val g = (Y - 0.344f  * U - 0.714f * V).toInt().coerceIn(0, 255)
                val b = (Y + 1.772f  * U).toInt().coerceIn(0, 255)

                rgba[idx++] = r.toByte()
                rgba[idx++] = g.toByte()
                rgba[idx++] = b.toByte()
                rgba[idx++] = 0xFF.toByte()
            }
        }
        return rgba
    }

    // ── RTMP via MediaPlayer (works on Qualcomm devices) ────────────────────
    private suspend fun playRtmpLoop(url: String) {
        withContext(Dispatchers.Main) {
            try {
                val mp = MediaPlayer()
                mediaPlayer = mp
                // Use a SurfaceTexture to intercept frames
                // For simplicity: rely on Qualcomm's built-in RTMP support in MediaPlayer
                // Frame extraction via Surface + ImageReader requires API gymnastics;
                // here we just play the stream and the hook library replaces camera with
                // whatever was last written by local video decode as a fallback.
                // A proper RTMP frame grab requires custom RTMP client (future enhancement).
                mp.setDataSource(url)
                mp.isLooping = true
                mp.prepare()
                mp.start()
                Log.i(TAG, "RTMP MediaPlayer started: $url")

                while (currentCoroutineContext().isActive) delay(500)

                runCatching { mp.stop(); mp.release() }
                mediaPlayer = null
            } catch (e: Exception) {
                Log.e(TAG, "RTMP error: ${e.message}")
            }
        }
    }

    // ── Run injector as root ─────────────────────────────────────────────────
    private fun runInjector(): Boolean {
        // Extract injector executable from assets
        val injectorFile = File(context.filesDir, "vcam_injector")
        val hookLib = File(context.applicationInfo.nativeLibraryDir, "libvcam_hook.so")

        try {
            context.assets.open("bin/vcam_injector").use { inp ->
                FileOutputStream(injectorFile).use { out -> inp.copyTo(out) }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Cannot extract injector from assets: ${e.message}")
            return false
        }

        if (!hookLib.exists()) {
            Log.e(TAG, "Hook library not found at: ${hookLib.absolutePath}")
            return false
        }

        val injPath  = injectorFile.absolutePath
        val hookPath = hookLib.absolutePath

        // Root commands:
        // 1. Disable SELinux enforcement (required for cross-process injection)
        // 2. Make injector executable
        // 3. Find cameraserver PID and inject
        val script = """
setenforce 0
chmod 755 "$injPath"
PID=$(pgrep cameraserver 2>/dev/null | head -1)
if [ -z "${'$'}PID" ]; then
  echo "ERROR: cameraserver not found" >&2
  exit 1
fi
echo "Injecting libvcam_hook.so into cameraserver (PID=${'$'}PID)"
"$injPath" "$hookPath" "${'$'}PID"
""".trimIndent()

        return try {
            val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", script))
            val exitCode = proc.waitFor()
            val stdout = proc.inputStream.bufferedReader().readText().trim()
            val stderr = proc.errorStream.bufferedReader().readText().trim()
            if (stdout.isNotBlank()) Log.i(TAG, "Injector: $stdout")
            if (stderr.isNotBlank()) Log.e(TAG, "Injector stderr: $stderr")
            Log.i(TAG, "Injector exit code: $exitCode")
            exitCode == 0
        } catch (e: Exception) {
            Log.e(TAG, "Failed to run injector: ${e.message}")
            false
        }
    }
}
