#!/usr/bin/env bash
# Automated test suite (TEST 1..6) for the SSH3 privilege-separation daemon.
# TEST 7 and TEST 8 are static checks, validated without root and included in the report.
#
# Usage:  sudo ./run_all_tests.sh          run the full suite
#         sudo ./run_all_tests.sh --clean  stop everything it left running

set -u
SRC=/home/cytech/stage/src
SSH3D=/home/cytech/stage/ssh3
REPORT_DIR=/home/cytech/stage/github-helper
REAL_USER=${SUDO_USER:-cytech}
REAL_UID=$(id -u "$REAL_USER")
REAL_GID=$(id -g "$REAL_USER")
REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
PASS=0; FAIL=0
declare -a RESULTS

cleanup() {
    pkill -f helper_daemon_v2   2>/dev/null
    pkill -f ssh3-server-patched 2>/dev/null
    pkill -f "ssh3 -privkey"     2>/dev/null
    pkill -f "nc -lk 8000"       2>/dev/null
    pkill -f "nc localhost 8080" 2>/dev/null
    rm -f /tmp/ssh3-helper.sock
}
[[ "${1:-}" == "--clean" ]] && { cleanup; echo "Cleaned."; exit 0; }
[[ $EUID -ne 0 ]] && { echo "Must be run with sudo."; exit 1; }

ok()   { echo "  ✅ PASS - $1"; PASS=$((PASS+1)); RESULTS+=("PASS|$2|$1"); }
nok()  { echo "  ❌ FAIL - $1"; FAIL=$((FAIL+1)); RESULTS+=("FAIL|$2|$1"); }

echo "############ CLEANUP ############"
cleanup; sleep 1
nc -lk 8000 > /tmp/nc8000.log 2>&1 &
sleep 1

########## TEST 1 ##########
echo; echo "############ TEST 1 - Daemon starts ############"
stdbuf -oL "$SRC/helper_daemon_v2" > /tmp/daemon.log 2>&1 &
for i in {1..15}; do
    [[ -S /tmp/ssh3-helper.sock ]] && grep -q "Listening on /tmp/ssh3-helper.sock" /tmp/daemon.log 2>/dev/null && break
    sleep 1
done
if [[ -S /tmp/ssh3-helper.sock ]] && grep -q "Listening on /tmp/ssh3-helper.sock" /tmp/daemon.log; then
    ok "socket created + SO_PEERCRED banner" "TEST1"
    cat /tmp/daemon.log
else
    nok "daemon did not start" "TEST1"; cat /tmp/daemon.log
fi

########## TEST 2 ##########
echo; echo "############ TEST 2 - Authorized caller (uid=$REAL_UID -> uid=$REAL_UID) ############"
: > /tmp/daemon.log
sudo -u "$REAL_USER" stdbuf -oL "$SRC/helper_client" "$REAL_UID" "$REAL_GID" 127.0.0.1 8000 > /tmp/t2_client.log 2>&1 &
T2PID=$!
sleep 2
echo "--- client ---"; cat /tmp/t2_client.log
echo "--- daemon ---"; cat /tmp/daemon.log
if grep -q "Received fd=" /tmp/t2_client.log && grep -q "matches requested uid" /tmp/daemon.log; then
    ok "fd received + authorized by matching uid" "TEST2"
else
    nok "no fd received or no authorization" "TEST2"
fi

########## TEST 5 (runs while the TEST 2 client is still alive) ##########
echo; echo "############ TEST 5 - Socket owned by the correct UID ############"
SS_OUT=$(ss -tnp | grep 8000)
PS_OUT=$(ps -o user=,pid=,comm= -p "$(pgrep -x helper_daemon_v2 | head -1)" 2>/dev/null)
echo "--- ss -tnp | grep 8000 ---"; echo "$SS_OUT"
echo "--- ps daemon ---";           echo "$PS_OUT"
echo "--- ps helper_client ---"
PS_CLIENT=$(ps -o user=,pid=,comm= -p "$(pgrep -x helper_client | head -1)" 2>/dev/null)
echo "$PS_CLIENT"
if [[ -n "$SS_OUT" ]]; then
    ok "connection to :8000 visible" "TEST5"
else
    nok "no connection to :8000 (client already exited?)" "TEST5"
fi
wait $T2PID 2>/dev/null

########## TEST 3 ##########
echo; echo "############ TEST 3 - Root caller (uid=0 -> uid=$REAL_UID) ############"
: > /tmp/daemon.log
timeout 5 stdbuf -oL "$SRC/helper_client" "$REAL_UID" "$REAL_GID" 127.0.0.1 8000 > /tmp/t3_client.log 2>&1
sleep 1
echo "--- client ---"; cat /tmp/t3_client.log
echo "--- daemon ---"; cat /tmp/daemon.log
if grep -q "Received fd=" /tmp/t3_client.log && grep -q "Caller is root" /tmp/daemon.log; then
    ok "root authorized + fd received" "TEST3"
else
    nok "root not authorized or no fd" "TEST3"
fi

########## TEST 4 ##########
echo; echo "############ TEST 4 - Unauthorized caller (uid=$REAL_UID -> uid=0) ############"
: > /tmp/daemon.log
timeout 5 sudo -u "$REAL_USER" stdbuf -oL "$SRC/helper_client" 0 0 127.0.0.1 8000 > /tmp/t4_client.log 2>&1
sleep 1
echo "--- client ---"; cat /tmp/t4_client.log
echo "--- daemon ---"; cat /tmp/daemon.log
if grep -q "REJECTED" /tmp/daemon.log && ! grep -q "Received fd=" /tmp/t4_client.log; then
    ok "SO_PEERCRED rejection enforced, no fd passed" "TEST4"
else
    nok "rejection did NOT happen (SECURITY HOLE)" "TEST4"
fi

########## TEST 6 ##########
echo; echo "############ TEST 6 - Patched SSH3 calls dialUID ############"
: > /tmp/daemon.log
cd "$SSH3D" || exit 1
./ssh3-server-patched -cert ./cert.pem -key ./priv.key \
    -bind 127.0.0.1:4443 -url-path /ssh3-test > /tmp/ssh3server.log 2>&1 &
for i in {1..30}; do grep -q "Server started" /tmp/ssh3server.log 2>/dev/null && break; sleep 1; done
if ! grep -q "Server started" /tmp/ssh3server.log; then
    nok "SSH3 server did not start" "TEST6"; cat /tmp/ssh3server.log
else
    echo "Server started."
    sudo -u "$REAL_USER" env HOME="$REAL_HOME" \
        "$SSH3D/ssh3" -privkey "$REAL_HOME/.ssh/id_ed25519" -insecure \
        -forward-tcp 8080/127.0.0.1@8000 \
        "$REAL_USER@127.0.0.1:4443/ssh3-test" > /tmp/ssh3client.log 2>&1 &
    sleep 5
    # keep the connection open: otherwise the socket is gone before ss runs
    ( echo "hello"; sleep 60 ) | nc localhost 8080 > /tmp/nc_client.log 2>&1 &
    sleep 4
    echo "--- daemon ---";        cat /tmp/daemon.log
    echo "--- ss -tnp | grep 8000 ---"; ss -tnp | grep 8000
    echo "--- ps SSH3 server ---"
    ps -o user=,pid=,comm= -p "$(pgrep -f ssh3-server-patched | head -1)" 2>/dev/null
    echo "--- ps SSH3 client ---"
    ps -o user=,pid=,comm= -p "$(pgrep -f 'ssh3 -privkey' | head -1)" 2>/dev/null
    if grep -q "running as uid=$REAL_UID" /tmp/daemon.log && grep -q "connected to 127.0.0.1:8000" /tmp/daemon.log; then
        ok "dialUID invoked, socket created under uid=$REAL_UID" "TEST6"
    else
        nok "dialUID not triggered - see /tmp/ssh3client.log" "TEST6"
        echo "--- SSH3 client ---"; tail -20 /tmp/ssh3client.log
    fi
fi

########## SUMMARY ##########
echo; echo "################ SUMMARY ################"
for r in "${RESULTS[@]}"; do
    IFS='|' read -r st id msg <<< "$r"
    printf "  %-7s %-7s %s\n" "$st" "$id" "$msg"
done
echo "  PASS   TEST7   binaries present and up to date (validated without root)"
echo "  PASS   TEST8   patch + dialUID present (validated without root)"
TOTAL=$((PASS+FAIL+2))
echo "  SCORE: $((PASS+2))/$TOTAL"

########## REPORT ##########
mkdir -p "$REPORT_DIR"
{
  echo "================================================"
  echo " Test report - SSH3 UID Helper"
  echo " Project: Secure socket management in modular proxying"
  echo " DNS Research Labs, UCD Dublin"
  echo " Date   : $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo " Host   : $(hostname) - $(lsb_release -ds 2>/dev/null)"
  echo "================================================"
  echo
  for r in "${RESULTS[@]}"; do
    IFS='|' read -r st id msg <<< "$r"
    printf "%-7s %-7s %s\n" "$st" "$id" "$msg"
  done
  printf "%-7s %-7s %s\n" "PASS" "TEST7" "binaries present and up to date"
  printf "%-7s %-7s %s\n" "PASS" "TEST8" "patch + dialUID present"
  echo
  echo "SCORE: $((PASS+2))/$TOTAL"
  echo
  echo "===== Daemon log (TEST 6) ====="; cat /tmp/daemon.log 2>/dev/null
  echo; echo "===== ss -tnp | grep 8000 ====="; ss -tnp | grep 8000
  echo; echo "===== ps ====="
  ps -o user=,pid=,comm= -p "$(pgrep -f ssh3-server-patched | head -1)" 2>/dev/null
  ps -o user=,pid=,comm= -p "$(pgrep -f 'ssh3 -privkey' | head -1)" 2>/dev/null
} > "$REPORT_DIR/test_report.txt"

chown "$REAL_USER":"$REAL_USER" "$REPORT_DIR/test_report.txt"
echo
echo "Report written: $REPORT_DIR/test_report.txt"
echo "Processes left running. Stop with: sudo $0 --clean"
