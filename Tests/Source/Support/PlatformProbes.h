#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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
