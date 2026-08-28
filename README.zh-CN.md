[GDK - 面向 C/C++ 的游戏模式 SDK][github]
============================================

[English](README.md) | **简体中文**

简介
------

GDK（游戏模式开发套件，Gamemode Development Kit）是一个 C/C++ 库，允许你用
C/C++ 编写 SA-MP 游戏模式。它镜像了 SA-MP 服务器提供的 Pawn 脚本 API，并让你
以类似的方式处理各种 SA-MP 事件（即回调）。

如果你没有耐心，[这里](plugins/helloworld/helloworld.cpp) 是用 C++ 编写时的样子：

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

构建说明
----------

本仓库使用了 git 子模块，因此克隆时请加上 `--recursive`（或者在普通克隆后
执行 `git submodule update --init --recursive`）：

```sh
git clone --recursive https://github.com/dockfries/sampgdk-backup.git
```

要构建 GDK，你首先需要下载并安装以下依赖：

* [SA-MP 插件 SDK][sdk]（以子模块形式位于 `deps/` 下）
* [open.mp Pawn 库][omp_stdlib]（以子模块形式位于 `deps/omp-stdlib` 下，
  供代码生成脚本使用）
* [CMake][cmake] 3.5+
* [Python][python] 3.x
* [PLY][ply]（Python Lex-Yacc），可以通过 [pip][pip] 安装
* C 编译器
* C++ 编译器（可选，用于构建示例插件）

安装完所有依赖后，你可以使用以下命令构建并安装这个库：

```sh
cd path/to/sampgdk
mkdir build && cd build
cmake .. -DSAMP_SDK_ROOT=path/to/sdk
cmake --build . --config Release
cmake --build . --config Release --target install
```

你可以向 CMake 传入额外的参数，并修改以下一个或多个选项：

* `SAMPGDK_STATIC`             - 构建为静态库（默认是 OFF）
* `SAMPGDK_BUILD_PLUGINS`      - 构建示例插件（默认是 OFF）
* `SAMPGDK_BUILD_AMALGAMATION` - 构建合并（amalgamation）文件（默认是 OFF）
* `SAMPGDK_BUILD_DOCS`         - 构建 Doxygen 文档（默认是 ON）
* `SAMPGDK_TINY`               - 精简构建：只生成回调，不生成 IDL native
                                 （默认是 OFF）
* `SAMPGDK_ARCH`               - 目标架构：32 或 64（默认 32）

例如，将 GDK 构建为静态库并同时构建示例插件：

```sh
cmake .. -DSAMPGDK_STATIC=ON -DSAMPGDK_BUILD_PLUGINS=ON
```

以下内置变量可能也很有用：

* `CMAKE_BUILD_TYPE`     - 构建类型：Debug、Release、RelWIthDebInfo、
                           MinSizeRel
* `CMAKE_INSTALL_PREFIX` - 安装文件的位置

如需了解关于 CMake 的更多信息或有疑问，请阅读
[CMake FAQ][cmake_faq]。

快速上手
----------

你可以从下载源码开始，先玩玩 [helloworld][helloworld] 插件。如果你需要文档，
可以查看 [这里][online_docs] 的文档，也可以在 GDK 的头文件中查看。

如果你想创建一个新项目，仓库中的 [doc/](doc/) 目录提供了关于 CMake 的说明
（包括 Doxygen 文档构建和合并文件的使用方法）。即使你之前完全没有 CMake
经验，也可以照着做。

### 使用 Git

如果你熟悉 Git，并且已经成功构建过这个库，那么上手的最简单方式大概是克隆
本仓库（如果还没克隆的话），然后为你的个人项目创建一个新的本地分支：

```
git clone --recursive https://github.com/dockfries/sampgdk-backup.git
git checkout -b my-project
```

然后就可以直接在 GDK 的源码树里开始工作了。你可以选择修改 helloworld
项目，或者在 `plugins/` 目录下另建一个文件夹来创建新项目。推荐后者，因为
如果 helloworld 在上游被更新了，这样可以避免可能的合并冲突。

之后如果你决定要更新这个库，比如说更新到 v1.2.3 版本，你只需从上游拉取
master 分支，并将改动合并到你的项目分支中：

```
git fetch origin master
git merge v1.2.3
```

许可证
--------

基于 Apache License 2.0 版本授权。参见 LICENSE.txt 文件。

[github]: https://github.com/dockfries/sampgdk-backup
[version]: https://github.com/dockfries/sampgdk-backup/releases
[sdk]: https://github.com/AmyrAhmady/samp-plugin-sdk
[omp_stdlib]: https://github.com/openmultiplayer/omp-stdlib
[cmake]: https://cmake.org/
[python]: https://www.python.org/
[ply]: https://pypi.org/project/ply/
[pip]: https://pypi.org/project/pip/
[helloworld]: plugins/helloworld/helloworld.cpp
[online_docs]: doc/
[cmake_faq]: https://cmake.org/cmake/help/latest/faq.html
