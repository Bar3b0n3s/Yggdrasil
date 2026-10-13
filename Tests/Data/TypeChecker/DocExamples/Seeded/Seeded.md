# Seeded documentation

The DocExamples test reads this page like a documentation page. Its three Luau examples have a type error on purpose.

```luau prelude=Behaviour
function Example.OnStart(self: Example)
	self.Count = "three"
end
```

```luau path=Assets/Scripts/Seeded.luau
--!strict
local Seeded = {}

function Seeded.Count(): number
	return "seven"
end

return Seeded
```

```luau prelude=Behaviour
-- Shadows the engine's Script module, so the prelude's own return statement, after this example, indexes a number.
local Script = 5
```

```lua skip="shows invalid syntax on purpose"
local = 1
```

```json
{"Not": "Luau"}
```
