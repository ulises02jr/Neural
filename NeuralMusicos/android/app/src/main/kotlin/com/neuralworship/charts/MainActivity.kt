package com.neuralworship.charts

import android.app.Activity
import android.content.Intent
import android.media.MediaPlayer
import android.net.Uri
import android.os.Bundle
import android.webkit.WebView
import android.webkit.WebViewClient
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

class MainActivity : FlutterActivity() {
    private lateinit var audio: MultiTrackAudio

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        audio = MultiTrackAudio(applicationContext)
        val messenger = flutterEngine.dartExecutor.binaryMessenger

        MethodChannel(messenger, "neural/audio").setMethodCallHandler { call, result ->
            val a = call.arguments as? Map<*, *> ?: emptyMap<String, Any>()
            when (call.method) {
                "load" -> {
                    @Suppress("UNCHECKED_CAST")
                    val stems = (a["stems"] as? List<Map<String, Any>>) ?: emptyList()
                    audio.load(stems) { ok, dur, err ->
                        runOnUiThread {
                            if (ok) result.success(dur)
                            else result.error("load", err, null)
                        }
                    }
                }
                "play" -> { audio.play((a["offset"] as? Double) ?: 0.0); result.success(null) }
                "pause" -> { audio.pause(); result.success(null) }
                "seek" -> { audio.seek((a["offset"] as? Double) ?: 0.0); result.success(null) }
                "setVolume" -> {
                    val id = a["id"] as? String
                    val v = (a["value"] as? Double)?.toFloat() ?: 1f
                    if (id != null) audio.setVolume(id, v)
                    result.success(null)
                }
                "position" -> result.success(audio.positionSec())
                "isPlaying" -> result.success(audio.isPlaying())
                "stop" -> { audio.stop(); result.success(null) }
                "cacheSize" -> result.success(audio.cacheBytes())
                "clearCache" -> { audio.clearCache(); result.success(null) }
                else -> result.notImplemented()
            }
        }

        MethodChannel(messenger, "neural/app").setMethodCallHandler { call, result ->
            val a = call.arguments as? Map<*, *> ?: emptyMap<String, Any>()
            when (call.method) {
                "openUrl" -> {
                    (a["url"] as? String)?.let {
                        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(it)))
                    }
                    result.success(null)
                }
                "openLive" -> {
                    (a["url"] as? String)?.let {
                        val i = Intent(this, LiveWebActivity::class.java)
                        i.putExtra("url", it)
                        startActivity(i)
                    }
                    result.success(null)
                }
                "prefsGet" -> result.success(prefsFile().let { if (it.exists()) it.readText() else "{}" })
                "prefsSet" -> {
                    (a["json"] as? String)?.let { prefsFile().writeText(it) }
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        }
    }

    private fun prefsFile(): File = File(applicationContext.filesDir, "neural_prefs.json")
}

/// WebView del sistema en vivo (LAN), dentro de la app.
class LiveWebActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val wv = WebView(this)
        wv.settings.javaScriptEnabled = true
        wv.settings.domStorageEnabled = true
        wv.webViewClient = WebViewClient()
        setContentView(wv)
        intent.getStringExtra("url")?.let { wv.loadUrl(it) }
    }
}

/// Motor de audio multipista para Android (un MediaPlayer por stem, arrancados juntos).
class MultiTrackAudio(private val ctx: android.content.Context) {
    private val players = HashMap<String, MediaPlayer>()
    private val volumes = HashMap<String, Float>()
    private var durationSec = 0.0
    private var refId: String? = null
    private val io = Executors.newSingleThreadExecutor()

    fun load(stems: List<Map<String, Any>>, cb: (Boolean, Double, String?) -> Unit) {
        stop()
        for (p in players.values) p.release()
        players.clear(); volumes.clear(); durationSec = 0.0; refId = null

        io.execute {
            try {
                val dir = cacheDir()
                for (stem in stems) {
                    val id = stem["id"] as? String ?: continue
                    val urlStr = stem["url"] as? String ?: continue
                    val key = (stem["key"] as? String) ?: id
                    val dest = File(dir, key)
                    if (!dest.exists()) descargar(urlStr, dest)
                    val mp = MediaPlayer()
                    mp.setDataSource(dest.absolutePath)
                    mp.prepare()
                    mp.setVolume(1f, 1f)
                    players[id] = mp
                    volumes[id] = 1f
                    val d = mp.duration / 1000.0
                    if (d > durationSec) durationSec = d
                    if (refId == null) refId = id
                }
                if (players.isEmpty()) cb(false, 0.0, "sin pistas")
                else cb(true, durationSec, null)
            } catch (e: Exception) {
                cb(false, 0.0, e.message)
            }
        }
    }

    private fun descargar(urlStr: String, dest: File) {
        val conn = URL(urlStr).openConnection() as HttpURLConnection
        conn.instanceFollowRedirects = true
        conn.connectTimeout = 20000
        conn.readTimeout = 20000
        conn.inputStream.use { input ->
            dest.outputStream().use { out -> input.copyTo(out) }
        }
        conn.disconnect()
    }

    fun play(offsetSec: Double) {
        val ms = (offsetSec * 1000).toInt()
        for (p in players.values) { try { p.seekTo(ms) } catch (_: Exception) {} }
        for (p in players.values) { try { p.start() } catch (_: Exception) {} }
    }

    fun pause() { for (p in players.values) { try { if (p.isPlaying) p.pause() } catch (_: Exception) {} } }

    fun seek(offsetSec: Double) {
        val ms = (offsetSec * 1000).toInt()
        for (p in players.values) { try { p.seekTo(ms) } catch (_: Exception) {} }
    }

    fun stop() {
        for (p in players.values) { try { if (p.isPlaying) p.pause(); p.seekTo(0) } catch (_: Exception) {} }
    }

    fun setVolume(id: String, v: Float) {
        volumes[id] = v
        players[id]?.let { try { it.setVolume(v, v) } catch (_: Exception) {} }
    }

    fun positionSec(): Double {
        val r = refId ?: return 0.0
        return try { (players[r]?.currentPosition ?: 0) / 1000.0 } catch (_: Exception) { 0.0 }
    }

    fun isPlaying(): Boolean = players.values.any { try { it.isPlaying } catch (_: Exception) { false } }

    private fun cacheDir(): File {
        val d = File(ctx.filesDir, "ensayo_cache")
        if (!d.exists()) d.mkdirs()
        return d
    }

    fun cacheBytes(): Long {
        val d = cacheDir()
        return d.listFiles()?.sumOf { it.length() } ?: 0L
    }

    fun clearCache() {
        stop()
        cacheDir().listFiles()?.forEach { it.delete() }
    }
}
