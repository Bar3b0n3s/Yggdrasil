#include "EnginePCH.h"
#include "Engine/App/EngineContext.h"

#include "Engine/Asset/AssetTypeRegistration.h"
#include "Engine/Asset/PakMount.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scripting/RegisterBindings.h"

#include <format>
#include <string>
#include <utility>

namespace Engine {

	namespace Utils {

		// "while creating the engine context (<Step>)", the context of a failed step's error.
		static std::string DescribeEngineContextStep(EngineContextStep step)
		{
			return std::format("while creating the engine context ({})", EngineContextStepToString(step));
		}

	}

	EngineContext::EngineContext(ConstructionKey /*key*/, const EngineContextSpecification& specification)
		: m_JobSystem(specification.WorkerCount, m_MainThreadQueue)
	{
		// The registries (§4.1): everything the context's scenes, project files and automation read and write.
		RegisterBuiltinComponents(m_TypeRegistry);
		RegisterProjectSettingsTypes(m_TypeRegistry);
		RegisterAssetTypes(m_TypeRegistry);
		if (specification.RegisterTypes != nullptr)
			specification.RegisterTypes(m_TypeRegistry);
		m_TypeRegistry.Freeze();
	}

	EngineContext::~EngineContext() = default;

	Result<Scope<EngineContext>> EngineContext::Create(const EngineContextSpecification& specification)
	{
		// Services: the constructor builds the infallible services in member order.
		Scope<EngineContext> context = CreateScope<EngineContext>(ConstructionKey(), specification);
		ENGINE_TRY(WithContext(RegisterBindings(context->m_ScriptApiRegistry, context->m_TypeRegistry),
			"while registering the engine context's script API"));

		if (!specification.UserDataDirectory.empty())
		{
			// user:// is the application's own folder: no .bak files there (ADR 0003 decision 16).
			const std::string step = Utils::DescribeEngineContextStep(EngineContextStep::UserData);
			ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> mount,
				WithContext(NativeDirectoryMount::Create(specification.UserDataDirectory, MountAccess::ReadWrite,
								AtomicWriteOptions{ .KeepBackup = false }),
					step));
			ENGINE_TRY(WithContext(context->m_Vfs.Mount("user", std::move(mount)), step));
		}

		if (!specification.EnginePak.empty() || !specification.EngineResourcesDirectory.empty() || !specification.EngineCacheDirectory.empty())
		{
			ENGINE_TRY(WithContext(context->MountEngineResources(specification),
				Utils::DescribeEngineContextStep(EngineContextStep::EngineResources)));
		}

		if (specification.Window.has_value())
		{
			ENGINE_TRY_ASSIGN(Window window,
				WithContext(Window::Create(*specification.Window), Utils::DescribeEngineContextStep(EngineContextStep::Window)));
			ENGINE_CORE_INFO("Engine context: window '{}' created ({}x{})", window.GetTitle(), window.GetWidth(), window.GetHeight());
			context->m_Window.emplace(std::move(window));
		}

		if (specification.Graphics.has_value())
			ENGINE_TRY(WithContext(context->CreateGraphics(*specification.Graphics), Utils::DescribeEngineContextStep(EngineContextStep::Graphics)));

		if (specification.Audio.has_value())
			ENGINE_TRY(WithContext(context->CreateAudio(*specification.Audio), Utils::DescribeEngineContextStep(EngineContextStep::Audio)));

		return context;
	}

	Status EngineContext::MountEngineResources(const EngineContextSpecification& specification)
	{
		if (!specification.EnginePak.empty())
		{
			// engine:// is either the exported game's pak or the development resources, never both.
			if (!specification.EngineResourcesDirectory.empty())
			{
				return MakeError(ErrorCode::InvalidArgument, "an Engine.pak ('{}') and an engine resources directory ('{}') were both given",
					FileSystem::PathToUtf8(specification.EnginePak), FileSystem::PathToUtf8(specification.EngineResourcesDirectory));
			}
			ENGINE_TRY(MountEnginePak(specification.EnginePak));
		}

		if (!specification.EngineResourcesDirectory.empty())
		{
			// The shipped resources are never written by the engine (§2.2, §4.10).
			ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> resources,
				NativeDirectoryMount::Create(specification.EngineResourcesDirectory, MountAccess::ReadOnly));
			ENGINE_TRY(m_Vfs.Mount("engine", std::move(resources)));
		}

		if (!specification.EngineCacheDirectory.empty())
		{
			// A cache is rebuilt from its sources, so it keeps no .bak files (§4.10, §7.5), like cache://.
			ENGINE_TRY(FileSystem::CreateDirectories(specification.EngineCacheDirectory));
			ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> cache,
				NativeDirectoryMount::Create(specification.EngineCacheDirectory, MountAccess::ReadWrite, AtomicWriteOptions{ .KeepBackup = false }));
			ENGINE_TRY(m_Vfs.Mount("enginecache", std::move(cache)));
		}
		return {};
	}

	Status EngineContext::MountEnginePak(const std::filesystem::path& pak)
	{
		// The pak is opened (its TOC hash verified) once: the mount and, through GetEnginePak, the Runtime's asset manager
		// share the reader.
		ENGINE_TRY_ASSIGN(Ref<const PakReader> reader, PakReader::Open(pak));
		ENGINE_TRY_ASSIGN(Scope<PakMount> mount, PakMount::Create(reader));
		ENGINE_TRY(m_Vfs.Mount("engine", std::move(mount)));
		m_EnginePak = std::move(reader);
		ENGINE_CORE_INFO("Engine context: '{}' mounted as engine:// ({} entries)", FileSystem::PathToUtf8(pak), m_EnginePak->GetEntries().size());
		return {};
	}

	Status EngineContext::CreateGraphics(const GraphicsSpecification& graphics)
	{
		// Development builds read the shaders the Shaders project compiled for this configuration (§2.2, §8.12); exported
		// games read the target configuration's SPIR-V from their Engine.pak in every configuration (§14.1).
#if !defined(ENGINE_DIST)
		if (m_EnginePak == nullptr)
		{
			Result<Scope<NativeDirectoryMount>> shaders = NativeDirectoryMount::Create(std::filesystem::path(ENGINE_SHADER_DIRECTORY),
				MountAccess::ReadOnly);
			if (!shaders.has_value())
			{
				return std::unexpected(std::move(shaders).error().WithHint(
					"build this configuration's Shaders project (python Scripts/Build.py), which compiles the shaders there"));
			}
			ENGINE_TRY(m_Vfs.Mount(ShaderScheme, std::move(*shaders)));
		}
#endif

		// A windowed process presents to the context's window; a headless one renders offscreen only (§8.1, §8.13).
		Window* presentWindow = GlfwLibrary::GetMode() == WindowMode::Windowed ? GetWindow() : nullptr;
		ENGINE_TRY_ASSIGN(m_GraphicsDevice, GraphicsDevice::Create({
												.Graphics = graphics,
												.PresentWindow = presentWindow,
												.ApplicationName = presentWindow != nullptr ? presentWindow->GetTitle() : std::string(),
											}));
		ENGINE_TRY_ASSIGN(const VfsPath shaderRoot, m_EnginePak != nullptr ? VfsPath::Create("engine", "Shaders") : VfsPath::Create(ShaderScheme, ""));
		m_ShaderLibrary = CreateScope<ShaderLibrary>(m_GraphicsDevice.get(), m_Vfs, shaderRoot);
		m_PipelineFactory = CreateScope<PipelineFactory>(*m_GraphicsDevice, *m_ShaderLibrary);
		return {};
	}

	Status EngineContext::CreateAudio(const AudioEngineSpecification& audio)
	{
		// The engine reads clips through the context's VFS, which outlives it (m_AudioEngine is the last member).
		ENGINE_TRY_ASSIGN(m_AudioEngine, AudioEngine::Create(audio, m_Vfs));
		return {};
	}

	GpuMessageCounts EngineContext::DestroyGraphics()
	{
		// The factory and the library hold GPU objects (pipelines, shaders), which must be gone before the device.
		m_PipelineFactory.reset();
		m_ShaderLibrary.reset();
		return GraphicsDevice::Destroy(std::move(m_GraphicsDevice));
	}

	std::string_view EngineContextStepToString(EngineContextStep step)
	{
		switch (step)
		{
			case EngineContextStep::Services:        return "Services";
			case EngineContextStep::UserData:        return "UserData";
			case EngineContextStep::EngineResources: return "EngineResources";
			case EngineContextStep::Window:          return "Window";
			case EngineContextStep::Graphics:        return "Graphics";
			case EngineContextStep::Audio:           return "Audio";
		}

		ENGINE_CORE_ASSERT(false, "Unknown EngineContextStep {}", std::to_underlying(step));
		return "Unknown";
	}

}
