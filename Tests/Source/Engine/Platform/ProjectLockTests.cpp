#include "TestsPCH.h"

#include "Engine/Platform/ProjectLock.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/DeathTest.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <condition_variable>
#include <mutex>

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
		std::mutex mutex;
		std::condition_variable never;
		std::unique_lock wait(mutex);
		never.wait(wait, []()
		{
			return false;
		});
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("ProjectLock: a second lock fails and names the pid; killing the holder releases it" * doctest::skip(true))
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

		TEST_CASE("ProjectLock: a stale pid without a lock does not block" * doctest::skip(true))
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

		TEST_CASE("ProjectLock: missing and malformed lock files are reported" * doctest::skip(true))
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
		}
	}

}
