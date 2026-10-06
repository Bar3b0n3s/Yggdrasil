#include "EnginePCH.h"
#include "Engine/Project/ProjectSerializer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// The oldest readable .eproj version (there has been no other yet).
		static constexpr uint32_t MinimumProjectVersion = 1;

		// The registered "ProjectSettings" struct. A registry without it is a programmer error (asserted); the error return
		// keeps builds without asserts safe.
		static Result<const StructInfo*> FindProjectSettingsType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "ProjectSerializer needs a frozen registry");
			const StructInfo* type = registry.FindStruct<ProjectSettings>();
			ENGINE_CORE_ASSERT(type != nullptr, "ProjectSerializer needs a registry on which RegisterProjectSettingsTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no ProjectSettings (RegisterProjectSettingsTypes was not called)");
			return type;
		}

		// `error` located in the file `path` too, unless it names a file already or `path` is empty.
		static Error WithSourceFile(Error error, const std::string& path)
		{
			if (path.empty() || !error.GetLocation().File.empty())
				return error;
			ErrorLocation location;
			location.File = path;
			return std::move(error).WithLocation(std::move(location));
		}

		static Result<ProjectSettings> ReadDocument(const Json& document, const TypeRegistry& registry, const ProjectLoadOptions& options,
			ProjectLoadReport& report)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindProjectSettingsType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion,
				root.ReadFormatHeader(ProjectSerializer::FormatName, MinimumProjectVersion, ProjectSerializer::CurrentVersion));

			// Everything below the header is the ProjectSettings object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			ProjectSettings settings;
			ReadContext context;
			context.Strict = options.StrictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&settings, JsonReader(fields), context));
			return settings;
		}

	}

	Result<Json> ProjectSerializer::ToJson(const ProjectSettings& settings, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindProjectSettingsType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&settings));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(FormatName);
		document["Version"] = CurrentVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> ProjectSerializer::SaveToString(const ProjectSettings& settings, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const Json document, ToJson(settings, registry));
		return JsonWriter::Write(document, JsonStyle::Pretty);
	}

	Result<ProjectSettings> ProjectSerializer::FromJson(const Json& document, const TypeRegistry& registry, const ProjectLoadOptions& options,
		ProjectLoadReport& report)
	{
		report = ProjectLoadReport{};
		Result<ProjectSettings> settings = Utils::ReadDocument(document, registry, options, report);
		if (!settings)
			return std::unexpected(Utils::WithSourceFile(std::move(settings).error(), options.SourcePath));
		return settings;
	}

	Result<ProjectSettings> ProjectSerializer::LoadFromString(std::string_view text, const TypeRegistry& registry, const ProjectLoadOptions& options,
		ProjectLoadReport& report)
	{
		report = ProjectLoadReport{};
		Result<Json> document = JsonReader::Parse(text);
		if (!document)
			return std::unexpected(Utils::WithSourceFile(std::move(document).error(), options.SourcePath));
		return FromJson(*document, registry, options, report);
	}

	Status ProjectSerializer::SaveToFile(const ProjectSettings& settings, const TypeRegistry& registry, VirtualFileSystem& vfs, const VfsPath& path)
	{
		ENGINE_TRY_ASSIGN(const std::string text, SaveToString(settings, registry));
		return WithContext(vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))),
			std::format("while saving project settings '{}'", path.ToString()));
	}

	Result<ProjectSettings> ProjectSerializer::LoadFromFile(const VirtualFileSystem& vfs, const VfsPath& path, const TypeRegistry& registry,
		const ProjectLoadOptions& options, ProjectLoadReport& report)
	{
		report = ProjectLoadReport{};
		ProjectLoadOptions fileOptions = options;
		if (fileOptions.SourcePath.empty())
			fileOptions.SourcePath = path.ToString();
		Result<std::string> text = vfs.ReadText(path);
		if (!text)
			return std::unexpected(Utils::WithSourceFile(std::move(text).error(), fileOptions.SourcePath));
		return LoadFromString(*text, registry, fileOptions, report);
	}

}
