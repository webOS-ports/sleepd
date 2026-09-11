#!/bin/sh
# check-binary.sh <sleepd-binary> [readelf] [nm]
#
# Verifies the exploit-mitigation posture of a built sleepd binary:
#   - PIE (ET_DYN with an interpreter)
#   - full RELRO (GNU_RELRO segment + BIND_NOW)
#   - stack canary references
#   - FORTIFY_SOURCE in effect (_chk variants present when fortifiable
#     libc calls are used)
#   - non-executable stack (GNU_STACK RW, not RWE)
#   - no runpath/rpath baked in
#
# Cross binaries are fine: pass the target readelf/nm as $2/$3, or plain
# host binutils, which read foreign ELF just as well.
set -u

BIN="${1:?usage: check-binary.sh <binary> [readelf] [nm]}"
READELF="${2:-readelf}"
NM="${3:-nm}"

fail=0
note() { printf '%-28s %s\n' "$1" "$2"; }
bad()  { note "$1" "FAIL: $2"; fail=1; }
ok()   { note "$1" "ok${2:+ ($2)}"; }

[ -r "$BIN" ] || { echo "cannot read $BIN" >&2; exit 2; }

HDR=$("$READELF" -hlWd "$BIN" 2>/dev/null)
DYNSYMS=$("$NM" -D "$BIN" 2>/dev/null)

# PIE
if echo "$HDR" | grep -q 'Type:[[:space:]]*DYN' && echo "$HDR" | grep -q 'INTERP'; then
    ok "PIE"
else
    bad "PIE" "not an ET_DYN executable"
fi

# RELRO
if echo "$HDR" | grep -q 'GNU_RELRO'; then
    if echo "$HDR" | grep -qE 'BIND_NOW|FLAGS.*NOW'; then
        ok "RELRO" "full"
    else
        bad "RELRO" "partial only (no BIND_NOW)"
    fi
else
    bad "RELRO" "no GNU_RELRO segment"
fi

# stack canary
if echo "$DYNSYMS" | grep -q '__stack_chk_fail'; then
    ok "stack canary"
else
    bad "stack canary" "__stack_chk_fail not referenced"
fi

# FORTIFY: only meaningful if fortifiable calls exist at all
if echo "$DYNSYMS" | grep -qE ' (memcpy|snprintf|sprintf|strcpy|strncpy|read)(@|$)' ; then
    if echo "$DYNSYMS" | grep -qE '__(memcpy|snprintf|sprintf|strcpy|strncpy|read)_chk'; then
        ok "FORTIFY_SOURCE"
    else
        bad "FORTIFY_SOURCE" "no _chk variants despite fortifiable calls"
    fi
else
    ok "FORTIFY_SOURCE" "all calls resolved to _chk or none fortifiable"
fi

# executable stack
GNUSTACK=$(echo "$HDR" | grep -A1 'GNU_STACK')
if echo "$GNUSTACK" | grep -q 'RWE'; then
    bad "non-exec stack" "GNU_STACK is RWE"
else
    ok "non-exec stack"
fi

# rpath/runpath
if echo "$HDR" | grep -qE '\(RPATH\)|\(RUNPATH\)'; then
    bad "no rpath" "RPATH/RUNPATH present"
else
    ok "no rpath"
fi

exit $fail
