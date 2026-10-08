#include "TestsPCH.h"

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/EnvironmentImporter.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>

// The cross-importer suite of Roadmap M6 (named after what it sweeps, CodeStyle §14): every importer over every asset
// fixture of the repository.

namespace Engine {

	namespace {

		// The XXH64 of every artifact of one import, in ImportResult order, plus the dependencies and diagnostic codes.
		struct ImportFingerprint
		{
			std::vector<std::pair<AssetHandle, uint64_t>> Artifacts;
			std::vector<AssetHandle> Dependencies;
			std::vector<std::string> DiagnosticCodes;

			bool operator==(const ImportFingerprint&) const = default;
		};

		Result<ImportFingerprint> ImportOnce(const IAssetImporter& importer, const VirtualFileSystem& vfs, const VfsPath& source,
			const VariantValue& settings, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(const Buffer bytes, vfs.ReadFile(source));
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(Hash64(0, source.GetPath()));
			metadata.Type = importer.GetMainType();
			metadata.Importer = std::string(importer.GetId());
			metadata.ImporterVersion = importer.GetVersion();
			metadata.Settings = settings;
			ImportContext context({
				.Vfs = &vfs,
				.SourcePath = source,
				.SourceBytes = bytes,
				.Settings = settings,
				.Registry = &registry,
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			ENGINE_TRY_ASSIGN(const ImportResult result, importer.Import(context, metadata));
			ImportFingerprint fingerprint;
			for (const ImportedArtifact& artifact : result.Artifacts)
				fingerprint.Artifacts.emplace_back(artifact.Handle, XXH64(artifact.Cooked));
			fingerprint.Dependencies = result.Dependencies;
			for (const AssetDiagnostic& diagnostic : result.Diagnostics)
				fingerprint.DiagnosticCodes.push_back(diagnostic.Code);
			return fingerprint;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("Importers: importing twice gives identical hashes")
		{
			// The fixtures: Tests/Data/Assets (textures, materials, scenes, prefabs, the generated glTF files) as
			// project://Assets, so that the importers read their dependency files inside the asset root as in a project
			// (ImportContext::ReadDependency), and the engine's Resources (the Inter font and the sound effect presets) as
			// engine://.
			Test::AssetTestFixture fixture;
			VirtualFileSystem vfs;
			Result<Scope<NativeDirectoryMount>> data = NativeDirectoryMount::Create(Test::GetTestDataPath(), MountAccess::ReadOnly);
			REQUIRE_MESSAGE(data.has_value(), data.error().ToString());
			REQUIRE(vfs.Mount("project", std::move(*data)).has_value());
			Result<Scope<NativeDirectoryMount>> resources = NativeDirectoryMount::Create(Test::GetRepositoryRoot() / "Resources", MountAccess::ReadOnly);
			REQUIRE(resources.has_value());
			REQUIRE(vfs.Mount("engine", std::move(*resources)).has_value());

			std::vector<VfsPath> sources;
			for (const std::string_view root : { "project://Assets", "engine://Fonts", "engine://Audio" })
			{
				Result<std::vector<VfsEntry>> listed = vfs.List(Test::ParseVfsPath(root), true);
				REQUIRE_MESSAGE(listed.has_value(), listed.error().ToString());
				for (const VfsEntry& entry : *listed)
				{
					if (!entry.Info.IsDirectory && fixture.GetImporters().FindForExtension(entry.Path.GetExtension()) != nullptr)
						sources.push_back(entry.Path);
				}
			}

			std::set<std::string> covered;
			for (const VfsPath& source : sources)
			{
				CAPTURE(source.ToString());
				const IAssetImporter* importer = fixture.GetImporters().FindForExtension(source.GetExtension());
				REQUIRE(importer != nullptr);
				VariantValue settings;
				if (!importer->GetSettingsTypeName().empty())
				{
					Result<VariantValue> defaults = fixture.GetManager().MergeImportSettings(importer->GetId(), VariantValue(), Json::object());
					REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
					settings = *defaults;
				}
				// Fixtures meant to fail (the rejected glTF variants) fail the same way twice; the others must match exactly.
				const Result<ImportFingerprint> first = ImportOnce(*importer, vfs, source, settings, fixture.GetRegistry());
				const Result<ImportFingerprint> second = ImportOnce(*importer, vfs, source, settings, fixture.GetRegistry());
				REQUIRE(first.has_value() == second.has_value());
				if (first.has_value())
				{
					CHECK(*first == *second);
					CHECK_FALSE(first->Artifacts.empty());
					covered.insert(std::string(importer->GetId()));
				}
				else
				{
					CHECK(first.error().ToString() == second.error().ToString());
				}
			}

			// Every importer is covered by at least one fixture that imports. EnvironmentImporter imports only with a GPU baker
			// (§8.6), so its determinism is checked by the GPU test "EnvironmentImporter: bakes and cooks the Studio HDRI
			// deterministically" instead.
			for (const IAssetImporter* importer : fixture.GetImporters().GetImporters())
			{
				CAPTURE(std::string(importer->GetId()));
				if (importer->GetId() != EnvironmentImporter::Id)
					CHECK(covered.contains(std::string(importer->GetId())));
			}
		}
	}

}
