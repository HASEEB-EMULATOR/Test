package com.example.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import com.example.model.HomeUiState
import com.example.model.RomInstallStage
import com.example.model.VmSystem
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.InputStream
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.zip.ZipInputStream

private val Context.appDataStore by preferencesDataStore(name = "haseeb_gaming_prefs")

class RomRepository(
    private val context: Context,
) {
    private val httpClient: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(20, TimeUnit.SECONDS)
        .readTimeout(120, TimeUnit.SECONDS)
        .followRedirects(true)
        .build()

    private val systemsRootDir: File
        get() = File(context.filesDir, "systems").apply { mkdirs() }

    private val downloadsCacheDir: File
        get() = File(context.cacheDir, "rom_downloads").apply { mkdirs() }

    private object PrefKeys {
        val FIRST_LAUNCH_OFFER_DISMISSED = booleanPreferencesKey("first_launch_offer_dismissed")
        val GITHUB_REPO_SLUG = stringPreferencesKey("github_repo_slug")
        val ROM_RELEASE_TAG = stringPreferencesKey("rom_release_tag")
    }

    val firstLaunchOfferDismissed: Flow<Boolean> =
        context.appDataStore.data.map { prefs -> prefs[PrefKeys.FIRST_LAUNCH_OFFER_DISMISSED] ?: false }

    val githubRepoSlug: Flow<String> =
        context.appDataStore.data.map { prefs ->
            prefs[PrefKeys.GITHUB_REPO_SLUG] ?: HomeUiState.DEFAULT_GITHUB_REPO
        }

    val romReleaseTag: Flow<String> =
        context.appDataStore.data.map { prefs ->
            prefs[PrefKeys.ROM_RELEASE_TAG] ?: HomeUiState.DEFAULT_ROM_TAG
        }

    suspend fun setFirstLaunchOfferDismissed(dismissed: Boolean) {
        context.appDataStore.edit { prefs ->
            prefs[PrefKeys.FIRST_LAUNCH_OFFER_DISMISSED] = dismissed
        }
    }

    suspend fun loadInstalledSystems(): List<VmSystem> = withContext(Dispatchers.IO) {
        val dirs = systemsRootDir.listFiles { file -> file.isDirectory } ?: return@withContext emptyList()
        dirs.mapNotNull { dir ->
            val metaFile = File(dir, "vm_system.json")
            if (!metaFile.exists()) return@mapNotNull null
            runCatching { parseVmSystem(JSONObject(metaFile.readText())) }.getOrNull()
        }.sortedByDescending { it.installedAtEpochMs }
    }

    suspend fun downloadVerifyAndImportBundledRom(
        onStage: (RomInstallStage) -> Unit,
    ): Result<VmSystem> = withContext(Dispatchers.IO) {
        runCatching {
            val repo = githubRepoSlug.first().trim()
            val tag = romReleaseTag.first().trim()

            onStage(RomInstallStage.Downloading(0, "Connecting to GitHub Releases…"))
            val releaseApiUrl = "https://api.github.com/repos/$repo/releases/tags/$tag"
            val releaseRequest = Request.Builder()
                .url(releaseApiUrl)
                .header("Accept", "application/vnd.github+json")
                .header("User-Agent", "HaseebGaming-Android/1.0")
                .build()

            val releaseJson = httpClient.newCall(releaseRequest).execute().use { resp ->
                if (!resp.isSuccessful) {
                    error("GitHub release '$tag' unavailable (HTTP ${resp.code}) at $repo")
                }
                JSONObject(resp.body?.string().orEmpty())
            }

            val assets = releaseJson.optJSONArray("assets")
                ?: error("Release '$tag' contains no downloadable ROM assets")

            var archiveUrl: String? = null
            var archiveName = "haseeb-gaming-a10-v1-arm64.zip"
            var shaAssetUrl: String? = null

            for (i in 0 until assets.length()) {
                val asset = assets.getJSONObject(i)
                val name = asset.optString("name")
                val url = asset.optString("browser_download_url")
                if (name.endsWith(".zip", ignoreCase = true)) {
                    archiveUrl = url
                    archiveName = name
                } else if (name.endsWith(".zip.sha256", ignoreCase = true)) {
                    shaAssetUrl = url
                }
            }

            val downloadUrl = archiveUrl ?: error("No .zip ROM archive found in release '$tag'")
            val expectedArchiveSha = shaAssetUrl?.let { fetchExpectedSha256(it) }

            val targetArchive = File(downloadsCacheDir, archiveName)
            streamDownload(downloadUrl, targetArchive) { percent, downloadedBytes, totalBytes ->
                val label = if (totalBytes > 0) {
                    "${formatBytes(downloadedBytes)} / ${formatBytes(totalBytes)}"
                } else {
                    formatBytes(downloadedBytes)
                }
                onStage(RomInstallStage.Downloading(percent, label))
            }

            onStage(RomInstallStage.VerifyingSha256)
            if (!expectedArchiveSha.isNullOrBlank()) {
                val actualSha = computeSha256(targetArchive)
                if (!actualSha.equals(expectedArchiveSha, ignoreCase = true)) {
                    targetArchive.delete()
                    error("Archive SHA-256 mismatch (expected ${expectedArchiveSha.take(12)}…, got ${actualSha.take(12)}…)")
                }
            }

            val imported = extractAndRegisterZip(
                zipFile = targetArchive,
                fallbackName = "Haseeb Gaming A10",
                onStage = onStage,
            )
            targetArchive.delete()
            setFirstLaunchOfferDismissed(true)
            imported
        }
    }

    suspend fun importFirmwareUri(
        uri: Uri,
        onStage: (RomInstallStage) -> Unit,
    ): Result<VmSystem> = withContext(Dispatchers.IO) {
        runCatching {
            val displayName = queryDisplayName(uri) ?: "imported_firmware.zip"
            onStage(RomInstallStage.Importing)
            val tempFile = File(downloadsCacheDir, "import_${System.currentTimeMillis()}_$displayName")
            context.contentResolver.openInputStream(uri)?.use { input ->
                FileOutputStream(tempFile).use { output -> input.copyTo(output) }
            } ?: error("Unable to open selected firmware file")

            val result = if (displayName.endsWith(".zip", ignoreCase = true)) {
                extractAndRegisterZip(
                    zipFile = tempFile,
                    fallbackName = displayName.removeSuffix(".zip"),
                    onStage = onStage,
                )
            } else {
                registerRawImage(tempFile, displayName)
            }
            tempFile.delete()
            result
        }
    }

    private fun extractAndRegisterZip(
        zipFile: File,
        fallbackName: String,
        onStage: (RomInstallStage) -> Unit,
    ): VmSystem {
        onStage(RomInstallStage.Extracting)
        val systemId = "vm_${UUID.randomUUID().toString().take(8)}"
        val systemDir = File(systemsRootDir, systemId).apply { mkdirs() }

        try {
            ZipInputStream(FileInputStream(zipFile).buffered()).use { zis ->
                var entry = zis.nextEntry
                while (entry != null) {
                    if (!entry.isDirectory) {
                        val safeName = File(entry.name).name
                        val outFile = File(systemDir, safeName)
                        if (!outFile.canonicalPath.startsWith(systemDir.canonicalPath)) {
                            error("Blocked unsafe zip entry path: ${entry.name}")
                        }
                        FileOutputStream(outFile).buffered().use { out ->
                            zis.copyTo(out)
                        }
                    }
                    zis.closeEntry()
                    entry = zis.nextEntry
                }
            }

            onStage(RomInstallStage.VerifyingSha256)
            val systemImg = File(systemDir, "system.img")
            val bootImg = File(systemDir, "boot.img")
            if (!systemImg.exists()) {
                error("Firmware archive does not contain system.img")
            }

            val manifestFile = File(systemDir, "manifest.json")
            var name = fallbackName
            var androidVersion = "10"
            var apiLevel = 29
            var arch = "arm64-v8a"
            var marker = ""
            var shaVerified = false
            var sysSha = computeSha256(systemImg)
            var bootSha = if (bootImg.exists()) computeSha256(bootImg) else ""

            if (manifestFile.exists()) {
                val manifest = JSONObject(manifestFile.readText())
                name = manifest.optString("name", fallbackName)
                androidVersion = manifest.optString("androidVersion", "10")
                apiLevel = manifest.optInt("apiLevel", 29)
                arch = manifest.optString("arch", "arm64-v8a")
                marker = manifest.optString("marker", "")

                val parts = manifest.optJSONObject("parts")
                if (parts != null) {
                    val expectedSysSha = parts.optJSONObject("system.img")?.optString("sha256").orEmpty()
                    if (expectedSysSha.isNotEmpty() && !expectedSysSha.equals(sysSha, ignoreCase = true)) {
                        error("system.img SHA-256 verification failed")
                    }
                    val expectedBootSha = parts.optJSONObject("boot.img")?.optString("sha256").orEmpty()
                    if (expectedBootSha.isNotEmpty() && bootImg.exists() && !expectedBootSha.equals(bootSha, ignoreCase = true)) {
                        error("boot.img SHA-256 verification failed")
                    }
                    shaVerified = expectedSysSha.isNotEmpty()
                }
            }

            onStage(RomInstallStage.Importing)
            val hasMagisk = bootImg.exists() && marker == HomeUiState.EXPECTED_ROM_MARKER
            val totalSize = systemImg.length() + (if (bootImg.exists()) bootImg.length() else 0L)
            val vmSystem = VmSystem(
                id = systemId,
                name = name,
                androidVersion = androidVersion,
                apiLevel = apiLevel,
                arch = arch,
                hasMagisk = hasMagisk,
                sha256Verified = shaVerified,
                romMarker = marker,
                systemImagePath = systemImg.absolutePath,
                bootImagePath = if (bootImg.exists()) bootImg.absolutePath else "",
                systemSha256 = sysSha,
                bootSha256 = bootSha,
                sizeBytes = totalSize,
                lastBootEpochMs = null,
                installedAtEpochMs = System.currentTimeMillis(),
            )
            saveVmSystemMetadata(systemDir, vmSystem)
            return vmSystem
        } catch (t: Throwable) {
            systemDir.deleteRecursively()
            throw t
        }
    }

    private fun registerRawImage(imageFile: File, displayName: String): VmSystem {
        val systemId = "vm_${UUID.randomUUID().toString().take(8)}"
        val systemDir = File(systemsRootDir, systemId).apply { mkdirs() }
        val targetImg = File(systemDir, "system.img")
        imageFile.copyTo(targetImg, overwrite = true)
        val sysSha = computeSha256(targetImg)

        val vmSystem = VmSystem(
            id = systemId,
            name = displayName.substringBeforeLast('.'),
            androidVersion = "10",
            apiLevel = 29,
            arch = "arm64-v8a",
            hasMagisk = false,
            sha256Verified = true,
            romMarker = "custom-import",
            systemImagePath = targetImg.absolutePath,
            bootImagePath = "",
            systemSha256 = sysSha,
            bootSha256 = "",
            sizeBytes = targetImg.length(),
            lastBootEpochMs = null,
            installedAtEpochMs = System.currentTimeMillis(),
        )
        saveVmSystemMetadata(systemDir, vmSystem)
        return vmSystem
    }

    private fun saveVmSystemMetadata(systemDir: File, vm: VmSystem) {
        val json = JSONObject().apply {
            put("id", vm.id)
            put("name", vm.name)
            put("androidVersion", vm.androidVersion)
            put("apiLevel", vm.apiLevel)
            put("arch", vm.arch)
            put("hasMagisk", vm.hasMagisk)
            put("sha256Verified", vm.sha256Verified)
            put("romMarker", vm.romMarker)
            put("systemImagePath", vm.systemImagePath)
            put("bootImagePath", vm.bootImagePath)
            put("systemSha256", vm.systemSha256)
            put("bootSha256", vm.bootSha256)
            put("sizeBytes", vm.sizeBytes)
            put("lastBootEpochMs", vm.lastBootEpochMs ?: JSONObject.NULL)
            put("installedAtEpochMs", vm.installedAtEpochMs)
        }
        File(systemDir, "vm_system.json").writeText(json.toString(2))
    }

    private fun parseVmSystem(json: JSONObject): VmSystem = VmSystem(
        id = json.getString("id"),
        name = json.getString("name"),
        androidVersion = json.getString("androidVersion"),
        apiLevel = json.getInt("apiLevel"),
        arch = json.getString("arch"),
        hasMagisk = json.optBoolean("hasMagisk", false),
        sha256Verified = json.optBoolean("sha256Verified", false),
        romMarker = json.optString("romMarker", ""),
        systemImagePath = json.getString("systemImagePath"),
        bootImagePath = json.optString("bootImagePath", ""),
        systemSha256 = json.optString("systemSha256", ""),
        bootSha256 = json.optString("bootSha256", ""),
        sizeBytes = json.optLong("sizeBytes", 0L),
        lastBootEpochMs = if (json.isNull("lastBootEpochMs")) null else json.optLong("lastBootEpochMs"),
        installedAtEpochMs = json.optLong("installedAtEpochMs", System.currentTimeMillis()),
    )

    private fun fetchExpectedSha256(url: String): String? = runCatching {
        val req = Request.Builder().url(url).build()
        httpClient.newCall(req).execute().use { resp ->
            if (!resp.isSuccessful) return@runCatching null
            resp.body?.string()?.trim()?.split(Regex("\\s+"))?.firstOrNull()
        }
    }.getOrNull()

    private fun streamDownload(
        url: String,
        target: File,
        onProgress: (percent: Int, downloadedBytes: Long, totalBytes: Long) -> Unit,
    ) {
        val req = Request.Builder().url(url).build()
        httpClient.newCall(req).execute().use { resp ->
            if (!resp.isSuccessful) {
                error("Download failed with HTTP ${resp.code}")
            }
            val body = resp.body ?: error("Empty download response")
            val total = body.contentLength()
            var downloaded = 0L
            var lastReportedPct = -1
            val buffer = ByteArray(64 * 1024)
            body.byteStream().use { input: InputStream ->
                FileOutputStream(target).use { output ->
                    while (true) {
                        val read = input.read(buffer)
                        if (read == -1) break
                        output.write(buffer, 0, read)
                        downloaded += read
                        val pct = if (total > 0) ((downloaded * 100L) / total).toInt().coerceIn(0, 100) else 0
                        if (pct != lastReportedPct) {
                            lastReportedPct = pct
                            onProgress(pct, downloaded, total)
                        }
                    }
                }
            }
        }
    }

    private fun computeSha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        val buffer = ByteArray(64 * 1024)
        FileInputStream(file).use { fis ->
            while (true) {
                val read = fis.read(buffer)
                if (read == -1) break
                digest.update(buffer, 0, read)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun queryDisplayName(uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }

    companion object {
        fun formatBytes(bytes: Long): String {
            if (bytes <= 0L) return "0 B"
            val kb = 1024.0
            val mb = kb * 1024.0
            val gb = mb * 1024.0
            return when {
                bytes >= gb -> "%.2f GB".format(bytes / gb)
                bytes >= mb -> "%.1f MB".format(bytes / mb)
                bytes >= kb -> "%.0f KB".format(bytes / kb)
                else -> "$bytes B"
            }
        }
    }
}
