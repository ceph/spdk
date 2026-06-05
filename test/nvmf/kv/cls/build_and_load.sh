#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Build the THROWAWAY KVX-3 test object class (cls_kvtest) against the Ceph tree
# and load it into the running vstart OSD so KV Exec -> rados_aio_exec can invoke
# it. NOT production tooling (ADR-0005 keeps real cls authoring to a later slice).
#
# Env (defaults target the in-repo vstart at /mnt/ceph):
#   CEPH_SRC   Ceph source tree (default /mnt/ceph/src)
#   CEPH_BUILD Ceph build dir   (default /mnt/ceph/build)
#   CEPH_CXX   C++ compiler Ceph was built with (default: auto-detected from
#              the OSD's ninja rule; falls back to /usr/bin/c++)
#
# WHY THIS IS NOT A BARE `g++ -std=c++20 -fPIC -shared`
# ----------------------------------------------------
# An object class is dlopen()'d INTO the running OSD process and its methods run
# on the OSD's op thread-pool threads (tp_osd_tp). It must therefore be ABI- and
# runtime-compatible with the OSD binary. A bare g++ build that merely scrapes a
# few include dirs gets this WRONG in ways that do not surface at load time but
# CRASH THE OSD when a method runs:
#   * Different C++ standard / libstdc++ ABI than the OSD (Ceph builds with
#     -std=c++23, hardening macros like -D_GLIBCXX_ASSERTIONS, -D__CEPH__,
#     -DCEPH_DEBUG_MUTEX, etc.). A mismatched bufferlist / exception layout
#     leads to a SEGV inside libgcc_s exception unwinding on the OSD op thread.
#   * Missing Ceph defines change struct layouts in the shared headers, so the
#     class and the OSD disagree on the memory layout of objects they exchange.
#
# The robust fix is to build cls_kvtest with the EXACT compiler, flags, defines
# and link line Ceph uses for its own cls_* libraries (e.g. cls_hello). We do
# not vendor a hand-maintained copy of those flags: we EXTRACT them live from
# the OSD's own build (the ninja rule for cls_hello) so they always match the
# Ceph tree this vstart was actually built from. The result is byte-for-byte the
# same toolchain/ABI the OSD itself uses, so the method runs without crashing.
#
# The vstart ceph.conf already sets 'osd class load list = *' and
# 'osd class default list = *', so dropping the .so into the OSD class dir
# (osd class dir = $CEPH_BUILD/lib) is enough; the OSD loads the class lazily on
# first use, so no OSD restart is required.

set -eu

here=$(readlink -f "$(dirname "$0")")
CEPH_SRC="${CEPH_SRC:-/mnt/ceph/src}"
CEPH_BUILD="${CEPH_BUILD:-/mnt/ceph/build}"

out="$here/libcls_kvtest.so"
obj="$here/cls_kvtest.cc.o"

# Reference class whose live ninja rules we mine for the exact toolchain/flags.
ref_obj="src/cls/CMakeFiles/cls_hello.dir/hello/cls_hello.cc.o"
ref_lib="lib/libcls_hello.so.1.0.0"

# --- Extract Ceph's EXACT compile command for an existing cls -----------------
# `ninja -t commands <obj>` prints the rule's full command line. We take the
# compile line for cls_hello, then surgically retarget it from cls_hello's
# source/object to ours, leaving every compiler/std/define/visibility flag (the
# ABI-relevant bits) untouched.
ref_compile=$(cd "$CEPH_BUILD" && ninja -t commands "$ref_obj" 2>/dev/null | tail -1)
if [[ -z "$ref_compile" ]]; then
	echo "ERROR: could not extract Ceph's compile flags for $ref_obj." >&2
	echo "       Is $CEPH_BUILD a configured Ceph (ninja) build dir?" >&2
	exit 1
fi

# Pull the EXACT source/object literals out of the mined command itself rather
# than reconstructing them: the ninja rule may spell paths with a different
# prefix (e.g. /home/ubuntu/ceph) than our CEPH_SRC symlink (/mnt/ceph),
# so guessing the source path and string-substituting it can silently NO-OP and
# leave the command compiling cls_hello.cc into our object — which then crashes
# the OSD at load (class registers under the wrong name). We extract the real
# `-c <src>`, `-o <obj>` and `-D<...>_EXPORTS` tokens and verify the swap took.
ref_compile_src=$(printf '%s\n' "$ref_compile" | grep -oE -- '-c [^ ]+' | awk '{print $2}')
ref_compile_obj=$(printf '%s\n' "$ref_compile" | grep -oE -- '-o [^ ]+' | awk '{print $2}')
if [[ -z "$ref_compile_src" || -z "$ref_compile_obj" ]]; then
	echo "ERROR: could not parse -c/-o from Ceph's compile command." >&2
	exit 1
fi
compile=$ref_compile
compile=${compile//"$ref_compile_obj"/"$obj"}
compile=${compile//"$ref_compile_src"/"$here/cls_kvtest.cc"}
compile=${compile//cls_hello_EXPORTS/cls_kvtest_EXPORTS}
# Drop cls_hello's depfile flags (paths are now stale); we don't need deps here.
compile=$(printf '%s\n' "$compile" | sed -E 's/-MD -MT [^ ]+ -MF [^ ]+ //')

# Fail loudly if the source swap did not take (would otherwise compile
# cls_hello.cc into cls_kvtest.cc.o and crash the OSD at class-load time).
if printf '%s\n' "$compile" | grep -qE -- '-c [^ ]*cls_hello\.cc'; then
	echo "ERROR: source retarget failed; command still compiles cls_hello.cc." >&2
	echo "       mined -c was: $ref_compile_src" >&2
	exit 1
fi

echo "compiling cls_kvtest with Ceph's own toolchain/flags (mined from cls_hello)"
( cd "$CEPH_BUILD" && eval "$compile" )

# Sanity-check the resulting object actually came from OUR source: it must
# define the kvtest class-name symbol and NOT cls_hello's.
if ! nm "$obj" 2>/dev/null | grep -q '__cls_name__kvtest'; then
	echo "ERROR: compiled object is not cls_kvtest (missing __cls_name__kvtest)." >&2
	echo "       Refusing to install a mislabeled class that would crash the OSD." >&2
	exit 1
fi

# --- Extract Ceph's EXACT link command and retarget it ------------------------
ref_link=$(cd "$CEPH_BUILD" && ninja -t commands "$ref_lib" 2>/dev/null | tail -1)
if [[ -z "$ref_link" ]]; then
	echo "ERROR: could not extract Ceph's link flags for $ref_lib." >&2
	exit 1
fi
# Extract the input object literal (the .cc.o token) from the mined link line
# so the swap is robust to the path prefix ninja happens to use.
ref_link_obj=$(printf '%s\n' "$ref_link" | grep -oE '[^ ]*cls_hello\.dir/hello/cls_hello\.cc\.o' | head -1)
if [[ -z "$ref_link_obj" ]]; then
	echo "ERROR: could not parse cls_hello object from Ceph's link command." >&2
	exit 1
fi
link=$ref_link
link=${link//"$ref_lib"/"$out.1.0.0.tmp"}
link=${link//"$ref_link_obj"/"$obj"}
link=${link//libcls_hello.so.1/libcls_kvtest.so.1}

echo "linking cls_kvtest with Ceph's own link line (mined from cls_hello)"
( cd "$CEPH_BUILD" && eval "$link" )
mv "$CEPH_BUILD/$out.1.0.0.tmp" "$out" 2>/dev/null || mv "$out.1.0.0.tmp" "$out"

# Final guard: the linked .so must export __cls_init and carry our class name.
if ! nm "$out" 2>/dev/null | grep -q '__cls_name__kvtest'; then
	echo "ERROR: linked $out is not cls_kvtest; refusing to install." >&2
	exit 1
fi

echo "built $out"

# Install into the OSD class dir using the soname convention other cls libs use.
dst="$CEPH_BUILD/lib"
cp "$out" "$dst/libcls_kvtest.so.1.0.0"
ln -sf libcls_kvtest.so.1.0.0 "$dst/libcls_kvtest.so.1"
ln -sf libcls_kvtest.so.1 "$dst/libcls_kvtest.so"
echo "installed into $dst (loaded lazily by the OSD on first KV Exec)"
