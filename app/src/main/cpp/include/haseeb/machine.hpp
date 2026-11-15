#pragma once

#include "haseeb/boot_image.hpp"
#include "haseeb/cpu_core.hpp"
#include "haseeb/fdt_builder.hpp"
#include "haseeb/guest_ram.hpp"
#include "haseeb/pl011_uart.hpp"
#include "haseeb/vm_types.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace haseeb::vm {

// Virtual machine host board: owns guest memfd RAM, MMIO bus, PL011 UART, FDT,
// boot image staging, and the M2 AArch64 DBT CPU + Stage-1 MMU core.
class VirtualMachineSkeleton {
public:
    explicit VirtualMachineSkeleton(Pl011Uart::TxLineCallback uart_cb = nullptr);
    ~VirtualMachineSkeleton();

    VirtualMachineSkeleton(const VirtualMachineSkeleton&) = delete;
    VirtualMachineSkeleton& operator=(const VirtualMachineSkeleton&) = delete;

    VmError InitializeAndStage(const MachineBootConfig& config,
                               const ParsedBootImage* boot_img,
                               std::string& out_error);

    // Executes the staged AArch64 code on the M2 DBT CPU core until HLT/budget/stop
    VmError RunBootStubUntilHalt(uint64_t max_steps = 100000ULL);
    void Shutdown();

    bool IsStaged() const { return ram_.IsAllocated(); }
    uint64_t DtbGuestAddr() const { return dtb_guest_addr_; }
    uint64_t KernelGuestAddr() const { return kernel_guest_addr_; }
    uint64_t InitrdGuestAddr() const { return initrd_guest_addr_; }
    uint64_t InitrdSizeBytes() const { return initrd_size_; }

    const CpuState& CpuState() const { return cpu_.State(); }
    AArch64CpuCore& Cpu() { return cpu_; }
    const AArch64CpuCore& Cpu() const { return cpu_; }

    const Pl011Uart& Uart() const { return uart_; }
    Pl011Uart& Uart() { return uart_; }
    const GuestRam& Ram() const { return ram_; }
    const MmioBus& Bus() const { return mmio_bus_; }

private:
    void EmitBootloaderStub(uint64_t dtb_addr,
                            uint64_t msg_addr,
                            const std::string& banner_msg);

    GuestRam ram_;
    MmioBus mmio_bus_;
    Pl011Uart uart_;
    AArch64CpuCore cpu_;

    uint64_t dtb_guest_addr_ = 0;
    uint64_t kernel_guest_addr_ = 0;
    uint64_t initrd_guest_addr_ = 0;
    uint64_t initrd_size_ = 0;
    std::atomic<bool> stop_requested_{false};
};

} // namespace haseeb::vm
