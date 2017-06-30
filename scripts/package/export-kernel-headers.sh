#!/bin/bash -e
# This script largely borrowed from rpmspec for the fedora kernel packages (specifically the -devel section)
fail() { echo "$@"; exit 1; }

SOURCEDIR=`cd $1; pwd`
OBJDIR=`cd $2; pwd`
mkdir -p "$3"
OUTDIR=`cd $3; pwd`
ARCH=$4

if [ "$ARCH" = "x86_64" ]; then
	ARCH="x86"
fi

[ -d "$SOURCEDIR" -a -d "$OBJDIR" ] || fail "Usage $0 <kernel source dir> <kernel obj dir> <header export dir>"

[ -e "$SOURCEDIR"/Makefile -a -e "$SOURCEDIR"/Kconfig ] || fail "Must run from within kernel tree."

# Needed for copies that use --parents
cd "$SOURCEDIR"
cp --parents $(find . -type f -name "Makefile*" -o -name "Kconfig*") "$OUTDIR"
rm -rf "$OUTDIR"/{Documentation,scripts,include}
cp -a "$SOURCEDIR"/scripts "$OUTDIR"
cp -a "$OBJDIR"/scripts/* "$OUTDIR"/scripts
rm -f "$OUTDIR"/scripts/*.o "$OUTDIR"/scripts/.*.cmd
rm -f "$OUTDIR"/scripts/*/*.o "$OUTDIR"/scripts/*/.*.cmd
rm -rf "$OUTDIR"/.pc
cp -a --parents arch/"$ARCH"/include "$OUTDIR"
cp -a "$OBJDIR"/arch/"$ARCH"/include/generated "$OUTDIR"/arch/"$ARCH"/include
if [ -d arch/"$ARCH"/mach-zynq ]
then
	cp -a --parents arch/"$ARCH"/mach-zynq/include "$OUTDIR"
fi
cp -a "$SOURCEDIR"/include "$OUTDIR"/include
cp -a "$OBJDIR"/include/* "$OUTDIR"/include
cp "$OBJDIR"/.config "$OUTDIR"/.config
