#include "EnginePCH.h"
#include "Engine/Physics/PhysicsLayers.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/FuzzySuggest.h"

namespace Engine {

	namespace Utils {

		// A Validation error located at `pointer` (the PhysicsSettings member it is about), as the project settings
		// validator reports the same rule.
		template<typename... Args>
		[[nodiscard]] static std::unexpected<Error> MakeLayerError(std::string pointer, std::format_string<Args...> format, Args&&... args)
		{
			ErrorLocation location;
			location.JsonPointer = std::move(pointer);
			return std::unexpected(Error(ErrorCode::Validation, std::format(format, std::forward<Args>(args)...)).WithLocation(std::move(location)));
		}

		// The object kind a broad-phase layer holds (GetPhysicsBroadPhaseLayer's inverse).
		[[nodiscard]] static PhysicsObjectKind GetBroadPhaseLayerKind(PhysicsBroadPhaseLayer broadPhaseLayer)
		{
			switch (broadPhaseLayer)
			{
				case PhysicsBroadPhaseLayer::NonMoving: return PhysicsObjectKind::Static;
				case PhysicsBroadPhaseLayer::Moving:    return PhysicsObjectKind::Moving;
				case PhysicsBroadPhaseLayer::Sensor:    return PhysicsObjectKind::Sensor;
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsBroadPhaseLayer {}", std::to_underlying(broadPhaseLayer));
			return PhysicsObjectKind::Moving;
		}

		// Whether `kind` is one of the three kinds an object layer encodes (its two low bits may also hold 3).
		[[nodiscard]] static bool IsKnownObjectKind(PhysicsObjectKind kind)
		{
			return std::to_underlying(kind) <= std::to_underlying(PhysicsObjectKind::Sensor);
		}

	}

	PhysicsLayerTable::PhysicsLayerTable()
		: m_Names{ std::string(DefaultPhysicsLayerName) }
	{
		m_Matrix[0] = 1;
	}

	Result<PhysicsLayerTable> PhysicsLayerTable::Create(std::span<const std::string> layers, std::span<const std::vector<std::string>> collisions)
	{
		// The rules and their order follow the project settings validator (ProjectSettings.cpp, ValidatePhysics), so a
		// project that loaded always gives a table.
		if (layers.empty() || layers.size() > MaxPhysicsLayers)
			return Utils::MakeLayerError("/Layers", "must declare 1 to {} layers (got {})", MaxPhysicsLayers, layers.size());

		for (size_t index = 0; index < layers.size(); ++index)
		{
			const std::string& name = layers[index];
			const auto previous = layers.begin() + static_cast<std::ptrdiff_t>(index);
			if (index == 0 && name != DefaultPhysicsLayerName)
				return Utils::MakeLayerError(std::format("/Layers/{}", index), "the first layer must be '{}' (got '{}')", DefaultPhysicsLayerName, name);
			if (name.empty())
				return Utils::MakeLayerError(std::format("/Layers/{}", index), "a layer name must not be empty");
			if (std::find(layers.begin(), previous, name) != previous)
				return Utils::MakeLayerError(std::format("/Layers/{}", index), "the layer '{}' is declared twice", name);
		}

		PhysicsLayerTable table;
		table.m_Names.assign(layers.begin(), layers.end());
		table.m_Matrix.fill(0);
		for (size_t index = 0; index < collisions.size(); ++index)
		{
			const std::vector<std::string>& pair = collisions[index];
			if (pair.size() != 2)
				return Utils::MakeLayerError(std::format("/Collisions/{}", index), "a collision pair names exactly 2 layers (got {})", pair.size());

			std::array<uint32_t, 2> sides{};
			for (size_t side = 0; side < pair.size(); ++side)
			{
				const std::optional<uint32_t> layer = table.FindLayer(pair[side]);
				if (!layer.has_value())
					return Utils::MakeLayerError(std::format("/Collisions/{}/{}", index, side), "'{}' is not a declared layer", pair[side]);
				sides[side] = *layer;
			}
			table.m_Matrix[sides[0]] = static_cast<uint16_t>(table.m_Matrix[sides[0]] | (1u << sides[1]));
			table.m_Matrix[sides[1]] = static_cast<uint16_t>(table.m_Matrix[sides[1]] | (1u << sides[0]));
		}
		return table;
	}

	std::span<const std::string> PhysicsLayerTable::GetNames() const
	{
		return m_Names;
	}

	uint32_t PhysicsLayerTable::GetLayerCount() const
	{
		return static_cast<uint32_t>(m_Names.size());
	}

	std::optional<uint32_t> PhysicsLayerTable::FindLayer(std::string_view name) const
	{
		const auto found = std::find(m_Names.begin(), m_Names.end(), name);
		if (found == m_Names.end())
			return std::nullopt;
		return static_cast<uint32_t>(found - m_Names.begin());
	}

	const std::string& PhysicsLayerTable::GetName(uint32_t index) const
	{
		ENGINE_CORE_ASSERT(index < m_Names.size(), "PhysicsLayerTable::GetName: layer {} of {}", index, m_Names.size());
		// The first layer always exists; it answers an out-of-range index where asserts are compiled out.
		return index < m_Names.size() ? m_Names[index] : m_Names.front();
	}

	bool PhysicsLayerTable::LayersCollide(uint32_t a, uint32_t b) const
	{
		const uint32_t count = GetLayerCount();
		ENGINE_CORE_ASSERT(a < count && b < count, "PhysicsLayerTable::LayersCollide: layers {} and {} of {}", a, b, count);
		if (a >= count || b >= count)
			return false;
		return (m_Matrix[a] & (1u << b)) != 0;
	}

	bool PhysicsLayerTable::ShouldCollide(PhysicsObjectLayer a, PhysicsObjectLayer b) const
	{
		const PhysicsObjectKind kindA = GetPhysicsObjectKind(a);
		const PhysicsObjectKind kindB = GetPhysicsObjectKind(b);
		if (!Utils::IsKnownObjectKind(kindA) || !Utils::IsKnownObjectKind(kindB))
			return false;
		return PhysicsObjectKindsCollide(kindA, kindB) && LayersCollide(GetPhysicsLayerIndex(a), GetPhysicsLayerIndex(b));
	}

	bool PhysicsLayerTable::ShouldCollide(PhysicsObjectLayer objectLayer, PhysicsBroadPhaseLayer broadPhaseLayer) const
	{
		const PhysicsObjectKind kind = GetPhysicsObjectKind(objectLayer);
		const uint32_t layer = GetPhysicsLayerIndex(objectLayer);
		ENGINE_CORE_ASSERT(layer < GetLayerCount(), "PhysicsLayerTable::ShouldCollide: layer {} of {}", layer, GetLayerCount());
		if (!Utils::IsKnownObjectKind(kind) || layer >= GetLayerCount())
			return false;
		return PhysicsObjectKindsCollide(kind, Utils::GetBroadPhaseLayerKind(broadPhaseLayer)) && m_Matrix[layer] != 0;
	}

	Result<PhysicsLayerMask> PhysicsLayerTable::MakeMask(std::span<const std::string_view> names) const
	{
		PhysicsLayerMask mask = 0;
		for (const std::string_view name : names)
		{
			const std::optional<uint32_t> layer = FindLayer(name);
			if (!layer.has_value())
			{
				const std::vector<std::string> suggestions = FuzzySuggest(name, std::span<const std::string>(m_Names));
				std::string hint = MakeDidYouMeanHint(suggestions);
				if (hint.empty())
				{
					hint = "the declared layers are";
					for (size_t index = 0; index < m_Names.size(); ++index)
						hint += std::format("{} '{}'", index == 0 ? "" : ",", m_Names[index]);
				}
				return std::unexpected(Error(ErrorCode::NotFound, std::format("no physics layer is named '{}'", name)).WithHint(std::move(hint)));
			}
			mask |= PhysicsLayerMask{ 1 } << *layer;
		}
		return mask;
	}

}
