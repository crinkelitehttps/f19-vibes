#include "core/pcspeaker.h"

#include <algorithm>
#include <cmath>

namespace f19 {

namespace {
constexpr double kSubLen = PcSpeaker::kPitHz / (PcSpeaker::kRate * 4.0);  // PIT ticks per sub-sample
// Modes 2 and 3 repeat; 6 and 7 are aliases of them.
bool periodic(int mode) { return mode == 2 || mode == 3; }
}  // namespace

void PcSpeaker::Biquad::lowpass(double fc, double fs, double q) {
    double w = 2 * M_PI * fc / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
    a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
}

void PcSpeaker::Biquad::highpass(double fc, double fs, double q) {
    double w = 2 * M_PI * fc / fs, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
    a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
}

void PcSpeaker::setup_filters() {
    const double fs = kRate * kOver;
    // 4th-order Butterworth anti-aliasing before decimation.
    aa_[0].lowpass(0.4 * kRate, fs, 0.5412);
    aa_[1].lowpass(0.4 * kRate, fs, 1.3066);
    // A small cone in a metal case: little below a few hundred Hz, falling
    // off in the treble. An approximation; real speakers varied a lot.
    cone_lp_.lowpass(7000, fs, 0.707);
    cone_hp_.highpass(150, kRate, 0.707);
    dc_.highpass(20, kRate, 0.707);
    filters_ready_ = true;
}

double PcSpeaker::high_time() const {
    // Mode 3: high for the first half (the larger one when odd); mode 2:
    // low for one tick at the end of each period.
    if (mode_ == 3) return std::ceil(count_ / 2);
    return count_ - 1;
}

bool PcSpeaker::out2(double t) const {
    if (periodic(mode_)) {
        if (!gate_ || !running_) return true;
        double x = std::fmod(t - t0_, count_);
        return x < high_time();
    }
    if (mode_ == 0) return running_ && t >= t0_ + count_;   // low until terminal count
    return true;
}

double PcSpeaker::out2_high_until(double t) const {
    double x = t - t0_, full = std::floor(x / count_);
    return full * high_time() + std::min(x - full * count_, high_time());
}

double PcSpeaker::level_integral(double a, double b) const {
    if (!data_ || b <= a) return 0;
    if (periodic(mode_)) {
        if (!gate_ || !running_) return b - a;
        return out2_high_until(b) - out2_high_until(a);
    }
    if (mode_ == 0) {
        if (!running_) return 0;
        return std::max(0.0, b - std::max(a, t0_ + count_));
    }
    return b - a;
}

void PcSpeaker::emit(double x) {
    if (!filters_ready_) setup_filters();
    double y = aa_[1].run(aa_[0].run(x));
    if (speaker_filter) y = cone_lp_.run(y);
    if (sub_index_ % kOver != kOver - 1) return;
    y = speaker_filter ? cone_hp_.run(y) : dc_.run(y);
    samples.push_back(float(std::clamp(y * volume, -1.0, 1.0)));
}

void PcSpeaker::render_const(double t) {
    if (t <= cur_) return;
    if (!enabled) {
        cur_ = t;
        sub_index_ = uint64_t(t / kSubLen);
        acc_ = 0;
        return;
    }
    for (double end = double(sub_index_ + 1) * kSubLen; end <= t; end = double(sub_index_ + 1) * kSubLen) {
        acc_ += level_integral(cur_, end);
        emit(acc_ / kSubLen);
        acc_ = 0;
        cur_ = end;
        sub_index_++;
    }
    acc_ += level_integral(cur_, t);
    cur_ = t;
}

void PcSpeaker::advance(double t) {
    // A count written while the channel runs takes effect at the end of the
    // current half-cycle (mode 3) or period (mode 2).
    while (pending_ && running_ && gate_ && periodic(mode_)) {
        double x = cur_ - t0_, start = t0_ + std::floor(x / count_) * count_;
        bool to_low = mode_ == 3 && cur_ < start + high_time();
        double tb = to_low ? start + high_time() : start + count_;
        if (tb > t) break;
        render_const(tb);
        count_ = pending_count_;
        pending_ = false;
        t0_ = to_low ? tb - high_time() : tb;
    }
    render_const(t);
}

void PcSpeaker::port61_write(double t, uint8_t v) {
    advance(t);
    bool gate = v & 1;
    if (gate && !gate_) {             // rising gate restarts the count
        t0_ = t;
        if (pending_) { count_ = pending_count_; pending_ = false; }
    }
    gate_ = gate;
    data_ = v & 2;
}

uint8_t PcSpeaker::port61_read(double t) const {
    // Bit 4 toggles with DRAM refresh (every 15 us); some code times with it.
    uint8_t refresh = (uint64_t(t / 18) & 1) ? 0x10 : 0;
    return uint8_t((gate_ ? 1 : 0) | (data_ ? 2 : 0) | refresh | (out2(t) ? 0x20 : 0));
}

void PcSpeaker::pit_control(double t, uint8_t v) {
    int access = (v >> 4) & 3;
    if (access == 0) return;          // counter latch: reads are not modelled
    advance(t);
    mode_ = (v >> 1) & 7;
    if (mode_ > 5) mode_ -= 4;
    access_ = access;
    write_phase_ = 0;
    running_ = false;
    pending_ = false;
}

void PcSpeaker::pit_data(double t, uint8_t v) {
    unsigned value;
    if (access_ == 3) {
        if (write_phase_ == 0) {
            low_byte_ = v;
            write_phase_ = 1;
            return;
        }
        write_phase_ = 0;
        value = low_byte_ | (v << 8);
    } else {
        value = access_ == 1 ? v : unsigned(v) << 8;
    }
    double count = value ? value : 65536;
    advance(t);
    if (running_ && periodic(mode_) && gate_) {
        pending_ = true;
        pending_count_ = count;
        return;
    }
    count_ = count;
    t0_ = t;
    running_ = true;
}

}  // namespace f19
