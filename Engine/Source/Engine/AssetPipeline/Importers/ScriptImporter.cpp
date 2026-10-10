#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Scripting/LoadTimeVm.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace Engine {

	namespace Utils {

		class ScriptImportModuleReader final : public IScriptModuleReader
		{
		public:
			explicit ScriptImportModuleReader(ImportContext& context)
				: m_Context(context)
			{
			}

			Result<AssetHandle> ResolveHandle(const VfsPath& path)
			{
				const auto asset = m_Context.FindAsset(path);
				if (!asset)
					return std::unexpected(Error(ErrorCode::NotFound, "required script has no standalone asset").WithLocation({ .File = path.ToString(), .JsonPointer = std::nullopt, .Entity = {} }));
				if (asset->Kind != AssetMetaKind::Asset || asset->Type != AssetType::Script || !asset->Handle.IsValid())
					return std::unexpected(Error(ErrorCode::Validation, "required module must be a standalone Script asset").WithLocation({ .File = path.ToString(), .JsonPointer = std::nullopt, .Entity = {} }));
				m_Dependencies.insert(asset->Handle);
				return asset->Handle;
			}

			Result<std::string> ReadModule(const VfsPath& path) override
			{
				if (const auto found = m_Sources.find(path); found != m_Sources.end())
					return found->second;
				// Record missing handle lookups too; a later-created standalone module must invalidate this attempt.
				auto handle = ResolveHandle(path);
				auto bytes = m_Context.ReadDependency(path);
				Result<std::string> result = !handle ? Result<std::string>(std::unexpected(std::move(handle).error())) : (!bytes ? Result<std::string>(std::unexpected(std::move(bytes).error())) : Result<std::string>(std::string(AsStringView(*bytes))));
				m_Sources.emplace(path, result);
				return result;
			}

			std::vector<AssetHandle> GetDependencies(AssetHandle root) const
			{
				std::vector<AssetHandle> result;
				for (const auto handle : m_Dependencies)
				{
					if (handle != root)
						result.push_back(handle);
				}
				return result;
			}
		private:
			ImportContext& m_Context; // borrowed for the enclosing synchronous Import call
			std::map<VfsPath, Result<std::string>> m_Sources{};
			std::set<AssetHandle> m_Dependencies{};
		};

	}

	std::span<const std::string_view> ScriptImporter::GetExtensions() const
	{
		static constexpr std::string_view Extensions[] = { ".luau" };
		return Extensions;
	}

	Result<ImportResult> ScriptImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		Utils::ScriptImportModuleReader modules(context);
		const ScriptCheckRequest request{ context.GetSourcePath(), AsStringView(context.GetSourceBytes()), &modules };
		ScriptImportCheck check;
		check.SourceHash = XXH64(context.GetSourceBytes());
		if (auto* provider = context.GetScriptDiagnostics())
		{
			check.Performed = true;
			check.EnvironmentHash = provider->GetEnvironmentHash();
			check.Diagnostics = provider->CheckScript(request);
		}
		context.SetScriptCheck(std::move(check));
		ENGINE_TRY_ASSIGN(const auto extracted, LoadTimeVm::Extract(request));
		ScriptData script = *extracted;
		for (auto& edge : script.Requires)
		{
			ENGINE_TRY_ASSIGN(const auto path, VfsPath::Create("project", edge.Path));
			ENGINE_TRY_ASSIGN(edge.Handle, modules.ResolveHandle(path));
		}
		std::sort(script.Requires.begin(), script.Requires.end(), [](const ScriptRequire& left, const ScriptRequire& right)
		{
			return std::tie(left.From, left.Request, left.Path, left.Handle) < std::tie(right.From, right.Request, right.Path, right.Handle);
		});
		script.Requires.erase(std::unique(script.Requires.begin(), script.Requires.end()), script.Requires.end());
		ENGINE_TRY_ASSIGN(Buffer cooked, CookScript(script, Version));
		ImportResult result;
		result.Artifacts.push_back({ metadata.Handle, AssetType::Script, {}, std::move(cooked) });
		result.Dependencies = modules.GetDependencies(metadata.Handle);
		return result;
	}

}
