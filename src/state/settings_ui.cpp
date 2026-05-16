// settings_ui — see include/state/settings_ui.h for the contract.

#include <Arduino.h>
#include <pico/mutex.h>

#include "settings_ui.h"

#include "buzzer.h"
#include "prefs.h"

namespace settings_ui {

namespace {

// FR-19 audio palette. Flash-resident so buzzer::play() can hold
// the pointer for the duration of the sequence (NFR-2.2).
//
// Open jingle: rising arpeggio — invites the operator into the
// menu and gives a strong "you opened something" cue.
constexpr buzzer::Note kOpenJingle[] = {
    {523, 80},   // C5
    {659, 80},   // E5
    {784, 80},   // G5
    {1047, 120}, // C6
};
// Close jingle: descending mirror of open. Reads as "you put
// something away" without being mistaken for an alert.
constexpr buzzer::Note kCloseJingle[] = {
    {1047, 80},  // C6
    {784, 80},   // G5
    {659, 80},   // E5
    {523, 120},  // C5
};
// Enter category — short two-step "click into".
constexpr buzzer::Note kEnterTick[] = {
    {659, 30},   // E5
    {880, 50},   // A5
};
// Back — mirror of enter (high then low).
constexpr buzzer::Note kBackTick[] = {
    {880, 30},
    {659, 50},
};
// Up/down nav — single deeper tick; lower than chirp so the menu
// has its own voice.
constexpr buzzer::Note kNavTick[] = {
    {440, 30},   // A4
};

// Idle auto-close window (FR-19.6). Long enough for the operator
// to read the screen and decide; short enough that an abandoned
// menu doesn't sit on screen forever.
constexpr uint32_t kIdleCloseMs   = 30'000;
// Animation envelope durations.
constexpr uint32_t kFadeMs        = 300;
constexpr uint32_t kFlashMs       = 80;     // inverse-flash on commit
// Save-toast lifetime — re-armed on every prefs_dirty falling edge
// so successive commits collapse into one visible toast.
constexpr uint32_t kToastMs       = 1500;

// Cross-core state. s_open is exposed via the lock-free is_open()
// fast path; everything else is read under s_mutex via snapshot().
mutex_t          s_mutex;
volatile uint8_t s_open_byte    = 0;        // 1 iff visible (not in fade-out)

Mode     s_mode             = Mode::CLOSED;
Cat      s_cat              = Cat::DISPLAY;
uint8_t  s_sel_index        = 0;            // category-local
FadeDir  s_fade_dir         = FadeDir::NONE;
uint32_t s_fade_started_ms  = 0;
uint32_t s_flash_started_ms = 0;
uint32_t s_toast_started_ms = 0;
uint32_t s_opened_ms        = 0;
uint32_t s_last_input_ms    = 0;
// Edge detector for the save-toast: rising edge of prefs_dirty
// that subsequently goes false fires the toast. Tracked here so
// the model owns its own state without stamping flash artefacts.
bool     s_was_dirty        = false;

uint8_t item_count_for(Cat c) {
  switch (c) {
    case Cat::DISPLAY: return static_cast<uint8_t>(DisplayItem::COUNT);
    case Cat::SOUND:   return static_cast<uint8_t>(SoundItem::COUNT);
    default:           return 1;
  }
}

void play_locked(const buzzer::Note* notes, uint8_t n) {
  // Settings cues compose with the FR-10.6 button-sound gate so a
  // user who muted button feedback gets a silent menu. Boot/night
  // quiet still wins one layer down inside buzzer::play().
  if (!prefs::current().button_sound) return;
  buzzer::play(notes, n);
}

void mark_input(uint32_t now_ms) {
  s_last_input_ms = now_ms;
}

void open_locked(uint32_t now_ms) {
  s_mode             = Mode::ROOT;
  s_cat              = Cat::DISPLAY;
  s_sel_index        = 0;
  s_fade_dir         = FadeDir::IN;
  s_fade_started_ms  = now_ms;
  s_opened_ms        = now_ms;
  s_last_input_ms    = now_ms;
  s_open_byte        = 1;
  play_locked(kOpenJingle,
              sizeof(kOpenJingle) / sizeof(kOpenJingle[0]));
}

void close_locked(uint32_t now_ms, bool play_jingle) {
  if (s_mode == Mode::CLOSED && s_fade_dir != FadeDir::IN) return;
  s_mode             = Mode::CLOSED;
  s_fade_dir         = FadeDir::OUT;
  s_fade_started_ms  = now_ms;
  s_open_byte        = 0;
  if (play_jingle) {
    play_locked(kCloseJingle,
                sizeof(kCloseJingle) / sizeof(kCloseJingle[0]));
  }
}

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void toggle_open() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  if (s_open_byte == 1) {
    close_locked(now_ms, /*play_jingle=*/true);
  } else {
    open_locked(now_ms);
  }
  mutex_exit(&s_mutex);
}

void force_close() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  if (s_open_byte == 1 || s_mode != Mode::CLOSED) {
    s_mode             = Mode::CLOSED;
    s_fade_dir         = FadeDir::NONE;   // hard close, no fade
    s_fade_started_ms  = now_ms;
    s_open_byte        = 0;
  }
  mutex_exit(&s_mutex);
}

bool is_open() {
  // Single byte read — atomic on RP2040 (CODING_PRACTICES §3).
  return s_open_byte != 0;
}

void nav_up() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::ROOT) {
    // Two tiles — wrap.
    s_sel_index = static_cast<uint8_t>(
        (s_sel_index + static_cast<uint8_t>(Cat::COUNT) - 1) %
        static_cast<uint8_t>(Cat::COUNT));
    play_locked(kNavTick, 1);
  } else if (s_mode == Mode::CATEGORY) {
    const uint8_t n = item_count_for(s_cat);
    if (n > 1) {
      s_sel_index = static_cast<uint8_t>((s_sel_index + n - 1) % n);
      play_locked(kNavTick, 1);
    }
  }
  mutex_exit(&s_mutex);
}

void nav_down() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::ROOT) {
    s_sel_index = static_cast<uint8_t>(
        (s_sel_index + 1) % static_cast<uint8_t>(Cat::COUNT));
    play_locked(kNavTick, 1);
  } else if (s_mode == Mode::CATEGORY) {
    const uint8_t n = item_count_for(s_cat);
    if (n > 1) {
      s_sel_index = static_cast<uint8_t>((s_sel_index + 1) % n);
      play_locked(kNavTick, 1);
    }
  }
  mutex_exit(&s_mutex);
}

namespace {

// Apply a value change to the selected item in CATEGORY mode.
// `delta` is +1 / -1 for tint and tick-mode; ignored (treated as
// "toggle") for the boolean rows. Returns true iff the value
// actually changed (for the inverse-flash + knob-pitch tick).
bool apply_value_delta(int8_t delta) {
  const prefs::Prefs& cur = prefs::current();
  switch (s_cat) {
    case Cat::DISPLAY: {
      // Only TINT in S.1. 10 % steps clamped at 0 / 100 — no
      // wrap; the rail produces a low-pitch "thunk" in nav_left/right.
      const uint8_t  before = cur.image_tint_pct;
      const int      stepped = static_cast<int>(before) + (delta > 0 ? 10 : -10);
      const uint8_t  next   = stepped < 0   ? 0u :
                              stepped > 100 ? 100u :
                              static_cast<uint8_t>(stepped);
      if (next == before) return false;
      prefs::set_image_tint_pct(next);
      return true;
    }
    case Cat::SOUND: {
      const SoundItem item = static_cast<SoundItem>(s_sel_index);
      switch (item) {
        case SoundItem::THEME: {
          const bool next = !cur.theme_sound;
          prefs::set_theme_sound(next);
          return true;
        }
        case SoundItem::BUTTON: {
          const bool next = !cur.button_sound;
          prefs::set_button_sound(next);
          return true;
        }
        case SoundItem::TICK: {
          const uint8_t before = static_cast<uint8_t>(cur.tick_sound_mode);
          const uint8_t kN     = 4;
          const uint8_t next   = static_cast<uint8_t>(
              (before + (delta > 0 ? 1 : kN - 1)) % kN);
          prefs::set_tick_sound_mode(static_cast<prefs::TickSoundMode>(next));
          return true;
        }
        default: return false;
      }
    }
    default: return false;
  }
}

// Tint-aware "knob pitch" — the click pitch literally rises with
// the tint percentage so the operator hears the value as well as
// sees it. Other rows fall back to a fixed nav tick.
void play_value_change_locked() {
  if (s_cat == Cat::DISPLAY &&
      static_cast<DisplayItem>(s_sel_index) == DisplayItem::TINT) {
    if (!prefs::current().button_sound) return;
    const uint16_t pct  = prefs::current().image_tint_pct;
    const uint16_t freq = static_cast<uint16_t>(400 + pct * 8);
    buzzer::tick_click(freq);
    return;
  }
  // Enum / boolean changes: walk a few semitones with the index so
  // each step in the SOUND category has a distinct voice.
  if (s_cat == Cat::SOUND) {
    if (!prefs::current().button_sound) return;
    const uint16_t freq = static_cast<uint16_t>(600 + s_sel_index * 150);
    buzzer::tick_click(freq);
    return;
  }
  play_locked(kNavTick, 1);
}

// Soft "rail thunk" when the user pushes ◄ at 0 % or ► at 100 %
// and the value can't move. Lower than the knob pitch so the ear
// reads it as "stop".
void play_rail_thunk_locked() {
  if (!prefs::current().button_sound) return;
  buzzer::tick_click(220);
}

}  // namespace

void nav_left() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::CATEGORY) {
    const bool changed = apply_value_delta(-1);
    if (changed) {
      s_flash_started_ms = now_ms;
      play_value_change_locked();
    } else {
      play_rail_thunk_locked();
    }
  }
  mutex_exit(&s_mutex);
}

void nav_right() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::CATEGORY) {
    const bool changed = apply_value_delta(+1);
    if (changed) {
      s_flash_started_ms = now_ms;
      play_value_change_locked();
    } else {
      play_rail_thunk_locked();
    }
  }
  mutex_exit(&s_mutex);
}

void commit_ok() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::ROOT) {
    s_cat       = static_cast<Cat>(s_sel_index %
                                   static_cast<uint8_t>(Cat::COUNT));
    s_mode      = Mode::CATEGORY;
    s_sel_index = 0;
    play_locked(kEnterTick,
                sizeof(kEnterTick) / sizeof(kEnterTick[0]));
  } else if (s_mode == Mode::CATEGORY) {
    // OK on a boolean toggles it (so the operator can use OK or ◄/►
    // interchangeably for the on/off rows). On non-boolean rows OK
    // is a no-op acknowledgement.
    if (s_cat == Cat::SOUND) {
      const SoundItem item = static_cast<SoundItem>(s_sel_index);
      if (item == SoundItem::THEME || item == SoundItem::BUTTON) {
        const bool changed = apply_value_delta(+1);
        if (changed) {
          s_flash_started_ms = now_ms;
          play_value_change_locked();
        }
        mutex_exit(&s_mutex);
        return;
      }
    }
    play_locked(kEnterTick, 1);
  }
  mutex_exit(&s_mutex);
}

void back() {
  const uint32_t now_ms = millis();
  mutex_enter_blocking(&s_mutex);
  mark_input(now_ms);
  if (s_mode == Mode::CATEGORY) {
    s_mode      = Mode::ROOT;
    s_sel_index = static_cast<uint8_t>(s_cat);  // anchor on the category just exited
    play_locked(kBackTick,
                sizeof(kBackTick) / sizeof(kBackTick[0]));
  } else if (s_mode == Mode::ROOT) {
    // BACK from the root closes the menu — same effect as a
    // second `*` press.
    close_locked(now_ms, /*play_jingle=*/true);
  }
  mutex_exit(&s_mutex);
}

void tick(uint32_t now_ms) {
  // Cheap fast path: nothing to do when we've never been opened
  // and there's no save toast pending.
  const bool dirty_now = prefs::is_dirty();
  if (s_open_byte == 0 && s_mode == Mode::CLOSED &&
      !dirty_now && !s_was_dirty) {
    return;
  }

  mutex_enter_blocking(&s_mutex);

  // FR-19.6 idle auto-close — 30 s of no nav input while visible.
  if (s_open_byte == 1 &&
      static_cast<int32_t>(now_ms - (s_last_input_ms + kIdleCloseMs)) >= 0) {
    close_locked(now_ms, /*play_jingle=*/true);
  }

  // Save-toast trigger: rising edge of dirty (a commit happened),
  // then on the falling edge stamp the toast so it appears the
  // moment the FR-18.4 writeback lands.
  if (dirty_now) {
    s_was_dirty = true;
  } else if (s_was_dirty) {
    s_was_dirty        = false;
    s_toast_started_ms = now_ms;
  }

  mutex_exit(&s_mutex);
}

Snapshot snapshot() {
  Snapshot s{};
  mutex_enter_blocking(&s_mutex);
  s.mode               = s_mode;
  s.cat                = s_cat;
  s.sel_index          = s_sel_index;
  s.fade_dir           = s_fade_dir;
  s.fade_started_ms    = s_fade_started_ms;
  s.flash_started_ms   = s_flash_started_ms;
  s.toast_started_ms   = s_toast_started_ms;
  s.opened_ms          = s_opened_ms;
  s.last_input_ms      = s_last_input_ms;
  // Snapshot the value-of-truth from prefs (still under our mutex so
  // a concurrent prefs setter doesn't matter — prefs has its own).
  const prefs::Prefs& p = prefs::current();
  s.tint_pct        = p.image_tint_pct;
  s.theme_sound     = p.theme_sound;
  s.button_sound    = p.button_sound;
  s.tick_sound_mode = static_cast<uint8_t>(p.tick_sound_mode);
  mutex_exit(&s_mutex);
  return s;
}

}  // namespace settings_ui
