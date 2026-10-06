#include "TestsPCH.h"

#include "Support/PlatformProbes.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

#include <fstream>

// The probes that reproduce a fault are exercised by the tests that need them: LockProcessHeap by "CrashHandler: a crash
// that leaves the heap locked still exits with code 4", StartProcessInheritingOutput by "Process: Run reports output that
// a process the child started keeps open, without killing anything", GetThreadStackGuarantee and
// GetNewThreadStackGuarantee by "CrashHandler: threads started while the handler is installed keep 64 KiB of stack for
// their own overflow on Windows", and the bytes of AllocateTextEndingInUnreadableBytes that fault by "CrashHandler: a
// fault inside the crash handler still prints the line and exits with code 4 on Windows".

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("PlatformProbes: StartProcessInheritingOutput refuses arguments that would need escaping")
		{
			const std::array<std::string, 2> quoted = { "--child-argument=a\"b", "--no-skip" };
			const Status refusedQuote = Test::StartProcessInheritingOutput(Test::GetTestOptions().ExecutablePath, quoted);
			REQUIRE_FALSE(refusedQuote.has_value());
			CHECK(refusedQuote.error().GetCode() == ErrorCode::InvalidArgument);

			const std::array<std::string, 1> trailingBackslash = { "--child-argument=a\\" };
			const Status refusedBackslash = Test::StartProcessInheritingOutput(Test::GetTestOptions().ExecutablePath, trailingBackslash);
			REQUIRE_FALSE(refusedBackslash.has_value());
			CHECK(refusedBackslash.error().GetCode() == ErrorCode::InvalidArgument);
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("PlatformProbes: ReadFileDenyingWriters reads a file and fails while a writer has it open")
		{
			Test::TempDirectory directory("ProbesDenyWrite");
			const std::filesystem::path file = directory / "Text.txt";
			const std::string text = "denying writers";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(text)).has_value());

			const Result<std::string> read = Test::ReadFileDenyingWriters(file);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(*read == text);

			std::ofstream writer(file, std::ios::binary | std::ios::app);
			REQUIRE(writer.is_open());
			const Result<std::string> refused = Test::ReadFileDenyingWriters(file);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("PlatformProbes: AllocateTextEndingInUnreadableBytes gives text of the requested length that starts readable")
		{
			const Result<Test::UnreadableText> text = Test::AllocateTextEndingInUnreadableBytes(100, 16);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->Text.size() == 116);
			// Only the readable start is read here; the rest would crash this process.
			CHECK(text->Text.substr(0, 100) == std::string(100, 'x'));

			const Result<Test::UnreadableText> none = Test::AllocateTextEndingInUnreadableBytes(100, 0);
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Test::UnreadableText> tooMany = Test::AllocateTextEndingInUnreadableBytes(0, 1024 * 1024);
			REQUIRE_FALSE(tooMany.has_value());
			CHECK(tooMany.error().GetCode() == ErrorCode::InvalidArgument);
		}
#else
		TEST_CASE("PlatformProbes: OpenInheritableDescriptor opens at or above its minimum until closed")
		{
			Test::TempDirectory directory("ProbesDescriptor");
			const std::filesystem::path file = directory / "Text.txt";
			const std::string text = "descriptor";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(text)).has_value());

			const Result<int> descriptor = Test::OpenInheritableDescriptor(file, 200);
			REQUIRE_MESSAGE(descriptor.has_value(), descriptor.error().ToString());
			CHECK(*descriptor >= 200);
			CHECK(Test::IsDescriptorOpen(*descriptor));
			Test::CloseDescriptor(*descriptor);
			CHECK_FALSE(Test::IsDescriptorOpen(*descriptor));

			const Result<int> missing = Test::OpenInheritableDescriptor(directory / "Missing.txt", 200);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::Io);
		}
#endif
	}

}
