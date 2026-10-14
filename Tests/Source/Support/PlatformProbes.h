#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// OS facilities that platform tests need and the engine deliberately does not offer: they reproduce a fault, or an
// observer, that engine code must cope with. PlatformProbes.cpp is the only Tests file with OS headers (its own rule in
// Scripts/ModuleRules.json, ADR 0005 decision 25); every other test uses the standard library and the engine's API.

namespace Engine {

	namespace Test {

		// Starts `executable` with `arguments` (UTF-8; none may contain '"' or end with '\', which would need escaping on
		// Windows) and returns without waiting for it or reaping it. Unlike Process, the new process inherits this
		// process's standard output and standard error (on POSIX also its standard input and every descriptor without
		// close-on-exec), the way a tool that starts a helper process passes its own pipes on. Errors: InvalidArgument for
		// an argument that would need escaping; Io when it cannot be started.
		[[nodiscard]] Status StartProcessInheritingOutput(const std::filesystem::path& executable, std::span<const std::string> arguments);

#if defined(ENGINE_PLATFORM_WINDOWS)
		// Takes the lock of the process heap (HeapLock(GetProcessHeap())) and keeps it: every other thread that allocates
		// from the process heap blocks, as after an access violation inside HeapAlloc on a corrupt heap. The calling thread
		// can still allocate (the lock is recursive). For a crash child that crashes right after. Errors: Io.
		[[nodiscard]] Status LockProcessHeap();

		// The content of `file`, read through a handle that denies writers (FILE_SHARE_READ), as .NET's File.ReadAllText
		// and _wfsopen with _SH_DENYWR do, so opening it fails while any other handle to the file has write access.
		// Errors: Io.
		[[nodiscard]] Result<std::string> ReadFileDenyingWriters(const std::filesystem::path& file);

		// The bytes of stack the calling thread keeps for its own stack overflow (SetThreadStackGuarantee's current
		// value); 0 when the thread keeps only the system's guard region. Errors: Io when the query fails.
		[[nodiscard]] Result<uint32_t> GetThreadStackGuarantee();

		// GetThreadStackGuarantee() on a new thread (CreateThread) whose stack reserves `stackBytes`, read as soon as the
		// thread runs. Errors: Io when the thread cannot be started, and the errors of GetThreadStackGuarantee.
		[[nodiscard]] Result<uint32_t> GetNewThreadStackGuarantee(size_t stackBytes);

		// Releases pages that VirtualAlloc reserved and committed.
		struct PageReleaser
		{
			void operator()(char* pages) const;
		};

		// Text whose last bytes fault when read, and the pages that hold it (released with the object).
		struct UnreadableText
		{
			std::unique_ptr<char, PageReleaser> Pages{};
			std::string_view Text{};
		};

		// Text of `readableBytes` 'x' characters followed by `unreadableBytes` (at most one page) that fault when read: the
		// pages are committed and filled, then the page after the readable bytes is made inaccessible (VirtualProtect with
		// PAGE_NOACCESS), so only the OS's access check sets them apart from ordinary memory. For a crash child that hands
		// the text to code that reads it. Errors: InvalidArgument when `unreadableBytes` is 0 or more than a page; Io.
		[[nodiscard]] Result<UnreadableText> AllocateTextEndingInUnreadableBytes(size_t readableBytes, size_t unreadableBytes);

		// An executable's resources as Windows itself reads them: the independent oracle of the resource patcher's acceptance
		// test (Engine/Platform/ResourcePatcher.h; Docs/Decisions/0024-m15-contract.md decision 4).
		struct ExecutableResourceProbe
		{
			std::array<uint16_t, 4> FileVersion{};            // VS_FIXEDFILEINFO dwFileVersionMS/LS, most significant first
			std::array<uint16_t, 4> ProductVersion{};         // dwProductVersionMS/LS
			std::map<std::string, std::string> Strings{};     // every present key of \StringFileInfo\040904B0, UTF-8
			std::vector<std::pair<uint16_t, Buffer>> Icons{}; // RT_GROUP_ICON 1's entries in order: size (0 read as 256), RT_ICON bytes
			bool HasManifest = false;                         // an RT_MANIFEST resource with id 1
		};

		// Reads `executable`'s version resource with GetFileVersionInfoW and VerQueryValueW (for the keys CompanyName,
		// FileDescription, FileVersion, InternalName, OriginalFilename, ProductName and ProductVersion), and its icon group
		// and manifest with LoadLibraryExW(LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE), FindResourceW and
		// LoadResource. A missing version resource leaves the versions zero and Strings empty; a missing icon group leaves
		// Icons empty. Errors: Io naming the failing call.
		[[nodiscard]] Result<ExecutableResourceProbe> ProbeExecutableResources(const std::filesystem::path& executable);

		// Delivers a press (`pressed`) or a release of `key` to the visible GLFW window of the process `processId` as the OS
		// delivers a keystroke: a WM_KEYDOWN or WM_KEYUP message (PostMessageW) carrying the key's virtual-key code and scan
		// code, which GLFW's window procedure turns into that press or release whether or not the window has the focus. For
		// tests of a windowed process that must receive real key events (the Runtime's F1 overlay, Docs/Decisions/
		// 0024-m15-contract.md decision 17). Keys: F1 to F12. Errors: InvalidArgument for another key; NotFound when the
		// process has no visible GLFW window (yet); Io when the message cannot be posted.
		[[nodiscard]] Status PostKeyToProcessWindow(uint32_t processId, Key key, bool pressed);
#else
		// Opens `file` for reading without close-on-exec, as a library that calls fopen does, and returns the descriptor:
		// `minimum` or the lowest free one above it, so that a child process is unlikely to reuse the number for a
		// descriptor of its own. Errors: Io.
		[[nodiscard]] Result<int> OpenInheritableDescriptor(const std::filesystem::path& file, int minimum);

		// Whether `descriptor` is open in this process (fcntl F_GETFD).
		[[nodiscard]] bool IsDescriptorOpen(int descriptor);

		void CloseDescriptor(int descriptor);
#endif

	}

}
