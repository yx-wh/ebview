#!/usr/bin/env bash
# 把构建产物收成一个"拷到别的 Windows 机器就能跑"的目录。
#
#   bash tools/pack_win.sh [输出目录]      # 默认 build/dist
#
# 产出 <输出目录>/EBView/，以及同名 zip。
# 目录布局必须保持：
#   EBView/ebview.exe
#   EBView/share/ebview/*          <- locate_package_dir() 找 share/ebview/shortcut.xml
#   EBView/locale/<lang>/LC_MESSAGES/ebview.mo   <- locate_locale_dir()
#   EBView/lib/gdk-pixbuf-2.0/...  <- 词典正文里的插图
#   EBView/etc/fonts/*             <- fontconfig 配置
set -e

UCRT=${UCRT:-/c/Users/yx/msys64/ucrt64}
VERSION=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' \
	"$(dirname "$0")/../CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "无法从 CMakeLists.txt 读出 VERSION" >&2; exit 1; }
BUILD=$(cd "$(dirname "$0")/../build" && pwd)
OUT=${1:-$BUILD/dist}
DEST=$OUT/EBView

if [ ! -f "$BUILD/ebview.exe" ]; then
	echo "找不到 $BUILD/ebview.exe，先编译" >&2
	exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST"

cp "$BUILD/ebview.exe" "$DEST/"

# 依赖闭包：ldd 会递归，只需要 ucrt64 里的那部分（系统 DLL 不拷）
export PATH="$UCRT/bin:$PATH"
n=0
for dll in $(ldd "$BUILD/ebview.exe" | grep -i "$UCRT" | awk '{print $3}' | sort -u); do
	cp "$dll" "$DEST/"
	n=$((n + 1))
done

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

# 打包（这台机器上没有 zip 命令，退回 python 的 zipfile）
ZIP=$OUT/EBView-${VERSION}-win64.zip
rm -f "$ZIP"
if command -v zip >/dev/null 2>&1; then
	(cd "$OUT" && zip -qr "$(basename "$ZIP")" EBView)
else
	python - "$OUT" "$ZIP" <<'PY'
import os, sys, zipfile
out, zpath = sys.argv[1], sys.argv[2]
root = os.path.join(out, "EBView")
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, _, files in os.walk(root):
        for f in files:
            full = os.path.join(base, f)
            z.write(full, os.path.relpath(full, out))
PY
fi

echo "DLL: $n 个"
echo "目录: $DEST  ($(du -sh "$DEST" | cut -f1))"
echo "zip : $ZIP  ($(du -h "$ZIP" | cut -f1))"
