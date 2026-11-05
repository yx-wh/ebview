# File Search 现代化：问题与调试记录

> 记录 2026-11 给 EBView 的「File Search」（内置图形化 grep）补 UTF-8 支持时，
> 连续踩到的 6 个坑。每条包含现象、根因、修复和验证方式，既作为改动说明，
> 也作为后续维护这套 GTK2 老代码的参考。

## 背景

File Search 是 2001 年加入的旁支功能：递归扫描普通文本文件做全文检索，
命中行做成结果列表，点开能看文件内容并高亮关键词。它原本只认
EUC-JP / ISO-2022-JP / Shift_JIS，而现代文本基本都是 UTF-8，导致
非 ASCII 关键词在 UTF-8 文件里完全搜不到。

改造方向不是「把老编码换成 UTF-8」——那会丢掉检索老日文文本的能力——
而是**新增一条 UTF-8 通道，三种编码并存**。

## 1. UTF-8 文件里的非 ASCII 词搜不到

**现象**：搜「词」「辞書」等词，UTF-8 文件零命中；纯 ASCII 词正常。

**根因**（三处叠加）：

1. `start_search()` 把输入词 `utf-8 → euc-jp` 后才交给 `grep_search()`。
   JIS X 0208 之外的字符（简体中文、emoji）走 `replace_char()` 被替换成
   占位符，词在进入检索前就已经变形。
2. `grep_file()` 用 `guess_kanji()` 判文件编码，只返回
   `KCODE_EUC/JIS/SJIS/ASCII/UNKNOWN`；UTF-8 文件要么被误判，要么
   `(Unknown code) ... skipped.` 整份跳过。
3. `show_file()` 按老编码二次转码，UTF-8 内容显示为乱码。

**修复**：

- `start_search()` 的 File Search 分支保留原始 UTF-8 词，其它搜索方法仍走 EUC-JP
- `jcode.h` 增加 `KCODE_UTF8`
- `grep_search_thread()` 以 UTF-8 词为基准派生三套匹配表：
  `bmh_utf8/reg_utf8`（原样）、`bmh_euc/reg_euc`（转 EUC-JP）、
  `bmh_sjis/reg_sjis`（转 Shift_JIS）
- 新增 `detect_code()`：先用 `g_utf8_validate()` 判断「合法 UTF-8 且含非 ASCII」
  → `KCODE_UTF8`，否则回退 `guess_kanji()`。合法 UTF-8 多字节序列几乎不可能
  同时是合法 EUC-JP/Shift_JIS 流，判定足够干净
- `show_file()` 的 UTF-8 分支直接 `g_strdup(segment)`，不再转码

**验证**：准备 UTF-8 / EUC-JP / Shift_JIS / ASCII 四个文件，搜「辞書」命中 3 个、
搜「词典」命中 1 个、搜 `dictionary` 命中 1 个。

## 2. `iconv: Argument list too long` 弹窗 → 界面卡死

**现象**：输入中文词后弹出错误框，窗口无响应（XFCE 提示 busy）。

**根因**：

- 新增的 `iconv_convert("utf-8", "euc-jp", 中文词)` 遇到无法转换的字符会调用
  `replace_char()`；该函数按 **2 字节**跳过（本来是为 EUC-JP 双字节设计的），
  而 UTF-8 中文是 3 字节 → 指针错位 → `iconv` 返回 `E2BIG` →
  `LOG_CRITICAL "iconv : Argument list too long"`
- `LOG_CRITICAL` 触发 `popup_error()` 模态框；而搜索进度框调用过
  `gtk_grab_add()` 持有全局 grab，新对话框收不到任何输入，主循环被占死

**修复**：

- 关键词转换不再用 `iconv_convert()`，改用 GLib 的 `g_convert()`：失败通过
  `GError` 返回，静默禁用该编码的匹配表，不弹窗
- `log.c` 在搜索期间（`thread_running != 0`）一律不弹模态框，作为兜底

**验证**：ASan 版搜中文词，日志 0 条 CRITICAL，进度框正常关闭。

## 3. 目录树复选框点不动

**现象**：鼠标在目录树上像在选文字，点不中条目，无法把目录加入搜索范围。

**根因**：`dirtree.c` 的 `button_press_event()` 对普通左键单击 `return TRUE`，
吞掉了事件，GTK 默认处理器收不到，`GtkCellRendererToggle` 的 `toggled` 信号
永远不发出。唯一能激活目录的是隐藏的 **Ctrl+单击**。

**修复**：

- 非 Ctrl 的单击返回 `FALSE`，交回 GTK 默认处理（复选框可点、行可选中）；
  Ctrl+单击保留兼容
- `method_changed()` 的 `SEARCH_METHOD_GREP` 分支补上 `note_tree → 页 3`
  （其它搜索方法都会切左侧页，唯独 File Search 没切）
- 目录树加 tooltip 说明操作方式

## 4. 勾选/取消勾选目录时 `free(): invalid pointer`

**现象**：点复选框后直接 `zsh: abort`。

**根因**：`item_toggled()` 的取消分支：

```c
active_dir_list = g_list_remove(active_dir_list, l->data);
g_free(l->data);   /* l 指向的节点已被 g_list_remove 释放 */
```

`g_list_remove()` 会释放命中的链表节点，紧接着读 `l->data` 就是
use-after-free。同样的写法还在 `button_press_event()` 的 Ctrl 分支和
`expand_history()` 里。

**修复**：抽出 `remove_active_dir()`，用 `g_list_delete_link()` 摘链，
先把 `data` 存到局部变量再 `g_free`；三处统一调用。

**验证**：gdb 直接调用 `item_toggled()` 模拟「勾选 → 取消 → 再勾选」，
修复前第二次 SIGABRT，修复后三次全过、`dirlist.xml` 正常写出。

## 5. 搜索有结果后 `Invalid text buffer iterator` + 段错误

**现象**：勾好目录、搜到结果，自动展示时冒出 GTK 警告然后崩溃。

**根因**：搜索跑在 pthread 里，而 `grep_file()` 每处理一个文件都直接调
`push_message()` 往 `GtkTextBuffer` 插文本，`set_cancel_dlg_text()` 也直接改
`GtkLabel`。GTK2 不是线程安全的：主线程正持有 `GtkTextIter` 展示结果时，
worker 一插入文本就让 iter 的 stamp 失效。

以前不暴露，是因为没勾目录时 `grep_file()` 根本不执行
（`Searching following files...` 后面没有文件名）。

**修复**：

- `push_message()` / `set_cancel_dlg_text()` 用
  `g_main_context_is_owner(g_main_context_default())` 判断；非主循环线程就
  `g_idle_add()` 排队，由主线程执行
- `show_file()` 在 `scroll_to_mark()` 之后按行号重新取 iter
  （滚动可能跑主循环回调，进而修改 buffer）

**验证**：10 个 UTF-8 文件、多命中跑完整异步流程，修复前日志有
`Pango-CRITICAL` / `Gdk-CRITICAL`，修复后完全干净。

## 6. 搜「词」必崩：grep 结果被当成词典结果

**现象**：ASan 报 `SEGV`，栈顶 `gtk_tree_store_set → g_strdup → strlen`，
位置 `headword.c:212`。

**根因**（连锁）：

1. `show_result_tree()` 先 `gtk_notebook_set_current_page(note_tree, 0)`
2. 这一步同步触发 `note_switch_page()` 的 `case 0`，把方法下拉改写成
   "Automatic Search"
3. 紧接着才判断 `ebook_search_method() == SEARCH_METHOD_GREP`，此时已经是
   AUTOMATIC
4. 用户配置 `sort_by_dictionary=1`，于是走 `SORT_BY_DICT` 分支
5. 该分支使用 `rp->data.eb.book_info->subbook_title`，而 `rp` 实际是 GREP 结果，
   union 里对应位置是 `grep.filename`（文件路径字符串）→ 字符串被当指针解引用

ASan 的寄存器值直接印证：`strlen` 收到的指针是 `0x7969432f7469672f`，
按小端拆开正是 `/git/Ciy`。

**修复**：

- `show_result_tree()` 在切页**之前**缓存 `is_grep`，用缓存值走分支
- 新增 `bshowing_result` 标志：`note_switch_page()` 在程序主动切页时不再改写
  方法下拉（顺带修了「结果页总显示 Automatic Search」的问题）
- `SORT_BY_DICT` 分支加 `RESULT_TYPE_EB` 类型守卫，防止同类回归

**验证**：同目录、同词、同 `sort_by_dictionary=1`，ASan 版修复前 SEGV，
修复后 0 ERROR、进程存活。

## 通用经验

### GTK2 老代码的三条红线

1. **控件只能主线程碰**。worker 线程要更新 UI，一律 `g_idle_add()`。
   判断当前是否主循环线程：`g_main_context_is_owner(g_main_context_default())`
2. **`GtkTextIter` 跨修改即失效**。任何插入/删除后不要复用旧 iter；
   用行号、字符偏移或 `GtkTextMark` 重新定位
3. **`GList` 的 remove/delete 语义不同**。`g_list_remove()` 释放节点但不释放
   data；要「移除并释放 data」，先存 data 再 `g_list_delete_link()`

### GTK2 具体坑

- `gtk_widget_size_request()` 会忽略未 `show` 的子控件，测出来只有边框宽度
- `GtkArrow` 的绘制尺寸是内部固定的，`size_request` 对它无效
- `gtk_notebook_set_current_page()` 会**同步**触发 `switch-page` 信号，
  回调里的副作用（如改写方法下拉）会立刻生效

### 调试工具选择

- **ASan 优于裸 gdb bt**：`-fsanitize=address` 能给出出错地址、完整调用链和
  寄存器值。构建方式：

  ```sh
  cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
  cmake --build build-asan -j
  ASAN_OPTIONS=detect_leaks=0 ./build-asan/ebview
  ```

- **gdb `-batch` 不要用 `continue` 等异步搜索**，会在 gdb 里挂住。可改用
  `call grep_search(...)` 后立刻 `detach`，让进程自由跑，再读日志
- **没有 debug 符号也能调函数**：只要符号表里有地址，
  `call (void) f((char*)"arg")` 就可用；配合 `-ex detach` 是抓真实异步流程的
  好办法
- **桌面自动化不可靠**：同名窗口可能有多个（主窗口、`hidden_window`、splash），
  按名字取会取错；用 `xdotool getwindowpid` 加窗口尺寸区分，或干脆改用函数级验证

### 编码探测

- `file -i` / `file --mime-encoding` 会把 EUC-JP 误判成 `iso-8859-1`，
  不要用它判断日文编码
- 判断顺序建议：`g_utf8_validate()` 严格校验 → 合法且含非 ASCII 即 UTF-8；
  否则再用启发式（EUC/SJIS 的字节分布）
- 老代码里 `replace_char()` 的「跳过 2 字节」只适用于 EUC-JP/Shift_JIS
  双字节输入，**不要**用于 UTF-8

## 当前局限

- 简体中文 / emoji 只能命中 UTF-8 文件；EUC-JP / Shift_JIS 文件里没有这些字符
  的码位，属于格式限制
- `Manual Select` 需要显式勾选目录；没勾时结果是 `No hit`，不是编码问题
- 递归枚举已排除 `.git`、`build`、`node_modules` 等噪声目录，跳过大于 2 MiB 的
  文件与含 NUL 的二进制文件，递归深度上限 32

---

# Windows / MSYS2 (UCRT64) 移植踩坑记录

> 2026-11 起，把 `ebview`（GTK2 + libeb 的 EPWING 词典阅读器）在 Windows 下用
> MSYS2 UCRT64 真正编译、链接、跑起来，把每一步真实报错逐项修掉。
> 相关源码修复已提交 `e8d9c48`。

## 0. 环境与工具链

- MSYS2 安装：`C:/Users/yx/msys64/`。必须用 **UCRT64** 终端（双击
  `msys64/ucrt64.exe`，或 Win+R 输该路径）。GTK2 在 MSYS2 只存在于
  `ucrt64` / `clang64` 仓库，MINGW64/MSYS 没有。
- 关键包版本：`gtk2` 2.24.33、`pango` 1.58.2（← 与 GTK2 2.24 不兼容，是启动崩溃根因）、
  `glib2` 2.8x。
- 依赖 `libeb` 4.4.3 在仓库外单独构建：`C:/Users/yx/src/eb-4.4.3/`

### 0.1 网络：镜像走直连、不要走代理
本机本地代理（`127.0.0.1:50280`，来自 Accesser）对 GitHub 有效，但**对 MSYS2 镜像
`mirror.msys2.org` 全部超时（curl 返回 000）**，镜像直连反而能通（302）。所有
`pacman` / 运行命令先：

```bash
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
```

### 0.2 工具链“装了等于没装”
`pacman -S base-devel` 报告 exit 0 却只装了 MSYS base 组，没有 `mingw-w64-ucrt-x86_64-*`。
务必显式装：

```bash
pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk2 mingw-w64-ucrt-x86_64-gettext \
  mingw-w64-ucrt-x86_64-zlib
pacman -S --needed make            # UCRT64 基础环不含 make
pacman -S --needed mingw-w64-ucrt-x86_64-gdb
```

## 1. libeb 4.4.3 的 MinGW 构建报错（仓库外，需自行修补）

```bash
cd C:/Users/yx/src/eb-4.4.3
export CPPFLAGS="-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0501"
export LIBS="-lws2_32"
./configure --prefix=/ucrt64 --disable-ebnet --disable-nls
make
cd eb && make install            # 只装核心 libeb.a，跳过 libebutils 工具
```

1. **`configure` `Failed to get size of sin_port`**：`AC_TYPE_IN_PORT_T` 检测在
   `if ENABLE_EBNET=yes` 块内，MinGW 无 `sys/socket.h` 失败。→ `--disable-ebnet`
   （ebview 不用 EBNET 网络取书）。
2. **`build-post.h:45 implicit declaration 'localedir'`**：Windows 分支
   `EB_LOCALEDIR localedir()` 在 4.4.3 未实现。→ `--disable-nls`（仅影响 libeb
   自身报错文案，ebview 无关）。
3. **`eb/booklist.c:126 implicit declaration 'ebnet_bind_booklist'`**：`#ifndef
   ENABLE_EBNET` 提前 `goto failed` 后后续 `ebnet_bind_booklist()` 仍被编译。→ 用
   `#else ... #endif` 把 ebnet 调用整体包裹。
4. **`libebutils/getopt.c` `conflicting types for 'getenv'`**：第 90 行
   `extern char *getenv();`（K&R 旧式声明）与 `<stdlib.h>` 冲突，GCC 15 升级为硬错误。
   → 删除该行。
5. **链接 `multiple definition of 'getopt_long'`**（ebfont.exe 等工具）：MinGW
   `libmingwex` 已提供，libeb 自带重复。→ ebview 不需要这些工具，**只 `cd eb &&
   make install` 装核心 `libeb.a`**，跳过工具链。静态链接，运行时无需 `eb.dll`。

## 2. ebview 编译/链接期报错（已提交 e8d9c48）

1. **链接 `__imp_mciSendCommandA` / `libiconv_*` / `regcomp` 未定义** →
   `CMakeLists.txt` 加 `if(WIN32) target_link_libraries(ebview PRIVATE winmm iconv gnurx) endif()`
   （`winmm`=MCI；`iconv`=GNU libiconv；`gnurx`=POSIX regex）。
2. **`src/misc.c:79 implicit declaration '_getdcwd'`** → 文件头加
   `#ifdef __WIN32__ #include <direct.h> #endif`。
3. **`src/popup.c` X11 拖拽代码报类型不匹配**（`gdk_window_foreign_new` /
   `gdk_window_get_pointer` 是 X11 专属）→ 相关分支用
   `#ifndef __WIN32__ ... #else { POINT pt; GetCursorPos(&pt); } #endif` 跨平台化；
   `create_popup_window` 的 `#ifdef __WIN32__` 段补 `gint pos_x, pos_y;` 并用
   `GetCursorPos` 初始化。`move_popup_window()` 已正确用
   `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)`，无需改。
4. **`src/selection.c:368 'GWL_WNDPROC' undeclared`** → 64 位下改用
   `GetWindowLongPtr` / `GWLP_WNDPROC` / `LONG_PTR`：
   `SetWindowLongPtr(hidden_hwnd, GWLP_WNDPROC, (LONG_PTR)HiddenWndProc)`。
5. **`src/preference.c` 数据目录定位**：原固定 `<exe_dir>\data`；CMake 构建树里数据在
   `<exe>\share\ebview`。→ 探测顺序 `EBVIEW_DATADIR` → `<exe>\data` →
   `<exe>\share\ebview`（构建树）→ `<exe>\..\share\ebview` → `PACKAGEDIR`，
   以 `shortcut.xml` 存在为判据。

## 3. 运行期崩溃：Pango Win32 字体加载路径损坏（已修复，e8d9c48）

- **现象**：启动即 `Segmentation fault (RC=139)`，gdb 栈顶全是
  `libpangowin32-1.0-0.dll`，来自 `src/preference.c:493 calculate_font_size()`。
- **根因（别再误判为“环境/headless”）**：写了一个**不含任何 ebview 代码**的最小复现
  程序，仅调 `pango_font_map_load_font()` / `load_fontset()` /
  `pango_context_get_metrics()`，对 `"en"` 和 `"ja"` **都崩**（RC=139）；而
  `pango_font_map_list_families()` 正常返回 **80 个字体**（含 Microsoft JhengHei UI
  等 CJK）。即字体**枚举**正常、**加载/度量**整条路径在 Pango 1.58.2 + GTK2 2.24.33 的
  Win32 后端里崩（崩溃前 Pango 打印 `couldn't load font "ja", falling back to
  "Sans"` 然后 SIGSEGV）。这是该版本组合已知坏路径，**真实桌面也会崩**。
- **修复**：`calculate_font_size()` 不再向 Pango 请求“加载字体”，改为从 `fontset_normal`
  的字号（`pango_font_description_get_size()` / `PANGO_SCALE`）和
  `gdk_screen_get_resolution()` 直接推算像素字高/宽/ascent/descent。grep 确认整个
  `src/` 再无第二处直接调 Pango 加载字体。

## 4. 启动提示 “Couldn't find gtkrc”（已修复，e8d9c48）

- 现象：`src/misc.c:145 CRITICAL : Couldn't find gtkrc. Check installation.`
- 原因：上游不随包发布 `gtkrc`，非致命但误导。
- 修复：新增最小 `data/gtkrc`（默认 `MS Gothic 9`），并在 `CMakeLists.txt` 的
  `file(COPY ...)` 与 `install(FILES ...)` 两处登记，首次启动自动落到配置目录。

## 5. 文件搜索时 Shift_JIS 文件名转换失败（已修复）

点“文件搜索”（目录树展开）报：

```
E:/git/EBView/src/dirtree.c:426 CRITICAL : Failed to convert filename from Shift_JIS
E:/git/EBView/src/dirtree.c:409 CRITICAL : Failed to open directory W:\(丈
E:/git/EBView/src/dirtree.c:426 CRITICAL : Failed to convert filename from Shift_JIS
```

- **根因（已定位并修复）**：`preference.c:227` 在 Windows 下把 `fs_codeset` 默认设成
  `"Shift_JIS"`。但 **Windows 上 GLib 的文件名（`g_dir_read_name`）与剪贴板文本都是
  UTF-8**（GLib 内部转 UTF-16），于是 `fs_to_unicode` / `unicode_to_fs` 以及
  `row_expanded()` 里直接调的 `iconv_convert(fs_codeset, "utf-8", name)` 把本就是 UTF-8
  的字符串当成 Shift_JIS 去转 → 乱码（`W:\(丈` 即错误转换后的产物），进而导致
  `g_dir_open()` 拿到非法路径而打不开目录。
- **修复**：Windows 下 `fs_codeset = strdup("UTF-8")`。`iconv_convert` 在编码相同时直接
  `g_strdup` 原样返回（见 `jcode.c:138`），于是所有 `fs_codeset` 相关转换变成恒等，
  等价于“跳过转换”，与 GLib 的 UTF-8 约定一致；同时让 `selection.c:337` /
  `shortcutfunc.c:362` 的剪贴板文本转换也变正确（之前同样被当 Shift_JIS 误转）。
  该判断包在 `#ifdef __WIN32__` 内，Linux 路径（`nl_langinfo(CODESET)`，现代系统多为
  UTF-8）不受影响。
- **验证方式**：在 UCRT64 终端 `./ebview.exe` → 文件搜索 → 展开任一驱动器/目录，
  观察不再出现上述两条 CRITICAL，目录能正常展开、子项文件名显示正确（含中文/日文）。

## 6. 复现/验证清单

```bash
cd /e/git/EBView
cmake -S . -B build -G Ninja      # 首次；之后直接 ninja
cd build && ninja
./ebview.exe                       # UCRT64 终端 PATH 已含 /ucrt64/bin，DLL 可直接找到
```

- 已验证：主窗口可正常显示（Pango 崩溃已根治）；`ldd build/ebview.exe` 无缺失 DLL
  （libeb 静态链接）；`gtkrc` 提示已消除。
- 仍需处理：§5 的 Shift_JIS 文件名转换（文件搜索 / 目录树展开）。
