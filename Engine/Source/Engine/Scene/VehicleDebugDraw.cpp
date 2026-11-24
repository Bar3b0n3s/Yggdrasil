#include "EnginePCH.h"
#include "Engine/Scene/VehicleDebugDraw.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/PhysicsVehicles.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The matrix an entity is drawn at (as the colliders' records are: ColliderDebugDrawOptions::Alpha): in a runtime scene
		// below alpha 1, its rendered pose; otherwise its world matrix.
		static glm::mat4 GetVehicleDrawnMatrix(ConstEntity entity, float alpha)
		{
			if (alpha < 1.0f && entity.GetScene()->IsRuntime())
				return ComputeRenderedWorldMatrix(entity, alpha);
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		static bool IsFiniteVehicleVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The unit direction of `direction`, or `fallback` when it has no length (a scale of 0).
		static glm::vec3 NormalizeOr(const glm::vec3& direction, const glm::vec3& fallback)
		{
			const float length = glm::length(direction);
			return length > 1.0e-6f && std::isfinite(length) ? direction / length : fallback;
		}

		// A world point the vehicle's body holds, moved from where the body is now to where it is drawn.
		static glm::vec3 ToDrawnVehiclePoint(ConstEntity vehicle, const glm::vec3& point, float alpha)
		{
			if (alpha >= 1.0f)
				return point;
			const glm::vec3 local = glm::vec3(glm::affineInverse(TransformSystem::ComputeWorldMatrix(vehicle)) * glm::vec4(point, 1.0f));
			return glm::vec3(GetVehicleDrawnMatrix(vehicle, alpha) * glm::vec4(local, 1.0f));
		}

	}

	glm::vec4 GetVehicleDebugColor(VehicleDebugState state)
	{
		switch (state)
		{
			case VehicleDebugState::InForce: return glm::vec4(0.95f, 0.35f, 0.8f, 1.0f);
			case VehicleDebugState::Refused: return glm::vec4(0.7f, 0.25f, 0.05f, 1.0f);
		}
		ENGINE_CORE_ASSERT(false, "Unknown VehicleDebugState {}", std::to_underlying(state));
		return glm::vec4(1.0f);
	}

	std::vector<VehicleWheelDebugShape> BuildVehicleDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics, float alpha)
	{
		ENGINE_CORE_ASSERT(alpha >= 0.0f && alpha <= 1.0f, "BuildVehicleDebugDraw: the alpha {} is outside [0, 1]", alpha);
		std::vector<VehicleWheelDebugShape> shapes;
		if (scene.GetRegistry().view<WheeledVehicleComponent>().empty())
			return shapes;
		const PhysicsVehicleComposition vehicles = ComposePhysicsVehicles(scene, ComposePhysicsBodies(scene, layers));
		for (const PhysicsVehiclePlan& plan : vehicles.Vehicles)
		{
			const ConstEntity vehicle = scene.FindEntityByID(plan.Entity);
			if (!vehicle.IsValid())
				continue;
			const std::optional<PhysicsVehicleReport> report = physics != nullptr ? physics->GetVehicleInfo(plan.Entity) : std::nullopt;
			const bool inForce = report.has_value() && report->Created;
			const VehicleDebugState state = (physics != nullptr ? inForce : plan.IsCreatable) ? VehicleDebugState::InForce : VehicleDebugState::Refused;

			// The body's pose as it is drawn: the vehicle entity's drawn matrix without its scale (the body has none).
			const glm::mat4 drawn = Utils::GetVehicleDrawnMatrix(vehicle, alpha);
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(drawn);
			const glm::quat bodyRotation = decomposed.has_value() ? decomposed->Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			const glm::mat4 body = glm::translate(glm::mat4(1.0f), glm::vec3(drawn[3])) * glm::mat4_cast(bodyRotation);
			const glm::vec3 bodyUp = bodyRotation * glm::vec3(0.0f, 1.0f, 0.0f);

			if (inForce)
			{
				// Where the session placed each wheel entity after the last step (suspension, steer and spin), at the rendered pose.
				for (const PhysicsVehicleWheelReport& reading : report->Wheels)
				{
					const ConstEntity wheel = scene.FindEntityByID(reading.Entity);
					const auto* component = wheel.IsValid() ? wheel.TryGetComponent<VehicleWheelComponent>() : nullptr;
					if (component == nullptr)
						continue;
					const glm::mat4 matrix = Utils::GetVehicleDrawnMatrix(wheel, alpha);
					VehicleWheelDebugShape shape;
					shape.Vehicle = plan.Entity;
					shape.Wheel = reading.Entity;
					shape.State = state;
					shape.Center = glm::vec3(matrix[3]);
					shape.Axle = Utils::NormalizeOr(glm::vec3(matrix[0]), bodyRotation * glm::vec3(1.0f, 0.0f, 0.0f));
					shape.Up = Utils::NormalizeOr(glm::vec3(matrix[1]), bodyUp);
					shape.Radius = component->Radius;
					shape.Attachment = shape.Center + bodyUp * reading.SuspensionLength;
					shape.SuspensionEnd = shape.Attachment - bodyUp * component->SuspensionMaxLength;
					shape.Touching = reading.Touching;
					if (reading.Touching)
					{
						shape.ContactPoint = Utils::ToDrawnVehiclePoint(vehicle, reading.Point, alpha);
						shape.ContactNormal = Utils::NormalizeOr(reading.Normal, bodyUp);
					}
					if (Utils::IsFiniteVehicleVector(shape.Center) && Utils::IsFiniteVehicleVector(shape.Attachment) && Utils::IsFiniteVehicleVector(shape.ContactPoint))
						shapes.push_back(shape);
				}
				continue;
			}

			// From the components: each wheel hanging at its authored centre at full droop, where the session would make it now.
			for (const UUID wheelId : plan.Wheels)
			{
				const ConstEntity wheel = scene.FindEntityByID(wheelId);
				const auto* component = wheel.IsValid() ? wheel.TryGetComponent<VehicleWheelComponent>() : nullptr;
				if (component == nullptr)
					continue;
				VehicleWheelDebugShape shape;
				shape.Vehicle = plan.Entity;
				shape.Wheel = wheelId;
				shape.State = state;
				shape.Radius = component->Radius;
				if (const std::optional<PhysicsVehicleWheelFrame> frame = MakePhysicsVehicleWheelFrame(vehicle, wheel))
				{
					shape.Center = glm::vec3(body * glm::vec4(frame->Center, 1.0f));
					const glm::vec3 up = bodyRotation * frame->Up;
					const glm::vec3 forward = bodyRotation * frame->Forward;
					shape.Up = up;
					shape.Axle = Utils::NormalizeOr(glm::cross(forward, up), bodyRotation * glm::vec3(1.0f, 0.0f, 0.0f));
				}
				else
				{
					// A chain that does not decompose: drawn at the wheel's entity, refused as the session would refuse it.
					shape.Center = glm::vec3(Utils::GetVehicleDrawnMatrix(wheel, alpha)[3]);
					shape.State = VehicleDebugState::Refused;
				}
				shape.Attachment = shape.Center + bodyUp * component->SuspensionMaxLength;
				shape.SuspensionEnd = shape.Center;
				if (Utils::IsFiniteVehicleVector(shape.Center) && Utils::IsFiniteVehicleVector(shape.Attachment))
					shapes.push_back(shape);
			}
		}
		return shapes;
	}

	void AppendVehicleDebugDraw(std::span<const VehicleWheelDebugShape> shapes, DebugDrawList& list)
	{
		for (const VehicleWheelDebugShape& shape : shapes)
		{
			const glm::vec4 color = GetVehicleDebugColor(shape.State);
			// The circle in the wheel's plane: Up at angle 0, then towards its forward (Up x Axle, the wheel's local -Z).
			const glm::vec3 forward = glm::cross(shape.Up, shape.Axle);
			const auto rim = [&shape, &forward](uint32_t index)
			{
				const SinCosResult<float> turn = DetMath::SinCos(2.0f * std::numbers::pi_v<float> * static_cast<float>(index) / static_cast<float>(VehicleDebugCircleSegments));
				return shape.Center + shape.Radius * (turn.Cos * shape.Up + turn.Sin * forward);
			};
			for (uint32_t index = 0; index < VehicleDebugCircleSegments; ++index)
				list.AddLine(rim(index), rim(index + 1), color, 0.0f, DebugDepthMode::OnTop);
			list.AddLine(shape.Center, shape.Center + shape.Up * shape.Radius, color, 0.0f, DebugDepthMode::OnTop);
			list.AddLine(shape.Attachment, shape.SuspensionEnd, color, 0.0f, DebugDepthMode::OnTop);
			if (shape.Touching)
				list.AddLine(shape.ContactPoint, shape.ContactPoint + shape.ContactNormal * VehicleDebugNormalLength, color, 0.0f, DebugDepthMode::OnTop);
		}
	}

}
