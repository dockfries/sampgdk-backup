/* open.mp example plugin for sampgdk 5.0.0.
 *
 * Demonstrates the open.mp additions:
 *   - calling open.mp natives (omp_player.h, omp_gangzone.h, ...)
 *   - receiving open.mp callbacks (OnNPCCreate, OnPlayerEnterGangZone, ...)
 *
 * This is also built by CI as a compile smoke-test for the open.mp modules.
 */

#define SAMPGDK_CPP_WRAPPERS

#include <sampgdk/a_players.h>
#include <sampgdk/a_samp.h>
#include <sampgdk/core.h>
#include <sampgdk/sdk.h>
#include <sampgdk/omp_gangzone.h>
#include <sampgdk/omp_npc.h>
#include <sampgdk/omp_player.h>

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports() {
  return sampgdk::Supports() | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void **ppData) {
  return sampgdk::Load(ppData);
}

PLUGIN_EXPORT void PLUGIN_CALL Unload() {
  sampgdk::Unload();
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick() {
  sampgdk::ProcessTick();
}

PLUGIN_EXPORT bool PLUGIN_CALL OnGameModeInit() {
  sampgdk::logprintf("omp_example: OnGameModeInit (sampgdk 5.0.0)");
  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerConnect(int playerid) {
  /* open.mp native: allow weapons for this player. */
  sampgdk::AllowPlayerWeapons(playerid, true);

  /* open.mp native: read player gravity. */
  float gravity = sampgdk::GetPlayerGravity(playerid);
  sampgdk::logprintf("omp_example: player %d gravity %f", playerid, gravity);

  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerSpawn(int playerid) {
  /* open.mp native: create a gangzone. */
  int zoneid = sampgdk::CreatePlayerGangZone(playerid, 1000.0f, 1000.0f,
                                             2000.0f, 2000.0f);
  if (zoneid != 0) {
    sampgdk::PlayerGangZoneShow(playerid, zoneid, 0x00FF00FF);
  }
  return true;
}

/* open.mp callbacks (new in sampgdk 5.0.0). */
PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerEnterGangZone(int playerid, int zoneid) {
  sampgdk::logprintf("omp_example: player %d entered gangzone %d",
                     playerid, zoneid);
  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnNPCCreate(int npcid) {
  sampgdk::logprintf("omp_example: NPC %d created", npcid);
  return true;
}
