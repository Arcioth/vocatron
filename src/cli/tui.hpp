#pragma once

namespace vocatron {

// Full-screen station list. Plays the selected channel through the speakers.
// Returns 0 when the user quits, 1 when the radio or the channel file failed,
// 2 when there is no terminal.
int run_tui();

} // namespace vocatron
