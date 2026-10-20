#include "EnginePCH.h"
#include "Engine/Platform/NativeFileDialog.h"

// The native file dialog on Windows: nativefiledialog-extended's IFileDialog backend on a thread of its own, in a
// single-threaded COM apartment (NativeFileDialog.h; Docs/Decisions/0029-m18-contract.md decisions 19 and 21).
//
// Open starts the dialog's thread, which calls NFD_Init, reports, shows the dialog unless it was cancelled meanwhile, and
// calls NFD_Quit; Open waits only for the report. Poll reads what the thread recorded. Cancel cannot reach the dialog's
// IFileDialog object, which lives in the other thread's apartment and which the library does not expose, so it closes the
// dialog's window the way its title bar's close button does: it posts WM_CLOSE to every dialog window (window class
// "#32770") of the dialog's thread, again and again until the thread ends, so a window the thread creates after the first
// attempt closes too. While it waits it dispatches the messages the dialog's thread sends to this thread's windows (an
// owned dialog enables and activates its owner when it closes, and SendMessage waits for the owner's thread), which would
// otherwise keep the dialog's thread from ending.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Core/Error.h"
	#include "Engine/Core/Log.h"
	#include "Engine/Platform/GlfwLibrary.h"
	#include "Engine/Platform/Private/PathsUtf8.h"
	#include "Engine/Platform/Window.h"

	// glfw3native.h declares glfwGetWin32Window only when GLFW_EXPOSE_NATIVE_WIN32 is defined before it (it includes
	// <windows.h> itself).
	#define GLFW_EXPOSE_NATIVE_WIN32
	#include <GLFW/glfw3.h>
	#include <GLFW/glfw3native.h>
	#include <nfd.h>
	#include <process.h>
	#include <windows.h>

	#include <algorithm>
	#include <array>
	#include <cerrno>
	#include <chrono>
	#include <condition_variable>
	#include <cstdint>
	#include <format>
	#include <memory>
	#include <mutex>
	#include <optional>
	#include <string>
	#include <string_view>
	#include <system_error>
	#include <utility>
	#include <vector>

namespace Engine {

	namespace {

		// How long Open waits for the dialog's thread to initialize, and Cancel for the dialog to close (NativeFileDialog.h).
		constexpr std::chrono::milliseconds DialogThreadBound{ 10000 };
		// Cancel's wait between two attempts to close the dialog's window.
		constexpr std::chrono::milliseconds CloseRetryInterval{ 10 };

		// What the calling thread and the dialog's thread share, under Mutex. The dialog's thread holds a reference until
		// it ends, so a thread left to end on its own (Open's or Cancel's bound passed) never touches freed memory.
		struct DialogThreadShared
		{
			std::mutex Mutex{};
			std::condition_variable Reported{};
			bool InitializationReported = false;        // NFD_Init returned on the dialog's thread
			std::optional<Error> InitializationError{}; // NFD_Init failed: Open fails with it
			bool CancelRequested = false;               // Cancel came before the dialog ended
			bool Finished = false;                      // the dialog ended: Outcome holds its result
			Result<FileDialogOutcome> Outcome = FileDialogOutcome{};
		};

		// The dialog's thread's start, which the thread owns: its copy of the request and its owner window.
		struct DialogThreadStart
		{
			Ref<DialogThreadShared> Shared{};
			FileDialogRequest Request{};
			HWND Owner = nullptr; // read on the dialog's thread only to pass it to the dialog
		};

		// Closes the dialog's thread handle.
		struct ThreadHandleCloser
		{
			void operator()(HANDLE handle) const { CloseHandle(handle); }
		};

		using ThreadHandle = std::unique_ptr<void, ThreadHandleCloser>;

		// Frees a path nativefiledialog-extended returned.
		struct DialogPathFreer
		{
			void operator()(nfdu8char_t* path) const { NFD_FreePathU8(path); }
		};

	}

	namespace Utils {

		// The window that owns the dialog: the HWND of `owner`'s GLFW window. None without an owner, and on GLFW's null
		// platform, whose windows have no HWND (glfwGetWin32Window would report a GLFW error there).
		static HWND GetDialogOwnerWindow(const Window* owner)
		{
			if (owner == nullptr || GlfwLibrary::GetPlatform() != GlfwPlatform::Win32)
				return nullptr;
			return glfwGetWin32Window(static_cast<GLFWwindow*>(owner->GetNativeHandle()));
		}

		// NFD_GetError's text after NFD_ERROR; the library keeps it as printable ASCII.
		static std::string GetDialogError()
		{
			const char* error = NFD_GetError();
			return error != nullptr ? std::string(error) : std::string("no error was reported");
		}

		// The filter of an OpenFile request: its extensions without their leading dots, comma-separated, which the library
		// turns into "*.eproj;*.test.luau".
		static std::string MakeDialogFilterSpec(const std::vector<std::string>& extensions)
		{
			std::string spec;
			for (const std::string& extension : extensions)
			{
				if (!spec.empty())
					spec += ',';
				spec += std::string_view(extension).substr(1);
			}
			return spec;
		}

		// The filter's label: the request's FilterName, or its extensions themselves.
		static std::string MakeDialogFilterName(const FileDialogRequest& request)
		{
			if (!request.FilterName.empty())
				return request.FilterName;
			std::string name;
			for (const std::string& extension : request.Extensions)
			{
				if (!name.empty())
					name += ", ";
				name += extension;
			}
			return name;
		}

		// Shows the dialog on the calling thread, which NFD_Init initialized, and returns its answer once the user (or
		// Cancel) closed it.
		static Result<FileDialogOutcome> ShowDialog(const FileDialogRequest& request, HWND owner)
		{
			const std::string defaultPath = NativePathToUtf8(request.InitialDirectory);
			nfdwindowhandle_t parent{};
			if (owner != nullptr)
			{
				parent.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
				parent.handle = owner;
			}

			nfdu8char_t* chosen = nullptr;
			nfdresult_t result = NFD_ERROR;
			if (request.Kind == FileDialogKind::PickFolder)
			{
				nfdpickfolderu8args_t arguments{};
				arguments.defaultPath = defaultPath.c_str();
				arguments.parentWindow = parent;
				arguments.title = request.Title.c_str();
				result = NFD_PickFolderU8_With(&chosen, &arguments);
			}
			else
			{
				const std::string filterName = MakeDialogFilterName(request);
				const std::string filterSpec = MakeDialogFilterSpec(request.Extensions);
				const nfdu8filteritem_t filter{ filterName.c_str(), filterSpec.c_str() };
				nfdopendialogu8args_t arguments{};
				arguments.filterList = request.Extensions.empty() ? nullptr : &filter;
				arguments.filterCount = request.Extensions.empty() ? 0u : 1u;
				arguments.defaultPath = defaultPath.c_str();
				arguments.parentWindow = parent;
				arguments.title = request.Title.c_str();
				result = NFD_OpenDialogU8_With(&chosen, &arguments);
			}

			switch (result)
			{
				case NFD_OKAY:
				{
					const std::unique_ptr<nfdu8char_t, DialogPathFreer> path(chosen);
					return FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = NativePathFromUtf8(path.get()) };
				}
				case NFD_CANCEL:
					return FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} };
				case NFD_ERROR:
					break;
			}
			return MakeError(ErrorCode::Io, "the native file dialog failed: {}", GetDialogError());
		}

		// The dialog's thread (_beginthreadex). It owns `argument`, a DialogThreadStart.
		static unsigned __stdcall RunDialogThread(void* argument)
		{
			const Scope<DialogThreadStart> start(static_cast<DialogThreadStart*>(argument));
			DialogThreadShared& shared = *start->Shared;
			const bool initialized = NFD_Init() == NFD_OKAY;
			std::optional<Error> failure;
			if (!initialized)
			{
				failure = Error(ErrorCode::Io, std::format("NFD_Init failed: {}", GetDialogError()))
							  .WithHint("the dialog's thread cannot create the single-threaded COM apartment IFileDialog needs");
			}
			bool show = false;
			{
				const std::lock_guard lock(shared.Mutex);
				if (failure.has_value())
				{
					shared.InitializationError = std::move(failure);
					shared.Finished = true;
				}
				shared.InitializationReported = true;
				show = initialized && !shared.CancelRequested;
			}
			shared.Reported.notify_all();
			if (!initialized)
				return 0;

			Result<FileDialogOutcome> outcome = show ? ShowDialog(start->Request, start->Owner)
													 : Result<FileDialogOutcome>(FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} });
			NFD_Quit();
			const std::lock_guard lock(shared.Mutex);
			shared.Outcome = std::move(outcome);
			shared.Finished = true;
			return 0;
		}

		// EnumThreadWindows' callback: asks a dialog window to close, as its title bar's close button would (a file dialog
		// then answers as cancelled). Posting never waits for the dialog's thread; a window that is gone meanwhile only
		// makes the post fail.
		static BOOL CALLBACK CloseDialogWindow(HWND window, LPARAM /*parameter*/)
		{
			std::array<wchar_t, 16> className{};
			const int length = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
			if (length > 0 && std::wstring_view(className.data(), static_cast<size_t>(length)) == L"#32770")
				PostMessageW(window, WM_CLOSE, 0, 0);
			return TRUE;
		}

	}

	struct NativeFileDialog::State
	{
		Ref<DialogThreadShared> Shared = CreateRef<DialogThreadShared>();
		ThreadHandle Thread{}; // until the thread ended and Cancel saw it end, or it was left to end on its own
		DWORD ThreadId = 0;
	};

	NativeFileDialog::NativeFileDialog(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	NativeFileDialog::~NativeFileDialog()
	{
		Cancel();
	}

	Result<Scope<NativeFileDialog>> NativeFileDialog::Open(const FileDialogRequest& request, const Window* owner)
	{
		ENGINE_TRY(ValidateFileDialogRequest(request));
		Scope<NativeFileDialog> dialog = CreateScope<NativeFileDialog>(ConstructionKey{});
		State& state = *dialog->m_State;

		Scope<DialogThreadStart> start = CreateScope<DialogThreadStart>();
		start->Shared = state.Shared;
		start->Request = request;
		start->Owner = Utils::GetDialogOwnerWindow(owner);
		DialogThreadStart* argument = start.release();
		unsigned threadId = 0;
		const uintptr_t thread = _beginthreadex(nullptr, 0, &Utils::RunDialogThread, argument, 0, &threadId);
		if (thread == 0)
		{
			const int error = errno;
			start.reset(argument);
			return MakeError(ErrorCode::Io, "cannot start the native file dialog's thread: {}", std::generic_category().message(error));
		}
		state.Thread.reset(reinterpret_cast<HANDLE>(thread));
		state.ThreadId = static_cast<DWORD>(threadId);

		DialogThreadShared& shared = *state.Shared;
		std::unique_lock lock(shared.Mutex);
		if (!shared.Reported.wait_for(lock, DialogThreadBound, [&shared]()
		{
			return shared.InitializationReported;
		}))
		{
			// Told not to show the dialog, and left to end on its own: it holds its own reference to what it shares.
			shared.CancelRequested = true;
			lock.unlock();
			state.Thread.reset();
			return MakeError(ErrorCode::Io, "the native file dialog's thread did not initialize within {} s",
				std::chrono::duration_cast<std::chrono::seconds>(DialogThreadBound).count());
		}
		if (shared.InitializationError.has_value())
		{
			Error error = *shared.InitializationError;
			lock.unlock();
			// The thread ends right after its report; the dialog's destructor waits for it.
			return std::unexpected(std::move(error));
		}
		lock.unlock();
		return dialog;
	}

	Result<FileDialogOutcome> NativeFileDialog::Poll()
	{
		DialogThreadShared& shared = *m_State->Shared;
		const std::lock_guard lock(shared.Mutex);
		if (shared.CancelRequested)
			return FileDialogOutcome{ .State = FileDialogState::Cancelled, .Path = {} };
		if (!shared.Finished)
			return FileDialogOutcome{};
		return shared.Outcome;
	}

	void NativeFileDialog::Cancel()
	{
		State& state = *m_State;
		{
			const std::lock_guard lock(state.Shared->Mutex);
			if (!state.Shared->Finished)
				state.Shared->CancelRequested = true;
		}
		if (!state.Thread)
			return;

		HANDLE thread = state.Thread.get();
		const auto deadline = std::chrono::steady_clock::now() + DialogThreadBound;
		for (;;)
		{
			if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0)
			{
				state.Thread.reset();
				return;
			}
			// The thread may not have created its window yet, or may be between two windows: every attempt looks again.
			EnumThreadWindows(state.ThreadId, &Utils::CloseDialogWindow, 0);
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				break;
			const auto slice = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now), CloseRetryInterval);
			// Wakes when the thread ends or another thread sends a message to this thread's windows, and dispatches those
			// sent messages (PM_QS_SENDMESSAGE leaves posted ones queued for the window's own event loop). A failed wait
			// or an empty queue only means the next attempt comes after the slice.
			MsgWaitForMultipleObjectsEx(1, &thread, static_cast<DWORD>(slice.count()), QS_SENDMESSAGE, 0);
			MSG message{};
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		ENGINE_CORE_ERROR("NativeFileDialog: the native file dialog did not close within {} s (a shell extension may hang inside it); "
						  "its thread is left to end with the process",
			std::chrono::duration_cast<std::chrono::seconds>(DialogThreadBound).count());
		state.Thread.reset();
	}

}

#endif
