#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Physics/PhysicsJoint.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <string_view>

// The Joints category (M37, Docs/Decisions/0054-m37-joints.md decision 1; ADR 0052's phase-6 ledger: a sixteenth category
// after Navigation, in landing order): HingeJoint, SliderJoint, DistanceJoint and FixedJoint, each joining the body of the
// RigidBody at or above its entity to another body or the world (Architecture §5.3, §9.10). One component per type, so each
// has only the fields it uses; each excludes the other three, so an entity holds at most one joint. Field bounds keep every
// value Jolt receives valid (§9.1): angles within a turn, non-negative frictions, forces and distances, a motor's
// frequency above 0, and springs and targets bounded so the solver's constants stay finite (the world's bounds,
// Physics/PhysicsJoint.h). What spans fields or entities (a zero Axis, equal limits, a MinDistance above MaxDistance, ends
// that make no body) is PHYSICS_JOINT_INVALID's (Scene/PhysicsJoints.h), never a write error, so a script may set the
// fields of a joint in any order.

namespace Engine {

	namespace Utils {

		// The farthest anchor and slider limit, the longest distance (MaxPhysicsCoordinate), the fastest motor target and the
		// strongest motor, and the spring bounds, as registry bounds.
		constexpr double MaxJointCoordinate = static_cast<double>(MaxPhysicsCoordinate);
		constexpr double MaxJointMotorTarget = static_cast<double>(MaxPhysicsJointMotorTarget);
		constexpr double MaxJointMotorForce = static_cast<double>(MaxPhysicsJointMotorForce);
		constexpr double MinJointMotorFrequency = static_cast<double>(MinPhysicsJointMotorFrequency);
		constexpr double MaxJointSpring = static_cast<double>(MaxPhysicsJointSpring);

		// The descriptions the four components share.
		constexpr std::string_view ConnectedBodyDescription =
			"The entity whose body this one is joined to: the body of the RigidBody at or above it, else the static body its own colliders "
			"form; null joins the body to the world.";
		constexpr std::string_view AnchorDescription = "The joint's point in this entity's space, in metres (its scale applies).";
		constexpr std::string_view CollideConnectedDescription =
			"Whether the two joined bodies still collide with each other; false keeps them apart, with no contact or collision event "
			"between them.";
		constexpr std::string_view BreakForceDescription =
			"The force, in newtons, beyond which the joint breaks, measured over the last collision step after each physics step; 0 never "
			"breaks.";
		constexpr std::string_view BreakTorqueDescription =
			"The torque, in newton metres, beyond which the joint breaks, measured as BreakForce is; 0 never breaks.";
		constexpr std::string_view BrokenDescription =
			"Whether the joint is broken: the physics sets it when the joint breaks (OnJointBreak), and a broken joint joins nothing until "
			"Broken is set false again, which mends it where it was first made.";
		constexpr std::string_view MotorDescription = "What the motor does: Off, drive to a target velocity, or drive to a target through a spring.";
		constexpr std::string_view MotorFrequencyDescription =
			"The frequency of the spring a Position motor drives with, in hertz: higher is stiffer (2 Hz is Jolt's default).";
		constexpr std::string_view MotorDampingDescription =
			"The damping ratio of the Position motor's spring: 0 oscillates, 1 is critically damped.";

	}

	void RegisterJointsComponents(TypeRegistry& registry)
	{
		registry.Enum<JointMotorMode>("JointMotorMode", "What a HingeJoint's or SliderJoint's motor does.")
			.Entry(JointMotorMode::Off, "Off", "No drive: the joint moves freely, against its MaxFrictionTorque or MaxFrictionForce.")
			.Entry(JointMotorMode::Velocity, "Velocity", "Drives the joint at MotorTargetVelocity with at most MotorMaxTorque or MotorMaxForce.")
			.Entry(JointMotorMode::Position, "Position",
				"Drives the joint to MotorTargetAngle or MotorTargetPosition through a spring of MotorFrequency and MotorDamping, with at most "
				"MotorMaxTorque or MotorMaxForce.");

		registry.Enum<PhysicsJointType>("JointType", "Which joint component a joint is (physics.jointInfo's type).")
			.Entry(PhysicsJointType::Hinge, "Hinge", "A HingeJoint: the body turns about an axis.")
			.Entry(PhysicsJointType::Slider, "Slider", "A SliderJoint: the body moves along an axis.")
			.Entry(PhysicsJointType::Distance, "Distance", "A DistanceJoint: the distance between two points stays within limits.")
			.Entry(PhysicsJointType::Fixed, "Fixed", "A FixedJoint: the two bodies move as one.");

		RegisterComponent<HingeJointComponent>(registry, "HingeJoint",
			"Joins the body of the RigidBody at or above the entity to another body or the world so it only turns about an axis, like a door "
			"or a trap door, with optional limits, motor and breaking.")
			.Category("Joints")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<SliderJointComponent>()
			.Excludes<DistanceJointComponent>()
			.Excludes<FixedJointComponent>()
			.Field("ConnectedBody", &HingeJointComponent::ConnectedBody, Utils::ConnectedBodyDescription)
			.Field("Anchor", &HingeJointComponent::Anchor, "The hinge's pivot in this entity's space, in metres (its scale applies).",
				{ .Min = -Utils::MaxJointCoordinate, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("Axis", &HingeJointComponent::Axis,
				"The axis the body turns about, in this entity's space; positive angles turn it by the right-hand rule. Normalized when used; a "
				"zero axis makes the joint invalid.")
			.Field("UseLimits", &HingeJointComponent::UseLimits, "Whether the angle stays between LowerAngle and UpperAngle.")
			.Field("LowerAngle", &HingeJointComponent::LowerAngle, "The smallest angle, in degrees from the authored pose, while UseLimits holds.",
				{ .Min = -180.0, .Max = 0.0, .Unit = "deg" })
			.Field("UpperAngle", &HingeJointComponent::UpperAngle,
				"The largest angle, in degrees from the authored pose, while UseLimits holds; equal limits make the joint invalid (use a "
				"FixedJoint).",
				{ .Min = 0.0, .Max = 180.0, .Unit = "deg" })
			.Field("MaxFrictionTorque", &HingeJointComponent::MaxFrictionTorque,
				"The friction torque, in newton metres, that resists turning while the motor is Off.", { .Min = 0.0, .Unit = "N m" })
			.Field("Motor", &HingeJointComponent::Motor, Utils::MotorDescription)
			.Field("MotorTargetVelocity", &HingeJointComponent::MotorTargetVelocity, "The angular velocity a Velocity motor drives to, in degrees per second.",
				{ .Min = -Utils::MaxJointMotorTarget, .Max = Utils::MaxJointMotorTarget, .Unit = "deg/s" })
			.Field("MotorTargetAngle", &HingeJointComponent::MotorTargetAngle,
				"The angle a Position motor drives to, in degrees from the authored pose; clamped to the limits while UseLimits holds.",
				{ .Min = -180.0, .Max = 180.0, .Unit = "deg" })
			.Field("MotorMaxTorque", &HingeJointComponent::MotorMaxTorque, "The most torque the motor applies, in newton metres.",
				{ .Min = 0.0, .Max = Utils::MaxJointMotorForce, .Unit = "N m" })
			.Field("MotorFrequency", &HingeJointComponent::MotorFrequency, Utils::MotorFrequencyDescription,
				{ .Min = Utils::MinJointMotorFrequency, .Max = Utils::MaxJointSpring, .Unit = "Hz" })
			.Field("MotorDamping", &HingeJointComponent::MotorDamping, Utils::MotorDampingDescription, { .Min = 0.0, .Max = Utils::MaxJointSpring })
			.Field("CollideConnected", &HingeJointComponent::CollideConnected, Utils::CollideConnectedDescription)
			.Field("BreakForce", &HingeJointComponent::BreakForce, Utils::BreakForceDescription, { .Min = 0.0, .Unit = "N" })
			.Field("BreakTorque", &HingeJointComponent::BreakTorque, Utils::BreakTorqueDescription, { .Min = 0.0, .Unit = "N m" })
			.Field("Broken", &HingeJointComponent::Broken, Utils::BrokenDescription);

		RegisterComponent<SliderJointComponent>(registry, "SliderJoint",
			"Joins the body of the RigidBody at or above the entity to another body or the world so it only moves along an axis without "
			"turning, like a piston or a rail, with optional limits, motor and breaking.")
			.Category("Joints")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<HingeJointComponent>()
			.Excludes<DistanceJointComponent>()
			.Excludes<FixedJointComponent>()
			.Field("ConnectedBody", &SliderJointComponent::ConnectedBody, Utils::ConnectedBodyDescription)
			.Field("Anchor", &SliderJointComponent::Anchor, Utils::AnchorDescription,
				{ .Min = -Utils::MaxJointCoordinate, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("Axis", &SliderJointComponent::Axis,
				"The axis the body moves along, in this entity's space; positive positions move it along the axis. Normalized when used; a "
				"zero axis makes the joint invalid.")
			.Field("UseLimits", &SliderJointComponent::UseLimits, "Whether the position stays between LowerLimit and UpperLimit.")
			.Field("LowerLimit", &SliderJointComponent::LowerLimit, "The smallest position, in metres from the authored pose, while UseLimits holds.",
				{ .Min = -Utils::MaxJointCoordinate, .Max = 0.0, .Unit = "m" })
			.Field("UpperLimit", &SliderJointComponent::UpperLimit,
				"The largest position, in metres from the authored pose, while UseLimits holds; equal limits make the joint invalid (use a "
				"FixedJoint).",
				{ .Min = 0.0, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("MaxFrictionForce", &SliderJointComponent::MaxFrictionForce, "The friction force, in newtons, that resists sliding while the motor is Off.",
				{ .Min = 0.0, .Unit = "N" })
			.Field("Motor", &SliderJointComponent::Motor, Utils::MotorDescription)
			.Field("MotorTargetVelocity", &SliderJointComponent::MotorTargetVelocity, "The velocity a Velocity motor drives to, in metres per second.",
				{ .Min = -Utils::MaxJointMotorTarget, .Max = Utils::MaxJointMotorTarget, .Unit = "m/s" })
			.Field("MotorTargetPosition", &SliderJointComponent::MotorTargetPosition,
				"The position a Position motor drives to, in metres from the authored pose; clamped to the limits while UseLimits holds.",
				{ .Min = -Utils::MaxJointMotorTarget, .Max = Utils::MaxJointMotorTarget, .Unit = "m" })
			.Field("MotorMaxForce", &SliderJointComponent::MotorMaxForce, "The most force the motor applies, in newtons.",
				{ .Min = 0.0, .Max = Utils::MaxJointMotorForce, .Unit = "N" })
			.Field("MotorFrequency", &SliderJointComponent::MotorFrequency, Utils::MotorFrequencyDescription,
				{ .Min = Utils::MinJointMotorFrequency, .Max = Utils::MaxJointSpring, .Unit = "Hz" })
			.Field("MotorDamping", &SliderJointComponent::MotorDamping, Utils::MotorDampingDescription, { .Min = 0.0, .Max = Utils::MaxJointSpring })
			.Field("CollideConnected", &SliderJointComponent::CollideConnected, Utils::CollideConnectedDescription)
			.Field("BreakForce", &SliderJointComponent::BreakForce, Utils::BreakForceDescription, { .Min = 0.0, .Unit = "N" })
			.Field("BreakTorque", &SliderJointComponent::BreakTorque, Utils::BreakTorqueDescription, { .Min = 0.0, .Unit = "N m" })
			.Field("Broken", &SliderJointComponent::Broken, Utils::BrokenDescription);

		RegisterComponent<DistanceJointComponent>(registry, "DistanceJoint",
			"Keeps a point of the body of the RigidBody at or above the entity between a minimum and a maximum distance from a point of "
			"another body or of the world, like a rope, a chain link or a rod, with an optional spring and breaking.")
			.Category("Joints")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<HingeJointComponent>()
			.Excludes<SliderJointComponent>()
			.Excludes<FixedJointComponent>()
			.Field("ConnectedBody", &DistanceJointComponent::ConnectedBody, Utils::ConnectedBodyDescription)
			.Field("Anchor", &DistanceJointComponent::Anchor, Utils::AnchorDescription,
				{ .Min = -Utils::MaxJointCoordinate, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("ConnectedAnchor", &DistanceJointComponent::ConnectedAnchor,
				"The other end's point in ConnectedBody's space, in metres (its scale applies); in world space when ConnectedBody is null.",
				{ .Min = -Utils::MaxJointCoordinate, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("MinDistance", &DistanceJointComponent::MinDistance,
				"The shortest the distance between the anchors gets, in metres; above MaxDistance the joint is invalid.",
				{ .Min = 0.0, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("MaxDistance", &DistanceJointComponent::MaxDistance, "The longest the distance between the anchors gets, in metres.",
				{ .Min = 0.0, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("Frequency", &DistanceJointComponent::Frequency,
				"The frequency of the spring that softens the limits, in hertz: 0 is rigid, low values stretch like an elastic rope.",
				{ .Min = 0.0, .Max = Utils::MaxJointSpring, .Unit = "Hz" })
			.Field("Damping", &DistanceJointComponent::Damping, "The damping ratio of the limits' spring: 0 oscillates, 1 is critically damped.",
				{ .Min = 0.0, .Max = Utils::MaxJointSpring })
			.Field("CollideConnected", &DistanceJointComponent::CollideConnected, Utils::CollideConnectedDescription)
			.Field("BreakForce", &DistanceJointComponent::BreakForce, Utils::BreakForceDescription, { .Min = 0.0, .Unit = "N" })
			.Field("Broken", &DistanceJointComponent::Broken, Utils::BrokenDescription);

		RegisterComponent<FixedJointComponent>(registry, "FixedJoint",
			"Welds the body of the RigidBody at or above the entity to another body or the world in the pose they have when it is first made, "
			"a weld that may break.")
			.Category("Joints")
			.Version(1)
			.Requires<TransformComponent>()
			.Excludes<HingeJointComponent>()
			.Excludes<SliderJointComponent>()
			.Excludes<DistanceJointComponent>()
			.Field("ConnectedBody", &FixedJointComponent::ConnectedBody, Utils::ConnectedBodyDescription)
			.Field("Anchor", &FixedJointComponent::Anchor, "The weld's point in this entity's space, in metres (its scale applies).",
				{ .Min = -Utils::MaxJointCoordinate, .Max = Utils::MaxJointCoordinate, .Unit = "m" })
			.Field("CollideConnected", &FixedJointComponent::CollideConnected, Utils::CollideConnectedDescription)
			.Field("BreakForce", &FixedJointComponent::BreakForce, Utils::BreakForceDescription, { .Min = 0.0, .Unit = "N" })
			.Field("BreakTorque", &FixedJointComponent::BreakTorque, Utils::BreakTorqueDescription, { .Min = 0.0, .Unit = "N m" })
			.Field("Broken", &FixedJointComponent::Broken, Utils::BrokenDescription);
	}

}
