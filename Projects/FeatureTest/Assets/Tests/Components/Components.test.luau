--!strict
-- Components.scene and its variants (Architecture §15.5, §15.6 gates 1 and 1b; ADR 0022 decision 9): every component
-- with every serialized field non-default somewhere and every value of every serialized enum used somewhere. The suite
-- checks that the authored values survive the round trip from the scaffold through the scene file into the running
-- scene, renders each scene through Test.Screenshot and loads every variant with Scene.Load (PostProcess and Environment
-- are unique per scene, so the other Tonemap values live in Assets/Scenes/Components/).

local AuthoredValues = require("./AuthoredValues")

type Authored = AuthoredValues.Authored

-- The variant scenes, loaded in this order: name, tone mapper, SSAO quality.
local VARIANTS: { { Name: string, Tonemap: string, Quality: string } } = {
	{ Name = "TonemapAces", Tonemap = "ACES", Quality = "Low" },
	{ Name = "TonemapPbrNeutral", Tonemap = "PbrNeutral", Quality = "Medium" },
	{ Name = "TonemapLinear", Tonemap = "Linear", Quality = "High" },
}

local EPSILON = 1e-5

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `{Scene.GetName()}.scene has an entity named {name}`)
	return entity
end

-- Whether an authored value and the value read through the proxy agree (numbers and vectors within EPSILON, colours and
-- quaternions per channel).
local function same(actual: any, expected: any): boolean
	local kind = typeof(expected)
	if typeof(actual) ~= kind then
		return false
	elseif kind == "number" then
		return math.abs(actual - expected) <= EPSILON
	elseif kind == "vector" then
		return vector.magnitude(actual - expected) <= EPSILON
	elseif kind == "Color" then
		return math.abs(actual.r - expected.r) <= EPSILON and math.abs(actual.g - expected.g) <= EPSILON
			and math.abs(actual.b - expected.b) <= EPSILON and math.abs(actual.a - expected.a) <= EPSILON
	elseif kind == "Quat" then
		return math.abs(actual.x - expected.x) <= EPSILON and math.abs(actual.y - expected.y) <= EPSILON
			and math.abs(actual.z - expected.z) <= EPSILON and math.abs(actual.w - expected.w) <= EPSILON
	end
	return actual == expected
end

local function read(value: Authored): any
	local entity: any = find(value.Entity)
	local component = entity:GetComponent(value.Component)
	assert(component ~= nil, `{value.Entity} has a {value.Component} component`)
	return component[value.Field]
end

local function variantCases(index: number)
	local variant = VARIANTS[index]
	Test.Case(`the {variant.Name} variant renders with its tone mapper`, function()
		Test.ExpectEqual(Scene.GetName(), variant.Name)
		Test.ExpectEqual(Scene.GetLoadParameters().Variant, variant.Name)
		local post = find("PostProcess").PostProcess
		assert(post, "the variant has a PostProcess component")
		Test.ExpectEqual(post.Tonemap, variant.Tonemap)
		Test.ExpectEqual(post.SsaoQuality, variant.Quality)
		local sun = find("Sun").DirectionalLight
		assert(sun, "the variant has a sun")
		Test.ExpectEqual(sun.CastShadows, false)
		Test.WaitTicks(2)
		Test.Screenshot(variant.Name)
		local following = VARIANTS[index + 1]
		local nextScene = if following then `Assets/Scenes/Components/{following.Name}.scene` else "Assets/Scenes/Components.scene"
		local asset = Assets.Load(nextScene)
		assert(asset, `{nextScene} loads`)
		Scene.Load(asset, { Variant = if following then following.Name else "Components" })
	end)
end

return Test.Suite("Components", function()
	Test.Case("every authored field value survives the round trip into the running scene", function()
		Test.ExpectEqual(Scene.GetName(), "Components")
		for _, value in AuthoredValues.Fields do
			local actual = read(value)
			Test.Expect(same(actual, value.Value), `{value.Entity}.{value.Component}.{value.Field} is {value.Value}, not {actual}`)
		end
		for _, value in AuthoredValues.Assets do
			local actual = read(value)
			Test.Expect(actual ~= nil, `{value.Entity}.{value.Component}.{value.Field} is set`)
			if actual then
				Test.ExpectEqual(Assets.GetPath(actual), value.Value, `{value.Entity}.{value.Component}.{value.Field}`)
			end
		end
	end)

	Test.Case("the hierarchy, tags and transforms load as authored", function()
		local ground = find("Ground")
		local mesh = find("GroundMesh")
		Test.ExpectEqual(mesh:GetParent(), ground)
		Test.ExpectEqual(#ground:GetChildren(), 1)
		Test.Expect(ground:HasTag("Level") and ground:HasTag("Floor"), "Ground keeps its tags")
		Test.ExpectEqual(table.concat(ground:GetTags(), ","), "Level,Floor")
		Test.Expect(find("Camera"):HasTag("MainCamera"), "Camera keeps its tag")
		Test.ExpectNear(find("Spot").Transform.EulerAngles, vector.create(-60, 20, 0), 1e-3)
		Test.ExpectNear(find("Ball").Transform.Scale, vector.create(0.8, 0.8, 0.8))
		Test.ExpectEqual(Scene.GetPrimaryCamera(), find("Camera"))
		local title = find("Title").Text
		assert(title, "Title has a Text component")
		Test.Expect(title.Font == nil, "a text without a font uses the default font")
		local materials = find("Beacon").MeshRenderer
		assert(materials, "Beacon has a MeshRenderer")
		local glow = Assets.Load("Assets/Scenes/Components/Materials/Glow.material")
		Test.ExpectEqual(materials:GetMaterial(1), glow)
	end)

	Test.Case("the scene renders", function()
		Test.WaitTicks(2)
		Test.Screenshot("Components")
		local first = VARIANTS[1]
		local asset = Assets.Load(`Assets/Scenes/Components/{first.Name}.scene`)
		assert(asset, `the {first.Name} variant loads`)
		Scene.Load(asset, { Variant = first.Name })
	end)

	for index = 1, #VARIANTS do
		variantCases(index)
	end

	Test.Case("the scene loads again after the variants", function()
		Test.ExpectEqual(Scene.GetName(), "Components")
		Test.ExpectEqual(Scene.GetLoadParameters().Variant, "Components")
		Test.Expect(find("Title"):IsValid(), "the reloaded scene has its entities")
	end)
end)
