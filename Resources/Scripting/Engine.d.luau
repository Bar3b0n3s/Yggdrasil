-- Generated from ScriptApiRegistry and TypeRegistry.
-- How a material's base colour alpha is used (the glTF 2.0 alpha modes).
export type AlphaMode = "Opaque" | "Mask" | "Blend"

-- What asset.create makes.
export type AssetCreateType = "Material" | "Scene" | "Prefab" | "SoundEffect" | "Folder" | "SpriteSheet" | "Tilemap" | "StringTable"

-- Where an asset is in its life (§7.2).
export type AssetState = "Unloaded" | "Loading" | "Loaded" | "Failed"

-- The kind of a loadable asset (§7.1).
export type AssetType = "None" | "Scene" | "Prefab" | "Mesh" | "Material" | "Texture" | "Environment" | "AudioClip" | "Script" | "Font" | "Replay" | "SpriteSheet" | "Tilemap" | "StringTable"

-- How a spatial sound's volume falls off with distance.
export type Attenuation = "None" | "Inverse" | "Linear" | "Exponential"

-- The state of the audio engine's playback device.
export type AudioDeviceState = "None" | "Running" | "Recreating" | "Failed"

-- The mixer group a sound plays in.
export type AudioGroup = "Music" | "Sfx" | "Ui"

-- Whether an imported audio clip decodes when it loads or streams while it plays.
export type AudioStreamMode = "Auto" | "Stream" | "Decode"

-- Who advances the audio engine's voices.
export type AudioTimeSource = "Device" | "Simulation" | "Host"

-- How a rigid body moves.
export type BodyType = "Static" | "Kinematic" | "Dynamic"

-- What a camera draws behind the scene's geometry.
export type ClearMode = "Skybox" | "Color"

-- Who issued an undoable command.
export type CommandOrigin = "User" | "Agent"

-- Native cursor visibility and capture.
export type CursorMode = "Normal" | "Hidden" | "Locked"

-- How serious a diagnostic is.
export type DiagnosticSeverity = "Warning" | "Error"

-- The kind of an engine event (§4.9).
export type EngineEventType = "EntityCreated" | "EntityDestroyed" | "ComponentChanged" | "SceneOpened" | "SceneChangedOnDisk" | "PlayStateChanged" | "AssetReloaded" | "AssetImportFailed" | "ScriptErrorRaised" | "DiagnosticsChanged" | "AutomationClientDisconnected" | "GpuDeviceLost" | "SceneActivated" | "SceneClosed"

-- The build configuration an export targets (§2.2).
export type ExportConfiguration = "Debug" | "Release" | "Dist"

-- A step of an export (§14.2), in the order the steps run.
export type ExportPhase = "Validate" | "Cook" | "WritePaks" | "NativeBuild" | "CopyRuntime" | "PatchResources" | "WriteManifest" | "Sign" | "SmokeTest" | "FeatureTest" | "MoveToOutput" | "Done"

-- Why an export put an asset into its paks (the first reason its walk found).
export type ExportReachReason = "StartScene" | "BuildScene" | "Testing" | "Dependency" | "LiteralLoad" | "Required" | "ComputedLoad"

-- The underlying reflected value kind.
export type FieldType = "Bool" | "Int32" | "UInt32" | "Float" | "Vec2" | "Vec3" | "Vec4" | "Quat" | "Color3" | "Color4" | "Bool3" | "String" | "EntityRef" | "AssetRef" | "Enum" | "Array" | "Struct" | "Map" | "Variant"

-- Up-positive stick axes and zero-to-one triggers.
export type GamepadAxis = "LeftX" | "LeftY" | "RightX" | "RightY" | "LeftTrigger" | "RightTrigger"

-- Gamepad button positions.
export type GamepadButton = "South" | "East" | "West" | "North" | "LeftBumper" | "RightBumper" | "Back" | "Start" | "Guide" | "LeftThumb" | "RightThumb" | "DPadUp" | "DPadRight" | "DPadDown" | "DPadLeft"

-- How an input action reads its controls.
export type InputActionType = "Button" | "Axis"

-- The state of a button-like input event (§13.6).
export type InputEventState = "Down" | "Up" | "Tap"

-- The type of an input event (§13.6).
export type InputEventType = "Action" | "Key" | "MouseButton" | "MouseMove" | "MouseDelta" | "Scroll" | "GamepadButton" | "GamepadAxis" | "Text"

-- Start a fresh recording or stop and save it.
export type InputRecordAction = "Start" | "Stop"

-- Keyboard keys by their US-layout position.
export type Key = "Space" | "Apostrophe" | "Comma" | "Minus" | "Period" | "Slash" | "D0" | "D1" | "D2" | "D3" | "D4" | "D5" | "D6" | "D7" | "D8" | "D9" | "Semicolon" | "Equal" | "A" | "B" | "C" | "D" | "E" | "F" | "G" | "H" | "I" | "J" | "K" | "L" | "M" | "N" | "O" | "P" | "Q" | "R" | "S" | "T" | "U" | "V" | "W" | "X" | "Y" | "Z" | "LeftBracket" | "Backslash" | "RightBracket" | "GraveAccent" | "World1" | "World2" | "Escape" | "Enter" | "Tab" | "Backspace" | "Insert" | "Delete" | "Right" | "Left" | "Down" | "Up" | "PageUp" | "PageDown" | "Home" | "End" | "CapsLock" | "ScrollLock" | "NumLock" | "PrintScreen" | "Pause" | "F1" | "F2" | "F3" | "F4" | "F5" | "F6" | "F7" | "F8" | "F9" | "F10" | "F11" | "F12" | "F13" | "F14" | "F15" | "F16" | "F17" | "F18" | "F19" | "F20" | "F21" | "F22" | "F23" | "F24" | "F25" | "Keypad0" | "Keypad1" | "Keypad2" | "Keypad3" | "Keypad4" | "Keypad5" | "Keypad6" | "Keypad7" | "Keypad8" | "Keypad9" | "KeypadDecimal" | "KeypadDivide" | "KeypadMultiply" | "KeypadSubtract" | "KeypadAdd" | "KeypadEnter" | "KeypadEqual" | "LeftShift" | "LeftControl" | "LeftAlt" | "LeftSuper" | "RightShift" | "RightControl" | "RightAlt" | "RightSuper" | "Menu"

-- What a 2D light lights.
export type Light2DType = "Point" | "Global"

-- The logger an entry came from.
export type LogChannel = "Engine" | "App" | "Script"

-- The level of a log entry.
export type LogLevel = "Trace" | "Info" | "Warn" | "Error" | "Critical"

-- How a moving body is checked for collisions between steps.
export type MotionQuality = "Discrete" | "LinearCast"

-- Mouse button names.
export type MouseButton = "Left" | "Right" | "Middle" | "Button4" | "Button5" | "Button6" | "Button7" | "Button8"

-- What a type of a game's native module is.
export type NativeTypeRole = "Component" | "Resource"

-- The shape of a 2D overlap query.
export type Physics2DQueryShape = "Circle" | "Box"

-- Why a physics body exists (§5.3 composition rules).
export type PhysicsBodyOrigin = "RigidBody" | "ImplicitStatic" | "ImplicitSensor" | "Character"

-- How a play session runs (§5.6).
export type PlayMode = "Play" | "Simulate"

-- The play state, in the vocabulary of _meta.playState.
export type PlayRunState = "Edit" | "Play" | "Simulate" | "Paused"

-- Which ticks of a play.step extract the play scene's game view (its primary camera) for rendering.
export type PlayStepRender = "Every" | "Last" | "None"

-- What a prefab override changes on one prefab entity of an instance.
export type PrefabOverrideKind = "Field" | "AddComponent" | "RemoveComponent" | "EntityKey" | "RemoveEntity"

-- What a new project starts with.
export type ProjectTemplate = "Empty" | "Basic3D"

-- How a camera projects the scene onto the screen.
export type ProjectionType = "Perspective" | "Orthographic"

-- What scene.diff compares the open scene with.
export type SceneDiffAgainst = "Saved" | "Revision"

-- How one entity changed.
export type SceneEntityChangeKind = "Created" | "Destroyed" | "Modified"

-- Which scene a request reads or changes (§13.4 "Target").
export type SceneTarget = "Edit" | "Play"

-- What a new scene starts with.
export type SceneTemplate = "Empty"

-- How scene.tree reports the hierarchy.
export type SceneTreeFormat = "Text" | "Json"

-- The scene and VM used by script.eval.
export type ScriptEvaluationContext = "Edit" | "Play"

-- Authenticated kind of the script's load-time return value.
export type ScriptKind = "Behaviour" | "Module" | "TestSuite"

-- The shipped template used to create a source file.
export type ScriptTemplate = "Behaviour" | "Module" | "Test"

-- The oscillator of a sound effect layer.
export type SoundWave = "Sine" | "Square" | "Triangle" | "Saw" | "Noise"

-- Coordinates relative to the entity's orientation or the world axes.
export type Space = "Local" | "World"

-- The sample count of screen-space ambient occlusion (GTAO).
export type SsaoQuality = "Low" | "Medium" | "High"

-- How much state the cases of a test suite share.
export type TestIsolation = "Suite" | "Case"

-- A run mode a test suite runs in.
export type TestMode = "Editor" | "Release" | "Dist"

-- A suite's PauseOnError override. A script error never pauses a test run, so no value changes the run's time.
export type TestPauseOnError = "Inherit" | "Pause" | "Continue"

-- How the lines of a text element are aligned to each other.
export type TextAlignment = "Left" | "Center" | "Right"

-- Where a text element is placed.
export type TextSpace = "Screen" | "World"

-- How a texture's texels are interpreted.
export type TextureUsage = "Color" | "Linear" | "NormalMap"

-- The curve that maps HDR scene colours to the display.
export type Tonemapper = "AgX" | "ACES" | "PbrNeutral" | "Linear"

-- Where a UI canvas is drawn: over the scene image, or in the world.
export type UiCanvasRenderMode = "Screen" | "World"

-- Where each child of a container sits across its main axis.
export type UiCrossAlignment = "Start" | "Center" | "End" | "Stretch"

-- How a UI image samples its texture.
export type UiImageFilter = "Point" | "Linear"

-- How a UI image fills its element's rectangle.
export type UiImageType = "Simple" | "Sliced"

-- The main axis of a UI layout container.
export type UiLayoutDirection = "Horizontal" | "Vertical"

-- How a UI layout container places its children.
export type UiLayoutKind = "Stack" | "Grid" | "Flex"

-- Where a container's children sit along its main axis when they do not fill it.
export type UiMainAlignment = "Start" | "Center" | "End"

-- How a UI canvas maps its units to scene pixels.
export type UiScaleMode = "ConstantPixelSize" | "MatchHeight"

-- Where a UI text block sits in its element's rectangle.
export type UiTextVerticalAlignment = "Top" | "Middle" | "Bottom"

-- The UI widget ui.createWidget builds.
export type UiWidgetKind = "Canvas" | "Panel" | "Image" | "Text" | "Button" | "Toggle" | "Slider" | "ScrollView" | "TextInput"

-- What project.validate checks.
export type ValidationScope = "Project" | "Scene"

-- Which view viewport.screenshot renders.
export type ViewportView = "Scene" | "Game"

-- First contact from the receiving body's side, with collider identities for compounds.
export type CollisionContact = {Point: vector, Normal: vector, RelativeSpeed: number, Collider: Entity, OtherCollider: Entity}

-- Editor metadata and numeric validation bounds for a field.
export type FieldOptions = {Min: number?, Max: number?, Step: number?, Tooltip: string?}

-- A query hit distinguishes the collider entity from its body owner.
export type RaycastHit = {Entity: Entity, Body: Entity, Point: vector, Normal: vector, Distance: number}

-- Owned script diagnostic with exclusive occurrence cursor.
export type ScriptError = {id: number, kind: string, script: string, line: number, column: number, message: string, callback: string, entity: string, entityName: string, tick: number, count: number, jsonPointer: string, traceback: {{script: string, line: number, ["function"]: string}}}

-- Every behaviour instance owns an Entity identity and copies its declared field defaults.
export type ScriptInstance = {Entity: Entity}

-- Stereo RMS and absolute peak of the requested simulation sample interval.
export type TestAudioLevels = {RmsLeft: number, RmsRight: number, Peak: number}

-- Positive tick timeout overriding the suite default.
export type TestCaseOptions = {TimeoutTicks: number?}

-- The last completed render extraction: its interpolation Alpha, and Frame, the number of extractions completed so far.
export type TestExtraction = {Alpha: number, Frame: number}

-- Authenticated suite constructor result.
export type TestSuite = {Name: string, Body: () -> (), Options: TestSuiteOptions}

-- Default positive tick timeout for cases in a suite.
export type TestSuiteOptions = {CaseTimeoutTicks: number?}

-- A UI element's laid-out rectangle: its top-left (Min) and bottom-right (Max) corners as (x, y, 0), x right and y down, in scene pixels for an element of a Screen canvas and in its canvas's own pixels for one of a World canvas, and the canvas it belongs to (Canvas).
export type UiElementRect = {Min: vector, Max: vector, Canvas: Entity}

-- An opaque immutable asset handle; resolve metadata through Assets.
declare extern type AssetRef with
	-- Compares asset handles, so two references to the same asset are equal. [All modes; LoadTime|Runtime|Test; pure/read]
	__eq: (self: AssetRef, other: AssetRef) -> boolean
end

-- Hears the scene from the entity's position and orientation.
declare extern type AudioListener with
	-- Whether this listener is used; with several primaries the first in canonical order wins, without any the primary camera listens. [All modes; Runtime|Test; pure/read; setter mutates]
	Primary: boolean
end

-- Plays an audio clip, optionally positioned at the entity.
declare extern type AudioSource with
	-- The distance attenuation model of a spatial sound. [All modes; Runtime|Test; pure/read; setter mutates]
	Attenuation: Attenuation
	-- The audio clip to play; null plays nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	Clip: AssetRef?
	-- The strength of the Doppler pitch shift (0 disables it). [All modes; Runtime|Test; pure/read; setter mutates]
	DopplerFactor: number
	-- The mixer group the sound plays in. [All modes; Runtime|Test; pure/read; setter mutates]
	Group: AudioGroup
	-- Whether this source owns an unpaused voice. [All modes; Runtime|Test; pure/read]
	IsPlaying: (self: AudioSource) -> boolean
	-- Whether the clip restarts when it ends. [All modes; Runtime|Test; pure/read; setter mutates]
	Loop: boolean
	-- The distance beyond which the sound stops getting quieter, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxDistance: number
	-- The distance within which the sound plays at full volume, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	MinDistance: number
	-- Pause this source without losing its cursor. [All modes; Runtime|Test; mutates]
	Pause: (self: AudioSource) -> ()
	-- The playback speed and pitch multiplier (1 is unchanged). [All modes; Runtime|Test; pure/read; setter mutates]
	Pitch: number
	-- Start or restart this active source. [All modes; Runtime|Test; mutates]
	Play: (self: AudioSource) -> ()
	-- Whether the clip starts playing when the play session starts. [All modes; Runtime|Test; pure/read; setter mutates]
	PlayOnStart: boolean
	-- Resume this active source's paused voice. [All modes; Runtime|Test; mutates]
	Resume: (self: AudioSource) -> ()
	-- How quickly the attenuation model falls off (1 is the model's own rate). [All modes; Runtime|Test; pure/read; setter mutates]
	Rolloff: number
	-- Whether the sound is positioned at the entity and attenuated with distance. [All modes; Runtime|Test; pure/read; setter mutates]
	Spatial: boolean
	-- Release this source's voice. [All modes; Runtime|Test; mutates]
	Stop: (self: AudioSource) -> ()
	-- The linear volume multiplier (1 is the clip's own level). [All modes; Runtime|Test; pure/read; setter mutates]
	Volume: number
end

-- Optional behaviour lifecycle callbacks in execution order and canonical entity order.
declare extern type Behaviour with
	-- Called when a collision begins, with the other entity and contact. [All modes; Runtime|Test; mutates]
	OnCollisionEnter: (self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()
	-- Called when a 2D collision begins, with the other entity and the contact in the XY plane. [All modes; Runtime|Test; mutates]
	OnCollisionEnter2D: (self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()
	-- Called when the corresponding collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnCollisionExit: (self: ScriptInstance, other: Entity) -> ()
	-- Called when the corresponding 2D collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnCollisionExit2D: (self: ScriptInstance, other: Entity) -> ()
	-- Called at the corresponding lifecycle transition. [All modes; Runtime|Test; mutates]
	OnCreate: (self: ScriptInstance) -> ()
	-- Called at the corresponding lifecycle transition. [All modes; Runtime|Test; mutates]
	OnDestroy: (self: ScriptInstance) -> ()
	-- Called at the corresponding lifecycle transition. [All modes; Runtime|Test; mutates]
	OnDisable: (self: ScriptInstance) -> ()
	-- Called at the corresponding lifecycle transition. [All modes; Runtime|Test; mutates]
	OnEnable: (self: ScriptInstance) -> ()
	-- Called for the current fixed, presentation or late presentation phase. [All modes; Runtime|Test; mutates]
	OnFixedUpdate: (self: ScriptInstance, dt: number) -> ()
	-- Called after the class is replaced while instance state survives. [EditorOnly; Runtime|Test; mutates]
	OnHotReload: (self: ScriptInstance) -> ()
	-- Called for the current fixed, presentation or late presentation phase. [All modes; Runtime|Test; mutates]
	OnLateUpdate: (self: ScriptInstance, dt: number) -> ()
	-- Called at the corresponding lifecycle transition. [All modes; Runtime|Test; mutates]
	OnStart: (self: ScriptInstance) -> ()
	-- Called when the corresponding collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnTriggerEnter: (self: ScriptInstance, other: Entity) -> ()
	-- Called when the corresponding 2D collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnTriggerEnter2D: (self: ScriptInstance, other: Entity) -> ()
	-- Called when the corresponding collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnTriggerExit: (self: ScriptInstance, other: Entity) -> ()
	-- Called when the corresponding 2D collision or trigger transition occurs. [All modes; Runtime|Test; mutates]
	OnTriggerExit2D: (self: ScriptInstance, other: Entity) -> ()
	-- Called when Cancel is pressed, at the focused UI element (or the last canvas when none is focused); bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiCancel: (self: ScriptInstance, element: Entity) -> boolean?
	-- Called when a UI button is clicked or submitted, at its entity, then bubbling to its ancestors; return true to stop it. [All modes; Runtime|Test; mutates]
	OnUiClick: (self: ScriptInstance, element: Entity) -> boolean?
	-- Called when a UI element gains (true) or loses (false) keyboard and gamepad focus; bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiFocus: (self: ScriptInstance, element: Entity, focused: boolean) -> boolean?
	-- Called when the pointer enters (true) or leaves (false) an interactable UI widget; bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiHover: (self: ScriptInstance, element: Entity, hovered: boolean) -> boolean?
	-- Called when interaction scrolls a UI scroll view, with its new ScrollPosition (x, y, 0); bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiScroll: (self: ScriptInstance, element: Entity, position: vector) -> boolean?
	-- Called when Enter (or Submit) is pressed while a UI text input has focus, with its text; bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiSubmit: (self: ScriptInstance, element: Entity, text: string) -> boolean?
	-- Called when typing or an editing key changes a UI text input's Text, with its new text; bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiTextChanged: (self: ScriptInstance, element: Entity, text: string) -> boolean?
	-- Called when interaction changes a UI toggle's IsOn (a boolean) or a UI slider's Value (a number); bubbles like OnUiClick. [All modes; Runtime|Test; mutates]
	OnUiValueChanged: (self: ScriptInstance, element: Entity, value: boolean | number) -> boolean?
	-- Called for the current fixed, presentation or late presentation phase. [All modes; Runtime|Test; mutates]
	OnUpdate: (self: ScriptInstance, dt: number) -> ()
end

-- A box-shaped collider in the entity's space.
declare extern type BoxCollider with
	-- Half the box's size along each local axis, in metres; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	HalfExtents: vector
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The box centre relative to the entity, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The box rotation relative to the entity: a unit quaternion. [All modes; Runtime|Test; pure/read; setter mutates]
	Rotation: Quat
end

-- A rectangle collider in the entity's XY plane.
declare extern type BoxCollider2D with
	-- The rectangle's rotation about the entity's Z axis, counter-clockwise, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	Angle: number
	-- Half the rectangle's size along local X and Y; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	HalfExtents: vector
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The rectangle's centre relative to the entity, in its XY plane. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
end

-- Renders the scene from the entity, looking down its local -Z axis.
declare extern type Camera with
	-- What the camera draws behind the scene. [All modes; Runtime|Test; pure/read; setter mutates]
	Clear: ClearMode
	-- The linear background colour when Clear is Color. [All modes; Runtime|Test; pure/read; setter mutates]
	ClearColor: Color
	-- The far clipping distance, in metres; beyond it nothing is drawn or culled in. [All modes; Runtime|Test; pure/read; setter mutates]
	FarClip: number
	-- The near clipping distance, in metres; below FarClip. [All modes; Runtime|Test; pure/read; setter mutates]
	NearClip: number
	-- Half the visible height of the orthographic projection, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	OrthographicSize: number
	-- Whether this is the scene's primary camera (exactly one active camera should be). [All modes; Runtime|Test; pure/read; setter mutates]
	Primary: boolean
	-- Perspective or orthographic projection. [All modes; Runtime|Test; pure/read; setter mutates]
	Projection: ProjectionType
	-- Ray through a viewport pixel center, origin top-left, using the rendered camera pose; under a PixelPerfectCamera through its snapped reference view and the output mapping. [All modes; Runtime|Test; pure/read]
	ScreenToWorldRay: (self: Camera, x: number, y: number) -> (vector, vector)
	-- The vertical field of view of the perspective projection, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	VerticalFov: number
	-- Project to top-left pixel coordinates and reverse-Z depth using the rendered camera pose; under a PixelPerfectCamera through its snapped reference view and the output mapping. [All modes; Runtime|Test; pure/read]
	WorldToScreen: (self: Camera, point: vector) -> vector
end

-- A capsule-shaped collider along the local Y axis.
declare extern type CapsuleCollider with
	-- Half the height of the cylinder between the hemispheres, in metres; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	HalfHeight: number
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The capsule centre relative to the entity, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The radius of the capsule's hemispheres and cylinder, in metres; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	Radius: number
	-- The capsule rotation relative to the entity: a unit quaternion. [All modes; Runtime|Test; pure/read; setter mutates]
	Rotation: Quat
end

-- Moves the entity as a walking character that climbs steps and slopes and collides with the world.
declare extern type CharacterController with
	-- Ground normal from the last update. [All modes; Runtime|Test; pure/read]
	GetGroundNormal: (self: CharacterController) -> vector
	-- Velocity after the last update. [All modes; Runtime|Test; pure/read]
	GetVelocity: (self: CharacterController) -> vector
	-- The multiplier of the project's gravity for the character. [All modes; Runtime|Test; pure/read; setter mutates]
	GravityFactor: number
	-- The character's total height, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Height: number
	-- While grounded, the character also moves with the horizontal velocity of what it stands on, so a moving platform carries it. [All modes; Runtime|Test; pure/read; setter mutates]
	InheritGroundVelocity: boolean
	-- Whether the last character update found supporting ground. [All modes; Runtime|Test; pure/read]
	IsGrounded: (self: CharacterController) -> boolean
	-- Physics layer name from project settings. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Layer: string
	-- The character's mass in kilograms, used when it pushes bodies. [All modes; Runtime|Test; pure/read; setter mutates]
	Mass: number
	-- The steepest slope the character can walk up, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxSlopeAngle: number
	-- Supply desired velocity for the next fixed character update. [All modes; Runtime|Test; mutates]
	Move: (self: CharacterController, velocity: vector) -> ()
	-- The radius of the character's capsule, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Radius: number
	-- The highest step the character climbs without jumping, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	StepHeight: number
end

-- A circle collider in the entity's XY plane.
declare extern type CircleCollider2D with
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The circle's centre relative to the entity, in its XY plane. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The circle's radius; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	Radius: number
end

-- A local linear RGBA float value. Channels are finite and unclamped, allowing HDR values.
declare extern type Color with
	-- Compares the four channels exactly, like vector equality. [All modes; LoadTime|Runtime|Test; pure/read]
	__eq: (self: Color, other: Color) -> boolean
	-- The local alpha channel, finite and unclamped. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	a: number
	-- The local linear blue channel, finite and unclamped. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	b: number
	-- The local linear green channel, finite and unclamped. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	g: number
	-- The local linear red channel, finite and unclamped. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	r: number
end

declare Color: {
	-- Reads #RRGGBB or #RRGGBBAA, case-insensitively, as linear channel bytes divided by 255; absent alpha is one. [All modes; LoadTime|Runtime|Test; pure/read]
	FromHex: (hex: string) -> Color,
	-- Interpolates all four linear channels with t clamped to [0, 1], producing a new color. [All modes; LoadTime|Runtime|Test; pure/read]
	Lerp: (a: Color, b: Color, t: number) -> Color,
	-- Creates a linear color; alpha defaults to one. Channels must fit finite engine floats. [All modes; LoadTime|Runtime|Test; pure/read]
	New: (r: number, g: number, b: number, a: number?) -> Color,
}

-- A sun-like light shining down the entity's local -Z axis.
declare extern type DirectionalLight with
	-- The number of shadow cascades, from 1 to 4. [All modes; Runtime|Test; pure/read; setter mutates]
	CascadeCount: number
	-- The cascade split blend from uniform (0) to logarithmic (1). [All modes; Runtime|Test; pure/read; setter mutates]
	CascadeSplitLambda: number
	-- Whether the light casts cascaded soft shadows. [All modes; Runtime|Test; pure/read; setter mutates]
	CastShadows: boolean
	-- The linear light colour. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- The shadow depth bias, in shadow-map texels. [All modes; Runtime|Test; pure/read; setter mutates]
	DepthBias: number
	-- The unitless brightness multiplied with Color. [All modes; Runtime|Test; pure/read; setter mutates]
	Intensity: number
	-- The angular diameter of the light source, in degrees; larger values soften shadow penumbrae. [All modes; Runtime|Test; pure/read; setter mutates]
	LightAngle: number
	-- The shadow normal-offset bias, in shadow-map texels. [All modes; Runtime|Test; pure/read; setter mutates]
	NormalBias: number
	-- How far from the camera shadows reach, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	ShadowDistance: number
end

-- A UUID and scene generation identifying an entity; no native address is exposed. After a Destroy, the entity's reads keep working until the destruction flush, while IsValid is false and its writes raise.
declare extern type Entity with
	-- Validate and add a component and return its proxy; Script instances are synchronized before returning, and nil is returned when the new Script's OnCreate destroyed the entity. Each value holds at most 4096 bytes of text (strings, map keys and Variant text together). [All modes; Runtime|Test; mutates]
	AddComponent: (self: Entity, name: string, values: {[string]: any}?) -> any
	-- Add a nonempty tag of at most 256 bytes if absent; an entity carries at most 256 tags. [All modes; Runtime|Test; mutates]
	AddTag: (self: Entity, tag: string) -> ()
	-- The entity's AudioListener component, or nil when absent. [All modes; Runtime|Test; pure/read]
	AudioListener: AudioListener?
	-- The entity's AudioSource component, or nil when absent. [All modes; Runtime|Test; pure/read]
	AudioSource: AudioSource?
	-- The entity's BoxCollider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	BoxCollider: BoxCollider?
	-- The entity's BoxCollider2D component, or nil when absent. [All modes; Runtime|Test; pure/read]
	BoxCollider2D: BoxCollider2D?
	-- The entity's Camera component, or nil when absent. [All modes; Runtime|Test; pure/read]
	Camera: Camera?
	-- The entity's CapsuleCollider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	CapsuleCollider: CapsuleCollider?
	-- The entity's CharacterController component, or nil when absent. [All modes; Runtime|Test; pure/read]
	CharacterController: CharacterController?
	-- The entity's CircleCollider2D component, or nil when absent. [All modes; Runtime|Test; pure/read]
	CircleCollider2D: CircleCollider2D?
	-- Mark this subtree for the normal destruction flush. [All modes; Runtime|Test; mutates]
	Destroy: (self: Entity) -> ()
	-- The entity's DirectionalLight component, or nil when absent. [All modes; Runtime|Test; pure/read]
	DirectionalLight: DirectionalLight?
	-- The entity's Environment component, or nil when absent. [All modes; Runtime|Test; pure/read]
	Environment: Environment?
	-- Find the first exact child name in depth-first sibling order; recursive defaults false. [All modes; Runtime|Test; pure/read]
	FindChild: (self: Entity, name: string, recursive: boolean?) -> Entity?
	-- Copy the children in sibling order. [All modes; Runtime|Test; pure/read]
	GetChildren: (self: Entity) -> {Entity}
	-- Get a component proxy, or nil when absent. [All modes; Runtime|Test; pure/read]
	function GetComponent(self, name: "Transform"): Transform?
	function GetComponent(self, name: "MeshRenderer"): MeshRenderer?
	function GetComponent(self, name: "Camera"): Camera?
	function GetComponent(self, name: "DirectionalLight"): DirectionalLight?
	function GetComponent(self, name: "PointLight"): PointLight?
	function GetComponent(self, name: "SpotLight"): SpotLight?
	function GetComponent(self, name: "Environment"): Environment?
	function GetComponent(self, name: "PostProcess"): PostProcess?
	function GetComponent(self, name: "Text"): Text?
	function GetComponent(self, name: "RigidBody"): RigidBody?
	function GetComponent(self, name: "BoxCollider"): BoxCollider?
	function GetComponent(self, name: "SphereCollider"): SphereCollider?
	function GetComponent(self, name: "CapsuleCollider"): CapsuleCollider?
	function GetComponent(self, name: "MeshCollider"): MeshCollider?
	function GetComponent(self, name: "CharacterController"): CharacterController?
	function GetComponent(self, name: "AudioSource"): AudioSource?
	function GetComponent(self, name: "AudioListener"): AudioListener?
	function GetComponent(self, name: "Script"): Script?
	function GetComponent(self, name: "SpriteRenderer"): SpriteRenderer?
	function GetComponent(self, name: "SpriteAnimator"): SpriteAnimator?
	function GetComponent(self, name: "PixelPerfectCamera"): PixelPerfectCamera?
	function GetComponent(self, name: "Light2D"): Light2D?
	function GetComponent(self, name: "TilemapRenderer"): TilemapRenderer?
	function GetComponent(self, name: "RigidBody2D"): RigidBody2D?
	function GetComponent(self, name: "BoxCollider2D"): BoxCollider2D?
	function GetComponent(self, name: "CircleCollider2D"): CircleCollider2D?
	function GetComponent(self, name: "PolygonCollider2D"): PolygonCollider2D?
	function GetComponent(self, name: "TilemapCollider"): TilemapCollider?
	function GetComponent(self, name: "UiCanvas"): UiCanvas?
	function GetComponent(self, name: "UiRect"): UiRect?
	function GetComponent(self, name: "UiImage"): UiImage?
	function GetComponent(self, name: "UiText"): UiText?
	function GetComponent(self, name: "UiLayout"): UiLayout?
	function GetComponent(self, name: "UiLayoutElement"): UiLayoutElement?
	function GetComponent(self, name: "UiButton"): UiButton?
	function GetComponent(self, name: "UiToggle"): UiToggle?
	function GetComponent(self, name: "UiSlider"): UiSlider?
	function GetComponent(self, name: "UiScrollView"): UiScrollView?
	function GetComponent(self, name: "UiTextInput"): UiTextInput?
	-- Get the parent, or nil for a root. [All modes; Runtime|Test; pure/read]
	GetParent: (self: Entity) -> Entity?
	-- Get the instance of the first script slot whose behaviour has this name, or of the first slot without a name; nil when there is none. [All modes; Runtime|Test; pure/read]
	GetScript: (self: Entity, name: string?) -> any
	-- Get the live instances of the entity's script slots, in slot order. [All modes; Runtime|Test; pure/read]
	GetScripts: (self: Entity) -> {any}
	-- Copy tags in insertion order. [All modes; Runtime|Test; pure/read]
	GetTags: (self: Entity) -> {string}
	-- Get the world AABB of what this entity draws: its render mesh, its sprite's quad and its tilemap's drawn tiles; nil without any of them. [All modes; Runtime|Test; pure/read]
	GetWorldBounds: (self: Entity) -> (vector?, vector?)
	-- Check an exposed component by its exact registry name. [All modes; Runtime|Test; pure/read]
	HasComponent: (self: Entity, name: string) -> boolean
	-- Check an exact tag. [All modes; Runtime|Test; pure/read]
	HasTag: (self: Entity, tag: string) -> boolean
	-- The immutable 16-digit hexadecimal UUID. [All modes; Runtime|Test; pure/read]
	ID: string
	-- The effective active state including every ancestor. [All modes; Runtime|Test; pure/read]
	IsActive: (self: Entity) -> boolean
	-- The entity's local active state. [All modes; Runtime|Test; pure/read]
	IsActiveSelf: (self: Entity) -> boolean
	-- Whether this identity still names a live entity in this scene. [All modes; Runtime|Test; pure/read]
	IsValid: (self: Entity) -> boolean
	-- The entity's Light2D component, or nil when absent. [All modes; Runtime|Test; pure/read]
	Light2D: Light2D?
	-- The entity's MeshCollider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	MeshCollider: MeshCollider?
	-- The entity's MeshRenderer component, or nil when absent. [All modes; Runtime|Test; pure/read]
	MeshRenderer: MeshRenderer?
	-- The entity name; a written name is UTF-8 without NUL and at most 256 bytes. [All modes; Runtime|Test; pure/read; setter mutates]
	Name: string
	-- The entity's PixelPerfectCamera component, or nil when absent. [All modes; Runtime|Test; pure/read]
	PixelPerfectCamera: PixelPerfectCamera?
	-- The entity's PointLight component, or nil when absent. [All modes; Runtime|Test; pure/read]
	PointLight: PointLight?
	-- The entity's PolygonCollider2D component, or nil when absent. [All modes; Runtime|Test; pure/read]
	PolygonCollider2D: PolygonCollider2D?
	-- The entity's PostProcess component, or nil when absent. [All modes; Runtime|Test; pure/read]
	PostProcess: PostProcess?
	-- Remove a removable component when no present component requires it. [All modes; Runtime|Test; mutates]
	RemoveComponent: (self: Entity, name: string) -> ()
	-- Remove a tag if present. [All modes; Runtime|Test; mutates]
	RemoveTag: (self: Entity, tag: string) -> ()
	-- The entity's RigidBody component, or nil when absent. [All modes; Runtime|Test; pure/read]
	RigidBody: RigidBody?
	-- The entity's RigidBody2D component, or nil when absent. [All modes; Runtime|Test; pure/read]
	RigidBody2D: RigidBody2D?
	-- Change local active state and dispatch effective lifecycle transitions before returning. [All modes; Runtime|Test; mutates]
	SetActive: (self: Entity, active: boolean) -> ()
	-- Reparent to the last sibling, preserving world pose by default; synchronize effective lifecycle transitions. [All modes; Runtime|Test; mutates]
	SetParent: (self: Entity, parent: Entity?, keepWorld: boolean?) -> ()
	-- The entity's SphereCollider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	SphereCollider: SphereCollider?
	-- The entity's SpotLight component, or nil when absent. [All modes; Runtime|Test; pure/read]
	SpotLight: SpotLight?
	-- The entity's SpriteAnimator component, or nil when absent. [All modes; Runtime|Test; pure/read]
	SpriteAnimator: SpriteAnimator?
	-- The entity's SpriteRenderer component, or nil when absent. [All modes; Runtime|Test; pure/read]
	SpriteRenderer: SpriteRenderer?
	-- The entity's Text component, or nil when absent. [All modes; Runtime|Test; pure/read]
	Text: Text?
	-- The entity's TilemapCollider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	TilemapCollider: TilemapCollider?
	-- The entity's TilemapRenderer component, or nil when absent. [All modes; Runtime|Test; pure/read]
	TilemapRenderer: TilemapRenderer?
	-- The entity's Transform component, which every entity has. [All modes; Runtime|Test; pure/read]
	Transform: Transform
	-- The entity's UiButton component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiButton: UiButton?
	-- The entity's UiCanvas component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiCanvas: UiCanvas?
	-- The entity's UiImage component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiImage: UiImage?
	-- The entity's UiLayout component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiLayout: UiLayout?
	-- The entity's UiLayoutElement component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiLayoutElement: UiLayoutElement?
	-- The entity's UiRect component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiRect: UiRect?
	-- The entity's UiScrollView component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiScrollView: UiScrollView?
	-- The entity's UiSlider component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiSlider: UiSlider?
	-- The entity's UiText component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiText: UiText?
	-- The entity's UiTextInput component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiTextInput: UiTextInput?
	-- The entity's UiToggle component, or nil when absent. [All modes; Runtime|Test; pure/read]
	UiToggle: UiToggle?
	-- Compare UUIDs; equality does not authorize access to an expired scene. [All modes; Runtime|Test; pure/read]
	__eq: (self: Entity, other: Entity) -> boolean
end

-- The scene's image-based lighting and skybox (one per scene).
declare extern type Environment with
	-- The environment map; null lights the scene with FallbackColor. [All modes; Runtime|Test; pure/read; setter mutates]
	Environment: AssetRef?
	-- The linear ambient colour used when no environment map is assigned. [All modes; Runtime|Test; pure/read; setter mutates]
	FallbackColor: Color
	-- The unitless strength of the image-based lighting. [All modes; Runtime|Test; pure/read; setter mutates]
	Intensity: number
	-- The rotation of the environment about the +Y axis, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	Rotation: number
	-- Whether cameras that clear to the skybox show the environment map. [All modes; Runtime|Test; pure/read; setter mutates]
	ShowSkybox: boolean
	-- How blurred the visible skybox is, from sharp (0) to fully blurred (1). [All modes; Runtime|Test; pure/read; setter mutates]
	SkyboxBlur: number
end

-- An authenticated immutable Field constructor descriptor.
declare extern type FieldDescriptor with
end

-- A 2D light: lights the lit sprites and tilemaps of the view, never unlit ones; no shadows.
declare extern type Light2D with
	-- The light's linear colour. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- Point: the fraction of the radius, from its edge inward, over which the light fades smoothly to nothing; 0 is a hard edge. [All modes; Runtime|Test; pure/read; setter mutates]
	Falloff: number
	-- Point: how far above the lit plane the light sits, in world units; only normal-mapped sprites read it. [All modes; Runtime|Test; pure/read; setter mutates]
	Height: number
	-- How strong the light is: its colour is multiplied by this, so values above 1 overbright. [All modes; Runtime|Test; pure/read; setter mutates]
	Intensity: number
	-- Point: how far the light reaches from the entity, in world units. [All modes; Runtime|Test; pure/read; setter mutates]
	Radius: number
	-- Whether the light is a point light around the entity or a global light of the whole view. [All modes; Runtime|Test; pure/read; setter mutates]
	Type: Light2DType
end

-- A collider shaped like a mesh: its convex hull, or the exact triangles for Static and Kinematic bodies.
declare extern type MeshCollider with
	-- Whether the convex hull is used; a non-convex mesh collider needs a Static or Kinematic body. [All modes; Runtime|Test; pure/read; setter mutates]
	Convex: boolean
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The collision mesh; null uses the entity's MeshRenderer mesh. [All modes; Runtime|Test; pure/read; setter mutates]
	Mesh: AssetRef?
end

-- Draws a mesh asset with one material per submesh.
declare extern type MeshRenderer with
	-- Whether the mesh casts shadows. [All modes; Runtime|Test; pure/read; setter mutates]
	CastShadows: boolean
	-- Read a 1-based material slot, resolving an absent or null override to the mesh default. [All modes; Runtime|Test; pure/read]
	GetMaterial: (self: MeshRenderer, index: number) -> AssetRef?
	-- One material per submesh; an empty list or a null slot uses the mesh's default material for that submesh. [All modes; Runtime|Test; pure/read; setter mutates]
	Materials: {AssetRef?}
	-- The mesh to draw; null draws nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	Mesh: AssetRef?
	-- Whether shadows fall on the mesh. [All modes; Runtime|Test; pure/read; setter mutates]
	ReceiveShadows: boolean
	-- Override an existing 1-based mesh or explicit material slot; nil restores the mesh default. [All modes; Runtime|Test; mutates]
	SetMaterial: (self: MeshRenderer, index: number, asset: AssetRef?) -> ()
	-- Whether the mesh is drawn at all. [All modes; Runtime|Test; pure/read; setter mutates]
	Visible: boolean
end

-- Renders the entity's camera at a reference resolution, upscaled by the largest whole factor that fits the view in a letterbox.
declare extern type PixelPerfectCamera with
	-- The linear colour of the bars around the upscaled image. [All modes; Runtime|Test; pure/read; setter mutates]
	LetterboxColor: Color
	-- How many scene pixels make one world unit; sprites of the same PixelsPerUnit map each texel to one pixel. [All modes; Runtime|Test; pure/read; setter mutates]
	PixelsPerUnit: number
	-- The height the scene renders at, in pixels; the orthographic view is this many pixels of PixelsPerUnit high. [All modes; Runtime|Test; pure/read; setter mutates]
	ReferenceHeight: number
	-- The width the scene renders at, in pixels. [All modes; Runtime|Test; pure/read; setter mutates]
	ReferenceWidth: number
end

-- A light shining in every direction from the entity (no shadows).
declare extern type PointLight with
	-- The linear light colour. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- The unitless brightness multiplied with Color. [All modes; Runtime|Test; pure/read; setter mutates]
	Intensity: number
	-- The distance at which the light's falloff reaches zero, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Range: number
	-- The radius of the emitting sphere, in metres; widens highlights. [All modes; Runtime|Test; pure/read; setter mutates]
	SourceRadius: number
end

-- A convex polygon collider in the entity's XY plane.
declare extern type PolygonCollider2D with
	-- The polygon's rotation about the entity's Z axis, counter-clockwise, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	Angle: number
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- Where the polygon's origin sits relative to the entity, in its XY plane. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The points whose convex hull is the collider, 3 to 64 of them, in the collider's own space before Offset and Angle. [All modes; Runtime|Test; pure/read; setter mutates]
	Points: {vector}
end

-- The scene's exposure, tone mapping, ambient occlusion, bloom and anti-aliasing (one per scene).
declare extern type PostProcess with
	-- Whether bright areas bloom. [All modes; Runtime|Test; pure/read; setter mutates]
	BloomEnabled: boolean
	-- How much of the bloom is blended into the image. [All modes; Runtime|Test; pure/read; setter mutates]
	BloomIntensity: number
	-- The exposure compensation in stops: the image is multiplied by 2^EV. [All modes; Runtime|Test; pure/read; setter mutates]
	ExposureEV: number
	-- Whether FXAA anti-aliasing is applied. [All modes; Runtime|Test; pure/read; setter mutates]
	FxaaEnabled: boolean
	-- Whether screen-space ambient occlusion (GTAO) is applied. It darkens the environment's image-based light only (the Environment's FallbackColor ambient without a map), never direct lights. [All modes; Runtime|Test; pure/read; setter mutates]
	SsaoEnabled: boolean
	-- The strength of the ambient occlusion, which darkens the environment's image-based light only, never direct lights. [All modes; Runtime|Test; pure/read; setter mutates]
	SsaoIntensity: number
	-- The ambient occlusion sample count. [All modes; Runtime|Test; pure/read; setter mutates]
	SsaoQuality: SsaoQuality
	-- The world-space radius within which ambient occlusion is sampled, in metres. An occluder further than this in front of a surface casts little occlusion on it: set it above the depth gap between the pieces and the surface behind them. [All modes; Runtime|Test; pure/read; setter mutates]
	SsaoRadius: number
	-- The tone mapping curve. [All modes; Runtime|Test; pure/read; setter mutates]
	Tonemap: Tonemapper
end

-- A local nonzero quaternion, stored as finite float x, y, z, w components. Angles are degrees.
declare extern type Quat with
	-- Returns the algebraic inverse, preserving reciprocal magnitude; an unrepresentable result is an error. [All modes; LoadTime|Runtime|Test; pure/read]
	Inverse: (self: Quat) -> Quat
	-- Returns a new unit quaternion with the same rotation. [All modes; LoadTime|Runtime|Test; pure/read]
	Normalized: (self: Quat) -> Quat
	-- Rotates a vector using the normalized quaternion; does not scale the vector. [All modes; LoadTime|Runtime|Test; pure/read]
	Rotate: (self: Quat, value: vector) -> vector
	-- Returns Z-X-Y Euler degrees with X in [-90, 90] and Y/Z in (-180, 180], matching the inspector. [All modes; LoadTime|Runtime|Test; pure/read]
	ToEuler: (self: Quat) -> vector
	-- Compares the four components exactly, like vector equality; q and -q are the same rotation but compare unequal. [All modes; LoadTime|Runtime|Test; pure/read]
	__eq: (self: Quat, other: Quat) -> boolean
	-- Composes quaternions (right operand applied first), or rotates a vector as Rotate does. [All modes; LoadTime|Runtime|Test; pure/read]
	function __mul(self, b: Quat): Quat
	function __mul(self, value: vector): vector
	-- The local w component; writes preserve a nonzero finite quaternion. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	w: number
	-- The local x component; writes preserve a nonzero finite quaternion. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	x: number
	-- The local y component; writes preserve a nonzero finite quaternion. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	y: number
	-- The local z component; writes preserve a nonzero finite quaternion. [All modes; LoadTime|Runtime|Test; pure/read; local setter]
	z: number
end

declare Quat: {
	-- Creates a unit rotation around a nonzero axis, normalizing its length. [All modes; LoadTime|Runtime|Test; pure/read]
	AngleAxis: (degrees: number, axis: vector) -> Quat,
	-- Creates a unit rotation from degrees, applying Z, then X, then Y, matching Transform.EulerAngles. [All modes; LoadTime|Runtime|Test; pure/read]
	FromEuler: (x: number, y: number, z: number) -> Quat,
	-- Creates the identity rotation (0, 0, 0, 1). [All modes; LoadTime|Runtime|Test; pure/read]
	Identity: () -> Quat,
	-- Points local -Z along forward and aligns +Y to projected up (default +Y). Zero or nearly parallel directions are errors. [All modes; LoadTime|Runtime|Test; pure/read]
	LookRotation: (forward: vector, up: vector?) -> Quat,
	-- Creates an unnormalized quaternion; at least one component must be nonzero. [All modes; LoadTime|Runtime|Test; pure/read]
	New: (x: number, y: number, z: number, w: number) -> Quat,
	-- Interpolates normalized rotations along the shortest arc, with t clamped to [0, 1]. [All modes; LoadTime|Runtime|Test; pure/read]
	Slerp: (a: Quat, b: Quat, t: number) -> Quat,
}

type EngineSignature_15_RandomGenerator_Choice = <T>(self: RandomGenerator, array: {T}) -> T
type EngineSignature_15_RandomGenerator_Shuffle = <T>(self: RandomGenerator, array: {T}) -> {T}
-- An independent value-owned seeded stream; its methods never change session randomness.
declare extern type RandomGenerator with
	-- Draws a boolean with the given probability, default one half. [All modes; Runtime|Test; pure/read]
	Bool: (self: RandomGenerator, probability: number?) -> boolean
	-- Chooses from a nonempty dense array using only this stream. [All modes; Runtime|Test; pure/read]
	Choice: EngineSignature_15_RandomGenerator_Choice
	-- Draws an inclusive bounded integer from this stream. [All modes; Runtime|Test; pure/read]
	Integer: (self: RandomGenerator, min: number, max: number) -> number
	-- Draws in [min,max); no arguments means [0,1), one means [0,max). [All modes; Runtime|Test; pure/read]
	Number: (self: RandomGenerator, min: number?, max: number?) -> number
	-- Reseeds this local generator with a nonnegative exact integer. [All modes; Runtime|Test; pure/read]
	Seed: (self: RandomGenerator, seed: number) -> ()
	-- Shuffles a mutable dense array in place using only this stream. [All modes; Runtime|Test; pure/read]
	Shuffle: EngineSignature_15_RandomGenerator_Shuffle
	-- Draws a uniform direction from this stream with deterministic trigonometry. [All modes; Runtime|Test; pure/read]
	UnitVector: (self: RandomGenerator) -> vector
end

-- Simulates the entity with Jolt physics.
declare extern type RigidBody with
	-- Apply an angular impulse to a Dynamic body. [All modes; Runtime|Test; mutates]
	AddAngularImpulse: (self: RigidBody, impulse: vector) -> ()
	-- Apply force to a Dynamic body. [All modes; Runtime|Test; mutates]
	AddForce: (self: RigidBody, force: vector) -> ()
	-- Apply force at a world position to a Dynamic body. [All modes; Runtime|Test; mutates]
	AddForceAtPosition: (self: RigidBody, force: vector, position: vector) -> ()
	-- Apply an impulse to a Dynamic body. [All modes; Runtime|Test; mutates]
	AddImpulse: (self: RigidBody, impulse: vector) -> ()
	-- Apply torque to a Dynamic body. [All modes; Runtime|Test; mutates]
	AddTorque: (self: RigidBody, torque: vector) -> ()
	-- Whether the body may sleep when it comes to rest. [All modes; Runtime|Test; pure/read; setter mutates]
	AllowSleeping: boolean
	-- How quickly angular velocity decays, per second. [All modes; Runtime|Test; pure/read; setter mutates]
	AngularDamping: number
	-- Removes ghost collisions with internal edges of compound shapes; set it on rolling bodies. [All modes; Runtime|Test; pure/read; setter mutates]
	EnhancedInternalEdgeRemoval: boolean
	-- The friction coefficient of the body's surfaces. A contact uses the geometric mean of both bodies' values, so 0 on one body removes friction from all its contacts. [All modes; Runtime|Test; pure/read; setter mutates]
	Friction: number
	-- Read current angular velocity. [All modes; Runtime|Test; pure/read]
	GetAngularVelocity: (self: RigidBody) -> vector
	-- Read current linear velocity. [All modes; Runtime|Test; pure/read]
	GetLinearVelocity: (self: RigidBody) -> vector
	-- The multiplier of the project's gravity for this body (0 floats). [All modes; Runtime|Test; pure/read; setter mutates]
	GravityFactor: number
	-- The world-space angular velocity the body starts with, in radians per second. [All modes; Runtime|Test; pure/read; setter mutates]
	InitialAngularVelocity: vector
	-- The world-space velocity the body starts with, in metres per second. [All modes; Runtime|Test; pure/read; setter mutates]
	InitialLinearVelocity: vector
	-- Whether the body is sleeping. [All modes; Runtime|Test; pure/read]
	IsSleeping: (self: RigidBody) -> boolean
	-- Physics layer name from project settings. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Layer: string
	-- How quickly linear velocity decays, per second. [All modes; Runtime|Test; pure/read; setter mutates]
	LinearDamping: number
	-- Per world axis: whether the body cannot rotate about it. [All modes; Runtime|Test; pure/read; setter mutates]
	LockRotation: {boolean}
	-- Per world axis: whether the body cannot move along it. [All modes; Runtime|Test; pure/read; setter mutates]
	LockTranslation: {boolean}
	-- Mass in kilograms (Dynamic only). [All modes; Runtime|Test; pure/read; setter mutates]
	Mass: number
	-- The angular speed the body is clamped to, in radians per second; raise it for fast rolling balls. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxAngularVelocity: number
	-- The speed the body is clamped to, in metres per second. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxLinearVelocity: number
	-- Discrete or continuous (LinearCast) collision detection. [All modes; Runtime|Test; pure/read; setter mutates]
	MotionQuality: MotionQuality
	-- Set a Kinematic target for the next fixed step. [All modes; Runtime|Test; mutates]
	MoveKinematic: (self: RigidBody, position: vector, rotation: Quat) -> ()
	-- Bounciness, from no bounce (0) to a perfectly elastic bounce (1). A contact uses the larger of both bodies' values, so 1 on a ball makes it bounce elastically off every surface. [All modes; Runtime|Test; pure/read; setter mutates]
	Restitution: number
	-- Set a Dynamic body's angular velocity. [All modes; Runtime|Test; mutates]
	SetAngularVelocity: (self: RigidBody, velocity: vector) -> ()
	-- Set a Dynamic body's linear velocity. [All modes; Runtime|Test; mutates]
	SetLinearVelocity: (self: RigidBody, velocity: vector) -> ()
	-- Place a body immediately, preserving velocity and resetting interpolation. [All modes; Runtime|Test; mutates]
	Teleport: (self: RigidBody, position: vector, rotation: Quat?) -> ()
	-- Static never moves; Kinematic is moved by script; Dynamic is simulated. [All modes; Runtime|Test; pure/read; setter mutates]
	Type: BodyType
	-- Wake a sleeping body. [All modes; Runtime|Test; mutates]
	WakeUp: (self: RigidBody) -> ()
end

-- Simulates the entity in the 2D world: moves along world X and Y and rotates about world Z.
declare extern type RigidBody2D with
	-- Apply an angular impulse about +Z to a Dynamic 2D body. [All modes; Runtime|Test; mutates]
	AddAngularImpulse: (self: RigidBody2D, impulse: number) -> ()
	-- Apply a force in the XY plane to a Dynamic 2D body. [All modes; Runtime|Test; mutates]
	AddForce: (self: RigidBody2D, force: vector) -> ()
	-- Apply a force in the XY plane at a world position of the plane to a Dynamic 2D body. [All modes; Runtime|Test; mutates]
	AddForceAtPosition: (self: RigidBody2D, force: vector, position: vector) -> ()
	-- Apply an impulse in the XY plane to a Dynamic 2D body. [All modes; Runtime|Test; mutates]
	AddImpulse: (self: RigidBody2D, impulse: vector) -> ()
	-- Apply a torque about +Z, in newton metres, to a Dynamic 2D body. [All modes; Runtime|Test; mutates]
	AddTorque: (self: RigidBody2D, torque: number) -> ()
	-- Whether the body may sleep when it comes to rest. [All modes; Runtime|Test; pure/read; setter mutates]
	AllowSleeping: boolean
	-- How quickly angular velocity decays, per second. [All modes; Runtime|Test; pure/read; setter mutates]
	AngularDamping: number
	-- Removes ghost collisions with internal edges of compound shapes; set it on bodies that roll or slide across tiles. [All modes; Runtime|Test; pure/read; setter mutates]
	EnhancedInternalEdgeRemoval: boolean
	-- Whether a Dynamic body is kept from rotating. [All modes; Runtime|Test; pure/read; setter mutates]
	FixedRotation: boolean
	-- The friction coefficient of the body's surfaces; a contact uses the geometric mean of both bodies' values. [All modes; Runtime|Test; pure/read; setter mutates]
	Friction: number
	-- Read the angular velocity about +Z in radians per second. [All modes; Runtime|Test; pure/read]
	GetAngularVelocity: (self: RigidBody2D) -> number
	-- Read the linear velocity in the XY plane (Z is 0). [All modes; Runtime|Test; pure/read]
	GetLinearVelocity: (self: RigidBody2D) -> vector
	-- The multiplier of the project's 2D gravity for this body (0 floats). [All modes; Runtime|Test; pure/read; setter mutates]
	GravityFactor: number
	-- The angular velocity about world Z the body starts with, in radians per second, counter-clockwise seen from +Z. [All modes; Runtime|Test; pure/read; setter mutates]
	InitialAngularVelocity: number
	-- The velocity along world X and Y the body starts with, in metres per second. [All modes; Runtime|Test; pure/read; setter mutates]
	InitialLinearVelocity: vector
	-- Whether the 2D body is sleeping. [All modes; Runtime|Test; pure/read]
	IsSleeping: (self: RigidBody2D) -> boolean
	-- Physics layer name from project settings (the 3D world's layers). A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Layer: string
	-- How quickly linear velocity decays, per second. [All modes; Runtime|Test; pure/read; setter mutates]
	LinearDamping: number
	-- Mass in kilograms (Dynamic only). [All modes; Runtime|Test; pure/read; setter mutates]
	Mass: number
	-- The angular speed the body is clamped to, in radians per second. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxAngularVelocity: number
	-- The speed the body is clamped to, in metres per second. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxLinearVelocity: number
	-- Discrete or continuous (LinearCast) collision detection. [All modes; Runtime|Test; pure/read; setter mutates]
	MotionQuality: MotionQuality
	-- Set a Kinematic 2D target in the plane, the angle in degrees about +Z, for the next fixed step. [All modes; Runtime|Test; mutates]
	MoveKinematic: (self: RigidBody2D, position: vector, angle: number) -> ()
	-- Bounciness, from no bounce (0) to a perfectly elastic bounce (1); a contact uses the larger of both bodies' values. [All modes; Runtime|Test; pure/read; setter mutates]
	Restitution: number
	-- Set a Dynamic 2D body's angular velocity about +Z in radians per second. [All modes; Runtime|Test; mutates]
	SetAngularVelocity: (self: RigidBody2D, velocity: number) -> ()
	-- Set a Dynamic 2D body's linear velocity from the vector's X and Y. [All modes; Runtime|Test; mutates]
	SetLinearVelocity: (self: RigidBody2D, velocity: vector) -> ()
	-- Place a 2D body at once in the plane (the entity keeps its Z), the angle in degrees about +Z kept when omitted, preserving velocity and resetting interpolation. [All modes; Runtime|Test; mutates]
	Teleport: (self: RigidBody2D, position: vector, angle: number?) -> ()
	-- Static never moves; Kinematic is moved by script; Dynamic is simulated. [All modes; Runtime|Test; pure/read; setter mutates]
	Type: BodyType
	-- Wake a sleeping 2D body. [All modes; Runtime|Test; mutates]
	WakeUp: (self: RigidBody2D) -> ()
end

-- Attaches Behaviour scripts to the entity, an ordered list of slots.
declare extern type Script with
	-- The behaviours of the entity, in slot order; each slot is its own instance. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Scripts: {{Script: AssetRef?, Fields: {[string]: any}, ExecutionOrder: number}}
end

-- A sphere-shaped collider in the entity's space.
declare extern type SphereCollider with
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The sphere centre relative to the entity, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The sphere radius, in metres; at least 1 mm. [All modes; Runtime|Test; pure/read; setter mutates]
	Radius: number
end

-- A cone of light shining down the entity's local -Z axis.
declare extern type SpotLight with
	-- Whether the light casts soft shadows (shadow atlas budget applies). [All modes; Runtime|Test; pure/read; setter mutates]
	CastShadows: boolean
	-- The linear light colour. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- The angle from the axis inside which the light is at full strength, in degrees; below OuterConeAngle. [All modes; Runtime|Test; pure/read; setter mutates]
	InnerConeAngle: number
	-- The unitless brightness multiplied with Color. [All modes; Runtime|Test; pure/read; setter mutates]
	Intensity: number
	-- The angle from the axis at which the light fades to zero, in degrees. [All modes; Runtime|Test; pure/read; setter mutates]
	OuterConeAngle: number
	-- The distance at which the light's falloff reaches zero, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Range: number
	-- The radius of the emitting disc, in metres; widens highlights and shadow penumbrae. [All modes; Runtime|Test; pure/read; setter mutates]
	SourceRadius: number
end

-- Plays a clip of the SpriteRenderer's sprite sheet, one step per clip frame for its ticks, driven by the fixed step.
declare extern type SpriteAnimator with
	-- The name of the sheet's clip to play; empty plays nothing. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Clip: string
	-- Play the sheet's clip named clip from its first step, which shows at the next fixed step's SpriteAnimation phase. [All modes; Runtime|Test; mutates]
	Play: (self: SpriteAnimator, clip: string) -> ()
	-- Whether the clip advances each fixed step; a clip without Loop clears it after its last step and holds its last frame. [All modes; Runtime|Test; pure/read; setter mutates]
	Playing: boolean
	-- The index of the clip step shown. [All modes; Runtime|Test; pure/read; setter mutates]
	Step: number
	-- How many fixed steps the current clip step has been shown. [All modes; Runtime|Test; pure/read; setter mutates]
	StepTicks: number
	-- Stop advancing the clip; the frame shown stays. [All modes; Runtime|Test; mutates]
	Stop: (self: SpriteAnimator) -> ()
end

-- Draws a sprite: a sprite sheet frame or a texture in the entity's XY plane.
declare extern type SpriteRenderer with
	-- The linear colour every texel is multiplied with, with opacity in the fourth component. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- Whether the image is mirrored left to right about its pivot. [All modes; Runtime|Test; pure/read; setter mutates]
	FlipX: boolean
	-- Whether the image is mirrored top to bottom about its pivot. [All modes; Runtime|Test; pure/read; setter mutates]
	FlipY: boolean
	-- The index of the sheet's frame to draw; an index the sheet lacks draws nothing (SPRITE_FRAME_OUT_OF_RANGE). [All modes; Runtime|Test; pure/read; setter mutates]
	Frame: number
	-- Whether 2D lights light the sprite; an unlit sprite keeps its texels unchanged. [All modes; Runtime|Test; pure/read; setter mutates]
	Lit: boolean
	-- A lit sprite's tangent-space normal map, laid like its image; point lights then light it by the angle to the light. Null: flat. [All modes; Runtime|Test; pure/read; setter mutates]
	NormalMap: AssetRef?
	-- The draw order within the sorting layer: higher values draw over lower ones, equal values in canonical entity order. [All modes; Runtime|Test; pure/read; setter mutates]
	OrderInLayer: number
	-- How many texels of the image make one unit of the entity's local space; the sprite's size is its texels divided by this. [All modes; Runtime|Test; pure/read; setter mutates]
	PixelsPerUnit: number
	-- Set Frame to the index of the sprite sheet's frame named name; an error names the sheet when it has no such frame. [All modes; Runtime|Test; mutates]
	SetFrameByName: (self: SpriteRenderer, name: string) -> ()
	-- The sprite sheet whose frame Frame is drawn; null draws Texture whole. [All modes; Runtime|Test; pure/read; setter mutates]
	Sheet: AssetRef?
	-- The sorting layer name from project settings; later layers draw over earlier ones, and an undeclared name draws in the first. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	SortingLayer: string
	-- The texture drawn whole, pivoted at its centre, when Sheet is null; null draws the built-in white square. [All modes; Runtime|Test; pure/read; setter mutates]
	Texture: AssetRef?
end

-- An opaque coroutine handle owned by one script VM.
declare extern type TaskHandle with
end

-- Draws a text string on the screen or in the world.
declare extern type Text with
	-- How the lines are aligned to each other. [All modes; Runtime|Test; pure/read; setter mutates]
	Alignment: TextAlignment
	-- Screen space: the point of the viewport the text is placed at, normalized: (0, 0) is the top-left corner and (1, 1) the bottom-right, X to the right and Y down; (0.5, 0.5) is the centre. [All modes; Runtime|Test; pure/read; setter mutates]
	Anchor: vector
	-- Whether world-space text always faces the camera. [All modes; Runtime|Test; pure/read; setter mutates]
	Billboard: boolean
	-- The linear text colour with opacity in the fourth component. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- The font; null uses the default font. [All modes; Runtime|Test; pure/read; setter mutates]
	Font: AssetRef?
	-- Screen space: a further offset from the anchor in pixels at the 1080p reference resolution (scaled with the viewport's height), X to the right and Y down. [All modes; Runtime|Test; pure/read; setter mutates]
	Offset: vector
	-- The point of the text block that sits on the anchor (Screen space) or at the entity's origin (World space), normalized to the block: (0, 0) is its top-left corner and (1, 1) its bottom-right. [All modes; Runtime|Test; pure/read; setter mutates]
	Pivot: vector
	-- The em size. Screen space: pixels at the 1080p reference resolution, scaled by the viewport's height / 1080. World space: an em of Size / 100 metres (100 world-text pixels per metre) before the entity's scale, in the entity's local XY plane or facing the camera with Billboard, so the default 32 is 0.32 m. [All modes; Runtime|Test; pure/read; setter mutates]
	Size: number
	-- Whether the text is placed on the screen or in the world. [All modes; Runtime|Test; pure/read; setter mutates]
	Space: TextSpace
	-- The UTF-8 text; a line feed starts a new line. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Text: string
end

-- The solid cells of a tilemap as 2D colliders: each chunk's solid cells merged into boxes, one static compound without a RigidBody2D.
declare extern type TilemapCollider with
	-- Whether the collider only detects overlaps instead of colliding. [All modes; Runtime|Test; pure/read; setter mutates]
	IsTrigger: boolean
	-- The tilemap whose solid cells collide; null takes the entity's TilemapRenderer.Tilemap. [All modes; Runtime|Test; pure/read; setter mutates]
	Tilemap: AssetRef?
end

-- Draws a tilemap's tiles in the entity's XY plane, each tile as a sprite of its palette entry's sheet frame.
declare extern type TilemapRenderer with
	-- The linear colour every tile's texels are multiplied with, with opacity in the fourth component. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- Whether 2D lights light the tiles; unlit tiles keep their texels unchanged. [All modes; Runtime|Test; pure/read; setter mutates]
	Lit: boolean
	-- The draw order within the sorting layer, among the sprites: higher values draw over lower ones, equal values in canonical entity order. [All modes; Runtime|Test; pure/read; setter mutates]
	OrderInLayer: number
	-- How many texels of a tile's frame make one unit of the entity's local space, as a sprite's. [All modes; Runtime|Test; pure/read; setter mutates]
	PixelsPerUnit: number
	-- The sorting layer name from project settings; the tilemap sorts with the sprites, and an undeclared name draws in the first. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	SortingLayer: string
	-- The tilemap whose tiles are drawn; null draws nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	Tilemap: AssetRef?
end

-- Positions, rotates and scales the entity relative to its parent.
declare extern type Transform with
	-- Rotation relative to the parent as Euler angles in degrees, applied Z, then X, then Y. [All modes; Runtime|Test; pure/read; setter mutates]
	EulerAngles: vector
	-- World direction of local -Z. [All modes; Runtime|Test; pure/read]
	Forward: (self: Transform) -> vector
	-- Transform a world point into local space. [All modes; Runtime|Test; pure/read]
	InverseTransformPoint: (self: Transform, point: vector) -> vector
	-- Face the target using world up +Y by default; reject coincident or parallel directions. [All modes; Runtime|Test; mutates]
	LookAt: (self: Transform, target: vector, up: vector?) -> ()
	-- The rendered world position, interpolated between fixed steps, in metres (read-only). [All modes; Runtime|Test; pure/read]
	RenderPosition: vector
	-- The rendered world rotation, interpolated between fixed steps (read-only). [All modes; Runtime|Test; pure/read]
	RenderRotation: Quat
	-- World direction of local +X. [All modes; Runtime|Test; pure/read]
	Right: (self: Transform) -> vector
	-- Rotate using deterministic Z-X-Y Euler degrees in Local space by default. [All modes; Runtime|Test; mutates]
	Rotate: (self: Transform, eulerDegrees: vector, space: Space?) -> ()
	-- Rotation relative to the parent: a unit quaternion written [x, y, z, w]. [All modes; Runtime|Test; pure/read; setter mutates]
	Rotation: Quat
	-- Scale along the local axes; each component's magnitude is at least 0.0001. [All modes; Runtime|Test; pure/read; setter mutates]
	Scale: vector
	-- Set optional world pose and reset interpolation. [All modes; Runtime|Test; mutates]
	Teleport: (self: Transform, position: vector?, rotation: Quat?) -> ()
	-- Rotate a direction into world space without translation or scale. [All modes; Runtime|Test; pure/read]
	TransformDirection: (self: Transform, direction: vector) -> vector
	-- Transform a point using world translation, rotation and scale. [All modes; Runtime|Test; pure/read]
	TransformPoint: (self: Transform, point: vector) -> vector
	-- Translate in Local orientation by default, or World axes. [All modes; Runtime|Test; mutates]
	Translate: (self: Transform, delta: vector, space: Space?) -> ()
	-- Position relative to the parent, in metres. [All modes; Runtime|Test; pure/read; setter mutates]
	Translation: vector
	-- World direction of local +Y. [All modes; Runtime|Test; pure/read]
	Up: (self: Transform) -> vector
	-- Position in world space, in metres; writing it moves the entity through its parent. [All modes; Runtime|Test; pure/read; setter mutates]
	WorldPosition: vector
	-- Rotation in world space; writing it rotates the entity through its parent. [All modes; Runtime|Test; pure/read; setter mutates]
	WorldRotation: Quat
	-- Approximate scale in world space (read-only: a sheared parent chain has no exact one). [All modes; Runtime|Test; pure/read]
	WorldScale: vector
end

-- A UI button: a pointer click or Submit while focused raises OnUiClick.
declare extern type UiButton with
	-- Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it. [All modes; Runtime|Test; pure/read; setter mutates]
	Focusable: boolean
	-- Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event. [All modes; Runtime|Test; pure/read; setter mutates]
	Interactable: boolean
end

-- The root of a UI tree, laid out over the whole scene image in scene pixels and drawn after the game's text, or, in World mode, laid out in its own canvas pixels and drawn as a rectangle in the world.
declare extern type UiCanvas with
	-- The UI widget keyboard and gamepad navigation focuses first while nothing has focus; null: navigation starts only from a pointer press or Ui.SetFocus. [All modes; Runtime|Test; pure/read; setter mutates]
	DefaultFocus: Entity?
	-- The font of the canvas's texts whose own Font is null; null uses the default font. [All modes; Runtime|Test; pure/read; setter mutates]
	Font: AssetRef?
	-- Rounds every element's corners to whole scene pixels, so images and clips land on pixel corners. [All modes; Runtime|Test; pure/read; setter mutates]
	PixelSnap: boolean
	-- A World canvas's canvas pixels per world unit: its rectangle is WorldWidth x WorldHeight over this, centred on the entity. [All modes; Runtime|Test; pure/read; setter mutates]
	PixelsPerUnit: number
	-- MatchHeight: the canvas's height in canvas units, so one unit is the scene's height / ReferenceHeight scene pixels. [All modes; Runtime|Test; pure/read; setter mutates]
	ReferenceHeight: number
	-- Screen draws the canvas over the scene image; World draws it as a rectangle in the world at the entity's transform, depth-tested against the scene. [All modes; Runtime|Test; pure/read; setter mutates]
	RenderMode: UiCanvasRenderMode
	-- ConstantPixelSize: the scene pixels per canvas unit. [All modes; Runtime|Test; pure/read; setter mutates]
	ScaleFactor: number
	-- How canvas units map to scene pixels. [All modes; Runtime|Test; pure/read; setter mutates]
	ScaleMode: UiScaleMode
	-- Canvases draw in ascending SortOrder, ties in entity order; a later canvas draws over earlier ones. [All modes; Runtime|Test; pure/read; setter mutates]
	SortOrder: number
	-- The state tints of the canvas's widgets (buttons, toggles, sliders and scroll views). [All modes; Runtime|Test; pure/read; setter mutates]
	Theme: {Normal: Color, Hovered: Color, Pressed: Color, Focused: Color, Disabled: Color}
	-- A World canvas's height in canvas pixels (its layout's scene resolution); Screen canvases ignore it. [All modes; Runtime|Test; pure/read; setter mutates]
	WorldHeight: number
	-- A World canvas's width in canvas pixels (its layout's scene resolution); Screen canvases ignore it. [All modes; Runtime|Test; pure/read; setter mutates]
	WorldWidth: number
end

-- Draws a sprite sheet frame, or a plain colour, over a UI element's rectangle.
declare extern type UiImage with
	-- Sliced: the canvas units each border texel takes. [All modes; Runtime|Test; pure/read; setter mutates]
	BorderScale: number
	-- The linear tint multiplied with the frame's texels, with opacity in the fourth component. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- How the frame's texture is sampled. [All modes; Runtime|Test; pure/read; setter mutates]
	Filter: UiImageFilter
	-- The index of the frame in the sheet's Frames; a frame the sheet lacks draws nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	Frame: number
	-- Whether the image catches the pointer for its nearest widget; false lets the pointer pass through it. [All modes; Runtime|Test; pure/read; setter mutates]
	RaycastTarget: boolean
	-- The sprite sheet whose frame is drawn; null draws a plain rectangle of Color. [All modes; Runtime|Test; pure/read; setter mutates]
	Sheet: AssetRef?
	-- Whether the frame stretches or keeps its 9-slice borders. [All modes; Runtime|Test; pure/read; setter mutates]
	Type: UiImageType
end

-- A container that places its children's rectangles in a stack, a grid or a flexible row or column.
declare extern type UiLayout with
	-- Grid: the size of each cell in canvas units. [All modes; Runtime|Test; pure/read; setter mutates]
	CellSize: vector
	-- Grid: the cells per row (Horizontal) or per column (Vertical); 0 fits as many as the container holds. [All modes; Runtime|Test; pure/read; setter mutates]
	Columns: number
	-- Where each child sits across the main axis. [All modes; Runtime|Test; pure/read; setter mutates]
	CrossAlignment: UiCrossAlignment
	-- The main axis the children follow. [All modes; Runtime|Test; pure/read; setter mutates]
	Direction: UiLayoutDirection
	-- How the children are placed. [All modes; Runtime|Test; pure/read; setter mutates]
	Kind: UiLayoutKind
	-- Where the children sit along the main axis when they do not fill it. [All modes; Runtime|Test; pure/read; setter mutates]
	MainAlignment: UiMainAlignment
	-- Insets of the container's rectangle that the children are placed in. [All modes; Runtime|Test; pure/read; setter mutates]
	Padding: {Left: number, Top: number, Right: number, Bottom: number}
	-- The gaps between children, lines and cells along x and y, in canvas units. [All modes; Runtime|Test; pure/read; setter mutates]
	Spacing: vector
	-- Flex: whether a child that does not fit the line starts a new line. [All modes; Runtime|Test; pure/read; setter mutates]
	Wrap: boolean
end

-- How a UI element takes part in its parent's layout container: flex weights, a minimum size, or none.
declare extern type UiLayoutElement with
	-- Flex: the element's share of the main axis's free space. [All modes; Runtime|Test; pure/read; setter mutates]
	Grow: number
	-- Whether the element keeps its own placement and takes no space in the container. [All modes; Runtime|Test; pure/read; setter mutates]
	IgnoreLayout: boolean
	-- The size the element never goes below in a Stack or Flex container, in canvas units. [All modes; Runtime|Test; pure/read; setter mutates]
	MinSize: vector
	-- Flex: the element's share, times its preferred size, of the main axis's overflow. [All modes; Runtime|Test; pure/read; setter mutates]
	Shrink: number
end

-- A UI element's rectangle in its parent's (or its canvas's), in canvas units, x right and y down.
declare extern type UiRect with
	-- The anchor rectangle's bottom-right corner, normalized in the parent's rectangle; equal to AnchorMin for a fixed-size element. [All modes; Runtime|Test; pure/read; setter mutates]
	AnchorMax: vector
	-- The anchor rectangle's top-left corner, normalized in the parent's rectangle: (0, 0) its top-left corner, (1, 1) its bottom-right. [All modes; Runtime|Test; pure/read; setter mutates]
	AnchorMin: vector
	-- Whether the element's descendants are clipped to its rectangle. [All modes; Runtime|Test; pure/read; setter mutates]
	ClipChildren: boolean
	-- The point of the element that Position places, normalized to the element: (0, 0) its top-left corner, (1, 1) its bottom-right. [All modes; Runtime|Test; pure/read; setter mutates]
	Pivot: vector
	-- The pivot's offset from its point in the anchor rectangle, in canvas units, x right and y down. [All modes; Runtime|Test; pure/read; setter mutates]
	Position: vector
	-- The element's size in canvas units, added to the anchor rectangle's size (negative values inset stretched anchors); a container's child takes it as its preferred size. [All modes; Runtime|Test; pure/read; setter mutates]
	Size: vector
end

-- A UI scroll view: clips its descendants and moves its Content by the scroll position, which the wheel, a drag and focus change.
declare extern type UiScrollView with
	-- The UI element below the scroll view that scrolls; null scrolls nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	Content: Entity?
	-- Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it. [All modes; Runtime|Test; pure/read; setter mutates]
	Focusable: boolean
	-- Whether the content scrolls along x. [All modes; Runtime|Test; pure/read; setter mutates]
	Horizontal: boolean
	-- Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event. [All modes; Runtime|Test; pure/read; setter mutates]
	Interactable: boolean
	-- Canvas units the content is moved left (x) and up (y), read clamped to the content's overflow. [All modes; Runtime|Test; pure/read; setter mutates]
	ScrollPosition: vector
	-- Canvas units per wheel step and per navigation press. [All modes; Runtime|Test; pure/read; setter mutates]
	ScrollStep: number
	-- Whether the content scrolls along y. [All modes; Runtime|Test; pure/read; setter mutates]
	Vertical: boolean
end

-- A UI slider: the pointer drags its value between Min and Max and the directions along it step it, raising OnUiValueChanged.
declare extern type UiSlider with
	-- Horizontal puts Min at the left edge; Vertical puts it at the bottom edge. [All modes; Runtime|Test; pure/read; setter mutates]
	Direction: UiLayoutDirection
	-- A UI element below the slider stretched from the Min edge to the value; null for none. [All modes; Runtime|Test; pure/read; setter mutates]
	Fill: Entity?
	-- Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it. [All modes; Runtime|Test; pure/read; setter mutates]
	Focusable: boolean
	-- A UI element below the slider placed at the value; null for none. [All modes; Runtime|Test; pure/read; setter mutates]
	Handle: Entity?
	-- Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event. [All modes; Runtime|Test; pure/read; setter mutates]
	Interactable: boolean
	-- The value at the end edge (right, or top when Vertical). [All modes; Runtime|Test; pure/read; setter mutates]
	Max: number
	-- The value at the start edge (left, or bottom when Vertical); below Max. [All modes; Runtime|Test; pure/read; setter mutates]
	Min: number
	-- The change per navigation press along the slider; 0 is a tenth of the range. With WholeNumbers it is rounded to a whole number of at least 1. [All modes; Runtime|Test; pure/read; setter mutates]
	Step: number
	-- The value, read clamped to Min and Max (and rounded with WholeNumbers). [All modes; Runtime|Test; pure/read; setter mutates]
	Value: number
	-- Whether the value is rounded to whole numbers. [All modes; Runtime|Test; pure/read; setter mutates]
	WholeNumbers: boolean
end

-- Draws text in a UI element's rectangle, wrapped and aligned in it.
declare extern type UiText with
	-- How each line is aligned within the rectangle's width. [All modes; Runtime|Test; pure/read; setter mutates]
	Alignment: TextAlignment
	-- The linear text colour with opacity in the fourth component. [All modes; Runtime|Test; pure/read; setter mutates]
	Color: Color
	-- The font; null uses its canvas's Font, or the default font when the canvas names none. [All modes; Runtime|Test; pure/read; setter mutates]
	Font: AssetRef?
	-- Multiplies the font's line height. [All modes; Runtime|Test; pure/read; setter mutates]
	LineSpacing: number
	-- Whether the text catches the pointer for its nearest widget; false lets the pointer pass through it. [All modes; Runtime|Test; pure/read; setter mutates]
	RaycastTarget: boolean
	-- The em size in canvas units. [All modes; Runtime|Test; pure/read; setter mutates]
	Size: number
	-- The UTF-8 text; a line feed starts a new line. Scripts read localized strings with Loc.Get. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Text: string
	-- Where the text block sits in the rectangle's height. [All modes; Runtime|Test; pure/read; setter mutates]
	VerticalAlignment: UiTextVerticalAlignment
	-- Whether lines break at spaces to fit the rectangle's width. [All modes; Runtime|Test; pure/read; setter mutates]
	Wrap: boolean
end

-- A UI text field: while focused, typed characters insert at its caret and the editing keys edit it, raising OnUiTextChanged; Enter raises OnUiSubmit.
declare extern type UiTextInput with
	-- The caret, in codepoints from the start of Text, read clamped to Text's length; typing and the editing keys move it. [All modes; Runtime|Test; pure/read; setter mutates]
	Caret: number
	-- Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it. [All modes; Runtime|Test; pure/read; setter mutates]
	Focusable: boolean
	-- Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event. [All modes; Runtime|Test; pure/read; setter mutates]
	Interactable: boolean
	-- The most codepoints typing reaches; characters typed past it are dropped. [All modes; Runtime|Test; pure/read; setter mutates]
	MaxLength: number
	-- A UI element below the input shown only while its Text is empty (a hint); null for none. [All modes; Runtime|Test; pure/read; setter mutates]
	Placeholder: Entity?
	-- The field's UTF-8 text: typing inserts at the caret; a script's write raises no OnUiTextChanged. A value a script writes holds at most 4096 bytes of text. [All modes; Runtime|Test; pure/read; setter mutates]
	Text: string
	-- A UI element below the input whose UiText shows the input's Text, with the caret while focused; null shows nothing. [All modes; Runtime|Test; pure/read; setter mutates]
	TextElement: Entity?
end

-- A UI toggle: a pointer click or Submit while focused flips IsOn and raises OnUiValueChanged; Graphic shows while it is on.
declare extern type UiToggle with
	-- Whether the widget takes keyboard and gamepad focus, from navigation, Ui.SetFocus or a pointer press; false: a press still activates it. [All modes; Runtime|Test; pure/read; setter mutates]
	Focusable: boolean
	-- A UI element below the toggle (its check mark) that is drawn and hit only while the toggle is on; null for none. [All modes; Runtime|Test; pure/read; setter mutates]
	Graphic: Entity?
	-- Whether the widget takes the pointer, focus and input; false shows its Disabled tint and raises no event. [All modes; Runtime|Test; pure/read; setter mutates]
	Interactable: boolean
	-- Whether the toggle is on. [All modes; Runtime|Test; pure/read; setter mutates]
	IsOn: boolean
end

-- Application identity, window state and deferred quit requests.
declare Application: {
	-- Returns the host platform name. [All modes; Runtime|Test; pure/read]
	GetPlatform: () -> string,
	-- Returns the running engine version. [All modes; Runtime|Test; pure/read]
	GetVersion: () -> string,
	-- Returns the size of the session's view in view coordinates as vector(width, height, 0): the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions. [All modes; Runtime|Test; pure/read]
	GetWindowSize: () -> vector,
	-- True in the editor; false in the standalone Runtime. Gameplay that branches on it splits the editor's and the export's state hashes, which the games suite compares. [All modes; Runtime|Test; pure/read]
	IsEditor: () -> boolean,
	-- Returns the current native-window focus state. Nondeterministic: it follows the host window in every session, lockstep, recording, replay and test sessions included, so never base gameplay on it. [All modes; Runtime|Test; pure/read]
	IsFocused: () -> boolean,
	-- Returns whether the host has no native window. [All modes; Runtime|Test; pure/read]
	IsHeadless: () -> boolean,
	-- Requests exit (default 0): stops editor Play, exits Runtime, or records quit and ends the current test suite. Never destroys the executing VM inline. [All modes; Runtime|Test; mutates]
	Quit: (exitCode: number?) -> (),
}

-- Resolves and loads project or built-in assets through the active asset manager.
declare Assets: {
	-- Returns the readable reference path, or an empty string for an unknown handle. [All modes; Runtime|Test; pure/read]
	GetPath: (asset: AssetRef) -> string,
	-- Returns the asset type name, or None for an unknown handle. [All modes; Runtime|Test; pure/read]
	GetType: (asset: AssetRef) -> string,
	-- Returns whether the handle is currently registered; nil is false. [All modes; Runtime|Test; pure/read]
	IsValid: (asset: AssetRef?) -> boolean,
	-- Loads a path or hexadecimal reference; missing references return nil and loading failures raise a located error. The import of an asset that is not cached yet does not count toward the callback budget. [All modes; Runtime|Test; pure/read]
	Load: (reference: string) -> AssetRef?,
}

-- Session-scoped mixing and one-shot playback.
declare Audio: {
	-- Read the session group gain. [All modes; Runtime|Test; pure/read]
	GetGroupVolume: (group: AudioGroup) -> number,
	-- Play a non-spatial one-shot when position is nil, otherwise spatial; volume defaults 1 and group Sfx. At most 1024 one-shots start per frame; a later call in the frame raises an error. [All modes; Runtime|Test; mutates]
	PlayOneShot: (clip: AssetRef, position: vector?, volume: number?, group: AudioGroup?) -> (),
	-- Set nonnegative session group gain; the prior mix is restored when the audio system stops. [All modes; Runtime|Test; mutates]
	SetGroupVolume: (group: AudioGroup, volume: number) -> (),
}

-- Depth-tested world-space debug primitives with white color and one-extraction duration by default.
declare Debug: {
	-- Pauses editor Play; Runtime does nothing; test runs count the call without pausing in every mode. [All modes; Runtime|Test; mutates]
	Break: () -> (),
	-- Adds an arrow with a 0.1 metre head. [All modes; Runtime|Test; mutates]
	DrawArrow: (from: vector, to: vector, color: Color?, duration: number?) -> (),
	-- Adds a box with positive half extents and a normalized rotation, identity by default. [All modes; Runtime|Test; mutates]
	DrawBox: (center: vector, halfExtents: vector, rotation: Quat?, color: Color?, duration: number?) -> (),
	-- Adds a line segment; duration is nonnegative simulation seconds. [All modes; Runtime|Test; mutates]
	DrawLine: (a: vector, b: vector, color: Color?, duration: number?) -> (),
	-- Adds a ray segment whose length is the nonzero direction vector's magnitude. [All modes; Runtime|Test; mutates]
	DrawRay: (origin: vector, direction: vector, color: Color?, duration: number?) -> (),
	-- Adds a sphere with positive radius. [All modes; Runtime|Test; mutates]
	DrawSphere: (center: vector, radius: number, color: Color?, duration: number?) -> (),
	-- Adds UTF-8 text of at most 1024 bytes at a world position with the default 16-pixel size. [All modes; Runtime|Test; mutates]
	DrawText: (position: vector, text: string, color: Color?, duration: number?) -> (),
}

-- Persistent behaviour field schemas.
declare Field: {
	-- Declare an empty array whose element keeps its complete nested field schema. [All modes; LoadTime|Runtime|Test; pure/read]
	Array: (element: FieldDescriptor) -> FieldDescriptor,
	-- Declare an asset reference restricted to the named AssetType (Scene, Prefab, Mesh, Material, Texture, Environment, AudioClip for imported audio and .sfx sound effects, Script, Font, Replay, SpriteSheet, Tilemap or StringTable), with a null default; any other name fails the import. [All modes; LoadTime|Runtime|Test; pure/read]
	Asset: (type: string) -> FieldDescriptor,
	-- Declare a boolean; the omitted default is false. [All modes; LoadTime|Runtime|Test; pure/read]
	Bool: (default: boolean?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare a linear RGBA color; the omitted default is white. [All modes; LoadTime|Runtime|Test; pure/read]
	Color: (default: Color?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare an Entity reference with a null default. [All modes; LoadTime|Runtime|Test; pure/read]
	Entity: () -> FieldDescriptor,
	-- Declare ordered unique string choices; the omitted default is the first choice. [All modes; LoadTime|Runtime|Test; pure/read]
	Enum: (values: {string}, default: string?) -> FieldDescriptor,
	-- Declare an exact signed 32-bit integer; the omitted default is zero. [All modes; LoadTime|Runtime|Test; pure/read]
	Integer: (default: number?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare a finite f32 number; the omitted default is zero. [All modes; LoadTime|Runtime|Test; pure/read]
	Number: (default: number?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare a finite nonzero quaternion; the omitted default is identity. [All modes; LoadTime|Runtime|Test; pure/read]
	Quat: (default: Quat?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare a string; the omitted default is empty. [All modes; LoadTime|Runtime|Test; pure/read]
	String: (default: string?, options: FieldOptions?) -> FieldDescriptor,
	-- Declare a native three-component vector; the omitted default is zero. [All modes; LoadTime|Runtime|Test; pure/read]
	Vector: (default: vector?, options: FieldOptions?) -> FieldDescriptor,
}

-- Queries the current step or frame latch, including injected input; gamepad indexes are zero-based.
declare Input: {
	-- Returns a configured action's combined axis value. [All modes; Runtime|Test; pure/read]
	GetAxis: (name: string) -> number,
	-- Returns the canonical cursor mode name. [All modes; Runtime|Test; pure/read]
	GetCursorMode: () -> CursorMode,
	-- Returns an engine-convention axis, or zero for a disconnected pad. [All modes; Runtime|Test; pure/read]
	GetGamepadAxis: (index: number, axis: GamepadAxis) -> number,
	-- Returns accumulated cursor movement in this phase. [All modes; Runtime|Test; pure/read]
	GetMouseDelta: () -> vector,
	-- Returns the cursor position in view coordinates as vector(x, y, 0): the pixels of the session's view, origin top-left (the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions). [All modes; Runtime|Test; pure/read]
	GetMousePosition: () -> vector,
	-- Returns accumulated scroll, right-positive and up-positive. [All modes; Runtime|Test; pure/read]
	GetScrollDelta: () -> vector,
	-- Returns the characters typed in the latched view as UTF-8: in the fixed step (OnFixedUpdate, tasks, physics and UI callbacks) the tick's committed characters, in the frame phase (OnUpdate, OnLateUpdate) those of every tick since the previous frame; empty when none were typed. [All modes; Runtime|Test; pure/read]
	GetText: () -> string,
	-- Returns a configured action's held state; an unknown name raises INPUT_UNKNOWN_ACTION. [All modes; Runtime|Test; pure/read]
	IsActionDown: (name: string) -> boolean,
	-- Returns whether a configured action was pressed since the previous latch of the current phase: in OnFixedUpdate since the previous tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises INPUT_UNKNOWN_ACTION. [All modes; Runtime|Test; pure/read]
	IsActionPressed: (name: string) -> boolean,
	-- Returns whether a configured action was released since the previous latch of the current phase: in OnFixedUpdate since the previous tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises INPUT_UNKNOWN_ACTION. [All modes; Runtime|Test; pure/read]
	IsActionReleased: (name: string) -> boolean,
	-- Returns a gamepad button's held state. [All modes; Runtime|Test; pure/read]
	IsGamepadButtonDown: (index: number, button: GamepadButton) -> boolean,
	-- Returns whether gamepad 0 through 3 is connected. [All modes; Runtime|Test; pure/read]
	IsGamepadConnected: (index: number) -> boolean,
	-- Returns the held key state in this phase. [All modes; Runtime|Test; pure/read]
	IsKeyDown: (key: Key) -> boolean,
	-- Returns this phase's press edge; repeated keys do not add an edge. [All modes; Runtime|Test; pure/read]
	IsKeyPressed: (key: Key) -> boolean,
	-- Returns this phase's release edge. [All modes; Runtime|Test; pure/read]
	IsKeyReleased: (key: Key) -> boolean,
	-- Returns the held mouse-button state. [All modes; Runtime|Test; pure/read]
	IsMouseButtonDown: (button: MouseButton) -> boolean,
	-- Returns this phase's mouse-button press edge. [All modes; Runtime|Test; pure/read]
	IsMouseButtonPressed: (button: MouseButton) -> boolean,
	-- Returns this phase's mouse-button release edge. [All modes; Runtime|Test; pure/read]
	IsMouseButtonReleased: (button: MouseButton) -> boolean,
	-- Changes cursor visibility/capture; refused during read-only evaluation. [All modes; Runtime|Test; mutates]
	SetCursorMode: (mode: CursorMode) -> (),
}

-- Localized strings from string tables (.stringtable assets) in the project's locale (Localization.Locale): a table's locale equal to it, else its language's, else the table's first (fallback) locale; an entry without a string for that locale gives the fallback's.
declare Loc: {
	-- Returns the string of key in the locale. A key the table lacks, an asset that is not a string table, or a table that fails to load raises a located error naming them (with the closest keys as a hint). [All modes; Runtime|Test; pure/read]
	Get: (table: AssetRef, key: string) -> string,
	-- Returns the project's locale (Localization.Locale) that lookups use; empty when it is unset, so every table reads its fallback locale. [All modes; Runtime|Test; pure/read]
	GetLocale: () -> string,
	-- Returns whether the string table has the key. An asset that is not a string table, or a table that fails to load, raises a located error naming it, as in Get. [All modes; Runtime|Test; pure/read]
	Has: (table: AssetRef, key: string) -> boolean,
}

-- Script-channel logging with authored source, line, entity and simulation tick; arguments use tostring and tab separators. An entry keeps at most 8192 bytes (longer text is truncated and marked), and one VM writes at most 1024 entries per frame, dropping and counting the rest.
declare Log: {
	-- Writes an Error entry without throwing or disabling the script instance. [All modes; Runtime|Test; pure/read]
	Error: (...any) -> (),
	-- Writes an Info entry and captures evaluation output. The print global aliases this registered function. [All modes; Runtime|Test; pure/read]
	Info: (...any) -> (),
	-- Writes a Trace entry; compiled out of Dist. [All modes; Runtime|Test; pure/read]
	Trace: (...any) -> (),
	-- Writes a Warn entry without raising a script error. [All modes; Runtime|Test; pure/read]
	Warn: (...any) -> (),
}

-- Deterministic interpolation, smoothing and angle utilities. All arguments and results must be finite.
declare Math: {
	-- Compares within epsilon times max(1, abs(a), abs(b)); epsilon defaults to 0.000001 and must be nonnegative. [All modes; LoadTime|Runtime|Test; pure/read]
	Approximately: (a: number, b: number, epsilon: number?) -> boolean,
	-- Clamps to ordered inclusive bounds; vector bounds apply component-wise. Reversed bounds are errors. [All modes; LoadTime|Runtime|Test; pure/read]
	Clamp: ((value: number, minimum: number, maximum: number) -> number) & ((value: vector, minimum: vector, maximum: vector) -> vector),
	-- Clamps a number or every vector component to [0, 1]. [All modes; LoadTime|Runtime|Test; pure/read]
	Clamp01: ((value: number) -> number) & ((value: vector) -> vector),
	-- Multiplier from degrees to radians. [All modes; LoadTime|Runtime|Test; pure/read]
	Deg2Rad: number,
	-- Returns the shortest signed difference in degrees in (-180, 180], choosing +180 for a half turn. [All modes; LoadTime|Runtime|Test; pure/read]
	DeltaAngle: (current: number, target: number) -> number,
	-- Returns the clamped [0, 1] fraction along a to b, including descending intervals; equal endpoints return zero. [All modes; LoadTime|Runtime|Test; pure/read]
	InverseLerp: (a: number, b: number, value: number) -> number,
	-- Interpolates with t clamped to [0, 1]; t=0 returns a and t=1 returns b. [All modes; LoadTime|Runtime|Test; pure/read]
	Lerp: ((a: number, b: number, t: number) -> number) & ((a: vector, b: vector, t: number) -> vector),
	-- Follows the shortest angular path in degrees with t clamped to [0, 1]; preserves current's full turns. [All modes; LoadTime|Runtime|Test; pure/read]
	LerpAngle: (current: number, target: number, t: number) -> number,
	-- Moves at most nonnegative maxDelta toward target without overshooting; vectors use Euclidean distance. [All modes; LoadTime|Runtime|Test; pure/read]
	MoveTowards: ((current: number, target: number, maxDelta: number) -> number) & ((current: vector, target: vector, maxDelta: number) -> vector),
	-- Pi, rounded to engine float precision. [All modes; LoadTime|Runtime|Test; pure/read]
	Pi: number,
	-- Reflects a repeating value into [0, length] with period 2*length; length must be positive. [All modes; LoadTime|Runtime|Test; pure/read]
	PingPong: (value: number, length: number) -> number,
	-- Multiplier from radians to degrees. [All modes; LoadTime|Runtime|Test; pure/read]
	Rad2Deg: number,
	-- Maps the clamped fraction along a nonempty source interval to the target interval; either may descend. [All modes; LoadTime|Runtime|Test; pure/read]
	Remap: (value: number, sourceMinimum: number, sourceMaximum: number, targetMinimum: number, targetMaximum: number) -> number,
	-- Wraps either sign of value into [0, length); length must be positive. [All modes; LoadTime|Runtime|Test; pure/read]
	Repeat: (value: number, length: number) -> number,
	-- Returns -1, 0 or 1; both signed zeros return zero. [All modes; LoadTime|Runtime|Test; pure/read]
	Sign: (value: number) -> number,
	-- Exact critically damped spring step, component-wise for vectors, returning value and velocity without crossing the target. smoothTime must be positive (effective minimum 0.0001 seconds), dt nonnegative; zero dt preserves both inputs. [All modes; LoadTime|Runtime|Test; pure/read]
	SmoothDamp: ((current: number, target: number, velocity: number, smoothTime: number, dt: number) -> (number, number)) & ((current: vector, target: vector, velocity: vector, smoothTime: number, dt: number) -> (vector, vector)),
	-- Interpolates from a to b using 3t squared minus 2t cubed, with t clamped to [0, 1]. [All modes; LoadTime|Runtime|Test; pure/read]
	SmoothStep: (a: number, b: number, t: number) -> number,
}

-- Queries and gravity for the active scene's physics system.
declare Physics: {
	-- World AABB of the body's complete compound, or nil if no body is owned. [All modes; Runtime|Test; pure/read]
	GetBodyBounds: (entity: Entity) -> (vector?, vector?),
	-- World AABB of this entity's collider sub-shapes, or nil if absent. [All modes; Runtime|Test; pure/read]
	GetColliderBounds: (entity: Entity) -> (vector?, vector?),
	-- Current world gravity. [All modes; Runtime|Test; pure/read]
	GetGravity: () -> vector,
	-- Combine named project layers; omitted query masks include every layer. [All modes; Runtime|Test; pure/read]
	LayerMask: (...string) -> number,
	-- Unique overlapping collider entities sorted by UUID; default identity rotation. [All modes; Runtime|Test; pure/read]
	OverlapBox: (center: vector, halfExtents: vector, rotation: Quat?, layerMask: number?) -> {Entity},
	-- Unique overlapping collider entities sorted by UUID. [All modes; Runtime|Test; pure/read]
	OverlapSphere: (center: vector, radius: number, layerMask: number?) -> {Entity},
	-- Closest hit, including sensors; normalize direction and reject invalid finite ranges. [All modes; Runtime|Test; pure/read]
	Raycast: (origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?,
	-- All hits sorted by distance then collider UUID. [All modes; Runtime|Test; pure/read]
	RaycastAll: (origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit},
	-- Set finite gravity within physics bounds for the session. [All modes; Runtime|Test; mutates]
	SetGravity: (gravity: vector) -> (),
	-- Closest swept sphere hit; radius obeys collider size bounds. [All modes; Runtime|Test; pure/read]
	SphereCast: (origin: vector, radius: number, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?,
}

-- Queries and gravity for the active scene's 2D physics world, in the XY plane.
declare Physics2D: {
	-- Current 2D world gravity (Z is 0). [All modes; Runtime|Test; pure/read]
	GetGravity: () -> vector,
	-- Unique overlapping 2D collider entities sorted by UUID; the box rotated by angle degrees about +Z (0 by default). [All modes; Runtime|Test; pure/read]
	OverlapBox: (center: vector, halfExtents: vector, angle: number?, layerMask: number?) -> {Entity},
	-- Unique overlapping 2D collider entities sorted by UUID; radius obeys collider size bounds. [All modes; Runtime|Test; pure/read]
	OverlapCircle: (center: vector, radius: number, layerMask: number?) -> {Entity},
	-- Closest 2D hit, including sensors; vectors in the plane (Z ignored, results with Z 0). [All modes; Runtime|Test; pure/read]
	Raycast: (origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> RaycastHit?,
	-- All 2D hits sorted by distance then collider UUID. [All modes; Runtime|Test; pure/read]
	RaycastAll: (origin: vector, direction: vector, maxDistance: number, layerMask: number?) -> {RaycastHit},
	-- Set the 2D world's gravity from the vector's X and Y for the session. [All modes; Runtime|Test; mutates]
	SetGravity: (gravity: vector) -> (),
}

-- Deterministic shared simulation randomness. Invalid arguments never advance the stream.
declare Random: {
	-- True with probability in [0,1], default one half. [All modes; Runtime|Test; mutates]
	Bool: (probability: number?) -> boolean,
	-- Chooses from a nonempty dense one-based array; frozen arrays may be read. [All modes; Runtime|Test; mutates]
	Choice: <T>(array: {T}) -> T,
	-- Uniform integer in inclusive bounds, each within the exact-number integer range. [All modes; Runtime|Test; mutates]
	Integer: (min: number, max: number) -> number,
	-- Creates an independent local stream; allowed in read-only evaluation. Its state is a script value outside the state hash, and a constant seed repeats the same sequence in every session: seed it from Random.Integer (Random.New(Random.Integer(0, 2147483647))) to follow the session seed. Not available at a module's top level. [All modes; Runtime|Test; pure/read]
	New: (seed: number) -> RandomGenerator,
	-- Uniform in [min,max); defaults to [0,1), one argument is the maximum. Equal bounds still consume one draw. [All modes; Runtime|Test; mutates]
	Number: (min: number?, max: number?) -> number,
	-- Reseeds with a nonnegative exact integer no greater than 2^53-1. [All modes; Runtime|Test; mutates]
	Seed: (seed: number) -> (),
	-- Fisher-Yates shuffle in place, returning the same array. Rejects holes, dictionary keys and frozen tables. [All modes; Runtime|Test; mutates]
	Shuffle: <T>(array: {T}) -> {T},
	-- Uniform unit direction using two draws and deterministic trigonometry. [All modes; Runtime|Test; mutates]
	UnitVector: () -> vector,
}

-- Active-scene operations; searches return canonical order and never expose pending destruction.
declare Scene: {
	-- Creates immediately with a seeded ID and default name Entity (a name is at most 256 bytes). Runs OnCreate before returning; use IsValid if it may destroy itself. [All modes; Runtime|Test; mutates]
	CreateEntity: (name: string?, parent: Entity?) -> Entity,
	-- Marks the entity and descendants immediately; the session performs lifecycle teardown and destruction at its safe point. [All modes; Runtime|Test; mutates]
	Destroy: (entity: Entity) -> (),
	-- Returns all matching names in canonical scene order. [All modes; Runtime|Test; pure/read]
	FindAllByName: (name: string) -> {Entity},
	-- Returns all tagged entities in canonical scene order. [All modes; Runtime|Test; pure/read]
	FindAllByTag: (tag: string) -> {Entity},
	-- Returns every entity carrying a script-visible component; unknown names raise a located error. [All modes; Runtime|Test; pure/read]
	FindAllWithComponent: (name: string) -> {Entity},
	-- Finds an exact 16-digit hexadecimal UUID; malformed IDs error, absent entities return nil. [All modes; Runtime|Test; pure/read]
	FindByID: (id: string) -> Entity?,
	-- Returns the first matching name in canonical scene order, or nil. [All modes; Runtime|Test; pure/read]
	FindByName: (name: string) -> Entity?,
	-- Resolves an escaped absolute hierarchy path; missing, malformed or ambiguous paths return nil. [All modes; Runtime|Test; pure/read]
	FindByPath: (path: string) -> Entity?,
	-- Returns the first tagged entity in canonical scene order, or nil. [All modes; Runtime|Test; pure/read]
	FindByTag: (tag: string) -> Entity?,
	-- Returns the scene asset the current scene came from, or nil for a scene without one. [All modes; Runtime|Test; pure/read]
	GetCurrent: () -> AssetRef?,
	-- Returns the number of live entities, excluding pending destruction. [All modes; Runtime|Test; pure/read]
	GetEntityCount: () -> number,
	-- Returns a detached copy of the parameters passed to the current scene. [All modes; Runtime|Test; pure/read]
	GetLoadParameters: () -> {[string]: any},
	-- Returns the scene name. [All modes; Runtime|Test; pure/read]
	GetName: () -> string,
	-- Returns the first active Primary camera in canonical order, or nil. [All modes; Runtime|Test; pure/read]
	GetPrimaryCamera: () -> Entity?,
	-- Instantiates through the shared prefab path and invokes OnCreate for the complete subtree before returning. A position that cannot be represented under the parent raises a located error before anything is created. [All modes; Runtime|Test; mutates]
	Instantiate: (prefab: AssetRef, position: vector?, rotation: Quat?, parent: Entity?) -> Entity,
	-- Requests a scene/VM replacement at the end of the frame with copied JSON parameters; never destroys the calling VM inline. [All modes; Runtime|Test; mutates]
	Load: (scene: AssetRef, parameters: {[string]: any}?) -> (),
	-- Requests a restart of the current scene from the document it started from, at the end of the frame; parameters replace the load parameters, absent keeps them. [All modes; Runtime|Test; mutates]
	Reload: (parameters: {[string]: any}?) -> (),
}

-- Behaviour class registration.
declare Script: {
	-- Authenticate a behaviour class and return the exact same table. [All modes; LoadTime|Runtime|Test; pure/read]
	Define: <T>(name: string, class: T) -> T,
}

-- Caller-owned coroutines scheduled in simulation ticks. All scheduling and waits require writable execution.
declare Task: {
	-- Cancels without resuming; cancelling a completed or already cancelled task succeeds. [All modes; Runtime|Test; mutates]
	Cancel: (handle: TaskHandle) -> (),
	-- Schedules the caller-owned function after ceil(seconds/fixedDelta) ticks, at least one. [All modes; Runtime|Test; mutates]
	Delay: (seconds: number, fn: () -> ()) -> TaskHandle,
	-- Runs immediately to the first yield/end; inherits the caller's entity, case and deadline. [All modes; Runtime|Test; mutates]
	Spawn: (fn: () -> ()) -> TaskHandle,
	-- Yields a task or case for ceil(seconds/fixedDelta) ticks, at least one; invalid in nonyieldable callbacks. [All modes; Runtime|Test; mutates]
	Wait: (seconds: number) -> (),
	-- Yields for an exactly representable nonnegative integer number of ticks, at least one. [All modes; Runtime|Test; mutates]
	WaitTicks: (ticks: number) -> (),
}

-- Feature test suites, assertions, waits and deterministic input. A reported message shows authored locations and keeps at most 8192 bytes, truncated and marked like a log entry. A failed Test.ExpectEqual or Test.ExpectNear adds its values after the message, which is cut first so they always fit: nil, booleans and numbers (shortest round-trip form) as Luau writes them, strings quoted (their first 128 bytes, then their length), vectors, Colors and Quats by component, Entities and AssetRefs by UUID and other values by type name. A case's report keeps at most 32 failures for expectations and unexpected script errors, the last then counting any further ones as [truncated: <n> more failures], and a suite at most 1024 such failures. Test.Inject* queues at most 1024 events per input tick.
declare Test: {
	-- Capture exactly the requested current-and-following ticks; yield until grouped audio is ready. [All modes; Test; mutates]
	CaptureAudio: (ticks: number) -> TestAudioLevels,
	-- Register a case while collecting the suite body; the name is nonempty, unique in the suite and at most 256 bytes, and a suite has at most 4096 cases. [All modes; Test; pure/read]
	Case: (name: string, fn: () -> (), options: TestCaseOptions?) -> (),
	-- Record a failed condition and continue the case. [All modes; Test; pure/read]
	Expect: (condition: boolean, message: string?) -> (),
	-- Record a failed Luau equality comparison of actual with expected. The failure shows the message (default "expected values to be equal") followed by (got <actual>, expected <expected>). [All modes; Test; pure/read]
	ExpectEqual: (actual: any, expected: any, message: string?) -> (),
	-- Compare finite numbers or vector components with an absolute epsilon, default 0.00001. The failure shows the message (default "values differ by more than epsilon") followed by (got <actual>, expected <expected>, epsilon <epsilon>). [All modes; Test; pure/read]
	ExpectNear: (actual: number | vector, expected: number | vector, epsilon: number?, message: string?) -> (),
	-- Claim one nonfatal error occurrence in the current case using a Luau message pattern of at most 1024 bytes; the case keeps at most 256 unclaimed errors to claim. [All modes; Test; pure/read]
	ExpectScriptError: (pattern: string, withinTicks: number) -> (),
	-- End this case as failed. A case thread, or a task a behaviour spawned (which is cancelled), never runs past the call, even inside pcall; a lifecycle callback cannot yield, so there the call raises a script error that unwinds to the nearest pcall. The outcome stays latched either way. [All modes; Test; pure/read]
	Fail: (message: string) -> (),
	-- Read an entity's position in the last render snapshot. [All modes; Test; pure/read]
	GetExtractedPosition: (entity: Entity) -> vector?,
	-- Read the last completed render extraction. [All modes; Test; pure/read]
	GetLastExtraction: () -> TestExtraction,
	-- Read occurrences newer than an exclusive error cursor; claimed errors remain visible. The VM keeps the 256 distinct errors with the latest occurrences. [All modes; Test; pure/read]
	GetScriptErrors: (since: number?) -> {ScriptError},
	-- Read the canonical simulated state hash: the world (every entity with its serialized components, the tick, the session's Random state, the id draws and every body's physics state), never script tables, audio or input. It includes the tick, so two reads at different ticks always differ: compare two runs with input.replay's stateHash instead. [All modes; Test; pure/read]
	GetStateHash: () -> string,
	-- Queue a named button state or analog value (with a nil or Down state) for the next unapplied input tick. A button stays down until an Up (Tap releases it on the next tick), and an analog value holds on every later tick until another value is injected for the action (0 releases it). [All modes; Test; mutates]
	InjectAction: (name: string, state: InputEventState?, value: number?) -> (),
	-- Queue a gamepad button state, or an engine-convention axis value when the third argument is a number. [All modes; Test; mutates]
	InjectGamepad: ((index: number, button: GamepadButton, state: InputEventState) -> ()) & ((index: number, axis: GamepadAxis, value: number) -> ()),
	-- Queue a key state for the next unapplied input tick. [All modes; Test; mutates]
	InjectKey: (key: Key, state: InputEventState) -> (),
	-- Queue a mouse button and optional finite XY position. [All modes; Test; mutates]
	InjectMouse: (button: MouseButton, state: InputEventState, position: vector?) -> (),
	-- Queue a mouse move to a finite XY position in view coordinates for the next unapplied input tick. [All modes; Test; mutates]
	InjectMouseMove: (position: vector) -> (),
	-- Queue a scroll of a finite XY delta (wheel steps, y up-positive) for the next unapplied input tick. [All modes; Test; mutates]
	InjectScroll: (delta: vector) -> (),
	-- Queue committed text (non-empty UTF-8) for the next unapplied input tick, as typed characters. [All modes; Test; mutates]
	InjectText: (text: string) -> (),
	-- Synchronously reload a script in an editor test. [EditorOnly; Test; mutates]
	ReloadScript: (asset: AssetRef) -> (),
	-- Capture a screenshot through the owning host; a suite takes at most 256 screenshots, and a later call raises an error without capturing. [All modes; Test; mutates]
	Screenshot: (name: string) -> (),
	-- End this case as skipped without erasing earlier failures; it stops its caller the way Test.Fail does, and the outcome stays latched. [All modes; Test; pure/read]
	Skip: (reason: string) -> (),
	-- Construct a suite without executing its body; the name is UTF-8 without NUL and at most 256 bytes. [All modes; LoadTime|Runtime|Test; pure/read]
	Suite: (name: string, body: () -> (), options: TestSuiteOptions?) -> TestSuite,
	-- Yield a test thread for simulation ticks. [All modes; Test; pure/read]
	WaitTicks: (ticks: number) -> (),
	-- Evaluate the predicate now and after every following tick until it returns true; it must return a boolean (anything else is a script error). Still false after timeoutTicks ticks, the case fails with "Test.WaitUntil exceeded its tick limit" at this call. [All modes; Test; pure/read]
	WaitUntil: (predicate: () -> boolean, timeoutTicks: number) -> (),
}

-- Phase-aware simulation time; use fixed time for deterministic gameplay.
declare Time: {
	-- Returns the fixed step in the fixed phase (the time scale changes how many steps a frame runs, not their length) and the scaled frame delta in seconds in the frame phase. [All modes; Runtime|Test; pure/read]
	GetDeltaTime: () -> number,
	-- Returns the session's fixed step in seconds. [All modes; Runtime|Test; pure/read]
	GetFixedDeltaTime: () -> number,
	-- Returns the current rendered-frame index. [All modes; Runtime|Test; pure/read]
	GetFrameCount: () -> number,
	-- Returns one during the fixed phase, otherwise this frame's interpolation alpha. [All modes; Runtime|Test; pure/read]
	GetInterpolationAlpha: () -> number,
	-- Returns monotonic wall-clock seconds from an unspecified epoch. Nondeterministic; never use for gameplay. [All modes; Runtime|Test; pure/read]
	GetRealTime: () -> number,
	-- Returns the current simulation tick. [All modes; Runtime|Test; pure/read]
	GetTick: () -> number,
	-- Returns simulation seconds: tick multiplied by fixed delta. [All modes; Runtime|Test; pure/read]
	GetTime: () -> number,
	-- Returns the current session time scale. [All modes; Runtime|Test; pure/read]
	GetTimeScale: () -> number,
	-- Sets the session time scale in [0, 100]; refused during read-only evaluation. [All modes; Runtime|Test; mutates]
	SetTimeScale: (scale: number) -> (),
}

-- The game UI's interaction state in the running scene: keyboard and gamepad focus, the widget under the pointer and the one it presses, and where elements were laid out. UI events reach behaviours through the OnUi callbacks, which bubble from the element to its ancestors.
declare Ui: {
	-- Returns the UI element that has keyboard and gamepad focus, or nil. [All modes; Runtime|Test; pure/read]
	GetFocus: () -> Entity?,
	-- Returns the interactable UI widget under the pointer, or nil. [All modes; Runtime|Test; pure/read]
	GetHovered: () -> Entity?,
	-- Returns the UI widget the held pointer pressed, or nil. [All modes; Runtime|Test; pure/read]
	GetPressed: () -> Entity?,
	-- Returns element's rectangle in the current UI layout, in scene pixels (which are view coordinates unless the output is upscaled) or, for an element of a World canvas, in that canvas's own pixels, with its canvas; nil when it is not laid out. [All modes; Runtime|Test; pure/read]
	GetRect: (element: Entity) -> UiElementRect?,
	-- Returns whether the pointer was over a UI image, text or widget that catches it at this tick, so gameplay can ignore clicks meant for the UI. [All modes; Runtime|Test; pure/read]
	IsPointerOverUi: () -> boolean,
	-- Moves focus to element, a laid-out, interactable and focusable UI widget, or clears it with nil; OnUiFocus follows in the next UiEvents phase. Any other element raises a located error saying why. [All modes; Runtime|Test; mutates]
	SetFocus: (element: Entity?) -> (),
}
