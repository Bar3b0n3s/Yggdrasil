#include "EnginePCH.h"
#include "Engine/App/EngineContext.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"

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

		if (specification.Window.has_value())
		{
			ENGINE_TRY_ASSIGN(Window window,
				WithContext(Window::Create(*specification.Window), Utils::DescribeEngineContextStep(EngineContextStep::Window)));
			ENGINE_CORE_INFO("Engine context: window '{}' created ({}x{})", window.GetTitle(), window.GetWidth(), window.GetHeight());
			context->m_Window.emplace(std::move(window));
		}

		return context;
	}

	std::string_view EngineContextStepToString(EngineContextStep step)
	{
		switch (step)
		{
			case EngineContextStep::Services: return "Services";
			case EngineContextStep::UserData: return "UserData";
			case EngineContextStep::Window:   return "Window";
		}

		ENGINE_CORE_ASSERT(false, "Unknown EngineContextStep {}", std::to_underlying(step));
		return "Unknown";
	}

}
