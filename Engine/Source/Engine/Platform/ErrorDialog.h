#pragma once

#include "Engine/Core/Base.h"

#include <string_view>

// The modal error dialog of a windowed process (Architecture §4.6 "message box when windowed", §8.1 "No loader -> exit 3
// with a readable message (and a message box when windowed)", §14.3: a Dist Runtime has no console). ProcessContext's
// fatal-error handler shows it as its step 4, and RunApplication and Application::Run show it for an initialization
// failure, in windowed processes only (ProcessContextSpecification::ShowErrorDialogs). The OS part lives in
// Platform/Windows/ErrorDialogWindows.cpp (MessageBoxW) and Platform/Posix/ErrorDialogPosix.cpp
// (CFUserNotificationDisplayAlert on macOS; nothing on Linux, where X11 has no standard dialog and the message stays in
// the log and on stderr). It needs no window and no GLFW, so it also works when GLFW failed to initialize.

namespace Engine {

	// Shows `message` under `title` (UTF-8) and blocks the calling thread until the user dismisses it. Returns whether a
	// dialog was shown: false on Linux and when the OS call fails. Never called by headless processes, tests or child
	// processes, which must never block on user input. Callable from any thread; the fatal-error handler calls it on the
	// failing thread.
	bool ShowErrorDialog(std::string_view title, std::string_view message);

}
