#include "haseeb/engine_runtime.hpp"

#include <android/log.h>
#include <chrono>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "HaseebVM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace haseeb::engine {

EngineRuntime& EngineRuntime::Instance() {
    static EngineRuntime instance;
    return instance;
}

EngineRuntime::EngineRuntime()
    : machine_([this](const std::string& line) {
          AppendLog("UART", line);
      }) {}

EngineRuntime::~EngineRuntime() {
    Stop();
}

vm::VmError EngineRuntime::SetPath(const std::string& dir) {
    if (dir.empty()) return vm::VmError::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex_);
    work_dir_ = dir;
    AppendLog("INFO", "Engine root path configured: " + dir);
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::SetNativeLibDir(const std::string& lib_dir) {
    if (lib_dir.empty()) return vm::VmError::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex_);
    native_lib_dir_ = lib_dir;
    AppendLog("INFO", "Native library directory set: " + lib_dir);
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::LoadBootImage(const std::string& path) {
    if (path.empty()) return vm::VmError::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex_);
    vm::ParsedBootImage parsed;
    std::string err;
    vm::VmError res = vm::BootImageParser::ParseBootFile(path, parsed, err);
    if (res != vm::VmError::Ok) {
        AppendLog("ERROR", "loadBootImage(" + path + ") failed: " + err);
        return res;
    }

    boot_image_ = std::move(parsed);
    boot_image_path_ = path;
    has_boot_image_ = true;

    std::ostringstream oss;
    oss << "Loaded boot.img v" << boot_image_.header_version
        << " (kernel=" << boot_image_.kernel.size()
        << "B, arm64_magic=" << (boot_image_.kernel_info.valid_arm64_magic ? "yes" : "compressed/no")
        << ", ramdisk=" << boot_image_.ramdisk.size()
        << "B, magisk=" << (boot_image_.magisk_detected ? "detected" : "not-detected") << ")";
    AppendLog("INFO", oss.str());
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::LoadSystemImage(const std::string& path) {
    if (path.empty()) return vm::VmError::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex_);
    if (::access(path.c_str(), R_OK) != 0) {
        AppendLog("ERROR", "loadSystemImage(" + path + ") failed: file unreadable");
        return vm::VmError::FileNotFound;
    }

    system_ext4_.Close();
    if (!system_ext4_.Open(path)) {
        AppendLog("ERROR", "loadSystemImage invalid raw ext4: " + system_ext4_.LastError());
        return vm::VmError::InvalidSystemImage;
    }

    system_image_path_ = path;
    has_system_image_ = true;

    std::string marker;
    const bool has_marker = system_ext4_.ReadFileString("/system/etc/haseeb_rom", marker) ||
                            system_ext4_.ReadFileString("/etc/haseeb_rom", marker);
    AppendLog("INFO", "Loaded system.img (" + std::to_string(system_ext4_.ImageSizeBytes() / (1024 * 1024)) +
              " MiB, blk=" + std::to_string(system_ext4_.BlockSize()) +
              (has_marker ? ", marker=" + marker : "") + ")");
    return vm::VmError::Ok;
}

void EngineRuntime::ConfigureGuestProps(const GuestConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    target_fps_.store(config.target_fps);
}

vm::VmError EngineRuntime::Start(int api, int width, int height, int dpi, int ram_mb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
        AppendLog("WARN", "Start ignored: VM already running");
        return vm::VmError::AlreadyRunning;
    }
    if (work_dir_.empty()) {
        AppendLog("ERROR", "Cannot start VM: setPath(dir) was not called");
        return vm::VmError::PathNotSet;
    }
    if (width <= 0 || height <= 0 || dpi <= 0 || ram_mb < 64 || ram_mb > 4096) {
        AppendLog("ERROR", "Invalid VM geometry or RAM argument");
        return vm::VmError::InvalidArgument;
    }

    config_.api_level = api;
    config_.width = width;
    config_.height = height;
    config_.dpi = dpi;
    config_.ram_mb = ram_mb;

    vm::MachineBootConfig mcfg;
    mcfg.ram_mb = static_cast<uint32_t>(ram_mb);
    mcfg.vcpu_count = static_cast<uint32_t>(config_.vcpu_count);
    mcfg.api_level = static_cast<uint32_t>(api);
    mcfg.width = static_cast<uint32_t>(width);
    mcfg.height = static_cast<uint32_t>(height);
    mcfg.dpi = static_cast<uint32_t>(dpi);
    mcfg.extra_cmdline = "androidboot.selinux=permissive haseeb.api=" + std::to_string(api);

    std::string err;
    const vm::ParsedBootImage* boot_ptr = has_boot_image_ ? &boot_image_ : nullptr;
    vm::VmError stage_err = machine_.InitializeAndStage(mcfg, boot_ptr, err);
    if (stage_err != vm::VmError::Ok) {
        AppendLog("ERROR", "VM staging failed: " + err);
        return stage_err;
    }

    running_.store(true);
    measured_fps_.store(0.0f);

    AppendLog("INFO", "M1 Full-System VM staged (RAM=" + std::to_string(ram_mb) +
              " MiB via memfd, FDT @0x" + std::to_string(machine_.DtbGuestAddr()) + ")");

    vm::VmError boot_res = machine_.RunBootStubUntilHalt();
    if (boot_res != vm::VmError::Ok) {
        AppendLog("ERROR", "M1 bootloader stub trap: " + std::string(vm::VmErrorToString(boot_res)));
        machine_.Shutdown();
        running_.store(false);
        return boot_res;
    }

    PresentBootFrame();
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::AttachSurface(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_ != nullptr) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }
    window_ = window;
    if (window_ != nullptr) {
        int32_t rc = ANativeWindow_setBuffersGeometry(
            window_, config_.width, config_.height, WINDOW_FORMAT_RGBA_8888);
        if (rc != 0) {
            AppendLog("ERROR", "ANativeWindow_setBuffersGeometry failed rc=" + std::to_string(rc));
            return vm::VmError::SurfaceAttachFailed;
        }
        AppendLog("INFO", "Attached ANativeWindow (" + std::to_string(config_.width) + "x" +
                  std::to_string(config_.height) + ")");
        if (running_.load()) {
            PresentBootFrame();
        }
    } else {
        AppendLog("INFO", "Detached ANativeWindow");
    }
    return vm::VmError::Ok;
}

void EngineRuntime::PresentBootFrame() {
    std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_ == nullptr) return;
    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(window_, &buf, nullptr) == 0) {
        auto* pixels = static_cast<uint32_t*>(buf.bits);
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

vm::VmError EngineRuntime::SendTouch(int action, int x, int y, int pointer_id) {
    if (!running_.load()) return vm::VmError::NotRunning;
    AppendLog("INPUT", "Touch(action=" + std::to_string(action) + ", x=" + std::to_string(x) +
              ", y=" + std::to_string(y) + ", id=" + std::to_string(pointer_id) + ") queued (M6 virtio-input)");
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::SendKey(int code, bool down) {
    if (!running_.load()) return vm::VmError::NotRunning;
    AppendLog("INPUT", "Key(code=" + std::to_string(code) + ", down=" + (down ? "1" : "0") +
              ") queued (M6 virtio-input)");
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::SendText(const std::string& text) {
    if (!running_.load()) return vm::VmError::NotRunning;
    machine_.Uart().PushRxString(text);
    AppendLog("INPUT", "Text(len=" + std::to_string(text.size()) + ") pushed to PL011 RX");
    return vm::VmError::Ok;
}

vm::VmError EngineRuntime::SetTargetFps(int fps) {
    if (fps < 1 || fps > 240) return vm::VmError::InvalidArgument;
    target_fps_.store(fps);
    config_.target_fps = fps;
    AppendLog("INFO", "Target display FPS set to " + std::to_string(fps) + " Hz");
    return vm::VmError::Ok;
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

vm::VmError EngineRuntime::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false)) {
        return vm::VmError::NotRunning;
    }
    machine_.Shutdown();
    system_ext4_.Close();
    {
        std::lock_guard<std::mutex> wlock(window_mutex_);
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }
    measured_fps_.store(0.0f);
    AppendLog("INFO", "M1 VM stopped cleanly");
    return vm::VmError::Ok;
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
