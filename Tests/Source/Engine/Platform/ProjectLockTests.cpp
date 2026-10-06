#include "TestsPCH.h"

#include "Engine/Platform/ProjectLock.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildProcess.h"
#include "Support/DeathTest.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <fstream>

namespace Engine {

	// Takes the lock named by --child-argument, announces it on stderr, then blocks until killed: the holder process of
	// the Roadmap test.
	ENGINE_DEATH_TEST("Platform/HoldsProjectLock")
	{
		Result<ProjectLock> lock = ProjectLock::Acquire(Test::PathFromUtf8(Test::GetTestOptions().ChildArgument));
		if (!lock.has_value())
		{
			ENGINE_CORE_ERROR("The lock holder could not take the lock: {}", lock.error());
			return;
		}
		ENGINE_CORE_WARN("Project lock held by process {}", Process::GetCurrentId());
		Test::BlockUntilKilled();
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("ProjectLock: a second lock fails and names the pid; killing the holder releases it")
		{
			Test::TempDirectory directory("ProjectLock");
			const std::filesystem::path lockFile = directory / "Editor.lock";

			// In this process: locks are per opened file, so a second Acquire fails too.
			{
				Result<ProjectLock> first = ProjectLock::Acquire(lockFile);
				REQUIRE(first.has_value());
				CHECK(first->GetPath() == lockFile);
				CHECK(ProjectLock::ReadHolderPid(lockFile) == Process::GetCurrentId());
				CHECK(ProjectLock::IsHeld(lockFile) == true);

				const Result<ProjectLock> second = ProjectLock::Acquire(lockFile);
				REQUIRE_FALSE(second.has_value());
				CHECK(second.error().GetCode() == ErrorCode::AlreadyExists);
				CHECK(second.error().GetMessageText().contains(std::format("process {}", Process::GetCurrentId())));
			}
			CHECK(ProjectLock::IsHeld(lockFile) == false);

			// Held by another process, which is then killed: the OS releases the lock.
			Result<Process> holder = Process::Spawn(Test::MakeTestsChildSpecification({
				"--death-test=Platform/HoldsProjectLock",
				"--child-argument=" + Test::PathToUtf8(lockFile),
			}));
			REQUIRE(holder.has_value());
			const Status announced = holder->WaitForOutput(ProcessStream::StandardError, "Project lock held by process",
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(announced.has_value(), announced.error().ToString());

			const Result<ProjectLock> blocked = ProjectLock::Acquire(lockFile);
			REQUIRE_FALSE(blocked.has_value());
			CHECK(blocked.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(blocked.error().GetMessageText().contains(std::format("process {}", holder->GetId())));
			CHECK(ProjectLock::ReadHolderPid(lockFile) == holder->GetId());
			CHECK(ProjectLock::IsHeld(lockFile) == true);

			REQUIRE(holder->Kill().has_value());

			Result<ProjectLock> after = ProjectLock::Acquire(lockFile);
			REQUIRE(after.has_value());
			CHECK(ProjectLock::ReadHolderPid(lockFile) == Process::GetCurrentId());
		}

		TEST_CASE("ProjectLock: a stale pid without a lock does not block")
		{
			Test::TempDirectory directory("ProjectLockStale");
			const std::filesystem::path lockFile = directory / "Editor.lock";
			const std::string stale = "99999999\n";
			REQUIRE(FileSystem::WriteFileAtomic(lockFile, std::as_bytes(std::span(stale))).has_value());

			CHECK(ProjectLock::IsHeld(lockFile) == false);
			CHECK(ProjectLock::ReadHolderPid(lockFile) == 99999999u);
			Result<ProjectLock> lock = ProjectLock::Acquire(lockFile);
			REQUIRE(lock.has_value());
			CHECK(ProjectLock::ReadHolderPid(lockFile) == Process::GetCurrentId());

			const Result<std::string> text = FileSystem::ReadText(lockFile);
			REQUIRE(text.has_value());
			CHECK(*text == std::format("{}\n", Process::GetCurrentId()));
		}

		TEST_CASE("ProjectLock: missing and malformed lock files are reported")
		{
			Test::TempDirectory directory("ProjectLockErrors");

			const Result<uint32_t> missing = ProjectLock::ReadHolderPid(directory / "Missing.lock");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(ProjectLock::IsHeld(directory / "Missing.lock") == false);

			const std::filesystem::path garbage = directory / "Garbage.lock";
			const std::string text = "not a pid";
			REQUIRE(FileSystem::WriteFileAtomic(garbage, std::as_bytes(std::span(text))).has_value());
			const Result<uint32_t> malformed = ProjectLock::ReadHolderPid(garbage);
			REQUIRE_FALSE(malformed.has_value());
			CHECK(malformed.error().GetCode() == ErrorCode::Parse);

			const Result<ProjectLock> noDirectory = ProjectLock::Acquire(directory / "NoSuchFolder" / "Editor.lock");
			REQUIRE_FALSE(noDirectory.has_value());
			CHECK(noDirectory.error().GetCode() == ErrorCode::NotFound);

			// A directory in the lock file's place cannot be locked.
			const Result<ProjectLock> onDirectory = ProjectLock::Acquire(directory.GetPath());
			REQUIRE_FALSE(onDirectory.has_value());
			CHECK(onDirectory.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("ProjectLock: only a decimal process ID and one line feed is a valid lock file")
		{
			Test::TempDirectory directory("ProjectLockFormat");
			const std::filesystem::path lockFile = directory / "Editor.lock";
			const auto read = [&lockFile](std::string_view text)
			{
				REQUIRE(FileSystem::WriteFileAtomic(lockFile, AsBytes(text), { .KeepBackup = false }).has_value());
				return ProjectLock::ReadHolderPid(lockFile);
			};

			CHECK(read("1\n") == 1u);
			CHECK(read("4294967295\n") == 4294967295u);
			CHECK(read("0042\n") == 42u);

			const std::array<std::string_view, 10> invalid = {
				"",
				"\n",
				"123",
				"123\n\n",
				"123\r\n",
				"+12\n",
				"12 \n",
				"0\n",
				"4294967296\n",
				"99999999999\n",
			};
			for (const std::string_view text : invalid)
			{
				CAPTURE(std::string(text));
				const Result<uint32_t> pid = read(text);
				REQUIRE_FALSE(pid.has_value());
				CHECK(pid.error().GetCode() == ErrorCode::Parse);
			}
		}

		TEST_CASE("ProjectLock: the pid text stays readable while the lock is held, and a moved lock stays held")
		{
			Test::TempDirectory directory("ProjectLockMove");
			const std::filesystem::path lockFile = directory / "Editor.lock";

			Result<ProjectLock> acquired = ProjectLock::Acquire(lockFile);
			REQUIRE(acquired.has_value());
			const Result<std::string> text = FileSystem::ReadText(lockFile);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(*text == std::format("{}\n", Process::GetCurrentId()));

			{
				ProjectLock moved(std::move(*acquired));
				CHECK(moved.GetPath() == lockFile);
				CHECK(ProjectLock::IsHeld(lockFile) == true);

				Test::TempDirectory otherDirectory("ProjectLockOther");
				Result<ProjectLock> other = ProjectLock::Acquire(otherDirectory / "Editor.lock");
				REQUIRE(other.has_value());
				// Assigning over a lock releases the lock it held.
				*other = std::move(moved);
				CHECK(other->GetPath() == lockFile);
				CHECK(ProjectLock::IsHeld(otherDirectory / "Editor.lock") == false);
				CHECK(ProjectLock::IsHeld(lockFile) == true);
			}
			CHECK(ProjectLock::IsHeld(lockFile) == false);
		}

		TEST_CASE("ProjectLock: a reader that asks for more than the pid text in one read gets it while the lock is held")
		{
			// Windows checks the byte range a read requests against locks, not the bytes the file holds, so a reader whose
			// first read asks for 64 KiB from offset 0 (as many tools and runtimes do) must not reach the locked byte.
			Test::TempDirectory directory("ProjectLockLargeRead");
			const std::filesystem::path lockFile = directory / "Editor.lock";
			const Result<ProjectLock> lock = ProjectLock::Acquire(lockFile);
			REQUIRE(lock.has_value());

			std::ifstream stream(lockFile, std::ios::binary);
			REQUIRE(stream.is_open());
			// A 64 KiB stream buffer makes the first read request 64 KiB. The MSVC runtime takes the buffer only once the
			// file is open; libstdc++ and libc++ keep their own, which is harmless here because flock never blocks reads.
			std::vector<char> streamBuffer(64 * 1024);
			stream.rdbuf()->pubsetbuf(streamBuffer.data(), static_cast<std::streamsize>(streamBuffer.size()));
			const std::string text{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
			CHECK(text == std::format("{}\n", Process::GetCurrentId()));
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("ProjectLock: a reader that denies writers opens the pid text while the lock is held")
		{
			// .NET's File.ReadAllText opens with FileShare.Read, which fails while any handle to the file has write access,
			// so the holder keeps only read access. flock never stands in a reader's way, so this is Windows-only.
			Test::TempDirectory directory("ProjectLockDenyWrite");
			const std::filesystem::path lockFile = directory / "Editor.lock";
			const Result<ProjectLock> lock = ProjectLock::Acquire(lockFile);
			REQUIRE(lock.has_value());

			const Result<std::string> text = Test::ReadFileDenyingWriters(lockFile);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(*text == std::format("{}\n", Process::GetCurrentId()));
			const Result<bool> held = ProjectLock::IsHeld(lockFile);
			REQUIRE(held.has_value());
			CHECK(*held);
		}
#endif

		TEST_CASE("ProjectLock: destroying the Process of a running holder releases its lock")
		{
			Test::TempDirectory directory("ProjectLockHolderGone");
			const std::filesystem::path lockFile = directory / "Editor.lock";
			{
				Result<Process> holder = Process::Spawn(Test::MakeTestsChildSpecification({
					"--death-test=Platform/HoldsProjectLock",
					"--child-argument=" + Test::PathToUtf8(lockFile),
				}));
				REQUIRE(holder.has_value());
				const Status announced = holder->WaitForOutput(ProcessStream::StandardError, "Project lock held by process",
					std::chrono::seconds(60));
				REQUIRE_MESSAGE(announced.has_value(), announced.error().ToString());
				CHECK(ProjectLock::IsHeld(lockFile) == true);
			}
			// The Process killed its child and waited for it, so the OS released the lock.
			CHECK(ProjectLock::IsHeld(lockFile) == false);
			CHECK(ProjectLock::Acquire(lockFile).has_value());
		}
	}

}
