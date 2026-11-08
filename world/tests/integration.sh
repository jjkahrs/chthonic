#!/usr/bin/env bash
# Runs every integration scenario against the compose stack (docker compose up -d --build first). Every compose
# call adds the test-only docker/test.override.yml (tests/stack_override.sh); auth, world and chat are recreated
# with it first if they run without it, and are left that way (back to the default: docker compose up -d).
# Prints PASS/FAIL/SKIP per scenario and the counts, and exits non-zero if any failed.
# Scenarios that need the sample maps (0, 16-20, 25, 30, 32, 41, 42) SKIP when the world's mounted maps folder
# (MAPS_DIR, default ./maps) lacks the files they need.
# Scenarios 10-13, 34 and 37 stop/restart containers, so run this against a test stack only.
# --admin runs only scenarios 39-40 (the admin port; WORLD_ADMIN_SECRET in .env).
# --instances runs only scenarios 17, 41 and 42 (portals, instance maps; 42 restarts world); export MAP_THREADS to run
# them at that setting.
# --layers runs only scenario 43 (layers: recreates world with a layered START_MAP, then with the default maps).
set -u
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1 # Git Bash on Windows: keep container paths as written

RUN="${RUN_ID:-it$(date +%s)}"
LOGDIR="${LOGDIR:-build/integration-$RUN}"
mkdir -p "$LOGDIR"
. tests/stack_override.sh
stack_default
docker compose up -d auth world chat >/dev/null 2>&1 # recreates only what runs without the test override
WORLD=$(docker compose ps -q world)
declare -a RESULTS=()
FAILED=0 NPASS=0 NFAIL=0 NSKIP=0

bot() { docker compose run --rm -T bot --run-id "$RUN" "$@"; }

record() { # name, ok(0/1), detail
    if [ "$2" -eq 0 ]; then RESULTS+=("PASS  $1"); NPASS=$((NPASS + 1)); else RESULTS+=("FAIL  $1  $3"); FAILED=1; NFAIL=$((NFAIL + 1)); fi
    echo "==> $( [ "$2" -eq 0 ] && echo PASS || echo FAIL ) $1"
}

skip() { # name, reason
    RESULTS+=("SKIP  $1  $2"); NSKIP=$((NSKIP + 1))
    echo "==> SKIP $1 ($2)"
}

# needs_maps NAME FILE...: 0 if every FILE (relative to the world's /app/maps) is there; otherwise records scenario NAME
# as SKIP and returns 1.
needs_maps() {
    local name=$1; shift
    docker compose exec -T world sh -c 'for f; do [ -e "/app/maps/$f" ] || exit 1; done' sh "$@" >/dev/null 2>&1 && return 0
    echo "--- $name"
    skip "$name" "maps folder lacks one of: $*"
    return 1
}
# What the sample-map scenarios need: start's portals (areas.json) and the map behind each, the Guard's spawn file.
PORTAL_CAVE="start/areas.json cave/map.json"
PORTAL_HILLS="start/areas.json hills/map.json"
PORTAL_CRYPT="start/areas.json crypt/map.json"
GUARD_SPAWN="start/.server/spawns/4_4.json"
map_scenario() { # "FILE...", name, bot args...: scenario, or SKIP without the maps it needs
    local req=$1; shift
    needs_maps "$1" $req && scenario "$@"
}

scenario() { # name, bot args...
    local name=$1; shift
    local log="$LOGDIR/${name%% *}.log"
    echo "--- $name"
    bot "$@" 2>&1 | tee "$log" | grep -v -E '^ *(Container|Network) '
    record "$name" "${PIPESTATUS[0]}" "see $log"
}

wait_line() { # file, pattern, timeout seconds
    local i
    for ((i = 0; i < $3 * 2; i++)); do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}

wait_healthy() { # service
    local i id
    for ((i = 0; i < 120; i++)); do
        id=$(docker compose ps -q "$1")
        [ -n "$id" ] && [ "$(docker inspect -f '{{.State.Health.Status}}' "$id" 2>/dev/null)" = healthy ] && return 0
        sleep 1
    done
    return 1
}

now_ms() { date +%s%3N; }

# Graceful stop of the world container while a bot is in the world; checks exit code, time and the client's
# Disconnect, then restarts and verifies the saved position. $1 = label, $2 = account tag, $3 = how (stop|restart)
graceful() {
    local name=$1 tag=$2 how=$3 log="$LOGDIR/${1%% *}.log" ok=0 detail=""
    echo "--- $name"
    bot --scenario shutdown --tag "$tag" >"$log" 2>&1 &
    local botpid=$!
    if ! wait_line "$log" '^READY' 90; then
        cat "$log"; record "$name" 1 "bot never got ready"; wait $botpid; return
    fi
    read -r _ x y z < <(grep '^LASTPOS' "$log")
    local since; since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    local t0; t0=$(now_ms)
    if [ "$how" = restart ]; then docker compose restart -t 15 world >/dev/null 2>&1; else docker compose stop -t 15 world >/dev/null 2>&1; fi
    local elapsed=$(( $(now_ms) - t0 ))
    wait $botpid; local botrc=$?
    grep -v -E '^ *(Container|Network) ' "$log"
    local logs; logs=$(docker logs --since "$since" "$WORLD" 2>&1)
    if [ "$how" = stop ]; then
        local code; code=$(docker inspect -f '{{.State.ExitCode}}' "$WORLD")
        echo "  world exit code $code after ${elapsed} ms"
        [ "$code" = 0 ] || { ok=1; detail+="exit code $code; "; }
        [ "$elapsed" -lt 10000 ] || { ok=1; detail+="took ${elapsed} ms; "; }
        docker compose start world >/dev/null 2>&1
    else
        echo "  restart took ${elapsed} ms"
    fi
    echo "$logs" | grep -E '"shutting down"|"shutdown complete"|"tick stats total"' | sed 's/^/  log: /'
    echo "$logs" | grep -q '"shutdown complete"' || { ok=1; detail+="no 'shutdown complete' log; "; }
    [ $botrc -eq 0 ] || { ok=1; detail+="client did not get server_shutdown; "; }
    wait_healthy world || { ok=1; detail+="world not healthy after restart; "; }
    bot --scenario restart-verify --tag "$tag" --expect-pos "$x,$y,$z" 2>&1 | tee -a "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || { ok=1; detail+="position not persisted; "; }
    record "$name" $ok "$detail"
}

echo "run id: $RUN   logs: $LOGDIR"
wait_healthy auth && wait_healthy world || { echo "auth or world is not healthy; run docker compose up -d --build"; exit 1; }

summary() {
    echo
    echo "================ integration summary ($RUN) ================"
    printf '%s\n' "${RESULTS[@]}"
    echo "pass $NPASS  fail $NFAIL  skip $NSKIP"
    exit $FAILED
}

# 39-40: the world's admin port (docs/world/protocol.html#admin). 40 recreates the world container without
# WORLD_ADMIN_SECRET, then again with the one from .env.
admin_scenarios() {
    scenario "39 admin port: wrong secret refused, online list, kick by character / account, kicked character saved" --scenario admin
    local name="40 admin port off (WORLD_ADMIN_SECRET empty): logged, nothing listening, the world still serves players"
    local log="$LOGDIR/40.log" ok=0
    echo "--- $name"
    export WORLD_ADMIN_SECRET= # the bot's `compose run` must see the same world config, or it recreates the world
    docker compose up -d world >/dev/null 2>&1
    wait_healthy world || ok=1
    WORLD=$(docker compose ps -q world)
    if docker logs "$WORLD" 2>&1 | grep '"admin port off' | tee "$log" | sed 's/^/  log: /' | grep -q .; then :; else ok=1; fi
    bot --scenario admin-off 2>&1 | tee -a "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
    unset WORLD_ADMIN_SECRET
    docker compose up -d world >/dev/null 2>&1
    wait_healthy world || ok=1
    WORLD=$(docker compose ps -q world)
    docker logs "$WORLD" 2>&1 | grep -q '"admin port listening"' || ok=1
    record "$name" $ok "see $log"
}

# --admin: only the admin port scenarios.
if [ "${1:-}" = "--admin" ]; then
    admin_scenarios
    summary
fi

instance_scenarios() {
    portal_scenario
    # 41: instance maps (docs/world/instances.html) with the sample maps/crypt and lua/systems/test_hooks.lua's
    # instanceKey; the admin port's instance list when WORLD_ADMIN_SECRET is set. ~35 s: it waits out the 5 s empty timer.
    instances_scenario
    instance_restart
}

portal_scenario() {
    local name="17 portal: MapChange{2} first, Bat seen, B lost, relog stays on map 2"
    map_scenario "$PORTAL_CAVE" "$name" --scenario portal
}

instances_scenario() {
    local name="41 instances: same key -> one copy, other key -> another (own NPCs), 3rd player refused (instance.full), relog rejoins, empty 5 s -> destroyed, relog -> exit"
    map_scenario "$PORTAL_CRYPT" "$name" --scenario instances
}

# 42: a character saved inside a copy enters at the map's exit after a world restart (copies are memory-only).
instance_restart() {
    local name="42 instance + restart: saved in a copy, the world restarts, relog -> the crypt's exit" log="$LOGDIR/42.log" ok=0
    needs_maps "$name" $PORTAL_CRYPT || return
    echo "--- $name"
    bot --scenario instance-restart 2>&1 | tee "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
    sleep 1 # the logout's save
    docker compose restart -t 15 world >/dev/null 2>&1 && echo "  world restarted"
    wait_healthy world || ok=1
    bot --scenario instance-restart-verify 2>&1 | tee -a "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
    record "$name" $ok "see $log"
}

# --instances: only the portal and instance map scenarios.
if [ "${1:-}" = "--instances" ]; then
    instance_scenarios
    summary
fi

# 43: layers (docs/world/maps.html#layers): the world recreated with START_MAP at "layerCap": 2 (a copy of the
# maps, tests/stack_override.sh), then again with the default maps.
layer_scenario() {
    local name="43 layers: START_MAP at layerCap 2, A and B share the base layer, C lands in a new layer and sees neither" log="$LOGDIR/43.log" ok=0
    echo "--- $name"
    stack_override 2
    docker compose up -d world >/dev/null 2>&1
    wait_healthy world || ok=1
    WORLD=$(docker compose ps -q world)
    bot --scenario layers 2>&1 | tee "$log" | grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
    docker logs "$WORLD" 2>&1 | grep '"layer created"' | tee -a "$log" | sed 's/^/  log: /' | grep -q . || ok=1
    stack_default
    docker compose up -d world >/dev/null 2>&1
    wait_healthy world || ok=1
    WORLD=$(docker compose ps -q world)
    record "$name" $ok "see $log"
}

# --layers: only the layer scenario.
if [ "${1:-}" = "--layers" ]; then
    layer_scenario
    summary
fi

# 0: chthonic_mapbake in the image (docs/world/maps.html#navmesh) bakes every tile of the mounted (read-only) hills into a temp dir;
# the server keeps the navtiles stored in the repo. Checksums are logged to compare with a Windows bake.
log="$LOGDIR/0.log"
if needs_maps "0 chthonic_mapbake bakes maps/hills in the image" hills/map.json hills/.server/nav; then
echo "--- 0 chthonic_mapbake bakes maps/hills in the image"
docker compose run --rm -T --entrypoint sh bot -c \
    './chthonic_mapbake /app/maps/hills --out /tmp/nav && sha256sum /tmp/nav/* /app/maps/hills/.server/nav/*' 2>&1 |
    tee "$log" | grep -v -E '^ *(Container|Network) '
ok=${PIPESTATUS[0]}
grep -q '^4 navtile(s) -> /tmp/nav' "$log" || ok=1
record "0 chthonic_mapbake bakes maps/hills in the image" "$ok" "see $log"
fi

scenario "1 register/verify/login + TLS + AuthOk" --scenario auth
scenario "2 character create/list/delete, limits, names" --scenario characters
scenario "3 characters are owner-only" --scenario ownership
scenario "4 AOI spawn/move/despawn" --scenario aoi
scenario "5 say chat in range only" --scenario chat
scenario "6 speedhack corrected, not replicated" --scenario speedhack
scenario "7 malformed/oversized/unknown frames" --scenario malformed
# root: reads auth's private key from its volume to mint an expired token (test only)
echo "--- 8 handshake rejections"
docker compose run --rm -T --user root bot --run-id "$RUN" --scenario handshake --auth-key /auth-keys/jwt-private.pem \
    2>&1 | tee "$LOGDIR/8.log" | grep -v -E '^ *(Container|Network) '
record "8 handshake rejections" "${PIPESTATUS[0]}" "see $LOGDIR/8.log"
scenario "9 duplicate login" --scenario duplicate
scenario "10a position persists across logout/login" --scenario persist
graceful "10b position persists across graceful restart" s10r restart

# 11: MySQL stopped mid-session
name="11 MySQL down mid-session"
echo "--- $name"
log="$LOGDIR/11.log"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
bot --scenario dbdown >"$log" 2>&1 &
botpid=$!
ok=0
if wait_line "$log" 'READY stop-db' 120; then
    docker compose stop mysql >/dev/null 2>&1 && echo "  mysql stopped"
    if wait_line "$log" 'READY start-db' 180; then
        sleep 5 # the logged-out bot's save is now failing and being retried
        docker compose start mysql >/dev/null 2>&1 && echo "  mysql started"
    else
        ok=1
        docker compose start mysql >/dev/null 2>&1
    fi
else
    ok=1
fi
wait $botpid || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
docker logs --since "$since" "$WORLD" 2>&1 | grep -E '"save failed|"database (unavailable|reconnected)"' | sed 's/^/  log: /'
wait_healthy mysql
wait_healthy auth
record "$name" $ok "see $log"

graceful "12 SIGTERM: server_shutdown, saves flushed, exit 0 < 10 s" s12 stop

# 13: Lua components with the sample lua/ (docs/world/lua.html): replication modes, then the
# persisted values across a relog (inside the scenario) and across a world restart.
name="13 Lua components: aoi/owner replication, persisted across relog and restart"
echo "--- $name"
log="$LOGDIR/13.log"
ok=0
bot --scenario components --tag s13 >"$log" 2>&1 || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
read -r _ gold health < <(grep '^COMPONENTS' "$log") || ok=1
if [ $ok -eq 0 ]; then
    docker compose restart -t 15 world >/dev/null 2>&1 && echo "  world restarted"
    wait_healthy world || ok=1
    bot --scenario components-verify --tag s13 --expect-components "$gold,$health" 2>&1 | tee -a "$log" |
        grep -v -E '^ *(Container|Network) '
    [ "${PIPESTATUS[0]}" -eq 0 ] || ok=1
fi
record "$name" $ok "see $log"

# 14: server events with the sample lua/systems/events.lua (docs/world/events.html)
scenario "14 Lua server events: sendTo/sendNearby/broadcast audiences" --scenario events

# 15: state machines with the sample lua/fsms/locomotion.lua + lua/systems/locomotion.lua (docs/world/state-machines.html)
scenario "15 state machines: transitions observed, denied one silent" --scenario fsm

# 16: NPCs with the sample lua/systems/npcs.lua (docs/world/npcs.html): the patrolling Guard seen, moving, leaving and returning
map_scenario "$GUARD_SPAWN" "16 NPCs: Spawn{npc, Guard}, Health from its definition, moves, Despawn out of range, Spawn on return" --scenario npcs

# 17: areas with the sample maps' areas.json (docs/world/areas.html): start's cave_mouth portal to map 2, kept across a relog
portal_scenario

# 18: NPC spawns (docs/world/npcs.html#npc-spawns): the test hook destroys the Guard; it respawns after the fixture's respawnSec (5 s)
map_scenario "$GUARD_SPAWN" "18 NPC respawn: Guard destroyed by lua/systems/test_hooks.lua, back 5 s later at its spawn" --scenario respawn

# 19: ground on the sample hills map (docs/world/maps.html#ground): slope walk accepted and seen on the ground, fly hack corrected
map_scenario "$PORTAL_HILLS" "19 terrain: hills_gate lands on the ground, slope walk accepted, fly hack -> Correction" --scenario terrain

# 20: navmesh on the sample hills map (docs/world/maps.html#navmesh): walking into the mesa's wall is corrected; the Sentry walks around it
map_scenario "$PORTAL_HILLS" "20 navmesh: mesa wall -> Correction, Sentry patrols around it on the ground" --scenario navmesh

# 21-22: client events (docs/world/protocol.html#client-events) with the sample lua/systems/test_hooks.lua
scenario "21 client events: client.<name> reaches Lua with the sender; bad name / > 1 KB / non-object -> malformed" --scenario client-events
name="22 client events: 10 a second per session, the rest dropped and counted"
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
scenario "$name" --scenario client-event-rate
sleep 1 # the logout's despawn logs the count
if ! docker logs --since "$since" "$WORLD" 2>&1 | grep '"client events dropped"' | grep -q '"count":20'; then
    [[ ${RESULTS[${#RESULTS[@]}-1]} == PASS* ]] && NPASS=$((NPASS - 1)) NFAIL=$((NFAIL + 1))
    RESULTS[${#RESULTS[@]}-1]="FAIL  $name  no 'client events dropped' log with count 20"
    FAILED=1
    echo "==> FAIL $name (dropped count not logged)"
fi

# 25: zone objects on the sample hills (docs/world/maps.html#objects) with the sample lua/systems/test_hooks.lua
map_scenario "$PORTAL_HILLS" "25 zone objects: Interact on another map / out of range / unknown id -> nothing, in range -> test.shrine event; into the obstacle crate -> Correction" --scenario objects

# 26-27: client prediction (docs/world/protocol.html#movement); meant for a stack with the lag simulator on
# (SIM_LATENCY_MS=150 SIM_JITTER_MS=50 in .env), and pass without it too
scenario "26 lag run: 10 s at 7 m/s with turns -> 0 Corrections" --scenario lag-run
scenario "27 lag reject: one 50 m jump while running -> exactly 1 Correction, the rebased client and the server agree" --scenario lag-reject

# 29-37: proximity chat kinds on the world server, and the chat service (docs/chat)
wait_healthy chat || echo "WARNING: the chat service is not healthy; scenarios 30-37 will fail"
scenario "29 yell within CHAT_YELL_RADIUS (past the AOI radius, not past its own); emote in say range" --scenario yell
map_scenario "$PORTAL_CAVE" "30 chat: ChatServer announced, whisper across maps, offline name -> not_online" --scenario chat-whisper
scenario "31 chat: General auto-joined, Trade join/leave, non-members don't hear" --scenario chat-channels
map_scenario "$PORTAL_CAVE" "32 chat: zone reaches the sender's map only and follows map changes" --scenario chat-zone
scenario "33 chat: ignore drops whisper/channel/zone lines, unignore restores" --scenario chat-ignore

# 34: the GM account gets role gm in auth's database, then the mute must survive `docker compose restart chat`
name="34 chat: player can't mute, GM can; chat + say/yell/emote dropped; survives a chat restart; expires; unmute"
echo "--- $name"
log="$LOGDIR/34.log"
ok=0
bot --scenario chat-mute-setup >"$log" 2>&1 || ok=1
email=$(grep '^GM_EMAIL' "$log" | awk '{print $2}')
docker compose exec -T -e EMAIL="$email" mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "$MYSQL_DATABASE" -e "UPDATE accounts SET role = '\''gm'\'' WHERE email = '\''$EMAIL'\''"' \
    >>"$log" 2>&1 || ok=1
bot --scenario chat-mute >>"$log" 2>&1 &
botpid=$!
if wait_line "$log" 'READY restart-chat' 120; then
    docker compose restart -t 10 chat >/dev/null 2>&1 && echo "  chat restarted"
else
    ok=1
fi
wait $botpid || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
record "$name" $ok "see $log"

scenario "35 chat: control chars stripped, oversize/invalid UTF-8 rejected, rate limit, no disconnect" --scenario chat-limits
# root: reads auth's private key from its volume to mint an expired token (test only)
echo "--- 36 chat refusals: bad/expired JWT, no character, wrong link secret/version"
docker compose run --rm -T --user root bot --run-id "$RUN" --scenario chat-auth --auth-key /auth-keys/jwt-private.pem \
    2>&1 | tee "$LOGDIR/36.log" | grep -v -E '^ *(Container|Network) '
record "36 chat refusals: bad/expired JWT, no character, wrong link secret/version" "${PIPESTATUS[0]}" "see $LOGDIR/36.log"

# 37: chat stopped -> say still works; started again -> clients reconnect, General, whispers
name="37 chat down: say still works; chat back: reconnect, General rejoined, whispers work"
echo "--- $name"
log="$LOGDIR/37.log"
ok=0
bot --scenario chat-down >"$log" 2>&1 &
botpid=$!
if wait_line "$log" 'READY stop-chat' 120; then
    docker compose stop -t 10 chat >/dev/null 2>&1 && echo "  chat stopped"
    if wait_line "$log" 'READY start-chat' 120; then
        docker compose start chat >/dev/null 2>&1 && echo "  chat started"
    else
        ok=1
        docker compose start chat >/dev/null 2>&1
    fi
else
    ok=1
fi
wait $botpid || ok=1
grep -v -E '^ *(Container|Network) ' "$log"
wait_healthy chat || ok=1
record "$name" $ok "see $log"

# Chat text never reaches the chat service's log at info (its own lines carry no text field).
name="38 chat service log: no chat text at info level"
if docker compose logs --no-color chat 2>&1 | grep -E 'psst, cave dweller|hello General|WTS sword|free at last' >/dev/null; then
    record "$name" 1 "chat text found in docker compose logs chat"
else
    record "$name" 0 ""
fi

instances_scenario
instance_restart
layer_scenario

admin_scenarios
summary
