// Station screen.
//
// Left: the station list from channels/istanbul.toml, with group tabs on
// top. Click a station (or use the arrows and Enter) to listen. Right: what
// is playing, a signal bar, and buttons for gain, the RF amp, squelch, and
// mute. Everything works with the mouse and with the keyboard.
//
// The radio runs in its own thread (run_listen). This screen never touches
// the HackRF directly. It sends changes through ListenRetune and reads back
// a ListenView snapshot about 20 times a second, so drawing can never cause
// an audio dropout.

#include "tui.hpp"

#include "tui_model.hpp"

#include "core/channels.hpp"
#include "core/listen.hpp"
#include "core/saved.hpp"
#include "core/scan.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vocatron {
namespace {

using namespace ftxui;

struct Group {
  const char *key;
  const char *label;
};

constexpr Group kGroups[] = {
    {"all", "All"},      {"saved", "Saved"},         {"found", "Found"}, {"airband", "Airband"},
    {"broadcast", "FM"}, {"marine", "Marine"},       {"amateur", "Ham"}, {"pmr", "PMR"},
    {"cb", "CB"},        {"shortwave", "Shortwave"},
};

// Squelch and mute change many times a second while audio plays, so they
// go straight to the DSP thread as atomics.
struct Live {
  std::atomic<bool> squelch_on{true};
  std::atomic<float> squelch_db{-52.0f};
  std::atomic<bool> pass_audio{true};
};

void sync_live(Live &live, const Knobs &knobs) {
  live.squelch_on.store(knobs.squelch_on, std::memory_order_relaxed);
  live.squelch_db.store(knobs.squelch_db, std::memory_order_relaxed);
  live.pass_audio.store(!knobs.muted, std::memory_order_relaxed);
}

const char *mode_label(const std::string &mode) {
  if (mode == "wfm") {
    return "wide FM";
  }
  if (mode == "am") {
    return "AM";
  }
  if (mode == "nfm") {
    return "narrow FM";
  }
  return "?";
}

// Owns the radio thread. start() opens the HackRF for one station,
// change() retunes or changes gain without closing it, stop() closes it.
class Receiver {
public:
  Receiver(ScreenInteractive *screen, Live *live) : screen_(screen), live_(live) {}
  ~Receiver() { stop(); }

  Receiver(const Receiver &) = delete;
  Receiver &operator=(const Receiver &) = delete;

  struct Snapshot {
    ListenView view;
    bool have_view = false;
    bool running = false;
  };

  // Why the last run ended. Only reported once.
  struct Ending {
    bool ended = false;
    int rc = 0;
    bool signaled = false;
  };

  void start(const Channel &station, const Knobs &knobs) {
    stop();
    station_ = station;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      view_ = ListenView{};
      have_view_ = false;
      running_ = true;
      ended_ = false;
      stop_req_ = false;
      pending_ = false;
    }
    thread_ = std::thread([this, knobs] { run(knobs); });
  }

  void change(const Channel &station, const Knobs &knobs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    pending_ = true;
    change_.channel_hz = station.hz;
    change_mode_ = station.mode;
    change_.lna_db = static_cast<std::uint32_t>(knobs.lna);
    change_.vga_db = static_cast<std::uint32_t>(knobs.vga);
    change_.amp = knobs.amp;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_req_ = true;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stop_req_ = false;
    running_ = false;
    ended_ = false;
  }

  Snapshot read() {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    snap.view = view_;
    snap.have_view = have_view_;
    snap.running = running_;
    return snap;
  }

  bool running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  Ending take_ending() {
    std::lock_guard<std::mutex> lock(mutex_);
    Ending out;
    out.ended = ended_;
    out.rc = rc_;
    out.signaled = signaled_;
    ended_ = false;
    return out;
  }

private:
  void run(Knobs knobs) {
    ListenConfig config;
    config.channel_hz = station_.hz;
    config.name = station_.name.c_str();
    config.mode = station_.mode.c_str();
    config.group = station_.group.c_str();
    config.lna_db = static_cast<std::uint32_t>(knobs.lna);
    config.vga_db = static_cast<std::uint32_t>(knobs.vga);
    config.amp = knobs.amp;
    config.speakers = true;
    config.squelch = knobs.squelch_on;
    config.squelch_dbfs = knobs.squelch_db;
    config.on_tick = &Receiver::tick;
    config.tick_user = this;
    config.squelch_enabled = &live_->squelch_on;
    config.squelch_level = &live_->squelch_db;
    config.pass_audio = &live_->pass_audio;
    bool signaled = false;
    config.signal_stop = &signaled;
    retune_ = ListenRetune{};
    config.retune = &retune_;

    const int rc = run_listen(config);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      ended_ = true;
      rc_ = rc;
      signaled_ = signaled;
    }
    screen_->PostEvent(Event::Custom);
  }

  // Runs on the radio thread about every 50 ms.
  static bool tick(const ListenView &view, void *user) {
    auto *self = static_cast<Receiver *>(user);
    bool keep = true;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->view_ = view;
      self->have_view_ = true;
      if (self->stop_req_) {
        keep = false;
      } else if (self->pending_) {
        self->pending_ = false;
        // run_listen reads this right after we return, on this same thread.
        self->retune_mode_ = self->change_mode_;
        self->retune_ = self->change_;
        self->retune_.mode = self->retune_mode_.c_str();
        self->retune_.pending = true;
      }
    }
    self->screen_->PostEvent(Event::Custom);
    return keep;
  }

  ScreenInteractive *screen_;
  Live *live_;
  std::thread thread_;
  // Copy of the station the radio was opened on. Its strings back the
  // config pointers for the whole run, so it never changes mid-run.
  Channel station_;

  std::mutex mutex_;
  ListenView view_;
  bool have_view_ = false;
  bool running_ = false;
  bool ended_ = false;
  bool stop_req_ = false;
  int rc_ = 0;
  bool signaled_ = false;
  bool pending_ = false;
  ListenRetune change_;
  std::string change_mode_;

  // Radio-thread side of a change.
  ListenRetune retune_;
  std::string retune_mode_;
};

// Runs an FM scan on its own thread. The receiver must be stopped first,
// because the scan opens the HackRF itself.
class Scanner {
public:
  explicit Scanner(ScreenInteractive *screen) : screen_(screen) {}
  ~Scanner() { cancel(true); }

  Scanner(const Scanner &) = delete;
  Scanner &operator=(const Scanner &) = delete;

  struct State {
    bool running = false;
    float fraction = 0.0f;
    std::string what;
  };

  void start(const ScanConfig &config) {
    cancel(true);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = true;
      finished_ = false;
      cancel_ = false;
      fraction_ = 0.0f;
      what_ = "starting";
    }
    thread_ = std::thread([this, config] {
      std::vector<ScanHit> hits;
      const int rc = run_fm_scan(config, &hits, &Scanner::progress, this);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        hits_ = std::move(hits);
        rc_ = rc;
        running_ = false;
        finished_ = true;
      }
      screen_->PostEvent(Event::Custom);
    });
  }

  // Stops the scan. `discard` drops what it found so far.
  void cancel(bool discard) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_ = true;
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    if (discard) {
      finished_ = false;
    }
  }

  State state() {
    std::lock_guard<std::mutex> lock(mutex_);
    State out;
    out.running = running_;
    out.fraction = fraction_;
    out.what = what_;
    return out;
  }

  // Once per finished scan: what it found.
  bool take_result(std::vector<ScanHit> *hits, int *rc) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!finished_) {
      return false;
    }
    finished_ = false;
    *hits = hits_;
    *rc = rc_;
    return true;
  }

private:
  static bool progress(float fraction, const char *what, void *user) {
    auto *self = static_cast<Scanner *>(user);
    bool keep = true;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->fraction_ = fraction;
      self->what_ = what;
      keep = !self->cancel_;
    }
    self->screen_->PostEvent(Event::Custom);
    return keep;
  }

  ScreenInteractive *screen_;
  std::thread thread_;
  std::mutex mutex_;
  bool running_ = false;
  bool finished_ = false;
  bool cancel_ = false;
  float fraction_ = 0.0f;
  std::string what_;
  std::vector<ScanHit> hits_;
  int rc_ = 0;
};

// Four dots for clarity 0..1.
std::string clarity_dots(float clarity) {
  const int filled = std::clamp(static_cast<int>(std::lround(clarity * 4.0f)), 0, 4);
  std::string out;
  for (int i = 0; i < 4; ++i) {
    out += i < filled ? "●" : "○";
  }
  return out;
}

Channel channel_from_hit(const ScanHit &hit) {
  Channel channel;
  channel.name = scan_hit_name(hit);
  channel.hz = hit.hz;
  channel.mode = "wfm";
  channel.bw_khz = 200;
  channel.group = "found";
  char text[160];
  std::snprintf(text, sizeof text, "Found by scan · clarity %.2f%s%s",
                static_cast<double>(hit.clarity), hit.rds_name.empty() ? "" : " · RDS ",
                hit.rds_name.c_str());
  channel.description = text;
  if (hit.rds_pi != 0) {
    char pi[24];
    std::snprintf(pi, sizeof pi, " · PI %04X", static_cast<unsigned>(hit.rds_pi));
    channel.description += pi;
  }
  return channel;
}

} // namespace

int run_tui() {
  const ChannelBook &book = channel_book();
  if (!book.error.empty()) {
    std::fprintf(stderr, "%s\n", book.error.c_str());
    return 1;
  }
  if (book.channels.empty()) {
    std::fprintf(stderr, "the channel file has no stations\n");
    return 1;
  }
  if (isatty(STDIN_FILENO) != 1 || isatty(STDOUT_FILENO) != 1) {
    std::fprintf(stderr, "vocatron tui needs a terminal\n");
    return 2;
  }

  auto screen = ScreenInteractive::Fullscreen();
  // Ctrl-C closes the radio cleanly instead of killing the program.
  screen.ForceHandleCtrlC(false);

  Live live;
  Receiver receiver(&screen, &live);
  Scanner scanner(&screen);
  Knobs knobs;
  std::string load_error;
  std::vector<Channel> saved = load_saved(&load_error);
  std::vector<Channel> found;
  // Clarity of each found station, by frequency, for the dots in the list.
  std::vector<std::pair<std::uint64_t, float>> found_clarity;

  // --- Station list -------------------------------------------------------
  int group_index = 0;
  std::vector<std::string> group_labels;
  for (const Group &group : kGroups) {
    group_labels.emplace_back(group.label);
  }
  std::vector<Channel> shown;
  std::vector<std::string> rows;
  int selected = 0;
  Channel now; // what is playing; empty name when stopped
  std::string message = load_error;

  auto is_saved = [&](const Channel &channel) {
    return std::any_of(saved.begin(), saved.end(),
                       [&](const Channel &s) { return same_station(s, channel); });
  };
  auto fill_rows = [&] {
    rows.clear();
    for (const Channel &channel : shown) {
      char row[96];
      const bool playing = receiver.running() && same_station(channel, now);
      std::snprintf(row, sizeof row, "%s %-14.14s %8.3f %s", playing ? "▶" : " ",
                    channel.name.c_str(), static_cast<double>(channel.hz) / 1.0e6,
                    is_saved(channel) ? "★" : " ");
      std::string line = row;
      if (channel.group == "found") {
        for (const auto &[hz, clarity] : found_clarity) {
          if (hz == channel.hz) {
            line += " " + clarity_dots(clarity);
          }
        }
      }
      rows.push_back(line);
    }
  };
  auto fill_shown = [&] {
    const std::string keep =
        selected >= 0 && selected < static_cast<int>(shown.size()) ? shown[selected].name : "";
    shown.clear();
    const std::string want = kGroups[group_index].key;
    if (want == "saved" || want == "all") {
      shown.insert(shown.end(), saved.begin(), saved.end());
    }
    if (want == "found") {
      shown = found;
    }
    for (const Channel &channel : book.channels) {
      if (want == "all" || channel.group == want) {
        shown.push_back(channel);
      }
    }
    selected = 0;
    for (int i = 0; i < static_cast<int>(shown.size()); ++i) {
      if (shown[static_cast<std::size_t>(i)].name == keep) {
        selected = i;
      }
    }
    fill_rows();
  };

  // --- Actions ------------------------------------------------------------
  auto tune = [&](const Channel &station) {
    if (scanner.state().running) {
      scanner.cancel(true);
    }
    const bool same = same_station(station, now);
    enter_station(knobs, station.mode, same);
    sync_live(live, knobs);
    message.clear();
    if (receiver.running()) {
      receiver.change(station, knobs);
    } else {
      receiver.start(station, knobs);
    }
    now = station;
    fill_rows();
  };
  auto tune_selected = [&] {
    if (selected >= 0 && selected < static_cast<int>(shown.size())) {
      tune(shown[static_cast<std::size_t>(selected)]);
    }
  };
  // Gain and amp go to the radio; nothing to do when it is stopped.
  auto push_gain = [&] {
    if (!now.name.empty()) {
      receiver.change(now, knobs);
    }
  };
  auto stop_radio = [&] {
    receiver.stop();
    knobs.amp = false;
    fill_rows();
  };
  auto quit = [&] {
    scanner.cancel(true);
    receiver.stop();
    screen.Exit();
  };
  auto show_group = [&](const char *key) {
    for (int i = 0; i < static_cast<int>(std::size(kGroups)); ++i) {
      if (std::string(kGroups[i].key) == key) {
        group_index = i;
      }
    }
    fill_shown();
  };
  // Save the playing station as one you can hear, or forget it again.
  auto toggle_save = [&] {
    if (now.name.empty()) {
      message = "Pick a station first, then Save it if you can hear it.";
      return;
    }
    std::vector<Channel> next = saved;
    const auto old = std::find_if(next.begin(), next.end(),
                                  [&](const Channel &s) { return same_station(s, now); });
    char hz_text[32];
    std::snprintf(hz_text, sizeof hz_text, "%.1f MHz", static_cast<double>(now.hz) / 1.0e6);
    std::string done;
    if (old != next.end()) {
      done = "Forgot " + old->name + " (" + hz_text + ").";
      next.erase(old);
    } else {
      Channel keep = now;
      // A found station is named by RDS when RDS has arrived meanwhile.
      const Receiver::Snapshot snap = receiver.read();
      if (keep.group == "found" && snap.view.rds_name[0] != '\0') {
        keep.name = snap.view.rds_name;
      }
      keep.group = "saved";
      next.push_back(keep);
      std::sort(next.begin(), next.end(),
                [](const Channel &a, const Channel &b) { return a.hz < b.hz; });
      done = "Saved " + keep.name + " (" + hz_text + "). It is in the Saved tab.";
    }
    std::string error;
    if (!write_saved(next, &error)) {
      message = error;
      return;
    }
    saved = next;
    message = done;
    fill_shown();
  };
  auto toggle_scan = [&] {
    if (scanner.state().running) {
      scanner.cancel(false);
      return;
    }
    stop_radio();
    ScanConfig config;
    config.lna_db = static_cast<std::uint32_t>(knobs.lna);
    config.vga_db = static_cast<std::uint32_t>(knobs.vga);
    config.amp = knobs.amp;
    message.clear();
    scanner.start(config);
  };

  // --- Components ---------------------------------------------------------
  MenuOption tab_option = MenuOption::Toggle();
  tab_option.entries = &group_labels;
  tab_option.selected = &group_index;
  tab_option.on_change = [&] { fill_shown(); };
  Component tabs = Menu(tab_option);

  MenuOption list_option = MenuOption::Vertical();
  list_option.entries = &rows;
  list_option.selected = &selected;
  list_option.on_enter = tune_selected;
  Component list = Menu(list_option);
  Box list_box;
  // A click picks the row (the menu does that) and then tunes it.
  Component list_click = CatchEvent(list, [&](Event event) {
    if (event.is_mouse() && event.mouse().button == Mouse::Left &&
        event.mouse().motion == Mouse::Pressed &&
        list_box.Contain(event.mouse().x, event.mouse().y)) {
      list->OnEvent(event);
      tune_selected();
      return true;
    }
    return false;
  });

  const ButtonOption flat = ButtonOption::Ascii();
  auto lna_step = [&](int direction) {
    if (step_lna(knobs, direction)) {
      push_gain();
    }
  };
  auto vga_step = [&](int direction) {
    if (step_vga(knobs, direction)) {
      push_gain();
    }
  };
  Component lna_down = Button(" − ", [&] { lna_step(-1); }, flat);
  Component lna_up = Button(" + ", [&] { lna_step(+1); }, flat);
  Component vga_down = Button(" − ", [&] { vga_step(-1); }, flat);
  Component vga_up = Button(" + ", [&] { vga_step(+1); }, flat);
  std::string amp_label;
  Component amp_button = Button(
      &amp_label,
      [&] {
        knobs.amp = !knobs.amp;
        message = knobs.amp ? "RF amp ON. If clip goes above 0, it turns itself off." : "";
        push_gain();
      },
      flat);
  Component sql_down = Button(
      " − ",
      [&] {
        step_squelch(knobs, -1);
        sync_live(live, knobs);
      },
      flat);
  Component sql_up = Button(
      " + ",
      [&] {
        step_squelch(knobs, +1);
        sync_live(live, knobs);
      },
      flat);
  std::string gate_label;
  Component gate_button = Button(
      &gate_label,
      [&] {
        toggle_squelch(knobs);
        sync_live(live, knobs);
      },
      flat);
  std::string mute_label;
  Component mute_button = Button(
      &mute_label,
      [&] {
        knobs.muted = !knobs.muted;
        sync_live(live, knobs);
      },
      flat);
  std::string play_label;
  Component play_button = Button(
      &play_label,
      [&] {
        if (receiver.running()) {
          stop_radio();
        } else if (!now.name.empty()) {
          tune(now);
        } else {
          tune_selected();
        }
      },
      flat);
  Component quit_button = Button(" Quit ", quit, flat);
  std::string save_label;
  Component save_button = Button(&save_label, toggle_save, flat);
  std::string scan_label;
  Component scan_button = Button(&scan_label, toggle_scan, flat);

  Component controls = Container::Vertical({
      Container::Horizontal({lna_down, lna_up}),
      Container::Horizontal({vga_down, vga_up}),
      amp_button,
      Container::Horizontal({sql_down, sql_up, gate_button}),
      Container::Horizontal({save_button, scan_button}),
      Container::Horizontal({mute_button, play_button, quit_button}),
  });
  Component body = Container::Horizontal({list_click, controls});
  Component layout = Container::Vertical({tabs, body});

  Component view = Renderer(layout, [&] {
    const Receiver::Snapshot snap = receiver.read();
    const ListenView &v = snap.view;
    const bool live_rf = snap.running && snap.have_view && v.usb_bytes > 0;

    amp_label = knobs.amp ? " ON " : " OFF ";
    gate_label = knobs.squelch_on ? " gate on " : " gate off ";
    mute_label = knobs.muted ? " Unmute " : " Mute ";
    play_label = snap.running ? " Stop " : " Play ";
    const Scanner::State scan = scanner.state();
    scan_label = scan.running ? " Cancel scan " : " Scan FM ";
    save_label = !now.name.empty() && is_saved(now) ? " ★ Saved (forget) " : " Save ";

    // What is happening, in words.
    std::string sound = "stopped";
    Color sound_color = Color::GrayDark;
    if (snap.running && !live_rf) {
      sound = "starting…";
    } else if (snap.running && knobs.muted) {
      sound = "muted";
      sound_color = Color::Yellow;
    } else if (snap.running && !knobs.squelch_on) {
      sound = "playing (gate off)";
      sound_color = Color::Green;
    } else if (snap.running && v.sql_open) {
      sound = "playing";
      sound_color = Color::Green;
    } else if (snap.running) {
      sound = "quiet channel, gate shut";
    }

    Elements info;
    if (scan.running) {
      info.push_back(text("Scanning the FM band") | bold);
      info.push_back(gauge(scan.fraction) | color(Color::Cyan));
      info.push_back(text(scan.what) | dim);
      info.push_back(paragraph("It finds every station this radio can hear, how clean each one "
                               "is, and its RDS name. About a minute.") |
                     dim);
    } else if (now.name.empty()) {
      info.push_back(text("Click a station to listen.") | bold);
    } else {
      char line[160];
      std::snprintf(line, sizeof line, "%.3f MHz · %s · %s", static_cast<double>(now.hz) / 1.0e6,
                    mode_label(now.mode), now.group.c_str());
      info.push_back(text(now.name) | bold);
      info.push_back(text(line));
      info.push_back(paragraph(now.description) | dim);
    }
    info.push_back(separatorEmpty());

    char db[32];
    std::snprintf(db, sizeof db, live_rf ? " %6.1f dBFS" : "     -- dBFS",
                  static_cast<double>(v.channel_dbfs));
    const float fill = live_rf ? signal_fill(v.channel_dbfs) : 0.0f;
    const Color bar_color = !live_rf           ? Color::GrayDark
                            : v.sql_open       ? Color::Green
                            : knobs.squelch_on ? Color::Yellow
                                               : Color::Green;
    info.push_back(hbox({text("Signal  "), gauge(fill) | color(bar_color) | flex, text(db)}));
    if (live_rf && v.clarity >= 0.0f) {
      char clear[16];
      std::snprintf(clear, sizeof clear, " %.2f", static_cast<double>(v.clarity));
      const Color clear_color = v.clarity >= 0.5f   ? Color::Green
                                : v.clarity >= 0.2f ? Color::Yellow
                                                    : Color::Red;
      info.push_back(hbox({text("Clarity "), gauge(v.clarity) | color(clear_color) | flex,
                           text(clear), text("      ")}));
    }
    if (live_rf && v.rds_name[0] != '\0') {
      info.push_back(hbox({text("RDS     "), text(v.rds_name) | bold | color(Color::Cyan)}));
    }
    info.push_back(hbox({text("Sound   "), text(sound) | color(sound_color)}));
    info.push_back(separatorEmpty());

    char value[32];
    std::snprintf(value, sizeof value, " %2d dB ", knobs.lna);
    info.push_back(hbox({text("Gain    "), lna_down->Render(), text(value), lna_up->Render(),
                         text("  LNA, the main one") | dim}));
    std::snprintf(value, sizeof value, " %2d dB ", knobs.vga);
    info.push_back(hbox({text("VGA     "), vga_down->Render(), text(value), vga_up->Render(),
                         text("  after the mixer") | dim}));
    Element amp = amp_button->Render();
    if (knobs.amp) {
      amp = hbox(
          {amp | color(Color::Red) | bold, text("  +11 dB at the antenna") | color(Color::Red)});
    } else {
      amp = hbox({amp, text("  off is safest") | dim});
    }
    info.push_back(hbox({text("RF amp  "), amp}));
    std::snprintf(value, sizeof value, " %4.0f dBFS ", static_cast<double>(knobs.squelch_db));
    info.push_back(hbox({text("Squelch "), sql_down->Render(), text(value), sql_up->Render(),
                         text(" "), gate_button->Render()}));
    info.push_back(separatorEmpty());
    info.push_back(hbox({save_button->Render(), text("  "), scan_button->Render()}));
    info.push_back(hbox({mute_button->Render(), text("  "), play_button->Render(), text("  "),
                         quit_button->Render()}));
    info.push_back(filler());

    if (live_rf) {
      char stats[160];
      std::snprintf(stats, sizeof stats, "adc %.1f dBFS · clip %.2f%% · dropped %llu · gaps %llu",
                    static_cast<double>(v.adc_dbfs), static_cast<double>(v.clip_pct),
                    static_cast<unsigned long long>(v.dropped),
                    static_cast<unsigned long long>(v.underruns));
      info.push_back(text(stats) | dim);
    }
    if (!message.empty()) {
      info.push_back(paragraph(message) | color(Color::Yellow));
    }

    Element station_list = list->Render() | vscroll_indicator | frame | reflect(list_box);
    return vbox({
               hbox({text(" vocatron  ") | bold | color(Color::Cyan), tabs->Render()}),
               separator(),
               hbox({station_list | size(WIDTH, EQUAL, 38), separator(), vbox(info) | flex}) | flex,
               separator(),
               text(" click a station · Save the ones you can hear · = − gain · [ ] squelch · "
                    "m mute · q quit") |
                   dim,
           }) |
           border;
  });

  // Keyboard shortcuts, and housekeeping on each refresh from the radio.
  Component root = CatchEvent(view, [&](Event event) {
    if (event == Event::Custom) {
      std::vector<ScanHit> hits;
      int scan_rc = 0;
      if (scanner.take_result(&hits, &scan_rc)) {
        if (scan_rc != 0) {
          message = "The scan could not use the radio. Is the HackRF plugged in and free?";
        } else {
          found.clear();
          found_clarity.clear();
          for (const ScanHit &hit : hits) {
            found.push_back(channel_from_hit(hit));
            found_clarity.emplace_back(hit.hz, hit.clarity);
          }
          show_group("found");
          message =
              "Found " + std::to_string(found.size()) +
              " stations (●●●● = clean). Click one to listen, and Save the ones you can hear.";
        }
      }
      const Receiver::Snapshot snap = receiver.read();
      // A found station without a name takes its RDS name once it arrives.
      if (snap.running && now.group == "found" && snap.view.rds_name[0] != '\0' &&
          now.name != snap.view.rds_name) {
        for (Channel &channel : found) {
          if (same_station(channel, now)) {
            channel.name = snap.view.rds_name;
          }
        }
        now.name = snap.view.rds_name;
        if (std::string(kGroups[group_index].key) == "found") {
          fill_shown();
        }
      }
      const Receiver::Ending ending = receiver.take_ending();
      if (ending.ended) {
        if (ending.signaled) {
          quit();
          return true;
        }
        if (ending.rc != 0) {
          message = "The receiver stopped. Is the HackRF plugged in and free? Click a station "
                    "to try again.";
        }
        knobs.amp = false;
        fill_rows();
      }
      if (snap.running && amp_should_drop(knobs, snap.view.clip_pct)) {
        knobs.amp = false;
        push_gain();
        message = "RF amp turned off: the input was clipping.";
      }
      return false;
    }
    if (event == Event::CtrlC || event == Event::Character('q')) {
      quit();
      return true;
    }
    if (event == Event::Escape) {
      stop_radio();
      return true;
    }
    if (event == Event::Character('m')) {
      knobs.muted = !knobs.muted;
      sync_live(live, knobs);
      return true;
    }
    if (event == Event::Character('=') || event == Event::Character('+')) {
      lna_step(+1);
      return true;
    }
    if (event == Event::Character('-')) {
      lna_step(-1);
      return true;
    }
    if (event == Event::Character('[') || event == Event::Character(']')) {
      step_squelch(knobs, event == Event::Character(']') ? +1 : -1);
      sync_live(live, knobs);
      return true;
    }
    if (event == Event::Character('s')) {
      toggle_squelch(knobs);
      sync_live(live, knobs);
      return true;
    }
    return false;
  });

  // Start on the airport loop, which is on the air around the clock.
  fill_shown();
  for (int i = 0; i < static_cast<int>(shown.size()); ++i) {
    if (shown[static_cast<std::size_t>(i)].name == "LTBA_ATIS") {
      selected = i;
    }
  }
  list->TakeFocus();
  tune_selected();

  screen.Loop(root);
  receiver.stop();
  return 0;
}

} // namespace vocatron
