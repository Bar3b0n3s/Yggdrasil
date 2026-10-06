#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/LoadReport.h"

#include <cstdint>

namespace Engine {

	class TypeRegistry;

	// Migrations of scene and prefab documents (Architecture §6): pure json -> json functions, file-level (by "Version")
	// and per component (by "ComponentVersions", through ComponentInfo::Migrate). Every migration has a golden fixture in
	// Tests/Data/Formats/, and fixtures of every old version keep loading. Static functions only; thread-safe for distinct
	// arguments.
	//
	// Version 0 (Docs/Decisions/0006-m3-decisions.md, decision 12) is the pre-release entity format: entities carry
	// "Enabled" instead of "Active" and a "Components" array of objects {"Type": <registry name>, <fields>...} instead of
	// an object keyed by registry name, and the document has no "ComponentVersions" (every component is at version 1).
	class Migrations
	{
	public:
		// The current entity-document version (scenes and prefabs share it).
		static constexpr uint32_t CurrentVersion = 1;
		// The oldest version that still loads.
		static constexpr uint32_t MinimumVersion = 0;

		// Upgrades `document` in place to CurrentVersion: the file-level steps from its "Version", then each known
		// component whose "ComponentVersions" entry is older than its registered version (unknown components are left
		// untouched for the serializer to preserve). Sets "Version" and the migrated "ComponentVersions" entries, and
		// report.FileVersion and report.Migrated. Errors: UnsupportedVersion naming both versions when the document, or a
		// component's entry, is newer than this build supports; Validation (located) when "Version" is missing, below
		// MinimumVersion or not an integer, or when the document does not have the shape of its version. Atomic: on error
		// `document` is unchanged.
		[[nodiscard]] static Status UpgradeDocument(Json& document, DocumentKind kind, const TypeRegistry& registry,
			LoadReport& report);

		// The file-level step from version 0 to 1 on its own (exposed for the golden-fixture test): renames "Enabled" to
		// "Active", turns each "Components" array into an object keyed by "Type" (array order kept), adds
		// "ComponentVersions" with version 1 for every component type used, in order of first use, and sets "Version" to 1.
		// Errors: Validation (located) for a component without a string "Type" or with a type listed twice on one entity.
		[[nodiscard]] static Status UpgradeVersion0To1(Json& document);
	};

}
