#include "TestsPCH.h"
#include "Engine/Platform/SharedLibrary.h"

#include "Engine/Core/FileSystem.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace Engine {

	TEST_SUITE("Platform")
	{
		// Docs/Decisions/0030-m19-contract.md decision 34 (ADR 0028 decision 10, Windows backends only): the one platform
		// check every native capability refuses through. The contract implements it: live.
		TEST_CASE("SharedLibrary: native modules are supported on Windows only")
		{
			const Status supported = SharedLibrary::CheckSupported();
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(supported.has_value());
#else
			REQUIRE_FALSE(supported.has_value());
			CHECK(supported.error().GetCode() == ErrorCode::Unsupported);
#endif
		}

		// Decision 31 (N-C): a library loads by its absolute path, resolves what it exports, refuses what it does not, and
		// unloads with its object; on Linux and macOS every load is Unsupported.
		TEST_CASE("SharedLibrary: a library loads, resolves an exported function, refuses a missing one and unloads")
		{
			const Test::SystemLibraryProbe probe = Test::GetSystemLibraryProbe();
#if defined(ENGINE_PLATFORM_WINDOWS)
			REQUIRE_FALSE(probe.Library.empty());
			Result<Scope<SharedLibrary>> library = SharedLibrary::Load(probe.Library);
			REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
			CHECK((*library)->GetPath() == probe.Library);
			const Result<void*> found = (*library)->FindSymbol(probe.Symbol);
			REQUIRE_MESSAGE(found.has_value(), found.error().ToString());
			CHECK(*found != nullptr);
			const Result<void*> missing = (*library)->FindSymbol("EngineNoSuchFunction");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			library->reset();

			const Result<Scope<SharedLibrary>> relative = SharedLibrary::Load(probe.Library.filename());
			REQUIRE_FALSE(relative.has_value());
			CHECK(relative.error().GetCode() == ErrorCode::InvalidArgument);
			const Test::TempDirectory directory("SharedLibrary");
			const Result<Scope<SharedLibrary>> absent = SharedLibrary::Load(directory / "Absent.dll");
			REQUIRE_FALSE(absent.has_value());
			CHECK(absent.error().GetCode() == ErrorCode::NotFound);
#else
			const Result<Scope<SharedLibrary>> refused = SharedLibrary::Load(std::filesystem::path("/usr/lib/libm.so"));
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
			CHECK(probe.Library.empty());
#endif
		}

		// Decision 31 (N-C): a file the system cannot load as a library is refused, naming it, and nothing stays loaded.
		TEST_CASE("SharedLibrary: a file that is not a library is refused with Io, naming it")
		{
			const Test::TempDirectory directory("SharedLibraryNotALibrary");
			const std::filesystem::path file = directory / "NotALibrary.dll";
			const std::string_view text = "not a library\n";
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			const Result<Scope<SharedLibrary>> refused = SharedLibrary::Load(file);
			REQUIRE_FALSE(refused.has_value());
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(refused.error().GetCode() == ErrorCode::Io);
			CHECK(refused.error().GetMessageText().contains("NotALibrary.dll"));
#else
			CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
#endif
			// The file is not held: it can be removed at once.
			CHECK(FileSystem::Remove(file).has_value());
		}
	}

}
