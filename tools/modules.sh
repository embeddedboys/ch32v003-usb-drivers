#!/usr/bin/env bash
# Load and unload the kernel drivers on a bench machine, in the one order that
# works, and say what actually happened.
#
#   tools/modules.sh load [insmod args]   # core first, then every child
#   tools/modules.sh unload               # children first, then the core
#   tools/modules.sh status               # what is loaded right now
#
# `make -C kernel test` does the same insmods, but it appends `|| true` to every
# one of them, so a module that fails to load is invisible - and a missing child
# is exactly the kind of failure that later looks like "the GPIO line is not
# there".  This script checks every exit code, prints the module that failed with
# the kernel's own message, and finishes with a summary.  It is also the only
# place that remembers the two things that are easy to get wrong:
#
#  * **the ADC child needs the IIO core.**  On this machine `industrialio` is
#    only present compressed (`industrialio.ko.zst`), which `insmod` cannot read,
#    and `modprobe` is not in the passwordless sudo set - so the module is
#    decompressed to /tmp with `zstd -d` first and loaded from there.
#  * **the order matters.**  Every child uses symbols the core exports, so
#    loading a child first fails with `Unknown symbol in module`.
#
# Only `insmod`, `rmmod` and `dmesg` are available without a password, and `sudo
# -n` is used so that anything else fails immediately instead of hanging on a
# prompt nobody can answer.

set -u

SUDO=sudo
SUDO_OPTS=-n
INSMOD=/usr/bin/insmod
RMMOD=/usr/bin/rmmod
DMESG=/usr/bin/dmesg

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
kernel=$here/../kernel

# core first, then children; unload walks this list backwards.  The order of the
# children among themselves does not matter - only that the core is first.
MODULES=(usb-mfd v003-gpio v003-i2c v003-spi v003-wdt v003-pwm v003-adc v003-uart)

failures=0
loaded=()

say() { printf '%s\n' "$*"; }
ok() { printf 'ok   %s\n' "$*"; }
bad() { printf 'FAIL %s\n' "$*"; failures=$((failures + 1)); }
info() { printf 'info %s\n' "$*"; }

kernel_message() {
	$SUDO $SUDO_OPTS $DMESG 2>/dev/null | tail -"${1:-6}"
}

# ---------------------------------------------------------------- load --------

ensure_iio() {
	# only the ADC child needs this; a machine that has industrialio built in
	# reports it in lsmod already and nothing has to happen
	if lsmod | grep -q '^industrialio'; then
		info "industrialio is already loaded (the ADC child needs it)"
		return 0
	fi

	local zst=/lib/modules/"$(uname -r)"/kernel/drivers/iio/industrialio.ko.zst
	local plain=/lib/modules/"$(uname -r)"/kernel/drivers/iio/industrialio.ko
	local tmp=/tmp/v003-industrialio.ko

	if [ -f "$plain" ]; then
		info "loading industrialio from $plain"
		$SUDO $SUDO_OPTS $INSMOD "$plain" && return 0
		bad "insmod $plain failed:"; kernel_message 6; return 1
	fi

	if [ ! -f "$zst" ]; then
		bad "no industrialio at $plain or $zst: load it yourself (modprobe industrialio)"
		return 1
	fi

	# decompress once and reuse; a stale copy would be a different kernel's
	if [ ! -f "$tmp" ] || [ "$zst" -nt "$tmp" ]; then
		info "decompressing $(basename "$zst") to $tmp (insmod cannot read .zst)"
		if ! zstd -q -d -f -o "$tmp" "$zst"; then
			bad "zstd could not decompress $zst"; return 1
		fi
	fi

	$SUDO $SUDO_OPTS $INSMOD "$tmp" || {
		bad "insmod $tmp failed:"; kernel_message 6; return 1; }
	ok "industrialio loaded from $tmp"
}

do_load() {
	local arg
	for arg in "${MODULES[@]}"; do
		if [ ! -f "$kernel/$arg.ko" ]; then
			bad "$kernel/$arg.ko is missing: run 'make -C kernel' first"
			return 1
		fi
	done

	# a stale module is a classic afternoon: the header changed, the .ko did not
	local m src
	for m in "${MODULES[@]}"; do
		src=$kernel/${m#v003-}.c
		if [ -f "$src" ] && [ "$src" -nt "$kernel/$m.ko" ]; then
			info "${m}.c is newer than ${m}.ko: run 'make -C kernel' first"
			break
		fi
	done

	# the ADC child is the only consumer of industrialio; load it lazily so a
	# bench without the ADC module still works
	if [ -f "$kernel/v003-adc.ko" ]; then
		ensure_iio || return 1
	fi

	local mod out
	for mod in "${MODULES[@]}"; do
		# extra arguments are module parameters and belong to the core
		# (`reserved=36`, the SPI chip select): a child would reject them
		if [ "$mod" = usb-mfd ] && [ "$#" -gt 0 ]; then
			out=$($SUDO $SUDO_OPTS $INSMOD "$kernel/$mod.ko" "$@" 2>&1)
		else
			out=$($SUDO $SUDO_OPTS $INSMOD "$kernel/$mod.ko" 2>&1)
		fi
		if [ $? = 0 ]; then
			ok "$mod.ko"
			loaded+=("$mod")
		else
			# "File exists" means it was already there, which is not a failure
			if printf '%s' "$out" | grep -q 'File exists'; then
				info "$mod is already loaded"
				loaded+=("$mod")
				continue
			fi
			bad "$mod.ko: $out"
			kernel_message 6
		fi
	done

	return $(( failures > 0 ))
}

# -------------------------------------------------------------- unload --------

do_unload() {
	local i mod
	for (( i=${#MODULES[@]}-1; i>=0; i-- )); do
		mod=${MODULES[$i]}
		local out
		if out=$($SUDO $SUDO_OPTS $RMMOD "$mod" 2>&1); then
			ok "$mod unloaded"
		elif printf '%s' "$out" | grep -q 'not currently loaded\|No such file'; then
			: # not loaded: nothing to do, and not a failure
		elif printf '%s' "$out" | grep -q 'is in use'; then
			bad "$mod is in use: something still holds it (a test, a mount, a shell in /dev/ttyV0?)"
		else
			bad "$mod: $out"
			kernel_message 4
		fi
	done

	if lsmod | grep -qE '^(usb_mfd|usb-mfd|v003_)'; then
		bad "something is still loaded:"
		lsmod | grep -E '^(usb_mfd|usb-mfd|v003_)'
		return 1
	fi
	ok "no usb-mfd or v003 modules left: the interface is free for the pyusb scripts"
	return 0
}

# -------------------------------------------------------------- status --------

do_status() {
	local line
	line=$(lsmod | grep -E '^(usb_mfd|usb-mfd|v003_)' || true)
	if [ -z "$line" ]; then
		info "no usb-mfd or v003 modules loaded (pyusb scripts can open the device)"
	else
		say "$line"
		info "the kernel drivers own the interface: unload them before a pyusb test"
	fi
	if lsmod | grep -q '^industrialio'; then
		info "industrialio loaded (v003-adc.ko can be loaded)"
	fi
}

case "${1:-status}" in
load)
	shift
	do_load "$@"
	code=$?
	say ""
	if [ "$code" = 0 ]; then
		ok "loaded: ${loaded[*]}"
	else
		bad "some modules did not load - read the messages above, they come from the kernel"
	fi
	lsmod | grep -E '^(usb_mfd|usb-mfd|v003_)' || true
	exit "$code"
	;;
unload)
	do_unload
	exit $?
	;;
status)
	do_status
	;;
*)
	say "usage: ${0##*/} {load|unload|status} [insmod arguments]"
	exit 2
	;;
esac
