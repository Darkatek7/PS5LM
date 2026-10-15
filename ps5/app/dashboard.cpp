// SPDX-License-Identifier: GPL-3.0-or-later
//
// PS5LM's screen. One idea runs through it: the model is the thing, and the
// network is where it goes. Home shows the model and how fast it runs on the
// left, the address other devices use on the right, and nothing else large;
// the console's temperature and power sit in the status line, where a car
// shows the weather. Colour means state only: blue is what is active or
// chosen, green a model that serves, red a fault. Everything else is grey.
#include "dashboard.hpp"
#include "suggested.hpp"
#include "version.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace ps5lm {

using hui::Action;
using hui::Direction;
using hui::gfx::Color;
using hui::gfx::Rect;
namespace gfx   = hui::gfx;
namespace ui    = hui::ui;
namespace tween = hui::tween;
using Cue = hui::audio::Cue;

namespace {

// ---- the design language -------------------------------------------------------

const Color kBase    = Color::rgb(0x121314);
const Color kSurface = Color::rgb(0x1c1d1f);  // sheets and panels
const Color kRaised  = Color::rgb(0x28292c);  // the focused row
const Color kHair    = Color::rgb(0x2c2e31);  // dividers
const Color kInk     = Color::rgb(0xf4f4f5);
const Color kInk2    = Color::rgb(0xa3a5aa);
const Color kInk3    = Color::rgb(0x6b6e74);
const Color kBlue    = Color::rgb(0x3e6ae1);
const Color kRed     = Color::rgb(0xe5484d);
const Color kGreen   = Color::rgb(0x3dbe7a);
const Color kBlack   = Color::rgb(0x000000);

constexpr float kLeft = 96.0f, kRight = 1824.0f;
constexpr float kTitleY = 182.0f;                 // page titles' baseline
constexpr float kDockY = 966.0f;                  // the dock's top edge
constexpr float kRailW = 330.0f, kRailPitch = 76.0f, kRailH = 64.0f;
constexpr float kListX = 486.0f, kListY = 236.0f, kRowPitch = 92.0f, kRowH = 80.0f;
constexpr int   kVisibleRows = 7;
constexpr float kTau = 6.2831853f;
constexpr const char * kDash = "\xE2\x80\x94";

constexpr const char * kPageNames[Dashboard::kPages] = { "Home", "Models", "API", "Settings" };
constexpr const char * kSectionNames[Dashboard::kSections] = { "On this PS5", "Suggested", "Search", "Downloads" };

// The market's searches, switched with L2/R2: Hugging Face's most downloaded
// GGUF repositories, then the families the presets know.
constexpr const char * kMarketChips[] = { "Popular", "Qwen", "Gemma", "Llama", "Mistral", "Phi", "Granite" };
constexpr int          kMarketChipCount = 7;

// ---- Settings -------------------------------------------------------------------

enum class Setting : int { auto_load, default_model, ctx_cap, kv_type, sounds, tools, scratch_limit, clear_scratch, download_dir };

struct SettingRow {
    Setting      id;
    const char * label;
    const char * help;
};

struct SettingCategory {
    const char * name;
    SettingRow   rows[3];
    int          count;
};

constexpr SettingCategory kCategories[] = {
    { "Startup",
      { { Setting::auto_load, "Load a model at launch", "Off: the app opens and waits for a choice" },
        { Setting::default_model, "Model to load", "One of the models on this PS5" } },
      2 },
    { "Model",
      { { Setting::ctx_cap, "Longest context", "The longest that fits, up to this; applies at the next load" },
        { Setting::kv_type, "Context cache", "Auto picks the best that fits; a fixed type trades length for quality" } },
      2 },
    { "Chat",
      { { Setting::tools, "File tools", "The browser chat may read and write files in /data/PS5LM/scratch" },
        { Setting::scratch_limit, "Warn above", "A notification when that folder grows past this" },
        { Setting::clear_scratch, "Clear the scratch folder", "Deletes everything the chat's tools wrote" } },
      3 },
    { "Sound and storage",
      { { Setting::sounds, "Sounds", "Interface sounds on the TV" },
        { Setting::download_dir, "Save downloads to", "Internal storage, or PS5LM/models on an M.2 or USB drive" } },
      2 },
    { "Diagnostics", {}, 0 },
};
constexpr int kCategoryCount = 5;
constexpr int kDiagnostics = 4;

constexpr uint32_t     kCtxSteps[] = { 4096, 8192, 16384, 32768, 65536, 131072 };
constexpr const char * kKvSteps[] = { "auto", "f16", "q8_0", "q4_0" };
constexpr uint32_t     kScratchSteps[] = { 1, 2, 5, 10, 20 };

constexpr int   kLogLines = 20;
constexpr float kLogLineH = 29.0f;

// ---- small helpers ----------------------------------------------------------------

float put(gfx::DrawList & l, const ui::FontRef & f, std::string_view v, float x, float y, float size, Color c,
           gfx::Align a = gfx::Align::left) {
    return ui::text(l, f, v, x, y, size, c, a);
}

std::string format(const char * fmt, double v) {
    char t[48];
    std::snprintf(t, sizeof(t), fmt, v);
    return t;
}

// "9,120"
std::string thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = (int) s.size() - 3; i > 0; i -= 3) {
        s.insert((size_t) i, ",");
    }
    return s;
}

// Sizes as the console's own screens give them: binary gigabytes, called GB.
std::string gb(double gib) { return format("%.1f GB", gib); }
std::string gb_text(std::string s) {
    for (size_t at = s.find("GiB"); at != std::string::npos; at = s.find("GiB", at)) {
        s.replace(at, 3, "GB");
    }
    return s;
}

// "Qwen3.8-27B-UD-IQ2_XXS" -> "Qwen3.8 27B" and "UD-IQ2_XXS": the build starts
// at the first part that names a quantization.
bool quant_part(const std::string & p) {
    std::string u = p;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    const auto starts = [&](const char * s) { return u.rfind(s, 0) == 0; };
    return u == "UD" || u == "BF16" || u == "F16" || u == "F32" || starts("MXFP") || u.find("_K") != std::string::npos ||
           (u.size() > 1 && (u[0] == 'Q' || starts("IQ") || starts("TQ") || starts("PQ")) &&
            std::isdigit((unsigned char) u[u[0] == 'Q' ? 1 : 2]));
}

void split_name(const std::string & label, std::string * name, std::string * build) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= label.size()) {
        const size_t dash = label.find('-', at);
        parts.push_back(label.substr(at, dash == std::string::npos ? std::string::npos : dash - at));
        if (dash == std::string::npos) {
            break;
        }
        at = dash + 1;
    }
    name->clear();
    build->clear();
    bool in_build = false;
    for (const auto & p : parts) {
        in_build = in_build || quant_part(p);
        std::string & to = in_build ? *build : *name;
        if (!in_build && (p == "it" || p == "Instruct" || p == "GGUF")) {
            continue;
        }
        to += (to.empty() ? "" : in_build ? "-" : " ") + p;
    }
    if (name->empty()) {
        *name = label;
    }
}

// A slim bar: the track, and the share filled.
void meter(gfx::DrawList & l, float x, float y, float w, float share, Color c) {
    l.rounded_rect({ x, y, w, 4 }, 2, kHair);
    if (share > 0.002f) {
        l.rounded_rect({ x, y, std::max(4.0f, w * tween::clamp01(share)), 4 }, 2, c);
    }
}

// The model's speed over the last two minutes as one line, newest on the
// right, on a faint floor; Tesla's energy graph, not a dashboard chart.
void trace(gfx::DrawList & l, const std::vector<float> & v, const Rect & g, Color c) {
    l.line(g.x, g.y + g.h, g.x + g.w, g.y + g.h, 1.5f, kHair);
    if (v.size() < 2) {
        return;
    }
    float top = 1.0f;
    for (float x : v) {
        top = std::max(top, x);
    }
    top *= 1.15f;
    const float step = g.w / 119.0f;
    float lx = 0, ly = 0;
    for (size_t age = 0; age < v.size(); ++age) {
        const float x = g.x + g.w - (float) age * step;
        const float y = g.y + g.h - v[v.size() - 1 - age] / top * g.h;
        if (age > 0) {
            l.line(lx, ly, x, y, 2.0f, c);
        }
        lx = x;
        ly = y;
    }
    const float y = g.y + g.h - v.back() / top * g.h;
    l.circle(g.x + g.w, y, 5, c);
}

// The dock's line icons, 32 px, drawn with strokes like the rest.
void icon(gfx::DrawList & l, int page, float cx, float cy, Color c) {
    const float w = 2.6f;
    switch (page) {
        case Dashboard::kHomePage:
            l.line(cx - 16, cy - 1, cx, cy - 14, w, c);
            l.line(cx, cy - 14, cx + 16, cy - 1, w, c);
            l.line(cx - 11, cy - 5, cx - 11, cy + 13, w, c);
            l.line(cx + 11, cy - 5, cx + 11, cy + 13, w, c);
            l.line(cx - 11, cy + 13, cx + 11, cy + 13, w, c);
            l.line(cx - 3, cy + 13, cx - 3, cy + 4, w, c);
            l.line(cx + 3, cy + 13, cx + 3, cy + 4, w, c);
            l.line(cx - 3, cy + 4, cx + 3, cy + 4, w, c);
            break;
        case Dashboard::kModelsPage: {
            // A cube: the hexagon and the three edges that meet in front.
            const float r = 15, h = 13;
            const float px[] = { cx, cx + h, cx + h, cx, cx - h, cx - h };
            const float py[] = { cy - r, cy - r / 2, cy + r / 2, cy + r, cy + r / 2, cy - r / 2 };
            for (int i = 0; i < 6; ++i) {
                l.line(px[i], py[i], px[(i + 1) % 6], py[(i + 1) % 6], w, c);
            }
            l.line(cx, cy, cx - h, cy - r / 2, w, c);
            l.line(cx, cy, cx + h, cy - r / 2, w, c);
            l.line(cx, cy, cx, cy + r, w, c);
            break;
        }
        case Dashboard::kApiPage:
            l.line(cx - 7, cy - 10, cx - 16, cy, w, c);
            l.line(cx - 16, cy, cx - 7, cy + 10, w, c);
            l.line(cx + 7, cy - 10, cx + 16, cy, w, c);
            l.line(cx + 16, cy, cx + 7, cy + 10, w, c);
            l.line(cx + 3, cy - 13, cx - 3, cy + 13, w, c);
            break;
        default: {
            const float knobs[] = { -6, 7, -1 };
            for (int i = 0; i < 3; ++i) {
                const float y = cy - 10 + 10.0f * (float) i;
                l.line(cx - 16, y, cx + 16, y, w, c);
                l.circle(cx + knobs[i], y, 5.5f, kBase);
                l.ring(cx + knobs[i], y, 5.0f, w, c);
            }
            break;
        }
    }
}

// A Tesla-style switch: a pill, blue and knob right when on.
void toggle_switch(gfx::DrawList & l, float right, float cy, bool on) {
    const Rect r{ right - 64, cy - 18, 64, 36 };
    l.rounded_rect(r, 18, on ? kBlue : kHair);
    l.circle(on ? r.x + r.w - 18 : r.x + 18, cy, 14, on ? kInk : kInk2);
}

// Greedy word wrap to `width`; a word longer than a line is cut.
std::vector<std::string> wrap(const ui::FontRef & f, const std::string & text, float size, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string word = text.substr(at, end - at);
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && f.measure(next, size) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = next;
        }
        at = end + 1;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    for (auto & l : lines) {
        l = f.font->fit(l, size, width);
    }
    return lines;
}

int first_visible(int cursor, int rows) {
    return std::clamp(cursor - kVisibleRows / 2, 0, std::max(0, rows - kVisibleRows));
}

// 2 error, 1 warning, 0 neither. llama.cpp marks its lines " E " and " W ";
// the rest is matched by words, so "failures=0" is not an error.
int log_level(const std::string & line) {
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char) std::tolower(ch); });
    const auto has = [&](const char * w) { return low.find(w) != std::string::npos; };
    if (line.find(" E ") != std::string::npos || has("error") || has("failed") || has("exception") || has("fatal") ||
        has("abort")) {
        return 2;
    }
    if (line.find(" W ") != std::string::npos || has("warn")) {
        return 1;
    }
    return 0;
}

// "2.1 of 10.4 GB, 3.8 MB/s, 37 min left", or the note of one that waits.
std::string download_line(const Download & d) {
    char t[200];
    const double done = d.done / 1073741824.0, all = d.bytes / 1073741824.0;
    if (d.state == DownloadState::running && d.mbps > 0.05) {
        const double left_s = (double) (d.bytes - std::min(d.done, d.bytes)) / (d.mbps * 1e6);
        if (left_s >= 3600) {
            std::snprintf(t, sizeof(t), "%.1f of %.1f GB, %.1f MB/s, %.0f h %02.0f min left", done, all, d.mbps,
                          std::floor(left_s / 3600), std::fmod(left_s / 60, 60));
        } else {
            std::snprintf(t, sizeof(t), "%.1f of %.1f GB, %.1f MB/s, %.0f min left", done, all, d.mbps, std::ceil(left_s / 60));
        }
        return t;
    }
    std::snprintf(t, sizeof(t), "%.1f of %.1f GB", done, all);
    if (d.state == DownloadState::running) {
        return std::string(t) + ", starting";
    }
    if (d.state == DownloadState::done) {
        return d.note;
    }
    if (!d.note.empty()) {
        return std::string(t) + ", " + d.note;
    }
    return d.state == DownloadState::queued ? std::string(t) + ", waiting its turn" : t;
}

// The value of a setting as its row shows it, and one step of it.
std::string setting_text(const Settings & s, Setting id, const std::vector<ModelRow> & models,
                         const std::vector<Drive> & drives) {
    switch (id) {
        case Setting::default_model: {
            if (s.default_model.empty()) {
                return "None";
            }
            for (const auto & m : models) {
                if (m.file == s.default_model) {
                    std::string name, build;
                    split_name(m.label, &name, &build);
                    return name + "  " + build;
                }
            }
            return "Missing";
        }
        case Setting::ctx_cap: return format("%.0fk tokens", s.ctx_cap / 1024.0);
        case Setting::kv_type: return s.kv_type == "auto" ? "Auto" : s.kv_type;
        case Setting::scratch_limit: return format("%.0f GB", (double) s.scratch_limit_gib);
        case Setting::download_dir: {
            for (const auto & d : drives) {
                if (d.models == s.download_dir) {
                    return d.name;
                }
            }
            return s.download_dir.rfind("/mnt/ext", 0) == 0 ? "M.2 drive, not found" : "USB drive, not found";
        }
        default: return "";
    }
}

bool is_toggle(Setting id) { return id == Setting::auto_load || id == Setting::sounds || id == Setting::tools; }

bool toggle_value(const Settings & s, Setting id) {
    return id == Setting::auto_load ? s.auto_load : id == Setting::sounds ? s.sounds : s.tools;
}

template <typename T, size_t N> bool step_in(const T (&steps)[N], T & value, int dir, int fallback) {
    int at = fallback;
    for (int i = 0; i < (int) N; ++i) {
        if (steps[i] == value) {
            at = i;
        }
    }
    if (at + dir < 0 || at + dir >= (int) N) {
        return false;
    }
    value = steps[at + dir];
    return true;
}

// Moves a stepper one place; false at either end.
bool step_setting(Settings & s, Setting id, int dir, const std::vector<ModelRow> & models, const std::vector<Drive> & drives) {
    switch (id) {
        case Setting::default_model: {
            // "None", then each model that fits, in the list's order.
            std::vector<std::string> choices = { "" };
            for (const auto & m : models) {
                if (m.fits) {
                    choices.push_back(m.file);
                }
            }
            int at = 0;
            for (int i = 0; i < (int) choices.size(); ++i) {
                if (choices[(size_t) i] == s.default_model) {
                    at = i;
                }
            }
            if (at + dir < 0 || at + dir >= (int) choices.size()) {
                return false;
            }
            s.default_model = choices[(size_t) (at + dir)];
            return true;
        }
        case Setting::ctx_cap: return step_in(kCtxSteps, s.ctx_cap, dir, 4);
        case Setting::kv_type: {
            int at = 0;
            for (int i = 0; i < 4; ++i) {
                if (s.kv_type == kKvSteps[i]) {
                    at = i;
                }
            }
            if (at + dir < 0 || at + dir >= 4) {
                return false;
            }
            s.kv_type = kKvSteps[at + dir];
            return true;
        }
        case Setting::scratch_limit: return step_in(kScratchSteps, s.scratch_limit_gib, dir, 1);
        case Setting::auto_load: s.auto_load = !s.auto_load; return true;
        case Setting::sounds: s.sounds = !s.sounds; return true;
        case Setting::tools: s.tools = !s.tools; return true;
        case Setting::clear_scratch: return false;
        case Setting::download_dir: {
            int at = -1;  // a missing drive counts as before the first
            for (int i = 0; i < (int) drives.size(); ++i) {
                if (drives[(size_t) i].models == s.download_dir) {
                    at = i;
                }
            }
            if (at + dir < 0 || at + dir >= (int) drives.size()) {
                return false;
            }
            s.download_dir = drives[(size_t) (at + dir)].models;
            return true;
        }
    }
    return false;
}

// The base name a suggestion's model file has on the console.
const char * local_name(const SuggestedFile & f) { return f.save_as ? f.save_as : f.file; }

}  // namespace

// ---- state ------------------------------------------------------------------------

void Dashboard::open_library() {
    page_from_ = page_;
    page_ = kModelsPage;
    page_age_ = 0;
    section_ = kLocal;
    in_list_ = true;
    local_cursor_ = 0;
    for (int i = 0; i < (int) live_.models.size(); ++i) {
        if (live_.models[(size_t) i].current) {
            local_cursor_ = i;
        }
    }
}

void Dashboard::show_downloads() {
    go_to(kModelsPage, nullptr);
    section_ = kDownloads;
    in_list_ = true;
}

bool Dashboard::wants_logs() const { return page_ == kSettingsPage && setting_category_ == kDiagnostics; }

void Dashboard::go_to(int page, ui::Feedback * feedback) {
    page_from_ = page_;
    page_ = page;
    page_age_ = 0;
    if (feedback) {
        feedback->play(Cue::tab, 1.0f + 0.04f * (float) page);
    }
}

void Dashboard::set_logs(std::vector<std::string> app, std::vector<std::string> llama) {
    app_log_ = std::move(app);
    llama_log_ = std::move(llama);
    refilter_logs();
}

void Dashboard::refilter_logs() {
    const auto keep = [&](const std::string & line) { return !log_errors_ || log_level(line) > 0; };
    app_view_.clear();
    llama_view_.clear();
    std::copy_if(app_log_.begin(), app_log_.end(), std::back_inserter(app_view_), keep);
    std::copy_if(llama_log_.begin(), llama_log_.end(), std::back_inserter(llama_view_), keep);
    log_scroll_ = std::min(log_scroll_, std::max(0, (int) log_view().size() - kLogLines));
}

// ---- input ------------------------------------------------------------------------

void Dashboard::update(const hui::InputFrame & raw, float dt, ui::Feedback & feedback) {
    const hui::InputFrame none{};
    const bool dialog = market_confirm_ || market_space_;
    const hui::InputFrame & input = busy() ? none : raw;  // busy: the screen says so, buttons wait
    age_ += dt;
    clock_ += dt;
    page_age_ += dt;

    // L1/R1 walk the dock, unless a question is open.
    const int step = input.is_pressed(Action::page_next) ? 1 : input.is_pressed(Action::page_prev) ? -1 : 0;
    if (step != 0 && !dialog) {
        const int next = page_ + step;
        if (next >= 0 && next < kPages) {
            go_to(next, &feedback);
        } else {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
        }
    } else if (page_ == kHomePage) {
        update_home(input, feedback);
    } else if (page_ == kModelsPage) {
        update_models(input, feedback);
    } else if (page_ == kSettingsPage) {
        update_settings(input, feedback);
    }

    // Where the highlight goes: a section or a row on Models, a category or a
    // row on Settings.
    float target = kListY;
    if (page_ == kModelsPage) {
        int row = section_;
        if (in_list_) {
            const int cursor = section_ == kLocal ? local_cursor_ : section_ == kSuggested ? suggested_cursor_
                             : section_ == kDownloads ? dl_cursor_ : market_files_ ? market_file_ : market_repo_;
            const int count = section_ == kLocal ? (int) live_.models.size() : section_ == kSuggested ? kSuggestionCount
                            : section_ == kDownloads ? (int) market_.downloads.size()
                            : market_files_ && market_.open >= 0 && market_.open < (int) market_.repos.size()
                                ? (int) market_.repos[(size_t) market_.open].files.size() : (int) market_.repos.size();
            row = cursor - first_visible(cursor, count);
            target = kListY + (section_ == kSearch ? 72.0f : 0.0f) + (float) row * kRowPitch;
        } else {
            target = kListY + (float) row * kRailPitch;
        }
    } else if (page_ == kSettingsPage) {
        target = in_rows_ && setting_category_ != kDiagnostics ? kListY + (float) setting_row_ * kRowPitch
                                                              : kListY + (float) setting_category_ * kRailPitch;
    }
    if (age_ <= dt) {
        rows_y_.snap(target);
    }
    rows_y_.target = target;
    rows_y_.update(dt, 22.0f);
    dock_x_.target = (float) page_;
    dock_x_.update(dt, 18.0f);
    const bool decision = !live_.decision.empty();
    hero_.target = decision ? live_.prompt_tps : live_.gen_tps;
    hero_.update(dt, 6.0f);
}

void Dashboard::update_home(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (input.is_pressed(Action::confirm)) {
        open_library();
        feedback.play(Cue::open);
    } else if (input.is_pressed(Action::north)) {
        go_to(kApiPage, &feedback);
    }
}

void Dashboard::update_models(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (market_confirm_ || market_space_) {
        update_market(input, feedback);
        return;
    }
    if (!in_list_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = section_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kSections) {
                section_ = next;
                feedback.play(Cue::focus, 1.0f + 0.04f * (float) next);
                if (section_ == kSearch && !market_searched_) {
                    market_searched_ = true;
                    request_ = { Request::market_search, "" };
                }
            } else if (!input.nav_repeat) {
                feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
            }
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm)) {
            in_list_ = true;
            feedback.play(Cue::select);
        }
        return;
    }
    // Left from a list's top level, or Circle there, goes back to the sections.
    const bool top = !(section_ == kSearch && market_files_);
    if (top && (input.nav == Direction::left || input.is_pressed(Action::back))) {
        in_list_ = false;
        dl_confirm_ = -1;
        feedback.play(Cue::back);
        return;
    }
    switch (section_) {
        case kLocal: update_local(input, feedback); break;
        case kSuggested: update_suggested(input, feedback); break;
        case kSearch: update_market(input, feedback); break;
        default: update_downloads(input, feedback); break;
    }
}

namespace {

// Up and down through a list: true when the cursor moved.
bool walk(const hui::InputFrame & input, int & cursor, int count, ui::Feedback & feedback) {
    if (input.nav != Direction::up && input.nav != Direction::down) {
        return false;
    }
    const int next = cursor + (input.nav == Direction::down ? 1 : -1);
    if (next >= 0 && next < count) {
        cursor = next;
        feedback.play(Cue::focus, 1.0f + 0.015f * (float) std::min(next, 12));
        return true;
    }
    if (!input.nav_repeat) {
        feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
    }
    return false;
}

}  // namespace

void Dashboard::update_local(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int rows = (int) live_.models.size();
    local_cursor_ = std::clamp(local_cursor_, 0, std::max(0, rows - 1));
    walk(input, local_cursor_, rows, feedback);
    if (input.is_pressed(Action::confirm) && rows > 0) {
        const ModelRow & m = live_.models[(size_t) local_cursor_];
        if (m.current) {
            request_ = { Request::unload, m.file };
            feedback.play(Cue::back);
        } else if (m.fits) {
            request_ = { Request::load, m.file };
            feedback.play(Cue::launch);
        } else {
            feedback.play(Cue::error);
        }
    }
}

void Dashboard::update_suggested(const hui::InputFrame & input, ui::Feedback & feedback) {
    walk(input, suggested_cursor_, kSuggestionCount, feedback);
    if (input.is_pressed(Action::confirm)) {
        const Suggestion & s = kSuggestions[suggested_cursor_];
        bool here = false;
        for (const auto & m : live_.models) {
            here = here || m.label + ".gguf" == local_name(s.model);
        }
        if (here) {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
        } else {
            request_ = { Request::suggested_download, settings_.download_dir, suggested_cursor_ };
            feedback.play(Cue::complete);
        }
    }
}

// After the space check: ask about a loaded model first, or start.
void Dashboard::start_download(ui::Feedback & feedback) {
    if (!live_.model_label.empty() && live_.free_gib < 2.0) {
        market_confirm_ = true;  // a model holds the memory: ask first
        feedback.play(Cue::modal_open);
    } else {
        request_ = { Request::market_download, market_dir_, market_.open, market_file_ };
        feedback.play(Cue::complete);
    }
}

void Dashboard::update_market(const hui::InputFrame & input, ui::Feedback & feedback) {
    // The download does not fit where it would go.
    if (market_space_) {
        if (input.is_pressed(Action::confirm) && !market_dir_.empty()) {
            market_space_ = false;
            start_download(feedback);
        } else if (input.is_pressed(Action::back)) {
            market_space_ = false;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    // The question before a download while a model holds the memory.
    if (market_confirm_) {
        if (input.is_pressed(Action::north)) {
            request_ = { Request::market_download_unload, market_dir_, market_.open, market_file_ };
            market_confirm_ = false;
            feedback.play(Cue::complete);
        } else if (input.is_pressed(Action::confirm)) {
            request_ = { Request::market_download, market_dir_, market_.open, market_file_ };
            market_confirm_ = false;
            feedback.play(Cue::complete);
        } else if (input.is_pressed(Action::back)) {
            market_confirm_ = false;
            feedback.play(Cue::modal_close);
        }
        return;
    }
    const int chip = input.is_pressed(Action::jump_next) ? 1 : input.is_pressed(Action::jump_prev) ? -1 : 0;
    if (chip != 0) {
        const int next = market_chip_ + chip;
        if (next >= 0 && next < kMarketChipCount) {
            market_chip_ = next;
            market_repo_ = 0;
            market_files_ = false;
            request_ = { Request::market_search, next == 0 ? std::string() : std::string(kMarketChips[next]) };
            feedback.play(Cue::tab);
        } else {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
        }
        return;
    }
    const int repos = (int) market_.repos.size();
    const MarketRepo * open = market_.open >= 0 && market_.open < repos ? &market_.repos[(size_t) market_.open] : nullptr;
    const int files = open ? (int) open->files.size() : 0;
    int & cursor = market_files_ ? market_file_ : market_repo_;
    if (walk(input, cursor, market_files_ ? files : repos, feedback) && market_files_) {
        request_ = { Request::market_plan, "", market_.open, cursor };
    }
    if (input.is_pressed(Action::confirm)) {
        if (!market_files_ && cursor < repos) {
            request_ = { Request::market_open, "", cursor, -1 };
            market_files_ = true;
            market_file_ = 0;
            feedback.play(Cue::open);
        } else if (market_files_ && open && cursor < files) {
            const MarketFile & f = open->files[(size_t) cursor];
            const double need = f.bytes / 1073741824.0 + 1.0;  // and 1 GiB spare, as Market::download wants
            const Drive * to = nullptr;
            const Drive * other = nullptr;  // the roomiest other drive it fits on
            for (const auto & d : live_.drives) {
                if (d.models == settings_.download_dir) {
                    to = &d;
                } else if (d.free >= need && (!other || d.free > other->free)) {
                    other = &d;
                }
            }
            market_dir_.clear();
            if (f.fits == 0) {
                feedback.play(Cue::error);  // too big
            } else if (!to || (to->total > 0 && to->free < need)) {  // unknown space (no exporter) does not block
                market_space_ = true;
                market_dir_ = other ? other->models : "";
                feedback.play(Cue::modal_open);
            } else {
                start_download(feedback);
            }
        }
    }
    // Square: read this file's header now and say whether it fits.
    if (input.is_pressed(Action::west) && market_files_ && open && cursor < files) {
        const MarketFile & f = open->files[(size_t) cursor];
        if (f.fits == -1 || f.fits == -2) {
            request_ = { Request::market_plan, "", market_.open, cursor };
            feedback.play(Cue::select);
        } else {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);  // checked already
        }
    }
    if (input.is_pressed(Action::back) && market_files_) {
        market_files_ = false;
        feedback.play(Cue::back);
    }
    // The first file of a freshly opened repository gets planned too.
    if (request_.kind == Request::none && market_files_ && files > 0 && market_file_ == 0 && open->files[0].fits == -1 &&
        !open->files[0].checking && !market_.busy) {
        request_ = { Request::market_plan, "", market_.open, 0 };
    }
}

void Dashboard::update_downloads(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int rows = (int) market_.downloads.size();
    dl_cursor_ = std::clamp(dl_cursor_, 0, std::max(0, rows - 1));
    if (walk(input, dl_cursor_, rows, feedback)) {
        dl_confirm_ = -1;
    }
    if (rows == 0) {
        return;
    }
    const Download & d = market_.downloads[(size_t) dl_cursor_];
    if (input.is_pressed(Action::confirm)) {
        if (d.state == DownloadState::running || d.state == DownloadState::retrying || d.state == DownloadState::queued) {
            request_ = { Request::download_pause, "", dl_cursor_ };
            feedback.play(Cue::select);
        } else if (d.state == DownloadState::paused || d.state == DownloadState::failed) {
            request_ = { Request::download_resume, "", dl_cursor_ };
            feedback.play(Cue::complete);
        } else {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    // Square: a finished one leaves the list; any other is cancelled and its
    // partial file deleted, after a second press.
    if (input.is_pressed(Action::west)) {
        if (d.state == DownloadState::done || dl_confirm_ == dl_cursor_) {
            request_ = { Request::download_remove, "", dl_cursor_ };
            dl_confirm_ = -1;
            feedback.play(Cue::back);
        } else {
            dl_confirm_ = dl_cursor_;
            feedback.play(Cue::modal_open);
        }
    }
}

void Dashboard::update_settings(const hui::InputFrame & input, ui::Feedback & feedback) {
    if (!in_rows_) {
        if (input.nav == Direction::up || input.nav == Direction::down) {
            const int next = setting_category_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < kCategoryCount) {
                setting_category_ = next;
                setting_row_ = 0;
                log_scroll_ = 0;
                feedback.play(Cue::focus, 1.0f + 0.04f * (float) next);
            } else if (!input.nav_repeat) {
                feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
            }
        }
        if (input.nav == Direction::right || input.is_pressed(Action::confirm)) {
            in_rows_ = true;
            feedback.play(Cue::select);
        }
        return;
    }
    if (input.is_pressed(Action::back) || input.nav == Direction::left) {
        in_rows_ = false;
        feedback.play(Cue::back);
        return;
    }
    if (setting_category_ == kDiagnostics) {
        update_logs(input, feedback);
        return;
    }
    const SettingCategory & c = kCategories[setting_category_];
    walk(input, setting_row_, c.count, feedback);
    const Setting id = c.rows[setting_row_].id;
    if (id == Setting::clear_scratch) {
        if (input.is_pressed(Action::confirm)) {
            request_ = { Request::clear_scratch, "" };
            feedback.play(Cue::saved);
        }
        return;
    }
    int dir = input.nav == Direction::right ? 1 : 0;
    if (is_toggle(id) && input.is_pressed(Action::confirm)) {
        dir = 1;
    }
    if (dir == 0 && input.is_pressed(Action::west)) {
        dir = -1;  // Square steps back, as left once did
    }
    if (is_toggle(id) && input.nav == Direction::right && toggle_value(settings_, id)) {
        dir = 0;  // right turns a switch on; it is on
    }
    if (dir != 0 && step_setting(settings_, id, dir, live_.models, live_.drives)) {
        request_ = { Request::settings, "" };
        feedback.play(is_toggle(id) ? Cue::toggle : Cue::slider, 1.0f + 0.05f * (float) dir);
    } else if (dir != 0 && !input.nav_repeat) {
        feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
    }
}

void Dashboard::update_logs(const hui::InputFrame & input, ui::Feedback & feedback) {
    const int most = std::max(0, (int) log_view().size() - kLogLines);
    if (input.nav == Direction::up || input.nav == Direction::down) {
        const int next = std::clamp(log_scroll_ + (input.nav == Direction::up ? 3 : -3), 0, most);
        if (next != log_scroll_) {
            log_scroll_ = next;
            feedback.play(Cue::focus, 1.0f, 0.0f, 0.4f);
        } else if (!input.nav_repeat) {
            feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    if (input.is_pressed(Action::west)) {
        log_llama_ = !log_llama_;
        log_scroll_ = 0;
        feedback.play(Cue::select);
    }
    if (input.is_pressed(Action::north)) {
        log_errors_ = !log_errors_;
        refilter_logs();
        log_scroll_ = 0;
        feedback.play(Cue::toggle);
    }
    if (input.is_pressed(Action::confirm)) {
        log_scroll_ = 0;  // back to the newest
        feedback.play(Cue::select);
    }
}

// ---- drawing: the frame ---------------------------------------------------------------

void Dashboard::draw(DashboardFrame & frame) const {
    frame.backdrop.mode = gfx::BackdropMode::gradient;
    frame.backdrop.colors[0] = Color::rgb(0x151618);
    frame.backdrop.colors[1] = kBase;
    frame.backdrop.colors[2] = kBase;
    frame.backdrop.time = clock_;

    gfx::DrawList & scene = frame.scene;
    // A page fades in and settles from a little below, the way a card rises
    // out of a car's dock.
    const float in = tween::cubic_out(tween::clamp01(page_age_ / 0.32f));
    scene.push_transform(1.0f, 960, 540, 0, 18.0f * (1.0f - in));
    scene.push_opacity(in);
    switch (page_) {
        case kHomePage: draw_home(scene); break;
        case kModelsPage: draw_models(scene); break;
        case kApiPage: draw_api(scene); break;
        default: draw_settings(scene); break;
    }
    scene.pop_opacity();
    scene.pop_transform();
    draw_status(scene);
    draw_dock(scene);
    draw_hints(scene);
    draw_dialogs(frame.overlay);
    if (busy()) {
        draw_busy(frame.overlay);
    }
}

void Dashboard::draw_status(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const float y = 72;
    float w = put(l, f.semibold, "PS5", kLeft, y, 28, kInk);
    put(l, f.semibold, "LM", kLeft + w, y, 28, kBlue);
    // Right to left: how long it has been up, power, temperature.
    std::vector<std::string> items;
    if (live_.soc_temp > -100) {
        items.push_back(format("%.0f\xC2\xB0" "C", live_.soc_temp));
    }
    if (live_.soc_power_w > 0) {
        items.push_back(format("%.0f W", live_.soc_power_w));
    }
    if (live_.uptime_s > 0) {
        const double m = live_.uptime_s / 60.0;
        items.push_back(m < 60 ? format("Up %.0f min", std::floor(m)) : format("Up %.1f h", m / 60.0));
    }
    float x = kRight;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        x -= put(l, f.regular, *it, x, y, 22, kInk2, gfx::Align::right) + 40;
    }
    put(l, f.regular, "v" PS5LM_VERSION, x, y, 22, kInk3, gfx::Align::right);
}

void Dashboard::draw_dock(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    l.line(kLeft, kDockY, kRight, kDockY, 1.5f, kHair);
    constexpr float pitch = 168;
    for (int i = 0; i < kPages; ++i) {
        const float cx = kLeft + 44 + pitch * (float) i;
        const float lit = tween::clamp01(1.0f - std::fabs(dock_x_.value - (float) i));
        const Color c = gfx::mix(kInk3, kInk, lit);
        icon(l, i, cx, 1004, c);
        put(l, f.regular, kPageNames[i], cx, 1056, 20, c, gfx::Align::center);
    }
    // The current tab's marker slides between them.
    const float mx = kLeft + 44 + pitch * dock_x_.value;
    l.rounded_rect({ mx - 18, 1068, 36, 4 }, 2, kBlue);
}

void Dashboard::draw_hints(gfx::DrawList & l) const {
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 20;
    layout.cy = 1018;
    layout.item_gap = 36;
    std::vector<ui::Hint> h;
    if (market_confirm_ || market_space_) {
        return;  // the dialog has its own
    }
    if (page_ == kHomePage) {
        h = { { ui::Button::l1, "Pages", ui::Button::r1 }, { ui::Button::triangle, "API" },
              { ui::Button::cross, live_.model_label.empty() ? "Choose a model" : "Change model" } };
    } else if (page_ == kModelsPage) {
        if (!in_list_) {
            h = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, "Open" } };
        } else if (section_ == kLocal) {
            const ModelRow * m = local_cursor_ < (int) live_.models.size() ? &live_.models[(size_t) local_cursor_] : nullptr;
            h = { { ui::Button::circle, "Back" } };
            if (m && (m->current || m->fits)) {
                h.push_back({ ui::Button::cross, m->current ? "Unload" : "Load" });
            }
        } else if (section_ == kSuggested) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Download" } };
        } else if (section_ == kSearch) {
            h = market_files_ ? std::vector<ui::Hint>{ { ui::Button::circle, "Back" }, { ui::Button::square, "Check fit" },
                                                       { ui::Button::cross, "Download" } }
                              : std::vector<ui::Hint>{ { ui::Button::l2, "Search", ui::Button::r2 }, { ui::Button::circle, "Back" },
                                                       { ui::Button::cross, "Open" } };
        } else {
            const Download * d = dl_cursor_ < (int) market_.downloads.size() ? &market_.downloads[(size_t) dl_cursor_] : nullptr;
            h = { { ui::Button::circle, "Back" } };
            if (d) {
                const bool runs = d->state == DownloadState::running || d->state == DownloadState::retrying ||
                                  d->state == DownloadState::queued;
                h.push_back({ ui::Button::square, d->state == DownloadState::done ? "Clear" : dl_confirm_ == dl_cursor_ ? "Yes, delete it" : "Cancel" });
                if (d->state != DownloadState::done) {
                    h.push_back({ ui::Button::cross, runs ? "Pause" : d->state == DownloadState::failed ? "Try again" : "Resume" });
                }
            }
        }
    } else if (page_ == kApiPage) {
        h = { { ui::Button::l1, "Pages", ui::Button::r1 } };
    } else if (!in_rows_) {
        h = { { ui::Button::dpad, "Choose" }, { ui::Button::cross, "Open" } };
    } else if (setting_category_ == kDiagnostics) {
        h = { { ui::Button::dpad, "Scroll" }, { ui::Button::square, log_llama_ ? "App log" : "Server log" },
              { ui::Button::triangle, log_errors_ ? "All lines" : "Errors only" } };
    } else {
        const Setting id = kCategories[setting_category_].rows[setting_row_].id;
        if (id == Setting::clear_scratch) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Clear" } };
        } else if (is_toggle(id)) {
            h = { { ui::Button::circle, "Back" }, { ui::Button::cross, "Switch" } };
        } else {
            h = { { ui::Button::circle, "Back" }, { ui::Button::square, "Less" }, { ui::Button::dpad, "More" } };
        }
    }
    ui::draw_hints(l, fonts_, style, h.data(), (int) h.size(), kRight, true, layout);
}

// ---- drawing: Home -------------------------------------------------------------------

void Dashboard::draw_home(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const bool loaded = !live_.model_label.empty();
    const bool decision = !live_.decision.empty();

    // Left: the model.
    if (!loaded || live_.state == ServerState::no_model) {
        put(l, f.regular, "Nothing is loaded", kLeft, 260, 26, kInk2);
        put(l, f.regular, "Choose a model", kLeft, 340, 64, kInk);
        put(l, f.regular, "It runs on the PS5's GPU and answers every device on your network.", kLeft, 400, 24, kInk2);
        put(l, f.regular, format("%.0f models on this PS5", (double) live_.models.size()), kLeft, 444, 24, kInk3);
    } else {
        std::string name, build;
        split_name(live_.model_label, &name, &build);
        put(l, f.regular, decision ? "Decision model" : "Language model", kLeft, 250, 26, kInk2);
        put(l, f.regular, f.regular.font->fit(name, 64, 860), kLeft, 326, 64, kInk);
        std::string detail = build;
        if (live_.ctx > 0) {
            detail += (detail.empty() ? "" : "    ") + format("%.0fk context", live_.ctx / 1024.0);
        }
        detail += (detail.empty() ? "" : "    ") + gb(live_.model_gib);
        put(l, f.regular, detail, kLeft, 372, 24, kInk3);

        // The state, as a car shows its gear: one word, coloured only when it matters.
        const char * word = live_.state == ServerState::loading ? "Loading"
                          : live_.state == ServerState::generating ? (decision ? "Deciding" : "Generating")
                          : live_.state == ServerState::failed ? "Stopped" : "Ready";
        const Color dot = live_.state == ServerState::failed ? kRed
                        : live_.state == ServerState::generating ? kBlue
                        : live_.state == ServerState::loading ? kInk2.with_alpha(0.5f + 0.5f * std::sin(clock_ * 4.0f)) : kGreen;
        l.circle(kLeft + 7, 432, 7, dot);
        put(l, f.regular, word, kLeft + 26, 441, 26, kInk);

        // The large number: speed while serving, progress while loading.
        float big = 0;
        std::string unit;
        bool known = true;
        if (live_.state == ServerState::loading) {
            big = std::max(0.0f, live_.load_progress) * 100.0f;
            unit = "% on the GPU";
        } else if (decision) {
            big = hero_.value;
            unit = "tokens read per second";
            known = live_.prompt_tps > 0;
        } else {
            big = hero_.value;
            unit = "tokens per second";
            known = live_.gen_tps > 0;
            if (!known) {
                // Idle: the last reply's speed, quieter.
                for (auto it = live_.gen_history.rbegin(); it != live_.gen_history.rend(); ++it) {
                    if (*it > 0) {
                        big = *it;
                        unit = "tokens per second, last reply";
                        known = true;
                        break;
                    }
                }
            }
        }
        const bool quiet = live_.state != ServerState::generating && live_.state != ServerState::loading;
        const std::string number = !known ? kDash : big >= 100 || live_.state == ServerState::loading ? format("%.0f", big) : format("%.1f", big);
        const float nw = put(l, f.display, number, kLeft - 8, 660, 232, quiet ? kInk2 : kInk);
        put(l, f.regular, unit, kLeft + nw + 12, 660, 26, kInk2);

        if (live_.state == ServerState::loading) {
            meter(l, kLeft, 712, 860, live_.load_progress, kInk);
        } else if (!decision) {
            trace(l, live_.gen_history, { kLeft, 702, 860, 100 }, kInk.with_alpha(0.75f));
        }

        // Two quiet meters: what the model holds, and how much of its context is used.
        const double used = live_.pool_gib - live_.free_gib;
        const float my = 868;
        put(l, f.regular, "Memory", kLeft, my, 22, kInk2);
        put(l, f.regular, format("%.1f", used) + " of " + gb(live_.pool_gib), kLeft + 400, my, 22, kInk,
             gfx::Align::right);
        meter(l, kLeft, my + 16, 400, live_.pool_gib > 0 ? (float) (used / live_.pool_gib) : 0, kInk.with_alpha(0.8f));
        put(l, f.regular, "Context", kLeft + 460, my, 22, kInk2);
        put(l, f.regular, thousands(live_.ctx_used) + " of " + format("%.0fk", live_.ctx / 1024.0), kLeft + 860, my, 22, kInk,
             gfx::Align::right);
        meter(l, kLeft + 460, my + 16, 400, live_.ctx > 0 ? (float) live_.ctx_used / (float) live_.ctx : 0, kInk.with_alpha(0.8f));
    }

    // Right: where the model is reached, and what the network asks of it, as
    // it happens. A single panel, as a car gives the map.
    const Rect p{ 1056, 196, kRight - 1056, 728 };
    l.rounded_rect(p, 24, kSurface);
    const float x = p.x + 56, w = p.w - 112;
    put(l, f.regular, "On your network", x, p.y + 70, 22, kInk2);
    const std::string url = live_.address.empty() ? std::string("Not connected") : "http://" + live_.address;
    put(l, f.regular, f.regular.font->fit(url, 40, w), x, p.y + 124, 40, live_.address.empty() ? kInk3 : kInk);
    put(l, f.regular,
        !loaded ? "Load a model and it answers here"
        : decision ? "TypeSafe decisions at /v1/systemone"
                   : "OpenAI, Anthropic and a chat page",
        x, p.y + 162, 22, kInk3);
    l.line(x, p.y + 196, x + w, p.y + 196, 1.5f, kHair);

    // The latest requests, newest first: who asked, what, and the answer as it arrives.
    float y = p.y + 240;
    const float bottom = p.y + p.h - 72;
    if (live_.activity.empty()) {
        put(l, f.regular, "Requests from other devices appear here as they arrive.", x, y + 8, 22, kInk3);
    }
    for (auto it = live_.activity.rbegin(); it != live_.activity.rend() && y < bottom - 90; ++it) {
        const Exchange & e = *it;
        const bool running = e.status == 0;
        const bool fault = e.status >= 400;
        put(l, f.regular, e.client + "    " + e.path, x, y, 19, kInk3);
        if (running) {
            const float tw = put(l, f.regular, "Answering", x + w, y, 19, kInk2, gfx::Align::right);
            l.circle(x + w - tw - 14, y - 7, 5, kBlue.with_alpha(0.55f + 0.45f * std::sin(clock_ * 6.0f)));
        } else {
            put(l, f.regular, fault ? format("Error %.0f", (double) e.status) : format(e.seconds < 10 ? "%.1f s" : "%.0f s", e.seconds),
                x + w, y, 19, fault ? kRed : kInk3, gfx::Align::right);
        }
        y += 34;
        put(l, f.regular, f.regular.font->fit(e.asked.empty() ? std::string(kDash) : e.asked, 22, w), x, y, 22, kInk2);
        y += 34;
        // While it streams, the answer's last lines, so it reads as it is
        // written; once done, its first ones.
        std::vector<std::string> lines = wrap(f.regular, e.answered, 22, w);
        const int keep = it == live_.activity.rbegin() ? 3 : 1;
        if ((int) lines.size() > keep) {
            if (running) {
                lines.erase(lines.begin(), lines.end() - keep);
            } else {
                lines.resize((size_t) keep);
                lines.back() = f.regular.font->fit(lines.back() + " \xE2\x80\xA6", 22, w);
            }
        }
        if (lines.empty()) {
            lines.push_back(running ? "" : kDash);
        }
        for (const auto & line : lines) {
            if (y > bottom) {
                break;
            }
            put(l, f.regular, line, x, y, 22, fault ? kRed : kInk);
            y += 31;
        }
        y += 30;
    }

    // What it has done since it started, like a trip meter.
    std::string trip = thousands(live_.served) + " requests    " + thousands(live_.prompt_tokens) + " tokens in";
    if (!decision) {
        trip += "    " + thousands(live_.tokens) + " out";
    }
    put(l, f.regular, trip, x, p.y + p.h - 40, 20, kInk3);
}

// ---- drawing: Models ------------------------------------------------------------------

void Dashboard::draw_models(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    put(l, f.semibold, "Models", kLeft, kTitleY, 44, kInk);

    // The highlight: on a section, or on a row of the list.
    const float hx = in_list_ ? kListX : kLeft - 20;
    const float hw = in_list_ ? kRight - kListX : kRailW;
    const float hh = in_list_ ? kRowH : kRailH;
    l.rounded_rect({ hx, rows_y_.value, hw, hh }, 14, kRaised);

    for (int i = 0; i < kSections; ++i) {
        const float y = kListY + kRailPitch * (float) i;
        const bool on = i == section_;
        if (on) {
            l.rounded_rect({ kLeft - 20, y + 16, 4, kRailH - 32 }, 2, kBlue);
        }
        put(l, f.regular, kSectionNames[i], kLeft, y + 42, 26, on ? kInk : kInk2);
        std::string count;
        if (i == kLocal) {
            count = std::to_string(live_.models.size());
        } else if (i == kDownloads && !market_.downloads.empty()) {
            int active = 0;
            for (const auto & d : market_.downloads) {
                active += d.state != DownloadState::done;
            }
            count = active ? std::to_string(active) : "";
        }
        put(l, f.regular, count, kLeft + kRailW - 44, y + 42, 22, kInk3, gfx::Align::right);
    }

    const Rect area{ kListX, kListY, kRight - kListX, kDockY - 40 - kListY };
    switch (section_) {
        case kLocal: draw_local(l, area); break;
        case kSuggested: draw_suggested(l, area); break;
        case kSearch: draw_market(l, area); break;
        default: draw_downloads(l, area); break;
    }
}

void Dashboard::draw_local(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int rows = (int) live_.models.size();
    if (rows == 0) {
        put(l, f.regular, "No models yet", a.x + 28, a.y + 46, 30, kInk);
        put(l, f.regular, "Get one from Suggested, or copy a .gguf to /data/PS5LM/models.", a.x + 28, a.y + 90, 22, kInk2);
        return;
    }
    const int first = first_visible(local_cursor_, rows);
    for (int i = first; i < std::min(rows, first + kVisibleRows); ++i) {
        const ModelRow & m = live_.models[(size_t) i];
        const float y = a.y + kRowPitch * (float) (i - first);
        std::string name, build;
        split_name(m.label, &name, &build);
        const float nw = put(l, f.regular, f.regular.font->fit(name, 26, 640), a.x + 28, y + 36, 26, m.fits ? kInk : kInk3);
        put(l, f.regular, build, a.x + 28 + nw + 14, y + 36, 21, kInk3);
        std::string meta = (m.decision.empty() ? "Language" : "Decision") + std::string("    ") + gb(m.size_gib);
        if (m.drive != "Internal storage" && !m.drive.empty()) {
            meta += "    " + m.drive;
        }
        std::string plan = gb_text(m.plan);
        if (plan.rfind("decision model (", 0) == 0 && plan.find("), ") != std::string::npos) {
            plan = plan.substr(plan.find("), ") + 3);  // the kind is said already
        }
        meta += "    " + plan;
        put(l, f.regular, f.regular.font->fit(meta, 20, a.w - 260), a.x + 28, y + 66, 20, kInk3);
        const char * state = m.current ? "Running" : m.fits ? "" : "Too big";
        put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, m.current ? kBlue : kInk3, gfx::Align::right);
    }
}

void Dashboard::draw_suggested(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int first = first_visible(suggested_cursor_, kSuggestionCount);
    for (int i = first; i < std::min(kSuggestionCount, first + kVisibleRows); ++i) {
        const Suggestion & s = kSuggestions[i];
        const float y = a.y + kRowPitch * (float) (i - first);
        const float nw = put(l, f.regular, s.name, a.x + 28, y + 36, 26, kInk);
        const double bytes = (double) s.model.bytes + (double) (s.mmproj.file ? s.mmproj.bytes : 0);
        put(l, f.regular, format("%.1f GB", bytes / 1073741824.0), a.x + 28 + nw + 14, y + 36, 21, kInk3);
        const std::string about = std::string(s.decision ? "Decision" : s.mmproj.file ? "Language, images" : "Language") + "    " + s.about;
        put(l, f.regular, f.regular.font->fit(about, 20, a.w - 260), a.x + 28, y + 66, 20, kInk3);
        // On the console already, on its way, or ready to get.
        std::string state;
        Color c = kInk3;
        for (const auto & m : live_.models) {
            if (m.label + ".gguf" == local_name(s.model)) {
                state = m.current ? "Running" : "On this PS5";
                c = m.current ? kBlue : kInk3;
            }
        }
        for (const auto & d : market_.downloads) {
            if (state.empty() && d.repo == s.repo && d.file == s.model.file && d.state != DownloadState::done) {
                state = d.bytes ? format("%.0f%%", 100.0 * (double) d.done / (double) d.bytes) : "Waiting";
                c = kInk2;
            }
        }
        if (state.empty() && in_list_ && i == suggested_cursor_) {
            state = "Get";
            c = kBlue;
        }
        put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, c, gfx::Align::right);
    }
}

void Dashboard::draw_market(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    // The searches, as tabs.
    float x = a.x + 28;
    for (int i = 0; i < kMarketChipCount; ++i) {
        const bool on = i == market_chip_;
        const float w = put(l, f.regular, kMarketChips[i], x, a.y + 34, 22, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ x, a.y + 48, w, 3 }, 1.5f, kBlue);
        }
        x += w + 36;
    }
    put(l, f.regular, market_.busy ? "Searching" : market_.status, a.x + a.w - 28, a.y + 34, 20, kInk3, gfx::Align::right);
    const float top = a.y + 72;
    const int repos = (int) market_.repos.size();
    const MarketRepo * open = market_.open >= 0 && market_.open < repos ? &market_.repos[(size_t) market_.open] : nullptr;
    if (!market_files_) {
        const int first = first_visible(market_repo_, repos);
        for (int i = first; i < std::min(repos, first + kVisibleRows); ++i) {
            const MarketRepo & r = market_.repos[(size_t) i];
            const float y = top + kRowPitch * (float) (i - first);
            const size_t slash = r.id.find('/');
            put(l, f.regular, f.regular.font->fit(slash == std::string::npos ? r.id : r.id.substr(slash + 1), 26, a.w - 300), a.x + 28,
                 y + 36, 26, kInk);
            put(l, f.regular, (slash == std::string::npos ? std::string() : r.id.substr(0, slash)) + "    " +
                                   thousands(r.downloads) + " downloads", a.x + 28, y + 66, 20, kInk3);
        }
        if (repos == 0 && !market_.busy) {
            put(l, f.regular, "Nothing found", a.x + 28, top + 46, 26, kInk2);
        }
        return;
    }
    if (!open) {
        return;
    }
    put(l, f.regular, open->id, a.x + a.w - 28, top - 4, 20, kInk2, gfx::Align::right);
    const int files = (int) open->files.size();
    const int first = first_visible(market_file_, files);
    for (int i = first; i < std::min(files, first + kVisibleRows); ++i) {
        const MarketFile & m = open->files[(size_t) i];
        const float y = top + kRowPitch * (float) (i - first);
        put(l, f.regular, f.regular.font->fit(m.name, 26, a.w - 300), a.x + 28, y + 36, 26, m.fits == 0 ? kInk3 : kInk);
        const std::string plan = m.checking ? "Reading its header" : m.fits == -1 ? "Square checks whether it fits" : m.plan;
        put(l, f.regular, f.regular.font->fit(format("%.1f GB", m.bytes / 1073741824.0) + "    " + gb_text(plan), 20, a.w - 300), a.x + 28, y + 66,
             20, kInk3);
        put(l, f.semibold, m.fits == 1 ? "Fits" : m.fits == 0 ? "Too big" : "", a.x + a.w - 28, y + 50, 22,
             m.fits == 1 ? kInk2 : kInk3, gfx::Align::right);
    }
}

void Dashboard::draw_downloads(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    const int rows = (int) market_.downloads.size();
    if (rows == 0) {
        put(l, f.regular, "No downloads", a.x + 28, a.y + 46, 30, kInk);
        put(l, f.regular, "Models you get from Suggested or Search wait here until they are saved.", a.x + 28, a.y + 90, 22, kInk2);
        return;
    }
    const int first = first_visible(dl_cursor_, rows);
    for (int i = first; i < std::min(rows, first + kVisibleRows); ++i) {
        const Download & d = market_.downloads[(size_t) i];
        const float y = a.y + kRowPitch * (float) (i - first);
        put(l, f.regular, f.regular.font->fit(d.file, 26, a.w - 320), a.x + 28, y + 34, 26,
             d.state == DownloadState::done ? kInk2 : kInk);
        const bool fault = d.state == DownloadState::failed;
        put(l, f.regular, f.regular.font->fit(download_line(d), 20, a.w - 320), a.x + 28, y + 64, 20, fault ? kRed : kInk3);
        const char * state = d.state == DownloadState::running ? "" : d.state == DownloadState::paused ? "Paused"
                           : d.state == DownloadState::failed ? "Failed" : d.state == DownloadState::done ? "Saved"
                           : d.state == DownloadState::retrying ? "Retrying" : "Waiting";
        if (d.state == DownloadState::running && d.bytes) {
            const float share = (float) ((double) d.done / (double) d.bytes);
            meter(l, a.x + a.w - 248, y + 42, 220, share, kBlue);
        } else {
            put(l, f.semibold, state, a.x + a.w - 28, y + 50, 22, fault ? kRed : kInk3, gfx::Align::right);
        }
    }
}

// ---- drawing: API -----------------------------------------------------------------

void Dashboard::draw_api(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const bool decision = !live_.decision.empty();
    const bool loaded = !live_.model_label.empty();
    put(l, f.semibold, "API", kLeft, kTitleY, 44, kInk);
    const std::string base = live_.address.empty() ? std::string("http://<PS5 address>:8081") : "http://" + live_.address;
    put(l, f.regular, "Base address", kLeft, 262, 24, kInk2);
    put(l, f.regular, base + "/v1", kLeft, 316, 44, kInk);
    put(l, f.regular, "Any device on this network can call it, with any model name: the loaded one answers.", kLeft, 360, 22,
         kInk3);

    struct Ep { const char * method; const char * path; const char * about; bool decision; };
    const Ep all[] = {
        { "POST", "/v1/chat/completions", "OpenAI chat, streaming and tools", false },
        { "POST", "/v1/messages", "Anthropic Messages, for Claude Code and its SDKs", false },
        { "POST", "/v1/completions", "Plain text completion", false },
        { "POST", "/v1/systemone", "TypeSafe decisions: choice, score and yes-or-no questions", true },
        { "GET", "/v1/models", "The loaded model", false },
        { "GET", "/health", "Ready or loading", false },
    };
    float y = 452;
    for (const auto & e : all) {
        const bool here = loaded && (e.decision == decision || std::string(e.method) == "GET");
        put(l, f.mono, e.method, kLeft, y, 24, kInk3);
        put(l, f.mono, e.path, kLeft + 96, y, 24, here ? kInk : kInk3);
        put(l, f.regular, e.about, kLeft + 96, y + 34, 20, here ? kInk2 : kInk3);
        y += 84;
    }

    // An example for what is loaded, in a quiet panel.
    const Rect p{ 1000, 236, kRight - 1000, 688 };
    l.rounded_rect(p, 24, kSurface);
    put(l, f.regular, "Try it", p.x + 48, p.y + 68, 24, kInk2);
    std::vector<std::string> code;
    if (decision) {
        code = { "curl " + base + "/v1/systemone \\", "  -H 'Content-Type: application/json' \\", "  -d '{", "    \"state\": \"Refund me now or I cancel.\",",
                 "    \"questions\": {", "      \"angry\": {", "        \"type\": \"noul\",", "        \"instructions\": \"Is the customer angry?\"",
                 "      }", "    }", "  }'" };
    } else {
        code = { "curl " + base + "/v1/chat/completions \\", "  -H 'Content-Type: application/json' \\", "  -d '{", "    \"messages\": [",
                 "      {\"role\": \"user\",", "       \"content\": \"Hello from my PS5\"}", "    ]", "  }'" };
    }
    float cy = p.y + 130;
    for (const auto & line : code) {
        put(l, f.mono, f.mono.font->fit(line, 21, p.w - 96), p.x + 48, cy, 21, kInk);
        cy += 34;
    }
    l.line(p.x + 48, p.y + p.h - 150, p.x + p.w - 48, p.y + p.h - 150, 1.5f, kHair);
    put(l, f.regular, decision ? "Ollaya and TypeSafe clients:" : "OpenAI SDKs:", p.x + 48, p.y + p.h - 100, 22, kInk2);
    put(l, f.mono, decision ? "base_url=" + base : "base_url=" + base + "/v1", p.x + 48 + (decision ? 300 : 170), p.y + p.h - 100, 21, kInk);
    put(l, f.regular, decision ? "" : "Claude Code:", p.x + 48, p.y + p.h - 56, 22, kInk2);
    put(l, f.mono, decision ? "" : "ANTHROPIC_BASE_URL=" + base, p.x + 218, p.y + p.h - 56, 21, kInk);
}

// ---- drawing: Settings ----------------------------------------------------------------

void Dashboard::draw_settings(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    put(l, f.semibold, "Settings", kLeft, kTitleY, 44, kInk);
    const bool rows = in_rows_ && setting_category_ != kDiagnostics;
    l.rounded_rect({ rows ? kListX : kLeft - 20, rows_y_.value, rows ? kRight - kListX : kRailW, rows ? kRowH : kRailH }, 14,
                   in_rows_ && setting_category_ == kDiagnostics ? kSurface : kRaised);
    for (int i = 0; i < kCategoryCount; ++i) {
        const float y = kListY + kRailPitch * (float) i;
        const bool on = i == setting_category_;
        if (on) {
            l.rounded_rect({ kLeft - 20, y + 16, 4, kRailH - 32 }, 2, kBlue);
        }
        put(l, f.regular, kCategories[i].name, kLeft, y + 42, 26, on ? kInk : kInk2);
    }
    if (setting_category_ == kDiagnostics) {
        draw_logs(l, { kListX, kListY, kRight - kListX, kDockY - 40 - kListY });
        return;
    }
    const SettingCategory & c = kCategories[setting_category_];
    for (int i = 0; i < c.count; ++i) {
        const SettingRow & r = c.rows[i];
        const float y = kListY + kRowPitch * (float) i;
        put(l, f.regular, r.label, kListX + 28, y + 36, 26, kInk);
        put(l, f.regular, r.help, kListX + 28, y + 66, 20, kInk3);
        const float cy = y + kRowH * 0.5f;
        if (is_toggle(r.id)) {
            toggle_switch(l, kRight - 28, cy, toggle_value(settings_, r.id));
        } else if (r.id != Setting::clear_scratch) {
            const bool focus = in_rows_ && i == setting_row_;
            put(l, f.regular, setting_text(settings_, r.id, live_.models, live_.drives), kRight - 64, y + 50, 24,
                 focus ? kInk : kInk2, gfx::Align::right);
            if (focus) {
                put(l, f.regular, "\xE2\x80\xBA", kRight - 34, y + 50, 30, kInk2);  // a chevron: there is more
            }
        } else if (live_.scratch_gib > 0) {
            put(l, f.regular, format("%.1f GB in it", live_.scratch_gib), kRight - 28, y + 50, 22, kInk2,
                 gfx::Align::right);
        }
    }
}

void Dashboard::draw_logs(gfx::DrawList & l, const Rect & a) const {
    const ui::Fonts & f = fonts_;
    float x = a.x + 28;
    const char * tabs[] = { "App", "Server" };
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 1) == log_llama_;
        const float w = put(l, f.regular, tabs[i], x, a.y + 40, 24, on ? kInk : kInk3);
        if (on) {
            l.rounded_rect({ x, a.y + 54, w, 3 }, 1.5f, kBlue);
        }
        x += w + 36;
    }
    put(l, f.regular, log_errors_ ? "Errors and warnings only" : "Every line", a.x + a.w - 28, a.y + 40, 20, kInk3,
         gfx::Align::right);
    const auto & v = log_view();
    const int last = (int) v.size() - log_scroll_;
    const int first = std::max(0, last - kLogLines);
    float y = a.y + 96;
    for (int i = first; i < last; ++i) {
        const int level = log_level(v[(size_t) i]);
        put(l, f.mono, f.mono.font->fit(v[(size_t) i], 18, a.w - 56), a.x + 28, y, 18,
             level == 2 ? kRed : level == 1 ? kInk : kInk2);
        y += kLogLineH;
    }
    if (v.empty()) {
        put(l, f.regular, "Nothing here yet", a.x + 28, a.y + 120, 24, kInk3);
    }
}

// ---- drawing: dialogs and the busy sheet -------------------------------------------------

namespace {

void sheet(gfx::DrawList & l, const Rect & r) {
    l.rounded_rect({ 0, 0, 1920, 1080 }, 0, kBlack.with_alpha(0.62f));
    l.shadow({ r.x, r.y + 16, r.w, r.h }, 24, 48, kBlack.with_alpha(0.5f));
    l.rounded_rect(r, 24, kSurface);
}

}  // namespace

void Dashboard::draw_dialogs(gfx::DrawList & l) const {
    if (!(page_ == kModelsPage && (market_confirm_ || market_space_))) {
        return;
    }
    const ui::Fonts & f = fonts_;
    const Rect r{ 960 - 440, 540 - 170, 880, 340 };
    sheet(l, r);
    ui::GlyphStyle style = ui::GlyphStyle::dark();
    style.label = kInk2;
    ui::HintLayout layout;
    layout.size = 32;
    layout.text_size = 22;
    layout.cy = r.y + r.h - 56;
    if (market_confirm_) {
        put(l, f.semibold, "Unload the model first?", r.x + 56, r.y + 86, 34, kInk);
        put(l, f.regular, "The download is faster with the memory free. The model can load again after.", r.x + 56, r.y + 136, 22, kInk2);
        const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Keep it loaded" }, { ui::Button::triangle, "Unload and download" } };
        ui::draw_hints(l, fonts_, style, h, 3, r.x + r.w - 56, true, layout);
    } else {
        put(l, f.semibold, "Not enough space there", r.x + 56, r.y + 86, 34, kInk);
        put(l, f.regular, market_dir_.empty() ? "No drive has room for it. Free some space and try again."
                                               : "It fits on " + drive_name(market_dir_) + ".",
             r.x + 56, r.y + 136, 22, kInk2);
        const ui::Hint h[] = { { ui::Button::circle, "Cancel" }, { ui::Button::cross, "Save it there" } };
        ui::draw_hints(l, fonts_, style, h, market_dir_.empty() ? 1 : 2, r.x + r.w - 56, true, layout);
    }
}

// Slow work: a turning arc and what is happening, so a pause never looks like a hang.
void Dashboard::draw_busy(gfx::DrawList & l) const {
    const ui::Fonts & f = fonts_;
    const Rect d{ 960 - 440, 540 - 110, 880, 220 };
    sheet(l, d);
    const float cx = d.x + 92, cy = d.cy();
    l.ring(cx, cy, 30, 4, kHair);
    l.arc(cx, cy, 30, 4, clock_ * 5.0f, 1.4f + 0.8f * std::sin(clock_ * 2.1f), kBlue);
    put(l, f.semibold, f.semibold.font->fit(busy_title_, 30, d.w - 220), d.x + 156, cy - 6, 30, kInk);
    if (!busy_text_.empty()) {
        put(l, f.regular, f.regular.font->fit(busy_text_, 21, d.w - 220), d.x + 156, cy + 32, 21, kInk2);
    }
}

}  // namespace ps5lm
