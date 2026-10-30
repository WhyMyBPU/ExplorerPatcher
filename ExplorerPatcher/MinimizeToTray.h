#ifndef _H_MINIMIZETOTRAY_H_
#define _H_MINIMIZETOTRAY_H_
#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimize any window to the notification area (system tray), in the spirit of RBTray
// (https://github.com/benbuck/rbtray, GPL-2.0):
//   - right-click a window's minimize button
//   - Shift + right-click a window's title bar
//   - Ctrl + Alt + Down minimizes the foreground window
// Click the tray icon to restore the window; right-click it for more options.
//
// Only call this from the shell (desktop/taskbar) explorer.exe instance. It is safe to call
// repeatedly; turning the feature off restores every window it hid. Windows hidden by a
// previous Explorer instance (e.g. after Explorer crashed/restarted) are picked up again,
// or shown again if the feature is now off.
void MinimizeToTray_SetEnabled(BOOL bEnabled);

#ifdef __cplusplus
}
#endif

#endif
