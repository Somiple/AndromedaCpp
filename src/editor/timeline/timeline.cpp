#include "timeline.h"
#include <algorithm>
#include <numeric>

namespace andromeda::editor::timeline {

namespace {

TickRatio modifier_ratio(SnapModifier m) {
    switch (m) {
    case SnapModifier::Triplet:    return { 2, 3 };  // 3 in the time of 2
    case SnapModifier::Dotted:     return { 3, 2 };
    case SnapModifier::Quintuplet: return { 4, 5 };  // 5 in the time of 4
    case SnapModifier::Septuplet:  return { 4, 7 };  // 7 in the time of 4
    default:                       return { 1, 1 };
    }
}

TickRatio base_step(const std::pair<uint8_t, uint8_t> ts, std::uint16_t ppq, TimelineGridSnap snap) {
    const std::uint64_t p = ppq;
    const std::uint64_t num = std::max<std::uint64_t>((uint64_t)ts.first, 1);
    const std::uint64_t den = std::max<std::uint64_t>((uint64_t)ts.second, 1);

    switch (snap) {
    case TimelineGridSnap::Bar:           return { p * 4 * num, den };
    case TimelineGridSnap::Beat:          return { p * 4, den };
    case TimelineGridSnap::HalfBeat:      return { p * 2, den };
    case TimelineGridSnap::QuarterBeat:   return { p, den };
    case TimelineGridSnap::WholeNote:     return { p * 4, 1 };
    case TimelineGridSnap::HalfNote:      return { p * 2, 1 };
    case TimelineGridSnap::QuarterNote:   return { p, 1 };
    case TimelineGridSnap::EighthNote:    return { p, 2 };
    case TimelineGridSnap::SixteenthNote: return { p, 4 };
    case TimelineGridSnap::Note32:        return { p, 8 };
    case TimelineGridSnap::Note64:        return { p, 16 };
    case TimelineGridSnap::Note128:       return { p, 32 };
    default:                              return { 1, 1 };
    }
}

TickRatio step_of(const std::pair<uint8_t, uint8_t> ts, std::uint16_t ppq, TimelineGridSnap snap,
    SnapModifier mod) {
    TickRatio s = base_step(ts, ppq, snap);
    if (snap != TimelineGridSnap::Bar) {          // "triplet bar" is not a useful grid
        const TickRatio m = modifier_ratio(mod);
        s.num *= m.num;
        s.den *= m.den;
    }
    const std::uint64_t g = std::gcd(s.num, s.den);
    if (g > 1) { s.num /= g; s.den /= g; }
    if (s.num == 0) s.num = 1;                    // ppq == 0 guard
    return s;
}

MIDITick step_of(const std::pair<uint8_t, uint8_t> ts, uint16_t ppq, TimelineGridSnap snap) {
    const MIDITick p = ppq;
    const MIDITick num = std::max<MIDITick>((uint64_t)ts.first, 1);
    const MIDITick den = std::max<MIDITick>((uint64_t)ts.second, 1);  // real denominator (4, 8...), not the MIDI exponent

    MIDITick step = 1;
    switch (snap) {
        // relative to the time signature
        case TimelineGridSnap::Bar:         step = p * 4 * num / den; break;
        case TimelineGridSnap::Beat:        step = p * 4 / den;       break;
        case TimelineGridSnap::HalfBeat:    step = p * 2 / den;       break;
        case TimelineGridSnap::QuarterBeat: step = p / den;           break;
        // absolute note values
        case TimelineGridSnap::WholeNote:     step = p * 4;  break;
        case TimelineGridSnap::HalfNote:      step = p * 2;  break;
        case TimelineGridSnap::QuarterNote:   step = p;      break;
        case TimelineGridSnap::EighthNote:    step = p / 2;  break;
        case TimelineGridSnap::SixteenthNote: step = p / 4;  break;
        case TimelineGridSnap::Note32:        step = p / 8;  break;
        case TimelineGridSnap::Note64:        step = p / 16; break;
        case TimelineGridSnap::Note128:       step = p / 32; break;
        default: break;
    }
    return std::max<MIDITick>(step, 1);
}


// grid point k, rounded to the nearest tick (no accumulated drift)
MIDITick grid_tick(MIDITick anchor, std::uint64_t k, TickRatio s) {
    return anchor + static_cast<MIDITick>((k * s.num + s.den / 2) / s.den);
}

// nearest grid point to `tick`, grid anchored at the time signature's tick (tick >= anchor)
MIDITick snap_to_grid(MIDITick tick, MIDITick anchor, TickRatio s) {
    const std::uint64_t rel = tick - anchor;
    const std::uint64_t k = (rel * s.den + s.num / 2) / s.num;   // nearest index
    return grid_tick(anchor, k, s);
}

// grid index k such that grid_tick(k) is the last grid point <= rel
std::uint64_t index_floor(std::uint64_t rel, TickRatio s) {
    std::uint64_t k = rel * s.den / s.num;                 // exact position <= rel
    // rounding can pull the next point back onto/below rel (e.g. 6.4 -> 6)
    while (grid_tick(0, k + 1, s) <= rel) ++k;
    return k;
}

}

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

    bump_revision();
}

BarPos Timeline::bar_at(MIDITick tick) const {
    auto it = std::partition_point(sigs_.begin(), sigs_.end(),
        [&](const TimeSigPoint& s) { return s.tick <= tick; });
    const TimeSigPoint& s = *(it - 1);               // sigs_[0].tick == 0, so it != begin()

    const MIDITick n = (tick - s.tick) / s.bar_len;
    BarPos b{ s.first_bar + uint32_t(n), s.tick + n * s.bar_len, s.bar_len, s.beat_len,
        { s.numerator, s.denominator } };
    if (it != sigs_.end() && b.start + b.len > it->tick) b.len = it->tick - b.start;
    return b;
}

MIDITick Timeline::snap_floor(MIDITick tick, TickRatio s) const {
    if (s.num == 0 || s.den == 0) return tick;
    const BarPos b = bar_at(tick);
    const MIDITick rel = tick - b.start;
    return grid_tick(b.start, index_floor(rel, s), s);
}

MIDITick Timeline::snap_ceil(MIDITick tick, TickRatio s) const {
    if (s.num == 0 || s.den == 0) return tick;
    const BarPos b = bar_at(tick);
    const MIDITick rel = tick - b.start;

    std::uint64_t k = index_floor(rel, s);
    if (grid_tick(0, k, s) < rel) ++k;                     // a point exactly on `rel` is its own ceil
    return b.start + std::min<MIDITick>(grid_tick(0, k, s), b.len);   // clamp = next bar line
}

MIDITick Timeline::snap_nearest(MIDITick tick, TickRatio s) const {
    const MIDITick lo = snap_floor(tick, s);
    const MIDITick hi = snap_ceil(tick, s);
    return (tick - lo < hi - tick) ? lo : hi;              // ties go upd
}

TickRatio Timeline::grid_at(MIDITick tick) const {
    const BarPos b = bar_at(tick);
    return step_of(b.time_sig, ppq_, snap_, modifier_);
}

MIDITick Timeline::step_length_at(MIDITick tick) const {
    const TickRatio s = grid_at(tick);
    return std::max<MIDITick>((s.num + s.den / 2) / s.den, 1);
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