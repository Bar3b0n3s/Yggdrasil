#include "TestsPCH.h"

#include "Engine/App/EngineContext.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

namespace Engine {

	static VfsPath MakeVfsPath(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return *path;
	}

	TEST_SUITE("App")
	{
		TEST_CASE("EngineContext: builds its services without a window or GLFW" * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0 });
			REQUIRE(created.has_value());
			EngineContext& context = **created;

			CHECK(context.GetWindow() == nullptr);
			CHECK(context.GetVfs().GetSchemes().empty());
			CHECK(context.GetJobSystem().IsInline());
			CHECK(context.GetMainThreadQueue().IsMainThread());
			CHECK(context.GetEventLog().GetSize() == 0);

			// The JobSystem posts continuations to the context's own MainThreadQueue.
			int completions = 0;
			JobHandle<int> job = context.GetJobSystem().Submit([]() -> Result<int>
			{
				return 42;
			});
			context.GetJobSystem().ContinueOnMainThread(job, [&completions](Result<int> result)
			{
				if (result.has_value() && *result == 42)
					++completions;
			});
			CHECK(context.GetMainThreadQueue().Drain() == 1);
			CHECK(completions == 1);
		}

		TEST_CASE("EngineContext: mounts user:// on the given directory without backups" * doctest::skip(true))
		{
			Test::TempDirectory directory("EngineContextUserData");
			Result<Scope<EngineContext>> created = EngineContext::Create({ .UserDataDirectory = directory.GetPath() });
			REQUIRE(created.has_value());
			VirtualFileSystem& vfs = (*created)->GetVfs();
			REQUIRE(vfs.IsMounted("user"));

			const std::string first = "first";
			const std::string second = "second";
			REQUIRE(vfs.WriteFileAtomic(MakeVfsPath("user://Editor.json"), std::as_bytes(std::span(first))).has_value());
			REQUIRE(vfs.WriteFileAtomic(MakeVfsPath("user://Editor.json"), std::as_bytes(std::span(second))).has_value());

			const Result<std::string> onDisk = FileSystem::ReadText(directory / "Editor.json");
			REQUIRE(onDisk.has_value());
			CHECK(*onDisk == "second");
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(directory / "Editor.json.bak", error));
		}

		TEST_CASE("EngineContext: a missing user-data directory fails with the step as context" * doctest::skip(true))
		{
			Test::TempDirectory directory("EngineContextMissing");
			const Result<Scope<EngineContext>> created = EngineContext::Create({ .UserDataDirectory = directory / "Missing" });
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::NotFound);
			CHECK(created.error().ToString().contains("UserData"));
		}

		TEST_CASE("EngineContext: creates a null-platform window in the headless Tests process" * doctest::skip(true))
		{
			const WindowSpecification specification = { .Title = "Context", .Width = 128, .Height = 96 };
			Result<Scope<EngineContext>> created = EngineContext::Create({ .Window = specification });
			REQUIRE(created.has_value());
			Window* window = (*created)->GetWindow();
			REQUIRE(window != nullptr);
			CHECK(window->GetWidth() == 128);
			CHECK(window->GetTitle() == "Context");
		}

		TEST_CASE("EngineContext: several contexts live side by side" * doctest::skip(true))
		{
			const WindowSpecification firstWindow = { .Title = "First", .Width = 64, .Height = 64 };
			const WindowSpecification secondWindow = { .Title = "Second", .Width = 32, .Height = 32 };
			Result<Scope<EngineContext>> first = EngineContext::Create({ .Window = firstWindow });
			Result<Scope<EngineContext>> second = EngineContext::Create({ .Window = secondWindow });
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(&(*first)->GetInputState() != &(*second)->GetInputState());
			CHECK((*first)->GetWindow()->GetWidth() == 64);
			CHECK((*second)->GetWindow()->GetWidth() == 32);

			// Events reach only the context whose window they were injected into.
			(*first)->GetInputState().Inject(KeyEvent{ .KeyCode = Key::F, .Action = ButtonAction::Pressed });
			(*first)->GetInputState().LatchFrame();
			(*second)->GetInputState().LatchFrame();
			CHECK((*first)->GetInputState().IsKeyDown(InputPhase::Frame, Key::F));
			CHECK_FALSE((*second)->GetInputState().IsKeyDown(InputPhase::Frame, Key::F));
		}

		TEST_CASE("EngineContext: EngineContextStepToString names every step" * doctest::skip(true))
		{
			CHECK(EngineContextStepToString(EngineContextStep::Services) == "Services");
			CHECK(EngineContextStepToString(EngineContextStep::UserData) == "UserData");
			CHECK(EngineContextStepToString(EngineContextStep::Window) == "Window");
		}
	}

}
