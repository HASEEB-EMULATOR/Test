#pragma once

#include "haseeb/boot_image.hpp"
#include "haseeb/cpio_reader.hpp"
#include "haseeb/cpu_core.hpp"
#include "haseeb/ext4_reader.hpp"
#include "haseeb/guest_ram.hpp"
#include "haseeb/pl011_uart.hpp"
#include "haseeb/psci.hpp"
#include "haseeb/vm_types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace haseeb::vm {

struct Elf64LoadResult {
    bool ok = false;
    uint64_t entry_pc = 0;
    uint64_t phdr_va = 0;
    uint16_t phent_size = 0;
    uint16_t phnum = 0;
    uint64_t brk_start = 0;
    uint64_t stack_top = 0;
    uint64_t initial_sp = 0;
    std::string error;
};

// Loads static or PIE ELF64 AArch64 binaries (init, magiskinit, sh, etc.) into
// Stage-1 4 KiB page tables in GuestRam and sets up argc/argv/envp/auxv stack.
class Elf64GuestLoader {
public:
    static bool IsValidAArch64Elf(const std::vector<uint8_t>& elf_bytes);

    static Elf64LoadResult LoadAndMap(GuestRam& ram,
                                      uint64_t ttbr0_pa,
                                      uint64_t& phys_alloc_cursor,
                                      uint64_t phys_alloc_limit,
                                      const std::vector<uint8_t>& elf_bytes,
                                      const std::vector<std::string>& argv,
                                      const std::vector<std::string>& envp);

    // Maps one 4 KiB VA page -> PA page into a 3-level (T0SZ=25, 4K granule) TTBR0 table
    static bool MapPage4K(GuestRam& ram,
                          uint64_t ttbr0_pa,
                          uint64_t& phys_alloc_cursor,
                          uint64_t phys_alloc_limit,
                          uint64_t va,
                          uint64_t pa,
                          bool el0_access,
                          bool writable,
                          bool executable);
};

// Minimal EL1 supervisor + Linux AArch64 SVC ABI runtime for booting CPIO initramfs
// (/magiskinit, /init) and ext4 system binaries (/system/bin/init, /system/bin/sh) in EL0.
class LinuxGuestSupervisor {
public:
    struct OpenFile {
        std::string path;
        std::vector<uint8_t> data;
        size_t offset = 0;
        bool is_dir = false;
        std::vector<std::string> dir_children;
    };

    LinuxGuestSupervisor(GuestRam& ram,
                         Pl011Uart& uart,
                         PsciController& psci,
                         const loader::Ext4Reader* ext4 = nullptr);

    void SetInitramfs(std::vector<CpioEntry> entries);
    void SetExt4Reader(const loader::Ext4Reader* ext4) { ext4_ = ext4; }
    void SetGuestIdentity(const std::string& cmdline, bool magisk_enabled);

    // Resolves /magiskinit -> /init -> /system/bin/init -> /system/bin/sh, maps EL1 vectors +
    // identity RAM + EL0 ELF64 image, and configures CPU to ERET/enter EL0 at target entry.
    VmError PrepareBoot(AArch64CpuCore& cpu,
                        uint64_t ram_bytes,
                        uint64_t kernel_reserved_end,
                        const std::vector<std::string>& argv,
                        std::string& out_selected_binary,
                        std::string& out_error,
                        uint64_t arena_max_limit = 0);

    HypercallResult HandleHypercall(AArch64CpuCore& cpu,
                                    CpuState& state,
                                    uint16_t imm16,
                                    bool is_smc);

    int ExitStatus() const { return exit_status_; }
    bool Exited() const { return exited_; }
    const std::string& ActiveBinary() const { return active_binary_; }

private:
    static constexpr uint16_t kSyscallHypercallImm = 0x4C58u; // "LX"

    bool MapFreshAddressSpace(AArch64CpuCore& cpu,
                              const std::vector<uint8_t>& elf_bytes,
                              const std::vector<std::string>& argv,
                              std::string& out_error);
    bool ResolveVfsFile(const std::string& path,
                        std::vector<uint8_t>& out_data,
                        bool& out_is_dir,
                        std::vector<std::string>& out_children) const;
    bool ReadUserString(AArch64CpuCore& cpu, uint64_t uva, std::string& out_str) const;
    bool ReadUserBytes(AArch64CpuCore& cpu, uint64_t uva, void* dst, size_t len) const;
    bool WriteUserBytes(AArch64CpuCore& cpu, uint64_t uva, const void* src, size_t len) const;
    int64_t DispatchSyscall(AArch64CpuCore& cpu, CpuState& st, bool& out_halt, bool& out_retry);

    GuestRam& ram_;
    Pl011Uart& uart_;
    PsciController& psci_;
    const loader::Ext4Reader* ext4_ = nullptr;

    std::vector<CpioEntry> initramfs_;
    std::string cmdline_;
    bool magisk_enabled_ = false;
    std::string active_binary_;

    uint64_t ram_bytes_ = 0;
    uint64_t arena_base_ = 0;
    uint64_t arena_limit_ = 0;
    uint64_t phys_cursor_ = 0;
    uint64_t ttbr0_pa_ = 0;
    uint64_t current_brk_ = 0;
    uint64_t mmap_cursor_ = 0x0000001000000000ULL;

    int next_fd_ = 3;
    std::unordered_map<int, OpenFile> open_fds_;
    bool exited_ = false;
    int exit_status_ = 0;
};

} // namespace haseeb::vm
