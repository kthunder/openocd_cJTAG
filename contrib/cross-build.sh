#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

# This is an example of how to do a cross-build of OpenOCD using pkg-config.
# Cross-building with pkg-config is deceptively hard and most guides and
# tutorials are incomplete or give bad advice. Some of the traps that are easy
# to fall in but handled by this script are:
#
#  * Polluting search paths and flags with values from the build system.
#  * Faulty pkg-config wrappers shipped with distribution packaged cross-
#    toolchains.
#  * Build failing because pkg-config discards some paths even though they are
#    correctly listed in the .pc file.
#  * Getting successfully built binaries that cannot find runtime data because
#    paths refer to the build file system.
#
# This script is probably more useful as a reference than as a complete build
# tool but for some configurations it may be usable as-is. It only cross-builds
# libusb-1.0, hidapi, libftdi and capstone from source, but the script can be
# extended to build other prerequisites in a similar manner.
#
# Usage:
# export LIBUSB1_SRC=/path/to/libusb-1.0
# export HIDAPI_SRC=/path/to/hidapi
export OPENOCD_CONFIG="--enable-klink --enable-static --disable-shared"
# cd /work/dir
# /path/to/openocd/contrib/cross-build.sh <host-triplet>
#
# For static linking, a workaround is to
# export LIBUSB1_CONFIG="--enable-static --disable-shared"
#
# All the paths must not contain any spaces.

set -e -x

WORK_DIR=$PWD
mkdir -p $WORK_DIR/build

export LIBUSB1_CONFIG="--enable-shared --disable-static"
export HIDAPI_CONFIG="--enable-shared --disable-static --disable-testgui"
export LIBFTDI_CONFIG="-DSTATICLIBS=OFF -DEXAMPLES=OFF -DFTDI_EEPROM=OFF"
export CAPSTONE_CONFIG="CAPSTONE_BUILD_CORE_ONLY=yes CAPSTONE_STATIC=yes CAPSTONE_SHARED=no"
export LIBJAYLINK_CONFIG="--enable-shared --disable-static"

## Source code paths, customize as necessary
:    ${OPENOCD_SRC:="`dirname "$0"`/.."}
:    ${LIBUSB1_SRC:=$OPENOCD_SRC/../libusb-1.0.26}
:     ${HIDAPI_SRC:=$OPENOCD_SRC/../hidapi-hidapi-0.13.1}
:    ${CONFUSE_SRC:=$OPENOCD_SRC/../confuse-3.3}
:    ${LIBFTDI_SRC:=$OPENOCD_SRC/../libftdi1-1.5}
:   ${CAPSTONE_SRC:=$OPENOCD_SRC/../capstone-4.0.2}
: ${LIBJAYLINK_SRC:=$OPENOCD_SRC/../libjaylink-0.3.1}
:     ${JIMTCL_SRC:=$OPENOCD_SRC/../jimtcl-0.83}

OPENOCD_SRC=`readlink -m $OPENOCD_SRC`
LIBUSB1_SRC=`readlink -m $LIBUSB1_SRC`
HIDAPI_SRC=`readlink -m $HIDAPI_SRC`
CONFUSE_SRC=`readlink -m $CONFUSE_SRC`
LIBFTDI_SRC=`readlink -m $LIBFTDI_SRC`
CAPSTONE_SRC=`readlink -m $CAPSTONE_SRC`
LIBJAYLINK_SRC=`readlink -m $LIBJAYLINK_SRC`
JIMTCL_SRC=`readlink -m $JIMTCL_SRC`

HOST_TRIPLET=$1
BUILD_DIR=$WORK_DIR/build/$HOST_TRIPLET-build
LIBUSB1_BUILD_DIR=$BUILD_DIR/libusb1
HIDAPI_BUILD_DIR=$BUILD_DIR/hidapi
CONFUSE_BUILD_DIR=$BUILD_DIR/confuse
LIBFTDI_BUILD_DIR=$BUILD_DIR/libftdi
CAPSTONE_BUILD_DIR=$BUILD_DIR/capstone
LIBJAYLINK_BUILD_DIR=$BUILD_DIR/libjaylink
JIMTCL_BUILD_DIR=$BUILD_DIR/jimtcl
OPENOCD_BUILD_DIR=$BUILD_DIR/openocd

## Root of host file tree
SYSROOT=$WORK_DIR/build/$HOST_TRIPLET-root

## Install location within host file tree
: ${PREFIX=/usr}

## Make parallel jobs
: ${MAKE_JOBS:=12}

## OpenOCD-only install dir for packaging
: ${OPENOCD_TAG:=`git --git-dir=$OPENOCD_SRC/.git describe --tags`}
PACKAGE_DIR=$WORK_DIR/build/openocd_${OPENOCD_TAG}_${HOST_TRIPLET}

#######

# Create pkg-config wrapper and make sure it's used
export PKG_CONFIG=$WORK_DIR/build/$HOST_TRIPLET-pkg-config

cat > $PKG_CONFIG <<EOF
#!/bin/sh

SYSROOT=$SYSROOT

export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR=\${SYSROOT}$PREFIX/lib/pkgconfig:\${SYSROOT}$PREFIX/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=\${SYSROOT}

# The following have to be set to avoid pkg-config to strip /usr/include and /usr/lib from paths
# before they are prepended with the sysroot path. Feels like a pkg-config bug.
export PKG_CONFIG_ALLOW_SYSTEM_CFLAGS=
export PKG_CONFIG_ALLOW_SYSTEM_LIBS=

exec pkg-config "\$@"
EOF
chmod +x $PKG_CONFIG

# Clear out work dir
# rm -rf $SYSROOT $BUILD_DIR
mkdir -p $SYSROOT

# # libusb-1.0 build & install into sysroot
# if [ -d $LIBUSB1_SRC ] ; then
#   mkdir -p $LIBUSB1_BUILD_DIR
#   cd $LIBUSB1_BUILD_DIR
#   $LIBUSB1_SRC/configure --build=`$LIBUSB1_SRC/config.guess` --host=$HOST_TRIPLET \
#   --with-sysroot=$SYSROOT --prefix=$PREFIX \
#   $LIBUSB1_CONFIG
#   make -j $MAKE_JOBS
#   make install DESTDIR=$SYSROOT
# fi

# # hidapi build & install into sysroot
# if [ -d $HIDAPI_SRC ] ; then
#   mkdir -p $HIDAPI_BUILD_DIR
#   cd $HIDAPI_BUILD_DIR
#   $HIDAPI_SRC/configure --build=`$HIDAPI_SRC/config.guess` --host=$HOST_TRIPLET \
#     --with-sysroot=$SYSROOT --prefix=$PREFIX \
#     $HIDAPI_CONFIG
#   make -j $MAKE_JOBS
#   make install DESTDIR=$SYSROOT
# fi

# # confuse build & install into sysroot
# if [ -d $CONFUSE_SRC ] ; then
#   mkdir -p $CONFUSE_BUILD_DIR
#   cd $CONFUSE_BUILD_DIR
#   $CONFUSE_SRC/configure --host=$HOST_TRIPLET \
#     --with-sysroot=$SYSROOT --prefix=$PREFIX --disable-udev --disable-examples \
#     $CONFUSE_CONFIG
#   make -j $MAKE_JOBS
#   make install DESTDIR=$SYSROOT
# fi

# # libftdi build & install into sysroot
# if [ -d $LIBFTDI_SRC ] ; then
#   mkdir -p $LIBFTDI_BUILD_DIR
#   cd $LIBFTDI_BUILD_DIR
#   # note : libftdi versions < 1.5 requires libusb1 static
#   #   hint use : # export LIBUSB1_CONFIG="--enable-static ..."
#   #   not needed since libftdi-1.5 when LIBFTDI_CONFIG="-DSTATICLIBS=OFF ..."

#   # fix <toolchain>.cmake file
#   ESCAPED_SYSROOT=$(printf '%s\n' "$SYSROOT" | sed -e 's/[\/&]/\\&/g')
#   sed -i -E "s/(SET\(CMAKE_FIND_ROOT_PATH\s+).+\)/\1${ESCAPED_SYSROOT})/" \
#     ${LIBFTDI_SRC}/cmake/Toolchain-${HOST_TRIPLET}.cmake

#   cmake $LIBFTDI_CONFIG \
#     -DCMAKE_TOOLCHAIN_FILE=${LIBFTDI_SRC}/cmake/Toolchain-${HOST_TRIPLET}.cmake \
#     -DCMAKE_INSTALL_PREFIX=${PREFIX} -DEXAMPLES=0 \
#     -DPKG_CONFIG_EXECUTABLE=`which pkg-config` \
#     $LIBFTDI_SRC
#   make install DESTDIR=$SYSROOT
# fi

# # capstone build & install into sysroot
# if [ -d $CAPSTONE_SRC ] ; then
#   mkdir -p $CAPSTONE_BUILD_DIR
#   cd $CAPSTONE_BUILD_DIR
#   cp -r $CAPSTONE_SRC/* .
#   make install DESTDIR=$SYSROOT PREFIX=$PREFIX \
#     CROSS="${HOST_TRIPLET}-" \
#     $CAPSTONE_CONFIG
#   # fix the generated capstone.pc
#   CAPSTONE_PC_FILE=${SYSROOT}${PREFIX}/lib/pkgconfig/capstone.pc
#   sed -i '/^libdir=/d' $CAPSTONE_PC_FILE
#   sed -i '/^includedir=/d' $CAPSTONE_PC_FILE
#   sed -i '/^archive=/d' $CAPSTONE_PC_FILE
#   sed -i '1s;^;prefix=/usr \
# exec_prefix=${prefix} \
# libdir=${exec_prefix}/lib \
# includedir=${prefix}/include/capstone\n\n;' $CAPSTONE_PC_FILE
# fi

# # libjaylink build & install into sysroot
# if [ -d $LIBJAYLINK_SRC ] ; then
#   mkdir -p $LIBJAYLINK_BUILD_DIR
#   cd $LIBJAYLINK_BUILD_DIR
#   $LIBJAYLINK_SRC/configure --build=`$LIBJAYLINK_SRC/config.guess` --host=$HOST_TRIPLET \
#     --with-sysroot=$SYSROOT --prefix=$PREFIX \
#     $LIBJAYLINK_CONFIG
#   make -j $MAKE_JOBS
#   make install DESTDIR=$SYSROOT
# fi

# # jimtcl build & install into sysroot
# if [ -d $JIMTCL_SRC ] ; then
#   mkdir -p $JIMTCL_BUILD_DIR
#   cd $JIMTCL_BUILD_DIR
#   $JIMTCL_SRC/configure --host=$HOST_TRIPLET --prefix=$PREFIX \
#     $JIMTCL_CONFIG
#   make -j $MAKE_JOBS
#   # Running "make" does not create this file for static builds on Windows but "make install" still expects it
#   touch $JIMTCL_BUILD_DIR/build-jim-ext
#   make install DESTDIR=$SYSROOT
# fi

# OpenOCD build & install into sysroot
if [ -d $OPENOCD_SRC ] ; then
  mkdir -p $OPENOCD_BUILD_DIR
  cd $OPENOCD_BUILD_DIR
  # $OPENOCD_SRC/configure --build=`$OPENOCD_SRC/config.guess` --host=$HOST_TRIPLET \
  #   --with-sysroot=$SYSROOT --prefix=$PREFIX \
  #   $OPENOCD_CONFIG
  # bear -- make -j $MAKE_JOBS CFLAGS+="-Wno-error"
  make -j $MAKE_JOBS CFLAGS+="-Wno-error"
  make install-strip DESTDIR=$SYSROOT
  # Separate OpenOCD install w/o dependencies. OpenOCD will have to be linked
  # statically or have dependencies packaged/installed separately.
  # make install-strip DESTDIR=$PACKAGE_DIR
  # pwd
  mkdir -p /mnt/c/Users/Administrator/Desktop/openocd
  cp -r ../../i686-w64-mingw32-root/usr/* /mnt/c/Users/Administrator/Desktop/openocd
fi

