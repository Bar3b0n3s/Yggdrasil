#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include <charconv>
#include <format>

// The graph transpiler (Docs/Decisions/0035-m24-contract.md decisions 7 and 8). FindGraphNodeAtLine, GetGraphProbeKey and
// ParseGraphProbeKey are the contract's; stream G-B implements the rest.

namespace Engine {

	Result<GraphTranspileResult> TranspileGraph(const GraphDocument& /*graph*/, const GraphNodeLibrary& /*library*/, std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TranspileGraph (Docs/Decisions/0035-m24-contract.md decision 7, stream G-B)");
	}

	uint32_t FindGraphNodeAtLine(const GraphTranspileResult& result, uint32_t line)
	{
		if (line == 0 || line > result.LineNodes.size())
			return 0;
		return result.LineNodes[line - 1];
	}

	std::string GetGraphProbeKey(uint32_t node, std::string_view pin)
	{
		return std::format("{}.{}", node, pin);
	}

	std::optional<GraphPinRef> ParseGraphProbeKey(std::string_view key)
	{
		const size_t dot = key.find('.');
		if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size())
			return std::nullopt;
		const std::string_view digits = key.substr(0, dot);
		// The digits alone, without a sign or a leading zero, so a key reads back only as GetGraphProbeKey writes it.
		if (digits.front() == '0')
			return std::nullopt;
		uint32_t node = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), node);
		if (error != std::errc() || end != digits.data() + digits.size() || node == 0 || node > MaxGraphNodeId)
			return std::nullopt;
		return GraphPinRef{ .Node = node, .Pin = std::string(key.substr(dot + 1)) };
	}

	GraphAssetLiterals FindGraphAssetLiterals(const GraphDocument& /*graph*/, const GraphNodeLibrary& /*library*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
