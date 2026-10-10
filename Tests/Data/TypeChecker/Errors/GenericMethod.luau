--!strict
return function(random: RandomGenerator): string
	return random:Choice({1, 2, 3})
end
