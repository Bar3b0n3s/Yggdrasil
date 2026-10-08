#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Physics layers and collision filtering (Architecture §9.2 "Layers"). A project declares up to MaxPhysicsLayers named
// layers and the pairs of them that collide (PhysicsSettings.Layers and .Collisions, §6.1); the matrix is symmetric. Jolt
// sees object layers, which add the body's kind to the project layer:
//
//     object layer = (layerIndex << 2) | kind,   kind in {Static, Moving, Sensor}
//
// and three broad-phase layers, NonMoving, Moving and Sensor. Two object layers collide when
//
//     matrix[a >> 2][b >> 2] && !(both Static) && !(Sensor and Static) && !(both Sensor)
//
// so sensors never pay for static geometry or for each other, as Jolt recommends (sensors are Kinematic and kept active,
// so they still see sleeping bodies, §9.2). The three Jolt filter interfaces (BroadPhaseLayerInterface,
// ObjectVsBroadPhaseLayerFilter, ObjectLayerPairFilter) are small classes in Physics/Private/ over a PhysicsLayerTable.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 4). The encoding helpers below are complete;
// PhysicsLayerTable is a plain value type, thread-compatible (the world's filters read it from Jolt's worker threads, which
// never write it).

namespace Engine {

	// The most layers a project may declare (§9.2 "up to 16 named project layers"; ProjectSettings validates the same
	// limit, Docs/Decisions/0014-m11-decisions.md decision 4).
	inline constexpr uint32_t MaxPhysicsLayers = 16;

	// The layer every project declares first (PhysicsSettings.Layers[0]); bodies naming an unknown layer use it
	// (PHYSICS_UNKNOWN_LAYER).
	inline constexpr std::string_view DefaultPhysicsLayerName = "Default";

	// The kind part of an object layer.
	enum class PhysicsObjectKind : uint8_t
	{
		Static = 0, // static bodies (explicit or implicit)
		Moving = 1, // kinematic and dynamic bodies, and character inner bodies
		Sensor = 2  // trigger bodies (explicit Kinematic triggers and implicit sensor bodies)
	};

	// Jolt's broad-phase layers.
	enum class PhysicsBroadPhaseLayer : uint8_t
	{
		NonMoving = 0,
		Moving = 1,
		Sensor = 2
	};

	inline constexpr uint32_t PhysicsBroadPhaseLayerCount = 3;

	// An object layer as Jolt stores it (JPH::ObjectLayer is 16 bits wide here, JPH_OBJECT_LAYER_BITS=16).
	using PhysicsObjectLayer = uint16_t;

	// (layerIndex << 2) | kind. layerIndex < MaxPhysicsLayers (asserted by callers).
	[[nodiscard]] constexpr PhysicsObjectLayer MakePhysicsObjectLayer(uint32_t layerIndex, PhysicsObjectKind kind)
	{
		return static_cast<PhysicsObjectLayer>((layerIndex << 2) | std::to_underlying(kind));
	}

	[[nodiscard]] constexpr uint32_t GetPhysicsLayerIndex(PhysicsObjectLayer objectLayer)
	{
		return static_cast<uint32_t>(objectLayer) >> 2;
	}

	[[nodiscard]] constexpr PhysicsObjectKind GetPhysicsObjectKind(PhysicsObjectLayer objectLayer)
	{
		return static_cast<PhysicsObjectKind>(static_cast<uint32_t>(objectLayer) & 3u);
	}

	// The broad-phase layer of an object layer: Static -> NonMoving, Moving -> Moving, Sensor -> Sensor.
	[[nodiscard]] constexpr PhysicsBroadPhaseLayer GetPhysicsBroadPhaseLayer(PhysicsObjectLayer objectLayer)
	{
		switch (GetPhysicsObjectKind(objectLayer))
		{
			case PhysicsObjectKind::Static: return PhysicsBroadPhaseLayer::NonMoving;
			case PhysicsObjectKind::Moving: return PhysicsBroadPhaseLayer::Moving;
			case PhysicsObjectKind::Sensor: return PhysicsBroadPhaseLayer::Sensor;
		}
		return PhysicsBroadPhaseLayer::Moving;
	}

	// The kind rule of the pair filter, without the matrix: !(both Static) && !(Sensor and Static) && !(both Sensor).
	[[nodiscard]] constexpr bool PhysicsObjectKindsCollide(PhysicsObjectKind a, PhysicsObjectKind b)
	{
		if (a == PhysicsObjectKind::Static)
			return b == PhysicsObjectKind::Moving;
		if (a == PhysicsObjectKind::Sensor)
			return b == PhysicsObjectKind::Moving;
		return true;
	}

	// A project's layers and collision matrix (§9.2). Built once per play session from the project's PhysicsSettings and
	// copied into the world; the validator builds one too (PHYSICS_UNKNOWN_LAYER).
	class PhysicsLayerTable
	{
	public:
		// A table with the single layer "Default" colliding with itself: the defaults of PhysicsSettings.
		PhysicsLayerTable();

		// The table of `layers` (PhysicsSettings.Layers) and `collisions` (PhysicsSettings.Collisions: pairs of declared
		// names; a pair may name one layer twice). Errors: Validation located at the offending element ("/Layers",
		// "/Layers/<i>", "/Collisions/<i>", "/Collisions/<i>/<side>") for no layer or more than MaxPhysicsLayers, a first layer
		// other than DefaultPhysicsLayerName, an empty or repeated name, a collision entry that is not exactly two names, or
		// a name that is not declared: the rules the project settings validator applies (ProjectSettings.h), so a project
		// that loaded always gives a table.
		[[nodiscard]] static Result<PhysicsLayerTable> Create(std::span<const std::string> layers, std::span<const std::vector<std::string>> collisions);

		// The layer names, in declaration order (index = layer index).
		[[nodiscard]] std::span<const std::string> GetNames() const;
		[[nodiscard]] uint32_t GetLayerCount() const;
		// The index of the layer named exactly `name` (case-sensitive), or nullopt.
		[[nodiscard]] std::optional<uint32_t> FindLayer(std::string_view name) const;
		// The name of layer `index` (< GetLayerCount(), asserted).
		[[nodiscard]] const std::string& GetName(uint32_t index) const;

		// The collision matrix: whether layers `a` and `b` collide (symmetric; both < GetLayerCount(), asserted).
		[[nodiscard]] bool LayersCollide(uint32_t a, uint32_t b) const;
		// The full pair rule of the class comment over two object layers whose layer indices are < GetLayerCount()
		// (asserted). Jolt's ObjectLayerPairFilter.
		[[nodiscard]] bool ShouldCollide(PhysicsObjectLayer a, PhysicsObjectLayer b) const;
		// Whether an object layer may collide with anything in a broad-phase layer (Jolt's ObjectVsBroadPhaseLayerFilter):
		// the kind rule, and some layer of the matrix row colliding at all.
		[[nodiscard]] bool ShouldCollide(PhysicsObjectLayer objectLayer, PhysicsBroadPhaseLayer broadPhaseLayer) const;

		// The mask of the named layers (Physics.LayerMask("Track", "Default"), §11.5); no names give 0. Errors: NotFound
		// naming the first unknown name, with a "did you mean" hint over the declared names (FuzzySuggest).
		[[nodiscard]] Result<PhysicsLayerMask> MakeMask(std::span<const std::string_view> names) const;
		// Whether `mask` selects layer `index`.
		[[nodiscard]] static constexpr bool MaskContains(PhysicsLayerMask mask, uint32_t index)
		{
			return index < 32 && (mask & (PhysicsLayerMask{ 1 } << index)) != 0;
		}
	private:
		std::vector<std::string> m_Names;
		// Row i: bit j set when layers i and j collide; symmetric.
		std::array<uint16_t, MaxPhysicsLayers> m_Matrix{};
	};

}
