#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Native file dialogs (M18, C18.1, E-DIALOGS; Docs/Decisions/0029-m18-contract.md decisions 19 to 21): the operating
// system's open-file and pick-folder dialogs, for the editor's FileDialog API (Editor/FileDialog.h), which shows its ImGui
// picker wherever this layer has no dialog. Windows backends only (ADR 0028 decision 10): on Windows the dialog is
// nativefiledialog-extended's IFileDialog backend (Platform/Windows/NativeFileDialogWindows.cpp, the only part of that
// library ever compiled); on Linux and macOS NativeFileDialog::Open returns ErrorCode::Unsupported naming the platform
// (Platform/Posix/NativeFileDialogPosix.cpp), as ResourcePatcher::Patch does there, and no portal or AppKit backend is
// written. The request and outcome types are shared with the editor's API, so the native dialog and the ImGui picker take
// the same request and give the same kind of answer.
//
// A native dialog runs on a thread of its own and is polled: Open returns at once and the main thread keeps running frames
// while the user picks (the automation server, the busy watchdog and a play session keep going), and the dialog's thread
// gets the single-threaded COM apartment IFileDialog needs, which the main thread cannot give it once miniaudio's WASAPI
// backend has initialized COM there as multithreaded (Docs/Decisions/0015-m12-decisions.md). Headless processes, tests and
// child processes never open one: they must never wait for a user (ShowErrorDialog's rule).

namespace Engine {

	class Window;

	// What a dialog lets the user choose.
	enum class FileDialogKind : uint8_t
	{
		OpenFile,  // one existing file, of the request's Extensions when it lists any
		PickFolder // one existing folder
	};

	// One dialog's request; ValidateFileDialogRequest states its rules.
	struct FileDialogRequest
	{
		FileDialogKind Kind = FileDialogKind::OpenFile;
		std::string Title{}; // the dialog's caption, UTF-8
		// Absolute: the folder the dialog starts in when the operating system remembers no folder the user last chose in
		// such a dialog (Windows' IFileDialog::SetDefaultFolder: nativefiledialog-extended is built without
		// NFD_OVERRIDE_RECENT_WITH_DEFAULT, Vendor/nativefiledialog-extended/VENDOR.md); the ImGui picker always starts there.
		std::filesystem::path InitialDirectory{};
		// OpenFile: the accepted suffixes, lower case with their leading dot (".eproj", ".test.luau"), matched without regard
		// to ASCII case; empty: every file. PickFolder: none.
		std::vector<std::string> Extensions{};
		// OpenFile: the native dialog's label for Extensions ("Projects"); empty: the extensions themselves. PickFolder: empty.
		std::string FilterName{};

		bool operator==(const FileDialogRequest&) const = default;
	};

	// Where a dialog stands.
	enum class FileDialogState : uint8_t
	{
		Pending,  // still shown
		Accepted, // the user chose Path
		Cancelled // the user closed it without choosing, or it was cancelled
	};

	// What polling a dialog reports.
	struct FileDialogOutcome
	{
		FileDialogState State = FileDialogState::Pending;
		std::filesystem::path Path{}; // Accepted: the chosen file or folder as the dialog returned it, absolute; else empty

		bool operator==(const FileDialogOutcome&) const = default;
	};

	// The limits ValidateFileDialogRequest enforces, in UTF-8 bytes and entries.
	inline constexpr size_t MaxFileDialogTitleBytes = 256;
	inline constexpr size_t MaxFileDialogFilterNameBytes = 128;
	inline constexpr size_t MaxFileDialogExtensionBytes = 32;
	inline constexpr size_t MaxFileDialogExtensions = 64;

	// The rules of a request, the same on every platform and for both of the editor's dialogs, checked before anything is
	// shown: a Title of 1 to MaxFileDialogTitleBytes bytes of valid UTF-8 without control characters; an absolute
	// InitialDirectory (whether it exists is the caller's check: the FileDialog refuses a missing one, and a native dialog
	// that cannot start there starts in its default folder); for OpenFile at most MaxFileDialogExtensions unique extensions,
	// each a '.' followed by one or more '.'-separated runs of lower-case ASCII letters and digits, at most
	// MaxFileDialogExtensionBytes bytes, and a FilterName of at most MaxFileDialogFilterNameBytes bytes of valid UTF-8
	// without control characters; for PickFolder no extensions and an empty FilterName. Pure and thread-safe. Errors:
	// InvalidArgument naming the field and the rule.
	[[nodiscard]] Status ValidateFileDialogRequest(const FileDialogRequest& request);

	// One native dialog while it is shown. Created by Open on the thread that then polls and cancels it (the main thread);
	// not copyable or movable.
	class NativeFileDialog
	{
	public:
		// Restricts construction to Open; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class NativeFileDialog;
		};

		// Use Open.
		explicit NativeFileDialog(ConstructionKey key);
		// Cancel, then releases the dialog's thread.
		~NativeFileDialog();

		NativeFileDialog(const NativeFileDialog&) = delete;
		NativeFileDialog& operator=(const NativeFileDialog&) = delete;

		// Shows the operating system's dialog for `request` on a thread of its own. Open returns once that thread has
		// initialized (NFD_Init, its single-threaded COM apartment), waiting for it within a bounded 10 s, and before the
		// dialog is answered: the dialog shows, and the user answers it, while the caller keeps running. `owner` (a
		// documented back-reference, read only during Open; the editor's main window) makes the dialog modal to that window,
		// which the system disables while the dialog shows; null shows it on its own. Windows: nativefiledialog-extended's
		// IFileDialog backend (NFD_Init, then NFD_OpenDialogU8_With or NFD_PickFolderU8_With with the title, the default path,
		// one filter item of the extensions and the owner's native handle, then NFD_Quit, all on the dialog's thread).
		// Errors: InvalidArgument from ValidateFileDialogRequest (nothing shown); Unsupported naming the platform on Linux
		// and macOS; Io when the dialog cannot start, so the caller falls back to its own picker: the thread cannot be
		// created, NFD_Init fails (naming NFD_GetError), or the thread reports nothing within the bound (it is told not to
		// show the dialog and left to end on its own).
		[[nodiscard]] static Result<Scope<NativeFileDialog>> Open(const FileDialogRequest& request, const Window* owner = nullptr);

		// Pending while the dialog is shown; then its outcome, which every later poll reports again. Never blocks. Errors: Io
		// naming the dialog's own error (nativefiledialog-extended's NFD_GetError), reported by every later poll too.
		[[nodiscard]] Result<FileDialogOutcome> Poll();

		// Closes the dialog, as the user's cancel would, and waits for its thread within a bounded time (10 s); afterwards
		// Poll reports Cancelled, unless the dialog had already been answered. The dialog's window may not exist yet (Cancel
		// right after Open): Cancel marks the dialog cancelled, so a thread that has not shown it yet does not show it, and
		// until the thread ends it keeps closing the dialog's window whenever that window exists (on Windows from the
		// calling thread, posting the close again until the thread ends), all within the one bound. A dialog that does not
		// close within that time (a shell extension hung inside it) is left to end with the process, and the failure is
		// logged as an error. Idempotent.
		void Cancel();
	private:
		// The dialog's thread, its request and its outcome (each platform's file).
		struct State;
	private:
		Scope<State> m_State;
	};

}
