#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The animation proxies' methods (M23, C23.3, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 31): Animator:Play
// and SetWeight, which set the layers the AnimationSystem phase plays (script-driven blending: a crossfade is a script
// tweening weights), Animator:GetJointTransform and GetRootMotion, which read the pose of the last AnimationSystem phase, and
// SkinnedMesh:SetMorphWeight. Every write goes through the reflected field path (ScriptProxy::WriteField), so it is
// validated, recorded and counted as a field write from a script is.

namespace Engine {

	namespace Utils {

		// The receiver of an animation method that writes: a component of `expected` on a live entity (ValidateComponent
		// refuses one pending destruction).
		static std::pair<ScriptProxyIdentity, Entity> AnimationWriteSelf(ScriptCall& call, std::string_view expected)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponent(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != expected)
				Lua::RaiseError(call, "wrong component receiver");
			return { proxy, host.GetScene().FindEntityByID(proxy.Entity.ID) };
		}

		// The receiver of an animation method that reads: live, or pending destruction until the flush (M20, W-P02).
		static Entity AnimationReadSelf(ScriptCall& call, std::string_view expected)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponentForRead(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != expected)
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindReadableEntityByID(proxy.Entity.ID);
		}

		static void AnimationWrite(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field, const Value& value)
		{
			if (const Status written = ScriptProxy::WriteField(call, proxy, field, value); !written)
				Lua::RaiseError(call, written.error());
		}

		// A layer as the registry struct "AnimatorLayer" (its fields in registration order).
		static Value MakeLayerValue(const AnimatorLayer& layer)
		{
			return Value::FromStruct({ "Clip", "Time", "Speed", "Weight", "Loop" },
				{ Value::FromAssetRef(layer.Clip.GetHandle()), Value::FromFloat(layer.Time), Value::FromFloat(layer.Speed), Value::FromFloat(layer.Weight),
					Value::FromBool(layer.Loop) });
		}

		static void WriteLayers(ScriptCall& call, ScriptProxyIdentity proxy, const std::vector<AnimatorLayer>& layers)
		{
			std::vector<Value> values;
			values.reserve(layers.size());
			for (const AnimatorLayer& layer : layers)
				values.push_back(MakeLayerValue(layer));
			AnimationWrite(call, proxy, "Layers", Value::FromArray(std::move(values)));
		}

		// Animator:Play(layer, clip?, time?): layer `layer` (1-based) plays `clip` from `time` (0 when absent), keeping its
		// Speed, Weight and Loop; #Layers + 1 appends a layer {Speed 1, Weight 1, Loop true}.
		static int AnimatorPlay(ScriptCall& call)
		{
			const auto [proxy, entity] = AnimationWriteSelf(call, "Animator");
			const uint32_t layer = Lua::Check<uint32_t>(call, 2);
			const AssetHandle clip = Lua::IsNoneOrNil(call, 3) ? AssetHandle{} : Lua::Check<AssetHandle>(call, 3);
			const float time = Lua::IsNoneOrNil(call, 4) ? 0.0f : Lua::Check<float>(call, 4);
			if (time < 0.0f)
				return Lua::RaiseError(call, std::format("Animator:Play: the time {} must be at least 0", time));
			std::vector<AnimatorLayer> layers = entity.GetComponent<AnimatorComponent>().Layers;
			if (layer == 0 || layer > layers.size() + 1 || layer > MaxAnimatorLayers)
			{
				return Lua::RaiseError(call,
					Error(ErrorCode::InvalidArgument,
						std::format("Animator:Play: layer {} is not one of the Animator's {} layers or the next one", layer, layers.size()))
						.WithHint(std::format("use a layer from 1 to {} (an Animator holds at most {})", std::min<size_t>(layers.size() + 1, MaxAnimatorLayers),
							MaxAnimatorLayers)));
			}
			if (layer == layers.size() + 1)
				layers.push_back(AnimatorLayer{});
			AnimatorLayer& target = layers[layer - 1];
			target.Clip.SetHandle(clip);
			target.Time = time;
			WriteLayers(call, proxy, layers);
			return 0;
		}

		// Animator:SetWeight(layer, weight): the layer's Weight, for a layer it has and a weight in [0, 1].
		static int AnimatorSetWeight(ScriptCall& call)
		{
			const auto [proxy, entity] = AnimationWriteSelf(call, "Animator");
			const uint32_t layer = Lua::Check<uint32_t>(call, 2);
			const float weight = Lua::Check<float>(call, 3);
			std::vector<AnimatorLayer> layers = entity.GetComponent<AnimatorComponent>().Layers;
			if (layer == 0 || layer > layers.size())
				return Lua::RaiseError(call, std::format("Animator:SetWeight: the Animator has no layer {} (it has {})", layer, layers.size()));
			if (weight < 0.0f || weight > 1.0f)
				return Lua::RaiseError(call, std::format("Animator:SetWeight: the weight {} is outside [0, 1]", weight));
			layers[layer - 1].Weight = weight;
			WriteLayers(call, proxy, layers);
			return 0;
		}

		// Animator:GetJointTransform(joint): the world position and rotation of the joint named `joint` in the last
		// AnimationSystem phase's pose (FindAnimatorJointWorldTransform); nil, nil without a pose or such a joint, and the
		// position with a nil rotation when a scale of 0 on the way leaves the joint no rotation.
		static int AnimatorGetJointTransform(ScriptCall& call)
		{
			const Entity entity = AnimationReadSelf(call, "Animator");
			const std::string joint = Lua::Check<std::string>(call, 2);
			const std::optional<glm::mat4> world = FindAnimatorJointWorldTransform(entity, joint);
			if (!world.has_value())
			{
				Lua::PushNil(call);
				Lua::PushNil(call);
				return 2;
			}
			Lua::Push(call, glm::vec3((*world)[3]));
			// A degenerate scale on the way has no rotation to report.
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(*world);
			if (decomposed.has_value())
				Lua::Push(call, decomposed->Rotation);
			else
				Lua::PushNil(call);
			return 2;
		}

		// Animator:GetRootMotion(): the last step's blended root motion in the entity's parent space, as Apply adds it.
		static int AnimatorGetRootMotion(ScriptCall& call)
		{
			const Entity entity = AnimationReadSelf(call, "Animator");
			RootMotionDelta delta;
			const AnimationPoseComponent* pose = entity.TryGetComponent<AnimationPoseComponent>();
			if (pose != nullptr && entity.GetComponent<AnimatorComponent>().RootMotion != AnimatorRootMotion::None)
				delta = pose->RootMotion;
			const TransformComponent& transform = entity.GetComponent<TransformComponent>();
			Lua::Push(call, glm::vec3(transform.Rotation * (transform.Scale * delta.Translation)));
			Lua::Push(call, delta.Rotation);
			return 2;
		}

		// SkinnedMesh:SetMorphWeight(target, weight): the weight of the mesh's morph target named `target`, MorphWeights grown
		// with the targets' defaults up to it.
		static int SkinnedMeshSetMorphWeight(ScriptCall& call)
		{
			const auto [proxy, entity] = AnimationWriteSelf(call, "SkinnedMesh");
			const std::string target = Lua::Check<std::string>(call, 2);
			const float weight = Lua::Check<float>(call, 3);
			if (weight < -1.0f || weight > 1.0f)
				return Lua::RaiseError(call, std::format("SkinnedMesh:SetMorphWeight: the weight {} is outside [-1, 1]", weight));
			const AssetHandle meshHandle = entity.GetComponent<MeshRendererComponent>().Mesh.GetHandle();
			AssetManager* assets = call.Engine->GetHost().GetAssets();
			if (meshHandle.IsValid() && assets == nullptr)
				return Lua::RaiseError(call, "asset service is unavailable");
			AssetRef<MeshData> mesh;
			if (meshHandle.IsValid())
			{
				const Result<AssetRef<Asset>> loaded = Detail::LoadAssetAsHost(call, *assets, meshHandle);
				if (!loaded)
					return Lua::RaiseError(call, loaded.error());
				mesh = AssetCast<MeshData>(*loaded);
			}
			std::optional<size_t> found;
			if (mesh != nullptr)
			{
				const auto named = std::ranges::find(mesh->MorphTargets, target, &MeshMorphTarget::Name);
				if (named != mesh->MorphTargets.end())
					found = static_cast<size_t>(named - mesh->MorphTargets.begin());
			}
			if (!found.has_value())
			{
				const std::string meshName = meshHandle.IsValid() && assets != nullptr ? assets->GetReferencePath(meshHandle) : std::string("(none)");
				return Lua::RaiseError(call,
					Error(ErrorCode::NotFound, std::format("SkinnedMesh:SetMorphWeight: the mesh '{}' has no morph target named '{}'", meshName, target))
						.WithHint("name a morph target of the MeshRenderer's mesh, or set MorphWeights by index"));
			}
			const size_t index = *found;
			std::vector<Value> values;
			const std::vector<float>& current = entity.GetComponent<SkinnedMeshComponent>().MorphWeights;
			for (size_t slot = 0; slot < std::max(current.size(), index + 1); ++slot)
			{
				float value = weight;
				if (slot != index)
					value = slot < current.size() ? current[slot] : mesh->MorphTargets[slot].DefaultWeight;
				values.push_back(Value::FromFloat(value));
			}
			AnimationWrite(call, proxy, "MorphWeights", Value::FromArray(std::move(values)));
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterAnimation(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions write{};
			api.Type("Animator")
				.Method("Play", Utils::AnimatorPlay, "(self: Animator, layer: number, clip: AssetRef?, time: number?) -> ()",
					std::format("Set the 1-based layer to play the clip from time seconds (0 when absent), keeping its Speed, Weight and Loop; the "
								"next index appends a layer with Speed 1, Weight 1 and Loop. A layer beyond the next one or beyond {}, or a time below 0, "
								"raises an error.",
						MaxAnimatorLayers),
					write)
				.Method("SetWeight", Utils::AnimatorSetWeight, "(self: Animator, layer: number, weight: number) -> ()",
					"Set the 1-based layer's Weight, from 0 (no effect) to 1 (it overrides the layers before it on the paths its clip keys); a crossfade "
					"tweens two layers' weights. A layer the Animator lacks or a weight outside [0, 1] raises an error.",
					write)
				.Method("GetJointTransform", Utils::AnimatorGetJointTransform, "(self: Animator, joint: string) -> (vector?, Quat?)",
					"Read the world position and rotation of the skeleton's joint named joint in the pose of the last fixed step's AnimationSystem phase; "
					"nil, nil without a pose or such a joint, and the position with a nil rotation when a scale of 0 on the way (a clip scaling a "
					"joint to nothing) leaves the joint no rotation.",
					read)
				.Method("GetRootMotion", Utils::AnimatorGetRootMotion, "(self: Animator) -> (vector, Quat)",
					"Read the last fixed step's blended root motion in the entity's parent space: the translation RootMotion Apply adds to the "
					"Transform's Translation and the rotation it multiplies into Rotation; zero and identity without a pose or with RootMotion None.",
					read);
			api.Type("SkinnedMesh")
				.Method("SetMorphWeight", Utils::SkinnedMeshSetMorphWeight, "(self: SkinnedMesh, target: string, weight: number) -> ()",
					"Set the weight of the mesh's morph target named target, growing MorphWeights with the targets' default weights; an error names "
					"the target when the mesh has no such target, and a weight outside [-1, 1] raises one.",
					write);
			return {};
		}

	}

}
