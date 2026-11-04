# Sourced by load.sh and integration.sh (from world): one run's compose override, build/stack-override.yml,
# which every later `docker compose` call picks up through COMPOSE_FILE (the bot's `compose run` too, so it never
# recreates the world with other settings).
#   stack_override CAP: mounts a copy of the maps folder (MAPS_DIR from the shell or .env, default ./maps) with "layerCap": CAP on START_MAP's map.json, for the world
#     and the bot (docs/world/maps.html#layers). CAP empty: stack_default.
#   MAP_DIR (a maps folder: absolute, or ./ or ../ relative to world/; load.sh only): that folder instead of
#     the maps folder, START_MAP (default 1) the world's start map; with CAP, the copy is of it.
#   stack_default: docker-compose.yml alone again.
stack_override() {
    local cap=$1 out=build/stack-override.yml start src=${MAP_DIR:-$(maps_dir)} mount=./build/layer-maps
    [ -z "$cap" ] && [ -z "${MAP_DIR:-}" ] && { stack_default; return; }
    mkdir -p build
    if [ -z "$cap" ]; then mount=$src; fi
    {
        printf 'services:\n  world:\n'
        if [ -n "${MAP_DIR:-}" ]; then printf '    environment:\n      START_MAP: "%s"\n' "${START_MAP:-1}"; fi
        printf '    volumes:\n      - %s:/app/maps:ro\n  bot:\n    volumes:\n      - %s:/app/maps:ro\n' "$mount" "$mount"
    } > "$out"
    if [ -n "$cap" ]; then
        rm -rf build/layer-maps && cp -r "$src" build/layer-maps
        start=$(grep -l "\"id\": *${START_MAP:-1}," build/layer-maps/*/map.json | head -1)
        sed -i "0,/{/s//{\"layerCap\": $cap,/" "$start"
        echo "  layers: $start has \"layerCap\": $cap for this run"
    fi
    export COMPOSE_PATH_SEPARATOR=: COMPOSE_FILE="docker-compose.yml:$out"
}

# MAPS_DIR as docker compose sees it: the shell's, else .env's, else ./maps.
maps_dir() { local d=${MAPS_DIR:-$(sed -n 's/^MAPS_DIR=//p' .env 2>/dev/null | tr -d '\r' | tail -1)}; echo "${d:-./maps}"; }

stack_default() { unset COMPOSE_FILE COMPOSE_PATH_SEPARATOR; }
