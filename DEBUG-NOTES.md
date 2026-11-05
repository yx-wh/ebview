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

仓库里 `patches/` 保存了两个必须应用的补丁，**按顺序**（详见 §7、§9）：

```
libeb-4.4.3-win32-path.patch      # Windows 绝对路径被当相对路径
libeb-4.4.3-win32-unicode-path.patch  # 非 ASCII 词典目录（宽字符 open/opendir）
```

```bash
cd C:/Users/yx/src/eb-4.4.3
export CPPFLAGS="-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0501"
export LIBS="-lws2_32"
./configure --prefix=/ucrt64 --disable-ebnet --disable-nls
make
cd eb && make install            # 只装核心 libeb.a，跳过 libebutils 工具
```

> 注意：`--disable-ebnet` 不只是为了绕报错。libeb 的 `EB_ENABLE_EBNET` 会给
> `EB_Book` 多加一个 `int ebnet_file`（+8 字节对齐），一旦头文件和库的开关不一致，
> 就是 §8 那种堆损坏。

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

## 6. 文件搜索对非 ASCII 路径的处理（已修复）

补齐 §5 之后，文件搜索链路上仍有两处“窄字符 API + UTF-8 路径”的隐患，一并处理：

- **`grep.c: grep_acceptable_file()`** 用裸 `stat()` / `fopen()` 判断文件大小与
  是否二进制。Windows 下这两个函数按 ANSI 代码页解释路径，**非 ASCII 文件名会失败
  （返回 FALSE）导致文件被静默跳过**。修复：改用 GLib 的 `g_stat()` / `g_fopen()`
  （`<glib/gstdio.h>`；Windows 下内部转 UTF-16），类型用 `GStatBuf`。Linux 上
  `g_stat`/`g_fopen` 是 `stat`/`fopen` 的宏，行为不变。
- **`grep.c: open_file()`** 从搜索结果打开文件时用 `ShellExecute`（ANSI 版），
  UTF-8 路径同样会失败；且 `(int)hi > 32` 把 64 位 `HINSTANCE` 截断成 32 位。修复：
  用 `g_utf8_to_utf16()` 转宽字符后调 `ShellExecuteW()`，判据改 `(INT_PTR)hi > 32`。
- 注：`filter.c: get_cache_file()` 的 `stat()`/`unlink()`/`rename()` 只在配置了
  “过滤器”时才走到（普通文本文件搜索走 `g_file_get_contents()`，GLib 正确处理
  UTF-8 路径），暂未改动；若后续用到过滤器再统一替换为 `g_stat`/`g_unlink`/`g_rename`。

## 7. Dictionary Group → Search Disk 完全没反应（已修复）

**现象**：Preferences → Dictionary Group，Path 填 `E:\daijirin`、Depth 0，点
`Search Disk`，界面毫无反应：没有报错、也没有新增条目。

### (a) 根因：libeb 把 Windows 绝对路径当成相对路径

`eb_bind()` 的第一步是 `eb_canonicalize_path_name()`。libeb 有两套实现，用
`DOS_FILE_PATH` 区分 —— 但 **`configure.ac` 从不定义 `DOS_FILE_PATH`**，所以 MinGW
构建走的永远是 UNIX 版：

```c
if (*path_name != '/') {            /* "E:\daijirin" 不以 '/' 开头 */
    getcwd(cwd, EB_MAX_PATH_LENGTH + 1);
    sprintf(path_name, "%s/%s", cwd, path_name);   /* -> C:/cwd/E:\daijirin */
}
```

路径被拼成 `C:/当前目录/E:\daijirin`，`opendir()` 打不开 → 找不到 `catalog`/`catalogs`
→ 返回 `EB_ERR_FAIL_OPEN_CAT`（10）。**所有 Windows 绝对路径（含 Samba 映射盘 W:、
光盘根目录）都因此 bind 失败**，与 GUI 无关。

**修复**（仓库外，libeb 源码 `eb/filename.c`；补丁已存仓库
`patches/libeb-4.4.3-win32-path.patch`）：在 UNIX 版开头加 `#ifdef _WIN32` 分支，把
`X:\`、`X:/`、`X:dir`、`\\server\share` 一律视为绝对路径，只做分隔符归一化
（`/`→`\`）并去掉尾部 `\`（保留盘根 `X:\`）。

### (b) 失败时只有 LOG_INFO，默认级别看不到

`enumerate_subbook()` 里 bind 失败只写了 `LOG(LOG_INFO, "Failed to bind book...")`，
而默认 `ebview_log_level = LOG_MESSAGE`(=32)，`LOG_INFO`(=64) 被过滤 —— 于是
“扫描失败”和“什么都没找到”在界面上完全一样：**零反馈**。

**修复**（`src/pref_dictgroup.c`）：

- bind / `eb_subbook_list()` 失败 → `LOG_WARNING`，带上路径与 `ebook_error_message()`。
- “该书没有可用检索方式（word/endword/exactword/keyword/multi），已跳过”→ `LOG_WARNING`。
- `search_disk()`：先判目录是否存在，否则弹 `Directory does not exist...`；扫描后统计
  `books_found`，为 0 弹 `No dictionary was found. Check the path and the search depth.`。
- `search_dictionary_recursive()`：`sprintf` 到 512 字节栈缓冲 → 改 `g_build_filename()`
  （长路径不再溢出）；`strcasecmp` → `g_ascii_strcasecmp`；同一目录同时有 `CATALOG`
  与 `CATALOGS` 时只枚举一次，避免重复条目。

### (c) 脱离 GUI 的最小验证（`E:\daijirin`，单文件 ebzip 词典）

```c
eb_initialize_library();          /* 必须！见下面注意事项 */
eb_initialize_book(&book);
eb_bind(&book, "E:\\daijirin");   /* 0 = EB_SUCCESS */
eb_subbook_list(&book, list, &n);
eb_set_subbook(&book, list[0]);
eb_search_word(&book, "\xa4\xa2");   /* EUC-JP "あ" */
eb_hit_list(&book, 16, hits, &cnt);
eb_seek_text(&book, &hits[0].text);
eb_read_text(&book, NULL, NULL, NULL, sizeof(buf)-1, buf, &len);
```

修复后输出：

```
[bind] "E:\daijirin" -> 0 (EB_SUCCESS)
        subbook_list rc=0 count=1
        #0 dir="daijirin" title="..."(EUC-JP)
set_subbook rc=0  menu=1 copyright=1 word=1 endword=1 exactword=1 keyword=1 multi=2
eb_search_word rc=0  hit_list hits=16  read_text rc=0 len=119
```

修复前同一程序：`-> 10 (EB_ERR_FAIL_OPEN_CAT)`。

**注意事项**

- 必须先 `eb_initialize_library()`，否则 `eb_set_subbook()` 返回 18
  `EB_ERR_FAIL_READ_TEXT`（ebview 在 `src/eb.c:498` 已调用，所以 GUI 里不受影响；
  自己写测试程序时容易漏）。
- 词典目录名仍要求 **ASCII**：libeb 用窄字符 `open()`/`opendir()`，中文/日文目录名
  仍会失败。要支持需另打“UTF-8 → UTF-16 + `_wopen`/`_wopendir`”补丁。
- 只含 `HONMON.ebz`（ebzip 压缩、单文件）的词典已验证可 bind、可检索、可读正文。

### (d) 重建 libeb 时的 libtool 报错

重新编译 libeb 时 `libtool` 会报（gcc 单独调用却正常）：

```
Cannot create temporary file in C:\Windows\: Permission denied
```

绕过办法（等价于 libtool 的静态编译）：

```bash
cd /c/Users/yx/src/eb-4.4.3/eb
gcc -DHAVE_CONFIG_H -I. -I.. -DEB_BUILD_LIBRARY -DWIN32_LEAN_AND_MEAN \
    -D_WIN32_WINNT=0x0501 -g -O2 -c filename.c -DDLL_EXPORT -DPIC -o .libs/filename.o
ar r .libs/libeb.a .libs/filename.o && ranlib .libs/libeb.a
cp .libs/libeb.a /c/Users/yx/msys64/ucrt64/lib/libeb.a
```

## 8. `Search Disk` 之后直接闪崩：`_FILE_OFFSET_BITS` 导致结构体 ABI 不一致（已修复）

第 7 节修完路径问题后，`Search Disk` 能扫到词典了，但接着整个进程无声消失。

### (a) 现象不是「段错误」

在 bash 里只能看到退出码 `127`，很容易误判成 `_exit(127)`。用一个
`CreateProcess` + `GetExitCodeProcess` 的小外壳去观察，真实退出码是：

```
child exit code = 0xC0000374 (3221226356)   -> STATUS_HEAP_CORRUPTION
```

不是访问违例（`0xC0000005`），是**堆损坏**。这类错误由 CRT / ntdll 内部
`abort()`，`SetUnhandledExceptionFilter`、`AddVectoredExceptionHandler` 都拦不到，
而且 `my_log_handler` 原先是个空函数，把 GLib 的 `g_critical` 也一起吞了 —— 所以
表现出来就是「什么都没打印就死了」。

### (b) 让 Windows 自己说出坏在哪

关键技巧：**在 gdb 下运行**。gdb 用 `DEBUG_PROCESS` 创建进程，ntdll 会启用
debug heap，任何「写超过 request size」都会被精确报告：

```bash
gdb -batch -q -ex "set confirm off" -ex run -ex "bt 40" --args ./ebview.exe
```

于是拿到第一条确定性的证据：

```
warning: HEAP[ebview.exe]:
warning: Heap block at 0000000009023950 modified at 0000000009024738 past requested size of dd8
```

`0xdd8 = 3544` —— 一个 3544 字节的块被越界写。再配合自己埋的
`HeapValidate(GetProcessHeap(), 0, NULL)` 检查点（**先打印 tag 再校验**，否则
ntdll 的 SIGTRAP 会在你的 `fprintf` 之前打断），把范围压缩到两条语句之间：

```
SZ binfo=224 ebook=3544 book=1200
HV> lb4 after malloc book
HV> lb5 after eb_initialize_book      <- 已打印，随后 SIGTRAP
```

即 `malloc(sizeof(EB_Book))` 之后紧跟的 `eb_initialize_book()` 把这块写穿了，
而 `sizeof(EB_Book)` 恰好就是 3544。

### (c) 根因：`off_t` 一边 4 字节一边 8 字节

用 gdb 读**库里编译时**的结构体布局（`ebview.exe` 自身没带 `-g`，所以 gdb 只会
用 libeb.a 里的调试信息）：

```bash
gdb -batch -q -ex "ptype /o EB_Book" ./某个链接了 libeb 的程序
```

| | 本程序编译出来的 | libeb.a 里的 |
|---|---|---|
| `sizeof(EB_Text_Context)` | 336 | **344** |
| `offsetof(EB_Book, text_context)` | 56 | 56 |
| `offsetof(EB_Book, binary_context)` | 392 | **400** |
| `offsetof(EB_Book, search_contexts)` | 584 | **592** |
| `sizeof(EB_Book)` | 3544 | **3552** |

差的 8 字节出自 `EB_Text_Context::location` 的类型 —— `off_t`：

```
off_t = 4 字节 -> sizeof(EB_Text_Context) = 336, sizeof(EB_Book) = 3544
off_t = 8 字节 -> sizeof(EB_Text_Context) = 344, sizeof(EB_Book) = 3552
```

libeb 用 autoconf 构建，`AC_SYS_LARGEFILE` 在它的 `config.h` 里写了
`#define _FILE_OFFSET_BITS 64`（MinGW 下这会把 `off_t` 变成 8 字节）；而
`eb/defs.h` 是**被编进客户端**的，ebview 的 CMake 构建没有这个宏，于是拿到了
4 字节的 `off_t`。上游的 autotools 构建对 libeb 和 ebview 都套了
`AC_SYS_LARGEFILE`，移植到 CMake 时把这个宏丢掉了。

`eb_initialize_book()` 会逐个字段初始化 `search_contexts[]`，最后写到
`keyword_heading.offset`，正好落在它认为的 3548 处 —— 比我们分配的 3544 多 4 字节，
踩进堆块尾巴，之后任意一次 `free()`/分配就报 `STATUS_HEAP_CORRUPTION`。

### (d) 修复

`CMakeLists.txt`（`target_include_directories` 之后）补上，让两边 ABI 一致：

```cmake
target_compile_definitions(ebview PRIVATE _FILE_OFFSET_BITS=64)
```

同时在 `eb.c` 加了一个一次性自检 `check_eb_abi_structure()`（在
`ebook_start()` 里 `eb_initialize_library()` 之后调用）：分配
`sizeof(EB_Book) + 256` 字节、填 `0xAA`、调一次 `eb_initialize_book()`，若
`sizeof(EB_Book)` 之后有字节被改写，就用 `LOG_CRITICAL` 明确指出
「头和库的 ABI 设置不一致，最可能是 `_FILE_OFFSET_BITS`」。这类错配以后再出现，
不必再花一轮 gdb 才能定位。

### (e) 经验

- **静态库 + 公开头文件里的结构体 = 必须共享全部 ABI 宏**
  （`_FILE_OFFSET_BITS`、`_REENTRANT`、`-mms-bitfields`、`_WIN32_WINNT`…）。
  库若不带 `.pc` 文件，CMake 的 `find_path`/`find_library` 完全看不到这些宏。
- 崩溃点离开真实原因很远时，**不要继续加 printf**，先让运行环境给出精确位置：
  Windows 上是 gdb（debug heap）+ `HeapValidate` 检查点，Linux 上是
  `MALLOC_CHECK_=3` / ASan。
- `127` 这个退出码在 MSYS2 bash 下**同时**代表 `abort()` 和 `_exit(127)`，
  不能用来区分。

## 9. 词典目录名支持 Unicode（非 ASCII，已修复）

之前文档里写着「词典目录名必须 ASCII」，因为 libeb 全程用 `char *` 路径调用窄字符
文件 API。Windows 上 `open()` / `opendir()` 把参数按**系统 ANSI 代码页**解释，
UTF-8 的 `测试词典` 被当成 GBK/CP1252 的一串字节，拼出来的路径根本不存在，
`eb_bind()` 直接返回 `EB_ERR_FAIL_OPEN_CAT`。

### (a) 需要改的地方只有 4 处

```
eb/filename.c:263   dir = opendir(path);            // eb_fix_directory_name()
eb/filename.c:423   dir = opendir(path_name);       // eb_find_file_name()
eb/zio.c:1973/1976  open(file_name, O_RDONLY|O_BINARY);  // zio_open_raw()
```

目录**里面的**条目名（`CATALOG`、`CATALOGS`、`HONMON`、`*.ebz`、`GA16HALF`…）
在所有 EPWING/EB 词典里都是 ASCII，所以只有「路径」需要宽字符，条目名可以
转回 UTF-8 继续按字节处理，上层逻辑一行都不用动。

### (b) 新增 `eb/win32path.h`（header-only）

之所以做成头文件而不是新的 `.c`：libeb 的 autotools 构建要往
`eb/Makefile.am` 里加对象，而 header-only 完全不用碰构建脚本
（本机 libtool 还坏着，见 §7(d)）。

它提供 4 个 `static inline` 的包装，非 Windows 分支就是原来的 POSIX 调用：

| 接口 | Windows | POSIX |
|---|---|---|
| `eb_open_readonly(path)` | `_wopen()` | `open(path, O_RDONLY\|O_BINARY)` |
| `eb_dir_open(path)` | `_wopendir()` | `opendir()` |
| `eb_dir_read_name(dir)` | `_wreaddir()` + `WideCharToMultiByte` | `readdir()->d_name` |
| `eb_dir_close(dir)` | `_wclosedir()` | `closedir()` |

路径转换用 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`；
**转换失败时回退到 `CP_ACP`**，这样仍然传 ANSI 路径的老调用方不会被弄坏。
`filename.c` 里 `DIR *`/`struct dirent *entry` 相应换成 `EB_DIR *`/`const char *name`，
两处 `entry->d_name` 全部改成 `name`。

编码约定：EBView 在 Windows 上 `fs_codeset = "UTF-8"`（§5），
`gtk_entry_get_text()`、`g_build_filename()`、`g_dir_read_name()` 给的都是 UTF-8，
所以 libeb 收到的就是 UTF-8 —— 与 `CP_UTF8` 分支正好对上。

### (c) 验证

用**硬链接**造一个非 ASCII 路径的副本（不占额外空间、不复制 312MB）：

```bash
cp -rl /e/daijirin "/e/测试词典"
```

然后直接调 libeb（不经过 GUI）：

```
--- E:\测试词典                        --- E:\daijirin
  eb_bind      = 0                      eb_bind      = 0
  subbooks     = 1 (err 0)              subbooks     = 1 (err 0)
  set_subbook  = 0                      set_subbook  = 0
  have_word    = 1                      have_word    = 1
  search_word  = 0                      search_word  = 0
  hit_list     = 0 (10 hits)            hit_list     = 0 (10 hits)
  read_text    = 119 bytes              read_text    = 119 bytes
```

两条路径结果完全一致，说明宽字符 open/opendir 生效，且 ebzip（`HONMON.ebz`）
解压路径也走通了。

### (d) 补丁与重建

两个补丁按顺序应用（**顺序不能反**，第二个是相对第一个改完的状态生成的）：

```bash
cd /c/Users/yx/src
tar -xjf eb-4.4.3.orig.tar.bz2
cd eb-4.4.3
patch -p1 < /e/git/EBView/patches/libeb-4.4.3-win32-path.patch
patch -p1 < /e/git/EBView/patches/libeb-4.4.3-win32-unicode-path.patch
```

`make` 在本机会挂在 libtool 上（`Cannot create temporary file in C:\Windows\`，
且**只要 PATH 里有 `/usr/bin` 就必现**），所以照旧单文件编译后更新归档：

```bash
cd eb-4.4.3/eb
export PATH="/c/Users/yx/msys64/ucrt64/bin:$PATH" TMPDIR=/tmp
FLAGS="-DHAVE_CONFIG_H -I. -I.. -DEB_BUILD_LIBRARY -DWIN32_LEAN_AND_MEAN \
       -D_WIN32_WINNT=0x0501 -g -O2 -DDLL_EXPORT -DPIC"
for f in filename zio; do gcc $FLAGS -c $f.c -o .libs/$f.o; done
ar r .libs/libeb.a .libs/filename.o .libs/zio.o && ranlib .libs/libeb.a
cp .libs/libeb.a /c/Users/yx/msys64/ucrt64/lib/libeb.a
```

注意两个坑：

- `TMPDIR` 要用 **POSIX 风格的 `/tmp`**（不是 `C:/Users/.../tmp`），否则
  gcc 找不到临时目录，退到 `C:\Windows` 就被拒绝。
- 链接 libeb 的测试程序也必须带 `-D_FILE_OFFSET_BITS=64`（§8），否则
  `sizeof(EB_Book)` 差 8 字节，一调用就堆损坏。

## 10. Samba 共享上 Search Disk 弹窗后界面卡死（已修复，282e6b2）

### 现象

把挂载的 Samba 共享（`W:` → `\\192.168.2.119\yx`）整个目录加进 Dictionary
Group，设好 Depth 后点 **Search Disk**：扫描跑完弹出一条警告

```
W:\...\辞書の...: the book has no search method (word/endword/exactword/
keyword/multi), so it cannot be used for lookups and was skipped.
```

然后**整个界面卡死**——警告框的 OK 按钮点不动，preference 窗口也没反应，
只能强杀进程。

### 根因：GTK 的 grab 栈

`show_preference()` 末尾有这样一句：

```c
gtk_widget_show_all(pref_dlg);
gtk_grab_add(GTK_WIDGET(pref_dlg));   /* src/preference.c:715 */
```

即 preference 窗口**手动**抓取了输入。而 `idle_warning()` 弹出的是**非模态**
对话框（`gtk_widget_show()` + 信号回调，没有 `gtk_dialog_run()`）。非模态窗口
不会自己抓取输入，GTK 的规则是**事件只发给 grab 栈顶的那个 widget**——栈顶
始终是 preference 窗口，所以警告框完全收不到鼠标/键盘事件：显示出来了，却
一句输入都拿不到，看起来就是整个程序死掉。

用一个最小 GTK 程序复刻（A = preference 窗口，B = 警告框）：

```
1. prefs shown + grab_add(A)      current_grab = A(pref_dlg)
2. dialog shown, before fix       current_grab = A(pref_dlg)   <-- 弹窗失效
3. after gtk_grab_add(B)          current_grab = B(dialog)
4. after destroy(B)               current_grab = A(pref_dlg)   <-- 正常恢复
```

第 2 行就是卡死的直接证据：B 已经显示，grab 却还在 A 上。

### 修复

`idle_warning()` / `idle_error()` 在 `gtk_widget_show()` 之后，若发现别人持有
grab，就把 grab 转到对话框自己身上：

```c
if(gtk_grab_get_current() != NULL && gtk_grab_get_current() != dialog)
    gtk_grab_add(dialog);
```

`gtk_widget_destroy()` → `gtk_widget_unrealize()` 会自动 `gtk_grab_remove()`，
所以关闭对话框后前一个持有者（preference 窗口）自动恢复，不需要手动记账
（上面测试的第 4 行已验证）。

顺带把 `parent = gtk_grab_get_current();` 加了 `!GTK_IS_WINDOW(parent)` 判断，
grab 栈顶不一定是窗口（某些 widget 也会抓），否则 `gtk_message_dialog_new()`
会拿一个非窗口做 parent。

### 同一路径上顺带修的三个问题

Samba 上一个共享里可能有几十上百个词典目录，下面每一条都会被放大：

1. **每个不可用词典单独弹一次窗**。`enumerate_subbook()` 里原本用
   `LOG(LOG_WARNING, ...)` 报告 bind 失败/无检索方式，而 `log.c` 会把
   `LOG_WARNING`/`LOG_MESSAGE`/`LOG_CRITICAL` 统统转成弹窗（`popup_warning`
   / `popup_error`）。于是扫一个共享要逐个点掉几十个对话框。改成
   `books_skipped++` + `LOG(LOG_DEBUG, ...)`，扫描结束汇总一条：

   ```
   Added 12 dictionaries. 37 could not be used (no search method, or the
   book could not be read) and were skipped.
   ```

   注意 `LOG_DEBUG` 默认被过滤（`ebview_log_level = LOG_MESSAGE`），所以这些
   细节只在 `LOG_LEVEL=DEBUG` 时才进日志，不会打扰用户。

2. **`enumerate_subbook()` 有 4 条 `return(1)` 漏了 `eb_finalize_book()`**。
   它们分别在 `eb_subbook_directory2` / `eb_subbook_title2` /
   `eb_set_subbook` / `load_book` 失败之后——此时 book 已经 bind 成功、
   文件已打开，直接 return 就漏一个句柄。词典一多可能耗尽句柄。统一改成
   `goto failed;`，由标签处统一 `eb_finalize_book()`。

3. **每遍历一个目录条目要 stat 两次**。原来对每个 entry 调用
   `g_file_test(IS_REGULAR)` 和 `g_file_test(IS_DIR)`，各一次 `stat()`。
   本地磁盘无所谓，网络共享上每次 `stat()` 都是一次往返回合。改成一次
   `g_stat()` + `S_ISREG`/`S_ISDIR`：

   ```c
   GStatBuf st;
   if(g_stat(fullpath, &st) != 0){ g_free(fullpath); continue; }
   if(S_ISREG(st.st_mode)) { ... }
   else if(S_ISDIR(st.st_mode)) { ... }
   ```

   必须用 `g_stat()` 而不是 `stat()`：glib 版本接受 UTF-8 路径，在 Windows
   上会转 UTF-16（与 §5/§6 的 `g_stat`/`g_fopen` 是同一个道理）。

4. 扫描期间 `set_scan_cursor(TRUE)`（`GDK_WATCH` + `gdk_flush()`），结束后
   恢复。扫描是同步跑在主线程里的，主循环完全阻塞，wait 光标至少让"忙"和
   "死"能区分开。

### 经验

- **GTK2 里 `gtk_grab_add()` 与"模态"是两套机制**。`gtk_window_set_modal()`
  会让窗口在 map 时自己抓取，而手动 `gtk_grab_add()` 只是压栈。任何在
  手动 grab 期间弹出的**非模态**窗口都收不到输入——不是崩溃，也不报错，
  纯粹"点了没反应"，非常容易被误判成死锁或线程问题。
- 想确认是不是 grab 问题，只要在原进程里看 `gtk_grab_get_current()` 返回谁。
  栈顶不是当前活动窗口，就一定是它。
- 把"每个文件弹一次窗"改成"结束汇总一次"是这类扫描器的通用原则：用户要的
  是结果数量，不是几十条被逐个打断的失败原因。

## 11. 配置里有、硬盘上没了的词典（已修复）

`dictgroup.xml` 里还留着某本词典，但它的目录已经不在了（用户删了、盘没插、
Samba 共享没挂上）。原来的处理方式是**静默丢弃**，而且会在下一次保存时把
配置里的条目一起抹掉：

1. `load_dictgroup()` 逐条 `load_book()`，`eb_bind()` 失败 → `LOG(LOG_CRITICAL)` →
   `binfo = NULL`（`src/eb.c` 的 `FAILED:`）。
2. `src/pref_io.c` 只有 `if(binfo != NULL)` 才 `gtk_tree_store_append()`，
   所以这一条**根本不会进列表**——不是灰显，是凭空消失。
3. 启动阶段 `bstarting_up == TRUE`，`log.c` 的弹窗分支被 `!bstarting_up` 短路，
   只有一个控制台输出（exe 是 CUI 子系统，双击时那个控制台窗口里能看到）。
4. 关键时刻：**`save_dictgroup()` 是唯一写这个文件的函数，而它只遍历
   `dict_store`**。所以任何一次保存都会把这条永久删掉，触发点有三处：
   `dictbar.c` 的 `dict_toggled()`（勾选/取消任一词典）、`group_changed()`
   （切换词典组）、`pref_dictgroup.c` 的 `pref_end_dictgroup()`（关掉设置）。

第 4 条才是真正危险的：挂载点暂时不可用和"真的删了"在程序眼里完全一样，
用户随手点一下别的词典的勾，几十条配置就一起没了。

### 现在的行为

- 加载失败的条目仍然插进 `dict_store`，只是 `DICT_MEMBER_COLUMN == NULL`、
  `DICT_AVAILABLE_COLUMN == FALSE`；名字列把 `"sensitive"` 绑到这个新列上，
  于是该行**灰显**（不可编辑，但仍可选中、可删除）。
- `save_dictgroup()` 对 `binfo == NULL` 的行改用 `DICT_PATH_COLUMN` /
  `DICT_SUBBOOK_NO_COLUMN` 等列写回，配置**永不丢失**。
- 所有搜索路径跳过 NULL：`eb.c` 本来就有 `if(binfo == NULL) continue;`，
  这次又给 `menu.c`（菜单/版权）、`multi.c`（多词检索）补上了同样的保护；
  `dictbar.c` 原本就有。
- 启动完成后由 `g_idle_add()` 弹出**一条**汇总提示（`ebview.c` 的
  `show_unavailable_dicts()`），列出前 5 本的名字。不能在 `gtk_main()` 之前弹，
  否则会卡住启动——这也是 `log.c` 用 `bstarting_up` 抑制弹窗的原因。

### 两个容易踩的实现细节

- **不要用 `gtk_tree_view_column_set_cell_data_func()` 做灰显**。GTK2 里一旦挂了
  cell data func，列上通过 `gtk_tree_view_insert_column_with_attributes()` 设的
  `"text"` / `"editable"` 属性就**不再生效**，名字会全部空白，必须在 func 里
  自己重设。改用 `gtk_tree_view_column_add_attribute(col, renderer,
  "sensitive", DICT_AVAILABLE_COLUMN)` 最省事，属性绑定互不干扰。
- `gtk_tree_view_insert_column_with_attributes()` 在 GTK2 返回的是**列号**
  （`gint`），不是 `GtkTreeViewColumn *`。要拿列对象得再
  `gtk_tree_view_get_column(view, index)`。

### 顺带修掉的两个相关缺陷

- `load_book()` 的 `FAILED:` 原来只写 `binfo->available = FALSE; return NULL;`，
  既不 `eb_finalize_book()` 也不 free：每本读不出来的词典泄漏一个 `EB_Book`
  （3.5 KB）+ 路径字符串，并且留下未关闭的文件句柄。整块 Samba 共享掉线时
  一次泄漏几十份。
- `sublist[binfo->subbook_no]` 没有和 `subcount` 比较。配置里的 `subbook=7`
  配上只剩 1 本子书的新版词典，会把栈上未初始化的元素（数组 50 个，不越界
  但值是垃圾）送进 `eb_subbook_directory2()`。现在越界直接报
  `Subbook number %d is out of range`。

### 怎么验证"保存后条目还在"

这个修复的关键在于 `save_dictgroup()` 的行为，而它平时只在关闭设置对话框时
触发。不想去点界面的话，用 gdb 直接调：

```bash
gdb -batch -q \
 -ex "set confirm off" \
 -ex "break gtk_main" \
 -ex "run" \
 -ex "call (int)save_dictgroup()" \
 --args ./ebview.exe
```

断在 `gtk_main` 上时 `dict_store` 已经填好、GTK 主循环还没跑，此时 `call`
是安全的。然后比对 `%APPDATA%\EBView\dictgroup.xml` 的条目数——
修复后失效条目连同 `path`/`subbook`/`appendix_path`/`active`/`fg`/`bg`
会原样写回。

## 12. Win10 上拖动标题栏 / 拉边框缩放，窗口卡死（已修复）

### 现象

Windows 10 上，鼠标按住标题栏拖动，或者把鼠标移到窗口边缘拉大小，
**窗口立刻停住，界面永久无响应**，只能强杀。同一个二进制在 Debian XFCE 下
完全正常。

### 这个 bug 用程序化方式测不出来

先试过 `MoveWindow()` / `SetWindowPos()` 连续改尺寸和位置，40 次全部正常响应。
原因：拖动标题栏时 Windows 会让窗口过程进入 `WM_ENTERSIZEMOVE` 的**内部模态
循环**（`DefWindowProc` 里自己 `GetMessage`/`DispatchMessage`），而
`MoveWindow()` 只发 `WM_WINDOWPOSCHANGED`，根本不进这个循环——所以拿它做回归
测试永远是绿的。

必须用**真实鼠标事件**才能复现：

```python
u.SetCursorPos(x, y)                       # 标题栏中部
u.mouse_event(MOUSEEVENTF_LEFTDOWN, ...)
for i in range(1, 21):                     # 分步移动
    u.SetCursorPos(x + dx * i // 20, y + dy * i // 20)
u.mouse_event(MOUSEEVENTF_LEFTUP, ...)
```

**两个必须注意的坑**，否则会得到假的"通过"结果：

1. `SetForegroundWindow()` 对后台启动的进程通常被系统拒绝，窗口不在前台时
   按下鼠标等于什么都没做。要用 `AttachThreadInput()` 借用前台线程的输入
   状态再调。
2. 判据不能只看"还响应不响应"，必须**确认窗口真的移动了 / 尺寸真的变了**。
   我最早把抓取点写成 `r.top + 8`，窗口恰好在 `0,0` 时那是上边框而不是标题栏，
   结果既没进入拖动、也没卡死，白白多测了几轮。

### 定位过程

最小 GTK2 程序（window + textview + treeview）拖标题栏**不卡**，说明是 EBView
自己的初始化引入的。于是在 `main()` 里加一个环境变量分支，只建一个空窗口：

| 步骤 | 结果 |
| --- | --- |
| `gtk_window_new` + `gtk_main` | 卡 |
| `+ g_thread_init(NULL)` | 正常 |
| `+ gdk_threads_init()` | **卡** |
| `gdk_threads_enter(); gtk_main(); gdk_threads_leave();` | 卡（因为它依赖 `gdk_threads_init`） |

即：**只要调用过 `gdk_threads_init()`，空窗口也会卡死**。

卡死时的调用栈（gdb 抓的，主线程，从外到内）：

```
#7  USER32!DispatchMessageW
#6  libgdk-win32-2.0-0.dll
#5  libglib-2.0-0.dll          <- 主循环派发
#1  ntdll!RtlAcquireSRWLockExclusive
#0  ntdll!ZwWaitForAlertByThreadId     <- 永等
```

而且栈上 `DispatchMessageW` **嵌套了好几层**：GDK 在窗口过程里又跑了一遍
GLib 主循环。其余线程全部在 idle 等待，没有谁"持有锁不放"——所以是**自死锁**，
不是线程间互相等。

### 根因

`gdk_threads_init()` 会让 GDK 的每个入口点都去拿 `gdk_threads_mutex`。
Windows 拖动/缩放时那个模态循环会把消息重新 `DispatchMessage` 进 GDK 的窗口
过程，此时 GLib 主循环已经持有 `GMainContext` 的锁，重入再拿就永远等下去。

X11 下的窗口移动是窗口管理器的事，应用进程不会进入任何模态循环，所以 Debian
上永远碰不到这条路径——这就是"只有 Windows 会卡"的原因。

### 修复

`src/ebview.c`：Windows 上不调用 `gdk_threads_init()`，也不再用
`gdk_threads_enter()/leave()` 包 `gtk_main()`；`src/splash.c` 的
`gtk_main_iteration()` 循环同理（那段也是在 `gtk_main()` 之外主动泵事件）。
非 Windows 平台保持原样。

```c
g_thread_init(NULL);
#ifndef __WIN32__
	gdk_threads_init();
#endif
	...
#ifndef __WIN32__
	gdk_threads_enter();
#endif
	gtk_main();
#ifndef __WIN32__
	gdk_threads_leave();
#endif
```

### 为什么去掉锁是安全的

`gdk_threads` 只保护 **GDK** 入口，本来就不保护 GTK widget 树，所以它对
EBView 的实际作用是零：

- 搜索线程（`thread_search.c`）只写 `add_result()`（自带 pthread mutex）、
  `set_progress()`、`thread_end()`，UI 更新一律 `g_idle_add()` 回主线程；
- 进度文字走 `set_cancel_dlg_text()`，里面先判 `g_main_context_is_owner()`；
- `push_message()` 在 Windows 上直接 return。

唯一在 worker 线程里碰 GTK 的是 `ebook_search_thread()` 里读 `dict_store`
（`gtk_tree_model_get` 之类）——这些不是 GDK 调用，以前也不受
`gdk_threads_mutex` 保护，去掉锁不改变它的性质。

### 怎么验证

```bash
# 修复后 3/3 通过（拖动），3/3 通过（缩放）
python build/drag_any.py build/ebview.exe
python build/drag_any.py build/ebview.exe --resize
```

输出里 `window moved: True 0,0 -> 160,110` / `window resized: True 686x479 ->
826x569` 才算真的测到了；只有 `hung at step None` 没有位移，说明抓取点没中。

搜索路径另外验证过：临时加一个 `EBVIEW_AUTOSEARCH=<word>` 的定时器，
从主循环里调用 `start_search()`，日志里能看到完整的
`IN : start_search` → `IN : ebook_search_thread(a)` → `OUT : ebook_search_thread()`，
进程全程存活。搜索**进行中**拖窗口也做过（`EBVIEW_AUTOSEARCH=a` + 拖动），正常。

### 经验

- **GTK2 在 Win32 上的 `gdk_threads_*` 家族本身就是雷**。只在确实有多个线程
  直接调用 GDK 时才需要它；如果所有跨线程更新都走 `g_idle_add`，不要开。
- 卡死 vs 变慢要分清：这里的判据是"等 30 秒仍不响应且栈不动"，而不是超时一次。
- `gdb -p` attach 在这台机器上直接 `error 5`（拒绝访问）。可用的替代是让 gdb
  自己 `run` 启动目标，再用外部 `DebugBreakProcess()` 注入断点，gdb 就能拿到
  完整栈。gdb 18.1（MSYS2）**不支持** `target minidump`，别指望离线分析 dump。

## 13. Windows 下界面不跟着系统语言走（已修复）

### 现象

同一份源码在 Debian 上，`LANG=zh_CN.UTF-8` 启动就是简体中文、`zh_TW` 是繁体、
`ja_JP` 是日文；换到 Windows 上不管系统语言是什么，界面**永远是英文**。
`po/` 里的 `zh_CN.po` / `zh_TW.po` / `ja.po` 都在，`build/locale/*/LC_MESSAGES/
ebview.mo` 也编译出来了，就是不生效。

### 先排除"gettext 认不出中文 Windows"

容易先怀疑 Windows 没有 `LANG`。实测**不是**：mingw-w64 的 libintl 在找不到
`LC_ALL`/`LC_MESSAGES`/`LANG` 时会自己去问 Windows（`GetUserDefaultUILanguage`
那一套，glib 的 `g_win32_getlocale()` 给出同样的结果）。拿一个 10 行的探针程序
验证：

```
=== 清空 LANG/LC_ALL（等价于资源管理器双击启动）
g_win32_getlocale()    = zh_CN
translated("File")     = [文件]          <- 只要 mo 能被找到，就是中文
```

（探针源码留在 `build/probe_intl.c`，只链接 glib 和 intl。）

所以问题只在 **mo 的路径**。

### 根因

`main()` 里 Windows 分支写的是：

```c
bindtextdomain(PACKAGE, package_dir);     /* package_dir = <exe>\share\ebview */
```

而 CMake 把目录树编译到 `<build>/locale/<lang>/LC_MESSAGES/ebview.mo`，
`package_dir` 指向 `<build>\share\ebview`，**从未包含 locale 目录**。
gettext 找不到 catalog 就静默回退到 msgid（英文），连一条警告都没有。
非 Windows 分支反而做了探测（`<exe_dir>/locale` 存在就用它），所以 Debian 正常。

### 修复

- `src/preference.c` 新增 `locate_locale_dir()`，和已有的 `locate_package_dir()`
  一个套路：依次探测 `$EBVIEW_LOCALEDIR`、`<exe-dir>/locale`（构建树）、
  `<exe-dir>/share/locale`、`<exe-dir>/../share/locale`（安装前缀），最后
  `LOCALEDIR`。**候选目录必须真的含 `<lang>/LC_MESSAGES/ebview.mo` 才算数**
  （否则一个空目录会把 `LOCALEDIR` 顶掉）。两个平台共用，结果存进新全局
  `locale_dir`。
- `src/ebview.c` 的 `bindtextdomain()` 改用 `locale_dir`，那段 `#ifdef` 探测删掉。
- 顺带修两处同一来源的问题：`show_about()` / `show_usage()` 原来 Windows 分支
  **写死日语**（`about.jp`、`help/ja`），现在统一走新的 `ui_language()`
  （`src/misc.c`，Windows 用 `g_win32_getlocale()`，其它平台看 `LANG`）。
  而且旧代码指向 `help/index.html`——任何布局里都没有这个文件，Help 菜单在
  Windows 上一直是打不开的，现在指向 `help/<lang>/index.html`。

### 验证

| 环境 | 菜单栏 |
| --- | --- |
| 无 `LANG`/`LC_*`（系统 zh-CN） | 文件 视图 检索方式 工具 帮助 |
| `LANG=zh_TW.UTF-8` | 檔案 檢視 檢索方式 工具 幫助 |
| `LANG=ja_JP.UTF-8` | ファイル 表示 検索方法 ツール ヘルプ |
| `LANG=en_US.UTF-8` | File View Search Method Tools Help |

实测脚本 `build/i18n_test.py`（gitignore）：清掉所有 `LC_*`/`LANG` 再启动，
按窗口类 `gdkWindowToplevel` 找主窗口截图，用来肉眼比对。
另外确认中文界面下没有豆腐块——`data/gtkrc` 的兜底字体 `MS Gothic 9` 覆盖
大部分汉字，缺的字由 Pango 的 win32 fallback 补上。

### 附带：帮助手册的中文版

`doc/` 原来只有 `ja`（EUC-JP，帧结构 index/menu/body）和 `en`（单页）。
中文界面既然叫得出来，手册也得跟上，于是照 `ja` 的结构补了 `doc/zh_CN` 和
`doc/zh_TW`（UTF-8，`<meta charset=UTF-8>`，锚点 SEC1–SEC15 与 menu.html
一一对应，脚本校验 52/52 无缺失、无多余）。术语按 `po/zh_CN.po`、
`po/zh_TW.po` 现有的译法对齐：检索方式／完全一致检索／关键字检索／词典组／
过滤器／快捷键；繁体用「辭典、檔案、視窗、剪貼簿、快取、字型、滑鼠」。
繁体版由 OpenCC `s2twp` 从简体转出，再手工修了几处 OpenCC 词选得不对的
地方（`擴充套件正規表示式` → `擴充`、`物件` → `對象`、`影象` → `影像`、
`單詞` → `單字`）。

`ui_language()` 相应改成返回完整标签（`ja` / `zh_CN` / `zh_TW` / `en`）：
Windows 取 `g_win32_getlocale()`，其它平台按 gettext 的顺序看
`LC_ALL` → `LC_MESSAGES` → `LANG`，并去掉 `.UTF-8` 之类的后缀；
`zh_TW` / `zh_HK` / `zh_MO` / `zh_Hant` 都归到 `zh_TW`。

**关于「关于」对话框**：它仍然只显示 `about.en` / `about.jp`。原因是正文
管线是 EUC-JP——`render.c:172` 把传入的文本按 EUC-JP 转成 UTF-8，
`show_about()` 读文件后是原样喂给 `show_text()` 的。中文根本没法规格化进
EUC-JP，所以非日语一律回退英文（ASCII，两边都安全）。

顺带修了 `show_usage()` 的一个真实缺陷：它原本先
`iconv_convert("utf-8", "euc-jp", _("Help will be shown..."))` 再 `show_text()`。
中文经这一趟会被塞进替换字符，正文里显示成
`帮助将用外部####打开`；整段全是不可映射字符时还会得到空串，而
`show_text()` 里 `text[length-1]` 会越界读（`length == 0` 时访问 `text[-1]`）。
现在这条提示统一走 `status_message()`——状态栏是纯 GTK 部件，吃 UTF-8，
和 `websearch.c` 里 "Lanuching web browser..." 的做法一致。

## 14. 复现/验证清单

```bash
cd /e/git/EBView
cmake -S . -B build -G Ninja      # 首次；之后直接 ninja
cd build && ninja
./ebview.exe                       # UCRT64 终端 PATH 已含 /ucrt64/bin，DLL 可直接找到
```

- 已验证：主窗口可正常显示（Pango 崩溃已根治）；`ldd build/ebview.exe` 无缺失 DLL
  （libeb 静态链接）；`gtkrc` 提示已消除；文件搜索可展开目录/命中/打开；
  `eb_bind("E:\daijirin")` 成功并可检索（§7）。
- 已修复：§3 Pango 启动崩溃、§4 gtkrc 提示、§5 Shift_JIS 文件名、§6 非 ASCII 路径
  的 stat/fopen 与 ShellExecute、§7 Windows 绝对路径导致找不到词典 + 扫描无反馈、
  §8 结构体 ABI 不一致导致 `Search Disk` 后堆损坏崩溃、§9 libeb 窄字符
  `open()`/`opendir()` 导致非 ASCII 词典目录打不开、§10 Samba 共享上
  `Search Disk` 弹窗后界面卡死（GTK grab 栈）、§11 配置里失效词典被静默删除、
  §12 Win10 拖动标题栏 / 拉边框缩放导致界面永久卡死（`gdk_threads_init`）、
  §13 Windows 上界面语言永远是英文（mo 路径绑错）+ 帮助手册补简体/繁体版。
- 已验证（§12）：空窗口 + `gdk_threads_init()` 即可复现，去掉后 3/3 拖动、
  3/3 缩放均正常且有真实位移；搜索线程跑完整链路，搜索进行中拖动也不卡。
- 已验证（§11）：往 `dictgroup.xml` 注入一条指向不存在目录的条目，启动后
  ①弹出一条汇总提示（文案正确、可点掉、主窗口不受影响）；②`gdb` 里
  `call save_dictgroup()` 之后条目连同全部字段仍在文件里；③把真实词典的
  `subbook` 改成 99，日志报 `Subbook number 99 is out of range ... has 1
  subbook(s)` 而不是拿垃圾值去查子书。
- 已验证（用户实测）：Dictionary Group 里 `Path=E:\daijirin`、`Depth=0`、点
  `Search Disk` 能出现在 “Disk Search Result” 组下并正常查词（§7/§8 修复后）。
- 待验证（§10 修复后）：对整个 Samba 共享（`W:` → `\\192.168.2.119\yx`）做
  Search Disk，预期是：扫描期间显示 watch 光标；结束后只弹**一条**汇总
  （`Added N dictionaries. M could not be used ...`），且该对话框的 OK 可以
  正常点掉——不再出现"弹出警告后整个界面无响应"。
- 已知限制：无。原「词典目录名必须 ASCII」的限制在 §9 已解除。
