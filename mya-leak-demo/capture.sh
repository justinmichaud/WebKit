#!/bin/bash
# Capture one demo: JSC's own heap dump, then mya's, of the same WebContent process.
#
#   capture.sh <demo-dir> <seconds>
#
# <demo-dir>/page is served and opened in the Debug MiniBrowser, which runs the page for
# <seconds>. JSC's dump (com.apple.WebKit.dumpGCHeap) collects first; mya's snapshot is taken
# right after it, so both see a heap the collector has just collected, and mya reads JSC's dump
# for which references in C++ storage the collector marks through. The target runs with
# the options design.md's A8 gives a measured target. Everything lands in <demo-dir>/capture.
set -euo pipefail

demo=$(cd "$1" && pwd)
seconds=$2
webkit=$(cd "$(dirname "$0")/.." && pwd)
out="$demo/capture"
port=8100
rm -rf "$out"
mkdir -p "$out"
log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$out/capture.log"; }

# A MiniBrowser left from another run reloads its own page in a new WebContent process.
stop_browser() {
    pkill -f "MiniBrowser.app/Contents/MacOS/MiniBrowser" 2>/dev/null || true
    pkill -f "WebContent.Development" 2>/dev/null || true
    for _ in $(seq 1 20); do
        pgrep -f "MiniBrowser.app/Contents/MacOS/MiniBrowser|WebContent.Development" > /dev/null || return 0
        sleep 0.5
    done
    log "MiniBrowser did not exit"
    exit 1
}
stop_browser
python3 -u -m http.server "$port" --bind 127.0.0.1 --directory "$demo/page" > "$out/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null || true; stop_browser' EXIT
sleep 1
# A URL of its own, so that no cache answers for another demo's page.
page="index.html?capture=$(basename "$demo")-$(date +%s)"

log "launching MiniBrowser on http://127.0.0.1:$port/$page"
(cd "$webkit" && env JSC_useConcurrentGC=0 __XPC_JSC_useConcurrentGC=0 \
    JSC_useConcurrentJIT=0 __XPC_JSC_useConcurrentJIT=0 \
    JSC_useWarmUpMarkedBlocks=0 __XPC_JSC_useWarmUpMarkedBlocks=0 \
    Tools/Scripts/run-minibrowser --debug "http://127.0.0.1:$port/$page" > "$out/minibrowser.log" 2>&1) &

pid=""
for _ in $(seq 1 60); do
    pid=$(pgrep -f "WebContent.Development" | head -1 || true)
    [ -n "$pid" ] && grep -qF "GET /$page" "$out/server.log" 2>/dev/null && break
    sleep 1
done
[ -n "$pid" ] || { log "no WebContent process"; exit 1; }
grep -qF "GET /$page" "$out/server.log" || { log "the server never served /$page"; exit 1; }
log "WebContent pid $pid; page loaded; running for ${seconds}s"
sleep "$seconds"

mark="$out/.mark"
touch "$mark"
log "JSC dump: notifyutil -p com.apple.WebKit.dumpGCHeap"
notifyutil -p com.apple.WebKit.dumpGCHeap
dump=""
for _ in $(seq 1 30); do
    dump=$(find /var/folders -name 'GCHeap*' -newer "$mark" 2>/dev/null | head -1 || true)
    [ -n "$dump" ] && break
    sleep 1
done
[ -n "$dump" ] || { log "JSC wrote no dump"; exit 1; }
sleep 2 # Until the file is complete.
cp "$dump" "$out/jsc.json"
log "JSC dump: $(wc -c < "$out/jsc.json") bytes from $dump"

log "mya: snapshot --pid $pid; heapdump --collector jsc.json"
printf 'snapshot --pid %s\nheapdump %s --collector %s\nquit\n' "$pid" "$out/mya.json" "$out/jsc.json" \
    | (cd "$webkit" && DYLD_FRAMEWORK_PATH=WebKitBuild/Debug WebKitBuild/Debug/mya) > "$out/mya.log" 2>&1
log "mya: $(grep -E '^Wrote' "$out/mya.log" || echo 'no dump written')"
log "mya: $(grep -E 'strong edges between cells come from' "$out/mya.log" | sed 's/^ *//' || echo 'no collector edges')"
