#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"

// The common main of the Editor and the Runtime (Architecture §4.1):
//
//     int main(int argc, char** argv)
//     {
//         return Engine::RunApplication(argc, argv, &Engine::CreateRuntimeApp);
//     }

namespace Engine {

	// Runs an application as a whole process and returns its exit code (§4.1 table):
	//   1. argv[1..] become UTF-8 strings (Windows executables run with the UTF-8 active code page of App.manifest, §2.2).
	//   2. `factory` builds the application. An error is printed to stderr through the fallback logger and returns
	//      ExitCode::UsageError (2) for InvalidArgument (a bad command line) or ExitCode::InitFailed (3) for any other code
	//      (a required file missing or invalid, see ApplicationFactory).
	//   3. ProcessContext::Create with the application's Name, WindowMode and UserDataRoot, VulkanLoaderPolicy::Required
	//      for RendererMode::Vulkan (None otherwise) and ShowErrorDialogs for a windowed application; a failure is printed
	//      (in a windowed process also shown in an error dialog) and returns ExitCode::InitFailed (3). A missing Vulkan
	//      loader ends here, with NoVulkanLoaderMessage (§8.1).
	//   4. Application::Run.
	//   5. The application is destroyed, then the ProcessContext; the Run result is returned.
	// Steps 2 to 5 run inside the last-resort boundary (the allowlisted try/catch of §4.6 item 6), so an exception from
	// the factory, from ProcessContext::Create or from teardown never escapes main (where std::terminate would end the
	// process with the C runtime's abort code, 3 on Windows, and misreport a crash as InitFailed): std::bad_alloc becomes
	// FatalError(OutOfMemory), any other exception FatalError(UnhandledException); both exit with code 4. Before the
	// ProcessContext exists, FatalError prints through the fallback logger and no crash report is written.
	// `factory` must be non-null (asserted). Main thread; once per process.
	[[nodiscard]] int RunApplication(int argc, char** argv, ApplicationFactory factory);

}
