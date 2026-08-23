file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/init.c "")

sampgdk_add_sources(
  amx.c
  amx.h
  amxhooks.c
  array.c
  array.h
  callback.c
  callback.h
  fakeamx.c
  fakeamx.h
  hook.c
  hook.h
  init.h
  likely.h
  log.c
  log.h
  logprintf.c
  logprintf.h
  native.c
  native.h
  param.c
  param.h
  plugin.c
  plugin.h
  timer.c
  timer.h
  types.h
)

# The init list drives generated init.c. The tiny build drops the open.mp
# modules, so generate a filtered init list for that case.
set(SAMPGDK_INIT_LIST ${CMAKE_CURRENT_SOURCE_DIR}/init.list)
if(SAMPGDK_TINY)
  set(SAMPGDK_INIT_LIST ${CMAKE_CURRENT_BINARY_DIR}/init_tiny.list)
  file(WRITE ${SAMPGDK_INIT_LIST} "")
  file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/init.list _init_lines)
  foreach(_line IN LISTS _init_lines)
    if(NOT _line MATCHES "omp_")
      file(APPEND ${SAMPGDK_INIT_LIST} "${_line}\n")
    endif()
  endforeach()
endif()

sampgdk_add_init_gen(sampgdk_init ${SAMPGDK_INIT_LIST}
                                  ${CMAKE_CURRENT_BINARY_DIR}/init.c)
