-- Sample Lua component: every field type, an enum, an array and min/max. See docs/world/lua.html.
-- persist and replicate are world-server keys (docs/world/lua.html); a client sharing the file ignores them.
return {
    name = "Health",
    displayName = "Health",
    persist = true,     -- saved per character in MySQL
    replicate = "aoi",  -- sent to the owner and everyone who can see the character
    fields = {
        { name = "current", type = "float",  default = 100, min = 0 },
        { name = "max",     type = "float",  default = 100, min = 1 },
        { name = "regen",   type = "bool",   default = true },
        { name = "lives",   type = "int",    default = 3, min = 0, max = 9 },
        { name = "title",   type = "string", default = "Hero" },
        { name = "tint",    type = "vec3",   color = true, default = {x = 1, y = 0, z = 0} },
        { name = "facing",  type = "quat" },
        { name = "faction", type = "enum",   values = {"neutral", "ally", "enemy"}, default = "neutral" },
        { name = "drops",   type = "array",  of = "string", default = {"gold", "potion"} },
    },
}
