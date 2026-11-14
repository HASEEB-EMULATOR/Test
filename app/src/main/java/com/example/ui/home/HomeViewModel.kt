package com.example.ui.home

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.data.RomRepository
import com.example.model.HomeUiState
import com.example.model.RomInstallStage
import com.example.model.VmSystem
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class HomeViewModel(application: Application) : AndroidViewModel(application) {
    private val repository = RomRepository(application.applicationContext)

    private val _uiState = MutableStateFlow(HomeUiState())
    val uiState: StateFlow<HomeUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            refreshSystems()
        }
        viewModelScope.launch {
            combine(
                repository.firstLaunchOfferDismissed,
                repository.githubRepoSlug,
                repository.romReleaseTag,
            ) { dismissed, repo, tag ->
                Triple(dismissed, repo, tag)
            }.collect { (dismissed, repo, tag) ->
                _uiState.update { state ->
                    state.copy(
                        showFirstLaunchOffer = !dismissed && state.systems.isEmpty(),
                        settings = state.settings.copy(
                            githubRepoSlug = repo,
                            romReleaseTag = tag,
                        ),
                    )
                }
            }
        }
    }

    fun refreshSystems() {
        viewModelScope.launch {
            val loaded = repository.loadInstalledSystems()
            _uiState.update { state ->
                state.copy(
                    systems = loaded,
                    showFirstLaunchOffer = state.showFirstLaunchOffer && loaded.isEmpty(),
                )
            }
        }
    }

    fun dismissFirstLaunchOffer() {
        viewModelScope.launch {
            repository.setFirstLaunchOfferDismissed(true)
            _uiState.update { it.copy(showFirstLaunchOffer = false) }
        }
    }

    fun installBundledGamingRom() {
        if (_uiState.value.installStage is RomInstallStage.Downloading ||
            _uiState.value.installStage is RomInstallStage.VerifyingSha256 ||
            _uiState.value.installStage is RomInstallStage.Extracting
        ) {
            return
        }

        viewModelScope.launch {
            val result = repository.downloadVerifyAndImportBundledRom { stage ->
                _uiState.update { it.copy(installStage = stage) }
            }
            result.fold(
                onSuccess = { vm ->
                    val updatedSystems = repository.loadInstalledSystems()
                    _uiState.update {
                        it.copy(
                            systems = updatedSystems,
                            showFirstLaunchOffer = false,
                            installStage = RomInstallStage.Completed(vm.name),
                        )
                    }
                },
                onFailure = { error ->
                    _uiState.update {
                        it.copy(
                            installStage = RomInstallStage.Failed(
                                error.message ?: "Unknown network or checksum failure",
                            ),
                        )
                    }
                },
            )
        }
    }

    fun importFirmwareFromUri(uri: Uri) {
        viewModelScope.launch {
            val result = repository.importFirmwareUri(uri) { stage ->
                _uiState.update { it.copy(installStage = stage) }
            }
            result.fold(
                onSuccess = { vm ->
                    val updatedSystems = repository.loadInstalledSystems()
                    _uiState.update {
                        it.copy(
                            systems = updatedSystems,
                            showFirstLaunchOffer = false,
                            installStage = RomInstallStage.Completed(vm.name),
                        )
                    }
                },
                onFailure = { error ->
                    _uiState.update {
                        it.copy(
                            installStage = RomInstallStage.Failed(
                                error.message ?: "Invalid firmware archive",
                            ),
                        )
                    }
                },
            )
        }
    }

    fun clearInstallStatus() {
        _uiState.update { it.copy(installStage = RomInstallStage.Idle) }
    }

    fun inspectSystem(system: VmSystem?) {
        _uiState.update { it.copy(gamingDialogSystem = system) }
    }
}
