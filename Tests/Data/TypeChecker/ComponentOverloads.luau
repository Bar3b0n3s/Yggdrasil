--!strict
local function check(entity: Entity, random: RandomGenerator)
	do
		local component: Transform? = entity:GetComponent("Transform")
		if component then print(component) end
	end
	do
		local component: MeshRenderer? = entity:GetComponent("MeshRenderer")
		if component then print(component) end
	end
	do
		local component: Camera? = entity:GetComponent("Camera")
		if component then print(component) end
	end
	do
		local component: DirectionalLight? = entity:GetComponent("DirectionalLight")
		if component then print(component) end
	end
	do
		local component: PointLight? = entity:GetComponent("PointLight")
		if component then print(component) end
	end
	do
		local component: SpotLight? = entity:GetComponent("SpotLight")
		if component then print(component) end
	end
	do
		local component: Environment? = entity:GetComponent("Environment")
		if component then print(component) end
	end
	do
		local component: PostProcess? = entity:GetComponent("PostProcess")
		if component then print(component) end
	end
	do
		local component: Text? = entity:GetComponent("Text")
		if component then print(component) end
	end
	do
		local component: RigidBody? = entity:GetComponent("RigidBody")
		if component then print(component) end
	end
	do
		local component: BoxCollider? = entity:GetComponent("BoxCollider")
		if component then print(component) end
	end
	do
		local component: SphereCollider? = entity:GetComponent("SphereCollider")
		if component then print(component) end
	end
	do
		local component: CapsuleCollider? = entity:GetComponent("CapsuleCollider")
		if component then print(component) end
	end
	do
		local component: MeshCollider? = entity:GetComponent("MeshCollider")
		if component then print(component) end
	end
	do
		local component: CharacterController? = entity:GetComponent("CharacterController")
		if component then print(component) end
	end
	do
		local component: AudioSource? = entity:GetComponent("AudioSource")
		if component then print(component) end
	end
	do
		local component: AudioListener? = entity:GetComponent("AudioListener")
		if component then print(component) end
	end
	do
		local component: Script? = entity:GetComponent("Script")
		if component then print(component) end
	end
	local choice: number = random:Choice({1, 2, 3})
	local shuffled: {string} = random:Shuffle({"a", "b"})
	return choice, shuffled
end
return check
