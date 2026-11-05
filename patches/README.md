# libeb 4.4.3 Windows 补丁

这里保存的是对 **libeb 4.4.3** 源码的补丁，不是本仓库的代码。EBView 在
Windows 上依赖静态库 `libeb.a`，而 libeb 有些地方假设了 POSIX 路径语义，
在 Windows 上会失效。因为改的是外部依赖，所以以补丁形式单独保存，构建前
需要先把它应用到 libeb 源码树上。

普通 POSIX 平台不需要这些补丁。

## 应用顺序（重要）

第二个补丁是**相对第一个补丁改完的源码**生成的，`hunk` 的行号依赖前者的
结果，所以必须按下面的顺序应用，不能颠倒、也不能跳过第一个：

| # | 文件 | 解决的问题 | 引入提交 |
|---|------|-----------|---------|
| 1 | `libeb-4.4.3-win32-path.patch` | Windows 绝对路径（`E:\dict`）被当成相对路径拼接 | `3b17f35` |
| 2 | `libeb-4.4.3-win32-unicode-path.patch` | 非 ASCII 词典目录名（通过 UTF-16 宽字符 API 打开） | `9c2e963` |

## 完整步骤

```bash
# 1. 取得原始源码：eb-4.4.3.orig.tar.bz2
#    Debian 源码包：https://deb.debian.org/debian/pool/main/e/eb/
#    或原项目发布页
tar -xjf eb-4.4.3.orig.tar.bz2
cd eb-4.4.3

# 2. 按顺序应用，-p1 表示剥掉第一层路径（a/eb/filename.c -> eb/filename.c）
patch -p1 --dry-run < /path/to/EBView/patches/libeb-4.4.3-win32-path.patch
patch -p1           < /path/to/EBView/patches/libeb-4.4.3-win32-path.patch

patch -p1 --dry-run < /path/to/EBView/patches/libeb-4.4.3-win32-unicode-path.patch
patch -p1           < /path/to/EBView/patches/libeb-4.4.3-win32-unicode-path.patch

# 3. 构建（MSYS2 UCRT64）
export CPPFLAGS="-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0501"
export LIBS="-lws2_32"
./configure --prefix=/ucrt64 --disable-ebnet --disable-nls
make
cd eb && make install
```

构建细节和本机踩到的两个工具链坑（`TMPDIR` 要用 POSIX 风格、`make` 走
libtool 在带 `/usr/bin` 的 PATH 下会失败）记在 `DEBUG-NOTES.md` §9。

## 补丁 1：Windows 绝对路径

`eb_canonicalize_path_name()` 在 `config.h` 里由 `DOS_FILE_PATH` 决定分支，
但 **configure 在 MinGW 上从不定义 `DOS_FILE_PATH`**，于是走了 UNIX 分支，
把 `E:\daijirin` 当相对路径拼成 `<cwd>/E:\daijirin`，`eb_bind()` 返回
`EB_ERR_FAIL_OPEN_CAT`。

补丁在 `eb/filename.c` 里加了一个 `_WIN32` 分支：识别 `X:\`、`X:/`、`\\`
开头的形式，把 `/` 归一化成 `\`，去掉末尾多余的分隔符，然后直接返回成功。
**只改 `filename.c`，一个函数。**

## 补丁 2：非 ASCII 词典目录

libeb 全程用 `char *` 传路径。Windows 的窄字符 `open()` / `opendir()` 会按
**系统 ANSI 代码页**解释参数，而 EBView 在 Windows 上的 `fs_codeset` 是
`UTF-8`（GLib 文件系统编码）。于是 `测试词典` 的 UTF-8 字节被当成 GBK，
拼出的路径不存在。

需要宽字符化的地方只有 4 处——两处 `opendir`（`filename.c` 的
`eb_fix_directory_name()` / `eb_find_file_name()`）和两处 `open`
（`zio.c` 的 `zio_open_raw()`）。目录**内部**的条目名（`CATALOG`、
`HONMON`、`*.ebz`、`GA16HALF`…）在所有 EPWING/EB 词典里都是 ASCII，
所以只有「路径」需要 UTF-16，条目名转回 UTF-8 后上层逻辑不用改。

补丁做两件事：

- 新增 **`eb/win32path.h`**，提供 4 个包装函数。做成 header-only +
  `static inline` 是刻意的：libeb 的 autotools 构建否则要往
  `eb/Makefile.am` 加一个对象。

  | 包装 | Windows | POSIX |
  |---|---|---|
  | `eb_open_readonly(path)` | `_wopen()` | `open()` |
  | `eb_dir_open(path)` | `_wopendir()` | `opendir()` |
  | `eb_dir_read_name(dir)` | `_wreaddir()` + `WideCharToMultiByte` | `readdir()->d_name` |
  | `eb_dir_close(dir)` | `_wclosedir()` | `closedir()` |

- 把上述 4 个调用点改成使用这些包装；`filename.c` 里的
  `DIR *` / `struct dirent *entry` 相应改成 `EB_DIR *` / `const char *name`。

路径转换优先 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, …)`，
**失败时回退 `CP_ACP`**，这样仍在传 ANSI 路径的调用方不会被弄坏。非
Windows 平台走的是原样直通，所以补丁在所有平台上都能干净应用、行为不变。

## 两个必须知道的点

1. **`_FILE_OFFSET_BITS=64` 不能丢。** libeb 由 autoconf 构建，
   `AC_SYS_LARGEFILE` 让 `off_t` 是 8 字节；如果你的程序编译时没有这个宏，
   `off_t` 就是 4 字节，`EB_Book` 会比库里的少 8 字节，
   `eb_initialize_book()` 直接越界写堆，症状是之后随机位置
   `0xC0000374` 崩溃。详见 `DEBUG-NOTES.md` §8。
2. **补丁文件固定为 LF 换行**（`.gitattributes` 里标了 `*.patch -text`）。
   仓库开了 `core.autocrlf=true`，如果不这样标记，签出时补丁会被转成 CRLF，
   `patch` 的上下文匹配就会失败。
