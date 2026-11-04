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
