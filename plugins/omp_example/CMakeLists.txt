# This example uses open.mp natives; it is only added when not building tiny
# (see plugins/CMakeLists.txt).
include(AMXConfig)
include(AddSAMPPlugin)

if(MSVC)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

add_samp_plugin(omp_example
  omp_example.cpp
  omp_example.def
  ${SAMPSDK_DIR}/amxplugin.cpp
)

target_link_libraries(omp_example sampgdk)
