#include "ui/widgets/music_glyph.h"

#include <cmath>

namespace andromeda::app::glyphs {

using editor::timeline::SnapModifier;
using editor::timeline::TimelineGridSnap;

// ---------------------------------------------------------------- glyphs ---

Glyph make_note(int flags, bool hollow, bool whole) {
    Glyph g; g.kind = Glyph::Kind::Note;
    g.flags = flags; g.hollow = hollow; g.whole = whole;
    return g;
}

Glyph make_measure(float portion, bool ticks) {
    Glyph g; g.kind = Glyph::Kind::Measure;
    g.portion = portion; g.ticks = ticks;
    return g;
}

namespace {

ImU32 scale_alpha(ImU32 col, float k) {
    const ImU32 a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    return (col & ~IM_COL32_A_MASK) | (ImU32(float(a) * k) << IM_COL32_A_SHIFT);
}

// ------------------------------------------------------------- primitives ---

void draw_filled_ellipse(ImDrawList* dl, ImVec2 c, float rx, float ry, float rot, ImU32 col) {
    constexpr int N = 28;
    ImVec2 pts[N];
    const float cr = std::cos(rot), sr = std::sin(rot);
    for (int i = 0; i < N; ++i) {
        const float a = 6.2831853f * float(i) / float(N);
        const float x = rx * std::cos(a), y = ry * std::sin(a);
        pts[i] = { c.x + x * cr - y * sr, c.y + x * sr + y * cr };
    }
    dl->AddConvexPolyFilled(pts, N, col);
}

// Distance from an ellipse's centre to its edge in direction `a`.
float ellipse_radius(float rx, float ry, float rot, float a) {
    const float t  = a - rot;
    const float cx = ry * std::cos(t), sx = rx * std::sin(t);
    return rx * ry / std::sqrt(cx * cx + sx * sx);
}

// Ellipse with a differently-rotated elliptical hole (half / whole noteheads).
// Built as a triangle strip so it works over any background (selection/hover colour).
void draw_hollow_head(ImDrawList* dl, ImVec2 c,
                      float rx, float ry, float rot,
                      float hrx, float hry, float hrot, ImU32 col) {
    constexpr int N = 32;
    ImVec2 outer[N], inner[N];
    for (int i = 0; i < N; ++i) {
        const float a  = 6.2831853f * float(i) / float(N);
        const float ro = ellipse_radius(rx, ry, rot, a);
        const float ri = ellipse_radius(hrx, hry, hrot, a);
        outer[i] = { c.x + ro * std::cos(a), c.y + ro * std::sin(a) };
        inner[i] = { c.x + ri * std::cos(a), c.y + ri * std::sin(a) };
    }
    // AA on filled triangles leaves faint seams between neighbours, so fill
    // without AA and smooth the outer edge with an AA outline afterwards.
    const ImDrawListFlags saved = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < N; ++i) {
        const int j = (i + 1) % N;
        dl->AddTriangleFilled(outer[i], outer[j], inner[j], col);
        dl->AddTriangleFilled(outer[i], inner[j], inner[i], col);
    }
    dl->Flags = saved;
    dl->AddPolyline(outer, N, col, ImDrawFlags_Closed, 1.0f); // older ImGui: (outer, N, col, true, 1.0f)
}

// One tapered flag hanging from the stem at `p`.
void draw_flag(ImDrawList* dl, ImVec2 p, float s, ImU32 col, float th) {
    const ImVec2 p0 = p;
    const ImVec2 p1 = { p.x + 0.02f * s, p.y + 0.18f * s };
    const ImVec2 p2 = { p.x + 0.44f * s, p.y + 0.22f * s };
    const ImVec2 p3 = { p.x + 0.30f * s, p.y + 0.54f * s };

    const auto at = [&](float t) {
        const float u = 1.0f - t;
        const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        return ImVec2{ b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
                       b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y };
    };

    constexpr int K = 12;
    for (int i = 0; i < K; ++i) {
        const float t0 = float(i) / K, t1 = float(i + 1) / K;
        const float w  = th * (1.9f - 1.1f * t0);              // thick at stem -> thin at tip
        dl->AddLine(at(t0), at(t1), col, std::fmax(1.0f, w));
    }
}

// ----------------------------------------------------------------- note ---

void draw_note(ImDrawList* dl, ImVec2 o, float s, ImU32 col, const Glyph& g) {
    const float th = std::fmax(1.0f, std::round(s / 12.0f));
    ImVec2 c;
    float ext;                                   // half-width of the notehead

    if (g.whole) {
        c   = { o.x + 0.42f * s, o.y + 0.50f * s };
        ext = 0.38f * s;
        draw_hollow_head(dl, c, ext, 0.25f * s, 0.0f, 0.20f * s, 0.085f * s, -1.0f, col);
    } else {
        const float rx = 0.30f * s, ry = 0.21f * s, rot = -0.42f;
        c = { o.x + 0.34f * s, o.y + 0.78f * s };
        const float cr = std::cos(rot), sr = std::sin(rot);
        ext = std::sqrt(rx * rx * cr * cr + ry * ry * sr * sr);

        if (g.hollow) draw_hollow_head(dl, c, rx, ry, rot, 0.21f * s, 0.075f * s, -0.62f, col);
        else          draw_filled_ellipse(dl, c, rx, ry, rot, col);

        const float sx  = c.x + ext - th * 0.5f;
        const float top = o.y + 0.02f * s;
        dl->AddLine({ sx, c.y }, { sx, top }, col, th);

        const float gap = (g.flags <= 2) ? 0.15f : (g.flags == 3 ? 0.13f : 0.105f);
        for (int i = 0; i < g.flags; ++i)
            draw_flag(dl, { sx, top + gap * s * float(i) }, s, col, th);
    }

    if (g.dotted)
        dl->AddCircleFilled({ c.x + ext + 0.17f * s, c.y - 0.02f * s }, std::fmax(1.2f, s * 0.07f), col);

    if (g.tuplet) {
        const char buf[2] = { g.tuplet, 0 };
        dl->AddText(ImGui::GetFont(), s * 0.6f, { o.x, o.y - 0.05f * s }, col, buf);
    }
}

// -------------------------------------------------------------- measure ---

void draw_measure(ImDrawList* dl, ImVec2 o, float s, ImU32 col, float portion, bool ticks) {
    const float th = std::fmax(1.0f, std::round(s / 14.0f));
    const float x0 = std::floor(o.x + 0.05f * s) + 0.5f;     // +0.5 keeps 1px lines crisp
    const float x1 = x0 + std::round(1.2f * s);
    const float y0 = std::floor(o.y + 0.19f * s) + 0.5f;
    const float y1 = y0 + std::round(0.62f * s);
    const float ym = std::floor((y0 + y1) * 0.5f) + 0.5f;
    const float ov = 0.08f * s;                              // barlines overshoot the staff

    // highlighted part
    if (ticks) {
        const float fw = std::fmax(1.5f, (x1 - x0) * portion);
        dl->AddRectFilled({ x0, y0 }, { x0 + fw, y1 }, col);
        for (int k = 1; k < 4; ++k) {                        // beat dividers
            const float x = std::floor(x0 + (x1 - x0) * float(k) / 4.0f) + 0.5f;
            dl->AddLine({ x, y0 }, { x, y1 }, scale_alpha(col, 0.4f), 1.0f);
        }
    } else {
        dl->AddRectFilled({ x0, y0 }, { x1, y1 }, scale_alpha(col, 0.45f));
    }

    // staff lines + barlines
    dl->AddLine({ x0, y0 }, { x1, y0 }, col, 1.0f);
    dl->AddLine({ x0, ym }, { x1, ym }, col, 1.0f);
    dl->AddLine({ x0, y1 }, { x1, y1 }, col, 1.0f);
    dl->AddLine({ x0, y0 - ov }, { x0, y1 + ov }, col, th);
    dl->AddLine({ x1, y0 - ov }, { x1, y1 + ov }, col, th);
}

} // namespace

void draw_glyph(ImDrawList* dl, ImVec2 origin, float s, ImU32 col, const Glyph& g) {
    switch (g.kind) {
    case Glyph::Kind::Note:    draw_note(dl, origin, s, col, g); break;
    case Glyph::Kind::Measure: draw_measure(dl, origin, s, col, g.portion, g.ticks); break;
    default: break;
    }
}

Glyph glyph_for(TimelineGridSnap s) {
    switch (s) {
    case TimelineGridSnap::Bar:           return make_measure(1.0f,    false);
    case TimelineGridSnap::Beat:          return make_measure(0.25f,   true);
    case TimelineGridSnap::HalfBeat:      return make_measure(0.125f,  true);
    case TimelineGridSnap::QuarterBeat:   return make_measure(0.0625f, true);
    case TimelineGridSnap::WholeNote:     return make_note(0, true,  true);
    case TimelineGridSnap::HalfNote:      return make_note(0, true);
    case TimelineGridSnap::QuarterNote:   return make_note(0);
    case TimelineGridSnap::EighthNote:    return make_note(1);
    case TimelineGridSnap::SixteenthNote: return make_note(2);
    case TimelineGridSnap::Note32:        return make_note(3);
    case TimelineGridSnap::Note64:        return make_note(4);
    case TimelineGridSnap::Note128:       return make_note(5);
    default:                              return {};
    }
}

Glyph glyph_for(SnapModifier m) {
    Glyph g = make_note(0);                                  // quarter note base
    switch (m) {
    case SnapModifier::Dotted:     g.dotted = true; break;
    case SnapModifier::Triplet:    g.tuplet = '3';  break;
    case SnapModifier::Quintuplet: g.tuplet = '5';  break;
    case SnapModifier::Septuplet:  g.tuplet = '7';  break;
    case SnapModifier::Straight:   return {};                // blank, keeps alignment
    default: break;
    }
    return g;
}

// ------------------------------------------------------------ selectable ---

bool icon_selectable(const char* text, bool selected, const Glyph& g) {
    const float fh     = ImGui::GetFontSize();
    const float icon_w = fh * 1.5f;
    const float h      = ImGui::GetTextLineHeight();
    const ImVec2 ts    = ImGui::CalcTextSize(text);
    const ImVec2 pos   = ImGui::GetCursorScreenPos();

    ImGui::PushID(text);
    const bool pressed = ImGui::Selectable("##sel", selected,
        ImGuiSelectableFlags_DontClosePopups, ImVec2(icon_w + ts.x, h));
    ImGui::PopID();

    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_glyph(dl, { pos.x, pos.y + (h - fh) * 0.5f }, fh, col, g);
    dl->AddText({ pos.x + icon_w, pos.y }, col, text);
    return pressed;
}

const char* snap_name(TimelineGridSnap s) {
    switch (s) {
    case TimelineGridSnap::Bar:           return "Bar";
    case TimelineGridSnap::Beat:          return "Beat";
    case TimelineGridSnap::HalfBeat:      return "1/2 beat";
    case TimelineGridSnap::QuarterBeat:   return "1/4 beat";
    case TimelineGridSnap::WholeNote:     return "Whole";
    case TimelineGridSnap::HalfNote:      return "Half";
    case TimelineGridSnap::QuarterNote:   return "Quarter";
    case TimelineGridSnap::EighthNote:    return "Eighth";
    case TimelineGridSnap::SixteenthNote: return "1/16th";
    case TimelineGridSnap::Note32:        return "1/32nd";
    case TimelineGridSnap::Note64:        return "1/64th";
    case TimelineGridSnap::Note128:       return "1/128th";
    default:                              return "None";
    }
}

const char* modifier_name(SnapModifier m) {
    switch (m) {
    case SnapModifier::Straight: return "";
    case SnapModifier::Dotted: return "dotted";
    case SnapModifier::Triplet: return "triplet";
    case SnapModifier::Quintuplet: return "quintuplet";
    case SnapModifier::Septuplet: return "septuplet";
    default: return "????";
    }
}

} // namespace andromeda::app::glyphs
