--!strict
-- The world pause (M35, Docs/Decisions/0050-m35-pause-entity-state.md decisions 1 to 3 and 6; World Tree's brief 12) in
-- WorldPause.scene (WorldPause.jsonl, IdSeed 3201), in every run mode: a paused world holds its body in flight, its
-- particles, its skeletal and sprite animators, the world behaviour's callbacks and tasks and its clock, while the menu,
-- whose class sets RunsWhileWorldPaused, runs and takes pointer and navigation input; the resume lets everything move on;
-- the sounds choice holds or keeps the world's voices, and the Ui group's play on. Each case starts from a fresh load of
-- the scene (Isolation "Case").
local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `{name} is in the scene`)
	return entity
end

local function velocity(entity: Entity): vector
	local body = entity.RigidBody
	assert(body, `{entity.Name} has a RigidBody`)
	return body:GetLinearVelocity()
end

local function layerTime(entity: Entity): number
	local animator = entity.Animator
	assert(animator, `{entity.Name} has an Animator`)
	return animator.Layers[1].Time
end

local function spriteStep(entity: Entity): number
	local animator = entity.SpriteAnimator
	assert(animator, `{entity.Name} has a SpriteAnimator`)
	return animator.Step * 1000 + animator.StepTicks
end

local function particles(entity: Entity): number
	local emitter = entity.ParticleEmitter
	assert(emitter, `{entity.Name} has a ParticleEmitter`)
	return emitter:GetParticleCount()
end

local function source(name: string): AudioSource
	local component = find(name).AudioSource
	assert(component, `{name} has an AudioSource`)
	return component
end

-- The centre of a laid-out element, in view coordinates (the session's view is the scene's resolution here).
local function centre(element: Entity): vector
	local rect = Ui.GetRect(element)
	assert(rect, `{element.Name} is laid out`)
	return (rect.Min + rect.Max) * 0.5
end

return Test.Suite("WorldPause", function()
	Test.Case("a paused world holds its body, particles, animators and clock, and moves on from where it held after the resume", function()
		Test.WaitTicks(8)
		local shard = find("Shard")
		local sparks = find("Sparks")
		local armature = find("Armature")
		local strider = find("Strider")
		Test.Expect(velocity(shard).y > 0.5, "the shard is in flight, rising")
		Test.ExpectEqual(Time.IsWorldPaused(), false)
		Time.PauseWorld()
		Test.ExpectEqual(Time.IsWorldPaused(), true, "the request is the next step's state")
		-- The pause takes effect at the next fixed step; this case resumes inside that paused tick.
		Test.WaitTicks(1)
		local position = shard.Transform.Translation
		local moving = velocity(shard)
		local digest = Test.GetParticleDigest(sparks)
		local count = particles(sparks)
		local poseTime = layerTime(armature)
		local step = spriteStep(strider)
		local worldTick = Time.GetTick()
		local worldTime = Time.GetTime()
		local sessionTick = Time.GetSessionTick()
		local sessionTime = Time.GetSessionTime()
		Test.WaitTicks(60)
		Test.ExpectEqual(shard.Transform.Translation, position, "the body holds where it hung")
		Test.ExpectEqual(velocity(shard), moving, "and keeps its velocity")
		Test.ExpectEqual(Test.GetParticleDigest(sparks), digest, "the particles hold")
		Test.ExpectEqual(particles(sparks), count)
		Test.ExpectEqual(layerTime(armature), poseTime, "the Animator holds")
		Test.ExpectEqual(spriteStep(strider), step, "the SpriteAnimator holds")
		Test.ExpectEqual(Time.GetTick(), worldTick, "the world's clock holds")
		Test.ExpectEqual(Time.GetTime(), worldTime)
		Test.ExpectEqual(Time.GetSessionTick(), sessionTick + 60, "the session's clock counts on")
		Test.ExpectNear(Time.GetSessionTime(), sessionTime + 60 * Time.GetFixedDeltaTime(), 1e-9)
		Time.ResumeWorld()
		Test.ExpectEqual(Time.IsWorldPaused(), false)
		Test.WaitTicks(2)
		Test.Expect(shard.Transform.Translation ~= position, "the body moves on after the resume")
		Test.Expect(Test.GetParticleDigest(sparks) ~= digest, "and so do the particles")
		Test.Expect(layerTime(armature) ~= poseTime, "and the Animator")
		Test.ExpectEqual(Time.GetTick(), worldTick + 1, "the world ran the tick after the resume")
	end)

	Test.Case("only behaviours that run while the world is paused update, and the world's tasks wait for the resume", function()
		local world = find("World"):GetScript("WorldCounter")
		local menu = find("Menu"):GetScript("PauseMenu")
		-- Both behaviours began a half-second delay (32 ticks at 64 Hz) in their OnStart, before tick 0.
		Test.WaitTicks(4)
		Time.PauseWorld()
		Test.WaitTicks(1)
		local worldFixed, worldUpdates, worldLate, worldTasks = world.Fixed, world.Updates, world.LateUpdates, world.TaskRuns
		local menuFixed, menuUpdates, menuLate, menuTasks = menu.Fixed, menu.Updates, menu.LateUpdates, menu.TaskRuns
		Test.WaitTicks(40)
		Test.ExpectEqual(world.Fixed, worldFixed, "the world behaviour's OnFixedUpdate holds")
		Test.ExpectEqual(world.Updates, worldUpdates, "its OnUpdate")
		Test.ExpectEqual(world.LateUpdates, worldLate, "its OnLateUpdate")
		Test.ExpectEqual(world.TaskRuns, worldTasks, "its task")
		Test.ExpectEqual(world.DelayEndedAt, -1, "its delay waits")
		Test.ExpectEqual(menu.Fixed, menuFixed + 40, "the menu's OnFixedUpdate runs every tick")
		Test.ExpectEqual(menu.Updates, menuUpdates + 40, "its OnUpdate every frame")
		Test.ExpectEqual(menu.LateUpdates, menuLate + 40, "its OnLateUpdate")
		Test.ExpectEqual(menu.TaskRuns, menuTasks + 40, "its task")
		Test.ExpectEqual(menu.DelayEndedAt, 32, "its delay counts the session's ticks and ended during the pause")
		Time.ResumeWorld()
		Test.WaitTicks(40)
		local held = Time.GetSessionTick() - Time.GetTick()
		Test.Expect(held >= 40, "the world held for the paused ticks")
		Test.ExpectEqual(world.DelayEndedAt, 32 + held, "the world's delay counted only the world's ticks")
		Test.Expect(world.Fixed > worldFixed and world.TaskRuns > worldTasks, "the world behaviour runs again")
	end)

	Test.Case("the menu takes pointer and navigation input while the world is paused, and its Resume button resumes it", function()
		local menu = find("Menu"):GetScript("PauseMenu")
		local resume = find("Resume")
		local options = find("Options")
		Test.InjectKey("P", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(Time.IsWorldPaused(), true, "P paused the world through the menu")
		Test.Expect(menu.PausedAt >= 0)
		local worldTick = Time.GetTick()
		-- Navigation: down from the canvas's default focus, Resume, to Options.
		Test.InjectKey("Down", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(Ui.GetFocus(), resume, "the first navigation focuses the canvas's DefaultFocus")
		Test.InjectKey("Down", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(Ui.GetFocus(), options, "and the next moves on")
		Test.ExpectEqual(menu.Focused.Options, 1)
		-- The pointer clicks Options.
		Test.InjectMouseMove(centre(options))
		Test.InjectMouse("Left", "Tap", centre(options))
		Test.WaitTicks(2)
		Test.ExpectEqual(menu.Clicks.Options, 1, "a click reaches the menu while the world is paused")
		Test.ExpectEqual(Time.IsWorldPaused(), true)
		Test.ExpectEqual(Time.GetTick(), worldTick, "the world held throughout")
		-- Submit on Resume resumes the world from the menu's callback.
		Test.InjectKey("Up", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(Ui.GetFocus(), resume)
		Test.InjectKey("Enter", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(menu.Clicks.Resume, 1)
		Test.ExpectEqual(Time.IsWorldPaused(), false, "the menu resumed the world")
		Test.Expect(Time.GetTick() > worldTick, "and the world runs again")
	end)

	Test.Case("Pause holds the world's sounds, Finish lets them play on, and the Ui group plays either way", function()
		local hum = source("Hum")
		local chime = source("Chime")
		Test.ExpectEqual(hum.Group, "Sfx")
		Test.ExpectEqual(chime.Group, "Ui")
		chime.Volume = 0
		Test.Expect(Test.CaptureAudio(2).Peak > 0, "the world's looping hum plays")
		Time.PauseWorld("Pause")
		Test.WaitTicks(1)
		Test.ExpectEqual(Test.CaptureAudio(2).Peak, 0, "Pause holds it")
		chime.Volume = 1
		Test.Expect(Test.CaptureAudio(2).Peak > 0, "the Ui group's chime plays on")
		chime.Volume = 0
		Time.PauseWorld("Finish")
		Test.WaitTicks(1)
		Test.Expect(Test.CaptureAudio(2).Peak > 0, "Finish lets the world's hum play on")
		Time.ResumeWorld()
		Test.WaitTicks(1)
		Test.Expect(Test.CaptureAudio(2).Peak > 0, "and it plays after the resume")
		Test.ExpectEqual(Time.IsWorldPaused(), false)
	end)
end)
