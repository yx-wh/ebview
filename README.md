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
sudo apt install ./build/dist/ebview_0.4.0_amd64.deb
```

发布用源码 tarball，内容与 git 标签完全一致（排除仅作对照用的 `upstream/`）：

```sh
git archive --format=tar --prefix=ebview-0.4.0/ v0.4.0 -- . ':(exclude)upstream' \
    | xz -9e > build/dist/ebview-0.4.0.tar.xz
```

Windows 免安装包，需在 MSYS2 UCRT64 环境下执行（会收集依赖 DLL、gdk-pixbuf
加载器与 fontconfig 配置）：

```sh
bash tools/pack_win.sh                  # -> build/dist/EBView-win64.zip
```

首次运行会在 `~/.ebview/` 生成 `preference.xml`、`dictgroup.xml`、
`shortcut.xml`、`searchengines.xml`、`filter.xml` 等配置文件。

## 加载 EPWING 词典

在程序的偏好设置 → 词典组（Dictionary Group）中添加包含 `CATALOGS`
文件的词典目录即可；也可直接编辑 `~/.ebview/dictgroup.xml`。
Debian 仓库中的 `edict-fpw` 等包提供可用的 EPWING 数据。
支持同时挂载多本词典进行串刺搜索。

## 菜单栏隐藏后怎么恢复

菜单里可以关掉 `Menu Bar`，但关掉之后就没菜单可用了。任选一种方式恢复：

- **`Ctrl+M`**：硬编码的切换快捷键，不依赖快捷键配置，任何状态下都可用。
- **右键 / 中键点击正文区域 → `Display` → `Menu bar`**：弹出菜单里的这一项只负责显示，不会再隐藏。
- **改配置**：先**完全退出程序**，再编辑 `~/.ebview/preference.xml`，把
  `<show_menu_bar>0</show_menu_bar>` 改成 `1`。必须先退出，否则程序退出时
  `save_preference()` 会把当前状态写回去，覆盖你的修改。

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
