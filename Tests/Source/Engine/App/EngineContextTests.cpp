#include "TestsPCH.h"

#include "Engine/App/EngineContext.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/PakFormat.h"
#include "Engine/Asset/RuntimeAssetManager.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"

namespace Engine {

	static VfsPath MakeVfsPath(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return *path;
	}

	namespace {

		// A struct an application registers through RegisterTypes.
		struct ApplicationOwnedSettings
		{
			uint32_t Level = 3;
		};

	}

	static void RegisterApplicationOwnedTypes(TypeRegistry& registry)
	{
		registry.Struct<ApplicationOwnedSettings>("ApplicationOwnedSettings", "A struct registered by the application.")
			.Field("Level", &ApplicationOwnedSettings::Level, "A level.");
	}

	TEST_SUITE("App")
	{
		TEST_CASE("EngineContext: the type registry holds the built-in components and the project settings types, frozen")
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0 });
			REQUIRE(created.has_value());
			const TypeRegistry& registry = (*created)->GetTypeRegistry();

			CHECK(registry.IsFrozen());
			CHECK(registry.AreComponentsRegistered(BuiltinComponents{}));
			CHECK(registry.FindStruct<ProjectSettings>() != nullptr);
			CHECK(registry.FindStruct("ApplicationOwnedSettings") == nullptr);
		}

		TEST_CASE("EngineContext: RegisterTypes adds the application's types before the registry is frozen")
		{
			Result<Scope<EngineContext>> created =
				EngineContext::Create({ .WorkerCount = 0, .RegisterTypes = &RegisterApplicationOwnedTypes });
			REQUIRE(created.has_value());
			const TypeRegistry& registry = (*created)->GetTypeRegistry();

			CHECK(registry.IsFrozen());
			const StructInfo* settings = registry.FindStruct<ApplicationOwnedSettings>();
			REQUIRE(settings != nullptr);
			CHECK(settings->GetName() == "ApplicationOwnedSettings");
			CHECK(registry.FindComponent<TransformComponent>() != nullptr);
		}

		TEST_CASE("EngineContext: builds its services without a window or GLFW")
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

		TEST_CASE("EngineContext: mounts user:// on the given directory without backups")
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

		TEST_CASE("EngineContext: engine resources mount engine:// read-only and enginecache:// read-write")
		{
			Test::TempDirectory directory("EngineContextResources");
			REQUIRE(FileSystem::CreateDirectories(directory / "Resources").has_value());
			const std::string catalogue = "{}";
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Resources/EngineAssets.json", std::as_bytes(std::span(catalogue))).has_value());
			// The cache directory is created when missing.
			Result<Scope<EngineContext>> created = EngineContext::Create({
				.EngineResourcesDirectory = directory / "Resources",
				.EngineCacheDirectory = directory / "bin/EngineCache",
			});
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			VirtualFileSystem& vfs = (*created)->GetVfs();
			CHECK(vfs.Exists(MakeVfsPath("engine://EngineAssets.json")));
			const Status readOnly = vfs.WriteFileAtomic(MakeVfsPath("engine://Other.json"), std::as_bytes(std::span(catalogue)));
			REQUIRE_FALSE(readOnly.has_value());
			CHECK(readOnly.error().GetCode() == ErrorCode::PermissionDenied);
			REQUIRE(vfs.CreateDirectories(MakeVfsPath("enginecache://00000000000001c1")).has_value());
			REQUIRE(vfs.WriteFileAtomic(MakeVfsPath("enginecache://00000000000001c1/key.bin"), std::as_bytes(std::span(catalogue))).has_value());
			std::error_code error;
			CHECK(std::filesystem::exists(directory / "bin/EngineCache/00000000000001c1/key.bin", error));
			// No .bak files in the engine cache.
			CHECK_FALSE(std::filesystem::exists(directory / "bin/EngineCache/00000000000001c1/key.bin.bak", error));
		}

		TEST_CASE("EngineContext: a missing engine resources directory fails with the step as context")
		{
			Test::TempDirectory directory("EngineContextNoResources");
			Result<Scope<EngineContext>> created = EngineContext::Create({ .EngineResourcesDirectory = directory / "Missing" });
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().ToString().find("EngineResources") != std::string::npos);
		}

		TEST_CASE("EngineContext: the injected asset manager is served until it is removed")
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({});
			REQUIRE(created.has_value());
			EngineContext& context = **created;
			CHECK(context.GetAssetManager() == nullptr);
			AssetLoaderRegistry loaders;
			RuntimeAssetManager manager({
				.Jobs = &context.GetJobSystem(),
				.MainThread = &context.GetMainThreadQueue(),
				.Registry = &context.GetTypeRegistry(),
				.Loaders = &loaders,
			});
			context.SetAssetManager(&manager);
			CHECK(context.GetAssetManager() == &manager);
			context.SetAssetManager(nullptr);
			CHECK(context.GetAssetManager() == nullptr);
		}

		TEST_CASE("EngineContext: the type registry holds the Asset module's types")
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({});
			REQUIRE(created.has_value());
			CHECK((*created)->GetTypeRegistry().FindStruct("Material") != nullptr);
			CHECK((*created)->GetTypeRegistry().FindEnum("AlphaMode") != nullptr);
		}

		TEST_CASE("EngineContext: a missing user-data directory fails with the step as context")
		{
			Test::TempDirectory directory("EngineContextMissing");
			const Result<Scope<EngineContext>> created = EngineContext::Create({ .UserDataDirectory = directory / "Missing" });
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::NotFound);
			CHECK(created.error().ToString().contains("UserData"));
		}

		TEST_CASE("EngineContext: creates a null-platform window in the headless Tests process")
		{
			const WindowSpecification specification = { .Title = "Context", .Width = 128, .Height = 96 };
			Result<Scope<EngineContext>> created = EngineContext::Create({ .Window = specification });
			REQUIRE(created.has_value());
			Window* window = (*created)->GetWindow();
			REQUIRE(window != nullptr);
			CHECK(window->GetWidth() == 128);
			CHECK(window->GetTitle() == "Context");
		}

		TEST_CASE("EngineContext: several contexts live side by side")
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

		TEST_CASE("EngineContext: a failed Graphics step fails Create with the step as context")
		{
			// FramesInFlight 0 is rejected before any Vulkan call (GraphicsDevice::Create), so the step fails the same way on
			// every machine, with or without a GPU or a Vulkan loader.
			const Result<Scope<EngineContext>> created =
				EngineContext::Create({ .WorkerCount = 0, .Graphics = GraphicsSpecification{ .FramesInFlight = 0 } });
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().ToString().contains("while creating the engine context (Graphics)"));
		}

		TEST_CASE("EngineContext: DestroyGraphics without a device returns zero counts")
		{
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0 });
			REQUIRE(created.has_value());
			const GpuMessageCounts counts = (*created)->DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
		}

		TEST_CASE("EngineContext: the Graphics step mounts shaders:// and creates the device and its services"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			// Headless: the device presents to nothing, like the fixture's.
			GraphicsSpecification graphics;
			graphics.Validation = true;
			graphics.SynchronizationValidation = true;
			graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			Result<Scope<EngineContext>> created = EngineContext::Create({ .WorkerCount = 0, .Graphics = graphics });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			EngineContext& context = **created;
			CHECK(context.GetVfs().IsMounted(ShaderScheme));
			REQUIRE(context.GetGraphicsDevice() != nullptr);
			CHECK(context.GetShaderLibrary() != nullptr);
			CHECK(context.GetPipelineFactory() != nullptr);
			const GraphicsDevice& device = *context.GetGraphicsDevice();
			CHECK(device.GetInfo().Validation);
			CHECK(device.GetInfo().ApiVersion <= Test::GetTestOptions().VulkanApi);

			// DestroyGraphics tears the GPU services down now and returns the device's final counts.
			const GpuMessageCounts counts = context.DestroyGraphics();
			CHECK(counts.Errors == 0);
			CHECK(counts.Warnings == 0);
			CHECK(context.GetGraphicsDevice() == nullptr);
			CHECK(context.GetShaderLibrary() == nullptr);
			CHECK(context.GetPipelineFactory() == nullptr);
		}

		TEST_CASE("EngineContext: EngineContextStepToString names every step")
		{
			CHECK(EngineContextStepToString(EngineContextStep::Services) == "Services");
			CHECK(EngineContextStepToString(EngineContextStep::UserData) == "UserData");
			CHECK(EngineContextStepToString(EngineContextStep::EngineResources) == "EngineResources");
			CHECK(EngineContextStepToString(EngineContextStep::Window) == "Window");
			CHECK(EngineContextStepToString(EngineContextStep::Graphics) == "Graphics");
		}

		TEST_CASE("EngineContext: an Engine.pak is mounted as engine:// and kept for the asset manager" * doctest::skip(true))
		{
			// Skipped skeleton of the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 10); stream C. The pak holds
			// one plain file; a context without a device needs no shaders.
			const Test::TempDirectory directory("EnginePak");
			PakWriter writer;
			const std::string text = "engine resource";
			const std::span<const std::byte> bytes = std::as_bytes(std::span(text.data(), text.size()));
			REQUIRE(writer.Add(PakWriterEntry{ .Type = std::string(PakFileEntryType), .Path = "Shaders/Readme.txt", .Data = Buffer(bytes.begin(), bytes.end()) }).has_value());
			REQUIRE(writer.WriteToFile(directory.GetPath() / "Engine.pak").has_value());

			EngineContextSpecification specification;
			specification.EnginePak = directory.GetPath() / "Engine.pak";
			Result<Scope<EngineContext>> context = EngineContext::Create(specification);
			REQUIRE_MESSAGE(context.has_value(), context.error().ToString());
			REQUIRE((*context)->GetEnginePak() != nullptr);
			const Result<std::string> read = (*context)->GetVfs().ReadText(VfsPath::Parse("engine://Shaders/Readme.txt").value());
			REQUIRE(read.has_value());
			CHECK(*read == text);

			EngineContextSpecification both = specification;
			both.EngineResourcesDirectory = directory.GetPath();
			const Result<Scope<EngineContext>> conflicting = EngineContext::Create(both);
			REQUIRE_FALSE(conflicting.has_value());
			CHECK(conflicting.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
