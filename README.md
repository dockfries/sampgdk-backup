[GDK - Gamemode SDK for C/C++][github]
======================================

**English** | [简体中文](README.zh-CN.md)

Introduction
-------------

GDK (Gamemode Development Kit) is a C/C++ library that allows you to write
SA-MP gamemodes in C/C++. It mirrors the Pawn scripting API provied by
the SA-MP server and lets you handle various SA-MP events a.k.a callbacks
in a similar fashion.

For the impatient, [here](plugins/helloworld/helloworld.cpp) is what it
looks like in C++:

```c++
#include <stdio.h>
#include <string.h>

#include <sampgdk/a_players.h>
#include <sampgdk/a_samp.h>
#include <sampgdk/core.h>
#include <sampgdk/sdk.h>

void SAMPGDK_CALL PrintTickCountTimer(int timerid, void *params) {
  sampgdk::logprintf("Tick count: %d", GetTickCount());
}

PLUGIN_EXPORT bool PLUGIN_CALL OnGameModeInit() {
  SetGameModeText("Hello, World!");
  AddPlayerClass(0, 1958.3783f, 1343.1572f, 15.3746f, 269.1425f,
                 0, 0, 0, 0, 0, 0);
  SetTimer(1000, true, PrintTickCountTimer, 0);
  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerConnect(int playerid) {
  SendClientMessage(playerid, 0xFFFFFFFF, "Welcome to the HelloWorld server!");
  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerRequestClass(int playerid,
                                                    int classid) {
  SetPlayerPos(playerid, 1958.3783f, 1343.1572f, 15.3746f);
  SetPlayerCameraPos(playerid, 1958.3783f, 1343.1572f, 15.3746f);
  SetPlayerCameraLookAt(playerid, 1958.3783f, 1343.1572f, 15.3746f, CAMERA_CUT);
  return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerCommandText(int playerid,
                                                   const char *cmdtext) {
  if (strcmp(cmdtext, "/hello") == 0) {
    char name[MAX_PLAYER_NAME];
    GetPlayerName(playerid, name, sizeof(name));
    char message[MAX_CLIENT_MESSAGE];
    sprintf(message, "Hello, %s!", name);
    SendClientMessage(playerid, 0x00FF00FF, message);
    return true;
  }
  return false;
}
```

Build Instructions
------------------

The repository uses git submodules, so clone it with `--recursive` (or run
`git submodule update --init --recursive` after a plain clone):

```sh
git clone --recursive https://github.com/dockfries/sampgdk-backup.git
```

In order to build the GDK you first you need to download and install the
following dependencies:

* [SA-MP plugin SDK][sdk] (pulled in as a submodule under `deps/`)
* [open.mp Pawn library][omp_stdlib] (pulled in as a submodule under
  `deps/omp-stdlib`; used by the code generation scripts)
* [Zydis][zydis] x86/x64 decoder (pulled in as a submodule under
  `deps/zydis`; used by the hooking engine)
* [CMake][cmake] 3.5+
* [Python][python] 3.x
* [PLY][ply] (Python Lex-Yacc) can be installed via [pip][pip]
* C compiler (the vendored Zydis decoder requires C11 or later)
* C++ compiler (optional, for building example plugins)

Once all dependencies are installed you can use the following commands to
build and install the library:

```sh
cd path/to/sampgdk
mkdir build && cd build
cmake .. -DSAMP_SDK_ROOT=path/to/sdk
cmake --build . --config Release
cmake --build . --config Release --target install
```

You can pass additional arguments to CMake and change one or more of the
following options:

* `SAMPGDK_STATIC`        - Build as static library (default is OFF)
* `SAMPGDK_BUILD_PLUGINS` - Build example plugins (default is OFF)
* `SAMPGDK_BUILD_DOCS`    - Build Doxygen documentation (default is ON)
* `SAMPGDK_BUILD_AMALGAMATION` - Build single-file amalgamation (default OFF)
* `SAMPGDK_TINY`          - Tiny build: callbacks only, no IDL natives
                            (default is OFF)
* `SAMPGDK_ARCH`          - Target architecture: 32 or 64 (default 32)

For example, to build GDK as a static library together with example
plugins:

```sh
cmake .. -DSAMPGDK_STATIC=ON -DSAMPGDK_BUILD_PLUGINS=ON
```

### Using sampgdk as a git submodule

sampgdk is designed to be consumed from another CMake project via
`add_subdirectory`. Add it as a submodule and link the `sampgdk` target:

```sh
git submodule add https://github.com/dockfries/sampgdk-backup.git deps/sampgdk
git submodule update --init --recursive   # pulls sampgdk's own submodules
```

```cmake
add_subdirectory(deps/sampgdk)
target_link_libraries(my_target sampgdk)  # headers + Zydis propagate transitively
```

The `sampgdk` target propagates its include directories and its Zydis
dependency through `target_link_libraries`, so no extra include paths are
needed. The SA-MP plugin SDK is found under sampgdk's own `deps/` submodule
unless you set `SAMPSDK_DIR` or `SAMP_SDK_ROOT` before adding the directory.

### Using the single-file amalgamation

As an alternative to the submodule, sampgdk can be built into a single-file
amalgamation (`sampgdk.c` + `sampgdk.h`) that you drop into your own build:

```sh
cmake -DSAMPGDK_BUILD_AMALGAMATION=ON -B build -S .
cmake --build build --target sampgdk_amalgamate
# outputs build/lib/sampgdk/sampgdk.c and sampgdk.h
```

The amalgamation folds all of sampgdk's sources into one translation unit,
but it deliberately does **not** bundle Zydis: `hook.c` keeps its
`#include <Zydis/Zydis.h>`, so you must provide Zydis yourself. Add the
upstream submodule and link its static library:

```sh
git submodule add https://github.com/zyantific/zydis.git deps/zydis
git submodule update --init --recursive   # pulls zydis' own zycore submodule
```

```cmake
add_subdirectory(deps/zydis)
target_link_libraries(my_target sampgdk_c Zydis)  # or add sampgdk.c to your sources
# include dirs: your Zydis include/ (and zycore include/) plus the SA-MP SDK
```

Build `sampgdk.c` as C11 (same requirement as the submodule build).

The following built-in variables may also be useful:

* `CMAKE_BUILD_TYPE`     - Bulid type: Debug, Release, RelWIthDebInfo,
                           MinSizeRel
* `CMAKE_INSTALL_PREFIX` - Where to install files

For more information or questions about CMake please read the
[CMake FAQ][cmake_faq].

Getting Started
---------------

You can start with downloading the source code and playing a little bit with
the [helloworld][helloworld] plugin. In case you need documentation it's
available [here][online_docs], in a browsable form, as well as in
the GDK header files.

If you feel like making a new project there's
some information in the [doc/](doc/) directory on setting up a GDK project,
including building the Doxygen documentation. No prior knowledge of CMake is
required to follow it.

### Using Git

If you know Git and you've already managed to build the library successfully
the easiest way to get started is probably to clone this repo (if you haven't
done so) and create a new local branch for your personal project:

```
git clone --recursive https://github.com/dockfries/sampgdk-backup.git
git checkout -b my-project
```

and begin working on it right inside the GDK source tree. You could either
edit the helloworld project or create a new project in a separate folder under
the `plugins/` directory. The latter is recommended as it would avoid possible
merge conflicts if helloworld suddenly gets updated in upstream.

Later if you decide that it's time to update the library, say to version
v1.2.3, you would simply fetch master from upstream and merge the changes
into your project's branch:

```
git fetch origin master
git merge v1.2.3
```

License
-------

Licensed under the Apache License version 2.0. See the LICENSE.txt file.

[github]: https://github.com/dockfries/sampgdk-backup
[version]: https://github.com/dockfries/sampgdk-backup/releases
[sdk]: https://github.com/AmyrAhmady/samp-plugin-sdk
[omp_stdlib]: https://github.com/openmultiplayer/omp-stdlib
[zydis]: https://github.com/zyantific/zydis
[cmake]: https://cmake.org/
[python]: https://www.python.org/
[ply]: https://pypi.org/project/ply/
[pip]: https://pypi.org/project/pip/
[helloworld]: plugins/helloworld/helloworld.cpp
[online_docs]: doc/
[cmake_faq]: https://cmake.org/cmake/help/latest/faq.html

