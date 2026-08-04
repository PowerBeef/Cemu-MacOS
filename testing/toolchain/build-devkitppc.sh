#!/bin/bash
# Build devkitPPC + wut from source into $HOME, with no root and no access to
# devkitPro's package host.
#
# WHY THIS EXISTS
#
# The normal way to get this toolchain is devkitPro's installer. Use that if you can:
#
#     curl -fsSLO https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg
#     sudo installer -pkg devkitpro-pacman-installer.pkg -target /
#     sudo dkp-pacman -Syu --noconfirm && sudo dkp-pacman -S --noconfirm wiiu-dev
#
# This script is for when that is not possible. It was written because both halves
# of the normal route were closed at once:
#
#   1. The installer needs sudo, which cannot be answered when driving this repo
#      remotely (and an agent shell has no TTY, so sudo's credential cache cannot
#      carry across from an interactive one either).
#   2. downloads.devkitpro.org returns 403 via Cloudflare from some networks --
#      including its root, while github.com and cloudflare.com return 200, so it is
#      site-specific. That host serves EVERY source devkitPro's buildscripts fetch,
#      binutils/gcc/newlib included, not only their own components.
#
# The way through: their download loop skips files that already exist
# (`if [ ! -f $archive ]`) and honours BUILD_DKPRO_SRCDIR, so pre-staging all six
# archives from upstream mirrors makes the stock scripts run essentially untouched.
#
# WHAT IT PRODUCES
#
#   $PREFIX/devkitPPC   binutils 2.45.1, gcc 15.2.0, newlib 4.6.0.20260123
#   $PREFIX/tools/bin   elf2rpl, rplimportgen, wuhbtool, readrpl, udplogserver
#   $PREFIX/wut         libwut.a + headers
#
# Then:
#   export DEVKITPRO=$HOME/.local/devkitpro
#   export DEVKITPPC=$DEVKITPRO/devkitPPC
#   export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
#
# KNOWN DEVIATIONS FROM A STOCK INSTALL -- rule these out first if something is odd.
# Documented in full in docs/testing/00-test-strategy.md.
#
#   * libgloss/libsysbase/dummy.c is STUBBED. No devkitPro patch creates it; it ships
#     inside their repackaged newlib tarball, which is unreachable here. 54 sibling
#     libsysbase/*.c files are present, only this one is absent.
#   * int32_t/uint32_t are long/unsigned long with this newlib rather than
#     int/unsigned int. That forces a one-line signature fix in wut and -Wno-format
#     for its build. Both are 32-bit on powerpc-eabi, so this is diagnostic-only.
#   * mn10200 binutils is skipped -- GameCube/Wii DSP toolchain, irrelevant to Wii U.
#
# Requires: Xcode CLT, autotools, cmake, and Homebrew (for gmp/mpfr/libmpc/freeimage).
# Takes roughly an hour. Safe to re-run; completed stages are skipped.

set -u

PREFIX="${PREFIX:-$HOME/.local/devkitpro}"
WORK="${WORK:-$HOME/.cache/tessera-toolchain}"
STAGE="$WORK/archives"
LOG="$WORK/build.log"

BINUTILS_VER=2.45.1        # versions come from buildscripts/select_toolchain.sh,
GCC_VER=15.2.0             # the VERSION=2 (devkitPPC) branch. If you bump these,
NEWLIB_VER=4.6.0.20260123  # bump the patches too -- they are version-pinned.
CRTLS_VER=1.0.0
RULES_VER=1.2.1

mkdir -p "$WORK" "$STAGE" "$PREFIX"
exec > >(tee -a "$LOG") 2>&1
echo "=== devkitPPC from-source build $(date) -> $PREFIX"

need() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING: $1"; return 1; }; }
for t in curl git make cmake autoconf automake libtool cc tar; do need "$t" || exit 1; done

echo "--- [1/6] host prerequisites (Homebrew, no sudo)"
brew install gmp mpfr libmpc texinfo freeimage 2>&1 | tail -2

echo "--- [2/6] devkitPro buildscripts (release, not git -- git omits the versions)"
cd "$WORK"
if [ ! -d bsrel ]; then
	tag=$(curl -fsSL https://api.github.com/repos/devkitPro/buildscripts/releases \
		| sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)
	echo "    using buildscripts $tag"
	mkdir -p bsrel
	curl -fsSL "https://api.github.com/repos/devkitPro/buildscripts/tarball/$tag" \
		| tar xz -C bsrel --strip-components=1 || exit 1
fi

echo "--- [3/6] staging sources from reachable mirrors"
fetch() {
	[ -s "$STAGE/$2" ] && { echo "    have $2"; return 0; }
	echo "    fetching $2"
	curl -fsSL --retry 3 -o "$STAGE/$2.part" "$1" && mv "$STAGE/$2.part" "$STAGE/$2"
}
fetch "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz" "binutils-${BINUTILS_VER}.tar.xz" || exit 1
fetch "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"  "gcc-${GCC_VER}.tar.xz"      || exit 1
fetch "https://sourceware.org/pub/newlib/newlib-${NEWLIB_VER}.tar.gz"     "newlib-${NEWLIB_VER}.tar.gz" || exit 1

# crtls/rules exist as tarballs only on the blocked host, but the same content is
# tagged on GitHub. GitHub packs them as <org>-<repo>-<hash>/ while build-crtls.sh
# cds into <name>-<version>/, so repack rather than rename.
repack() {
	[ -s "$STAGE/$4" ] && { echo "    have $4"; return 0; }
	echo "    repacking $4 from GitHub tag $2"
	local tmp="$WORK/.repack.$$"; rm -rf "$tmp"; mkdir -p "$tmp/$3"
	curl -fsSL "https://github.com/devkitPro/$1/archive/refs/tags/$2.tar.gz" \
		| tar xz -C "$tmp/$3" --strip-components=1 || { rm -rf "$tmp"; return 1; }
	tar czf "$STAGE/$4" -C "$tmp" "$3"; rm -rf "$tmp"
}
repack devkitppc-crtls "v${CRTLS_VER}" "devkitppc-crtls-${CRTLS_VER}" "devkitppc-crtls-${CRTLS_VER}.tar.gz" || exit 1
repack devkitppc-rules "v${RULES_VER}" "devkitppc-rules-${RULES_VER}" "devkitppc-rules-${RULES_VER}.tar.gz" || exit 1

echo "--- [4/6] patching devkitPro's buildscripts"
cd "$WORK/bsrel"
# INSTALLDIR is a bare assignment, not ${VAR:-default}, so it must be rewritten.
grep -q "^INSTALLDIR=$PREFIX" build-devkit.sh || \
	sed -i.bak "s|^INSTALLDIR=/opt/devkitpro|INSTALLDIR=$PREFIX|" build-devkit.sh
# UPSTREAM BUG: extract_and_patch takes (name ver pkgrel ext) but the mn10200 call
# passes only three args, so "bz2" is read as the pkgrel and the extension ends up
# empty -> malformed tar. Skipped rather than fixed: mn10200 is the GameCube/Wii DSP
# toolchain and is irrelevant to Wii U.
sed -i.bak2 -e 's|^if \[ \$VERSION -eq 2 \]; then extract_and_patch binutils \$MN_BINUTILS_VER bz2; fi|# skipped: mn10200 (GC/Wii DSP), and the upstream call is malformed|' \
            -e 's|^if \[ \$VERSION -eq 2 \]; then \. \${BUILDSCRIPTDIR}/build-mn10200-binutils.sh.*|# skipped: mn10200 binutils build|' \
            build-devkit.sh
# binutils and gcc bundle a zlib whose zutil.h does `#define fdopen(fd,mode) NULL`,
# which then breaks the macOS SDK's declaration of fdopen in stdio.h.
for f in build-binutils.sh build-gcc-stage1.sh build-gcc-stage2.sh; do
	[ -f "$f" ] && ! grep -q -- '--with-system-zlib' "$f" && \
		sed -i.zbak 's|--prefix=\$prefix|--with-system-zlib --prefix=$prefix|' "$f"
done

echo "--- [5/6] building devkitPPC (the long part)"
export BUILD_DKPRO_AUTOMATED=1 BUILD_DKPRO_PACKAGE=2 BUILD_DKPRO_SRCDIR="$STAGE"
export GCC_DOWNLOAD_PREREQS=1   # gcc fetches gmp/mpfr/mpc itself from gcc.gnu.org
export OSXMIN=11.0              # arm64 macOS minimum; the 10.9 default is invalid
export MAKEFLAGS="-j$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# libgloss/Makefile lists libsysbase/dummy.c but no patch creates it. Stub it before
# the newlib stage; harmless if newlib has not been extracted yet (retry re-runs).
stub_dummy() {
	local d="$WORK/bsrel/.devkitPPC/newlib-${NEWLIB_VER}/libgloss/libsysbase"
	[ -d "$d" ] && [ ! -f "$d/dummy.c" ] && {
		printf '/* Placeholder: listed in libgloss/Makefile but created by no patch.\n * See docs/testing/00-test-strategy.md. */\ntypedef int _libsysbase_dummy_placeholder;\n' > "$d/dummy.c"
		echo "    stubbed libsysbase/dummy.c"
	}
}
./build-devkit.sh; rc=$?
if [ $rc -ne 0 ]; then
	stub_dummy && { echo "    retrying after stub"; ./build-devkit.sh; rc=$?; }
fi
[ -x "$PREFIX/devkitPPC/bin/powerpc-eabi-gcc" ] || { echo "FAILED: no powerpc-eabi-gcc"; exit 1; }
echo "    $("$PREFIX/devkitPPC/bin/powerpc-eabi-gcc" --version | head -1)"

echo "--- [6/6] wut-tools and wut"
export DEVKITPRO="$PREFIX" DEVKITPPC="$PREFIX/devkitPPC"
export PATH="$PREFIX/tools/bin:$DEVKITPPC/bin:$PATH"

cd "$WORK"
[ -d wut-tools ] || git clone -q --depth 1 --recursive https://github.com/devkitPro/wut-tools.git
if [ ! -x "$PREFIX/tools/bin/rplimportgen" ]; then
	cd wut-tools && ./autogen.sh >/dev/null 2>&1
	mkdir -p b && cd b
	FI=$(brew --prefix freeimage 2>/dev/null)   # wuhbtool needs libfreeimage
	CPPFLAGS="-I$FI/include" LDFLAGS="-L$FI/lib" ../configure --prefix="$PREFIX/tools" >/dev/null || exit 1
	make -j8 >/dev/null && make install >/dev/null || exit 1
	cd "$WORK"
fi

[ -d wut ] || git clone -q --depth 1 https://github.com/devkitPro/wut.git
cd wut
# This newlib declares __syscall_lock_try_acquire_recursive as returning int (see
# sys/iosupport.h); wut defines it as int32_t, which here is `long` -- a distinct
# type in C, hence "conflicting types". Match the header.
perl -0pi -e 's/int32_t\n(__SYSCALL\(lock_try_acquire_recursive\))/int\n$1/' libraries/wutnewlib/wut_thread.c
# Same root cause: uint32_t is `unsigned long`, so wut's %u/%d formats trip -Werror.
# Both are 32-bit on powerpc-eabi, so this is diagnostic noise, not an ABI problem.
sed -i.bak 's|^CFLAGS\t:=\t-g -Wall -Werror \\|CFLAGS\t:=\t-g -Wall -Wno-format \\|' Makefile
make -j8 >/dev/null && make install >/dev/null || { echo "FAILED: wut"; exit 1; }

echo
echo "=== DONE $(date)"
echo "  export DEVKITPRO=$PREFIX"
echo "  export DEVKITPPC=$PREFIX/devkitPPC"
echo "  export PATH=\"$PREFIX/tools/bin:$PREFIX/devkitPPC/bin:\$PATH\""
echo
echo "  Then: cd testing/cpu-tests && make && ./run.sh | ./report.py -"
