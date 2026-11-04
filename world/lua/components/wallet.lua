-- Sample owner-only component: only the owning player's client ever receives it.
return {
    name = "Wallet",
    persist = true,
    replicate = "owner",
    fields = {
        { name = "gold", type = "int", default = 0, min = 0 },
    },
}
