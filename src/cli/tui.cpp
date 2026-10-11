// Station screen.
//
// The top half is the receiver: which station, how strong the channel is,
// and whether the squelch is letting sound through. The bottom half is the
// list from channels/istanbul.toml. Enter tunes the highlighted row.
// The speaker is open the whole time. Each mode has its own squelch,
// so an empty channel stays quiet instead of playing hiss.

#include "tui.hpp"

#include "tui_model.hpp"

#include "core/channels.hpp"
#include "core/listen.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vocatron {
namespace {

volatile std::sig_atomic_t g_tui_stop = 0;

void on_tui_signal(int /*sig*/) { g_tui_stop = 1; }

struct TuiSignals {
  struct sigaction old_int{};
  struct sigaction old_term{};
  bool installed = false;

  TuiSignals() {
    struct sigaction action{};
    action.sa_handler = on_tui_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) == 0 && sigaction(SIGTERM, &action, &old_term) == 0) {
      installed = true;
    }
  }

  ~TuiSignals() {
    if (installed) {
      sigaction(SIGINT, &old_int, nullptr);
      sigaction(SIGTERM, &old_term, nullptr);
    }
  }

  TuiSignals(const TuiSignals &) = delete;
  TuiSignals &operator=(const TuiSignals &) = delete;
};

const char *kGroups[] = {"all",     "broadcast", "airband", "marine",
                         "amateur", "pmr",       "cb",      "shortwave"};
constexpr int kGroupCount = 8;

const char *kReset = "\033[0m";
const char *kDim = "\033[2m";
const char *kRev = "\033[7m";
const char *kCyan = "\033[36m";
const char *kGreen = "\033[32m";
const char *kBold = "\033[1m";

class Term {
public:
  bool enter() {
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
      return false;
    }
    termios raw = saved_;
    raw.c_lflag = static_cast<tcflag_t>(raw.c_lflag & ~(ICANON | ECHO));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
      return false;
    }
    raw_ = true;
    std::fputs("\033[?1049h\033[?25l", stdout);
    std::fflush(stdout);
    alt_ = true;
    return true;
  }

  ~Term() { leave(); }

  void leave() {
    if (alt_) {
      std::fputs("\033[?25h\033[?1049l", stdout);
      std::fflush(stdout);
      alt_ = false;
    }
    if (raw_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
      raw_ = false;
    }
  }

  Term() = default;
  Term(const Term &) = delete;
  Term &operator=(const Term &) = delete;

  TuiKey read_key(int timeout_ms) {
    pollfd fd{};
    fd.fd = STDIN_FILENO;
    fd.events = POLLIN;
    const int pr = ::poll(&fd, 1, timeout_ms);
    if (pr <= 0) {
      return TuiKey::None;
    }
    unsigned char byte = 0;
    if (::read(STDIN_FILENO, &byte, 1) != 1) {
      return TuiKey::None;
    }
    if (byte != 27) {
      return map_byte(byte);
    }
    fd.revents = 0;
    if (::poll(&fd, 1, 20) <= 0) {
      return TuiKey::Stop;
    }
    unsigned char seq[6] = {};
    const ssize_t n = ::read(STDIN_FILENO, seq, sizeof seq);
    if (n >= 2 && seq[0] == '[') {
      if (seq[1] == 'A') {
        return TuiKey::Up;
      }
      if (seq[1] == 'B') {
        return TuiKey::Down;
      }
    }
    return TuiKey::None;
  }

private:
  static TuiKey map_byte(unsigned char byte) {
    switch (byte) {
    case 'q':
    case 'Q':
      return TuiKey::Quit;
    case '\n':
    case '\r':
      return TuiKey::Enter;
    case 'k':
    case 'K':
      return TuiKey::Up;
    case 'j':
    case 'J':
      return TuiKey::Down;
    case '[':
      return TuiKey::SqlLoosen;
    case ']':
      return TuiKey::SqlTighten;
    case 's':
    case 'S':
      return TuiKey::SqlToggle;
    case 'm':
    case 'M':
      return TuiKey::Mute;
    case 'g':
    case 'G':
      return TuiKey::Group;
    case '-':
      return TuiKey::LnaDown;
    case '=':
    case '+':
      return TuiKey::LnaUp;
    default:
      return TuiKey::None;
    }
  }

  termios saved_{};
  bool raw_ = false;
  bool alt_ = false;
};

struct Live {
  std::atomic<bool> squelch_on{true};
  std::atomic<float> squelch_db{-40.0f};
  std::atomic<bool> pass_audio{true};
};

void sync_live(Live &live, const TuiModel &model) {
  live.squelch_on.store(model.squelch_on, std::memory_order_relaxed);
  live.squelch_db.store(model.squelch_db, std::memory_order_relaxed);
  live.pass_audio.store(model.hear, std::memory_order_relaxed);
}

void window_size(int *rows, int *cols) {
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_row < 8 || size.ws_col < 40) {
    *rows = 24;
    *cols = 80;
    return;
  }
  *rows = size.ws_row;
  *cols = size.ws_col;
}

void fill_shown(const ChannelBook &book, int group, std::vector<Channel> &out) {
  const char *want = kGroups[group];
  out.clear();
  for (const Channel &channel : book.channels) {
    if (group == 0 || channel.group == want) {
      out.push_back(channel);
    }
  }
}

int find_name(const std::vector<Channel> &rows, const std::string &name) {
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    if (rows[static_cast<std::size_t>(i)].name == name) {
      return i;
    }
  }
  return -1;
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
  return "mode";
}

// Measured on this radio, amp off, VGA 20, LNA 40. AM opens on LTFM ATIS
// near -50 dBFS and stays shut on the LNA 16 hiss near -68. Wide FM noise
// sat near -41, so -32 stays shut. Empty narrow FM sat near -60, so -48
// stays shut until a radio actually keys up.
float squelch_for_mode(const std::string &mode) {
  if (mode == "am") {
    return -58.0f;
  }
  if (mode == "wfm") {
    return -32.0f;
  }
  return -48.0f;
}

std::string clip_plain(std::string text, int width) {
  if (width < 1) {
    return {};
  }
  if (static_cast<int>(text.size()) > width) {
    text.resize(static_cast<std::size_t>(width));
  }
  return text;
}

struct Screen {
  TuiModel *model = nullptr;
  const std::vector<Channel> *shown = nullptr;
  const Channel *now = nullptr;
  const ListenView *view = nullptr;
  const std::string *message = nullptr;
  int rows = 24;
  int cols = 80;
};

std::string draw_screen(const Screen &screen) {
  const TuiModel &model = *screen.model;
  const int rows = screen.rows;
  const int cols = screen.cols;
  const int text_cols = cols > 1 ? cols - 1 : 1;
  std::vector<std::string> lines;
  lines.reserve(static_cast<std::size_t>(rows));

  char head[160];
  std::snprintf(head, sizeof head, "%svocatron%s   group %s   LNA %d   VGA 20   amp off   %s",
                kCyan, kReset, kGroups[model.group], model.lna,
                model.hear ? "speaker on" : "muted");
  lines.push_back(clip_plain(head, text_cols + 16));

  if (screen.now == nullptr) {
    lines.emplace_back("not tuned");
    lines.emplace_back("");
    lines.emplace_back("enter plays the highlighted station");
    lines.emplace_back("");
    lines.emplace_back("");
    lines.emplace_back("");
  } else {
    const Channel &now = *screen.now;
    char title[160];
    std::snprintf(title, sizeof title, "%s%s%s   %s%.3f MHz%s   %s", kBold, now.name.c_str(),
                  kReset, kBold, static_cast<double>(now.hz) / 1.0e6, kReset, mode_label(now.mode));
    lines.push_back(title);
    lines.push_back(clip_plain(now.description, text_cols));

    const bool have_rf = screen.view != nullptr && screen.view->usb_bytes > 0;
    std::string gate = "tuning";
    const char *gate_color = kDim;
    if (!model.squelch_on) {
      gate = "gate off";
      gate_color = kBold;
    } else if (have_rf) {
      gate = screen.view->sql_open ? "OPEN" : "SHUT";
      gate_color = screen.view->sql_open ? kGreen : kDim;
    }
    char gate_line[120];
    std::snprintf(gate_line, sizeof gate_line, "sql %s%s%s   squelch %.0f dBFS%s", gate_color,
                  gate.c_str(), kReset, static_cast<double>(model.squelch_db),
                  model.hear ? "" : "   muted");
    lines.push_back(gate_line);

    const float channel_db = have_rf ? screen.view->channel_dbfs : -200.0f;
    const std::string bar = level_meter(channel_db, model.squelch_db, 32);
    char meter[160];
    if (have_rf) {
      std::snprintf(meter, sizeof meter, "ch %6.1f  [%s]", static_cast<double>(channel_db),
                    bar.c_str());
    } else {
      std::snprintf(meter, sizeof meter, "ch    --  [%s]", bar.c_str());
    }
    lines.push_back(meter);

    char stats[160];
    if (have_rf) {
      std::snprintf(stats, sizeof stats, "adc %6.1f   clip %5.2f%%   dropped %llu   underruns %llu",
                    static_cast<double>(screen.view->adc_dbfs),
                    static_cast<double>(screen.view->clip_pct),
                    static_cast<unsigned long long>(screen.view->dropped),
                    static_cast<unsigned long long>(screen.view->underruns));
    } else {
      std::snprintf(stats, sizeof stats, "waiting for samples");
    }
    lines.push_back(stats);
    lines.emplace_back("Gate shut is silence. = raises LNA when a voice is thin.");
  }

  lines.emplace_back("");
  lines.emplace_back("  station         MHz      mode  group");

  const int header_lines = static_cast<int>(lines.size());
  int list_rows = rows - header_lines - 3;
  if (list_rows < 3) {
    list_rows = 3;
  }
  int scroll = 0;
  if (model.selected >= list_rows) {
    scroll = model.selected - list_rows + 1;
  }
  const int count = static_cast<int>(screen.shown->size());
  for (int row = 0; row < list_rows; ++row) {
    const int index = scroll + row;
    if (index >= count) {
      lines.emplace_back("");
      continue;
    }
    const Channel &channel = (*screen.shown)[static_cast<std::size_t>(index)];
    const bool selected = index == model.selected;
    const bool playing = model.listening && index == model.playing;
    char row_text[160];
    std::snprintf(row_text, sizeof row_text, "%c %-14s %8.3f  %-4s %s", playing ? '>' : ' ',
                  channel.name.c_str(), static_cast<double>(channel.hz) / 1.0e6,
                  channel.mode.c_str(), channel.group.c_str());
    std::string line = clip_plain(row_text, text_cols);
    if (selected) {
      line = std::string(kRev) + line + kReset;
    }
    lines.push_back(std::move(line));
  }

  if (screen.message != nullptr && !screen.message->empty()) {
    lines.push_back(clip_plain(*screen.message, text_cols));
  } else {
    lines.emplace_back("");
  }
  lines.emplace_back("up/down move    enter tune    [ ] squelch    s gate    m mute");
  lines.emplace_back("-/= LNA    g group    esc stop    q quit");

  if (static_cast<int>(lines.size()) > rows) {
    lines.resize(static_cast<std::size_t>(rows));
  }

  std::string frame;
  frame.reserve(static_cast<std::size_t>(rows) * 80);
  frame += "\033[H";
  for (const std::string &line : lines) {
    frame += line;
    frame += "\033[K\n";
  }
  frame += "\033[J";
  return frame;
}

struct TickBag {
  TuiModel *model = nullptr;
  std::vector<Channel> *shown = nullptr;
  const ChannelBook *book = nullptr;
  Channel *now = nullptr;
  Term *term = nullptr;
  Live *live = nullptr;
  std::string *message = nullptr;
  bool *quit = nullptr;
  int rows = 24;
  int cols = 80;
  // [ ] and s keep the user's gate. A new station name clears it.
  bool squelch_manual = false;

  void note_squelch_key(TuiKey key) {
    if (key == TuiKey::SqlLoosen || key == TuiKey::SqlTighten || key == TuiKey::SqlToggle) {
      squelch_manual = true;
    }
  }

  void adopt(const Channel &next) {
    if (now->name != next.name) {
      squelch_manual = false;
    }
    *now = next;
    if (!squelch_manual) {
      model->squelch_on = true;
      model->squelch_db = squelch_for_mode(now->mode);
    }
    sync_live(*live, *model);
  }

  void apply_group() {
    const std::string keep =
        shown->empty() ? std::string() : (*shown)[static_cast<std::size_t>(model->selected)].name;
    const std::string playing = now->name;
    apply_tui_key(*model, TuiKey::Group);
    fill_shown(*book, model->group, *shown);
    model->count = static_cast<int>(shown->size());
    const int sel = find_name(*shown, keep);
    model->selected = sel >= 0 ? sel : 0;
    if (model->count == 0) {
      model->selected = 0;
    }
    if (model->listening) {
      model->playing = find_name(*shown, playing);
    }
  }

  bool on_view(const ListenView &view) {
    window_size(&rows, &cols);
    bool stop = false;
    for (;;) {
      const TuiKey key = term->read_key(0);
      if (key == TuiKey::None) {
        break;
      }
      note_squelch_key(key);
      TuiAction action = TuiAction::Stay;
      if (key == TuiKey::Group) {
        apply_group();
      } else {
        action = apply_tui_key(*model, key);
      }
      sync_live(*live, *model);
      if (action == TuiAction::Quit) {
        *quit = true;
        stop = true;
      } else if (action == TuiAction::Idle || action == TuiAction::Retune ||
                 action == TuiAction::Listen) {
        stop = true;
      }
    }
    Screen screen;
    screen.model = model;
    screen.shown = shown;
    screen.now = model->listening ? now : nullptr;
    screen.view = &view;
    screen.message = message;
    screen.rows = rows;
    screen.cols = cols;
    const std::string frame = draw_screen(screen);
    std::fwrite(frame.data(), 1, frame.size(), stdout);
    std::fflush(stdout);
    return !stop;
  }
};

bool tick_thunk(const ListenView &view, void *user) {
  return static_cast<TickBag *>(user)->on_view(view);
}

void paint(TickBag &bag, const ListenView *view) {
  window_size(&bag.rows, &bag.cols);
  Screen screen;
  screen.model = bag.model;
  screen.shown = bag.shown;
  screen.now = bag.model->listening && !bag.now->name.empty() ? bag.now : nullptr;
  screen.view = view;
  screen.message = bag.message;
  screen.rows = bag.rows;
  screen.cols = bag.cols;
  const std::string frame = draw_screen(screen);
  std::fwrite(frame.data(), 1, frame.size(), stdout);
  std::fflush(stdout);
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

  g_tui_stop = 0;
  TuiSignals signals;
  Term term;
  if (!term.enter()) {
    std::fprintf(stderr, "could not set the terminal\n");
    return 2;
  }

  std::vector<Channel> shown;
  TuiModel model;
  model.groups = kGroupCount;
  model.group = 0;
  model.squelch_on = true;
  model.hear = true;
  model.lna = 40;
  fill_shown(book, model.group, shown);
  model.count = static_cast<int>(shown.size());
  const int atis = find_name(shown, "LTFM_ATIS");
  const int pmr = find_name(shown, "PMR1");
  if (atis >= 0) {
    model.selected = atis;
  } else if (pmr >= 0) {
    model.selected = pmr;
  } else {
    model.selected = 0;
  }
  model.playing = model.selected;
  model.listening = model.count > 0;

  Channel now;
  Live live;
  std::string message;
  bool quit = false;

  TickBag bag;
  bag.model = &model;
  bag.shown = &shown;
  bag.book = &book;
  bag.now = &now;
  bag.term = &term;
  bag.live = &live;
  bag.message = &message;
  bag.quit = &quit;
  if (model.count > 0) {
    bag.adopt(shown[static_cast<std::size_t>(model.selected)]);
  } else {
    sync_live(live, model);
  }

  while (!quit && g_tui_stop == 0) {
    if (!model.listening || now.name.empty()) {
      paint(bag, nullptr);
      const TuiKey key = term.read_key(200);
      if (g_tui_stop != 0) {
        break;
      }
      if (key == TuiKey::None) {
        continue;
      }
      bag.note_squelch_key(key);
      TuiAction action = TuiAction::Stay;
      if (key == TuiKey::Group) {
        bag.apply_group();
      } else {
        action = apply_tui_key(model, key);
      }
      sync_live(live, model);
      if (action == TuiAction::Quit) {
        break;
      }
      if (action == TuiAction::Listen || action == TuiAction::Retune) {
        if (model.count > 0) {
          bag.adopt(shown[static_cast<std::size_t>(model.selected)]);
          model.playing = model.selected;
          model.listening = true;
          message.clear();
        }
      }
      continue;
    }

    paint(bag, nullptr);
    ListenConfig config;
    config.channel_hz = now.hz;
    config.name = now.name.c_str();
    config.mode = now.mode.c_str();
    config.group = now.group.c_str();
    config.seconds = 0;
    config.lna_db = static_cast<std::uint32_t>(model.lna);
    config.vga_db = 20;
    config.amp = false;
    config.speakers = true;
    config.squelch = model.squelch_on;
    config.squelch_dbfs = model.squelch_db;
    config.on_tick = tick_thunk;
    config.tick_user = &bag;
    config.squelch_enabled = &live.squelch_on;
    config.squelch_level = &live.squelch_db;
    config.pass_audio = &live.pass_audio;
    bool signaled = false;
    config.signal_stop = &signaled;

    const int rc = run_listen(config);
    if (g_tui_stop != 0 || signaled) {
      break;
    }
    if (quit) {
      break;
    }
    if (rc != 0) {
      model.listening = false;
      model.playing = -1;
      message = "Receiver stopped. Is the HackRF free? Enter tries again.";
      continue;
    }
    if (!model.listening) {
      continue;
    }
    if (model.playing >= 0 && model.playing < model.count) {
      bag.adopt(shown[static_cast<std::size_t>(model.playing)]);
    }
    message.clear();
  }
  return 0;
}

} // namespace vocatron
