#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class TypeRegistry;
	class VirtualFileSystem;

	struct ProjectLoadOptions
	{
		// --strict (CI): unknown keys are errors instead of REFLECTION_UNKNOWN_FIELD warnings (§6).
		bool StrictUnknowns = false;
		// The file's path for ErrorLocation::File ("Projects/RollingBall/RollingBall.eproj"); may be empty.
		std::string SourcePath;
	};

	// What a project load reports besides its result: the file's version and its warnings in document order.
	struct ProjectLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics;
	};

	// .eproj files (Architecture §6.1): {"Format": "Project", "Version": 1, <ProjectSettings fields in registry order>},
	// written by the canonical JsonWriter (default values included, Input.Actions keys sorted, §5.4, §6) and read strictly
	// through the registry (StructInfo::FromJson of "ProjectSettings": missing keys keep their defaults, unknown keys warn,
	// every value validated, enum names exact). Load then save is byte-identical for a canonical file. Requires a registry
	// on which RegisterProjectSettingsTypes ran and that is frozen (asserted). Static functions only; pure apart from the
	// VFS calls; thread-safe for distinct arguments.
	class ProjectSerializer
	{
	public:
		static constexpr std::string_view FormatName = "Project";
		static constexpr uint32_t CurrentVersion = 1;

		// The canonical document. Errors: Validation for a value that cannot be written (non-finite), located.
		[[nodiscard]] static Result<Json> ToJson(const ProjectSettings& settings, const TypeRegistry& registry);
		[[nodiscard]] static Result<std::string> SaveToString(const ProjectSettings& settings, const TypeRegistry& registry);

		// Reads a project document. Errors: Validation (located, with every bad field as an issue) for a wrong "Format", a
		// version below 1, a wrong JSON type, an out-of-range or invalid value (Simulation.FixedHz outside [1, 100000],
		// Scripting.CallbackBudgetMs below 10, undeclared collision layers, a malformed Export.Version, ...);
		// UnsupportedVersion naming both versions for a newer file.
		[[nodiscard]] static Result<ProjectSettings> FromJson(const Json& document, const TypeRegistry& registry,
			const ProjectLoadOptions& options, ProjectLoadReport& report);

		// JsonReader::Parse then FromJson. Errors: Parse (line and column), and as FromJson.
		[[nodiscard]] static Result<ProjectSettings> LoadFromString(std::string_view text, const TypeRegistry& registry,
			const ProjectLoadOptions& options, ProjectLoadReport& report);

		// Through the VFS: an atomic write (§4.10), and a UTF-8-validated read (options.SourcePath defaults to the path).
		[[nodiscard]] static Status SaveToFile(const ProjectSettings& settings, const TypeRegistry& registry, VirtualFileSystem& vfs,
			const VfsPath& path);
		[[nodiscard]] static Result<ProjectSettings> LoadFromFile(const VirtualFileSystem& vfs, const VfsPath& path, const TypeRegistry& registry,
			const ProjectLoadOptions& options, ProjectLoadReport& report);
	};

}
