#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"

#include "Engine/Scripting/ScriptApiRegistry.h"

// The graph node library (Docs/Decisions/0035-m24-contract.md decision 6). GraphPinTypeToString and AreGraphPinsCompatible
// are the contract's; stream G-B implements the rest.

namespace Engine {

	std::string_view GraphPinTypeToString(GraphPinType type)
	{
		switch (type)
		{
			case GraphPinType::Exec:   return "Exec";
			case GraphPinType::Any:    return "Any";
			case GraphPinType::Bool:   return "Bool";
			case GraphPinType::Number: return "Number";
			case GraphPinType::String: return "String";
			case GraphPinType::Vector: return "Vector";
			case GraphPinType::Color:  return "Color";
			case GraphPinType::Quat:   return "Quat";
			case GraphPinType::Entity: return "Entity";
			case GraphPinType::Asset:  return "Asset";
			case GraphPinType::Enum:   return "Enum";
			case GraphPinType::Object: return "Object";
		}
		return "Unknown";
	}

	bool AreGraphPinsCompatible(const GraphPinDescription& from, const GraphPinDescription& to)
	{
		const bool fromExec = from.Type == GraphPinType::Exec;
		const bool toExec = to.Type == GraphPinType::Exec;
		if (fromExec || toExec)
			return fromExec && toExec;
		if (from.Type == GraphPinType::Any || to.Type == GraphPinType::Any)
			return true;
		// A T? output may give nil, which the strict checker lets reach only an optional input.
		if (from.Optional && !to.Optional)
			return false;
		if (from.Type == GraphPinType::Enum && to.Type == GraphPinType::String)
			return true;
		if (from.Type != to.Type)
			return false;
		if (from.Type == GraphPinType::Enum || from.Type == GraphPinType::Object)
			return from.TypeName == to.TypeName;
		return true;
	}

	struct GraphNodeLibrary::State
	{
		std::vector<GraphNodeDescription> Nodes{};
	};

	GraphNodeLibrary::GraphNodeLibrary(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	GraphNodeLibrary::~GraphNodeLibrary() = default;

	Result<Scope<GraphNodeLibrary>> GraphNodeLibrary::Create(const ScriptApiRegistry& /*api*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphNodeLibrary::Create (Docs/Decisions/0035-m24-contract.md decision 6, stream G-B)");
	}

	std::span<const GraphNodeDescription> GraphNodeLibrary::GetNodes() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const GraphNodeDescription* GraphNodeLibrary::Find(std::string_view /*kind*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<GraphNodeDescription> GraphNodeLibrary::Resolve(std::string_view /*kind*/, const GraphDocument& /*graph*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphNodeLibrary::Resolve (Docs/Decisions/0035-m24-contract.md decision 6, stream G-B)");
	}

	std::vector<GraphNodeDescription> GraphNodeLibrary::GetMemberNodes(const GraphDocument& /*graph*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t GraphNodeLibrary::GetFingerprint() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<GraphIssue> ValidateGraph(const GraphDocument& /*graph*/, const GraphNodeLibrary& /*library*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<uint32_t> FindUnreachableGraphNodes(const GraphDocument& /*graph*/, const GraphNodeLibrary& /*library*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
