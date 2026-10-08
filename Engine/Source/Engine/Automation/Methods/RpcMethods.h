#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <string>
#include <vector>

// rpc.discover (Architecture §13.5): the host's method registry, described. Shared by the Editor and the Runtime (§13.5
// "Runtime subset"; moved here from EditorCore with M7, unchanged in name and wire format, Docs/Decisions/
// 0012-m7-decisions.md decision 12), each describing the methods it registered. Conventions as in MethodRegistry.h.

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// rpc.discover {method?, domain?}: every method (test hooks only when enabled), or the one named `method`, or those of
	// `domain`; giving both is InvalidArgument.
	struct RpcDiscoverParams
	{
		std::string Method{};
		std::string Domain{};
	};

	struct RpcDiscoverResult
	{
		std::string Protocol{};              // "protocolVersion"
		std::vector<std::string> Domains{};  // every domain, in name order
		std::vector<VariantValue> Methods{}; // MethodRegistry::Describe of each selected method, in name order
	};

	namespace Automation {

		// rpc.discover. Errors: NotFound for an unknown method or domain (with suggestions); InvalidArgument for both filters.
		[[nodiscard]] Result<RpcDiscoverResult> RpcDiscover(AutomationMethodContext& context, const RpcDiscoverParams& params);

	}

	void RegisterRpcMethodTypes(TypeRegistry& registry);

	// rpc.discover: available in the launcher state and in the Runtime; AllowedInBatch; not a tool (the bridge's
	// engine_methods wraps it).
	void RegisterRpcMethods(MethodRegistry& methods);

}
