#include "EditorPCH.h"
#include "EditorCore/Reference/ReferenceGenerator.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

namespace Engine {

	namespace Utils {

		// Writes `document` canonically (JsonWriter, pretty) to `file`.
		[[nodiscard]] static Status WriteReferenceJson(const std::filesystem::path& file, const Json& document)
		{
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document, JsonStyle::Pretty));
			return FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false });
		}

		[[nodiscard]] static Status WriteReferenceText(const std::filesystem::path& file, const std::string& text)
		{
			return FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false });
		}

	}

	Result<std::vector<GeneratedReferenceFile>> GenerateReferences(const ReferenceSources& /*sources*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 reference generator contract"));
	}

	Status WriteReferences(std::span<const GeneratedReferenceFile> /*files*/, const std::filesystem::path& /*directory*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M14 reference writer contract"));
	}

	Status DumpReferences(const TypeRegistry& types, const ScriptApiRegistry& scripts, const std::filesystem::path& directory)
	{
		MethodRegistry methods(types);
		RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
		methods.Freeze();
		ENGINE_TRY(FileSystem::CreateDirectories(directory));
		ENGINE_TRY(Utils::WriteReferenceJson(directory / "Methods.json", methods.BuildMethodCatalog()));
		ENGINE_TRY(Utils::WriteReferenceJson(directory / "catalog.json", methods.BuildToolCatalog()));
		ENGINE_TRY_ASSIGN(const std::string definitions, scripts.GenerateDefinitions());
		ENGINE_TRY_ASSIGN(const std::string documentation, scripts.GenerateDocumentation());
		ENGINE_TRY(Utils::WriteReferenceText(directory / "Engine.d.luau", definitions));
		ENGINE_TRY(Utils::WriteReferenceText(directory / "ScriptAPI.md", documentation));
		ENGINE_INFO("Wrote the method catalogue ({} methods), MCP tools and script references", methods.GetMethods().size());
		return {};
	}

}
