package com.haseeb.emulator

import android.graphics.Color
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.haseeb.emulator.ui.home.HomeScreen
import com.haseeb.emulator.ui.theme.HaseebGamingTheme
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        installCrashLogger()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
        )
        setContent {
            HaseebGamingTheme {
                HomeScreen()
            }
        }
    }

    /** writes the crash to files/crash.txt so it can be read without adb */
    private fun installCrashLogger() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, error ->
            runCatching {
                val buffer = StringWriter()
                error.printStackTrace(PrintWriter(buffer))
                File(filesDir, "crash.txt").writeText(
                    "thread: ${thread.name}\nversion: 1\n\n$buffer"
                )
            }
            previous?.uncaughtException(thread, error)
        }
    }
}
