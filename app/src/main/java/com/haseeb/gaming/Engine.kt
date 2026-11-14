package com.haseeb.gaming

import android.view.Surface

object Engine {
    @Volatile
    var nativeLibraryLoaded: Boolean = false
        private set

    @Volatile
    var nativeLoadError: String? = null
        private set

    init {
        try {
            System.loadLibrary("haseeb_vm")
            nativeLibraryLoaded = true
        } catch (t: Throwable) {
            nativeLibraryLoaded = false
            nativeLoadError = t.message ?: "Failed to load libhaseeb_vm.so"
        }
    }

    external fun setPath(path: String)

    external fun setNativeLibDir(libDir: String)

    external fun configureGuestProps(
        model: String,
        brand: String,
        manufacturer: String,
        device: String,
        name: String,
        marketname: String,
        fingerprint: String,
        navBarOff: Boolean,
        magiskPromptEnabled: Boolean,
        targetFps: Int,
    )

    external fun start(api: Int, width: Int, height: Int, dpi: Int, ramMb: Int): Boolean

    external fun attachSurface(surface: Surface?)

    external fun sendTouch(action: Int, x: Int, y: Int, id: Int)

    external fun sendKey(code: Int, down: Boolean)

    external fun sendText(text: String)

    external fun readLog(maxBytes: Int): String

    external fun getGuestFps(): Float

    external fun isRunning(): Boolean

    external fun stop()
}
