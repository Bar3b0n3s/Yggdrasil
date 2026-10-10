--!strict
return function(entity: Entity): Transform?
	return entity:GetComponent("RigidBody")
end
