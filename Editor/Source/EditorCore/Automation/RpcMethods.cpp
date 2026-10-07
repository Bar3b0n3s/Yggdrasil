#include "EditorPCH.h"
#include "EditorCore/Automation/RpcMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Automation {

		Result<RpcDiscoverResult> RpcDiscover(EditorMethodContext& context, const RpcDiscoverParams& params)
		{
			const MethodRegistry& registry = context.GetRegistry();
			if (!params.Method.empty() && !params.Domain.empty())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/domain", "give method or domain, not both",
					"a method's entry already names its domain"));
			}

			RpcDiscoverResult result;
			result.Protocol = CurrentProtocolVersion.ToString();
			result.Domains = registry.GetDomains();

			std::vector<const MethodDescriptor*> selected;
			if (!params.Method.empty())
			{
				const MethodDescriptor* method = registry.Find(params.Method);
				if (method == nullptr)
				{
					std::vector<std::string> suggestions = registry.SuggestMethodNames(params.Method);
					return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/method", std::format("no method '{}'", params.Method),
						MakeDidYouMeanHint(suggestions)));
				}
				selected.push_back(method);
			}
			else
			{
				for (const MethodDescriptor* method : registry.GetMethods())
				{
					if (params.Domain.empty() || method->Domain == params.Domain)
						selected.push_back(method);
				}
				if (selected.empty())
				{
					std::vector<std::string> suggestions = FuzzySuggest(params.Domain, std::span<const std::string>(result.Domains));
					return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/domain", std::format("no domain '{}'", params.Domain),
						MakeDidYouMeanHint(suggestions)));
				}
			}

			for (const MethodDescriptor* method : selected)
				result.Methods.emplace_back(registry.Describe(*method));
			return result;
		}

	}

	void RegisterRpcMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<RpcDiscoverParams>("RpcDiscoverParams", "The params of rpc.discover: every method, one method or one domain.")
			.Field("method", &RpcDiscoverParams::Method, "Describe only this method, such as \"entity.create\".")
			.Field("domain", &RpcDiscoverParams::Domain, "Describe only the methods of this domain, such as \"scene\".");

		registry.Struct<RpcDiscoverResult>("RpcDiscoverResult", "The method registry, described.")
			.Field("protocolVersion", &RpcDiscoverResult::Protocol, "The protocol version the server speaks.")
			.Field("domains", &RpcDiscoverResult::Domains, "Every domain, in name order.")
			.Field("methods", &RpcDiscoverResult::Methods,
				"Each selected method in name order: name, domain, description, flags, timeout, required params, the params and result "
				"schemas and examples.");
	}

	void RegisterRpcMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["method"] = "entity.create";
		methods.Add(
			{
				.Name = "rpc.discover",
				.Description = "Describes the automation methods: descriptions, flags, full JSON schemas of params and results, and examples. "
							   "Filter by method or by domain.",
				.AvailableInRuntime = true,
				.AvailableInLauncher = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Describe entity.create.", .Params = example } },
			},
			&Automation::RpcDiscover);
	}

}
