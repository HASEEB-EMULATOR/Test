#pragma once

#include "haseeb/boot_image.hpp"
#include "haseeb/fdt_builder.hpp"
#include "haseeb/guest_ram.hpp"
#include "haseeb/pl011_uart.hpp"
#include "haseeb/vm_types.hpp"

#include <atomic>
#include <cstdint>
#include <string>

namespace haseeb::vm {

// M1 architectural vCPU state handed off from 2nd-stage bootloader to kernel (expanded in M2)
struct M1BootCpuState {
    uint64_t x[31]{};
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint32_t nzcv = 0;
    bool halted = false;
    uint64_t instructions_executed = 0;
};

// Virtual board skeleton: owns guest RAM, MMIO bus, PL011 UART, FDT, boot image staging,
// and the M1 2nd-stage AArch64 bootloader bring-up runner (boots to UART output).
class VirtualMachineSkeleton {
public:
    explicit VirtualMachineSkeleton(Pl011Uart::TxLineCallback uart_cb = nullptr);
    ~VirtualMachineSkeleton();

    VirtualMachineSkeleton(const VirtualMachineSkeleton&) = delete;
    VirtualMachineSkeleton& operator=(const VirtualMachineSkeleton&) = delete;

    VmError InitializeAndStage(const MachineBootConfig& config,
                               const ParsedBootImage* boot_img,
                               std::string& out_error);

    // Executes the staged AArch64 2nd-stage bootloader stub to UART output
    VmError RunBootStubUntilHalt(uint64_t max_steps = 100000ULL);
    void Shutdown();

    bool IsStaged() const { return ram_.IsAllocated(); }
    uint64_t DtbGuestAddr() const { return dtb_guest_addr_; }
    uint64_t KernelGuestAddr() const { return kernel_guest_addr_; }
    uint64_t InitrdGuestAddr() const { return initrd_guest_addr_; }
    uint64_t InitrdSizeBytes() const { return initrd_size_; }

    const M1BootCpuState& CpuState() const { return cpu_; }
    const Pl011Uart& Uart() const { return uart_; }
    Pl011Uart& Uart() { return uart_; }
    const GuestRam& Ram() const { return ram_; }
    const MmioBus& Bus() const { return mmio_bus_; }

private:
    bool LoadPhysU8(uint64_t addr, uint8_t& out) const;
    bool LoadPhysU32(uint64_t addr, uint32_t& out) const;
    bool LoadPhysU64(uint64_t addr, uint64_t& out) const;
    bool StorePhysU8(uint64_t addr, uint8_t val);
    bool StorePhysU32(uint64_t addr, uint32_t val);
    bool StorePhysU64(uint64_t addr, uint64_t val);

    void EmitBootloaderStub(uint64_t dtb_addr,
                            uint64_t msg_addr,
                            const std::string& banner_msg);

    GuestRam ram_;
    MmioBus mmio_bus_;
    Pl011Uart uart_;
    M1BootCpuState cpu_{};

    uint64_t dtb_guest_addr_ = 0;
    uint64_t kernel_guest_addr_ = 0;
    uint64_t initrd_guest_addr_ = 0;
    uint64_t initrd_size_ = 0;
    std::atomic<bool> stop_requested_{false};
};

} // namespace haseeb::vm
