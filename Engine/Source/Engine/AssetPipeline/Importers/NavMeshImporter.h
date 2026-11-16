#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	// .navmesh files (M30, Docs/Decisions/0045-m30-navigation.md decision 4; Architecture §6, §7.4): the navigation meshes
	// navigation.bake writes beside the scene it baked (EditorCore/Navigation/NavigationBake.h). ReadNavMeshSource
	// (Asset/NavMeshData.h), cooked unchanged with CookNavMesh. One artifact, the mesh, under the source's handle; no
	// sub-assets, no settings, no dependency files and no dependencies (a mesh reads only its own source, which holds its bake
	// input's hash, and references no asset; a stale bake is NAV_MESH_STALE's to report: ADR 0042's phase-5 ledger's
	// importer rule). A pure function of the source bytes and the importer version, identical in every run and configuration.
	// Registered in RegisterBuiltinImporters, with its row in Asset/SourceFormats.cpp (the importer NavMesh, ".navmesh", main
	// type NavMesh, importable). Thread-safe; no main-thread state.
	class NavMeshImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "NavMesh";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::NavMesh; }
		// ".navmesh".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		// Errors: ImportFailed (Utils::MakeImportFailed) keeping the message of ReadNavMeshSource's Parse, Validation or
		// UnsupportedVersion; atomic on failure.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
