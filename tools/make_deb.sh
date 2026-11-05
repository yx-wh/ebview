#!/usr/bin/env bash
# 从 CMake 安装树生成 Debian 二进制包。
#
#   bash tools/make_deb.sh
#
# 产物: build/dist/ebview_<version>_<arch>.deb
#
# 单独用一个 build-deb 目录，原因有两点：
#   * 必须用 -DCMAKE_INSTALL_PREFIX=/usr 重新配置，否则 LOCALEDIR /
#     PACKAGEDIR 会编译成 /usr/local/...，装到 /usr 后找不到数据；
#   * 顺带不会动到日常开发用的 build/。
set -eu

cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
[ -n "$VERSION" ] || { echo "无法从 CMakeLists.txt 读出 VERSION" >&2; exit 1; }

ARCH=$(dpkg --print-architecture)
PKG=ebview
BUILD=build-deb
OUT=build/dist
STAGE=$BUILD/stage/${PKG}_${VERSION}_${ARCH}

MAINTAINER=$(git config user.name || true)
MAINTAINER_MAIL=$(git config user.email || true)
MAINTAINER=${MAINTAINER:-EBView maintainers}
MAINTAINER_MAIL=${MAINTAINER_MAIL:-ebview@localhost}

echo "==> 配置并编译 $PKG $VERSION ($ARCH)"
cmake -S . -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    >/dev/null
cmake --build "$BUILD" --parallel "$(nproc)"

echo "==> 安装到暂存目录 $STAGE"
rm -rf "$STAGE"
DESTDIR="$PWD/$STAGE" cmake --install "$BUILD" --strip >/dev/null

# ---- DEBIAN 控制信息 -------------------------------------------------------
mkdir -p "$STAGE/DEBIAN"

# libgtk2.0-0t64 是 trixie 的 time64 转换名，bookworm 及更早仍叫 libgtk2.0-0。
# 用 | 备选依赖，一个包同时适配新旧发行版。
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER <$MAINTAINER_MAIL>
Installed-Size: $(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
Depends: libc6 (>= 2.34), libgtk2.0-0t64 | libgtk2.0-0, libeb16, zlib1g
Recommends: fonts-noto-cjk | fonts-noto-cjk-extra
Suggests: xdg-utils
Section: text
Priority: optional
Homepage: https://github.com/fujii/ebview
Description: EPWING electronic dictionary viewer
 EBView is a viewer for EPWING CD-ROM dictionaries, the format used by
 most Japanese electronic dictionaries.  It is built on the EB Library and
 opens dictionaries directly from their directory.
 .
 Features include search across multiple dictionaries, forward/backward/
 keyword/full-text search, X selection lookup, rendering of gaiji and
 embedded images, dictionary groups, bookmarks and web search engines.
EOF

# Debian 要求 /usr/share/doc/<pkg>/ 下至少有一个 changelog 与 copyright。
DOCDIR="$STAGE/usr/share/doc/$PKG"
mkdir -p "$DOCDIR"
gzip -9n -c NEWS > "$DOCDIR/changelog.gz"

cat > "$DOCDIR/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: ebview
Source: https://github.com/fujii/ebview

Files: *
Copyright: 2001-2003 Kenichi Suto <deep_blue@users.sourceforge.net>
License: GPL-2+
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 .
 On Debian systems the full text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
EOF

# 图标缓存与桌面数据库；两个工具都不保证存在，失败不算错误。
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database -q /usr/share/applications || true
fi
exit 0
EOF

cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
    command -v gtk-update-icon-cache >/dev/null 2>&1 &&
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    command -v update-desktop-database >/dev/null 2>&1 &&
        update-desktop-database -q /usr/share/applications || true
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

# md5sums 由 dpkg 用来校验/卸载，路径必须相对于根目录。
( cd "$STAGE" && find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums )

# ---- 打包 ------------------------------------------------------------------
mkdir -p "$OUT"
DEB=$OUT/${PKG}_${VERSION}_${ARCH}.deb
rm -f "$DEB"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB" >/dev/null

echo
echo "deb : $DEB  ($(du -h "$DEB" | cut -f1))"
dpkg-deb --info "$DEB" | sed -n '1,12p'
