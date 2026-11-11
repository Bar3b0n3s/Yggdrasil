#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"

#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string_view>

// The graph document (Docs/Decisions/0035-m24-contract.md decision 4). IsGraphIdentifier, FindGraphNode and
// GetNextGraphNodeId are the contract's; stream G-A implements the rest.

namespace Engine {

	namespace Utils {

		// Luau's reserved words, which no graph name may be: the transpiler writes "<Name> = Field..." and "self.<Name>".
		static constexpr std::array<std::string_view, 21> LuauReservedWords = { "and", "break", "do", "else", "elseif", "end", "false", "for",
			"function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };

	}

	void RegisterGraphTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool IsGraphIdentifier(std::string_view name)
	{
		if (name.empty() || name.size() > MaxGraphNameBytes || name.starts_with("__"))
			return false;
		const auto isLetter = [](char character)
		{
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
		};
		if (!isLetter(name.front()))
			return false;
		return std::ranges::all_of(name, [&isLetter](char character)
		{
			return isLetter(character) || (character >= '0' && character <= '9');
		}) && std::ranges::find(Utils::LuauReservedWords, name) == Utils::LuauReservedWords.end();
	}

	Status ValidateGraphStructure(const GraphDocument& /*graph*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidateGraphStructure (Docs/Decisions/0035-m24-contract.md decision 4, stream G-A)");
	}

	void CanonicalizeGraph(GraphDocument& /*graph*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const GraphNode* FindGraphNode(const GraphDocument& graph, uint32_t id)
	{
		const auto found = std::ranges::lower_bound(graph.Nodes, id, std::less<>(), &GraphNode::Id);
		return found != graph.Nodes.end() && found->Id == id ? &*found : nullptr;
	}

	uint32_t GetNextGraphNodeId(const GraphDocument& graph)
	{
		uint32_t highest = 0;
		for (const GraphNode& node : graph.Nodes)
			highest = std::max(highest, node.Id);
		return highest >= MaxGraphNodeId ? 0 : highest + 1;
	}

	Result<Json> GraphToJson(const GraphDocument& /*graph*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphToJson (Docs/Decisions/0035-m24-contract.md decision 4, stream G-A)");
	}

	Result<std::string> GraphToText(const GraphDocument& /*graph*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphToText (Docs/Decisions/0035-m24-contract.md decision 4, stream G-A)");
	}

	Result<GraphDocument> GraphFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, GraphLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphFromJson (Docs/Decisions/0035-m24-contract.md decision 4, stream G-A)");
	}

	Result<GraphDocument> GraphFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, GraphLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphFromText (Docs/Decisions/0035-m24-contract.md decision 4, stream G-A)");
	}

}
