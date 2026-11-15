#include "haseeb/kernel_boot.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace haseeb::vm {

namespace {

inline constexpr uint16_t kElfEtExec = 2u;
inline constexpr uint16_t kElfEtDyn  = 3u;
inline constexpr uint16_t kElfEmAarch64 = 183u;
inline constexpr uint32_t kPtLoad = 1u;
inline constexpr uint32_t kPfX = 1u;
inline constexpr uint32_t kPfW = 2u;

#pragma pack(push, 1)
struct Elf64Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};
#pragma pack(pop)

uint64_t AlignDown4K(uint64_t v) { return v & ~0xFFFULL; }
uint64_t AlignUp4K(uint64_t v) { return (v + 0xFFFULL) & ~0xFFFULL; }

bool AllocZeroPage(GuestRam& ram, uint64_t& cursor, uint64_t limit, uint64_t& out_pa) {
    cursor = AlignUp4K(cursor);
    if (cursor + 4096ULL > limit) return false;
    uint8_t zeros[4096]{};
    if (!ram.WriteBytes(cursor, zeros, sizeof(zeros))) return false;
    out_pa = cursor;
    cursor += 4096ULL;
    return true;
}

std::string NormalizeGuestPath(const std::string& raw) {
    if (raw.empty()) return "/";
    std::string clean;
    for (char c : raw) {
        if (c == '/' && !clean.empty() && clean.back() == '/') continue;
        clean.push_back(c);
    }
    if (clean.front() != '/') clean.insert(clean.begin(), '/');
    while (clean.size() > 1u && clean.back() == '/') clean.pop_back();
    return clean;
}

} // namespace

bool Elf64GuestLoader::IsValidAArch64Elf(const std::vector<uint8_t>& elf_bytes) {
    if (elf_bytes.size() < sizeof(Elf64Ehdr)) return false;
    Elf64Ehdr eh{};
    std::memcpy(&eh, elf_bytes.data(), sizeof(eh));
    return eh.e_ident[0] == 0x7Fu && eh.e_ident[1] == 'E' &&
           eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F' &&
           eh.e_ident[4] == 2u && eh.e_ident[5] == 1u &&
           eh.e_machine == kElfEmAarch64 &&
           (eh.e_type == kElfEtExec || eh.e_type == kElfEtDyn);
}

bool Elf64GuestLoader::MapPage4K(GuestRam& ram,
                                 uint64_t ttbr0_pa,
                                 uint64_t& phys_alloc_cursor,
                                 uint64_t phys_alloc_limit,
                                 uint64_t va,
                                 uint64_t pa,
                                 bool el0_access,
                                 bool writable,
                                 bool executable) {
    const uint64_t l1_idx = (va >> 30) & 0x1FFULL;
    const uint64_t l2_idx = (va >> 21) & 0x1FFULL;
    const uint64_t l3_idx = (va >> 12) & 0x1FFULL;

    auto ensure_table = [&](uint64_t table_pa, uint64_t idx, uint64_t& next_table_pa) -> bool {
        uint64_t desc = 0;
        if (!ram.ReadU64(table_pa + idx * 8ULL, desc)) return false;
        if ((desc & 3ULL) == 3ULL) {
            next_table_pa = desc & 0x0000FFFFFFFFF000ULL;
            return true;
        }
        if (!AllocZeroPage(ram, phys_alloc_cursor, phys_alloc_limit, next_table_pa)) {
            return false;
        }
        return ram.WriteU64(table_pa + idx * 8ULL, next_table_pa | 3ULL);
    };

    uint64_t l2_pa = 0, l3_pa = 0;
    if (!ensure_table(ttbr0_pa, l1_idx, l2_pa) ||
        !ensure_table(l2_pa, l2_idx, l3_pa)) {
        return false;
    }

    uint64_t leaf = (pa & 0x0000FFFFFFFFF000ULL) | 3ULL; // Valid L3 page
    leaf |= (1ULL << 10);                                 // AF = 1
    if (el0_access) leaf |= (1ULL << 6);                  // AP[1] = 1 (EL0 allowed)
    if (!writable) leaf |= (1ULL << 7);                   // AP[2] = 1 (Read-Only)
    if (el0_access) {
        leaf |= (1ULL << 53);                             // PXN = 1 for user pages
        if (!executable) leaf |= (1ULL << 54);            // UXN = 1 when non-exec
    } else {
        leaf |= (1ULL << 54);                             // UXN = 1 for EL1 pages
        if (!executable) leaf |= (1ULL << 53);            // PXN = 1 when non-exec
    }
    return ram.WriteU64(l3_pa + l3_idx * 8ULL, leaf);
}

Elf64LoadResult Elf64GuestLoader::LoadAndMap(GuestRam& ram,
                                             uint64_t ttbr0_pa,
                                             uint64_t& phys_alloc_cursor,
                                             uint64_t phys_alloc_limit,
                                             const std::vector<uint8_t>& elf_bytes,
                                             const std::vector<std::string>& argv,
                                             const std::vector<std::string>& envp) {
    Elf64LoadResult res;
    if (!IsValidAArch64Elf(elf_bytes)) {
        res.error = "Invalid or non-AArch64 ELF64 binary";
        return res;
    }

    Elf64Ehdr eh{};
    std::memcpy(&eh, elf_bytes.data(), sizeof(eh));
    if (eh.e_phnum == 0 || eh.e_phentsize < sizeof(Elf64Phdr) ||
        eh.e_phoff + static_cast<uint64_t>(eh.e_phnum) * eh.e_phentsize > elf_bytes.size()) {
        res.error = "Corrupt ELF64 program header table";
        return res;
    }

    const uint64_t load_bias = (eh.e_type == kElfEtDyn) ? 0x0000000000400000ULL : 0ULL;
    uint64_t highest_va = load_bias + 0x10000ULL;
    uint64_t first_pt_load_va = 0;
    bool has_pt_load = false;

    std::unordered_map<uint64_t, uint64_t> mapped_va_to_pa;

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64Phdr ph{};
        std::memcpy(&ph, elf_bytes.data() + eh.e_phoff + static_cast<size_t>(i) * eh.e_phentsize, sizeof(ph));
        if (ph.p_type != kPtLoad || ph.p_memsz == 0) continue;
        if (ph.p_offset + ph.p_filesz > elf_bytes.size()) {
            res.error = "ELF64 PT_LOAD segment out of file bounds";
            return res;
        }

        const uint64_t seg_va = load_bias + ph.p_vaddr;
        if (!has_pt_load) {
            first_pt_load_va = AlignDown4K(seg_va);
            has_pt_load = true;
        }
        const uint64_t page_start = AlignDown4K(seg_va);
        const uint64_t page_end = AlignUp4K(seg_va + ph.p_memsz);
        if (page_end > highest_va) highest_va = page_end;

        const bool writable = (ph.p_flags & kPfW) != 0;
        const bool executable = (ph.p_flags & kPfX) != 0;

        for (uint64_t va = page_start; va < page_end; va += 4096ULL) {
            uint64_t pa = 0;
            auto it = mapped_va_to_pa.find(va);
            if (it == mapped_va_to_pa.end()) {
                if (!AllocZeroPage(ram, phys_alloc_cursor, phys_alloc_limit, pa)) {
                    res.error = "Out of guest RAM while mapping ELF64 segment";
                    return res;
                }
                mapped_va_to_pa[va] = pa;
            } else {
                pa = it->second;
            }
            if (!MapPage4K(ram, ttbr0_pa, phys_alloc_cursor, phys_alloc_limit,
                           va, pa, true, writable, executable)) {
                res.error = "Failed installing Stage-1 page table entry for ELF64";
                return res;
            }
        }

        // Copy file-backed bytes page-by-page
        uint64_t copied = 0;
        while (copied < ph.p_filesz) {
            const uint64_t cur_va = seg_va + copied;
            const uint64_t va_pg = AlignDown4K(cur_va);
            const uint64_t pg_off = cur_va - va_pg;
            const size_t chunk = static_cast<size_t>(
                std::min<uint64_t>(4096ULL - pg_off, ph.p_filesz - copied));
            uint64_t pa = mapped_va_to_pa.at(va_pg);
            if (!ram.WriteBytes(pa + pg_off, elf_bytes.data() + ph.p_offset + copied, chunk)) {
                res.error = "Failed writing ELF64 segment bytes to guest RAM";
                return res;
            }
            copied += chunk;
        }
    }

    // Allocate 64 KiB EL0 user stack near top of 32-bit user VA space (0x7FFF0000..0x80000000)
    constexpr uint64_t kUserStackTop = 0x000000007FFF0000ULL;
    constexpr uint64_t kUserStackSize = 64ULL * 1024ULL;
    const uint64_t stack_bottom = kUserStackTop - kUserStackSize;
    std::unordered_map<uint64_t, uint64_t> stack_pages;
    for (uint64_t va = stack_bottom; va < kUserStackTop; va += 4096ULL) {
        uint64_t pa = 0;
        if (!AllocZeroPage(ram, phys_alloc_cursor, phys_alloc_limit, pa) ||
            !MapPage4K(ram, ttbr0_pa, phys_alloc_cursor, phys_alloc_limit,
                       va, pa, true, true, false)) {
            res.error = "Out of guest RAM while mapping EL0 user stack";
            return res;
        }
        stack_pages[va] = pa;
    }

    auto write_stack = [&](uint64_t va, const void* src, size_t len) -> bool {
        size_t off = 0;
        while (off < len) {
            uint64_t cur = va + off;
            uint64_t pg = AlignDown4K(cur);
            size_t in_pg = static_cast<size_t>(cur - pg);
            size_t chunk = std::min(4096u - in_pg, len - off);
            if (!ram.WriteBytes(stack_pages.at(pg) + in_pg,
                                static_cast<const uint8_t*>(src) + off, chunk)) {
                return false;
            }
            off += chunk;
        }
        return true;
    };

    // Build Linux AArch64 initial user stack: strings + AT_RANDOM + auxv + envp + argv + argc
    uint64_t sp = kUserStackTop - 16ULL;
    uint8_t rand16[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    sp -= 16ULL;
    uint64_t random_va = sp;
    write_stack(random_va, rand16, sizeof(rand16));

    std::vector<uint64_t> env_vas;
    for (const auto& s : envp) {
        sp -= (s.size() + 1u);
        write_stack(sp, s.c_str(), s.size() + 1u);
        env_vas.push_back(sp);
    }
    std::vector<uint64_t> arg_vas;
    for (const auto& s : argv) {
        sp -= (s.size() + 1u);
        write_stack(sp, s.c_str(), s.size() + 1u);
        arg_vas.push_back(sp);
    }

    sp &= ~15ULL; // 16-byte AArch64 SP alignment
    std::vector<uint64_t> words;
    words.push_back(argv.size());
    for (uint64_t p : arg_vas) words.push_back(p);
    words.push_back(0ULL);
    for (uint64_t p : env_vas) words.push_back(p);
    words.push_back(0ULL);

    const uint64_t phdr_va = has_pt_load ? (first_pt_load_va + eh.e_phoff) : 0ULL;
    auto push_aux = [&](uint64_t key, uint64_t val) {
        words.push_back(key);
        words.push_back(val);
    };
    push_aux(3u, phdr_va);            // AT_PHDR
    push_aux(4u, eh.e_phentsize);     // AT_PHENT
    push_aux(5u, eh.e_phnum);         // AT_PHNUM
    push_aux(6u, 4096ULL);            // AT_PAGESZ
    push_aux(9u, load_bias + eh.e_entry); // AT_ENTRY
    push_aux(11u, 0ULL);              // AT_UID
    push_aux(12u, 0ULL);              // AT_EUID
    push_aux(25u, random_va);         // AT_RANDOM
    push_aux(0u, 0ULL);               // AT_NULL

    const uint64_t table_bytes = words.size() * sizeof(uint64_t);
    sp = (sp - table_bytes) & ~15ULL;
    if (!write_stack(sp, words.data(), table_bytes)) {
        res.error = "Failed writing initial EL0 stack frame";
        return res;
    }

    res.ok = true;
    res.entry_pc = load_bias + eh.e_entry;
    res.phdr_va = phdr_va;
    res.phent_size = eh.e_phentsize;
    res.phnum = eh.e_phnum;
    res.brk_start = highest_va;
    res.stack_top = kUserStackTop;
    res.initial_sp = sp;
    return res;
}

// =========================================================================
// LinuxGuestSupervisor (EL1 Trap Vector + Linux ARM64 SVC Syscall Engine)
// =========================================================================

LinuxGuestSupervisor::LinuxGuestSupervisor(GuestRam& ram,
                                           Pl011Uart& uart,
                                           PsciController& psci,
                                           const loader::Ext4Reader* ext4)
    : ram_(ram), uart_(uart), psci_(psci), ext4_(ext4) {}

void LinuxGuestSupervisor::SetInitramfs(std::vector<CpioEntry> entries) {
    initramfs_ = std::move(entries);
}

void LinuxGuestSupervisor::SetGuestIdentity(const std::string& cmdline, bool magisk_enabled) {
    cmdline_ = cmdline;
    magisk_enabled_ = magisk_enabled;
}

bool LinuxGuestSupervisor::ResolveVfsFile(const std::string& path,
                                          std::vector<uint8_t>& out_data,
                                          bool& out_is_dir,
                                          std::vector<std::string>& out_children) const {
    const std::string norm = NormalizeGuestPath(path);
    out_data.clear();
    out_children.clear();
    out_is_dir = false;

    if (norm == "/proc/cmdline") {
        std::string text = cmdline_ + "\n";
        out_data.assign(text.begin(), text.end());
        return true;
    }
    if (norm == "/proc/version") {
        std::string text = "Linux version 5.10.209-haseeb #1 SMP PREEMPT aarch64\n";
        out_data.assign(text.begin(), text.end());
        return true;
    }

    // 1. Check initramfs CPIO entries first
    if (norm == "/") {
        out_is_dir = true;
        for (const auto& e : initramfs_) {
            if (e.name.find('/') == std::string::npos) {
                out_children.push_back(e.name);
            }
        }
        if (ext4_ && ext4_->IsOpen()) {
            std::vector<loader::Ext4DirEntry> root_entries;
            if (ext4_->ReadDir("/", root_entries)) {
                for (const auto& re : root_entries) {
                    if (re.name != "." && re.name != "..") out_children.push_back(re.name);
                }
            }
        }
        return true;
    }

    const std::string rel = norm.substr(1);
    if (const CpioEntry* ce = CpioArchive::FindEntry(initramfs_, rel)) {
        if (ce->IsDirectory()) {
            out_is_dir = true;
            std::string prefix = rel + "/";
            for (const auto& item : initramfs_) {
                if (item.name.rfind(prefix, 0) == 0) {
                    std::string tail = item.name.substr(prefix.size());
                    if (!tail.empty() && tail.find('/') == std::string::npos) {
                        out_children.push_back(tail);
                    }
                }
            }
        } else {
            out_data = ce->data;
        }
        return true;
    }

    // 2. Fallback to mount-free Ext4Reader system image
    if (ext4_ && ext4_->IsOpen()) {
        loader::Ext4Stat st{};
        if (ext4_->Stat(norm, st)) {
            if (st.IsDirectory()) {
                out_is_dir = true;
                std::vector<loader::Ext4DirEntry> entries;
                if (ext4_->ReadDir(norm, entries)) {
                    for (const auto& de : entries) out_children.push_back(de.name);
                }
                return true;
            }
            if (ext4_->ReadFile(norm, out_data)) {
                return true;
            }
        }
    }
    return false;
}

bool LinuxGuestSupervisor::MapFreshAddressSpace(AArch64CpuCore& cpu,
                                                const std::vector<uint8_t>& elf_bytes,
                                                const std::vector<std::string>& argv,
                                                std::string& out_error) {
    phys_cursor_ = arena_base_;
    if (!AllocZeroPage(ram_, phys_cursor_, arena_limit_, ttbr0_pa_)) {
        out_error = "Failed allocating guest TTBR0 root page";
        return false;
    }

    // Install EL1 VBAR exception trampoline page at arena_base_ + 0x10000 (identity mapped)
    uint64_t vbar_pa = 0;
    if (!AllocZeroPage(ram_, phys_cursor_, arena_limit_, vbar_pa)) {
        out_error = "Failed allocating EL1 VBAR page";
        return false;
    }
    constexpr uint32_t kInsnHvcLx = 0xD4000002u | (static_cast<uint32_t>(kSyscallHypercallImm) << 5);
    constexpr uint32_t kInsnEret  = 0xD69F03E0u;
    const uint32_t trampoline[2] = {kInsnHvcLx, kInsnEret};
    // Lower EL AArch64 Sync (+0x400) & Current EL SPx Sync/IRQ (+0x200, +0x280)
    ram_.WriteBytes(vbar_pa + 0x200ULL, trampoline, sizeof(trampoline));
    ram_.WriteBytes(vbar_pa + 0x400ULL, trampoline, sizeof(trampoline));
    ram_.WriteBytes(vbar_pa + 0x480ULL, trampoline, sizeof(trampoline));
    if (!Elf64GuestLoader::MapPage4K(ram_, ttbr0_pa_, phys_cursor_, arena_limit_,
                                     vbar_pa, vbar_pa, false, false, true)) {
        out_error = "Failed identity-mapping EL1 VBAR";
        return false;
    }

    std::vector<std::string> envp = {
        "PATH=/sbin:/system/bin:/vendor/bin",
        "HOME=/",
        "TERM=vt100",
        magisk_enabled_ ? "HASEEB_ROOT=magisk" : "HASEEB_ROOT=0",
    };

    Elf64LoadResult load = Elf64GuestLoader::LoadAndMap(
        ram_, ttbr0_pa_, phys_cursor_, arena_limit_, elf_bytes, argv, envp);
    if (!load.ok) {
        out_error = load.error;
        return false;
    }

    current_brk_ = AlignUp4K(load.brk_start);
    mmap_cursor_ = 0x0000001000000000ULL;

    // Configure EL1 MMU + enter EL0 at loaded binary entry point
    cpu.Mmu().InvalidateAll();
    cpu.Cache().InvalidateAll();
    cpu.Reset(load.entry_pc, load.initial_sp, 0);
    auto& st = cpu.State();
    st.sp_el[0] = load.initial_sp;
    st.pstate.el = 0;
    st.pstate.spsel = false;
    st.sys.vbar_el1 = vbar_pa;
    st.sys.ttbr0_el1 = ttbr0_pa_;
    st.sys.tcr_el1 = 25ULL | (1ULL << 23); // T0SZ=25 (39-bit VA, 4K), EPD1=1
    st.sys.mair_el1 = 0xFFULL;
    st.sys.cpacr_el1 = (3ULL << 20);
    st.sys.sctlr_el1 = 0x30D00801ULL; // MMU enabled (M=1)
    st.x[0] = 0;
    return true;
}

VmError LinuxGuestSupervisor::PrepareBoot(AArch64CpuCore& cpu,
                                          uint64_t ram_bytes,
                                          uint64_t kernel_reserved_end,
                                          const std::vector<std::string>& argv,
                                          std::string& out_selected_binary,
                                          std::string& out_error,
                                          uint64_t arena_max_limit) {
    ram_bytes_ = ram_bytes;
    arena_base_ = std::max<uint64_t>(kGuestRamBase + 0x00200000ULL, AlignUp4K(kernel_reserved_end));
    const uint64_t ram_top = kGuestRamBase + ram_bytes;
    const uint64_t default_top = ram_top - std::min<uint64_t>(ram_bytes / 4u, 0x00400000ULL);
    arena_limit_ = (arena_max_limit > 0ULL && arena_max_limit < default_top)
                       ? AlignDown4K(arena_max_limit)
                       : default_top;
    if (arena_limit_ <= arena_base_ + 0x00200000ULL) {
        out_error = "Insufficient guest RAM for EL0 init address space";
        return VmError::OutOfMemory;
    }

    exited_ = false;
    exit_status_ = 0;
    open_fds_.clear();
    next_fd_ = 3;

    // Priority order matching Android/Magisk boot flow: /magiskinit -> /init -> /system/bin/init -> /system/bin/sh
    std::vector<std::string> candidates;
    if (magisk_enabled_) candidates.push_back("/magiskinit");
    candidates.push_back("/init");
    if (!magisk_enabled_) candidates.push_back("/magiskinit");
    candidates.push_back("/system/bin/init");
    candidates.push_back("/system/bin/sh");

    std::vector<uint8_t> elf_bytes;
    std::string chosen;
    for (const auto& path : candidates) {
        std::vector<uint8_t> data;
        bool is_dir = false;
        std::vector<std::string> children;
        if (ResolveVfsFile(path, data, is_dir, children) && !is_dir &&
            Elf64GuestLoader::IsValidAArch64Elf(data)) {
            elf_bytes = std::move(data);
            chosen = path;
            break;
        }
    }

    if (chosen.empty()) {
        out_error = "No valid AArch64 ELF64 init/magiskinit/sh binary found in initramfs or system.img";
        return VmError::FileNotFound;
    }

    std::vector<std::string> eff_argv = argv;
    if (eff_argv.empty()) eff_argv.push_back(chosen);
    active_binary_ = chosen;
    out_selected_binary = chosen;

    if (!MapFreshAddressSpace(cpu, elf_bytes, eff_argv, out_error)) {
        return VmError::ExecutionTrap;
    }
    return VmError::Ok;
}

bool LinuxGuestSupervisor::ReadUserBytes(AArch64CpuCore& cpu,
                                         uint64_t uva,
                                         void* dst,
                                         size_t len) const {
    auto* out = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < len; ++i) {
        MmuTranslationResult tr = cpu.Mmu().Translate(cpu.State(), uva + i, MemAccessType::Read, false);
        if (!tr.ok || !ram_.ReadBytes(tr.phys_addr, out + i, 1)) return false;
    }
    return true;
}

bool LinuxGuestSupervisor::WriteUserBytes(AArch64CpuCore& cpu,
                                          uint64_t uva,
                                          const void* src,
                                          size_t len) const {
    const auto* in = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < len; ++i) {
        MmuTranslationResult tr = cpu.Mmu().Translate(cpu.State(), uva + i, MemAccessType::Write, false);
        if (!tr.ok || !ram_.WriteBytes(tr.phys_addr, in + i, 1)) return false;
    }
    return true;
}

bool LinuxGuestSupervisor::ReadUserString(AArch64CpuCore& cpu,
                                          uint64_t uva,
                                          std::string& out_str) const {
    out_str.clear();
    for (size_t i = 0; i < 4096u; ++i) {
        char ch = 0;
        if (!ReadUserBytes(cpu, uva + i, &ch, 1)) return false;
        if (ch == '\0') return true;
        out_str.push_back(ch);
    }
    return false;
}

int64_t LinuxGuestSupervisor::DispatchSyscall(AArch64CpuCore& cpu,
                                              CpuState& st,
                                              bool& out_halt,
                                              bool& out_retry) {
    out_halt = false;
    out_retry = false;
    const uint64_t nr = st.x[8];

    switch (nr) {
        case 17: { // getcwd(buf, size)
            uint64_t buf = st.x[0];
            uint64_t sz = st.x[1];
            const char cwd[] = "/";
            if (sz < sizeof(cwd)) return -34; // ERANGE
            return WriteUserBytes(cpu, buf, cwd, sizeof(cwd)) ? static_cast<int64_t>(buf) : -14;
        }
        case 25: // fcntl
        case 29: // ioctl
            return 0;
        case 48: // faccessat(dirfd, pathname, mode, flags)
        case 56: { // openat(dirfd, pathname, flags, mode)
            std::string path;
            if (!ReadUserString(cpu, st.x[1], path)) return -14;
            std::vector<uint8_t> data;
            bool is_dir = false;
            std::vector<std::string> children;
            if (!ResolveVfsFile(path, data, is_dir, children)) return -2; // ENOENT
            if (nr == 48) return 0;
            int fd = next_fd_++;
            OpenFile of;
            of.path = NormalizeGuestPath(path);
            of.data = std::move(data);
            of.is_dir = is_dir;
            of.dir_children = std::move(children);
            open_fds_.emplace(fd, std::move(of));
            return fd;
        }
        case 57: { // close(fd)
            int fd = static_cast<int>(st.x[0]);
            if (fd <= 2) return 0;
            return (open_fds_.erase(fd) > 0) ? 0 : -9;
        }
        case 62: { // lseek
            int fd = static_cast<int>(st.x[0]);
            auto it = open_fds_.find(fd);
            if (it == open_fds_.end()) return -9;
            int64_t off = static_cast<int64_t>(st.x[1]);
            int whence = static_cast<int>(st.x[2]);
            int64_t target = off;
            if (whence == 1) target = static_cast<int64_t>(it->second.offset) + off;
            else if (whence == 2) target = static_cast<int64_t>(it->second.data.size()) + off;
            if (target < 0) return -22;
            it->second.offset = static_cast<size_t>(target);
            return target;
        }
        case 63: { // read(fd, buf, count)
            int fd = static_cast<int>(st.x[0]);
            uint64_t buf = st.x[1];
            size_t count = static_cast<size_t>(st.x[2]);
            if (fd == 0) {
                size_t got = 0;
                while (got < count) {
                    if ((uart_.Read(0x018ULL, 4) & (1u << 4)) != 0u) break; // RXFE
                    uint8_t b = static_cast<uint8_t>(uart_.Read(0x000ULL, 4) & 0xFFu);
                    if (!WriteUserBytes(cpu, buf + got, &b, 1)) return -14;
                    ++got;
                    if (b == '\n' || b == '\r') break;
                }
                if (got == 0 && count > 0) {
                    // Yield CPU until stdin bytes arrive on PL011 RX; preserve x0..x2/x8 for retry
                    st.waiting_for_interrupt = true;
                    st.sys.elr_el1 -= 4ULL;
                    out_retry = true;
                    return 0;
                }
                return static_cast<int64_t>(got);
            }
            auto it = open_fds_.find(fd);
            if (it == open_fds_.end() || it->second.is_dir) return -9;
            size_t avail = (it->second.offset < it->second.data.size())
                               ? (it->second.data.size() - it->second.offset)
                               : 0u;
            size_t n = std::min(count, avail);
            if (n > 0 && !WriteUserBytes(cpu, buf, it->second.data.data() + it->second.offset, n)) {
                return -14;
            }
            it->second.offset += n;
            return static_cast<int64_t>(n);
        }
        case 64: { // write(fd, buf, count)
            int fd = static_cast<int>(st.x[0]);
            uint64_t buf = st.x[1];
            size_t count = static_cast<size_t>(st.x[2]);
            if (fd != 1 && fd != 2) return -9;
            std::vector<uint8_t> tmp(count);
            if (count > 0 && !ReadUserBytes(cpu, buf, tmp.data(), count)) return -14;
            for (uint8_t b : tmp) {
                uart_.Write(0x000ULL, 1, b);
            }
            return static_cast<int64_t>(count);
        }
        case 66: { // writev(fd, iov, iovcnt)
            int fd = static_cast<int>(st.x[0]);
            uint64_t iov_va = st.x[1];
            int iovcnt = static_cast<int>(st.x[2]);
            if (fd != 1 && fd != 2) return -9;
            int64_t total = 0;
            for (int i = 0; i < iovcnt; ++i) {
                uint64_t vec[2]{};
                if (!ReadUserBytes(cpu, iov_va + static_cast<uint64_t>(i) * 16ULL, vec, 16)) return -14;
                if (vec[1] > 0) {
                    std::vector<uint8_t> tmp(static_cast<size_t>(vec[1]));
                    if (!ReadUserBytes(cpu, vec[0], tmp.data(), tmp.size())) return -14;
                    for (uint8_t b : tmp) uart_.Write(0x000ULL, 1, b);
                    total += static_cast<int64_t>(vec[1]);
                }
            }
            return total;
        }
        case 79: // newfstatat(dirfd, pathname, statbuf, flags)
        case 80: { // fstat(fd, statbuf)
            uint64_t stat_va = (nr == 79) ? st.x[2] : st.x[1];
            bool is_dir = false;
            uint64_t size_bytes = 0;
            if (nr == 79) {
                std::string path;
                if (!ReadUserString(cpu, st.x[1], path)) return -14;
                std::vector<uint8_t> data;
                std::vector<std::string> children;
                if (!ResolveVfsFile(path, data, is_dir, children)) return -2;
                size_bytes = data.size();
            } else {
                int fd = static_cast<int>(st.x[0]);
                if (fd <= 2) {
                    size_bytes = 0;
                } else {
                    auto it = open_fds_.find(fd);
                    if (it == open_fds_.end()) return -9;
                    is_dir = it->second.is_dir;
                    size_bytes = it->second.data.size();
                }
            }
            struct LinuxStatAArch64 {
                uint64_t st_dev;
                uint64_t st_ino;
                uint32_t st_mode;
                uint32_t st_nlink;
                uint32_t st_uid;
                uint32_t st_gid;
                uint64_t st_rdev;
                uint64_t pad1;
                int64_t  st_size;
                int32_t  st_blksize;
                int32_t  pad2;
                int64_t  st_blocks;
                uint64_t sec_pad[6];
            } ks{};
            ks.st_dev = 1;
            ks.st_ino = 1;
            ks.st_mode = is_dir ? 0040755u : 0100755u;
            ks.st_nlink = 1;
            ks.st_size = static_cast<int64_t>(size_bytes);
            ks.st_blksize = 4096;
            ks.st_blocks = static_cast<int64_t>((size_bytes + 511u) / 512u);
            return WriteUserBytes(cpu, stat_va, &ks, sizeof(ks)) ? 0 : -14;
        }
        case 93:   // exit
        case 94: { // exit_group
            exit_status_ = static_cast<int>(st.x[0]);
            exited_ = true;
            out_halt = true;
            return 0;
        }
        case 96:  return 1; // set_tid_address
        case 113: { // clock_gettime(clk_id, tp)
            uint64_t tp[2] = {1700000000ULL, 0ULL};
            return WriteUserBytes(cpu, st.x[1], tp, sizeof(tp)) ? 0 : -14;
        }
        case 134: // rt_sigaction
        case 135: // rt_sigprocmask
            return 0;
        case 160: { // uname(struct new_utsname* buf)
            struct LinuxUtsName {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
                char domainname[65];
            } uts{};
            std::snprintf(uts.sysname, 65, "Linux");
            std::snprintf(uts.nodename, 65, "haseeb-android");
            std::snprintf(uts.release, 65, "5.10.209-haseeb");
            std::snprintf(uts.version, 65, "#1 SMP PREEMPT");
            std::snprintf(uts.machine, 65, "aarch64");
            std::snprintf(uts.domainname, 65, "localdomain");
            return WriteUserBytes(cpu, st.x[0], &uts, sizeof(uts)) ? 0 : -14;
        }
        case 172: // getpid
        case 178: // gettid
            return 1;
        case 173: // getppid
        case 174: // getuid
        case 175: // geteuid
        case 176: // getgid
        case 177: // getegid
            return 0;
        case 214: { // brk(addr)
            uint64_t req = st.x[0];
            if (req == 0ULL || req <= current_brk_) return static_cast<int64_t>(current_brk_);
            uint64_t end_pg = AlignUp4K(req);
            for (uint64_t va = current_brk_; va < end_pg; va += 4096ULL) {
                uint64_t pa = 0;
                if (!AllocZeroPage(ram_, phys_cursor_, arena_limit_, pa) ||
                    !Elf64GuestLoader::MapPage4K(ram_, ttbr0_pa_, phys_cursor_, arena_limit_,
                                                 va, pa, true, true, false)) {
                    return static_cast<int64_t>(current_brk_);
                }
            }
            current_brk_ = end_pg;
            return static_cast<int64_t>(req);
        }
        case 221: { // execve(pathname, argv, envp)
            std::string path;
            if (!ReadUserString(cpu, st.x[0], path)) return -14;
            std::vector<uint8_t> elf_bytes;
            bool is_dir = false;
            std::vector<std::string> children;
            if (!ResolveVfsFile(path, elf_bytes, is_dir, children) || is_dir ||
                !Elf64GuestLoader::IsValidAArch64Elf(elf_bytes)) {
                return -2;
            }
            std::vector<std::string> argv = {path};
            if (st.x[1] != 0ULL) {
                argv.clear();
                for (size_t idx = 0; idx < 16u; ++idx) {
                    uint64_t ptr = 0;
                    if (!ReadUserBytes(cpu, st.x[1] + idx * 8ULL, &ptr, 8) || ptr == 0ULL) break;
                    std::string arg;
                    if (ReadUserString(cpu, ptr, arg)) argv.push_back(arg);
                }
                if (argv.empty()) argv.push_back(path);
            }
            std::string err;
            active_binary_ = path;
            if (!MapFreshAddressSpace(cpu, elf_bytes, argv, err)) return -8;
            // Stay in EL1h so the immediately following ERET at VBAR+0x404 cleanly transitions to EL0t
            const uint64_t new_entry = st.pc;
            const uint64_t new_sp_el0 = st.sp_el[0];
            st.pstate.el = 1;
            st.pstate.spsel = true;
            st.pstate.d = true;
            st.pstate.a = true;
            st.pstate.i = true;
            st.pstate.f = true;
            st.sys.elr_el1 = new_entry;
            st.sys.spsr_el1 = 0ULL; // EL0t
            st.sp_el[0] = new_sp_el0;
            st.x[0] = 0ULL;
            return 0;
        }
        case 222: { // mmap(addr, length, prot, flags, fd, offset)
            uint64_t addr = st.x[0];
            uint64_t len = AlignUp4K(st.x[1]);
            uint64_t prot = st.x[2];
            if (len == 0ULL) return -22;
            if (addr == 0ULL) {
                addr = mmap_cursor_;
                mmap_cursor_ += len + 4096ULL;
            } else {
                addr = AlignDown4K(addr);
            }
            const bool w = (prot & 2ULL) != 0;
            const bool x = (prot & 4ULL) != 0;
            for (uint64_t va = addr; va < addr + len; va += 4096ULL) {
                uint64_t pa = 0;
                if (!AllocZeroPage(ram_, phys_cursor_, arena_limit_, pa) ||
                    !Elf64GuestLoader::MapPage4K(ram_, ttbr0_pa_, phys_cursor_, arena_limit_,
                                                 va, pa, true, w, x)) {
                    return -12; // ENOMEM
                }
            }
            return static_cast<int64_t>(addr);
        }
        case 215: // munmap
        case 226: // mprotect
            return 0;
        case 278: { // getrandom(buf, buflen, flags)
            uint64_t buf = st.x[0];
            size_t len = static_cast<size_t>(st.x[1]);
            std::vector<uint8_t> rnd(len);
            std::random_device rd;
            for (size_t i = 0; i < len; ++i) rnd[i] = static_cast<uint8_t>(rd());
            return WriteUserBytes(cpu, buf, rnd.data(), len) ? static_cast<int64_t>(len) : -14;
        }
        default:
            return -38; // ENOSYS
    }
}

HypercallResult LinuxGuestSupervisor::HandleHypercall(AArch64CpuCore& cpu,
                                                      CpuState& state,
                                                      uint16_t imm16,
                                                      bool is_smc) {
    if (!is_smc && imm16 == kSyscallHypercallImm) {
        const uint32_t ec = static_cast<uint32_t>(state.sys.esr_el1 >> 26) & 0x3Fu;
        if (ec == ec::kSvc64) {
            bool halt = false;
            bool retry = false;
            int64_t ret = DispatchSyscall(cpu, state, halt, retry);
            if (!retry) {
                state.x[0] = static_cast<uint64_t>(ret);
            }
            return HypercallResult{true, halt};
        }
        // Unexpected synchronous/IRQ fault inside EL0/EL1 supervisor
        uart_.Write(0x000ULL, 1, '!');
        exited_ = true;
        exit_status_ = -1;
        return HypercallResult{true, true};
    }
    return psci_.HandleHypercall(state, imm16, is_smc);
}

} // namespace haseeb::vm
