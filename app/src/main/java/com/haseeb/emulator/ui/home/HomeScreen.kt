package com.haseeb.emulator.ui.home

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.CheckCircle
import androidx.compose.material.icons.rounded.Close
import androidx.compose.material.icons.rounded.Download
import androidx.compose.material.icons.rounded.ErrorOutline
import androidx.compose.material.icons.rounded.Memory
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Security
import androidx.compose.material.icons.rounded.SportsEsports
import androidx.compose.material.icons.rounded.Tune
import androidx.compose.material.icons.rounded.Verified
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.haseeb.emulator.R
import com.haseeb.emulator.data.RomRepository
import com.haseeb.emulator.model.RomInstallStage
import com.haseeb.emulator.model.VmSystem
import com.haseeb.emulator.ui.components.GlassCard
import com.haseeb.emulator.ui.components.SectionHeader
import com.haseeb.emulator.ui.components.StatusPill
import com.haseeb.emulator.ui.components.TealGlowButton
import com.haseeb.emulator.ui.theme.CyanHighlight
import com.haseeb.emulator.ui.theme.ErrorCoral
import com.haseeb.emulator.ui.theme.GlassBorderTeal
import com.haseeb.emulator.ui.theme.MagiskEmerald
import com.haseeb.emulator.ui.theme.ObsidianBg
import com.haseeb.emulator.ui.theme.OnTealAccent
import com.haseeb.emulator.ui.theme.TealAccent
import com.haseeb.emulator.ui.theme.TealAccentBright
import com.haseeb.emulator.ui.theme.TextMuted
import com.haseeb.emulator.ui.theme.TextSecondary
import java.text.DateFormat
import java.util.Date

@Composable
fun HomeScreen(
    viewModel: HomeViewModel = viewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    val firmwarePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) {
            viewModel.importFirmwareFromUri(uri)
        }
    }

    val isBusy = uiState.installStage is RomInstallStage.Downloading ||
        uiState.installStage is RomInstallStage.VerifyingSha256 ||
        uiState.installStage is RomInstallStage.Extracting ||
        uiState.installStage is RomInstallStage.Importing

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.radialGradient(
                    colors = listOf(
                        TealAccent.copy(alpha = 0.14f),
                        Color(0xFF0A111D),
                        ObsidianBg,
                    ),
                    center = Offset(240f, 120f),
                    radius = 1200f,
                ),
            ),
    ) {
        Scaffold(
            containerColor = Color.Transparent,
            contentWindowInsets = WindowInsets.safeDrawing,
            floatingActionButton = {
                ExtendedFloatingActionButton(
                    onClick = {
                        firmwarePickerLauncher.launch(
                            arrayOf("application/zip", "application/octet-stream", "*/*"),
                        )
                    },
                    containerColor = TealAccent,
                    contentColor = OnTealAccent,
                    shape = RoundedCornerShape(18.dp),
                    modifier = Modifier.testTag("import_firmware_fab"),
                    icon = {
                        Icon(
                            imageVector = Icons.Rounded.Add,
                            contentDescription = stringResource(R.string.cd_import_firmware),
                        )
                    },
                    text = {
                        Text(
                            text = stringResource(R.string.fab_import_label),
                            style = MaterialTheme.typography.labelLarge,
                        )
                    },
                )
            },
        ) { innerPadding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding),
                contentAlignment = Alignment.TopCenter,
            ) {
                LazyColumn(
                    modifier = Modifier
                        .fillMaxSize()
                        .widthIn(max = 720.dp),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = 12.dp,
                        bottom = 96.dp,
                    ),
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    item {
                        BrandHeader()
                    }

                    item {
                        AnimatedVisibility(
                            visible = uiState.installStage !is RomInstallStage.Idle,
                            enter = fadeIn(),
                            exit = fadeOut(),
                        ) {
                            InstallProgressBanner(
                                stage = uiState.installStage,
                                onDismiss = viewModel::clearInstallStatus,
                            )
                        }
                    }

                    if (uiState.showFirstLaunchOffer) {
                        item {
                            FirstLaunchRomOfferCard(
                                repoSlug = uiState.settings.githubRepoSlug,
                                releaseTag = uiState.settings.romReleaseTag,
                                isBusy = isBusy,
                                onDownload = viewModel::installBundledGamingRom,
                                onDismiss = viewModel::dismissFirstLaunchOffer,
                            )
                        }
                    }

                    item {
                        InstallGamingRomHeroCard(
                            isBusy = isBusy,
                            onInstallClick = viewModel::installBundledGamingRom,
                        )
                    }

                    item {
                        SectionHeader(
                            title = stringResource(R.string.section_installed_systems),
                            trailingBadge = stringResource(
                                R.string.section_installed_count,
                                uiState.systems.size,
                            ),
                        )
                    }

                    if (uiState.systems.isEmpty()) {
                        item {
                            EmptySystemsCard()
                        }
                    } else {
                        items(
                            items = uiState.systems,
                            key = { it.id },
                        ) { system ->
                            VmSystemCard(
                                system = system,
                                onBootClick = { viewModel.inspectSystem(system) },
                                onGamingModeClick = { viewModel.inspectSystem(system) },
                            )
                        }
                    }
                }
            }
        }
    }

    uiState.gamingDialogSystem?.let { system ->
        SystemInspectDialog(
            system = system,
            onDismiss = { viewModel.inspectSystem(null) },
        )
    }
}

@Composable
private fun BrandHeader() {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween,
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(46.dp)
                    .clip(RoundedCornerShape(14.dp))
                    .background(TealAccent.copy(alpha = 0.14f)),
                contentAlignment = Alignment.Center,
            ) {
                Image(
                    painter = painterResource(id = R.drawable.ic_launcher_art),
                    contentDescription = stringResource(R.string.cd_brand_logo),
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            }
            Column {
                Text(
                    text = stringResource(R.string.app_name),
                    style = MaterialTheme.typography.headlineMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Text(
                    text = stringResource(R.string.brand_subtitle),
                    style = MaterialTheme.typography.labelSmall,
                    color = TealAccent,
                )
            }
        }

        StatusPill(
            label = "ARM64 · 16KB",
            tint = CyanHighlight,
            icon = Icons.Rounded.Memory,
        )
    }
}

@Composable
private fun FirstLaunchRomOfferCard(
    repoSlug: String,
    releaseTag: String,
    isBusy: Boolean,
    onDownload: () -> Unit,
    onDismiss: () -> Unit,
) {
    GlassCard(
        elevated = true,
        accentBorder = true,
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            StatusPill(
                label = stringResource(R.string.first_launch_badge),
                tint = TealAccent,
                icon = Icons.Rounded.Download,
            )
            IconButton(
                onClick = onDismiss,
                modifier = Modifier
                    .size(48.dp)
                    .testTag("dismiss_first_launch_button"),
            ) {
                Icon(
                    imageVector = Icons.Rounded.Close,
                    contentDescription = stringResource(R.string.action_skip_for_now),
                    tint = TextSecondary,
                )
            }
        }
        Spacer(modifier = Modifier.height(6.dp))
        Text(
            text = stringResource(R.string.first_launch_title),
            style = MaterialTheme.typography.titleLarge,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Spacer(modifier = Modifier.height(6.dp))
        Text(
            text = stringResource(R.string.first_launch_body),
            style = MaterialTheme.typography.bodyMedium,
            color = TextSecondary,
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = "github.com/$repoSlug · $releaseTag",
            style = MaterialTheme.typography.labelSmall,
            color = TealAccentBright,
        )
        Spacer(modifier = Modifier.height(14.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TealGlowButton(
                text = stringResource(R.string.action_download_verify),
                icon = Icons.Rounded.Download,
                enabled = !isBusy,
                onClick = onDownload,
                modifier = Modifier
                    .weight(1f)
                    .testTag("first_launch_download_button"),
            )
        }
    }
}

@Composable
private fun InstallGamingRomHeroCard(
    isBusy: Boolean,
    onInstallClick: () -> Unit,
) {
    val cardShape = RoundedCornerShape(24.dp)
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(cardShape)
            .testTag("install_gaming_rom_card"),
    ) {
        Image(
            painter = painterResource(id = R.drawable.img_rom_hero_banner),
            contentDescription = stringResource(R.string.cd_hero_banner),
            contentScale = ContentScale.Crop,
            modifier = Modifier
                .matchParentSize(),
        )
        Box(
            modifier = Modifier
                .matchParentSize()
                .background(
                    Brush.verticalGradient(
                        colors = listOf(
                            Color(0x9906090F),
                            Color(0xE8080D16),
                            Color(0xF506090F),
                        ),
                    ),
                ),
        )
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(20.dp),
        ) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                StatusPill(
                    label = stringResource(R.string.hero_card_badge),
                    tint = TealAccent,
                    icon = Icons.Rounded.SportsEsports,
                )
                StatusPill(
                    label = "SHA-256",
                    tint = MagiskEmerald,
                    icon = Icons.Rounded.Verified,
                )
            }
            Spacer(modifier = Modifier.height(14.dp))
            Text(
                text = stringResource(R.string.hero_card_title),
                style = MaterialTheme.typography.displaySmall,
                color = MaterialTheme.colorScheme.onBackground,
            )
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = stringResource(R.string.hero_card_subtitle),
                style = MaterialTheme.typography.bodyLarge,
                color = TextSecondary,
            )
            Spacer(modifier = Modifier.height(18.dp))
            TealGlowButton(
                text = stringResource(R.string.hero_card_cta),
                icon = Icons.Rounded.Download,
                enabled = !isBusy,
                onClick = onInstallClick,
                modifier = Modifier
                    .fillMaxWidth()
                    .testTag("hero_install_rom_button"),
            )
        }
    }
}

@Composable
private fun InstallProgressBanner(
    stage: RomInstallStage,
    onDismiss: () -> Unit,
) {
    GlassCard(
        elevated = true,
        accentBorder = stage !is RomInstallStage.Failed,
        modifier = Modifier
            .fillMaxWidth()
            .testTag("rom_install_progress_banner"),
    ) {
        when (stage) {
            is RomInstallStage.Downloading -> {
                Text(
                    text = stringResource(
                        R.string.status_downloading,
                        stage.progressPercent,
                        stage.downloadedLabel,
                    ),
                    style = MaterialTheme.typography.titleMedium,
                    color = TealAccentBright,
                )
                Spacer(modifier = Modifier.height(10.dp))
                LinearProgressIndicator(
                    progress = { stage.progressPercent / 100f },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(6.dp)
                        .clip(CircleShape),
                    color = TealAccent,
                    trackColor = TealAccent.copy(alpha = 0.18f),
                )
            }
            RomInstallStage.VerifyingSha256 -> {
                IndeterminateStageRow(stringResource(R.string.status_verifying))
            }
            RomInstallStage.Extracting -> {
                IndeterminateStageRow(stringResource(R.string.status_extracting))
            }
            RomInstallStage.Importing -> {
                IndeterminateStageRow(stringResource(R.string.status_importing))
            }
            is RomInstallStage.Completed -> {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        modifier = Modifier.weight(1f),
                    ) {
                        Icon(
                            imageVector = Icons.Rounded.CheckCircle,
                            contentDescription = null,
                            tint = MagiskEmerald,
                        )
                        Text(
                            text = "${stringResource(R.string.status_rom_ready)}: ${stage.systemName}",
                            style = MaterialTheme.typography.titleMedium,
                            color = MaterialTheme.colorScheme.onBackground,
                        )
                    }
                    IconButton(onClick = onDismiss, modifier = Modifier.size(48.dp)) {
                        Icon(
                            imageVector = Icons.Rounded.Close,
                            contentDescription = stringResource(R.string.action_skip_for_now),
                            tint = TextSecondary,
                        )
                    }
                }
            }
            is RomInstallStage.Failed -> {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp),
                        modifier = Modifier.weight(1f),
                    ) {
                        Icon(
                            imageVector = Icons.Rounded.ErrorOutline,
                            contentDescription = null,
                            tint = ErrorCoral,
                        )
                        Text(
                            text = stringResource(R.string.status_error_prefix, stage.reason),
                            style = MaterialTheme.typography.bodyMedium,
                            color = ErrorCoral,
                        )
                    }
                    IconButton(onClick = onDismiss, modifier = Modifier.size(48.dp)) {
                        Icon(
                            imageVector = Icons.Rounded.Close,
                            contentDescription = stringResource(R.string.action_skip_for_now),
                            tint = TextSecondary,
                        )
                    }
                }
            }
            RomInstallStage.Idle -> Unit
        }
    }
}

@Composable
private fun IndeterminateStageRow(label: String) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            style = MaterialTheme.typography.titleMedium,
            color = TealAccentBright,
        )
        Spacer(modifier = Modifier.height(10.dp))
        LinearProgressIndicator(
            modifier = Modifier
                .fillMaxWidth()
                .height(6.dp)
                .clip(CircleShape),
            color = TealAccent,
            trackColor = TealAccent.copy(alpha = 0.18f),
        )
    }
}

@Composable
private fun EmptySystemsCard() {
    GlassCard(
        modifier = Modifier
            .fillMaxWidth()
            .testTag("empty_systems_card"),
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(48.dp)
                    .clip(RoundedCornerShape(14.dp))
                    .background(TealAccent.copy(alpha = 0.12f)),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    imageVector = Icons.Rounded.SportsEsports,
                    contentDescription = null,
                    tint = TealAccent,
                )
            }
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.empty_systems_title),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = stringResource(R.string.empty_systems_body),
                    style = MaterialTheme.typography.bodyMedium,
                    color = TextSecondary,
                )
            }
        }
    }
}

@Composable
private fun VmSystemCard(
    system: VmSystem,
    onBootClick: () -> Unit,
    onGamingModeClick: () -> Unit,
) {
    val lastBootText = system.lastBootEpochMs?.let { epoch ->
        DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(epoch))
    } ?: stringResource(R.string.label_never_booted)

    GlassCard(
        elevated = true,
        modifier = Modifier
            .fillMaxWidth()
            .testTag("vm_system_card_${system.id}"),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.Top,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = system.name,
                    style = MaterialTheme.typography.titleLarge,
                    color = MaterialTheme.colorScheme.onBackground,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = "Android ${system.androidVersion} (API ${system.apiLevel}) · ${system.arch} · ${RomRepository.formatBytes(system.sizeBytes)}",
                    style = MaterialTheme.typography.labelMedium,
                    color = TextSecondary,
                )
            }
        }

        Spacer(modifier = Modifier.height(10.dp))
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (system.hasMagisk) {
                StatusPill(
                    label = stringResource(R.string.label_magisk_badge),
                    tint = MagiskEmerald,
                    icon = Icons.Rounded.Security,
                )
            }
            if (system.sha256Verified) {
                StatusPill(
                    label = stringResource(R.string.label_verified_badge),
                    tint = TealAccent,
                    icon = Icons.Rounded.Verified,
                )
            }
        }

        Spacer(modifier = Modifier.height(10.dp))
        Text(
            text = "${stringResource(R.string.label_last_boot)}: $lastBootText",
            style = MaterialTheme.typography.labelSmall,
            color = TextMuted,
        )

        Spacer(modifier = Modifier.height(14.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            TealGlowButton(
                text = stringResource(R.string.action_boot_vm),
                icon = Icons.Rounded.PlayArrow,
                onClick = onBootClick,
                modifier = Modifier
                    .weight(1f)
                    .testTag("boot_vm_button_${system.id}"),
            )
            OutlinedButton(
                onClick = onGamingModeClick,
                modifier = Modifier
                    .defaultMinSize(minHeight = 48.dp)
                    .testTag("gaming_mode_button_${system.id}"),
                shape = RoundedCornerShape(14.dp),
                border = BorderStroke(1.dp, GlassBorderTeal),
            ) {
                Icon(
                    imageVector = Icons.Rounded.Tune,
                    contentDescription = null,
                    tint = TealAccentBright,
                    modifier = Modifier.size(18.dp),
                )
                Spacer(modifier = Modifier.width(6.dp))
                Text(
                    text = stringResource(R.string.action_gaming_mode),
                    style = MaterialTheme.typography.labelLarge,
                    color = TealAccentBright,
                )
            }
        }
    }
}

@Composable
private fun SystemInspectDialog(
    system: VmSystem,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Color(0xFF0E1624),
        titleContentColor = MaterialTheme.colorScheme.onBackground,
        textContentColor = TextSecondary,
        title = {
            Text(
                text = system.name,
                style = MaterialTheme.typography.titleLarge,
            )
        },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    text = stringResource(R.string.milestone_notice_title),
                    style = MaterialTheme.typography.titleMedium,
                    color = TealAccent,
                )
                Text(
                    text = stringResource(R.string.milestone_notice_body),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    text = "Marker: ${system.romMarker.ifEmpty { "custom" }}\nsystem.img SHA-256: ${system.systemSha256.take(16)}…",
                    style = MaterialTheme.typography.labelSmall,
                    color = TealAccentBright,
                )
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) {
                Text("OK", color = TealAccent)
            }
        },
    )
}
