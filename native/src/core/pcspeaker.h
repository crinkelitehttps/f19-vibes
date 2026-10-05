// PC speaker: PIT channel 2 and the speaker bits of port 61h, rendered to
// audio in emulated time.
//
// The speaker is driven by (port 61h bit 1) AND (PIT channel 2 output);
// bit 0 is channel 2's gate. ISOUND.EXE uses both ways of driving it: sound
// effects program channel 2 as a square wave (mode 3) and retune it every
// timer tick; the title music toggles bit 1 directly from a delay loop
// (gate low, so the channel output stays high). Every change is applied at
// its emulated time, in PIT clock ticks (1.193182 MHz), and the 1-bit signal
// is integrated exactly over each sub-sample (4x oversampled), low-passed
// and decimated.
#pragma once

#include <cstdint>
#include <vector>

namespace f19 {

class PcSpeaker {
public:
    static constexpr int kRate = 48000;
    static constexpr double kPitHz = 1193182.0;

    // Rendering is skipped (time only advances) while disabled.
    bool enabled = false;
    // Approximate response of a small PC speaker (bass and treble roll-off);
    // false leaves only the anti-aliasing and DC-blocking filters.
    bool speaker_filter = true;
    float volume = 0.5f;

    // All `t` arguments are emulated time in PIT ticks, non-decreasing.
    void port61_write(double t, uint8_t v);
    uint8_t port61_read(double t) const;     // bits 0-1 as written, 4 refresh toggle, 5 OUT2
    void pit_control(double t, uint8_t v);   // control word addressed to channel 2
    void pit_data(double t, uint8_t v);      // write to port 42h
    // Render everything up to time t into `samples` (mono float, kRate).
    void advance(double t);
    std::vector<float> samples;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void lowpass(double fc, double fs, double q);
        void highpass(double fc, double fs, double q);
        double run(double x) {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // Port 61h.
    bool gate_ = false, data_ = false;
    // Channel 2.
    int mode_ = 3, access_ = 3, write_phase_ = 0;
    uint8_t low_byte_ = 0;
    double count_ = 65536;     // current reload value
    double t0_ = 0;            // start of the current period
    bool running_ = false;     // a count has been loaded since the control word
    bool pending_ = false;     // count written while running: loads at the next half-cycle
    double pending_count_ = 0;

    // Rendering.
    static constexpr int kOver = 4;
    double cur_ = 0;           // rendered up to here
    uint64_t sub_index_ = 0;   // sub-sample being accumulated
    double acc_ = 0;           // time the output was high in it so far
    bool filters_ready_ = false;
    Biquad aa_[2], cone_lp_, cone_hp_, dc_;

    double high_time() const;               // length of the high part of a period
    bool out2(double t) const;
    double out2_high_until(double t) const; // integral of OUT2 from t0_ to t
    double level_integral(double a, double b) const;
    void render_const(double t);
    void emit(double x);
    void setup_filters();
};

}  // namespace f19
