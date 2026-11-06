#!/usr/bin/env bash
# 把构建产物收成一个"拷到别的 Windows 机器就能跑"的目录。
#
#   bash tools/pack_win.sh [输出目录]      # 默认 build/dist
#
# 默认收 UCRT64 的 build/；换工具链时用环境变量覆盖，例如 MINGW64：
#   UCRT=/mingw64 BUILD=/e/git/EBView/build-mingw64 \
#       bash tools/pack_win.sh E:/git/EBView/build-mingw64/dist
#
# 构筑系统（ucrt64 / mingw64 / clang64 ...）从 UCRT 前缀自动推断，写进 zip
# 文件名和包内 BUILD-INFO.txt；要手工指定可设 SYSNAME=...，SUFFIX 可再加标签。
#
# 产出 <输出目录>/EBView/ 与
#   <输出目录>/EBView-<版本>-win64-<构筑系统>[<SUFFIX>].zip
# 目录布局必须保持：
#   EBView/ebview.exe
#   EBView/BUILD-INFO.txt          <- 构筑系统、编译器、CRT 等溯源信息
#   EBView/share/ebview/*          <- locate_package_dir() 找 share/ebview/shortcut.xml
#   EBView/locale/<lang>/LC_MESSAGES/ebview.mo   <- locate_locale_dir()
#   EBView/lib/gdk-pixbuf-2.0/...  <- 词典正文里的插图
#   EBView/etc/fonts/*             <- fontconfig 配置
set -e

UCRT=${UCRT:-/c/Users/yx/msys64/ucrt64}
VERSION=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' \
	"$(dirname "$0")/../CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "无法从 CMakeLists.txt 读出 VERSION" >&2; exit 1; }
BUILD=${BUILD:-$(cd "$(dirname "$0")/../build" && pwd)}
OUT=${1:-$BUILD/dist}
DEST=$OUT/EBView

# 构筑系统标识：从工具链前缀路径推断，可用 SYSNAME 显式覆盖。
# 它同时进 zip 文件名和包内 BUILD-INFO.txt，UCRT64 / MINGW64 包不会再混。
SYSNAME=${SYSNAME:-}
if [ -z "$SYSNAME" ]; then
	case "$UCRT" in
		*mingw64*) SYSNAME=mingw64 ;;
		*clang64*) SYSNAME=clang64 ;;
		*ucrt64*)  SYSNAME=ucrt64 ;;
		*mingw32*) SYSNAME=mingw32 ;;
		*clang32*) SYSNAME=clang32 ;;
		*)         SYSNAME=$(basename "$UCRT") ;;
	esac
fi

if [ ! -f "$BUILD/ebview.exe" ]; then
	echo "找不到 $BUILD/ebview.exe，先编译" >&2
	exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"

cp "$BUILD/ebview.exe" "$DEST/"

# 依赖闭包：ldd 会递归，只拷工具链前缀里的 DLL（系统 DLL 不拷）。
# ldd 打印的可能是 MSYS 挂载别名（/ucrt64/bin/x.dll），也可能是盘符绝对路径
# （/c/Users/yx/msys64/ucrt64/bin/x.dll）；统一用 cygpath 转成 Windows 路径
# 再比前缀，否则在 UCRT64 终端里一个 DLL 都匹配不上、会静默打出空包。
export PATH="$UCRT/bin:$PATH"
SYSROOT_W=$(cygpath -w "$UCRT" 2>/dev/null || true)
n=0
for dll in $(ldd "$BUILD/ebview.exe" | awk '{print $3}' | sort -u); do
	[ -f "$dll" ] || continue
	if [ -n "$SYSROOT_W" ]; then
		DLL_W=$(cygpath -w "$dll" 2>/dev/null || true)
		if [ -z "$DLL_W" ] || [ "$DLL_W" = "${DLL_W#"$SYSROOT_W"}" ]; then
			continue
		fi
	else
		case "$dll" in *"$UCRT"*) ;; *) continue ;; esac
	fi
	cp "$dll" "$DEST/"
	n=$((n + 1))
done
if [ "$n" -eq 0 ]; then
	echo "错误：$UCRT 下没找到任何依赖 DLL，拒绝打空包" >&2
	exit 1
fi

cp -r "$BUILD/share" "$DEST/"
cp -r "$BUILD/locale" "$DEST/"

# gdk-pixbuf 的图片加载器。loaders.cache 里是相对路径（lib\gdk-pixbuf-2.0\...），
# gdk-pixbuf 按 DLL 所在目录解析，所以整个目录原样拷过去即可。
mkdir -p "$DEST/lib"
cp -r "$UCRT/lib/gdk-pixbuf-2.0" "$DEST/lib/"
rm -f "$DEST"/lib/gdk-pixbuf-2.0/*/loaders/*.dll.a

# fontconfig 需要 fonts.conf，否则初始化失败、所有文字变豆腐块
mkdir -p "$DEST/etc"
cp -r "$UCRT/etc/fonts" "$DEST/etc/"

# 把构筑系统 / 工具链版本写进包内，拷到别的机器也能一眼看出是哪套编的。
# 所有采集都允许失败并回落 n/a，缺工具不影响打包本身。
# 用预处理器把宏完全展开：头文件里可能是 0x601 / 0xA00（省略前导零），
# 也可能定义成 _WIN32_WINNT_WIN7 这类别名，所以按数值判断、再规范成 4 位。
WINNT_RAW=$(printf '#include <windows.h>\nEBVIEW_WINNT _WIN32_WINNT\n' \
	| gcc -E -P -x c - 2>/dev/null | awk '/EBVIEW_WINNT/ {print $2; exit}')
if [[ "$WINNT_RAW" =~ ^0[xX][0-9a-fA-F]+$ ]]; then
	WINNT_NUM=$((WINNT_RAW))
	WINNT=$(printf '0x%04X' "$WINNT_NUM")
else
	WINNT_NUM=-1
	WINNT=n/a
fi
case "$WINNT_NUM" in
	2560) TARGET="Windows 10" ;;
	1539) TARGET="Windows 8.1" ;;
	1538) TARGET="Windows 8" ;;
	1537) TARGET="Windows 7" ;;
	1536) TARGET="Windows Vista" ;;
	*)    TARGET="unknown" ;;
esac
CRT_IMPORTS=$(objdump -p "$BUILD/ebview.exe" 2>/dev/null | sed -n 's/.*DLL Name: //p')
if printf '%s\n' "$CRT_IMPORTS" | grep -qi '^msvcrt.dll$'; then
	CRT=msvcrt.dll
elif printf '%s\n' "$CRT_IMPORTS" | grep -qi 'ucrtbase.dll'; then
	CRT=ucrtbase.dll
elif printf '%s\n' "$CRT_IMPORTS" | grep -qi 'api-ms-win-crt'; then
	CRT='UCRT (api-ms-win-crt-*)'
else
	CRT=n/a
fi
GCC_VER=$(gcc -dumpversion 2>/dev/null || echo n/a)
GTK_VER=$(pkg-config --modversion gtk+-2.0 2>/dev/null || echo n/a)
GLIB_VER=$(pkg-config --modversion glib-2.0 2>/dev/null || echo n/a)
EB_LIB=$(sed -n 's/^EB_LIBRARY:FILEPATH=//p' "$BUILD/CMakeCache.txt" 2>/dev/null | head -1)
[ -n "$EB_LIB" ] || EB_LIB=n/a
EXE_SHA=$(sha256sum "$BUILD/ebview.exe" 2>/dev/null | awk '{print $1}')
[ -n "$EXE_SHA" ] || EXE_SHA=n/a

cat > "$DEST/BUILD-INFO.txt" <<EOF
EBView ${VERSION} - build information
========================================
build-system      : ${SYSNAME}
toolchain-prefix  : ${UCRT}
target-platform   : _WIN32_WINNT=${WINNT} (${TARGET})
compiler          : gcc ${GCC_VER}
gtk2              : ${GTK_VER}
glib2             : ${GLIB_VER}
eb-library        : ${EB_LIB}
exe-crt-import    : ${CRT}
dll-count         : ${n}
exe-sha256        : ${EXE_SHA}
build-tree        : ${BUILD}
packed-at         : $(date '+%Y-%m-%d %H:%M:%S %z')
EOF

# 打包（这台机器上没有 zip 命令，退回 python 的 zipfile）
ZIP=$OUT/EBView-${VERSION}-win64-${SYSNAME}${SUFFIX}.zip
rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
	(cd "$OUT" && zip -qr "$(basename "$ZIP")" EBView)
else
	# MSYS2 里的 python 可能是原生 Windows 版，认不得 /e/... 这种路径。
	if command -v cygpath >/dev/null 2>&1; then
		PYOUT=$(cygpath -w "$OUT")
		PYZIP=$(cygpath -w "$ZIP")
	else
		PYOUT=$OUT
		PYZIP=$ZIP
	fi
	python - "$PYOUT" "$PYZIP" <<'PY'
import os, sys, zipfile
out, zpath = sys.argv[1], sys.argv[2]
root = os.path.join(out, "EBView")
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, _, files in os.walk(root):
        for f in files:
            full = os.path.join(base, f)
            z.write(full, os.path.relpath(full, out).replace(os.sep, "/"))
PY
fi

echo "构筑系统: $SYSNAME  ($TARGET)"
echo "DLL: $n 个"
echo "目录: $DEST  ($(du -sh "$DEST" | cut -f1))"
echo "zip : $ZIP  ($(du -h "$ZIP" | cut -f1))"
