# Explicit source boundaries; sourced by host and Android build scripts.
NZM_PRODUCTION_MODULES=(main core lifecycle backend dm_backend demand_watch demand_exec
    swap vm metrics policy execution intent event_daemon diagnostics boot_state
    memory_budget log config io helper recovery spawn_fd request_backend)
NZM_LEGACY_MODULES=(planner executor ledger runtime psi wear_budget wear_store)
