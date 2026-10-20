#include "EnginePCH.h"
#include "Engine/Session/NativeWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Hash.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace Engine {

	struct NativeWorld::State
	{
		Ref<const NativeModuleTypes> Types{};
	};

	NativeWorld::NativeWorld(ConstructionKey /*key*/, Ref<const NativeModuleTypes> types)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_ASSERT(types != nullptr, "a native world needs its module's types");
		m_State->Types = std::move(types);
	}

	NativeWorld::~NativeWorld() = default;

	Result<Scope<NativeWorld>> NativeWorld::Create(Ref<const NativeModuleTypes> /*types*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "a play session's native world (Docs/Decisions/0030-m19-contract.md decision 28, stream N-B)"));
	}

	void NativeWorld::Step(PlaySession& /*session*/, const SimStep& /*step*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void NativeWorld::AppendStateHash(XXH64Hasher& /*hasher*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	Json NativeWorld::GetResources() const
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	const NativeModuleTypes& NativeWorld::GetTypes() const
	{
		return *m_State->Types;
	}

}
