#include "haseeb/boot_image.hpp"
#include "haseeb/compression.hpp"
#include "haseeb/cpio_reader.hpp"
#include "haseeb/kernel_boot.hpp"
#include "haseeb/machine.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace haseeb::vm;

namespace {

std::vector<uint8_t> GzipVec(const std::vector<uint8_t>& v) {
    return CompressionCodec::CompressGzip(v.data(), v.size());
}

std::vector<uint8_t> Lz4FVec(const std::vector<uint8_t>& v) {
    return CompressionCodec::CompressLz4Frame(v.data(), v.size());
}

std::vector<uint8_t> Lz4LVec(const std::vector<uint8_t>& v) {
    return CompressionCodec::CompressLz4Legacy(v.data(), v.size());
}

CpioEntry MakeCpio(const std::string& name, uint32_t mode, std::vector<uint8_t> data = {}) {
    CpioEntry e;
    e.name = name;
    e.mode = mode;
    e.uid = 0;
    e.gid = 0;
    e.nlink = 1;
    e.mtime = 0;
    e.data = std::move(data);
    return e;
}

// --- Minimal AArch64 Assembler Helpers ---
uint32_t Movz(uint8_t xd, uint16_t imm16, uint8_t hw = 0) {
    return 0xD2800000u | (static_cast<uint32_t>(hw & 3u) << 21) |
           (static_cast<uint32_t>(imm16) << 5) | (xd & 31u);
}

uint32_t Movk(uint8_t xd, uint16_t imm16, uint8_t hw) {
    return 0xF2800000u | (static_cast<uint32_t>(hw & 3u) << 21) |
           (static_cast<uint32_t>(imm16) << 5) | (xd & 31u);
}

void EmitImm64(std::vector<uint32_t>& code, uint8_t xd, uint64_t val) {
    code.push_back(Movz(xd, static_cast<uint16_t>(val & 0xFFFFu), 0));
    code.push_back(Movk(xd, static_cast<uint16_t>((val >> 16) & 0xFFFFu), 1));
    code.push_back(Movk(xd, static_cast<uint16_t>((val >> 32) & 0xFFFFu), 2));
    code.push_back(Movk(xd, static_cast<uint16_t>((val >> 48) & 0xFFFFu), 3));
}

uint32_t Adr(uint8_t xd, int32_t byte_offset) {
    uint32_t u21 = static_cast<uint32_t>(byte_offset) & 0x1FFFFFu;
    uint32_t immlo = u21 & 3u;
    uint32_t immhi = (u21 >> 2) & 0x7FFFFu;
    return 0x10000000u | (immlo << 29) | (immhi << 5) | (xd & 31u);
}

uint32_t AddImm64(uint8_t xd, uint8_t xn, uint16_t imm12) {
    return 0x91000000u | (static_cast<uint32_t>(imm12 & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5) | (xd & 31u);
}

uint32_t SubImm64(uint8_t xd, uint8_t xn, uint16_t imm12) {
    return 0xD1000000u | (static_cast<uint32_t>(imm12 & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5) | (xd & 31u);
}

uint32_t CmpImm64(uint8_t xn, uint16_t imm12) {
    return 0xF100001Fu | (static_cast<uint32_t>(imm12 & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5);
}

uint32_t LdrW(uint8_t wt, uint8_t xn, uint16_t imm12_bytes = 0) {
    return 0xB9400000u | (static_cast<uint32_t>((imm12_bytes / 4u) & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5) | (wt & 31u);
}

uint32_t Ldrb(uint8_t wt, uint8_t xn, uint16_t imm12_bytes = 0) {
    return 0x39400000u | (static_cast<uint32_t>(imm12_bytes & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5) | (wt & 31u);
}

uint32_t Strb(uint8_t wt, uint8_t xn, uint16_t imm12_bytes = 0) {
    return 0x39000000u | (static_cast<uint32_t>(imm12_bytes & 0xFFFu) << 10) |
           (static_cast<uint32_t>(xn & 31u) << 5) | (wt & 31u);
}

uint32_t Svc(uint16_t imm = 0) {
    return 0xD4000001u | (static_cast<uint32_t>(imm) << 5);
}

uint32_t Hvc(uint16_t imm = 0) {
    return 0xD4000002u | (static_cast<uint32_t>(imm) << 5);
}

uint32_t B(int32_t rel_insns) {
    return 0x14000000u | (static_cast<uint32_t>(rel_insns) & 0x03FFFFFFu);
}

uint32_t BCond(uint8_t cond, int32_t rel_insns) {
    return 0x54000000u | ((static_cast<uint32_t>(rel_insns) & 0x7FFFFu) << 5) | (cond & 0xFu);
}

uint32_t Cbz64(uint8_t xt, int32_t rel_insns) {
    return 0xB4000000u | ((static_cast<uint32_t>(rel_insns) & 0x7FFFFu) << 5) | (xt & 31u);
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

// Wraps raw AArch64 machine instructions (+ appended rodata) into a minimal valid ET_EXEC ELF64
std::vector<uint8_t> BuildMinimalAArch64Elf64(const std::vector<uint8_t>& code_and_data,
                                              uint64_t base_va = 0x0000000000400000ULL) {
    constexpr uint64_t kCodeOffset = 0x100ULL;
    const uint64_t file_size = kCodeOffset + code_and_data.size();
    std::vector<uint8_t> elf(file_size, 0);

    elf[0] = 0x7Fu; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2u;    // ELFCLASS64
    elf[5] = 1u;    // ELFDATA2LSB
    elf[6] = 1u;    // EV_CURRENT
    auto write16 = [&](size_t off, uint16_t v) {
        elf[off] = static_cast<uint8_t>(v & 0xFFu);
        elf[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto write32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) elf[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    };
    auto write64 = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) elf[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    };

    write16(16, 2u);                    // e_type = ET_EXEC
    write16(18, 183u);                  // e_machine = EM_AARCH64
    write32(20, 1u);                    // e_version
    write64(24, base_va + kCodeOffset); // e_entry
    write64(32, 64ULL);                 // e_phoff
    write16(52, 64u);                   // e_ehsize
    write16(54, 56u);                   // e_phentsize
    write16(56, 1u);                    // e_phnum

    // Single RWX PT_LOAD mapping entire binary
    write32(64 + 0, 1u);                   // PT_LOAD
    write32(64 + 4, 7u);                   // PF_R | PF_W | PF_X
    write64(64 + 8, 0ULL);                 // p_offset
    write64(64 + 16, base_va);             // p_vaddr
    write64(64 + 24, base_va);             // p_paddr
    write64(64 + 32, file_size);           // p_filesz
    write64(64 + 40, file_size + 4096ULL); // p_memsz
    write64(64 + 48, 4096ULL);             // p_align

    std::memcpy(elf.data() + kCodeOffset, code_and_data.data(), code_and_data.size());
    return elf;
}

// Wraps kernel + ramdisk into an Android boot.img v2
std::vector<uint8_t> BuildBootImgV2(const std::vector<uint8_t>& kernel,
                                    const std::vector<uint8_t>& ramdisk,
                                    const std::string& cmdline) {
    constexpr uint32_t kPage = 4096u;
    auto align_up = [&](size_t v) { return (v + kPage - 1u) & ~static_cast<size_t>(kPage - 1u); };
    const size_t k_off = kPage;
    const size_t r_off = k_off + align_up(kernel.size());
    const size_t total = r_off + align_up(ramdisk.size());

    std::vector<uint8_t> img(total, 0u);
    auto put32 = [&](size_t off, uint32_t v) {
        img[off] = v & 0xFFu;
        img[off + 1] = (v >> 8) & 0xFFu;
        img[off + 2] = (v >> 16) & 0xFFu;
        img[off + 3] = (v >> 24) & 0xFFu;
    };
    std::memcpy(img.data(), "ANDROID!", 8);
    put32(8, static_cast<uint32_t>(kernel.size()));
    put32(16, static_cast<uint32_t>(ramdisk.size()));
    put32(36, kPage);
    put32(40, 2u); // header_version = 2
    size_t c_len = std::min<size_t>(cmdline.size(), 511u);
    std::memcpy(img.data() + 64, cmdline.data(), c_len);
    if (!kernel.empty()) std::memcpy(img.data() + k_off, kernel.data(), kernel.size());
    if (!ramdisk.empty()) std::memcpy(img.data() + r_off, ramdisk.data(), ramdisk.size());
    return img;
}

struct CodeBlobBuilder {
    std::vector<uint32_t> insns;
    std::vector<uint8_t> data;
    struct Patch {
        size_t insn_idx;
        uint8_t rd;
        size_t data_offset;
    };
    std::vector<Patch> patches;

    size_t AddString(const std::string& s) {
        size_t off = data.size();
        data.insert(data.end(), s.begin(), s.end());
        data.push_back('\0');
        return off;
    }

    size_t EmitAdrPlaceholder(uint8_t rd, size_t data_offset) {
        size_t idx = insns.size();
        insns.push_back(0);
        patches.push_back({idx, rd, data_offset});
        return idx;
    }

    std::vector<uint8_t> Finalize() {
        const size_t code_bytes = insns.size() * 4u;
        for (const auto& p : patches) {
            int32_t insn_byte_off = static_cast<int32_t>(p.insn_idx * 4u);
            int32_t target_byte_off = static_cast<int32_t>(code_bytes + p.data_offset);
            insns[p.insn_idx] = Adr(p.rd, target_byte_off - insn_byte_off);
        }
        std::vector<uint8_t> blob;
        blob.reserve(code_bytes + data.size());
        for (uint32_t i : insns) AppendLe32(blob, i);
        blob.insert(blob.end(), data.begin(), data.end());
        return blob;
    }
};

// Test 1: Gzip (stored & DEFLATE), LZ4 Frame & LZ4 Legacy roundtrips + BootImageParser inspection
void TestCompressionAndBootInspection() {
    std::vector<uint8_t> sample;
    for (int i = 0; i < 64; ++i) {
        const char line[] = "haseeb-vm-deflate-lz4-test-pattern-0123456789\n";
        sample.insert(sample.end(), line, line + sizeof(line) - 1);
    }

    std::string err;
    std::vector<uint8_t> gz = GzipVec(sample);
    std::vector<uint8_t> gz_out;
    ArchiveFormat fmt = ArchiveFormat::Unknown;
    assert(CompressionCodec::DecompressAuto(gz, gz_out, fmt, err));
    assert(fmt == ArchiveFormat::Gzip);
    assert(gz_out == sample);

    // Corrupt CRC32 must fail cleanly
    std::vector<uint8_t> bad_gz = gz;
    bad_gz[bad_gz.size() - 6] ^= 0xFFu;
    std::vector<uint8_t> bad_out;
    assert(!CompressionCodec::DecompressAuto(bad_gz, bad_out, fmt, err));

    // LZ4 Frame roundtrip
    std::vector<uint8_t> lz4f = Lz4FVec(sample);
    std::vector<uint8_t> lz4f_out;
    assert(CompressionCodec::DecompressAuto(lz4f, lz4f_out, fmt, err));
    assert(fmt == ArchiveFormat::Lz4Frame);
    assert(lz4f_out == sample);

    // LZ4 Legacy roundtrip
    std::vector<uint8_t> lz4l = Lz4LVec(sample);
    std::vector<uint8_t> lz4l_out;
    assert(CompressionCodec::DecompressAuto(lz4l, lz4l_out, fmt, err));
    assert(fmt == ArchiveFormat::Lz4Legacy);
    assert(lz4l_out == sample);

    // Inspect compressed ARM64 kernel + compressed ramdisk inside boot.img
    std::vector<uint8_t> fake_arm64(512, 0u);
    fake_arm64[8] = 0x00u; fake_arm64[9] = 0x00u; fake_arm64[10] = 0x08u; // text_offset=0x80000
    fake_arm64[56] = 'A'; fake_arm64[57] = 'R'; fake_arm64[58] = 'M'; fake_arm64[59] = 0x64u;
    std::vector<uint8_t> k_gz = GzipVec(fake_arm64);

    CpioEntry e1 = MakeCpio("magiskinit", 0100755u, {0x7Fu, 'E', 'L', 'F'});
    CpioEntry e2 = MakeCpio(".backup/.magisk", 0100644u,
                            {'K', 'E', 'E', 'P', 'V', 'E', 'R', 'I', 'T', 'Y', '=', '1'});
    std::vector<uint8_t> rd_lz4 = Lz4FVec(CpioArchive::Build({e1, e2}));

    std::vector<uint8_t> boot = BuildBootImgV2(k_gz, rd_lz4, "androidboot.selinux=permissive");
    ParsedBootImage parsed;
    assert(BootImageParser::ParseBootBytes(boot, parsed, err) == VmError::Ok);
    assert(parsed.kernel_info.compression == KernelCompression::Gzip);
    assert(parsed.magisk_detected);

    assert(BootImageParser::DecompressKernelAndInspect(parsed, err) == VmError::Ok);
    assert(parsed.kernel_info.valid_arm64_magic);
    assert(parsed.kernel_info.text_offset == 0x80000ULL);
    std::cout << "[PASS] Test 1: Gzip/LZ4 codecs + compressed kernel & Magisk ramdisk inspection\n";
}

// Test 2: SVR4 newc CPIO Archive parsing including concatenated archives
void TestCpioArchive() {
    std::vector<CpioEntry> base = {
        MakeCpio("sbin", 0040755u),
        MakeCpio("init", 0100755u, {'I', 'N', 'I', 'T'}),
        MakeCpio("sbin/su", 0120777u, {'/', 'm', 'a', 'g', 'i', 's', 'k', 'i', 'n', 'i', 't'}),
    };
    std::vector<CpioEntry> overlay = {
        MakeCpio(".backup/.magisk", 0100644u, {'M', 'A', 'G', 'I', 'S', 'K'}),
    };
    std::vector<uint8_t> combined = CpioArchive::Build(base);
    std::vector<uint8_t> part2 = CpioArchive::Build(overlay);
    combined.insert(combined.end(), part2.begin(), part2.end());

    assert(CpioArchive::LooksLikeNewc(combined));
    std::vector<CpioEntry> entries;
    std::string err;
    assert(CpioArchive::Parse(combined, entries, err));
    const CpioEntry* init_e = CpioArchive::FindEntry(entries, "/init");
    const CpioEntry* su_e = CpioArchive::FindEntry(entries, "sbin/su");
    const CpioEntry* bk_e = CpioArchive::FindEntry(entries, ".backup/.magisk");
    assert(init_e && init_e->data.size() == 4);
    assert(su_e && su_e->IsSymlink());
    assert(bk_e && std::string(bk_e->data.begin(), bk_e->data.end()) == "MAGISK");
    std::cout << "[PASS] Test 2: SVR4 newc CPIO parser & concatenated ramdisk overlays\n";
}

// Test 3: Direct ARM64 Image.gz Kernel Boot (DTB pointer check + UART + PSCI SYSTEM_OFF)
void TestCompressedArm64KernelDirectBoot() {
    CodeBlobBuilder cb;
    const size_t ok_off = cb.AddString("[KERNEL] booted via Image.gz\n");
    const size_t fail_off = cb.AddString("[KERNEL] DTB bad!\n");

    cb.insns.push_back(LdrW(1, 0, 0));       // ldr w1, [x0]
    cb.insns.push_back(Movz(2, 0x0DD0u, 0));
    cb.insns.push_back(Movk(2, 0xEDFEu, 1)); // 0xEDFE0DD0 (big-endian FDT magic)
    cb.insns.push_back(0x6B02003Fu);         // cmp w1, w2
    const size_t bne_idx = cb.insns.size();
    cb.insns.push_back(0);
    cb.EmitAdrPlaceholder(11, ok_off);
    const size_t b_print_idx = cb.insns.size();
    cb.insns.push_back(0);
    const size_t fail_idx = cb.insns.size();
    cb.EmitAdrPlaceholder(11, fail_off);

    const size_t print_idx = cb.insns.size();
    EmitImm64(cb.insns, 10, kPl011UartBase);
    cb.insns.push_back(0x3840156Cu); // ldrb w12, [x11], #1
    cb.insns.push_back(Cbz64(12, 3));
    cb.insns.push_back(Strb(12, 10, 0));
    cb.insns.push_back(B(-3));
    cb.insns.push_back(Movz(0, 0x0008u, 0));
    cb.insns.push_back(Movk(0, 0x8400u, 1)); // PSCI_SYSTEM_OFF
    cb.insns.push_back(Hvc(0));

    cb.insns[bne_idx] = BCond(1u, static_cast<int32_t>(fail_idx - bne_idx)); // NE
    cb.insns[b_print_idx] = B(static_cast<int32_t>(print_idx - b_print_idx));

    std::vector<uint8_t> code_blob = cb.Finalize();
    std::vector<uint8_t> arm64_image(64u + code_blob.size(), 0u);
    uint32_t hdr_branch = B(16); // B +64 past header
    std::memcpy(arm64_image.data(), &hdr_branch, 4);
    arm64_image[8] = 0x00u; arm64_image[9] = 0x00u; arm64_image[10] = 0x08u; // text_offset=0x80000
    arm64_image[56] = 'A'; arm64_image[57] = 'R'; arm64_image[58] = 'M'; arm64_image[59] = 0x64u;
    std::memcpy(arm64_image.data() + 64u, code_blob.data(), code_blob.size());

    std::vector<uint8_t> gz_kernel = GzipVec(arm64_image);
    std::vector<uint8_t> boot_bytes = BuildBootImgV2(gz_kernel, {}, "loglevel=7");

    ParsedBootImage parsed;
    std::string err;
    assert(BootImageParser::ParseBootBytes(boot_bytes, parsed, err) == VmError::Ok);

    std::vector<std::string> uart_lines;
    VirtualMachineSkeleton vm([&](const std::string& l) { uart_lines.push_back(l); });

    MachineBootConfig cfg;
    cfg.ram_mb = 64u;
    assert(vm.InitializeAndStage(cfg, &parsed, err) == VmError::Ok);
    std::string booted_target;
    assert(vm.BootGuest(false, booted_target, err) == VmError::Ok);
    assert(booted_target == "arm64-kernel");

    assert(vm.StepGuest(50000ULL) == VmError::Ok);
    assert(vm.CpuState().halted);
    const std::string uart = vm.Uart().CapturedOutput();
    assert(uart.find("[KERNEL] booted via Image.gz") != std::string::npos);
    std::cout << "[PASS] Test 3: Direct Image.gz ARM64 kernel boot + DTB check + PSCI poweroff\n";
}

// Test 4: Real EL0 /magiskinit -> execve("/system/bin/sh") -> Interactive Shell Prompt
void TestEl0MagiskInitToShellPrompt() {
    std::vector<uint8_t> magiskinit_elf;
    {
        CodeBlobBuilder cb;
        const size_t path_bk = cb.AddString("/.backup/.magisk");
        const size_t msg_ok = cb.AddString("[magiskinit] su root active\n");
        const size_t sh_path = cb.AddString("/system/bin/sh");

        cb.insns.push_back(Movz(0, 0, 0));
        cb.EmitAdrPlaceholder(1, path_bk);
        cb.insns.push_back(Movz(2, 0, 0));
        cb.insns.push_back(Movz(3, 0, 0));
        cb.insns.push_back(Movz(8, 56u, 0)); // __NR_openat
        cb.insns.push_back(Svc(0));

        cb.insns.push_back(Movz(8, 57u, 0)); // __NR_close
        cb.insns.push_back(Svc(0));

        cb.insns.push_back(Movz(0, 1u, 0));
        cb.EmitAdrPlaceholder(1, msg_ok);
        cb.insns.push_back(Movz(2, 28u, 0));
        cb.insns.push_back(Movz(8, 64u, 0)); // __NR_write
        cb.insns.push_back(Svc(0));

        cb.EmitAdrPlaceholder(0, sh_path);
        cb.insns.push_back(Movz(1, 0, 0));
        cb.insns.push_back(Movz(2, 0, 0));
        cb.insns.push_back(Movz(8, 221u, 0)); // __NR_execve
        cb.insns.push_back(Svc(0));

        cb.insns.push_back(Movz(0, 42u, 0));
        cb.insns.push_back(Movz(8, 94u, 0)); // __NR_exit_group fallback
        cb.insns.push_back(Svc(0));

        magiskinit_elf = BuildMinimalAArch64Elf64(cb.Finalize());
    }

    std::vector<uint8_t> sh_elf;
    {
        CodeBlobBuilder cb;
        const size_t prompt_off = cb.AddString("haseeb:/ # ");
        const size_t uid_off = cb.AddString("uid=0(root)\n");
        const size_t bye_off = cb.AddString("bye\n");
        const size_t nl_off = cb.AddString("\n");

        cb.insns.push_back(SubImm64(31, 31, 512u)); // sub sp, sp, #512

        cb.insns.push_back(AddImm64(0, 31, 0));
        cb.insns.push_back(Movz(8, 160u, 0)); // __NR_uname
        cb.insns.push_back(Svc(0));

        cb.insns.push_back(Movz(0, 1u, 0));
        cb.EmitAdrPlaceholder(1, prompt_off);
        cb.insns.push_back(Movz(2, 11u, 0));
        cb.insns.push_back(Movz(8, 64u, 0));
        cb.insns.push_back(Svc(0));

        // loop_read: read(0, sp + 400, 64)
        const size_t loop_idx = cb.insns.size();
        cb.insns.push_back(Movz(0, 0u, 0));
        cb.insns.push_back(AddImm64(1, 31, 400u));
        cb.insns.push_back(Movz(2, 64u, 0));
        cb.insns.push_back(Movz(8, 63u, 0)); // __NR_read
        cb.insns.push_back(Svc(0));

        cb.insns.push_back(Ldrb(10, 31, 400u));

        // 'u' -> write(1, uts.release at sp+130, 15) + "\n"
        cb.insns.push_back(CmpImm64(10, static_cast<uint16_t>('u')));
        const size_t b_not_u = cb.insns.size();
        cb.insns.push_back(0);
        cb.insns.push_back(Movz(0, 1u, 0));
        cb.insns.push_back(AddImm64(1, 31, 130u));
        cb.insns.push_back(Movz(2, 15u, 0));
        cb.insns.push_back(Movz(8, 64u, 0));
        cb.insns.push_back(Svc(0));
        cb.insns.push_back(Movz(0, 1u, 0));
        cb.EmitAdrPlaceholder(1, nl_off);
        cb.insns.push_back(Movz(2, 1u, 0));
        cb.insns.push_back(Movz(8, 64u, 0));
        cb.insns.push_back(Svc(0));
        const size_t b_after_u = cb.insns.size();
        cb.insns.push_back(0);

        // 'i' -> write(1, "uid=0(root)\n", 12)
        const size_t check_i_idx = cb.insns.size();
        cb.insns.push_back(CmpImm64(10, static_cast<uint16_t>('i')));
        const size_t b_not_i = cb.insns.size();
        cb.insns.push_back(0);
        cb.insns.push_back(Movz(0, 1u, 0));
        cb.EmitAdrPlaceholder(1, uid_off);
        cb.insns.push_back(Movz(2, 12u, 0));
        cb.insns.push_back(Movz(8, 64u, 0));
        cb.insns.push_back(Svc(0));
        const size_t b_after_i = cb.insns.size();
        cb.insns.push_back(0);

        // 'e' -> write(1, "bye\n", 4); exit_group(0)
        const size_t check_e_idx = cb.insns.size();
        cb.insns.push_back(CmpImm64(10, static_cast<uint16_t>('e')));
        const size_t b_not_e = cb.insns.size();
        cb.insns.push_back(0);
        cb.insns.push_back(Movz(0, 1u, 0));
        cb.EmitAdrPlaceholder(1, bye_off);
        cb.insns.push_back(Movz(2, 4u, 0));
        cb.insns.push_back(Movz(8, 64u, 0));
        cb.insns.push_back(Svc(0));
        cb.insns.push_back(Movz(0, 0u, 0));
        cb.insns.push_back(Movz(8, 94u, 0));
        cb.insns.push_back(Svc(0));

        const size_t next_idx = cb.insns.size();
        cb.insns.push_back(B(static_cast<int32_t>(loop_idx - next_idx)));

        cb.insns[b_not_u] = BCond(1u, static_cast<int32_t>(check_i_idx - b_not_u));
        cb.insns[b_after_u] = B(static_cast<int32_t>(loop_idx - b_after_u));
        cb.insns[b_not_i] = BCond(1u, static_cast<int32_t>(check_e_idx - b_not_i));
        cb.insns[b_after_i] = B(static_cast<int32_t>(loop_idx - b_after_i));
        cb.insns[b_not_e] = BCond(1u, static_cast<int32_t>(next_idx - b_not_e));

        sh_elf = BuildMinimalAArch64Elf64(cb.Finalize());
    }

    std::vector<uint8_t> cpio_magisk = CpioArchive::Build({
        MakeCpio(".backup", 0040755u),
        MakeCpio(".backup/.magisk", 0100644u,
                 {'K', 'E', 'E', 'P', 'V', 'E', 'R', 'I', 'T', 'Y', '=', '1'}),
        MakeCpio("magiskinit", 0100755u, magiskinit_elf),
    });
    std::vector<uint8_t> cpio_sh = CpioArchive::Build({
        MakeCpio("system", 0040755u),
        MakeCpio("system/bin", 0040755u),
        MakeCpio("system/bin/sh", 0100755u, sh_elf),
    });
    std::vector<uint8_t> rd_combined = Lz4FVec(cpio_magisk);
    std::vector<uint8_t> rd_part2 = GzipVec(cpio_sh);
    rd_combined.insert(rd_combined.end(), rd_part2.begin(), rd_part2.end());

    std::vector<uint8_t> boot_bytes = BuildBootImgV2({}, rd_combined, "androidboot.selinux=permissive");
    ParsedBootImage parsed;
    std::string err;
    assert(BootImageParser::ParseBootBytes(boot_bytes, parsed, err) == VmError::Ok);
    assert(parsed.magisk_detected);

    VirtualMachineSkeleton vm;
    MachineBootConfig cfg;
    cfg.ram_mb = 64u;
    assert(vm.InitializeAndStage(cfg, &parsed, err) == VmError::Ok);

    std::string target;
    assert(vm.BootGuest(true, target, err) == VmError::Ok);
    assert(target == "/magiskinit");

    assert(vm.StepGuest(50000ULL) == VmError::Ok);
    assert(vm.Supervisor()->ActiveBinary() == "/system/bin/sh");
    assert(vm.CpuState().stop_reason == CpuStopReason::WfiWait);
    assert(vm.CpuState().waiting_for_interrupt);

    std::string out1 = vm.Uart().CapturedOutput();
    assert(out1.find("[magiskinit] su root active") != std::string::npos);
    assert(out1.find("haseeb:/ # ") != std::string::npos);

    vm.Uart().PushRxString("uname\nid\nexit\n");
    assert(vm.StepGuest(50000ULL) == VmError::Ok);
    assert(vm.CpuState().halted);
    assert(vm.Supervisor()->Exited());
    assert(vm.Supervisor()->ExitStatus() == 0);

    std::string out2 = vm.Uart().CapturedOutput();
    assert(out2.find("5.10.209-haseeb") != std::string::npos);
    assert(out2.find("uid=0(root)") != std::string::npos);
    assert(out2.find("bye") != std::string::npos);
    std::cout << "[PASS] Test 4: EL0 /magiskinit -> execve(/system/bin/sh) -> interactive UART shell\n";
}

} // namespace

int main() {
    TestCompressionAndBootInspection();
    TestCpioArchive();
    TestCompressedArm64KernelDirectBoot();
    TestEl0MagiskInitToShellPrompt();
    std::cout << "ALL M4 KERNEL & INIT BOOT TESTS PASSED.\n";
    return 0;
}
