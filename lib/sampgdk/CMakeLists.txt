add_subdirectory(internal)

set(SAMPGDK_DEF_FILE ${CMAKE_CURRENT_BINARY_DIR}/sampgdk.def)
set(SAMPGDK_SYM_FILE ${CMAKE_CURRENT_BINARY_DIR}/sampgdk.sym)

sampgdk_add_sources(
  a_actor.idl
  a_http.idl
  a_objects.idl
  a_players.idl
  a_samp.idl
  a_samp.idl.c
  a_vehicles.idl
  core.api
  core.c
  interop.api
  interop.c
  sampgdk.rc
  version.api
  version.c
)

# open.mp natives/constants are excluded in the tiny build (callbacks only).
if(NOT SAMPGDK_TINY)
  sampgdk_add_sources(
    omp_actor.idl
    omp_checkpoint.idl
    omp_class.idl
    omp_core.idl
    omp_database.idl
    omp_dialog.idl
    omp_gangzone.idl
    omp_http.idl
    omp_menu.idl
    omp_network.idl
    omp_npc.idl
    omp_object.idl
    omp_pickup.idl
    omp_player.idl
    omp_textdraw.idl
    omp_textlabel.idl
    omp_variable.idl
    omp_vehicle.idl
    omp_consts.idl
  )
endif()

if(WIN32)
  sampgdk_add_sources(${SAMPGDK_DEF_FILE})
elseif(UNIX)
  sampgdk_add_sources(${SAMPGDK_SYM_FILE})
endif()

if(WIN32)
  sampgdk_set_property(SAMPGDK_DEF_FILE ${SAMPGDK_DEF_FILE})
elseif(UNIX)
  sampgdk_set_property(SAMPGDK_SYM_FILE ${SAMPGDK_SYM_FILE})
endif()
