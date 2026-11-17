#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsEngineState.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string_view>
#include <utility>

// The physics world's joints (M37, Docs/Decisions/0054-m37-joints.md decision 2; PhysicsWorld.h's joints section). Every
// value is checked before it reaches Jolt (§9.1): the asserts content could reach (equal hinge or slider limits without a
// spring, hinge limits outside [-pi, pi], slider limits of the wrong sign, axes that are not unit, invalid motor settings,
// a position drive without a spring) are refused or made safe here. Jolt's frames are relative to each body's centre of
// mass (EConstraintSpace::LocalToBodyCOM); the world's are relative to the body origin, so they are converted with the
// shape's centre of mass at creation, and SetShape tells the joints when it moves.

namespace Engine {

	namespace Utils {

		// How far an axis may be from perpendicular to its normal (the cosine of the angle between them).
		constexpr float PerpendicularTolerance = 1.0e-3f;

		[[nodiscard]] static bool IsKnownJointType(PhysicsJointType type)
		{
			return type == PhysicsJointType::Hinge || type == PhysicsJointType::Slider || type == PhysicsJointType::Distance || type == PhysicsJointType::Fixed;
		}

		[[nodiscard]] static bool IsKnownMotor(PhysicsJointMotor motor)
		{
			return motor == PhysicsJointMotor::Off || motor == PhysicsJointMotor::Velocity || motor == PhysicsJointMotor::Position;
		}

		[[nodiscard]] static bool HasAxes(PhysicsJointType type)
		{
			return type != PhysicsJointType::Distance;
		}

		// The live body `body` names in `system`, or null (the invalid handle, a destroyed body, another world's).
		[[nodiscard]] static JPH::Body* FindLiveBody(const JPH::PhysicsSystem& system, BodyHandle body)
		{
			const std::optional<JPH::BodyID> id = Detail::ToJoltBodyID(body);
			if (!id.has_value())
				return nullptr;
			return system.GetBodyLockInterfaceNoLock().TryGetBody(*id);
		}

		// InvalidArgument unless `frame` is usable: its point finite and within MaxPhysicsCoordinate and, for a type with
		// axes, its axis and normal unit and perpendicular.
		[[nodiscard]] static Status CheckFrame(const PhysicsJointFrame& frame, PhysicsJointType type, std::string_view side)
		{
			if (!Detail::IsFinite(frame.Point) || !IsWithinPhysicsRange(frame.Point))
			{
				return MakeError(ErrorCode::InvalidArgument, "the {} frame's point ({}, {}, {}) is not finite or beyond {} m", side, frame.Point.x, frame.Point.y,
					frame.Point.z, MaxPhysicsCoordinate);
			}
			if (!HasAxes(type))
				return {};
			if (!Detail::IsUnitVector(frame.Axis) || !Detail::IsUnitVector(frame.Normal))
				return MakeError(ErrorCode::InvalidArgument, "the {} frame's axis and normal must be unit vectors", side);
			if (std::abs(glm::dot(frame.Axis, frame.Normal)) > PerpendicularTolerance)
				return MakeError(ErrorCode::InvalidArgument, "the {} frame's normal must be perpendicular to its axis", side);
			return {};
		}

		// A hinge's limits in Jolt's range: clamped to [-pi, 0] and [0, pi] (a limit converted from 180 degrees can round
		// above pi), or the whole turn without limits.
		[[nodiscard]] static std::pair<float, float> GetHingeLimits(const PhysicsJointSettings& settings)
		{
			if (!settings.UseLimits)
				return { -JPH::JPH_PI, JPH::JPH_PI };
			return { std::clamp(settings.LowerLimit, -JPH::JPH_PI, 0.0f), std::clamp(settings.UpperLimit, 0.0f, JPH::JPH_PI) };
		}

		// A slider's limits, or none (Jolt's -FLT_MAX and FLT_MAX).
		[[nodiscard]] static std::pair<float, float> GetSliderLimits(const PhysicsJointSettings& settings)
		{
			if (!settings.UseLimits)
				return { -FLT_MAX, FLT_MAX };
			return { settings.LowerLimit, settings.UpperLimit };
		}

		// InvalidArgument unless `settings` is valid for `type` (PhysicsJointSettings).
		[[nodiscard]] static Status CheckSettings(PhysicsJointType type, const PhysicsJointSettings& settings)
		{
			const std::array<std::pair<std::string_view, float>, 10> values = { {
				{ "LowerLimit", settings.LowerLimit },
				{ "UpperLimit", settings.UpperLimit },
				{ "LimitFrequency", settings.LimitFrequency },
				{ "LimitDamping", settings.LimitDamping },
				{ "MaxFriction", settings.MaxFriction },
				{ "MotorTargetVelocity", settings.MotorTargetVelocity },
				{ "MotorTargetPosition", settings.MotorTargetPosition },
				{ "MotorMaxForce", settings.MotorMaxForce },
				{ "MotorFrequency", settings.MotorFrequency },
				{ "MotorDamping", settings.MotorDamping },
			} };
			for (const auto& [name, value] : values)
			{
				if (!Detail::IsFinite(value))
					return MakeError(ErrorCode::InvalidArgument, "{} is not finite", name);
			}

			switch (type)
			{
				case PhysicsJointType::Hinge:
				case PhysicsJointType::Slider:
				{
					if (settings.UseLimits)
					{
						if (settings.LowerLimit > 0.0f || settings.UpperLimit < 0.0f)
							return MakeError(ErrorCode::InvalidArgument, "the limits must be LowerLimit <= 0 <= UpperLimit (got {} and {})", settings.LowerLimit, settings.UpperLimit);
						const auto [lower, upper] = type == PhysicsJointType::Hinge ? GetHingeLimits(settings) : GetSliderLimits(settings);
						if (lower == upper)
							return MakeError(ErrorCode::InvalidArgument, "equal limits ({}) hold the joint still: a fixed joint is the joint that does not move", lower);
					}
					if (settings.MaxFriction < 0.0f)
						return MakeError(ErrorCode::InvalidArgument, "MaxFriction must be >= 0 (got {})", settings.MaxFriction);
					if (!IsKnownMotor(settings.Motor))
						return MakeError(ErrorCode::InvalidArgument, "unknown motor {}", std::to_underlying(settings.Motor));
					if (std::abs(settings.MotorTargetVelocity) > MaxPhysicsJointMotorTarget || std::abs(settings.MotorTargetPosition) > MaxPhysicsJointMotorTarget)
					{
						return MakeError(ErrorCode::InvalidArgument, "the motor's targets ({} and {}) must be at most {} in magnitude", settings.MotorTargetVelocity,
							settings.MotorTargetPosition, MaxPhysicsJointMotorTarget);
					}
					if (settings.MotorMaxForce < 0.0f || settings.MotorMaxForce > MaxPhysicsJointMotorForce)
						return MakeError(ErrorCode::InvalidArgument, "MotorMaxForce must be in [0, {}] (got {})", MaxPhysicsJointMotorForce, settings.MotorMaxForce);
					if (settings.MotorFrequency < MinPhysicsJointMotorFrequency || settings.MotorFrequency > MaxPhysicsJointSpring)
					{
						return MakeError(ErrorCode::InvalidArgument, "MotorFrequency must be in [{}, {}] Hz (got {})", MinPhysicsJointMotorFrequency, MaxPhysicsJointSpring,
							settings.MotorFrequency);
					}
					if (settings.MotorDamping < 0.0f || settings.MotorDamping > MaxPhysicsJointSpring)
						return MakeError(ErrorCode::InvalidArgument, "MotorDamping must be in [0, {}] (got {})", MaxPhysicsJointSpring, settings.MotorDamping);
					return {};
				}
				case PhysicsJointType::Distance:
				{
					if (settings.LowerLimit < 0.0f || settings.LowerLimit > settings.UpperLimit || settings.UpperLimit > MaxPhysicsCoordinate)
					{
						return MakeError(ErrorCode::InvalidArgument, "the distances must be 0 <= minimum <= maximum <= {} m (got {} and {})", MaxPhysicsCoordinate,
							settings.LowerLimit, settings.UpperLimit);
					}
					if (settings.LimitFrequency < 0.0f || settings.LimitFrequency > MaxPhysicsJointSpring || settings.LimitDamping < 0.0f
						|| settings.LimitDamping > MaxPhysicsJointSpring)
					{
						return MakeError(ErrorCode::InvalidArgument, "the spring's frequency and damping must be in [0, {}] (got {} and {})", MaxPhysicsJointSpring,
							settings.LimitFrequency, settings.LimitDamping);
					}
					return {};
				}
				case PhysicsJointType::Fixed:
					return {};
			}
			return MakeError(ErrorCode::InvalidArgument, "unknown joint type {}", std::to_underlying(type));
		}

		[[nodiscard]] static JPH::EMotorState ToJoltMotorState(PhysicsJointMotor motor)
		{
			switch (motor)
			{
				case PhysicsJointMotor::Off:      return JPH::EMotorState::Off;
				case PhysicsJointMotor::Velocity: return JPH::EMotorState::Velocity;
				case PhysicsJointMotor::Position: return JPH::EMotorState::Position;
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsJointMotor {}", std::to_underlying(motor));
			return JPH::EMotorState::Off;
		}

		// The motor's settings: the position drive's spring and the force and torque limits (a linear motor reads the
		// force, an angular one the torque).
		[[nodiscard]] static JPH::MotorSettings MakeMotorSettings(const PhysicsJointSettings& settings)
		{
			JPH::MotorSettings motor;
			motor.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, settings.MotorFrequency, settings.MotorDamping);
			motor.SetForceLimit(settings.MotorMaxForce);
			motor.SetTorqueLimit(settings.MotorMaxForce);
			return motor;
		}

		// `point` (relative to the body's origin) relative to its centre of mass, Jolt's constraint space.
		[[nodiscard]] static JPH::Vec3 ToCenterOfMassSpace(const JPH::Body& body, const glm::vec3& point)
		{
			return Detail::ToJolt(point) - body.GetShape()->GetCenterOfMass();
		}

		// Applies `settings` to `constraint` in place: limits first, so a target is clamped to the new ones.
		static void ApplySettings(JPH::TwoBodyConstraint& constraint, PhysicsJointType type, const PhysicsJointSettings& settings)
		{
			switch (type)
			{
				case PhysicsJointType::Hinge:
				{
					auto& hinge = static_cast<JPH::HingeConstraint&>(constraint);
					const auto [lower, upper] = GetHingeLimits(settings);
					hinge.SetLimits(lower, upper);
					hinge.SetMaxFrictionTorque(settings.MaxFriction);
					hinge.GetMotorSettings() = MakeMotorSettings(settings);
					hinge.SetMotorState(ToJoltMotorState(settings.Motor));
					hinge.SetTargetAngularVelocity(settings.MotorTargetVelocity);
					hinge.SetTargetAngle(std::clamp(settings.MotorTargetPosition, -JPH::JPH_PI, JPH::JPH_PI));
					return;
				}
				case PhysicsJointType::Slider:
				{
					auto& slider = static_cast<JPH::SliderConstraint&>(constraint);
					const auto [lower, upper] = GetSliderLimits(settings);
					slider.SetLimits(lower, upper);
					slider.SetMaxFrictionForce(settings.MaxFriction);
					slider.GetMotorSettings() = MakeMotorSettings(settings);
					slider.SetMotorState(ToJoltMotorState(settings.Motor));
					slider.SetTargetVelocity(settings.MotorTargetVelocity);
					slider.SetTargetPosition(settings.MotorTargetPosition);
					return;
				}
				case PhysicsJointType::Distance:
				{
					auto& distance = static_cast<JPH::DistanceConstraint&>(constraint);
					distance.SetDistance(settings.LowerLimit, settings.UpperLimit);
					distance.SetLimitsSpringSettings(JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, settings.LimitFrequency, settings.LimitDamping));
					return;
				}
				case PhysicsJointType::Fixed:
					return;
			}
		}

		// Jolt's constraint for `description` between `connected` (Jolt's body 1) and `body` (its body 2), its frames relative
		// to the centres of mass, with its settings applied.
		[[nodiscard]] static JPH::Ref<JPH::TwoBodyConstraint> MakeConstraint(const PhysicsJointDescription& description, JPH::Body& connected, JPH::Body& body)
		{
			const PhysicsJointFrame& frame1 = description.ConnectedFrame;
			const PhysicsJointFrame& frame2 = description.Frame;
			const JPH::Vec3 point1 = ToCenterOfMassSpace(connected, frame1.Point);
			const JPH::Vec3 point2 = ToCenterOfMassSpace(body, frame2.Point);
			const JPH::Vec3 axis1 = Detail::ToJolt(frame1.Axis).Normalized();
			const JPH::Vec3 axis2 = Detail::ToJolt(frame2.Axis).Normalized();
			const JPH::Vec3 normal1 = Detail::ToJolt(frame1.Normal).Normalized();
			const JPH::Vec3 normal2 = Detail::ToJolt(frame2.Normal).Normalized();

			JPH::Ref<JPH::TwoBodyConstraint> constraint;
			switch (description.Type)
			{
				case PhysicsJointType::Hinge:
				{
					JPH::HingeConstraintSettings settings;
					settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = point1;
					settings.mHingeAxis1 = axis1;
					settings.mNormalAxis1 = normal1;
					settings.mPoint2 = point2;
					settings.mHingeAxis2 = axis2;
					settings.mNormalAxis2 = normal2;
					constraint = settings.Create(connected, body);
					break;
				}
				case PhysicsJointType::Slider:
				{
					JPH::SliderConstraintSettings settings;
					settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = point1;
					settings.mSliderAxis1 = axis1;
					settings.mNormalAxis1 = normal1;
					settings.mPoint2 = point2;
					settings.mSliderAxis2 = axis2;
					settings.mNormalAxis2 = normal2;
					constraint = settings.Create(connected, body);
					break;
				}
				case PhysicsJointType::Distance:
				{
					JPH::DistanceConstraintSettings settings;
					settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = point1;
					settings.mPoint2 = point2;
					settings.mMinDistance = description.Settings.LowerLimit;
					settings.mMaxDistance = description.Settings.UpperLimit;
					constraint = settings.Create(connected, body);
					break;
				}
				case PhysicsJointType::Fixed:
				{
					JPH::FixedConstraintSettings settings;
					settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
					settings.mPoint1 = point1;
					settings.mAxisX1 = axis1;
					settings.mAxisY1 = normal1;
					settings.mPoint2 = point2;
					settings.mAxisX2 = axis2;
					settings.mAxisY2 = normal2;
					constraint = settings.Create(connected, body);
					break;
				}
			}
			ApplySettings(*constraint, description.Type, description.Settings);
			constraint->SetConstraintPriority(description.Priority);
			return constraint;
		}

		// Wakes `id` unless it is the world, a static body or not live (a static body cannot wake).
		static void WakeBody(JPH::PhysicsSystem& system, const JPH::BodyID& id)
		{
			if (id.IsInvalid())
				return;
			const JPH::Body* body = system.GetBodyLockInterfaceNoLock().TryGetBody(id);
			if (body != nullptr && !body->IsStatic())
				system.GetBodyInterface().ActivateBody(id);
		}

		// Makes Jolt collide `id` afresh at the next step (BodyInterface::InvalidateContactCache) when whether a joint keeps it
		// apart from another body changed: Jolt reuses a resting pair's cached contacts (none, while a joint kept it apart; the
		// old ones, after a joint begins to) as long as the two bodies keep their relative pose.
		static void RefreshContacts(JPH::PhysicsSystem& system, const JPH::BodyID& id)
		{
			if (!id.IsInvalid() && system.GetBodyLockInterfaceNoLock().TryGetBody(id) != nullptr)
				system.GetBodyInterface().InvalidateContactCache(id);
		}

		// The length of a 3-vector or of the vector of up to three of a constraint's lambdas.
		[[nodiscard]] static float GetLength(float x, float y, float z)
		{
			return std::sqrt(x * x + y * y + z * z);
		}

	}

	void PhysicsWorld::State::DestroyJointsOf(const JPH::BodyID& body)
	{
		bool removed = false;
		for (auto joint = Joints.begin(); joint != Joints.end();)
		{
			Detail::PhysicsJointRecord& record = joint->second;
			if (record.Body != body && record.ConnectedBody != body)
			{
				++joint;
				continue;
			}
			System->RemoveConstraint(record.Constraint.GetPtr());
			const JPH::BodyID other = record.Body == body ? record.ConnectedBody : record.Body;
			if (!record.Settings.CollideConnected && !record.ConnectedBody.IsInvalid())
			{
				JointedPairs.Remove(record.Body, record.ConnectedBody);
				Utils::RefreshContacts(*System, other);
			}
			Utils::WakeBody(*System, other);
			joint = Joints.erase(joint);
			removed = true;
		}
		if (removed)
			System->SetSimShapeFilter(JointedPairs.IsEmpty() ? nullptr : &JointedPairFilter);
	}

	void PhysicsWorld::State::NotifyJointsShapeChanged(const JPH::BodyID& body, JPH::Vec3Arg deltaCenterOfMass)
	{
		for (auto& [handle, record] : Joints)
		{
			if (record.Body == body || record.ConnectedBody == body)
				record.Constraint->NotifyShapeChanged(body, deltaCenterOfMass);
		}
	}

	void PhysicsWorld::State::DestroyAllJoints()
	{
		for (auto& [handle, record] : Joints)
			System->RemoveConstraint(record.Constraint.GetPtr());
		Joints.clear();
		JointedPairs = Detail::PhysicsJointedPairs{};
		System->SetSimShapeFilter(nullptr);
	}

	Result<JointHandle> PhysicsWorld::CreateJoint(const PhysicsJointDescription& description)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsWorld::CreateJoint called off the main thread");
		if (!Utils::IsKnownJointType(description.Type))
			return MakeError(ErrorCode::InvalidArgument, "unknown joint type {}", std::to_underlying(description.Type));
		JPH::Body* body = Utils::FindLiveBody(*state.System, description.Body);
		if (body == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the joint's body 0x{:08x} names no live body of this world", description.Body.GetValue());
		JPH::Body* connected = &JPH::Body::sFixedToWorld;
		if (description.ConnectedBody.IsValid())
		{
			connected = Utils::FindLiveBody(*state.System, description.ConnectedBody);
			if (connected == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "the connected body 0x{:08x} names no live body of this world", description.ConnectedBody.GetValue());
			if (connected == body)
				return MakeError(ErrorCode::InvalidArgument, "a joint joins two bodies, and both of its ends are 0x{:08x}", description.Body.GetValue());
		}
		ENGINE_TRY(Utils::CheckFrame(description.Frame, description.Type, "joint's"));
		ENGINE_TRY(Utils::CheckFrame(description.ConnectedFrame, description.Type, "connected"));
		ENGINE_TRY(Utils::CheckSettings(description.Type, description.Settings));

		Detail::PhysicsJointRecord record;
		record.Constraint = Utils::MakeConstraint(description, *connected, *body);
		record.Type = description.Type;
		record.Body = body->GetID();
		record.ConnectedBody = description.ConnectedBody.IsValid() ? connected->GetID() : JPH::BodyID();
		record.Settings = description.Settings;
		state.System->AddConstraint(record.Constraint.GetPtr());
		if (!record.Settings.CollideConnected && !record.ConnectedBody.IsInvalid())
		{
			state.JointedPairs.Add(record.Body, record.ConnectedBody);
			state.System->SetSimShapeFilter(&state.JointedPairFilter);
			Utils::RefreshContacts(*state.System, record.Body);
		}
		Utils::WakeBody(*state.System, record.Body);
		Utils::WakeBody(*state.System, record.ConnectedBody);

		const JointHandle handle(state.NextJoint++);
		state.Joints.emplace(handle.GetValue(), std::move(record));
		return handle;
	}

	void PhysicsWorld::DestroyJoint(JointHandle joint)
	{
		State& state = *m_State;
		const auto found = state.Joints.find(joint.GetValue());
		if (found == state.Joints.end())
			return;
		Detail::PhysicsJointRecord& record = found->second;
		state.System->RemoveConstraint(record.Constraint.GetPtr());
		if (!record.Settings.CollideConnected && !record.ConnectedBody.IsInvalid())
		{
			state.JointedPairs.Remove(record.Body, record.ConnectedBody);
			Utils::RefreshContacts(*state.System, record.Body);
		}
		Utils::WakeBody(*state.System, record.Body);
		Utils::WakeBody(*state.System, record.ConnectedBody);
		state.Joints.erase(found);
		state.System->SetSimShapeFilter(state.JointedPairs.IsEmpty() ? nullptr : &state.JointedPairFilter);
	}

	bool PhysicsWorld::IsJointValid(JointHandle joint) const
	{
		return m_State->Joints.contains(joint.GetValue());
	}

	Status PhysicsWorld::SetJointSettings(JointHandle joint, const PhysicsJointSettings& settings)
	{
		State& state = *m_State;
		const auto found = state.Joints.find(joint.GetValue());
		if (found == state.Joints.end())
			return MakeError(ErrorCode::InvalidArgument, "joint {} names no live joint of this world", joint.GetValue());
		Detail::PhysicsJointRecord& record = found->second;
		ENGINE_TRY(Utils::CheckSettings(record.Type, settings));

		if (!record.ConnectedBody.IsInvalid() && settings.CollideConnected != record.Settings.CollideConnected)
		{
			if (settings.CollideConnected)
				state.JointedPairs.Remove(record.Body, record.ConnectedBody);
			else
				state.JointedPairs.Add(record.Body, record.ConnectedBody);
			state.System->SetSimShapeFilter(state.JointedPairs.IsEmpty() ? nullptr : &state.JointedPairFilter);
			Utils::RefreshContacts(*state.System, record.Body);
		}
		Utils::ApplySettings(*record.Constraint, record.Type, settings);
		record.Settings = settings;
		Utils::WakeBody(*state.System, record.Body);
		Utils::WakeBody(*state.System, record.ConnectedBody);
		return {};
	}

	void PhysicsWorld::SetJointPriority(JointHandle joint, uint32_t priority)
	{
		if (const auto found = m_State->Joints.find(joint.GetValue()); found != m_State->Joints.end())
			found->second.Constraint->SetConstraintPriority(priority);
	}

	PhysicsJointState PhysicsWorld::GetJointState(JointHandle joint) const
	{
		const State& state = *m_State;
		const auto found = state.Joints.find(joint.GetValue());
		if (found == state.Joints.end())
			return {};
		const Detail::PhysicsJointRecord& record = found->second;
		const JPH::TwoBodyConstraint& constraint = *record.Constraint;
		float linear = 0.0f;
		float angular = 0.0f;
		PhysicsJointState reading;
		// The frames where the bodies hold them now (Jolt's constraint-to-body matrices are relative to the centres of mass).
		const JPH::Mat44 connectedFrame = constraint.GetBody1()->GetCenterOfMassTransform() * constraint.GetConstraintToBody1Matrix();
		const JPH::Mat44 bodyFrame = constraint.GetBody2()->GetCenterOfMassTransform() * constraint.GetConstraintToBody2Matrix();
		reading.Point = Detail::ToGlm(bodyFrame.GetTranslation());
		reading.ConnectedPoint = Detail::ToGlm(connectedFrame.GetTranslation());
		if (record.Type == PhysicsJointType::Hinge || record.Type == PhysicsJointType::Slider)
			reading.Axis = Detail::ToGlm(connectedFrame.GetAxisX().Normalized());
		switch (record.Type)
		{
			case PhysicsJointType::Hinge:
			{
				const auto& hinge = static_cast<const JPH::HingeConstraint&>(constraint);
				reading.Value = hinge.GetCurrentAngle();
				linear = hinge.GetTotalLambdaPosition().Length();
				const JPH::Vector<2> rotation = hinge.GetTotalLambdaRotation();
				angular = Utils::GetLength(rotation[0], rotation[1], hinge.GetTotalLambdaRotationLimits());
				break;
			}
			case PhysicsJointType::Slider:
			{
				const auto& slider = static_cast<const JPH::SliderConstraint&>(constraint);
				reading.Value = slider.GetCurrentPosition();
				const JPH::Vector<2> position = slider.GetTotalLambdaPosition();
				linear = Utils::GetLength(position[0], position[1], slider.GetTotalLambdaPositionLimits());
				angular = slider.GetTotalLambdaRotation().Length();
				break;
			}
			case PhysicsJointType::Distance:
			{
				const auto& distance = static_cast<const JPH::DistanceConstraint&>(constraint);
				reading.Value = (bodyFrame.GetTranslation() - connectedFrame.GetTranslation()).Length();
				linear = std::abs(distance.GetTotalLambdaPosition());
				break;
			}
			case PhysicsJointType::Fixed:
			{
				const auto& fixed = static_cast<const JPH::FixedConstraint&>(constraint);
				linear = fixed.GetTotalLambdaPosition().Length();
				angular = fixed.GetTotalLambdaRotation().Length();
				break;
			}
		}
		if (state.CollisionStepDelta > 0.0f)
		{
			reading.Force = linear / state.CollisionStepDelta;
			reading.Torque = angular / state.CollisionStepDelta;
		}
		return reading;
	}

	uint32_t PhysicsWorld::GetJointCount() const
	{
		return static_cast<uint32_t>(m_State->Joints.size());
	}

}
