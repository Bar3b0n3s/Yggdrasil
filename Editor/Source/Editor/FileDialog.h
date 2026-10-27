#pragma once

#include "Editor/FolderPicker.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/NativeFileDialog.h"

#include <filesystem>
#include <functional>
#include <optional>

// The editor's FileDialog API (M18, C18.1, E-DIALOGS; Docs/Decisions/0029-m18-contract.md decisions 19 and 20): one
// open-file or pick-folder dialog per instance, shown natively through an injected backend when there is one and it can
// show the request, otherwise by the ImGui picker (FolderPicker), which therefore serves headless editors, tests, editors
// without a native backend and Linux and macOS (where the native backend is Unsupported). Every editor dialog that picks a
// path goes through it: the launcher's project location and project file, the content browser's import source, and every
// later editor tool (ADR 0028's phase-1 ledger: the FileDialog API is M18's file; M24 owns it in phase 3).
//
// The dialogs are presentation (decision 20): each fills a path into a field whose action is an automation method that
// takes the path (project.create {path}, project.open {path}, asset.import {source}), so a dialog adds no capability and
// has no method or Python test of its own; this API is tested through an injected backend.
//
// Main thread only, inside an ImGui frame for Draw; one instance per dialog site, owning its request, its picker and its
// native session.

namespace Engine {

	class Window;

	// A native dialog shown for one FileDialog request (the editor's backend wraps NativeFileDialog; tests script one).
	class IFileDialogSession
	{
	public:
		virtual ~IFileDialogSession() = default;

		// Pending while the dialog is shown, then its outcome, which every later poll reports again. Never blocks. Errors: the
		// dialog's own (the FileDialog reports them and closes the dialog).
		[[nodiscard]] virtual Result<FileDialogOutcome> Poll() = 0;

		// Closes a dialog still shown, as the user's cancel would; Poll then reports Cancelled unless it was answered first.
		// Idempotent.
		virtual void Cancel() = 0;
	};

	// Starts a native dialog for a request ValidateFileDialogRequest accepted. Errors: Unsupported when this platform has no
	// native dialog (Linux, macOS); any other error when the dialog could not start. On either the FileDialog shows the
	// request in its ImGui picker instead, logging a warning for an error other than Unsupported.
	using FileDialogBackend = std::function<Result<Scope<IFileDialogSession>>(const FileDialogRequest& request)>;

	// The editor's native backend: NativeFileDialog::Open with `owner` as the dialog's owner window. `owner` is a documented
	// back-reference that outlives every dialog the backend starts (the editor's main window). EditorApp puts it in
	// EditorPanelContext::FileDialogs only in a windowed editor; headless editors and tests put none there.
	[[nodiscard]] FileDialogBackend MakeNativeFileDialogBackend(const Window& owner);

	class FileDialog
	{
	public:
		// Opens a dialog for `request`, closing the one open before (a native one through its session), and returns without
		// waiting for an answer (a native dialog's start waits only for its thread's initialization, NativeFileDialog::Open).
		// The request must pass ValidateFileDialogRequest and its InitialDirectory must be a readable directory. With a
		// `backend`, the request is offered to it first: a session it returns is the open dialog; its error (Unsupported on
		// Linux and macOS) leaves the request to the ImGui picker. Without one, the ImGui picker shows it. Errors, which keep
		// the dialog open before as it was: InvalidArgument from ValidateFileDialogRequest; NotFound or Io for an
		// InitialDirectory that is not a readable directory (the launcher walks up to an existing ancestor, as it does now).
		[[nodiscard]] Status Open(const FileDialogRequest& request, const FileDialogBackend& backend = {});

		// Once per frame while open. The ImGui picker is drawn (FolderPicker::Draw); a native dialog is polled and nothing is
		// drawn. Returns nullopt while the dialog is pending and when it was cancelled (then it is closed), and the accepted
		// path once, revalidated as the ImGui picker revalidates it: canonical and absolute, an existing directory for
		// PickFolder, an existing regular file whose name ends with one of the request's extensions (ignoring ASCII case)
		// for OpenFile; the dialog is closed then. Errors: the ImGui picker's, which leave it open as now; for a native dialog
		// its session's error, or NotFound or InvalidArgument for a path that fails the revalidation, after which the dialog
		// is closed (its site shows the error).
		[[nodiscard]] Result<std::optional<std::filesystem::path>> Draw();

		// Closes the open dialog: the ImGui picker, or a native one through its session's Cancel. Idempotent.
		void Cancel();

		[[nodiscard]] bool IsOpen() const;
		// Whether the open dialog is a native one (false for the ImGui picker and when none is open).
		[[nodiscard]] bool IsNative() const;
	private:
		FileDialogRequest m_Request{};
		FolderPicker m_Picker{};
		Scope<IFileDialogSession> m_Native{};
	};

}
