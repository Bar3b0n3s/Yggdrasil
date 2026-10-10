#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Physics/CharacterController.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Engine {

	namespace Utils {

		static void ComponentChecked(ScriptCall& call, Status result)
		{
			if (!result)
				Lua::RaiseError(call, result.error());
		}
		static Entity ComponentSelf(ScriptCall& call, std::string_view expected)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			auto& host = call.Engine->GetHost();
			ComponentChecked(call, ScriptProxy::ValidateComponent(host, proxy));
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != expected)
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindEntityByID(proxy.Entity.ID);
		}
		static void ComponentMutation(ScriptCall& call)
		{
			ComponentChecked(call, call.PrepareHostMutation());
		}
		static glm::dquat WidenRotation(const glm::quat& value)
		{
			return { static_cast<double>(value.w), static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z) };
		}
		static glm::quat NarrowRotation(const glm::dquat& value)
		{
			return { static_cast<float>(value.w), static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z) };
		}
		static glm::quat UnitRotation(ScriptCall& call, int index)
		{
			const auto value = WidenRotation(Lua::Check<glm::quat>(call, index));
			return NarrowRotation(value / std::sqrt(glm::dot(value, value)));
		}
		static glm::vec3 FiniteVector(ScriptCall& call, const glm::vec3& value)
		{
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
				Lua::RaiseError(call, "result is not a finite vector");
			return value;
		}
		static void WriteTransform(ScriptCall& call, std::string_view name, const Value& value)
		{
			ComponentChecked(call, ScriptProxy::WriteField(call, Lua::Check<ScriptProxyIdentity>(call, 1), name, value));
		}
		static int TransformTeleport(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			const auto position = Lua::IsNoneOrNil(call, 2) ? TransformSystem::GetWorldPosition(entity) : Lua::Check<glm::vec3>(call, 2);
			const auto rotation = Lua::IsNoneOrNil(call, 3) ? TransformSystem::GetWorldRotation(entity) : UnitRotation(call, 3);
			WriteTransform(call, "WorldPosition", Value::FromVec3(position));
			WriteTransform(call, "WorldRotation", Value::FromQuat(rotation));
			call.Engine->GetHost().MarkTeleported(entity.GetUUID());
			return 0;
		}
		template<int Axis>
		static int TransformAxis(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			const glm::vec3 axis = Axis == 0 ? glm::vec3(0, 0, -1) : (Axis == 1 ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0));
			Lua::Push(call, TransformSystem::GetWorldRotation(entity) * axis);
			return 1;
		}
		static int TransformLookAt(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			const glm::dvec3 target(Lua::Check<glm::vec3>(call, 2));
			const glm::dvec3 up = Lua::IsNoneOrNil(call, 3) ? glm::dvec3(0, 1, 0) : glm::dvec3(Lua::Check<glm::vec3>(call, 3));
			const glm::dvec3 delta = target - glm::dvec3(TransformSystem::GetWorldPosition(entity));
			const double distance = glm::length(delta);
			const double upLength = glm::length(up);
			if (distance == 0 || upLength == 0)
				return Lua::RaiseError(call, "target direction and up must be nonzero");
			const glm::dvec3 forward = delta / distance;
			const glm::dvec3 side = glm::cross(forward, up / upLength);
			const double length = glm::length(side);
			if (length <= 0.000001)
				return Lua::RaiseError(call, "target direction and up must not be parallel");
			const glm::dvec3 right = side / length;
			const glm::quat rotation = NarrowRotation(glm::normalize(glm::quat_cast(glm::dmat3(right, glm::cross(right, forward), -forward))));
			WriteTransform(call, "WorldRotation", Value::FromQuat(rotation));
			return 0;
		}
		static int TransformTranslate(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			glm::vec3 delta = Lua::Check<glm::vec3>(call, 2);
			const auto space = Lua::CheckEnum(call, 3, "Space");
			if (space == 0)
				delta = TransformSystem::GetWorldRotation(entity) * delta;
			WriteTransform(call, "WorldPosition", Value::FromVec3(FiniteVector(call, TransformSystem::GetWorldPosition(entity) + delta)));
			return 0;
		}
		static int TransformRotate(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			const auto degrees = Lua::Check<glm::vec3>(call, 2);
			const auto space = Lua::CheckEnum(call, 3, "Space");
			const auto delta = TransformSystem::QuaternionFromEulerDegrees(degrees);
			const auto current = TransformSystem::GetWorldRotation(entity);
			WriteTransform(call, "WorldRotation", Value::FromQuat(glm::normalize(space == 0 ? current * delta : delta * current)));
			return 0;
		}
		template<int Operation>
		static int TransformPoint(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Transform");
			const auto value = Lua::Check<glm::vec3>(call, 2);
			const auto world = TransformSystem::ComputeWorldMatrix(entity);
			if constexpr (Operation == 0)
				Lua::Push(call, glm::vec3(world * glm::vec4(value, 1.0f)));
			else if constexpr (Operation == 1)
				Lua::Push(call, glm::vec3(glm::affineInverse(world) * glm::vec4(value, 1.0f)));
			else
				Lua::Push(call, TransformSystem::GetWorldRotation(entity) * value);
			return 1;
		}
		static PhysicsSystem& ComponentPhysics(ScriptCall& call)
		{
			auto* physics = call.Engine->GetHost().GetPhysics();
			if (!physics)
				Lua::RaiseError(call, "physics is unavailable");
			return *physics;
		}
		static void CheckBody(ScriptCall& call, PhysicsSystem& physics, UUID entity, std::optional<PhysicsMotionType> motion = {})
		{
			const auto body = physics.GetBodyInfo(entity);
			if (!body)
				Lua::RaiseError(call, "entity has no physics body");
			if (motion && body->MotionType != *motion)
				Lua::RaiseError(call, "body must be " + std::string(PhysicsMotionTypeToString(*motion)));
		}
		static glm::vec3 BoundedVector(ScriptCall& call, int index, float bound)
		{
			const auto value = Lua::Check<glm::vec3>(call, index);
			if (glm::any(glm::greaterThan(glm::abs(value), glm::vec3(bound))))
				Lua::RaiseError(call, "vector exceeds the physics range");
			return value;
		}
		template<auto Method>
		static int BodyVectorWrite(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "RigidBody");
			auto& physics = ComponentPhysics(call);
			const auto value = BoundedVector(call, 2, 1.0e12f);
			CheckBody(call, physics, entity.GetUUID(), PhysicsMotionType::Dynamic);
			ComponentMutation(call);
			ComponentChecked(call, (physics.*Method)(entity.GetUUID(), value));
			return 0;
		}
		static int BodyForceAtPosition(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "RigidBody");
			auto& physics = ComponentPhysics(call);
			const auto force = BoundedVector(call, 2, 1.0e12f);
			const auto position = BoundedVector(call, 3, MaxPhysicsCoordinate);
			CheckBody(call, physics, entity.GetUUID(), PhysicsMotionType::Dynamic);
			ComponentMutation(call);
			ComponentChecked(call, physics.AddForceAtPosition(entity.GetUUID(), force, position));
			return 0;
		}
		template<auto Method>
		static int BodyRead(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "RigidBody");
			auto value = (ComponentPhysics(call).*Method)(entity.GetUUID());
			if (!value)
				return Lua::RaiseError(call, value.error());
			Lua::Push(call, *value);
			return 1;
		}
		template<bool Teleport>
		static int BodyPose(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "RigidBody");
			auto& physics = ComponentPhysics(call);
			const auto position = BoundedVector(call, 2, MaxPhysicsCoordinate);
			const auto rotation = Teleport && Lua::IsNoneOrNil(call, 3) ? TransformSystem::GetWorldRotation(entity) : UnitRotation(call, 3);
			CheckBody(call, physics, entity.GetUUID(), Teleport ? std::nullopt : std::optional{ PhysicsMotionType::Kinematic });
			ComponentMutation(call);
			if constexpr (Teleport)
			{
				ComponentChecked(call, physics.Teleport(entity.GetUUID(), position, rotation));
				call.Engine->GetHost().MarkTeleported(entity.GetUUID());
			}
			else
				ComponentChecked(call, physics.MoveKinematic(entity.GetUUID(), position, rotation));
			return 0;
		}
		static int BodyWake(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "RigidBody");
			auto& physics = ComponentPhysics(call);
			CheckBody(call, physics, entity.GetUUID());
			ComponentMutation(call);
			ComponentChecked(call, physics.WakeUp(entity.GetUUID()));
			return 0;
		}
		static int CharacterMove(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "CharacterController");
			auto& physics = ComponentPhysics(call);
			const auto velocity = BoundedVector(call, 2, MaxCharacterSpeed);
			auto state = physics.GetCharacterState(entity.GetUUID());
			if (!state)
				return Lua::RaiseError(call, state.error());
			ComponentMutation(call);
			ComponentChecked(call, physics.MoveCharacter(entity.GetUUID(), velocity));
			return 0;
		}
		template<int Field>
		static int CharacterRead(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "CharacterController");
			auto state = ComponentPhysics(call).GetCharacterState(entity.GetUUID());
			if (!state)
				return Lua::RaiseError(call, state.error());
			if constexpr (Field == 0)
				Lua::Push(call, state->IsGrounded);
			else if constexpr (Field == 1)
				Lua::Push(call, state->GroundNormal);
			else
				Lua::Push(call, state->Velocity);
			return 1;
		}
		static AudioSystem& ComponentAudio(ScriptCall& call)
		{
			auto* audio = call.Engine->GetHost().GetAudio();
			if (!audio)
				Lua::RaiseError(call, "audio is unavailable");
			return *audio;
		}
		template<auto Method, bool Active>
		static int SourceWrite(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "AudioSource");
			auto& audio = ComponentAudio(call);
			if constexpr (Active)
			{
				if (!entity.IsActive())
					return Lua::RaiseError(call, "entity is disabled");
			}
			ComponentMutation(call);
			ComponentChecked(call, (audio.*Method)(entity));
			return 0;
		}
		static int SourcePlaying(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "AudioSource");
			Lua::Push(call, ComponentAudio(call).IsPlaying(entity));
			return 1;
		}
		static AssetRef<MeshData> MaterialMesh(ScriptCall& call, AssetHandle handle)
		{
			if (!handle.IsValid())
				return nullptr;
			auto* assets = call.Engine->GetHost().GetAssets();
			if (!assets)
				Lua::RaiseError(call, "asset service is unavailable");
			auto loaded = assets->Load(handle);
			if (!loaded)
				Lua::RaiseError(call, loaded.error());
			auto mesh = AssetCast<MeshData>(*loaded);
			if (!mesh)
				Lua::RaiseError(call, "renderer mesh has the wrong asset type");
			return mesh;
		}
		static int MaterialGet(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "MeshRenderer");
			const uint32_t index = Lua::Check<uint32_t>(call, 2);
			const auto renderer = entity.GetComponent<MeshRendererComponent>();
			const auto mesh = MaterialMesh(call, renderer.Mesh.GetHandle());
			if (index == 0 || index > std::max(renderer.Materials.size(), mesh ? mesh->Slots.size() : size_t{}))
				return Lua::RaiseError(call, "material index is outside the 1-based slot range");
			AssetHandle material = index <= renderer.Materials.size() ? renderer.Materials[index - 1].GetHandle() : AssetHandle{};
			if (!material.IsValid() && mesh && index <= mesh->Slots.size())
			{
				material = mesh->Slots[index - 1].DefaultMaterial;
				if (!material.IsValid())
					material = BuiltinAssetHandles::DefaultMaterial;
			}
			Lua::Push(call, material);
			return 1;
		}
		static int MaterialSet(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "MeshRenderer");
			const uint32_t index = Lua::Check<uint32_t>(call, 2);
			const auto asset = Lua::IsNoneOrNil(call, 3) ? AssetHandle{} : Lua::Check<AssetHandle>(call, 3);
			const auto renderer = entity.GetComponent<MeshRendererComponent>();
			const auto mesh = MaterialMesh(call, renderer.Mesh.GetHandle());
			if (index == 0 || index > std::max(renderer.Materials.size(), mesh ? mesh->Slots.size() : size_t{}))
				return Lua::RaiseError(call, "material index is outside the 1-based slot range");
			std::vector<Value> values;
			for (const auto& material : renderer.Materials)
				values.push_back(Value::FromAssetRef(material.GetHandle()));
			values.resize(std::max(values.size(), static_cast<size_t>(index)), Value::FromAssetRef({}));
			values[index - 1] = Value::FromAssetRef(asset);
			ComponentChecked(call, ScriptProxy::WriteField(call, Lua::Check<ScriptProxyIdentity>(call, 1), "Materials", Value::FromArray(std::move(values))));
			return 0;
		}
		static glm::dvec2 CameraSize(ScriptCall& call)
		{
			const glm::dvec2 size(call.Engine->GetHost().GetEnvironment().WindowSize);
			if (!(size.x > 0 && size.y > 0) || !std::isfinite(size.x) || !std::isfinite(size.y))
				Lua::RaiseError(call, "camera projection requires a nonzero viewport");
			return size;
		}
		static int CameraRay(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Camera");
			const auto camera = entity.GetComponent<CameraComponent>();
			const auto size = CameraSize(call);
			const double x = Lua::Check<double>(call, 2), y = Lua::Check<double>(call, 3);
			if (x < 0 || y < 0 || x >= size.x || y >= size.y)
				return Lua::RaiseError(call, "pixel is outside the camera viewport");
			const double nx = 2.0 * (x + 0.5) / size.x - 1.0, ny = 1.0 - 2.0 * (y + 0.5) / size.y;
			const double aspect = size.x / size.y;
			const auto rotation = WidenRotation(TransformSystem::GetRenderRotation(entity));
			glm::dvec3 origin(TransformSystem::GetRenderPosition(entity));
			glm::dvec3 direction(0, 0, -1);
			if (camera.Projection == ProjectionType::Perspective)
			{
				const double tangent = DetMath::Tan(static_cast<double>(camera.VerticalFov) * std::numbers::pi / 360.0);
				direction = glm::normalize(glm::dvec3(nx * aspect * tangent, ny * tangent, -1));
			}
			else
				origin += rotation * glm::dvec3(nx * aspect * camera.OrthographicSize, ny * camera.OrthographicSize, -camera.NearClip);
			Lua::Push(call, glm::vec3(origin));
			Lua::Push(call, glm::vec3(rotation * direction));
			return 2;
		}
		static int CameraScreen(ScriptCall& call)
		{
			const Entity entity = ComponentSelf(call, "Camera");
			const auto camera = entity.GetComponent<CameraComponent>();
			const auto size = CameraSize(call);
			const glm::dvec3 point(Lua::Check<glm::vec3>(call, 2));
			const glm::dvec3 local = glm::inverse(WidenRotation(TransformSystem::GetRenderRotation(entity))) * (point - glm::dvec3(TransformSystem::GetRenderPosition(entity)));
			const double depth = -local.z;
			double halfHeight = camera.OrthographicSize;
			if (camera.Projection == ProjectionType::Perspective)
			{
				if (depth == 0)
					return Lua::RaiseError(call, "point lies on the camera plane");
				halfHeight = depth * DetMath::Tan(static_cast<double>(camera.VerticalFov) * std::numbers::pi / 360.0);
			}
			const double x = (local.x / (halfHeight * size.x / size.y) + 1) * size.x / 2 - 0.5;
			const double y = (1 - local.y / halfHeight) * size.y / 2 - 0.5;
			const double z = camera.Projection == ProjectionType::Perspective ? camera.NearClip / depth : (camera.FarClip - depth) / (camera.FarClip - camera.NearClip);
			Lua::Push(call, glm::vec3(x, y, z));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterComponents(ScriptApiRegistry& api)
		{
			EnumInfo space("Space", "Coordinates relative to the entity's orientation or the world axes.");
			space.AddEntry({ "Local", 0, "Entity orientation." });
			space.AddEntry({ "World", 1, "World axes." });
			ENGINE_TRY(api.RegisterEnum(space));
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions write{};
			ScriptMemberOptions transformSpace;
			transformSpace.EnumParameters.push_back({ 3, "Space", true, "Local" });
			api.Type("Transform")
				.Method("Teleport", Utils::TransformTeleport, "(self: Transform, position: vector?, rotation: Quat?) -> ()", "Set optional world pose and reset interpolation.", write)
				.Method("Forward", Utils::TransformAxis<0>, "(self: Transform) -> vector", "World direction of local -Z.", read)
				.Method("Right", Utils::TransformAxis<1>, "(self: Transform) -> vector", "World direction of local +X.", read)
				.Method("Up", Utils::TransformAxis<2>, "(self: Transform) -> vector", "World direction of local +Y.", read)
				.Method("LookAt", Utils::TransformLookAt, "(self: Transform, target: vector, up: vector?) -> ()", "Face the target using world up +Y by default; reject coincident or parallel directions.", write)
				.Method("Translate", Utils::TransformTranslate, "(self: Transform, delta: vector, space: Space?) -> ()", "Translate in Local orientation by default, or World axes.", transformSpace)
				.Method("Rotate", Utils::TransformRotate, "(self: Transform, eulerDegrees: vector, space: Space?) -> ()", "Rotate using deterministic Z-X-Y Euler degrees in Local space by default.", transformSpace)
				.Method("TransformPoint", Utils::TransformPoint<0>, "(self: Transform, point: vector) -> vector", "Transform a point using world translation, rotation and scale.", read)
				.Method("InverseTransformPoint", Utils::TransformPoint<1>, "(self: Transform, point: vector) -> vector", "Transform a world point into local space.", read)
				.Method("TransformDirection", Utils::TransformPoint<2>, "(self: Transform, direction: vector) -> vector", "Rotate a direction into world space without translation or scale.", read);
			api.Type("RigidBody")
				.Method("AddForce", Utils::BodyVectorWrite<&PhysicsSystem::AddForce>, "(self: RigidBody, force: vector) -> ()", "Apply force to a Dynamic body.", write)
				.Method("AddForceAtPosition", Utils::BodyForceAtPosition, "(self: RigidBody, force: vector, position: vector) -> ()", "Apply force at a world position to a Dynamic body.", write)
				.Method("AddTorque", Utils::BodyVectorWrite<&PhysicsSystem::AddTorque>, "(self: RigidBody, torque: vector) -> ()", "Apply torque to a Dynamic body.", write)
				.Method("AddImpulse", Utils::BodyVectorWrite<&PhysicsSystem::AddImpulse>, "(self: RigidBody, impulse: vector) -> ()", "Apply an impulse to a Dynamic body.", write)
				.Method("AddAngularImpulse", Utils::BodyVectorWrite<&PhysicsSystem::AddAngularImpulse>, "(self: RigidBody, impulse: vector) -> ()", "Apply an angular impulse to a Dynamic body.", write)
				.Method("GetLinearVelocity", Utils::BodyRead<&PhysicsSystem::GetLinearVelocity>, "(self: RigidBody) -> vector", "Read current linear velocity.", read)
				.Method("SetLinearVelocity", Utils::BodyVectorWrite<&PhysicsSystem::SetLinearVelocity>, "(self: RigidBody, velocity: vector) -> ()", "Set a Dynamic body's linear velocity.", write)
				.Method("GetAngularVelocity", Utils::BodyRead<&PhysicsSystem::GetAngularVelocity>, "(self: RigidBody) -> vector", "Read current angular velocity.", read)
				.Method("SetAngularVelocity", Utils::BodyVectorWrite<&PhysicsSystem::SetAngularVelocity>, "(self: RigidBody, velocity: vector) -> ()", "Set a Dynamic body's angular velocity.", write)
				.Method("MoveKinematic", Utils::BodyPose<false>, "(self: RigidBody, position: vector, rotation: Quat) -> ()", "Set a Kinematic target for the next fixed step.", write)
				.Method("Teleport", Utils::BodyPose<true>, "(self: RigidBody, position: vector, rotation: Quat?) -> ()", "Place a body immediately, preserving velocity and resetting interpolation.", write)
				.Method("IsSleeping", Utils::BodyRead<&PhysicsSystem::IsSleeping>, "(self: RigidBody) -> boolean", "Whether the body is sleeping.", read)
				.Method("WakeUp", Utils::BodyWake, "(self: RigidBody) -> ()", "Wake a sleeping body.", write);
			api.Type("CharacterController")
				.Method("Move", Utils::CharacterMove, "(self: CharacterController, velocity: vector) -> ()", "Supply desired velocity for the next fixed character update.", write)
				.Method("IsGrounded", Utils::CharacterRead<0>, "(self: CharacterController) -> boolean", "Whether the last character update found supporting ground.", read)
				.Method("GetGroundNormal", Utils::CharacterRead<1>, "(self: CharacterController) -> vector", "Ground normal from the last update.", read)
				.Method("GetVelocity", Utils::CharacterRead<2>, "(self: CharacterController) -> vector", "Velocity after the last update.", read);
			api.Type("AudioSource")
				.Method("Play", Utils::SourceWrite<&AudioSystem::Play, true>, "(self: AudioSource) -> ()", "Start or restart this active source.", write)
				.Method("Stop", Utils::SourceWrite<&AudioSystem::Stop, false>, "(self: AudioSource) -> ()", "Release this source's voice.", write)
				.Method("Pause", Utils::SourceWrite<&AudioSystem::Pause, false>, "(self: AudioSource) -> ()", "Pause this source without losing its cursor.", write)
				.Method("Resume", Utils::SourceWrite<&AudioSystem::Resume, true>, "(self: AudioSource) -> ()", "Resume this active source's paused voice.", write)
				.Method("IsPlaying", Utils::SourcePlaying, "(self: AudioSource) -> boolean", "Whether this source owns an unpaused voice.", read);
			api.Type("Camera")
				.Method("ScreenToWorldRay", Utils::CameraRay, "(self: Camera, x: number, y: number) -> (vector, vector)", "Ray through a viewport pixel center, origin top-left, using the rendered camera pose.", read)
				.Method("WorldToScreen", Utils::CameraScreen, "(self: Camera, point: vector) -> vector", "Project to top-left pixel coordinates and reverse-Z depth using the rendered camera pose.", read);
			api.Type("MeshRenderer")
				.Method("GetMaterial", Utils::MaterialGet, "(self: MeshRenderer, index: number) -> AssetRef?", "Read a 1-based material slot, resolving an absent or null override to the mesh default.", read)
				.Method("SetMaterial", Utils::MaterialSet, "(self: MeshRenderer, index: number, asset: AssetRef?) -> ()", "Override an existing 1-based mesh or explicit material slot; nil restores the mesh default.", write);
			return {};
		}

	}

}
