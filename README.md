# EBView — 现代化构建版

把 Debian 曾经收录、后来因 GTK 2 被弃用而移除的 `ebview`，重新搬到现代 Linux 上编译并运行。

## 背景

`ebview` 是須藤賢一（Kenichi Suto）编写的 EPWING CD-ROM 词典阅读器，基于
[EB Library](http://www.sra.co.jp/people/m-kasahr/eb/) (`libeb`) 与 GTK 2，
支持多词典串刺搜索、X selection 自动取词、全文/条件检索、外字与图片显示等。

- Debian 最后一个版本是 `0.3.6.2-3`。
- 2020 年因依赖已废弃的 GTK 2 被报告 [Bug #967325](https://bugs.debian.org/967325)，
  2023 年随 [Bug #1035776](https://bugs.debian.org/1035776) 从 unstable 移除。
- 上游 SourceForge 项目自 0.3.6 起停止维护。本仓库以 GitHub 上的
  [`fujii/ebview`](https://github.com/fujii/ebview) 上游源码镜像为基线
  （GPL-2.0-or-later），原始归档完整保存在 `upstream/`，便于逐行对照。

## 这版做了什么

| 项目 | 原版 | 本版 |
| --- | --- | --- |
| 构建系统 | autotools + 已失效的 `eb_LIB_EB4` / `AM_PATH_GTK_2_0` m4 宏 | CMake + `pkg-config` + `find_library`，`config.h` 自动生成 |
| 编译器 | 仅适配老 gcc | 修复 K&R 声明、缺失原型、函数指针签名，适配现代 gcc / C11 |
| 字体度量 | `pango_xft_get_font_map()` + 裸 X11 `Display` | `pango_cairo_font_map_get_default()`，去掉 Xft 依赖 |
| 外部程序默认值 | `gnome-moz-remote`、`plaympeg`、`playwave`、`emacs`（现代系统均已不存在） | `xdg-open` |
| 首次运行复制配置 | `system("cp ...")`，路径含空格即出错且依赖 coreutils | GLib `g_file_get_contents` / `g_file_set_contents` |
| 数据目录 | 写死 `configure --prefix` | 自动探测可执行文件旁的数据目录，并支持 `EBVIEW_DATADIR` 覆盖 |
| 安装集成 | Debian 补丁里手工维护 | `cmake --install` 安装二进制、数据、帮助文档、日语翻译、图标与 `.desktop` |
| 构建入口 | `./autogen.sh && ./configure && make` | `./build.sh` |
| 默认字体 | `Kochi Mincho` / `Kochi Gothic`（已停用） | `Noto Serif CJK JP` / `Noto Sans CJK JP`；旧配置中的默认字体会自动迁移，用户自定义字体不受影响 |

另外修复了 web 搜索 URL 拼接中的自重叠 `sprintf`（UB）和字体大小格式化的缓冲区溢出。

## 当前状态与限制

**已实测**：Debian `forky/sid`、GTK 2.24.33、CMake 4.3、GCC 16.2 下从零完整编译链接通过
（0 error、无未定义符号）；在 X11 会话中启动后进入主事件循环、无报错，并正确生成全套用户配置；
`cmake --install` 的完整安装树也已验证。

仍需说明的边界：

- **UI 仍使用 GTK 2**。GTK 2 在现代 Debian 中依然提供且可运行（Wayland 会话下经 XWayland），
  但已属 legacy：无原生 Wayland 支持、无 HiDPI 缩放。
- 之所以没有直接换成 GTK 3：探测编译显示有约 488 处错误，其中自定义
  `GtkCellRendererEbook`（从 GTK 2.0.9 复制而来）的虚函数签名、以及
  `GdkPixmap`/`GdkGC` 绘图管线在 GTK 3 中已被 cairo 完全取代，
  属于重写渲染层而非机械替换。强行移植会破坏词典正文/外字渲染。
  本版选择先保证「功能完整且真的能跑」，GTK 3 化可作为后续独立工作。
- 程序内部沿用 EUC-JP 文本处理设计，界面文案在 UTF-8 locale 下的表现依赖原版转换逻辑。
- `X selection` 自动取词依赖 X11/XWayland。
- 本仓库不含任何 EPWING 词典数据，需自行提供词典目录。

## 依赖

Debian / Ubuntu：

```sh
sudo apt install build-essential cmake pkg-config \
                 libgtk2.0-dev libeb16-dev zlib1g-dev gettext
```

`libeb16-dev` 提供 EB Library 头文件与 `libeb`；GTK 2 开发包提供
`gtk+-2.0`、`gthread-2.0` 与 Pango/Cairo。`gettext`（`msgfmt`）用于可选的日文翻译，
缺失时构建会自动跳过。

## 构建与运行

```sh
./build.sh                       # 生成 build/
```

直接运行即可，程序会自动探测可执行文件旁的 `share/ebview`，无需安装：

```sh
./build/ebview
```

如需手动指定数据目录（例如移动过构建产物），仍可用 `EBVIEW_DATADIR` 覆盖：

```sh
EBVIEW_DATADIR="$PWD/build/share/ebview" ./build/ebview
```

或用真实 HOME 之外的隔离目录做测试：

```sh
HOME=/tmp/ebview-home ./build/ebview
```

安装到系统：

```sh
sudo cmake --install build       # 默认 prefix /usr/local
ebview                           # 也可从桌面菜单启动 "EBView"
```

自定义安装前缀：

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/opt/ebview
cmake --build build
sudo cmake --install build
```

## 打包

Debian / Ubuntu 二进制包。脚本会用一个独立的 `build-deb/` 以 `--prefix=/usr`
重新配置并编译（程序里的 `LOCALEDIR`、`PACKAGEDIR` 是编译期写死的，
用 `/usr/local` 前缀的构建树打包装到 `/usr` 会找不到数据），然后生成
`control`、`md5sums`、`postinst`/`postrm` 与 `copyright`：

```sh
bash tools/make_deb.sh                  # -> build/dist/ebview_<version>_<arch>.deb
sudo apt install ./build/dist/ebview_0.4.1_amd64.deb
```

发布用源码 tarball，内容与 git 标签完全一致（排除仅作对照用的 `upstream/`）：

```sh
git archive --format=tar --prefix=ebview-0.4.1/ v0.4.1 -- . ':(exclude)upstream' \
    | xz -9e > build/dist/ebview-0.4.1.tar.xz
```

Windows 免安装包，需在 MSYS2 环境下执行（会收集依赖 DLL、gdk-pixbuf 加载器与
fontconfig 配置）。默认按 UCRT64 工具链打包；要出 **Windows 7 可用**的包，需要换
MINGW64 工具链单独编译，再用环境变量覆盖工具链前缀与构建目录：

```sh
# UCRT64：Windows 10/11
bash tools/pack_win.sh                  # -> build/dist/EBView-<version>-win64-ucrt64.zip

# MINGW64：Windows 7 及以上
UCRT=/mingw64 BUILD=/e/git/EBView/build-mingw64 \
    bash tools/pack_win.sh E:/git/EBView/build-mingw64/dist
                                        # -> ...-win64-mingw64.zip
```

两套 CRT 的差别就在系统要求：UCRT64 包依赖 `ucrtbase.dll`，只能在 Windows 10 及以上
运行；MINGW64 包用的是 msvcrt，Windows 7 也能跑，介意老系统兼容时发这个。

### 发版清单

版本号只在 `CMakeLists.txt` 的 `VERSION` 一处改，它同时驱动 `PACKAGE_VERSION`、
AppStream 元数据和 Windows 的 VERSIONINFO，所以不用再改别的地方。

```sh
# 1. 改版本号：CMakeLists.txt 的 VERSION、NEWS 新增一段、README 里的示例命令；然后编译手测
./build.sh

# 2. 提交、打标签、推送
git commit -am "chore(release): bump version to 0.4.1"
git tag -a v0.4.1 -m "EBView 0.4.1"
git push origin main v0.4.1

# 3. 打包（deb 与源码包在 Linux 侧，Windows zip 在 MSYS2 侧产出后拷进来）
bash tools/make_deb.sh
git archive --format=tar --prefix=ebview-0.4.1/ v0.4.1 -- . ':(exclude)upstream' \
    | xz -9e > build/dist/ebview-0.4.1.tar.xz

# 4. 校验和：在 build/dist/ 内、只列本次要发布的文件
sha256sum ebview_0.4.1_amd64.deb ebview-0.4.1.tar.xz \
          EBView-0.4.1-win64-ucrt64.zip EBView-0.4.1-win64-mingw64.zip > SHA256SUMS
sha256sum -c SHA256SUMS

# 5. 发布（需要 gh 且已 gh auth login）
gh release create v0.4.1 --title "EBView 0.4.1" --notes-file /tmp/relnotes.md \
    build/dist/ebview_0.4.1_amd64.deb build/dist/ebview-0.4.1.tar.xz \
    build/dist/EBView-0.4.1-win64-ucrt64.zip \
    build/dist/EBView-0.4.1-win64-mingw64.zip build/dist/SHA256SUMS

# 6. 核对附件都在
gh release view v0.4.1 --json assets -q '.assets[] | "\(.name)  \(.state)"'
```

几个容易踩的点：

- 标签要先打再 `git archive`，否则 `v0.4.1` 解析不到；标签一旦推送就别再改指向，
  重发只能往上加版本；
- `data/` 下的文件由 `file(COPY ...)` 在 **configure** 阶段拷进
  `build/share/ebview/`，改了 `data/shortcut.xml` 之类必须重跑 `./build.sh`，
  只 `cmake --build` 不会刷新；
- 用户配置 `~/.ebview/*.xml` 只在文件缺失时才从安装目录复制，**新增的默认快捷键
  对老用户不生效**，发布说明里要写清（删文件重生成，或到设置里手动绑定）；
- 发布前清掉 `build/dist/` 里的旧版本产物，避免和 `SHA256SUMS` 对不上；该目录
  不入库（`.gitignore` 忽略了 `build/`）。

首次运行会在 `~/.ebview/` 生成 `preference.xml`、`dictgroup.xml`、
`shortcut.xml`、`searchengines.xml`、`filter.xml` 等配置文件。

## 加载 EPWING 词典

最方便的方式是 **`File → Search Disk for Dictionaries...`**：直接挑一个文件夹，
程序就递归扫描整棵目录树找 EPWING/EB 辞典（2002 年原版让人手填路径 + 小深度，是因为
当年硬盘慢；现在 SSD 全盘递归反而更快也更省心）。找到的辞典会加入
`Disk Search Result` 组，并**立即写入** `dictgroup.xml`、刷新词典工具条。

原来的路径也保留：`Tools → Options → Dictionary Group` 底部的 `Path` / `Depth` /
`Search Disk`，两处共用同一套扫描逻辑；这一处的结果在点 `Ok` 关闭设置时保存。

`Depth` 语义（两处一致）：`0` = 递归搜索所有子目录（默认）、
`1` = 只搜所选文件夹、`2` = 所选文件夹及其子文件夹，依此类推。
递归有 64 层硬上限，防止符号链接成环导致死循环。

也可直接编辑 `~/.ebview/dictgroup.xml`。Debian 仓库中的 `edict-fpw` 等包提供可用的
EPWING 数据。支持同时挂载多本词典进行串刺搜索。

## 菜单栏隐藏后怎么恢复

菜单里可以关掉 `Menu Bar`，但关掉之后就没菜单可用了。任选一种方式恢复：

- **`Ctrl+M`**：默认快捷键，绑定到 `Toggle Menu Bar` 命令。它是 `Tools → Options → Shortcut`
  里的普通条目，可以随意改键或删除（不再是硬编码）。快捷键通过挂在主窗口上的
  accel group 分发，所以菜单栏隐藏期间依然有效。
- **右键 / 中键点击正文区域 → `Display` → `Menu bar`**：弹出菜单里的这一项只负责显示，不会再隐藏。
- **改配置**：先**完全退出程序**，再编辑 `~/.ebview/preference.xml`，把
  `<show_menu_bar>0</show_menu_bar>` 改成 `1`。必须先退出，否则程序退出时
  `save_preference()` 会把当前状态写回去，覆盖你的修改。

## 命令行用法

除了 GUI，本版还提供两个命令行工具，便于在终端或管道里使用。

### `ebview --exact-terminal WORD`

对**当前激活辞典组里的所有辞典**做「完全一致」搜索，把每个命中词条的
EB 流式标记原文（`<gaiji>`、`<indent>`、`<keyword>`、`<sup>` 等）打到
标准输出，然后直接退出、不启动 GUI，也**不会**改动「上次使用的搜索模式」
（`last_search_method`）。设计上刻意保留原始 EB 标记（类似 `sdcv`），
方便用户再用管道（`sed` / `grep` / `awk` / `html2text` 之类）进一步处理。

```sh
ebview --exact-terminal 日本 | less -R
```

### `ebview-format`（包装脚本）

`tools/ebview-format` 把上面 `--exact-terminal` 的原始输出排版成美观的
终端文本：把 EB 标记转成**语义化颜色 / 加粗**而非简单删掉，外字
`<gaiji code=...>` 显示为 `[code]` 占位（整体不被折行切断），并按显示宽度
（中日韩全角算 2 格）软折行。

| EB 标记 | 终端渲染 |
| --- | --- |
| `<keyword>` | 加粗 + 黄（词头 / 关键词） |
| `<gaiji code=...>` | 品红 `[code]` 占位 |
| `<sup>` / `<sub>` | 青色 + 上标 / 下标字符 |
| `<em>` | 下划线 |
| `<reference>` | 蓝色（交叉引用） |
| `<indent position=N>` | 前导缩进 + 续行对齐 |
| `=== 标题 ===` | 加粗青色 |

用法：

```sh
ebview --exact-terminal 日本 | ebview-format          # 作为过滤器
ebview-format 日本                                      # 直接查词（内部调用 ebview）
ebview-format -w 60 日本                                # 指定折行宽度
ebview-format --no-color 日本 | sed ...                 # 关颜色便于再管道
EBVIEW_BIN=/path/to/ebview ebview-format 日本           # 指定 ebview 路径
```

配套还有 `ebdump`：一个从上游搬来的纯命令行 EB 文本区块底层 dump 工具
（`ebdump <book-path> <subbook-index> <block-number(hex)>`），用于查 EB 光盘
原始字节，不做任何词条解析。

## 默认快捷键

`Tools → Options → Shortcut` 里所有快捷键都可增删、改键。默认值定义在
`data/shortcut.xml`（首次运行复制到 `~/.ebview/shortcut.xml`）：

| 键 | 命令 |
| --- | --- |
| `Ctrl+M` | Toggle Menu Bar（显示 / 隐藏菜单栏） |
| `Ctrl+=` | Increase Font Size（增大正文字号） |
| `Ctrl+-` | Decrease Font Size（减小正文字号） |
| `Enter` | Start Search（**搜索框**里按回车开始检索） |
| `Enter` | Search Selected Text（**释义区**选中文字后按回车检索该词） |
| `Ctrl+N` / `Ctrl+P` | Next Hit / Previous Hit |
| `Ctrl+Q` | Quit Program |
| `F1`–`F9` | 切换各种搜索模式 |

其中 `Ctrl+M`、`Ctrl+=`、`Ctrl+-`、`Search Selected Text` 是本版新增的默认绑定。
前三个命令上游就有（`Toggle Menu Bar` / `Increase Font Size` /
`Decrease Font Size`），只是原来 `shortcut.xml` 没给它们分配按键。

两个 `Enter` 是**同键不同上下文**：`Start Search` 在搜索框里生效，
`Search Selected Text` 在"焦点在释义区且存在选区"时生效。两者都是
`Options → Shortcut` 里的普通条目，**可以改键或删除**：

- 裸 `Return` 永远不注册成全局加速键，否则会抢走搜索框 / 对话框的回车。
- `Start Search` 的回车由 `GtkEntry` 的 `activate` 信号触发，但回调会先查配置：
  只有该命令当前仍绑定裸 Enter 才执行。把它改绑到比如 `F5`，回车就不再检索，
  `F5` 由 accel group 全局接管；删掉该条目则回车彻底不检索。
- `Search Selected Text` 由主窗口按键处理按配置匹配，同样可改键 / 删除。

以前这两处（搜索框回车、释义区选中文字回车）都是硬编码，且 `Start Search` 在
快捷键设置里是个改了也没用的死条目；现在都走同一套可配置机制。

命令表里还有这些**存在但没有默认按键**的操作，需要时自行在 `Options → Shortcut`
里添加：

`Copy To Clipboard`、`Paste From Clipboard`、`Expand Lines`、`Shrink Lines`、
`Show Previous Text`、`Show Next Text`、`Toggle Status Bar`、
`Toggle Dictionary Bar`、`Iconify Window`。

**为什么 `Ctrl+C` / `Ctrl+V` 不设默认快捷键**：`GtkEntry` / `GtkTextView` 内建就
处理这两个键（复制 / 粘贴）。给命令表里的 `Copy To Clipboard` /
`Paste From Clipboard` 绑同一个键，全局 accel 会**抢占** GTK 内建而不是共存——实测
绑了 `Ctrl+C` 后，在搜索框选中文字按 Ctrl+C 复制不到任何东西。所以默认不绑；
`Copy To Clipboard` 仍可从释义区右键菜单使用，也可以自行绑到一个不冲突的键。

> 注：`~/.ebview/shortcut.xml` 一旦生成就不会被新默认配置覆盖。老配置若想获得
> 这几个默认键，在 `Options → Shortcut` 里手动添加即可。

## 目录结构

```
CMakeLists.txt        现代构建定义
config.h.in           由 CMake 生成的配置头模板
build.sh              一键构建
tools/                打包脚本（make_deb.sh / pack_win.sh）
src/                  翻新后的源码（约 3 万行）
data/ pixmaps/ doc/   运行时数据、图标、帮助文档
po/                   日文翻译（ja.po）
desktop/              桌面入口
upstream/             未经改动的原始源码归档（git clone，含完整提交历史）
build/                构建产物（生成）
```

## 许可证

GPL-2.0-or-later，见 `COPYING`。原作者須藤賢一，上游版权声明均保留在源码中。
