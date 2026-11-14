#pragma once

#include "haseeb/elf64_loader.hpp"
#include "haseeb/ext4_reader.hpp"
#include "haseeb/prop_area.hpp"
#include "haseeb/property_service.hpp"

#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace haseeb::engine {

struct GuestConfig {
    int api_level = 29;
    int width = 720;
    int height = 1560;
    int dpi = 320;
    int ram_mb = 2048;

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

    void SetPath(const std::string& dir);
    void SetNativeLibDir(const std::string& lib_dir);
    void ConfigureGuestProps(const GuestConfig& config);

    bool Start(int api, int width, int height, int dpi, int ram_mb);
    void AttachSurface(ANativeWindow* window);
    void SendTouch(int action, int x, int y, int pointer_id);
    void SendKey(int code, bool down);
    void SendText(const std::string& text);
    std::string ReadLog(int max_bytes);
    void Stop();

    bool IsRunning() const { return running_.load(); }
    float GetGuestFps() const { return measured_fps_.load(); }

    const prop::PropertyAreaManager& PropertyManager() const { return prop_manager_; }
    const loader::GuestVfsResolver& VfsResolver() const { return vfs_resolver_; }

private:
    EngineRuntime() = default;
    ~EngineRuntime();

    void AppendLog(const std::string& level, const std::string& msg);
    void ApplyGuestIdentityOverrides(const GuestConfig& cfg);
    void InitializeGuestLoaderLocked(int api);
    void PresentPlaceholderFrame();

    mutable std::mutex mutex_;
    std::mutex log_mutex_;
    std::mutex window_mutex_;

    std::string work_dir_;
    std::string native_lib_dir_;
    GuestConfig config_;

    prop::PropertyAreaManager prop_manager_;
    std::unique_ptr<prop::PropertyService> prop_service_;

    loader::Ext4Reader system_ext4_;
    loader::Ext4Reader vendor_ext4_;
    loader::GuestVfsResolver vfs_resolver_;
    loader::Elf64Loader elf_loader_;
    std::unique_ptr<loader::LoadedElfImage> probed_guest_exe_;
    std::unique_ptr<loader::LoadedElfImage> probed_guest_linker_;
    std::vector<uint64_t> guest_auxv_;

    ANativeWindow* window_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<float> measured_fps_{0.0f};

    std::deque<std::string> log_lines_;
    size_t log_bytes_ = 0;
    static constexpr size_t kMaxLogBufferBytes = 512u * 1024u;
};

} // namespace haseeb::engine
