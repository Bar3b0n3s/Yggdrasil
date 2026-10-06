#include "EditorPCH.h"
#include "EditorCore/EditorCommandLine.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol, editor wiring) implements the editor's options. The option table
// is real, so EditorApp can already parse command lines that name them.

namespace Engine {

	namespace {

		constexpr std::array EditorOptions = {
			CommandLineOption{ .Name = "--project", .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Open the project (.eproj or its directory)." },
			CommandLineOption{ .Name = "--read-only", .Value = CommandLineValue::None, .ValueName = {}, .Description = "Open the project read-only, without the lock." },
			CommandLineOption{ .Name = "--renderer", .Value = CommandLineValue::Required, .ValueName = "vulkan|none", .Description = "Render with Vulkan, or not at all." },
			CommandLineOption{ .Name = "--automation", .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Start the automation server (127.0.0.1)." },
			CommandLineOption{ .Name = "--automation-test-hooks", .Value = CommandLineValue::None, .ValueName = {}, .Description = "Register the debug.* test hooks." },
			CommandLineOption{ .Name = "--batch", .Value = CommandLineValue::Required, .ValueName = "file.jsonl", .Description = "Run the automation requests of a batch file and exit." },
			CommandLineOption{ .Name = "--upgrade", .Value = CommandLineValue::None, .ValueName = {}, .Description = "Run project.upgrade on the project and exit." },
			CommandLineOption{ .Name = "--dump-reference", .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Write the method catalogues and exit." },
		};

	}

	std::string_view EditorRendererToString(EditorRenderer /*renderer*/)
	{
		ENGINE_CONTRACT_STUB();
		return "vulkan";
	}

	std::span<const CommandLineOption> GetEditorCommandLineOptions()
	{
		return EditorOptions;
	}

	Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& /*commandLine*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParseEditorLaunchOptions is an M4 contract stub");
	}

}
