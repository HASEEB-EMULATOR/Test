#include "haseeb/pl011_uart.hpp"

#include <utility>

namespace haseeb::vm {

Pl011Uart::Pl011Uart(TxLineCallback on_line) : on_line_(std::move(on_line)) {}

void Pl011Uart::SetLineCallback(TxLineCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_line_ = std::move(cb);
}

void Pl011Uart::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    line_buf_.clear();
    total_output_.clear();
    rx_fifo_.clear();
    rsr_ = 0;
    ibrd_ = 1;
    fbrd_ = 0;
    lcr_h_ = 0x70;
    cr_ = 0x0301;
    ifls_ = 0x12;
    imsc_ = 0;
    ris_ = 0;
}

void Pl011Uart::PushRxString(const std::string& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (char c : input) {
        rx_fifo_.push_back(static_cast<uint8_t>(c));
    }
    if (!rx_fifo_.empty()) {
        ris_ |= (1u << 4); // RXRIS
    }
}

void Pl011Uart::FlushLine() {
    TxLineCallback cb;
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (line_buf_.empty()) return;
        pending.swap(line_buf_);
        cb = on_line_;
    }
    if (cb && !pending.empty()) {
        cb(pending);
    }
}

std::string Pl011Uart::CapturedOutput() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_output_;
}

uint64_t Pl011Uart::Read(uint64_t offset, unsigned /*size_bytes*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
        case 0x000: { // UARTDR
            if (rx_fifo_.empty()) return 0;
            uint8_t b = rx_fifo_.front();
            rx_fifo_.pop_front();
            if (rx_fifo_.empty()) ris_ &= ~(1u << 4);
            return b;
        }
        case 0x004: return rsr_;
        case 0x018: { // UARTFR: TXFE (bit 7), RXFE (bit 4)
            uint32_t fr = (1u << 7); // TX FIFO always empty/ready
            if (rx_fifo_.empty()) fr |= (1u << 4);
            return fr;
        }
        case 0x024: return ibrd_;
        case 0x028: return fbrd_;
        case 0x02C: return lcr_h_;
        case 0x030: return cr_;
        case 0x034: return ifls_;
        case 0x038: return imsc_;
        case 0x03C: return ris_;
        case 0x040: return ris_ & imsc_;
        // AMBA PrimeCell PL011 Peripheral & Cell IDs
        case 0xFE0: return 0x11u;
        case 0xFE4: return 0x10u;
        case 0xFE8: return 0x14u;
        case 0xFEC: return 0x00u;
        case 0xFF0: return 0x0Du;
        case 0xFF4: return 0xF0u;
        case 0xFF8: return 0x05u;
        case 0xFFC: return 0xB1u;
        default:    return 0u;
    }
}

void Pl011Uart::Write(uint64_t offset, unsigned /*size_bytes*/, uint64_t value) {
    TxLineCallback cb;
    std::string completed_line;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        switch (offset) {
            case 0x000: { // UARTDR TX byte
                char ch = static_cast<char>(value & 0xFFu);
                total_output_.push_back(ch);
                if (ch == '\r') {
                    // normalize CRLF cleanly
                } else if (ch == '\n') {
                    completed_line.swap(line_buf_);
                    cb = on_line_;
                } else {
                    line_buf_.push_back(ch);
                }
                ris_ |= (1u << 5); // TXRIS
                break;
            }
            case 0x004: rsr_ = 0; break;
            case 0x024: ibrd_ = static_cast<uint32_t>(value); break;
            case 0x028: fbrd_ = static_cast<uint32_t>(value); break;
            case 0x02C: lcr_h_ = static_cast<uint32_t>(value); break;
            case 0x030: cr_ = static_cast<uint32_t>(value); break;
            case 0x034: ifls_ = static_cast<uint32_t>(value); break;
            case 0x038: imsc_ = static_cast<uint32_t>(value); break;
            case 0x044: ris_ &= ~static_cast<uint32_t>(value); break;
            default: break;
        }
    }

    if (cb && !completed_line.empty()) {
        cb(completed_line);
    }
}

} // namespace haseeb::vm
