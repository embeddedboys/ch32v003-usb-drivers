#!/usr/bin/env bash
# Run everything, in the order the two halves require, and print one table.
#
#   tools/run_tests.sh                  # the whole suite against the firmware as it is
#   tools/run_tests.sh --flash          # build, flash, then test what was just flashed
#   tools/run_tests.sh --repeat 3       # everything three times (the flake check)
#   tools/run_tests.sh --list           # what would run, and what it needs
#   tools/run_tests.sh --kernel-only    # just the driver side
#   tools/run_tests.sh --pyusb-only     # just the raw protocol side
#
# The ordering is not a preference, it is a constraint, and getting it wrong
# produces confusing failures:
#
#  * **the driver side needs the modules loaded** - it goes through the kernel
#    (gpiochip, i2c-dev, IIO, tty), and the children need the core first;
#  * **the pyusb side needs them unloaded** - the interface is claimed by
#    `usb-mfd`, so pyusb cannot open it and the scripts report `Resource busy`.
#
# So: load, run the driver tests, unload, run the protocol tests.  Each test's
# full output goes to a log file ($LOG_DIR, /tmp/v003-tests by default) and the
# table says whether it exited 0; a failing test also prints its FAIL lines and
# the tail of its log here, because scrolling back through forty screens of a
# passing run to find the one failure is what this script exists to avoid.
#
# The kernel side is also checked for **warnings in dmesg**, on the lines the run
# itself produced.  That check is not decoration: the version of the TTY driver
# that freed an embedded `tty_port` corrupted the device-model PM list, and the
# only symptom was a `list_add corruption` line in dmesg - the tests all passed.
# A run that prints warnings is a failing run.
#
# The exit status is 0 only if every test passed and dmesg stayed clean.

set -u

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$here/..
cd "$root" || exit 1

PY=$root/.venv/bin/python
LOG_DIR=${LOG_DIR:-/tmp/v003-tests}
SUDO="sudo -n"
DMESG=/usr/bin/dmesg

# tests that go through the kernel, in this order (the core has to be loaded
# before any child, and the children are load-order independent).  A spec is
# `path[:argument]`; `tests/gpio_sysfs.py` takes the line to blink (32 = PC0, the
# board's LED) and says SKIP by itself on a kernel without CONFIG_GPIO_SYSFS.
KERNEL_TESTS=(
	tests/adc_iio_test.py
	tests/gpio_chardev.py
	tests/gpio_sysfs.py:32
	tests/i2c_dev_test.py
	tests/uart_tty_test.py
)

# tests that talk the vendor protocol over pyusb.  `scripts/ctrl_transfer.py`,
# `scripts/get_cfg.py` and `scripts/ep2_out_test.py` are deliberately not here:
# they are scratch dumps of the descriptors and one "Hello, World!" write, with
# no assertions, so they cannot fail and would only pad the pass count.
PYUSB_TESTS=(
	scripts/v003_test.py
	scripts/spi_test.py:--loopback
	scripts/i2c_test.py
	scripts/eeprom_test.py
	scripts/adc_test.py
	scripts/pwm_test.py
	scripts/uart_test.py
	scripts/wdg_test.py
	scripts/pwr_test.py
	scripts/frame_test.py
	scripts/combo_test.py
)

repeat=1
kernel_only=0
pyusb_only=0
flash=0
list_only=0

while [ $# -gt 0 ]; do
	case $1 in
	--repeat) repeat=$2; shift 2 ;;
	--kernel-only) kernel_only=1; shift ;;
	--pyusb-only) pyusb_only=1; shift ;;
	--flash) flash=1; shift ;;
	--list) list_only=1; shift ;;
	-h|--help) sed -n '2,32p' "$0"; exit 0 ;;
	*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

if [ "$list_only" = 1 ]; then
	echo "kernel side (needs the modules loaded; 'tools/modules.sh load'):"
	printf '  %s\n' "${KERNEL_TESTS[@]}"
	echo "pyusb side (needs them unloaded; 'tools/modules.sh unload'):"
	printf '  %s\n' "${PYUSB_TESTS[@]}"
	echo "log directory: $LOG_DIR"
	exit 0
fi

if [ ! -x "$PY" ]; then
	echo "FAIL $PY is missing: create it with" >&2
	echo "     python -m venv .venv && .venv/bin/pip install pyusb" >&2
	exit 1
fi

mkdir -p "$LOG_DIR"
results=()
passed=0
failed=0
skipped=0

# A test that says it skipped and exits 0 tested nothing: report it as a skip.
# Partial skips inside a passing test (an absent BH1750, a check that needs the
# boot button) stay a pass - the test still ran and still asserted what it could.
skipped_whole() { # skipped_whole <logfile>
	grep -q '^SKIP:' "$1" && ! grep -qE '^(ok |PASS|ALL TESTS PASSED)' "$1"
}

run_one() { # run_one <name> <logfile> <command...>
	local name=$1 log=$2
	shift 2
	local start=$SECONDS
	"$@" >"$log" 2>&1
	local code=$?
	local secs=$((SECONDS - start))
	if [ "$code" = 0 ]; then
		if skipped_whole "$log"; then
			results+=("skip $name (${secs}s: $(grep -m1 '^SKIP:' "$log"))")
			skipped=$((skipped + 1))
		else
			results+=("ok   $name (${secs}s)")
			passed=$((passed + 1))
		fi
		return 0
	fi
	# a test that refuses to run because the kernel or the wiring cannot support
	# it is a skip, not a failure - but only if it did not also report a failure
	if { skipped_whole "$log" ||
		grep -qiE 'not supported by this kernel|the jumper is missing|nothing came back' "$log"; } &&
		! grep -qE '^FAIL' "$log"; then
		results+=("skip $name (exit $code, ${secs}s: $log)")
		skipped=$((skipped + 1))
		return 0
	fi
	results+=("FAIL $name (exit $code, ${secs}s: $log)")
	failed=$((failed + 1))
	echo ""
	echo "---- $name failed (exit $code) ----"
	grep -E '^FAIL' "$log" | head -20
	echo "  ... last 20 lines of $log:"
	tail -20 "$log" | sed 's/^/  | /'
	return 1
}

run_spec() { # run_spec <spec>, spec = path[:argument]
	local spec=$1 path arg name
	path=${spec%%:*}
	arg=""
	[ "$spec" != "$path" ] && arg=${spec#*:}
	name=$(basename "$path" .py)${arg:+ ${arg}}
	if [ -n "$arg" ]; then
		run_one "$name" "$LOG_DIR/${path//\//_}.log" \
			timeout 300 "$PY" "$path" $arg
	else
		run_one "$name" "$LOG_DIR/${path//\//_}.log" \
			timeout 300 "$PY" "$path"
	fi
}

restore_modules() {
	# put the machine back the way this run found it
	if [ "$modules_were_loaded" = 1 ]; then
		"$here/modules.sh" load >/dev/null 2>&1
	else
		"$here/modules.sh" unload >/dev/null 2>&1
	fi
}

modules_were_loaded=0
if lsmod | grep -qE '^(usb_mfd|usb-mfd|v003_)'; then
	modules_were_loaded=1
fi
trap restore_modules EXIT

# ------------------------------------------------------------------- flash ----

if [ "$flash" = 1 ]; then
	echo "== build and flash =="
	# flashing needs SWIO, and the UART port holds it through the jumper; the
	# modules have to be unloaded first, both to talk to the device and because
	# a re-enumerating device under a loaded driver means a stale probe
	"$here/modules.sh" unload
	"$PY" "$here/free_swio.py" || echo "info free_swio could not reach the device"
	"$here/build.sh" || exit 1
	sleep 3  # the flash reboots the chip and the host has to re-enumerate it
fi

# ------------------------------------------------------------- device check ---

if [ "$kernel_only" = 0 ]; then
	echo "== the device answers over USB =="
	if ! "$PY" "$here/status.py" > "$LOG_DIR/status.log" 2>&1; then
		echo "FAIL tools/status.py could not talk to the device:"
		sed 's/^/  | /' "$LOG_DIR/status.log"
		echo "     the kernel modules own the interface if they are loaded:"
		lsmod | grep -E '^(usb_mfd|usb-mfd|v003_)' || echo "     (they are not loaded)"
		exit 1
	fi
	grep -E '^(firmware|capabilities|stack margin)' "$LOG_DIR/status.log" | sed 's/^/  /'
fi

# ------------------------------------------------------------- kernel side ----

dmesg_before=0
if [ "$pyusb_only" = 0 ]; then
	echo ""
	echo "== driver side (modules loaded) =="
	if ! "$here/modules.sh" load; then
		echo "FAIL the modules did not all load; the driver tests cannot run"
		exit 1
	fi
	dmesg_before=$($SUDO $DMESG 2>/dev/null | wc -l)
	for r in $(seq 1 "$repeat"); do
		[ "$repeat" -gt 1 ] && echo "  (round $r of $repeat)"
		for t in "${KERNEL_TESTS[@]}"; do
			run_spec "$t"
		done
	done

	echo ""
	echo "== dmesg since the driver tests started =="
	$SUDO $DMESG 2>/dev/null | tail -n +"$((dmesg_before + 1))" > "$LOG_DIR/dmesg.txt"
	if grep -qE 'WARNING|BUG:|Oops|Call Trace|corruption|list_add' "$LOG_DIR/dmesg.txt"; then
		results+=("FAIL dmesg has warnings from this run ($LOG_DIR/dmesg.txt)")
		failed=$((failed + 1))
		grep -nE 'WARNING|BUG:|Oops|Call Trace|corruption|list_add' "$LOG_DIR/dmesg.txt" |
			head -10 | sed 's/^/  | /'
	else
		results+=("ok   dmesg clean (no WARNING/BUG/corruption in $LOG_DIR/dmesg.txt)")
		passed=$((passed + 1))
	fi

	if ! "$here/modules.sh" unload; then
		echo "FAIL the modules did not all unload: the pyusb tests cannot run"
		exit 1
	fi
fi

# -------------------------------------------------------------- pyusb side ----

if [ "$kernel_only" = 0 ]; then
	echo ""
	echo "== protocol side (modules unloaded) =="
	for r in $(seq 1 "$repeat"); do
		[ "$repeat" -gt 1 ] && echo "  (round $r of $repeat)"
		for spec in "${PYUSB_TESTS[@]}"; do
			run_spec "$spec"
		done
	done
fi

# ------------------------------------------------------------------ table -----

echo ""
echo "== summary =="
for line in "${results[@]}"; do
	echo "$line"
done
echo ""
echo "$passed passed, $failed failed, $skipped skipped (logs in $LOG_DIR)"

[ "$failed" = 0 ] || exit 1
exit 0
