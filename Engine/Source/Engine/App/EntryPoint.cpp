#include "EnginePCH.h"
#include "Engine/App/EntryPoint.h"

#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"

#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	int RunApplication(int argc, char** argv, ApplicationFactory factory)
	{
		ENGINE_CORE_ASSERT(factory != nullptr, "RunApplication needs an application factory");

		// Declared outside the last-resort boundary so that they outlive it: when an exception escapes Run, FatalError runs
		// while the ProcessContext still exists, so its fatal-error handler writes the crash report.
		Scope<Application> application;
		Scope<ProcessContext> processContext;
		try
		{
			// 1. The arguments, UTF-8 (the Windows executables run with the UTF-8 active code page of App.manifest).
			std::vector<std::string> arguments;
			for (int index = 1; index < argc; ++index)
				arguments.emplace_back(argv[index] != nullptr ? argv[index] : "");

			// 2. The application. There is no log yet, so errors go through the fallback logger to stderr.
			Result<Scope<Application>> created = factory(arguments);
			if (!created.has_value())
			{
				if (created.error().GetCode() == ErrorCode::InvalidArgument)
				{
					ENGINE_CORE_ERROR("Invalid command line: {}", created.error());
					return ExitCode::UsageError;
				}
				ENGINE_CORE_ERROR("Cannot start the application: {}", created.error());
				return ExitCode::InitFailed;
			}
			application = std::move(*created);

			// 3. The process level (§4.1), set up as the application's specification asks.
			const ApplicationSpecification& specification = application->GetSpecification();
			Result<Scope<ProcessContext>> context = ProcessContext::Create({
				.AppName = specification.Name,
				.Window = specification.Window,
				.UserDataRoot = specification.UserDataRoot,
			});
			if (!context.has_value())
			{
				ENGINE_CORE_ERROR("Cannot initialize the process: {}", context.error());
				return ExitCode::InitFailed;
			}
			processContext = std::move(*context);

			// 4. and 5. Run, then tear down: the application first, the process level last.
			const int exitCode = application->Run();
			const std::string_view exitCodeName = ExitCodeToString(exitCode);
			ENGINE_CORE_INFO("Exiting with code {} ({})", exitCode, exitCodeName.empty() ? "undocumented" : exitCodeName);
			application.reset();
			processContext.reset();
			return exitCode;
		}
		catch (const std::bad_alloc&)
		{
			FatalError(FatalErrorKind::OutOfMemory, "std::bad_alloc: out of memory");
		}
		catch (const std::exception& exception)
		{
			// what() is passed as it is: formatting a message here could throw again.
			FatalError(FatalErrorKind::UnhandledException, exception.what());
		}
		catch (...)
		{
			FatalError(FatalErrorKind::UnhandledException, "an exception of unknown type escaped the application");
		}
	}

}
