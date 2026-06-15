#pragma once
// =============================================================================
// Steam integration wrapper for MassRTS
// -----------------------------------------------------------------------------
// Compiles to a NO-OP unless MASSRTS_USE_STEAM is defined AND the Steamworks
// SDK headers/libs are present. This lets the game build & run without Steam
// during development, then enable Steam for the shipping build.
//
// ENABLE STEAM:
//   1. Download Steamworks SDK, copy sdk/public/steam -> third_party/steam/
//   2. Link steam_api64.lib; ship steam_api64.dll next to the exe.
//   3. Put steam_appid.txt (containing your AppID) next to the exe during dev.
//      For test/dev you may use AppID 480 (SpaceWar).
//   4. Define MASSRTS_USE_STEAM (e.g. target_compile_definitions in CMake).
//
// USAGE:
//   SteamIntegration steam;
//   if (!steam.init(MY_APPID)) { /* not launched via Steam, or SDK missing */ }
//   ... main loop ...
//   steam.run_callbacks();          // every frame
//   steam.unlock_achievement("FIRST_WIN");
//   steam.set_stat("units_killed", n);
//   ... on exit ...
//   steam.shutdown();
// =============================================================================

#include <cstdint>
#include <string>
#include <cstdio>

#ifdef MASSRTS_USE_STEAM
  #include "steam/steam_api.h"
#endif

class SteamIntegration {
public:
    bool available = false;

    // Returns true if Steam is running and we are properly attached.
    // app_id: your Steamworks AppID. If the game was NOT started through Steam,
    // RestartAppIfNecessary relaunches it via Steam and we return false here.
    bool init(uint32_t app_id) {
#ifdef MASSRTS_USE_STEAM
        // If not launched by Steam, ask Steam to relaunch us (then this process
        // exits). Skip this branch if you provide steam_appid.txt during dev.
        if (SteamAPI_RestartAppIfNecessary(app_id)) {
            // Returning false signals caller to exit; Steam will relaunch.
            return false;
        }
        if (!SteamAPI_Init()) {
            printf("[Steam] SteamAPI_Init failed (is Steam running?)\n");
            available = false;
            return false;
        }
        available = true;
        printf("[Steam] Initialized. User: %s\n",
               SteamFriends() ? SteamFriends()->GetPersonaName() : "?");
        return true;
#else
        (void)app_id;
        available = false;
        return false;
#endif
    }

    void run_callbacks() {
#ifdef MASSRTS_USE_STEAM
        if (available) SteamAPI_RunCallbacks();
#endif
    }

    // ---- Achievements ------------------------------------------------------
    void unlock_achievement(const char* api_name) {
#ifdef MASSRTS_USE_STEAM
        if (!available || !SteamUserStats()) return;
        SteamUserStats()->SetAchievement(api_name);
        SteamUserStats()->StoreStats();
#else
        (void)api_name;
#endif
    }

    // ---- Stats -------------------------------------------------------------
    void set_stat(const char* api_name, int32_t value) {
#ifdef MASSRTS_USE_STEAM
        if (!available || !SteamUserStats()) return;
        SteamUserStats()->SetStat(api_name, value);
        SteamUserStats()->StoreStats();
#else
        (void)api_name; (void)value;
#endif
    }

    void add_stat(const char* api_name, int32_t delta) {
#ifdef MASSRTS_USE_STEAM
        if (!available || !SteamUserStats()) return;
        int32_t cur = 0;
        SteamUserStats()->GetStat(api_name, &cur);
        SteamUserStats()->SetStat(api_name, cur + delta);
        SteamUserStats()->StoreStats();
#else
        (void)api_name; (void)delta;
#endif
    }

    // ---- Identity ----------------------------------------------------------
    std::string persona_name() const {
#ifdef MASSRTS_USE_STEAM
        if (available && SteamFriends()) return SteamFriends()->GetPersonaName();
#endif
        return "Player";
    }

    uint64_t steam_id() const {
#ifdef MASSRTS_USE_STEAM
        if (available && SteamUser()) return SteamUser()->GetSteamID().ConvertToUint64();
#endif
        return 0;
    }

    void shutdown() {
#ifdef MASSRTS_USE_STEAM
        if (available) { SteamAPI_Shutdown(); available = false; }
#endif
    }
};

// -----------------------------------------------------------------------------
// NEXT STEPS for Steam multiplayer (replace/augment the UDP transport):
//   * ISteamNetworkingSockets gives NAT punch-through + Steam relay (SDR) so
//     players join via lobby/friend invite with no manual IP / port-forwarding.
//   * ISteamMatchmaking for lobbies + invites.
//   * Keep the NetSession lockstep layer; only swap the byte transport
//     (UDPSocket -> SteamNetworkingSockets) underneath it.
//   * ISteamUGC for Steam Workshop (custom maps/MODs; see CUSTOMIZATION.md).
//   * ISteamRemoteStorage for cloud saves.
// -----------------------------------------------------------------------------
