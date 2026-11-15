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

} // namespace

VirtualMachineSkeleton::VirtualMachineSkeleton(Pl011Uart::TxLineCallback uart_cb)
    : uart_(std::move(uart_cb)), cpu_(ram_, mmio_bus_, 0) {}

VirtualMachineSkeleton::~VirtualMachineSkeleton() {
    Shutdown();
}

void VirtualMachineSkeleton::Shutdown() {
    stop_requested_.store(true);
    cpu_.RequestStop();
    uart_.FlushLine();
    mmio_bus_.Clear();
    ram_.Free();
    dtb_guest_addr_ = 0;
    kernel_guest_addr_ = 0;
    initrd_guest_addr_ = 0;
    initrd_size_ = 0;
}

void VirtualMachineSkeleton::EmitBootloaderStub(uint64_t dtb_addr,
                                                uint64_t msg_addr,
                                                const std::string& banner_msg) {
    std::string nul_msg = banner_msg;
    nul_msg.push_back('\0');
    ram_.WriteBytes(msg_addr, nul_msg.data(), nul_msg.size());

    std::vector<uint32_t> code;
    EmitLoadImm64(code, 0, dtb_addr);        // Linux arm64 boot protocol: x0 = physical DTB ptr
    EmitLoadImm64(code, 1, 0);               // x1 = 0
    EmitLoadImm64(code, 2, 0);               // x2 = 0
    EmitLoadImm64(code, 3, 0);               // x3 = 0
    EmitLoadImm64(code, 10, kPl011UartBase); // x10 = UARTDR base
    EmitLoadImm64(code, 11, msg_addr);       // x11 = banner string cursor

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

    if (boot_img && !boot_img->ramdisk.empty()) {
        if (!ram_.WriteBytes(kInitrdLoadBase, boot_img->ramdisk.data(), boot_img->ramdisk.size())) {
            out_error = "Ramdisk exceeds guest RAM bounds";
            return VmError::OutOfMemory;
        }
        initrd_guest_addr_ = kInitrdLoadBase;
        initrd_size_ = boot_img->ramdisk.size();
    }

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
    banner << "[HASEEB-VM] PL011 UART online; M2 AArch64 DBT CPU & Stage-1 MMU active.\r\n";

    EmitBootloaderStub(dtb_guest_addr_, kBootStubBase + 0x400ULL, banner.str());

    cpu_.Reset(kBootStubBase, dtb_guest_addr_, 1);
    return VmError::Ok;
}

VmError VirtualMachineSkeleton::RunBootStubUntilHalt(uint64_t max_steps) {
    if (!ram_.IsAllocated()) return VmError::NotRunning;

    VmError res = cpu_.Run(max_steps);
    uart_.FlushLine();
    if (res != VmError::Ok) return res;
    return cpu_.State().halted ? VmError::Ok : VmError::ExecutionTrap;
}

} // namespace haseeb::vm
