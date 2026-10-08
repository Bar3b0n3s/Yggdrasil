#include "EnginePCH.h"
#include "Engine/App/EngineContext.h"

#include "Engine/Asset/AssetTypeRegistration.h"
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

		if (!specification.EngineResourcesDirectory.empty() || !specification.EngineCacheDirectory.empty())
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

		return context;
	}

	Status EngineContext::MountEngineResources(const EngineContextSpecification& specification)
	{
		if (!specification.EnginePak.empty())
			ENGINE_TRY(MountEnginePak(specification.EnginePak));

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

	Status EngineContext::MountEnginePak(const std::filesystem::path& /*pak*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "mounting Engine.pak is not implemented yet (M7 stream C)");
	}

	Status EngineContext::CreateGraphics(const GraphicsSpecification& graphics)
	{
		// Development builds read the shaders the Shaders project compiled for this configuration (§2.2, §8.12); exported
		// games mount their Engine.pak instead (M7).
#if !defined(ENGINE_DIST)
		Result<Scope<NativeDirectoryMount>> shaders = NativeDirectoryMount::Create(std::filesystem::path(ENGINE_SHADER_DIRECTORY),
			MountAccess::ReadOnly);
		if (!shaders.has_value())
		{
			return std::unexpected(std::move(shaders).error().WithHint(
				"build this configuration's Shaders project (python Scripts/Build.py), which compiles the shaders there"));
		}
		ENGINE_TRY(m_Vfs.Mount(ShaderScheme, std::move(*shaders)));
#endif

		// A windowed process presents to the context's window; a headless one renders offscreen only (§8.1, §8.13).
		Window* presentWindow = GlfwLibrary::GetMode() == WindowMode::Windowed ? GetWindow() : nullptr;
		ENGINE_TRY_ASSIGN(m_GraphicsDevice, GraphicsDevice::Create({
												.Graphics = graphics,
												.PresentWindow = presentWindow,
												.ApplicationName = presentWindow != nullptr ? presentWindow->GetTitle() : std::string(),
											}));
		ENGINE_TRY_ASSIGN(const VfsPath shaderRoot, VfsPath::Create(ShaderScheme, ""));
		m_ShaderLibrary = CreateScope<ShaderLibrary>(m_GraphicsDevice.get(), m_Vfs, shaderRoot);
		m_PipelineFactory = CreateScope<PipelineFactory>(*m_GraphicsDevice, *m_ShaderLibrary);
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
		}

		ENGINE_CORE_ASSERT(false, "Unknown EngineContextStep {}", std::to_underlying(step));
		return "Unknown";
	}

}
