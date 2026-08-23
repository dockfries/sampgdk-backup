sampgdk_add_subproject(blank)
if(NOT SAMPGDK_TINY)
  # helloworld uses IDL natives (SetTimer, SendClientMessage, ...), which
  # are not generated in the tiny build.
  sampgdk_add_subproject(helloworld)
  # omp_example requires open.mp natives, also excluded in the tiny build.
  sampgdk_add_subproject(omp_example)
endif()
sampgdk_add_subproject(ufs)
