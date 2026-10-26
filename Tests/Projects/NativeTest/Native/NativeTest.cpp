#include "Native/NativeTest.h"

constexpr float DegreesToRadians = 0.017453292f;
constexpr float FullTurnDegrees = 360.0f;

// While Spin is held, advances every Spinner's angle and writes it into its entity's rotation.
static void Spin(Engine::INativeStepContext& context)
{
	if (!context.IsActionDown("Spin"))
		return;
	Totals* totals = context.GetResource<Totals>();
	if (totals != nullptr)
		totals->SpinTicks += 1;
	const float deltaSeconds = static_cast<float>(context.GetFixedDelta());
	for (const Engine::NativeEntity entity : context.Query<Spinner>())
	{
		Spinner* spinner = context.Get<Spinner>(entity);
		Engine::NativeTransform transform{};
		if (spinner == nullptr || !context.GetTransform(entity, transform))
			continue;
		float angle = spinner->AngleDegrees + spinner->DegreesPerSecond * deltaSeconds;
		while (angle >= FullTurnDegrees)
			angle -= FullTurnDegrees;
		while (angle < 0.0f)
			angle += FullTurnDegrees;
		spinner->AngleDegrees = angle;
		spinner->SpinTicks += 1;
		transform.Rotation = context.AngleAxis(angle * DegreesToRadians, spinner->Axis);
		if (!context.SetTransform(entity, transform))
			context.Write(Engine::NativeLogLevel::Warning, "Spin could not rotate a Spinner's entity.");
	}
}

// On each press of Bump, counts the press on every Bumper and moves its entity to the height of its new level.
static void Bump(Engine::INativeStepContext& context)
{
	if (!context.WasActionPressed("Bump"))
		return;
	Totals* totals = context.GetResource<Totals>();
	if (totals != nullptr)
		totals->Presses += 1;
	for (const Engine::NativeEntity entity : context.Query<Bumper>())
	{
		Bumper* bumper = context.Get<Bumper>(entity);
		Engine::NativeTransform transform{};
		if (bumper == nullptr || !context.GetTransform(entity, transform))
			continue;
		bumper->Presses += 1;
		const uint32_t levels = bumper->Levels > 0 ? bumper->Levels : 1u;
		const uint32_t level = bumper->Presses % levels;
		transform.Translation.y = bumper->BaseHeight + static_cast<float>(level) * bumper->StepHeight;
		if (!context.SetTransform(entity, transform))
			context.Write(Engine::NativeLogLevel::Warning, "Bump could not move a Bumper's entity.");
	}
}

static void Register(Engine::INativeRegistrar& registrar)
{
	Engine::NativeTypeBuilder<Spinner> spinner = Engine::NativeComponent<Spinner>("Turns its entity about an axis while the Spin action is held.");
	spinner.Field("DegreesPerSecond", &Spinner::DegreesPerSecond, "How fast it turns while Spin is held.", { .Min = -720.0, .Max = 720.0, .Unit = "deg/s" });
	spinner.Field("Axis", &Spinner::Axis, "The local axis it turns about.");
	spinner.Field("AngleDegrees", &Spinner::AngleDegrees, "The angle it has turned, in [0, 360).", { .Min = 0.0, .Max = 360.0, .Unit = "deg" });
	spinner.Field("SpinTicks", &Spinner::SpinTicks, "The ticks it has turned.");
	registrar.Add(spinner);

	Engine::NativeTypeBuilder<Bumper> bumper = Engine::NativeComponent<Bumper>("Climbs one step per press of the Bump action.");
	bumper.Field("Presses", &Bumper::Presses, "The presses of Bump it has counted.");
	bumper.Field("Levels", &Bumper::Levels, "The presses after which it drops back to its base height.", { .Min = 1.0, .Max = 16.0 });
	bumper.Field("BaseHeight", &Bumper::BaseHeight, "Its height at level 0.", { .Min = -100.0, .Max = 100.0, .Unit = "m" });
	bumper.Field("StepHeight", &Bumper::StepHeight, "How much higher each level is.", { .Min = 0.0, .Max = 10.0, .Unit = "m" });
	registrar.Add(bumper);

	Engine::NativeTypeBuilder<Totals> totals = Engine::NativeResource<Totals>("The play session's Spin ticks and Bump presses.");
	totals.Field("SpinTicks", &Totals::SpinTicks, "The ticks Spin was held.");
	totals.Field("Presses", &Totals::Presses, "The presses of Bump.");
	registrar.Add(totals);

	registrar.AddSystem({ .Name = "Spin", .Description = "Turns every Spinner while Spin is held.", .Run = &Spin });
	registrar.AddSystem({ .Name = "Bump", .Description = "Raises every Bumper one level per press of Bump.", .Run = &Bump });
}

ENGINE_NATIVE_MODULE(Register)
