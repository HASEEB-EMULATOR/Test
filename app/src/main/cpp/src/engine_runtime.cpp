#include "haseeb/engine_runtime.hpp"

#include <android/log.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "HaseebEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace haseeb::engine {

EngineRuntime& EngineRuntime::Instance() {
    static EngineRuntime instance;
    return instance;
}

EngineRuntime::~EngineRuntime() {
    Stop();
}

void EngineRuntime::SetPath(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    work_dir_ = dir;
    AppendLog("INFO", "Engine root path configured: " + dir);
}

void EngineRuntime::SetNativeLibDir(const std::string& lib_dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    native_lib_dir_ = lib_dir;
    AppendLog("INFO", "Native library directory set: " + lib_dir);
}

void EngineRuntime::ConfigureGuestProps(const GuestConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (running_.load()) {
        ApplyGuestIdentityOverrides(config_);
    }
}

void EngineRuntime::ApplyGuestIdentityOverrides(const GuestConfig& cfg) {
    // Overrides are applied BEFORE guest reads anything from /dev/__properties__.
    prop_manager_.SetOverride("ro.product.model", cfg.model);
    prop_manager_.SetOverride("ro.product.brand", cfg.brand);
    prop_manager_.SetOverride("ro.product.manufacturer", cfg.manufacturer);
    prop_manager_.SetOverride("ro.product.device", cfg.device);
    prop_manager_.SetOverride("ro.product.name", cfg.name);
    prop_manager_.SetOverride("ro.product.marketname", cfg.marketname);
    prop_manager_.SetOverride("ro.build.fingerprint", cfg.fingerprint);
    prop_manager_.SetOverride("ro.build.product", cfg.device);
    prop_manager_.SetOverride("ro.build.device", cfg.device);
    prop_manager_.SetOverride("ro.build.tags", "release-keys");
    prop_manager_.SetOverride("ro.build.version.sdk", std::to_string(cfg.api_level));
    prop_manager_.SetOverride("ro.sf.lcd_density", std::to_string(cfg.dpi));
    prop_manager_.SetOverride("ro.haseeb.engine", "native-v1");
    prop_manager_.SetOverride("ro.haseeb.ram_mb", std::to_string(cfg.ram_mb));
    prop_manager_.SetOverride("ro.haseeb.display.width", std::to_string(cfg.width));
    prop_manager_.SetOverride("ro.haseeb.display.height", std::to_string(cfg.height));
    prop_manager_.SetOverride("qemu.hw.mainkeys", cfg.nav_bar_off ? "1" : "0");
    prop_manager_.SetOverride("persist.haseeb.magisk.su", cfg.magisk_prompt_enabled ? "1" : "0");
    prop_manager_.SetOverride("persist.haseeb.target_fps", std::to_string(cfg.target_fps));
}

void EngineRuntime::InitializeGuestLoaderLocked(int api) {
    system_ext4_.Close();
    vendor_ext4_.Close();
    probed_guest_exe_.reset();
    probed_guest_linker_.reset();
    guest_auxv_.clear();

    vfs_resolver_.SetHostFallbackRoot(work_dir_);

    // Locate and open raw ext4 partition images mount-free
    const std::vector<std::string> sys_candidates = {
        work_dir_ + "/system.img",
        work_dir_ + "/images/system.img",
    };
    bool has_sys_prefix = false;
    for (const auto& path : sys_candidates) {
        if (::access(path.c_str(), R_OK) == 0) {
            if (system_ext4_.Open(path)) {
                has_sys_prefix = system_ext4_.Exists("/system/build.prop");
                vfs_resolver_.AttachSystemExt4(&system_ext4_, has_sys_prefix);
                AppendLog("INFO", "M2 Ext4Reader opened system.img (blk=" +
                          std::to_string(system_ext4_.BlockSize()) + ", sar=" +
                          (has_sys_prefix ? "true" : "false") + ")");
            } else {
                AppendLog("WARN", "M2 Ext4Reader failed on " + path + ": " + system_ext4_.LastError());
            }
            break;
        }
    }

    const std::vector<std::string> ven_candidates = {
        work_dir_ + "/vendor.img",
        work_dir_ + "/images/vendor.img",
    };
    for (const auto& path : ven_candidates) {
        if (::access(path.c_str(), R_OK) == 0) {
            if (vendor_ext4_.Open(path)) {
                vfs_resolver_.AttachVendorExt4(&vendor_ext4_);
                AppendLog("INFO", "M2 Ext4Reader opened vendor.img (blk=" +
                          std::to_string(vendor_ext4_.BlockSize()) + ")");
            } else {
                AppendLog("WARN", "M2 Ext4Reader failed on " + path + ": " + vendor_ext4_.LastError());
            }
            break;
        }
    }

    // Load build.prop mount-free from ext4 first, then host-extracted fallback
    std::vector<uint8_t> prop_bytes;
    if (vfs_resolver_.ReadGuestFile("/system/build.prop", prop_bytes)) {
        std::string txt(reinterpret_cast<const char*>(prop_bytes.data()), prop_bytes.size());
        prop_manager_.LoadPropText(txt);
        AppendLog("INFO", "M2 loaded /system/build.prop mount-free (" +
                  std::to_string(prop_bytes.size()) + " bytes)");
    } else {
        prop_manager_.LoadPropFile(work_dir_ + "/system/build.prop");
    }

    prop_bytes.clear();
    if (vfs_resolver_.ReadGuestFile("/vendor/build.prop", prop_bytes)) {
        std::string txt(reinterpret_cast<const char*>(prop_bytes.data()), prop_bytes.size());
        prop_manager_.LoadPropText(txt);
        AppendLog("INFO", "M2 loaded /vendor/build.prop mount-free (" +
                  std::to_string(prop_bytes.size()) + " bytes)");
    } else {
        prop_manager_.LoadPropFile(work_dir_ + "/vendor/build.prop");
    }

    std::vector<uint8_t> rom_marker;
    if (vfs_resolver_.ReadGuestFile("/system/etc/haseeb_rom", rom_marker)) {
        std::string marker_txt(reinterpret_cast<const char*>(rom_marker.data()), rom_marker.size());
        AppendLog("INFO", "M2 detected ROM marker: " + marker_txt);
    }

    const std::string linker_path = vfs_resolver_.ResolveLinker64Path(api);
    const auto linker_env = vfs_resolver_.BuildLinker64Env(api, work_dir_);
    AppendLog("INFO", "M2 linker64 resolved: " + linker_path + " (page=" +
              std::to_string(elf_loader_.EffectivePageSize()) + "B, env_vars=" +
              std::to_string(linker_env.size()) + ")");

    // Probe /system/bin/sh or /system/bin/init if present in the guest rootfs/ext4
    const std::vector<std::string> probe_bins = {"/system/bin/sh", "/system/bin/init"};
    for (const auto& bin : probe_bins) {
        if (!vfs_resolver_.Exists(bin)) continue;
        std::string load_err;
        if (elf_loader_.LoadWithInterpreter(
                bin, vfs_resolver_, api, probed_guest_exe_, probed_guest_linker_, load_err)) {
            uint8_t dummy_random[16]{};
            guest_auxv_ = loader::GuestAuxvBuilder::BuildAuxv(
                *probed_guest_exe_,
                probed_guest_linker_.get(),
                reinterpret_cast<uintptr_t>(bin.c_str()),
                reinterpret_cast<uintptr_t>(dummy_random),
                elf_loader_.EffectivePageSize());
            AppendLog("INFO", "M2 ELF64 loaded " + bin + " (bias=0x" +
                      std::to_string(probed_guest_exe_->load_bias) + ", relr=" +
                      std::to_string(probed_guest_exe_->relr_applied_count) + ", rela=" +
                      std::to_string(probed_guest_exe_->rela_applied_count) +
                      ", auxv_entries=" + std::to_string(guest_auxv_.size() / 2) + ")");
        } else {
            AppendLog("WARN", "M2 ELF64 probe failed for " + bin + ": " + load_err);
        }
        break;
    }
}

bool EngineRuntime::Start(int api, int width, int height, int dpi, int ram_mb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
        AppendLog("WARN", "Start ignored: engine already running");
        return true;
    }
    if (work_dir_.empty()) {
        AppendLog("ERROR", "Cannot start engine: setPath(dir) was not called");
        return false;
    }

    config_.api_level = api;
    config_.width = width;
    config_.height = height;
    config_.dpi = dpi;
    config_.ram_mb = ram_mb;

    prop_manager_.ClearOverrides();
    ApplyGuestIdentityOverrides(config_);

    if (!prop_manager_.Initialize(work_dir_, work_dir_)) {
        AppendLog("ERROR", "Failed to initialize M1 /dev/__properties__ area in " + work_dir_);
        return false;
    }

    // M2: Open ext4 partitions mount-free, load build.prop, resolve linker64 & ELF64 probe
    InitializeGuestLoaderLocked(api);

    ::mkdir((work_dir_ + "/dev").c_str(), 0755);
    ::mkdir((work_dir_ + "/dev/socket").c_str(), 0755);

    prop_service_ = std::make_unique<prop::PropertyService>(prop_manager_);
    const std::string sock_path = work_dir_ + "/dev/socket/property_service";
    if (!prop_service_->Start(sock_path)) {
        AppendLog("WARN", "PropertyService socket failed to bind at " + sock_path);
    } else {
        AppendLog("INFO", "M1 PropertyService listening on " + sock_path);
    }

    running_.store(true);
    measured_fps_.store(0.0f);

    AppendLog("INFO", "M1 Property Area + M2 Guest Loader active (16 KiB aligned, overrides locked)");
    AppendLog("INFO", "Guest target: API " + std::to_string(api) + " (" +
              std::to_string(width) + "x" + std::to_string(height) + " @" +
              std::to_string(dpi) + "dpi, " + std::to_string(ram_mb) + " MB)");

    PresentPlaceholderFrame();
    return true;
}

void EngineRuntime::AttachSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    window_ = window;
    if (window_ != nullptr) {
        ANativeWindow_setBuffersGeometry(window_, config_.width, config_.height, WINDOW_FORMAT_RGBA_8888);
        AppendLog("INFO", "Attached ANativeWindow (" + std::to_string(config_.width) + "x" +
                  std::to_string(config_.height) + ")");
        if (running_.load()) {
            PresentPlaceholderFrame();
        }
    } else {
        AppendLog("INFO", "Detached ANativeWindow");
    }
}

void EngineRuntime::PresentPlaceholderFrame() {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_ == nullptr) return;
    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(window_, &buf, nullptr) == 0) {
        auto* pixels = static_cast<uint32_t*>(buf.bits);
        // Deep obsidian #06090F RGBA8888 little-endian (0xFF0F0906) with subtle #4FE6BB top bar
        for (int y = 0; y < buf.height; ++y) {
            uint32_t color = (y < 4) ? 0xFFBBE64Fu : 0xFF0F0906u;
            uint32_t* row = pixels + static_cast<size_t>(y) * buf.stride;
            for (int x = 0; x < buf.width; ++x) {
                row[x] = color;
            }
        }
        ANativeWindow_unlockAndPost(window_);
    }
}

void EngineRuntime::SendTouch(int action, int x, int y, int pointer_id) {
    if (!running_.load()) return;
    AppendLog("INPUT", "Touch(action=" + std::to_string(action) + ", x=" + std::to_string(x) +
              ", y=" + std::to_string(y) + ", id=" + std::to_string(pointer_id) + ") queued (M6 pending)");
}

void EngineRuntime::SendKey(int code, bool down) {
    if (!running_.load()) return;
    AppendLog("INPUT", "Key(code=" + std::to_string(code) + ", down=" + (down ? "1" : "0") +
              ") queued (M6 pending)");
}

void EngineRuntime::SendText(const std::string& text) {
    if (!running_.load()) return;
    AppendLog("INPUT", "Text(len=" + std::to_string(text.size()) + ") queued (M6 pending)");
}

std::string EngineRuntime::ReadLog(int max_bytes) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (max_bytes <= 0) return "";
    std::string combined;
    for (const auto& line : log_lines_) {
        combined += line;
        combined.push_back('\n');
    }
    if (combined.size() > static_cast<size_t>(max_bytes)) {
        return combined.substr(combined.size() - static_cast<size_t>(max_bytes));
    }
    return combined;
}

void EngineRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false)) {
        return;
    }
    if (prop_service_) {
        prop_service_->Stop();
        prop_service_.reset();
    }
    guest_auxv_.clear();
    probed_guest_exe_.reset();
    probed_guest_linker_.reset();
    vfs_resolver_.AttachSystemExt4(nullptr, false);
    vfs_resolver_.AttachVendorExt4(nullptr);
    system_ext4_.Close();
    vendor_ext4_.Close();
    {
        std::lock_guard<std::mutex> wlock(window_mutex_);
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }
    measured_fps_.store(0.0f);
    AppendLog("INFO", "Engine stopped cleanly");
}

void EngineRuntime::AppendLog(const std::string& level, const std::string& msg) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    std::ostringstream oss;
    oss << "[" << now_ms << "][" << level << "] " << msg;
    const std::string line = oss.str();
    LOGI("%s", line.c_str());

    std::lock_guard<std::mutex> lock(log_mutex_);
    log_lines_.push_back(line);
    log_bytes_ += line.size() + 1;
    while (log_bytes_ > kMaxLogBufferBytes && !log_lines_.empty()) {
        log_bytes_ -= log_lines_.front().size() + 1;
        log_lines_.pop_front();
    }
}

} // namespace haseeb::engine
