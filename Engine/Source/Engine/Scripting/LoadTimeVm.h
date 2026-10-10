#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <functional>

namespace Engine {

	struct ScriptCheckRequest;
	struct ScriptData;

	struct LoadTimeVmSpecification
	{
		uint32_t MemoryLimitMB = 256;
		// Same safety-clock contract as SandboxSpecification; empty selects lua_clock. Borrowed captures must live
		// for Extract only. This does not change the budget or expose time to the imported script.
		std::function<double()> ClockSeconds{};
	};

	// Import-time classification/extraction (§11.2). A fresh isolated VM/cache/fixed random seed per Extract, on the
	// calling import thread. Concurrent Extract calls are independent after process initialization. No Session,
	// AssetPipeline or Testing dependency, native filesystem reads, live scene or runtime engine host.
	class LoadTimeVm
	{
	public:
		static constexpr uint32_t RequireGraphBudgetMs = 250;

		// request.Source is the authoritative root text; only required files use request.Modules (non-null, borrowed
		// for this call). Uses RequireResolver for every edge, caches each return once per import, detects cycles.
		// One 250 ms deadline begins before root evaluation and covers the entire graph including nested require;
		// dependency reads/compilation are charged to it and checked at their boundaries, never given fresh budgets.
		// Returns Asset's ScriptData: engine bytecode + source map + authenticated Behaviour/Module/TestSuite kind,
		// name/field descriptors and canonical require edges (handles unset for the importer to resolve). No VM data
		// survives. Pure module tables/functions need not be serializable; only persistent metadata is extracted.
		// Errors are located: CompileFailed, Script (cycle/API misuse/invalid schema/memory), Timeout, reader errors,
		// InvalidArgument for missing reader/invalid limits, InvalidState before process startup. Import failure closes
		// the whole throwaway VM. Dist returns Unsupported without reading or compiling source.
		[[nodiscard]] static Result<Ref<const ScriptData>> Extract(const ScriptCheckRequest& request,
			const LoadTimeVmSpecification& specification = {});
	};

}
