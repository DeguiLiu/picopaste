// Resource ids shared by picopaste.rc (which defines the icons) and tray.cpp
// (which loads them). Kept in one place so the two cannot drift.
//
// The lowest id must stay the executable's own icon: the shell takes the
// lowest-numbered ICON resource as the file's icon, so PICOPASTE_ICON_OK is
// also what Explorer, the taskbar and Alt-Tab show.
#pragma once

#define PICOPASTE_ICON_OK 100    // healthy: blue clipboard
#define PICOPASTE_ICON_WARN 101  // degraded / retrying: amber
#define PICOPASTE_ICON_ERR 102   // failed / stopped: red
