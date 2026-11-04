// chthonic_bot: headless test client for the world server.
//   chthonic_bot --scenario <name> [options]                       one integration scenario, prints PASS/FAIL
//   chthonic_bot --mode load --count 500 --behaviour wander [...]  many sessions in one process
#include "crash_dialogs.h"
#include "load.h"
#include "scenarios.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace chthonic::bot;

namespace {

void usage() {
    std::printf(
        "usage: chthonic_bot [--scenario NAME | --mode load] [options]\n"
        "  --auth-url URL        auth service (default http://localhost:3100)\n"
        "  --mailpit-url URL     Mailpit API (default http://localhost:8026)\n"
        "  --host HOST --port N  world server (default localhost 7777)\n"
        "  --ca FILE             certificate/CA the server must present (world and chat)\n"
        "  --chat-host HOST --chat-port N --chat-link-port N  chat service (default localhost 7778 7779)\n"
        "  --admin-port N        the world server's admin port (default 7780; secret from WORLD_ADMIN_SECRET)\n"
        "  --run-id ID           namespaces bot accounts (default run)\n"
        "  --count N             load: bot sessions (default 1)\n"
        "  --behaviour B         load: idle | wander | chat | speedhack | malformed (default wander)\n"
        "  --duration S          load: seconds after the last bot connected (default 60)\n"
        "  --threads N           load: io threads (default 4)\n"
        "  --connect-rate N      load: new sessions per second (default 50)\n"
        "  --maps 1,2,4,5        load: bot i walks through the portals of --map-root's maps to the i-th listed map\n"
        "                        (round robin), then wanders there clear of the portals\n"
        "  --instance-size N     load: bots per instance copy on an instance map in --maps (default its maxPlayers,\n"
        "                        else 10), each group with its own instance key (the sample test_hooks.lua's)\n"
        "  --zone M              load: wanderers stay in the square 0..M on x and z (default 2048)\n"
        "  --terrain DIR         load: wanderers walk at the ground height of that map folder (its map.json and heights)\n"
        "  --auth-key FILE       handshake scenario: auth's private key, to mint an expired token (test only)\n"
        "  --expect-pos X,Y,Z    restart-verify scenario\n"
        "  --map-root DIR        terrain/navmesh scenarios: the server's maps/ folder (ground heights)\n"
        "  --expect-components G,H  components-verify scenario: Wallet gold and Health current before the restart\n"
        "  --tag T               shutdown/restart-verify scenarios: account tag\n"
        "  --char-option K=V     options for the characters it creates (repeatable), e.g. class=warrior\n"
        "scenarios: auth characters ownership aoi chat speedhack malformed handshake duplicate persist\n"
        "           shutdown restart-verify dbdown components components-verify events fsm npcs\n"
        "           portal respawn terrain navmesh client-events client-event-rate\n"
        "           yell chat-whisper chat-channels chat-zone chat-ignore chat-mute-setup chat-mute chat-limits\n"
        "           chat-auth chat-down (the chat ones need the chat service; see tests/integration.sh)\n"
        "           admin admin-off (the world's admin port on / off) instances (maps/crypt, admin port optional)\n");
}

} // namespace

int main(int argc, char** argv) {
    chthonic::disableCrashDialogsIfRequested();
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--auth-url") o.auth.authUrl = val();
        else if (a == "--mailpit-url") o.auth.mailpitUrl = val();
        else if (a == "--host") o.host = val();
        else if (a == "--port") o.port = static_cast<uint16_t>(std::stoi(val()));
        else if (a == "--ca") o.caFile = val();
        else if (a == "--chat-host") o.chatHost = val();
        else if (a == "--chat-port") o.chatPort = static_cast<uint16_t>(std::stoi(val()));
        else if (a == "--chat-link-port") o.chatLinkPort = static_cast<uint16_t>(std::stoi(val()));
        else if (a == "--admin-port") o.adminPort = static_cast<uint16_t>(std::stoi(val()));
        else if (a == "--run-id") o.runId = val();
        else if (a == "--mode") o.mode = val();
        else if (a == "--scenario") o.scenario = val();
        else if (a == "--behaviour") o.behaviour = val();
        else if (a == "--count") o.count = std::stoi(val());
        else if (a == "--duration") o.durationSec = std::stoi(val());
        else if (a == "--threads") o.threads = std::stoi(val());
        else if (a == "--connect-rate") o.connectRatePerSec = std::stoi(val());
        else if (a == "--auth-key") o.authKeyFile = val();
        else if (a == "--expect-pos") o.expectPos = val();
        else if (a == "--map-root") o.mapRoot = val();
        else if (a == "--tag") o.extra["tag"] = val();
        else if (a == "--expect-components") o.extra["expect-components"] = val();
        else if (a == "--zone") o.extra["zone"] = val();
        else if (a == "--terrain") o.extra["terrain"] = val();
        else if (a == "--maps") o.extra["maps"] = val();
        else if (a == "--instance-size") o.extra["instance-size"] = val();
        else if (a == "--char-option") {
            const std::string kv = val();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "--char-option wants KEY=VALUE, got %s\n", kv.c_str());
                return 2;
            }
            o.charOptions[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    try {
        if (o.mode == "load") return run_load(o);
        if (o.scenario.empty()) { usage(); return 2; }
        return run_scenario(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
