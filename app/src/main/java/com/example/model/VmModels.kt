package com.example.model

data class VmSystem(
    val id: String,
    val name: String,
    val androidVersion: String,
    val apiLevel: Int,
    val arch: String,
    val hasMagisk: Boolean,
    val sha256Verified: Boolean,
    val romMarker: String,
    val systemImagePath: String,
    val bootImagePath: String,
    val systemSha256: String,
    val bootSha256: String,
    val sizeBytes: Long,
    val lastBootEpochMs: Long?,
    val installedAtEpochMs: Long,
)

sealed interface RomInstallStage {
    data object Idle : RomInstallStage
    data class Downloading(
        val progressPercent: Int,
        val downloadedLabel: String,
    ) : RomInstallStage
    data object VerifyingSha256 : RomInstallStage
    data object Extracting : RomInstallStage
    data object Importing : RomInstallStage
    data class Completed(val systemName: String) : RomInstallStage
    data class Failed(val reason: String) : RomInstallStage
}

data class HomeUiState(
    val systems: List<VmSystem> = emptyList(),
    val showFirstLaunchOffer: Boolean = true,
    val installStage: RomInstallStage = RomInstallStage.Idle,
    val selectedSystemForInspect: VmSystem? = null,
    val githubRepoSlug: String = DEFAULT_GITHUB_REPO,
    val romReleaseTag: String = DEFAULT_ROM_TAG,
) {
    companion object {
        const val DEFAULT_GITHUB_REPO = "haseeb-gaming/haseeb-gaming-android"
        const val DEFAULT_ROM_TAG = "rom-a10-v1"
        const val EXPECTED_ROM_MARKER = "haseeb-gaming-a10-v1"
    }
}
