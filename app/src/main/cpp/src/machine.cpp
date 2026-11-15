#include "haseeb/machine.hpp"

#include <algorithm>
#include <sstream>

namespace haseeb::vm {

namespace {

// AArch64 second-stage bootloader opcode encoders
uint32_t EncodeMovz(uint8_t xd, uint16_t imm16, uint8_t hw) {
    return 0xD2800000u | (static_cast<uint32_t>(hw & 3u) << 21) |
           (static_cast<uint32_t>(imm16) << 5) | (xd & 31u);
}

uint32_t EncodeMovk(uint8_t xd, uint16_t imm16, uint8_t hw) {
    return 0xF2800000u | (static_cast<uint32_t>(hw & 3u) << 21) |
           (static_cast<uint32_t>(imm16) << 5) | (xd & 31u);
}

void EmitLoadImm64(std::vector<uint32_t>& code, uint8_t xd, uint64_t val) {
    code.push_back(EncodeMovz(xd, static_cast<uint16_t>(val & 0xFFFFu), 0));
    code.push_back(EncodeMovk(xd, static_cast<uint16_t>((val >> 16) & 0xFFFFu), 1));
    code.push_back(EncodeMovk(xd, static_cast<uint16_t>((val >> 32) & 0xFFFFu), 2));
    code.push_back(EncodeMovk(xd, static_cast<uint16_t>((val >> 48) & 0xFFFFu), 3));
}

uint32_t EncodeLdrbPostInc(uint8_t wt, uint8_t xn, int16_t simm9) {
    uint32_t u9 = static_cast<uint32_t>(simm9) & 0x1FFu;
    return 0x38400400u | (u9 << 12) | (static_cast<uint32_t>(xn & 31u) << 5) | (wt & 31u);
}

uint32_t EncodeStrbZero(uint8_t wt, uint8_t xn) {
    return 0x39000000u | (static_cast<uint32_t>(xn & 31u) << 5) | (wt & 31u);
}

uint32_t EncodeCbz64(uint8_t xt, int32_t rel_insns) {
    uint32_t u19 = static_cast<uint32_t>(rel_insns) & 0x7FFFFu;
    return 0xB4000000u | (u19 << 5) | (xt & 31u);
}

uint32_t EncodeB(int32_t rel_insns) {
    uint32_t u26 = static_cast<uint32_t>(rel_insns) & 0x03FFFFFFu;
    return 0x14000000u | u26;
}

uint32_t EncodeHlt(uint16_t imm16) {
    return 0xD4400000u | (static_cast<uint32_t>(imm16) << 5);
}

int64_t SignExtend(uint64_t v, int bits) {
    uint64_t shift = 64 - bits;
    return static_cast<int64_t>(v << shift) >> shift;
}

} // namespace

VirtualMachineSkeleton::VirtualMachineSkeleton(Pl011Uart::TxLineCallback uart_cb)
    : uart_(std::move(uart_cb)) {}

VirtualMachineSkeleton::~VirtualMachineSkeleton() {
    Shutdown();
}

void VirtualMachineSkeleton::Shutdown() {
    stop_requested_.store(true);
    uart_.FlushLine();
    mmio_bus_.Clear();
    ram_.Free();
    dtb_guest_addr_ = 0;
    kernel_guest_addr_ = 0;
    initrd_guest_addr_ = 0;
    initrd_size_ = 0;
}

bool VirtualMachineSkeleton::LoadPhysU8(uint64_t addr, uint8_t& out) const {
    uint64_t v = 0;
    if (mmio_bus_.Read(addr, 1, v)) {
        out = static_cast<uint8_t>(v & 0xFFu);
        return true;
    }
    return ram_.ReadU8(addr, out);
}

bool VirtualMachineSkeleton::LoadPhysU32(uint64_t addr, uint32_t& out) const {
    uint64_t v = 0;
    if (mmio_bus_.Read(addr, 4, v)) {
        out = static_cast<uint32_t>(v & 0xFFFFFFFFu);
        return true;
    }
    return ram_.ReadU32(addr, out);
}

bool VirtualMachineSkeleton::LoadPhysU64(uint64_t addr, uint64_t& out) const {
    uint64_t v = 0;
    if (mmio_bus_.Read(addr, 8, v)) {
        out = v;
        return true;
    }
    return ram_.ReadU64(addr, out);
}

bool VirtualMachineSkeleton::StorePhysU8(uint64_t addr, uint8_t val) {
    if (mmio_bus_.Write(addr, 1, val)) return true;
    return ram_.WriteU8(addr, val);
}

bool VirtualMachineSkeleton::StorePhysU32(uint64_t addr, uint32_t val) {
    if (mmio_bus_.Write(addr, 4, val)) return true;
    return ram_.WriteU32(addr, val);
}

bool VirtualMachineSkeleton::StorePhysU64(uint64_t addr, uint64_t val) {
    if (mmio_bus_.Write(addr, 8, val)) return true;
    return ram_.WriteU64(addr, val);
}

void VirtualMachineSkeleton::EmitBootloaderStub(uint64_t dtb_addr,
                                                uint64_t msg_addr,
                                                const std::string& banner_msg) {
    // Place null-terminated banner string in RAM
    std::string nul_msg = banner_msg;
    nul_msg.push_back('\0');
    ram_.WriteBytes(msg_addr, nul_msg.data(), nul_msg.size());

    // Assemble AArch64 second-stage bootloader at kBootStubBase (0x40000000)
    std::vector<uint32_t> code;
    EmitLoadImm64(code, 0, dtb_addr);        // Linux arm64 boot protocol: x0 = physical DTB ptr
    EmitLoadImm64(code, 1, 0);               // x1 = 0
    EmitLoadImm64(code, 2, 0);               // x2 = 0
    EmitLoadImm64(code, 3, 0);               // x3 = 0
    EmitLoadImm64(code, 10, kPl011UartBase); // x10 = UARTDR base
    EmitLoadImm64(code, 11, msg_addr);       // x11 = banner string cursor

    // UART print loop:
    // loop:
    //   ldrb w12, [x11], #1
    //   cbz  x12, done (+3 insns)
    //   strb w12, [x10, #0]
    //   b    loop (-3 insns)
    // done:
    //   hlt  #0 (M1 kernel handoff point; M2 replaces HLT target with PC = kernel_guest_addr_)
    code.push_back(EncodeLdrbPostInc(12, 11, 1));
    code.push_back(EncodeCbz64(12, 3));
    code.push_back(EncodeStrbZero(12, 10));
    code.push_back(EncodeB(-3));
    code.push_back(EncodeHlt(0));

    ram_.WriteBytes(kBootStubBase, code.data(), code.size() * sizeof(uint32_t));
}

VmError VirtualMachineSkeleton::InitializeAndStage(const MachineBootConfig& config,
                                                     const ParsedBootImage* boot_img,
                                                     std::string& out_error) {
    Shutdown();
    stop_requested_.store(false);
    uart_.Reset();

    const uint64_t ram_bytes = static_cast<uint64_t>(config.ram_mb) * 1024ULL * 1024ULL;
    VmError err = ram_.Allocate(kGuestRamBase, ram_bytes);
    if (err != VmError::Ok) {
        out_error = "Failed to allocate guest RAM (" + std::to_string(config.ram_mb) + " MiB)";
        return err;
    }

    mmio_bus_.Register(kPl011UartBase, 0x1000ULL, &uart_);

    std::string combined_cmdline = "console=ttyAMA0 earlycon=pl011,0x09000000 "
                                   "androidboot.hardware=virt";
    if (boot_img && !boot_img->cmdline.empty()) {
        combined_cmdline += " " + boot_img->cmdline;
    }
    if (!config.extra_cmdline.empty()) {
        combined_cmdline += " " + config.extra_cmdline;
    }

    // 1. Stage initrd ramdisk if provided
    if (boot_img && !boot_img->ramdisk.empty()) {
        if (!ram_.WriteBytes(kInitrdLoadBase, boot_img->ramdisk.data(), boot_img->ramdisk.size())) {
            out_error = "Ramdisk exceeds guest RAM bounds";
            return VmError::OutOfMemory;
        }
        initrd_guest_addr_ = kInitrdLoadBase;
        initrd_size_ = boot_img->ramdisk.size();
    }

    // 2. Stage kernel Image if provided
    uint64_t kernel_offset = 0x80000ULL;
    if (boot_img && !boot_img->kernel.empty()) {
        if (boot_img->kernel_info.valid_arm64_magic) {
            kernel_offset = boot_img->kernel_info.text_offset;
        }
        kernel_guest_addr_ = kGuestRamBase + kernel_offset;
        if (!ram_.WriteBytes(kernel_guest_addr_, boot_img->kernel.data(), boot_img->kernel.size())) {
            out_error = "Kernel image exceeds guest RAM bounds";
            return VmError::GuestKernelInvalid;
        }
    } else {
        kernel_guest_addr_ = kGuestRamBase + kernel_offset;
    }

    // 3. Compile DTB via FdtBuilder and place near top of RAM (aligned to 16 KiB)
    std::vector<uint8_t> dtb_blob;
    FdtBuildParams fdt_params;
    fdt_params.ram_base = kGuestRamBase;
    fdt_params.ram_size_bytes = ram_bytes;
    fdt_params.vcpu_count = std::clamp<uint32_t>(config.vcpu_count, 1u, 4u);
    fdt_params.initrd_start = initrd_guest_addr_;
    fdt_params.initrd_end = initrd_guest_addr_ + initrd_size_;
    fdt_params.bootargs = combined_cmdline;

    if (!FdtBuilder::Build(fdt_params, dtb_blob, out_error)) {
        return VmError::FdtBuildFailed;
    }

    const uint64_t ram_top = kGuestRamBase + ram_bytes;
    dtb_guest_addr_ = (ram_top - std::min<uint64_t>(ram_bytes / 4, kFdtMaxOffsetEnd)) & ~0x3FFFULL;
    if (!ram_.WriteBytes(dtb_guest_addr_, dtb_blob.data(), dtb_blob.size())) {
        out_error = "Failed writing compiled DTB into guest RAM";
        return VmError::OutOfMemory;
    }

    // 4. Emit AArch64 second-stage bootloader stub & set CPU reset state
    std::ostringstream banner;
    banner << "[HASEEB-VM] AArch64 bootloader stub initialized\r\n"
           << "[HASEEB-VM] RAM: " << config.ram_mb << " MiB @ 0x40000000, DTB @ 0x"
           << std::hex << dtb_guest_addr_ << std::dec << " (" << dtb_blob.size() << " B)\r\n"
           << "[HASEEB-VM] Kernel @ 0x" << std::hex << kernel_guest_addr_ << std::dec
           << ", Initrd @ 0x" << std::hex << initrd_guest_addr_ << std::dec
           << " (" << initrd_size_ << " B)\r\n"
           << "[HASEEB-VM] Bootargs: " << combined_cmdline << "\r\n";
    if (boot_img && boot_img->magisk_detected) {
        banner << "[HASEEB-VM] Magisk ramdisk signature detected: "
               << boot_img->magisk_evidence << "\r\n";
    }
    banner << "[HASEEB-VM] PL011 UART console online; awaiting M2 full AArch64 DBT CPU handoff.\r\n";

    EmitBootloaderStub(dtb_guest_addr_, kBootStubBase + 0x400ULL, banner.str());

    cpu_ = M1BootCpuState{};
    cpu_.pc = kBootStubBase;
    cpu_.sp = dtb_guest_addr_;
    return VmError::Ok;
}

VmError VirtualMachineSkeleton::RunBootStubUntilHalt(uint64_t max_steps) {
    if (!ram_.IsAllocated()) return VmError::NotRunning;

    while (!cpu_.halted && !stop_requested_.load() && cpu_.instructions_executed < max_steps) {
        uint32_t insn = 0;
        if (!LoadPhysU32(cpu_.pc, insn)) {
            return VmError::ExecutionTrap;
        }

        uint64_t next_pc = cpu_.pc + 4ull;

        // Decode subset emitted by M1 bootloader stub (full ISA moves to M2 DBT engine)
        if ((insn & 0x7F800000u) == 0x52800000u && (insn & 0x80000000u) != 0u) {
            // MOVZ / MOVK 64-bit: bit31=1, bits30..29 = 10(MOVZ) / 11(MOVK)
            uint8_t opc = static_cast<uint8_t>((insn >> 29) & 3u);
            uint8_t hw = static_cast<uint8_t>((insn >> 21) & 3u);
            uint64_t imm16 = (insn >> 5) & 0xFFFFu;
            uint8_t rd = static_cast<uint8_t>(insn & 31u);
            if (rd < 31u) {
                if (opc == 2u) { // MOVZ
                    cpu_.x[rd] = imm16 << (hw * 16u);
                } else if (opc == 3u) { // MOVK
                    uint64_t mask = 0xFFFFULL << (hw * 16u);
                    cpu_.x[rd] = (cpu_.x[rd] & ~mask) | (imm16 << (hw * 16u));
                } else {
                    return VmError::ExecutionTrap;
                }
            }
        } else if ((insn & 0xFFE00C00u) == 0x38400400u) {
            // LDRB Wt, [Xn], #simm9 (post-index)
            uint8_t rt = static_cast<uint8_t>(insn & 31u);
            uint8_t rn = static_cast<uint8_t>((insn >> 5) & 31u);
            int64_t simm9 = SignExtend((insn >> 12) & 0x1FFu, 9);
            uint64_t addr = (rn == 31u) ? cpu_.sp : cpu_.x[rn];
            uint8_t val = 0;
            if (!LoadPhysU8(addr, val)) return VmError::ExecutionTrap;
            if (rt < 31u) cpu_.x[rt] = val;
            if (rn == 31u) cpu_.sp += static_cast<uint64_t>(simm9);
            else cpu_.x[rn] += static_cast<uint64_t>(simm9);
        } else if ((insn & 0xFFC00000u) == 0x39000000u) {
            // STRB Wt, [Xn, #uimm12]
            uint8_t rt = static_cast<uint8_t>(insn & 31u);
            uint8_t rn = static_cast<uint8_t>((insn >> 5) & 31u);
            uint64_t imm12 = (insn >> 10) & 0xFFFu;
            uint64_t addr = ((rn == 31u) ? cpu_.sp : cpu_.x[rn]) + imm12;
            uint8_t val = (rt == 31u) ? 0u : static_cast<uint8_t>(cpu_.x[rt] & 0xFFu);
            if (!StorePhysU8(addr, val)) return VmError::ExecutionTrap;
        } else if ((insn & 0xFF000000u) == 0xB4000000u) {
            // CBZ Xt, #simm19
            uint8_t rt = static_cast<uint8_t>(insn & 31u);
            int64_t off = SignExtend((insn >> 5) & 0x7FFFFu, 19) * 4;
            uint64_t val = (rt == 31u) ? 0ull : cpu_.x[rt];
            if (val == 0ull) {
                next_pc = static_cast<uint64_t>(static_cast<int64_t>(cpu_.pc) + off);
            }
        } else if ((insn & 0xFC000000u) == 0x14000000u) {
            // B #simm26
            int64_t off = SignExtend(insn & 0x03FFFFFFu, 26) * 4;
            next_pc = static_cast<uint64_t>(static_cast<int64_t>(cpu_.pc) + off);
        } else if ((insn & 0xFFE0001Fu) == 0xD4400000u) {
            // HLT #imm16
            cpu_.halted = true;
        } else {
            return VmError::ExecutionTrap;
        }

        cpu_.pc = next_pc;
        cpu_.instructions_executed++;
    }

    uart_.FlushLine();
    return cpu_.halted ? VmError::Ok : VmError::ExecutionTrap;
}

} // namespace haseeb::vm
