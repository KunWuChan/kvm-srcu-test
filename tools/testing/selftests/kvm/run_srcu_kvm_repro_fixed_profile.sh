#!/usr/bin/env bash
set -euo pipefail

# Self-contained runner in selftests/kvm directory.
# Usage:
#   bash ./run_srcu_kvm_repro_fixed_profile.sh 0
#   bash ./run_srcu_kvm_repro_fixed_profile.sh 1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BT_SCRIPT="${SCRIPT_DIR}/srcu-kvm-bpftrace-callsite.bt"
BIN="${SCRIPT_DIR}/srcu_kvm_fixed_profile_test"
storm_arg="${1:-}"
post_run_settle_s="${POST_RUN_SETTLE_S:-45}"

case "${storm_arg}" in
	0|off|nostorm) storm=0 ;;
	1|on|storm) storm=1 ;;
	*)
		echo "[error] usage: $0 <storm>"
		echo "        storm: 0|1 (or off|on, nostorm|storm)"
		exit 2
		;;
esac

if [[ ! -f "${BT_SCRIPT}" ]]; then
	echo "[error] bpftrace script not found: ${BT_SCRIPT}"
	exit 2
fi
if [[ ! -x "${BIN}" ]]; then
	echo "[error] selftest binary not found or not executable: ${BIN}"
	exit 2
fi

TS="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="${OUT_DIR:-${PWD}/srcu-kvm-fixed-profile-${TS}-storm${storm}}"
mkdir -p "${OUT_DIR}"
BT_OUT="${OUT_DIR}/bpftrace.out"
TEST_OUT="${OUT_DIR}/selftest.stdout"

cleanup() {
	if [[ -n "${BT_PID:-}" ]] && kill -0 "${BT_PID}" 2>/dev/null; then
		kill -INT "${BT_PID}" 2>/dev/null || true
		wait "${BT_PID}" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "[info] output_dir=${OUT_DIR}"
echo "[info] storm=${storm}"

echo "[info] starting bpftrace..."
if [[ "${EUID}" -eq 0 ]]; then
	bpftrace "${BT_SCRIPT}" > "${BT_OUT}" 2>&1 &
else
	sudo bpftrace "${BT_SCRIPT}" > "${BT_OUT}" 2>&1 &
fi
BT_PID=$!

for _i in $(seq 1 30); do
	if rg -n "Attaching" "${BT_OUT}" >/dev/null 2>&1; then
		break
	fi
	if ! kill -0 "${BT_PID}" 2>/dev/null; then
		echo "[error] bpftrace exited before attach; see ${BT_OUT}:"
		sed -n '1,50p' "${BT_OUT}" 2>/dev/null || true
		exit 3
	fi
	sleep 0.2
done

echo "[info] running fixed profile selftest..."
"${BIN}" "${storm}" | tee "${TEST_OUT}"

if [[ "${post_run_settle_s}" != "0" ]]; then
	echo "[info] waiting ${post_run_settle_s}s for trailing GP_END samples..."
	sleep "${post_run_settle_s}"
fi

echo "[info] stopping bpftrace..."
kill -INT "${BT_PID}" 2>/dev/null || true
wait "${BT_PID}" 2>/dev/null || true
unset BT_PID

get_bt_counter() {
	local key="$1"
	awk -F': ' -v k="$key" '
	$1 == k { v = $2 }
	END {
		if (v == "") print "";
		else {
			gsub(/^[ \t]+|[ \t]+$/, "", v);
			print v;
		}
	}
	' "${BT_OUT}"
}

gp_end_seen_total="$(get_bt_counter "@gp_end_seen_total")"
call_main_cnt="$(get_bt_counter "@call_main_cnt")"
call_memslot_cnt="$(get_bt_counter "@call_memslot_cnt")"
call_ioeventfd_cnt="$(get_bt_counter "@call_ioeventfd_cnt")"
kvm_reader_ge_1ms="$(get_bt_counter "@kvm_reader_ge_1ms")"
kvm_reader_ge_4ms="$(get_bt_counter "@kvm_reader_ge_4ms")"

for vname in gp_end_seen_total call_main_cnt call_memslot_cnt call_ioeventfd_cnt kvm_reader_ge_1ms kvm_reader_ge_4ms; do
	v="${!vname}"
	if [[ -z "${v}" ]]; then
		v=0
		printf -v "${vname}" '%s' "${v}"
	fi
	if [[ ! "${v}" =~ ^[0-9]+$ ]]; then
		echo "[error] failed to parse ${vname}: '${v}'"
		exit 4
	fi
done

if (( gp_end_seen_total == 0 )); then
	echo "[error] no gp_end events captured"
	exit 4
fi
if (( call_main_cnt == 0 )); then
	echo "[warn] no paired call_main samples captured"
fi

echo "[ok] done"
echo "[ok] selftest output: ${TEST_OUT}"
echo "[ok] bpftrace output: ${BT_OUT}"
echo "[ok] counters: gp_end=${gp_end_seen_total} call_main=${call_main_cnt} memslot=${call_memslot_cnt} ioeventfd=${call_ioeventfd_cnt} reader_ge_1ms=${kvm_reader_ge_1ms} reader_ge_4ms=${kvm_reader_ge_4ms}"
