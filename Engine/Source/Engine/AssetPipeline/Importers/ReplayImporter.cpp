#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/ReplayImporter.h"

#include "Engine/Asset/ReplayData.h"
#include "Engine/Scripting/ScriptCompiler.h"

#include <format>

namespace Engine {

	std::span<const std::string_view> ReplayImporter::GetExtensions() const
	{
		static constexpr std::string_view Extensions[] = { ".replay" };
		return Extensions;
	}

	Result<ImportResult> ReplayImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		ReplayLoadReport report;
		auto parsed = ReplayFromText(AsStringView(context.GetSourceBytes()), report, true);
		if (!parsed)
			return std::unexpected(std::move(parsed).error().WithLocation({ .File = std::string(context.GetSourcePath().GetPath()), .JsonPointer = std::nullopt, .Entity = {} }));
		const auto scene = context.FindAsset(parsed->Header.Scene.Handle);
		if (!scene)
			return std::unexpected(Error(ErrorCode::NotFound, "replay scene handle is unknown").WithLocation({ .File = std::string(context.GetSourcePath().GetPath()), .JsonPointer = "/Scene/Handle", .Entity = {} }));
		if (scene->Type != AssetType::Scene || scene->Kind != AssetMetaKind::Asset)
			return std::unexpected(Error(ErrorCode::Validation, "replay scene handle does not name a standalone Scene").WithLocation({ .File = std::string(context.GetSourcePath().GetPath()), .JsonPointer = "/Scene/Handle", .Entity = {} }));
		std::vector<ReplayBytecodeExpectation> expectations;
		for (size_t i = 0; i < parsed->Expect.size(); ++i)
		{
			const auto& source = parsed->Expect[i];
			const std::string label = std::format("=replay/{}/Expect/{}", metadata.Handle.ToString(), i);
			const std::string pointer = std::format("/Expect/{}/Luau", i);
			ENGINE_TRY_ASSIGN(auto compiled, ScriptCompiler::Compile({ context.GetSourcePath(), source.Luau, ScriptCompileMode::ExpressionOrChunk, label, pointer }));
			ReplayBytecodeExpectation expectation;
			expectation.Tick = source.Tick;
			expectation.Script.Bytecode = std::move(compiled.Bytecode);
			expectation.Script.SourceMap = std::move(compiled.SourceMap);
			expectations.push_back(std::move(expectation));
		}
		ENGINE_TRY_ASSIGN(Buffer cooked, CookReplay(*parsed, expectations, Version));
		ImportResult result;
		result.Artifacts.push_back({ metadata.Handle, AssetType::Replay, {}, std::move(cooked) });
		result.Dependencies.push_back(scene->Handle);
		return result;
	}

}
