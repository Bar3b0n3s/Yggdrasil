#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <optional>

// The Terrain proxy's methods (M26, C-TERR; Docs/Decisions/0037-m26-contract.md decision 26, stream T-B): Terrain:SampleHeight
// and SampleNormal, which read the terrain's surface under a world point by the rule its drawing and its TerrainCollider's
// height field follow (Asset/TerrainGeometry.h): the point's XZ in the entity's local space, the local surface there, and
// back to world space. Reads only: they work in read-only evaluations and on an entity pending destruction, and change no
// state. Only IEEE-exact operations and sqrt (Scripting is on the simulation path's lint scope, §4.12).

namespace Engine {

	namespace {

		// The surface under a world point, in world space.
		struct TerrainSurfacePoint
		{
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		};

	}

	namespace Utils {

		// The receiver of a terrain method: a Terrain that is live, or pending destruction until the flush (M20, W-P02).
		static Entity TerrainSelf(ScriptCall& call)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponentForRead(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "Terrain")
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindReadableEntityByID(proxy.Entity.ID);
		}

		// The surface of the receiver's Terrain under the world point of argument 2 (a vector): nullopt outside the terrain,
		// without a Heightfield or an asset manager, or when the heightfield does not load as one (TERRAIN_NO_HEIGHTFIELD).
		// The point's XZ goes through the entity's inverse world matrix, SampleTerrainHeight and SampleTerrainNormal give the
		// local surface there, and the entity's world matrix carries it back: the position as a point, the normal by the
		// inverse transpose, normalized.
		static std::optional<TerrainSurfacePoint> SampleTerrainSurface(ScriptCall& call)
		{
			const Entity entity = TerrainSelf(call);
			const glm::vec3 position = Lua::Check<glm::vec3>(call, 2);
			const TerrainComponent& terrain = entity.GetComponent<TerrainComponent>();
			AssetManager* assets = call.Engine->GetHost().GetAssets();
			if (!terrain.Heightfield.IsValid() || assets == nullptr)
				return std::nullopt;
			const Result<AssetRef<Asset>> loaded = Detail::LoadAssetAsHost(call, *assets, terrain.Heightfield.GetHandle());
			const AssetRef<HeightfieldData> heightfield = loaded.has_value() ? AssetCast<HeightfieldData>(*loaded) : nullptr;
			if (heightfield == nullptr || !ValidateHeightfieldData(*heightfield).has_value())
				return std::nullopt;

			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			const glm::vec3 local(glm::affineInverse(world) * glm::vec4(position, 1.0f));
			const TerrainExtent extent{ .Size = terrain.Size, .HeightScale = terrain.HeightScale };
			const glm::vec2 localXZ(local.x, local.z);
			const std::optional<float> height = SampleTerrainHeight(*heightfield, extent, localXZ);
			const std::optional<glm::vec3> normal = SampleTerrainNormal(*heightfield, extent, localXZ);
			if (!height.has_value() || !normal.has_value())
				return std::nullopt;
			const glm::vec3 worldNormal = glm::inverseTranspose(glm::mat3(world)) * *normal;
			return TerrainSurfacePoint{ .Position = glm::vec3(world * glm::vec4(local.x, *height, local.z, 1.0f)), .Normal = glm::normalize(worldNormal) };
		}

		// Terrain:SampleHeight(position): the world height of the surface under the world point `position`, nil outside the
		// terrain or without a loaded heightfield.
		static int TerrainSampleHeight(ScriptCall& call)
		{
			const std::optional<TerrainSurfacePoint> surface = SampleTerrainSurface(call);
			if (!surface.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::Push(call, surface->Position.y);
			return 1;
		}

		// Terrain:SampleNormal(position): the surface's world unit normal there, nil where SampleHeight is.
		static int TerrainSampleNormal(ScriptCall& call)
		{
			const std::optional<TerrainSurfacePoint> surface = SampleTerrainSurface(call);
			if (!surface.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::Push(call, surface->Normal);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterTerrain(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Type("Terrain")
				.Method("SampleHeight", Utils::TerrainSampleHeight, "(self: Terrain, position: vector) -> number?",
					"Read the world-space height of the terrain's surface under the world point position (its XZ in the terrain's local space, the "
					"surface there, back to world space); nil outside the terrain or without a loaded heightfield.",
					read)
				.Method("SampleNormal", Utils::TerrainSampleNormal, "(self: Terrain, position: vector) -> vector?",
					"Read the world-space unit normal of the terrain's surface under the world point position; nil where SampleHeight is nil.", read);
			return {};
		}

	}

}
