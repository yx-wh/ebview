#!/usr/bin/env bash
# close_dlls.sh <dest> <prefix>
#
# 依赖闭包：ebview.exe 直接链接的 DLL 由 pack_win.sh 里的 ldd 抓取，但
# gdk-pixbuf 的图片加载器（lib/gdk-pixbuf-2.0/.../loaders/*.dll）是在运行时被
# dlopen 的，它们自己的依赖（librsvg、libxml2、libcroco …）不会被 ldd ebview.exe
# 抓到。这里以这些插件 DLL 为种子做依赖闭包，把仍缺失、且来自工具链前缀的 DLL
# 补进 dest，避免"SVG 加载器因缺 DLL 断言崩溃 / 图标全丢"。
#
# 只顺着插件与新补进来的 DLL 走（worklist），不每轮全量扫描整个 dest，因此在
# Windows 上足够快。系统 DLL（C:/Windows/...）一律不收。
#
# 用法（由 pack_win.sh 在内部 MSYSTEM bash 里调用）：
#   bash close_dlls.sh "$DEST" "$PREFIX"
set -e

DEST="$1"
PREFIX="$2"
[ -d "$DEST" ] || { echo "close_dlls: dest 不存在: $DEST" >&2; exit 1; }
[ -d "$PREFIX" ] || { echo "close_dlls: prefix 不存在: $PREFIX" >&2; exit 1; }

SYSROOT_W=$(cygpath -w "$PREFIX" 2>/dev/null || true)
export PATH="$DEST:$PREFIX/bin:$PATH"

# 种子：所有运行时插件 DLL（gdk-pixbuf 加载器、gio 模块等）。
todo=$(find "$DEST/lib" -type f -name '*.dll' 2>/dev/null)
round=0
while [ -n "$todo" ] && [ $round -lt 12 ]; do
    next=""
    for bin in $todo; do
        for dll in $(ldd "$bin" 2>/dev/null | awk '{print $3}' | sort -u); do
            [ -f "$dll" ] || continue
            DLL_W=$(cygpath -w "$dll" 2>/dev/null || true)
            [ -z "$DLL_W" ] && continue
            # 只收工具链前缀里的 DLL，系统 DLL（C:/Windows/...）不收。
            case "$DLL_W" in "$SYSROOT_W"*) ;; *) continue ;; esac
            b=$(basename "$dll")
            [ -f "$DEST/$b" ] && continue
            cp "$dll" "$DEST/" || continue
            next="$next $DEST/$b"
        done
    done
    todo="$next"
    round=$((round + 1))
done
echo "close_dlls: 完成（${round} 轮），dest DLL 数: $(ls "$DEST"/*.dll 2>/dev/null | wc -l)"
