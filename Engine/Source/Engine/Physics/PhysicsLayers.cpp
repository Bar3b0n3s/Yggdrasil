#include "EnginePCH.h"
#include "Engine/Physics/PhysicsLayers.h"

#include "Engine/Core/Assert.h"

// M11 contract stubs (Docs/Decisions/0014-m11-decisions.md): stream A implements the table. The default table is real,
// because every specification that holds a table default-constructs one.

namespace Engine {

	PhysicsLayerTable::PhysicsLayerTable()
		: m_Names{ std::string(DefaultPhysicsLayerName) }
	{
		m_Matrix[0] = 1;
	}

	Result<PhysicsLayerTable> PhysicsLayerTable::Create(std::span<const std::string> /*layers*/, std::span<const std::vector<std::string>> /*collisions*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the physics layer table is not implemented yet (M11 stream A)");
	}

	std::span<const std::string> PhysicsLayerTable::GetNames() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Names;
	}

	uint32_t PhysicsLayerTable::GetLayerCount() const
	{
		ENGINE_CONTRACT_STUB();
		return static_cast<uint32_t>(m_Names.size());
	}

	std::optional<uint32_t> PhysicsLayerTable::FindLayer(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	const std::string& PhysicsLayerTable::GetName(uint32_t index) const
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(index < m_Names.size(), "PhysicsLayerTable::GetName: layer {} of {}", index, m_Names.size());
		return m_Names[index];
	}

	bool PhysicsLayerTable::LayersCollide(uint32_t /*a*/, uint32_t /*b*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool PhysicsLayerTable::ShouldCollide(PhysicsObjectLayer /*a*/, PhysicsObjectLayer /*b*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool PhysicsLayerTable::ShouldCollide(PhysicsObjectLayer /*objectLayer*/, PhysicsBroadPhaseLayer /*broadPhaseLayer*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Result<PhysicsLayerMask> PhysicsLayerTable::MakeMask(std::span<const std::string_view> /*names*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the physics layer table is not implemented yet (M11 stream A)");
	}

}
