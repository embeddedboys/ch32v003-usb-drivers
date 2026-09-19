#!/usr/bin/env bash
# Build the firmware, flash it, and print the two budgets that this chip makes
# easy to get wrong.
#
#   tools/build.sh                 # build and flash (the default build)
#   tools/build.sh --no-flash      # build only
#   tools/build.sh MODULES=gpio,spi TRACE=1   # anything make -C vendor takes
#
# The toolchain is an xPack GCC that is not on PATH by default, and every
# command in this repo starts with the same `export PATH=...`.  This script finds
# it (the usual install prefix under ~/.local/opt, newest first) and says which
# one it used, so a build never silently picks a different compiler.
#
# The budgets it reports are the ones that bite here:
#
#  * **flash**: `vendor.bin` against the part's 16 kB.  The default build is at
#    about 77 %, and running out is a link error - loud, easy.
#  * **RAM**: statics grow *up* from 0x20000000 and the stack grows *down* from
#    0x20000800 (2 kB of SRAM), so an overrun corrupts variables instead of
#    faulting.  The headroom printed here is what is left for the stack, and the
#    number to compare against is the *measured* one from `V003_GET_STACK_FREE`
#    (`tools/status.py`), not the optimistic one: 352 bytes on a fresh boot, 448
#    once every module has run this session, 350 is the documented floor.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$here/..
vendor=$root/vendor

flash_bytes=16384
ram_top=$((0x20000800))
stack_floor=350

find_toolchain() {
	if command -v riscv-none-elf-gcc >/dev/null 2>&1; then
		printf '%s\n' "$(command -v riscv-none-elf-gcc)"
		return 0
	fi
	local prefix=$HOME/.local/opt
	[ -d "$prefix" ] || return 1
	# newest version last, so take the last match
	local cand
	cand=$(ls -d "$prefix"/xpack-riscv-none-elf-gcc-*/bin 2>/dev/null | sort -V | tail -1)
	[ -n "$cand" ] || return 1
	printf '%s\n' "$cand"
	return 0
}

toolchain_bin=$(find_toolchain)
if [ -z "$toolchain_bin" ]; then
	echo "FAIL no riscv-none-elf-gcc on PATH and no xpack toolchain in ~/.local/opt" >&2
	echo "     install it from https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases" >&2
	exit 1
fi
export PATH=$toolchain_bin:$PATH
echo "info toolchain $toolchain_bin"

flash=1
args=()
for arg in "$@"; do
	case $arg in
	--no-flash) flash=0 ;;
	*) args+=("$arg") ;;
	esac
done

target=flash
[ "$flash" = 1 ] || target=build

if [ "${#args[@]}" -gt 0 ]; then
	echo "info make -C vendor $target ${args[*]}"
	make -C "$vendor" "$target" "${args[@]}"
else
	make -C "$vendor" "$target"
fi
status=$?
if [ "$status" != 0 ]; then
	echo "FAIL the build failed (exit $status)"
	exit "$status"
fi

# ------------------------------------------------------------------ budgets ---

if [ -f "$vendor/vendor.bin" ]; then
	bin=$(stat -c %s "$vendor/vendor.bin")
	pct=$((bin * 100 / flash_bytes))
	ok=ok
	[ "$bin" -le "$flash_bytes" ] || ok=FAIL
	printf '%s   flash %d / %d bytes (%d %%) - vendor/vendor.bin\n' \
		"$ok" "$bin" "$flash_bytes" "$pct"
fi

# _ebss is the end of the statics; the linker symbol is read out of the ELF so
# this cannot drift from what was actually linked
ebss=$(riscv-none-elf-nm "$vendor/vendor.elf" 2>/dev/null |
	awk '$3 == "_ebss" { print $1 }')
if [ -n "$ebss" ]; then
	used=$((0x$ebss - 0x20000000))
	left=$((ram_top - 0x$ebss))
	ok=ok
	[ "$left" -ge "$stack_floor" ] || ok=FAIL
	printf '%s   ram   statics end at 0x%s (%d bytes of 2048), %d bytes left for the stack\n' \
		"$ok" "$ebss" "$used" "$left"
	if [ "$left" -lt "$stack_floor" ]; then
		echo "FAIL only $left bytes of stack headroom, below the $stack_floor byte floor:"
		echo "     the stack grows down into the statics and corrupts them instead of faulting."
		echo "     Use GET_STACK_FREE (tools/status.py) on hardware to see the real use."
	fi
fi

if [ "$flash" = 1 ]; then
	echo "info flashed.  minichlink can be blocked by the PD0<->PD1 jumper while the UART"
	echo "     port is enabled: 'nothing connected to linker'.  tools/free_swio.py releases it."
fi
