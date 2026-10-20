#include "timeline.h"

namespace andromeda::editor::timeline {

void Timeline::rebuild(const std::vector<MetaEvent>& metas, uint16_t ppq) {
	ppq_ = ppq ? ppq : 960;
	tempos_.clear();
	sigs_.clear();

    for (const MetaEvent& m : metas) {
        if (m.event_type == MetaEventType::Tempo) {
            const float bpm = bytes_as_tempo(m.data);
            if (bpm > 0.0f) tempos_.push_back({ m.tick, bpm, 0.0 });
        }
        else if (m.event_type == MetaEventType::TimeSignature && m.data.size() >= 2) {
            const unsigned nn = m.data[0];
            const unsigned denom = 1u << std::min<unsigned>(m.data[1], 6);
            const MIDITick beat = ppq_ * 4u / denom;
            if (nn == 0 || beat == 0) continue;
            sigs_.push_back({ m.tick, (uint8_t)nn, (uint8_t)denom, beat, nn * beat, 0 });
        }
    }

    auto by_tick = [](const auto& a, const auto& b) { return a.tick < b.tick; };
    std::stable_sort(tempos_.begin(), tempos_.end(), by_tick);
    std::stable_sort(sigs_.begin(), sigs_.end(), by_tick);

    // defaults at tick 0
    if (tempos_.empty() || tempos_.front().tick != 0) tempos_.insert(tempos_.begin(), { 0, 120.0, 0.0 });
    if (sigs_.empty() || sigs_.front().tick != 0) sigs_.insert(sigs_.begin(), { 0, 4, 4, ppq_, ppq_ * 4u, 0 });

    for (size_t i = 1; i < tempos_.size(); ++i) {
        const auto& p = tempos_[i - 1];
        tempos_[i].secs_at_tick = p.secs_at_tick +
            double(tempos_[i].tick - p.tick) * 60.0 / (p.bpm * ppq_);
    }

    // a bar cut short by a sig change still counts as a bar
    for (size_t i = 1; i < sigs_.size(); ++i) {
        const auto& p = sigs_[i - 1];
        const MIDITick span = sigs_[i].tick - p.tick;
        sigs_[i].first_bar = p.first_bar + (span + p.bar_len - 1) / p.bar_len;
    }
}

BarPos Timeline::bar_at(MIDITick tick) const {
    auto it = std::partition_point(sigs_.begin(), sigs_.end(),
        [&](const TimeSigPoint& s) { return s.tick <= tick; });
    const TimeSigPoint& s = *(it - 1);               // sigs_[0].tick == 0, so it != begin()

    const MIDITick n = (tick - s.tick) / s.bar_len;
    BarPos b{ s.first_bar + uint32_t(n), s.tick + n * s.bar_len, s.bar_len, s.beat_len };
    if (it != sigs_.end() && b.start + b.len > it->tick) b.len = it->tick - b.start;
    return b;
}

MIDITick Timeline::snap_floor(MIDITick tick, MIDITick step) const {
    if (step == 0) return tick;
    const BarPos b = bar_at(tick);
    return b.start + (tick - b.start) / step * step;
}

MIDITick Timeline::snap_ceil(MIDITick tick, MIDITick step) const {
    if (step == 0) return tick;
    const BarPos b = bar_at(tick);
    const MIDITick rel = tick - b.start;
    return b.start + std::min((rel + step - 1) / step * step, b.len);   // clamp = next bar line
}

MIDITick Timeline::snap_nearest(MIDITick tick, MIDITick step) const {
    const MIDITick lo = snap_floor(tick, step);
    const MIDITick hi = snap_ceil(tick, step);
    return (tick - lo < hi - tick) ? lo : hi;                           // ties go up
}

// from tempo map
double Timeline::tick_to_secs(MIDITick tick) const {
    if (tempos_.empty()) {
        return static_cast<double>(tick) * 60.0 / (120.0 / static_cast<double>(ppq_));
    }

    const auto it = std::partition_point(tempos_.begin(), tempos_.end(),
        [tick](const TempoPoint& t) { return t.tick <= tick; });

    std::size_t idx = static_cast<std::size_t>(it - tempos_.begin());
    idx = idx > 0 ? idx - 1 : 0;

    const TempoPoint& p = tempos_[idx];
    return p.secs_at_tick +
        static_cast<double>(tick - p.tick) * 60.0 / (p.bpm * static_cast<double>(ppq_));
}

MIDITick Timeline::secs_to_tick(double secs) const {
    if (tempos_.empty()) {
        return static_cast<MIDITick>(secs * (120.0 * static_cast<double>(ppq_)) / 60.0);
    }

    const auto it = std::partition_point(tempos_.begin(), tempos_.end(),
        [secs](const TempoPoint& t) { return t.secs_at_tick <= secs; });
    std::size_t idx = static_cast<std::size_t>(it - tempos_.begin());
    idx = idx > 0 ? idx - 1 : 0;

    const TempoPoint& p = tempos_[idx];
    const double sec_per_tick = 60.0 / (p.bpm * static_cast<double>(ppq_));
    const double delta_secs = secs - p.secs_at_tick;
    const double delta_ticks = delta_secs / sec_per_tick;
    return p.tick + static_cast<MIDITick>(delta_ticks);
}

double Timeline::bpm_at(MIDITick tick) const {
    if (tempos_.empty()) return 120.0;

    const auto it = std::partition_point(tempos_.begin(), tempos_.end(),
        [tick](const TempoPoint& t) { return t.tick <= tick; });
    size_t idx = (it == tempos_.begin()) ? 0 :
        static_cast<size_t>(std::distance(tempos_.begin(), it) - 1);

    const TempoPoint& p = tempos_[idx];
    return p.bpm;
}

}