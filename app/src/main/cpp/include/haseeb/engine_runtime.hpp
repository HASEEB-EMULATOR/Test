#pragma once

#include "haseeb/boot_image.hpp"
#include "haseeb/ext4_reader.hpp"
#include "haseeb/machine.hpp"
#include "haseeb/vm_types.hpp"

#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace haseeb::engine {

struct GuestConfig {
    int api_level = 29;
    int width = 720;
    int height = 1560;
    int dpi = 320;
    int ram_mb = 2048;
    int vcpu_count = 4;

    std::string model = "SM-S938B";
    std::string brand = "samsung";
    std::string manufacturer = "samsung";
    std::string device = "pa3q";
    std::string name = "pa3qxxx";
    std::string marketname = "Galaxy S25 Ultra";
    std::string fingerprint = "samsung/pa3qxxx/pa3q:10/QP1A.190711.020/S938BXXU1:user/release-keys";
    bool nav_bar_off = true;
    bool magisk_prompt_enabled = true;
    int target_fps = 60;
};

class EngineRuntime {
public:
    static EngineRuntime& Instance();

    vm::VmError SetPath(const std::string& dir);
    vm::VmError SetNativeLibDir(const std::string& lib_dir);
    vm::VmError LoadBootImage(const std::string& path);
    vm::VmError LoadSystemImage(const std::string& path);
    void ConfigureGuestProps(const GuestConfig& config);

    vm::VmError Start(int api, int width, int height, int dpi, int ram_mb);
    vm::VmError AttachSurface(ANativeWindow* window);
    vm::VmError SendTouch(int action, int x, int y, int pointer_id);
    vm::VmError SendKey(int code, bool down);
    vm::VmError SendText(const std::string& text);
    vm::VmError SetTargetFps(int fps);
    std::string ReadLog(int max_bytes);
    float GetGuestFps() const { return measured_fps_.load(); }
    bool IsRunning() const { return running_.load(); }
    vm::VmError Stop();

    const vm::VirtualMachineSkeleton& Machine() const { return machine_; }
    const vm::ParsedBootImage& BootInfo() const { return boot_image_; }

private:
    EngineRuntime();
    ~EngineRuntime();

    void AppendLog(const std::string& level, const std::string& msg);
    void PresentBootFrame();
    void RunGuestLoop();

    mutable std::mutex mutex_;
    std::mutex log_mutex_;
    std::mutex window_mutex_;

    std::string work_dir_;
    std::string native_lib_dir_;
    std::string boot_image_path_;
    std::string system_image_path_;
    bool has_boot_image_ = false;
    bool has_system_image_ = false;

    GuestConfig config_;
    vm::ParsedBootImage boot_image_;
    loader::Ext4Reader system_ext4_;
    vm::VirtualMachineSkeleton machine_;

    ANativeWindow* window_ = nullptr;
    std::thread guest_thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> target_fps_{60};
    std::atomic<float> measured_fps_{0.0f};

    std::deque<std::string> log_lines_;
    size_t log_bytes_ = 0;
    static constexpr size_t kMaxLogBufferBytes = 512u * 1024u;
};

} // namespace haseeb::engine
