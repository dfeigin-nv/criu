#!/usr/bin/env bash
#
# Tier-2 CRIU restore integration for the node-local memfd content cache.
# Drives REAL criu restore through the REAL Go cache Server (wrapped by
# memfdcache-testserve), exercising the criu-side hooks in criu/memfd.c:
# memfd_content_prepare (HIT/MISS) and memfd_content_donate, plus the eager
# primer (--memfd-cache-prime).
#
# REQUIRES root or CAP_CHECKPOINT_RESTORE; no GPU. Build now, run on a CRIU box.
#
# Steps:
#   1. Cache-off no-op   -- zdtm memfd/shmemfd tests pass unchanged (gating regression).
#   2. Lazy donate->HIT  -- restore a sealed-memfd image twice against one server:
#                           run 1 MISS+donate, run 2 HIT ("Borrowed cached memfd:").
#   3. Primer->HIT       -- --memfd-cache-prime donates, then a real restore HITs.
#   4. Userns donate->HIT-- same cycle inside a user namespace: the HIT fd is
#                           pre-owned, so cr_fchpermat tolerates the chown EPERM.
#
# Env:
#   CRIU                  criu binary (default: <repo>/criu/criu)
#   MEMFDCACHE_TESTSERVE  prebuilt testserve binary (default: go build from DYNAMO_SRC)
#   DYNAMO_SRC            dynamo checkout (default: sibling of this repo)
#   KEEP_WORK=1           keep the scratch dir for debugging
#
set -uo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # <repo>/test/memfdcache
CRIU_SRC="$(cd "$SELF_DIR/../.." && pwd)"                   # <repo>
CRIU="${CRIU:-$CRIU_SRC/criu/criu}"
ZDTM="$CRIU_SRC/test/zdtm.py"
DYNAMO_SRC="${DYNAMO_SRC:-$(cd "$CRIU_SRC/.." && pwd)/dynamo}"
CC="${CC:-cc}"

pass=0
fail=0
note()  { printf '\n=== %s ===\n' "$*"; }
ok()    { printf '  PASS: %s\n' "$*"; pass=$((pass + 1)); }
bad()   { printf '  FAIL: %s\n' "$*"; fail=$((fail + 1)); }

# ---- preflight ------------------------------------------------------------
note "preflight"
if [ "$(id -u)" -ne 0 ] && ! capsh --print 2>/dev/null | grep -q cap_checkpoint_restore; then
	echo "  warning: not root and no CAP_CHECKPOINT_RESTORE visible; criu steps will likely fail" >&2
fi
[ -x "$CRIU" ] || { echo "error: criu not found/built at $CRIU (set CRIU=...)" >&2; exit 1; }
echo "  criu: $CRIU ($("$CRIU" --version 2>/dev/null | head -n1))"

TESTSERVE="${MEMFDCACHE_TESTSERVE:-}"
if [ -z "$TESTSERVE" ]; then
	echo "  building memfdcache-testserve from $DYNAMO_SRC"
	[ -d "$DYNAMO_SRC/deploy/snapshot" ] || { echo "error: dynamo snapshot tree not at $DYNAMO_SRC (set DYNAMO_SRC=...)" >&2; exit 1; }
	TESTSERVE="$(mktemp -d)/memfdcache-testserve"
	( cd "$DYNAMO_SRC/deploy/snapshot" && go build -o "$TESTSERVE" ./cmd/memfdcache-testserve/ ) \
		|| { echo "error: failed to build testserve" >&2; exit 1; }
fi
echo "  testserve: $TESTSERVE"

WORK="$(mktemp -d)"
cleanup() {
	# Reap any restored fixtures that detached during the run.
	pkill -f "$WORK/.*fixture" 2>/dev/null || true
	[ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
}
trap cleanup EXIT
echo "  work dir: $WORK"

FIXTURE="$WORK/fixture"
"$CC" -Wall -Wextra -Werror "$SELF_DIR/fixture.c" -o "$FIXTURE" \
	|| { echo "error: fixture build failed" >&2; exit 1; }

# dump_fixture <img-dir> [unshare-prefix...]
# Runs the sealed-memfd fixture, dumps it to <img-dir>, returns once the image exists.
dump_fixture() {
	local img="$1"; shift
	local pidfile="$WORK/fixture.pid"
	mkdir -p "$img"
	rm -f "$pidfile"
	# Optional namespace prefix (used by the userns step).
	"$@" "$FIXTURE" "$pidfile" 2>/dev/null
	# Wait for the daemonized child to publish its PID.
	local tries=0 pid=""
	while [ -z "$pid" ] && [ "$tries" -lt 50 ]; do
		pid="$(cat "$pidfile" 2>/dev/null || true)"
		tries=$((tries + 1)); sleep 0.1
	done
	[ -n "$pid" ] || { echo "error: fixture never reported a pid" >&2; return 1; }
	"$CRIU" dump -t "$pid" -D "$img" -v4 -o dump.log
}

# Marker the HIT path logs (criu/memfd.c:400, needs -v4).
HIT_MARK="Borrowed cached memfd:"

# ---- step 1: cache-off regression ----------------------------------------
note "step 1: cache-off no-op (zdtm memfd/shmemfd)"
if [ -f "$ZDTM" ]; then
	z_ok=1
	for t in memfd00 memfd01 memfd02 memfd03 memfd04 memfd05 memfd06 shmemfd shmemfd-priv; do
		if python3 "$ZDTM" run -t "zdtm/static/$t" >"$WORK/zdtm-$t.log" 2>&1; then
			printf '    %-12s pass\n' "$t"
		else
			printf '    %-12s FAIL (see %s)\n' "$t" "$WORK/zdtm-$t.log"
			z_ok=0
		fi
	done
	[ "$z_ok" = 1 ] && ok "cache-off zdtm memfd/shmemfd unchanged" || bad "cache-off zdtm regression"
else
	bad "zdtm.py not found at $ZDTM (skipped)"
fi

# ---- step 2: lazy donate -> HIT (two real restores, fresh pidns each) -----
note "step 2: lazy donate -> HIT"
IMG2="$WORK/img-lazy"
if dump_fixture "$IMG2"; then
	# One server, two sessions: run 1 fills+donates, run 2 borrows. Each restore
	# runs in its own pid namespace so the restored PID does not collide.
	"$TESTSERVE" -- \
		unshare -pf --mount-proc -- "$CRIU" restore -D "$IMG2" --restore-detached \
			--memfd-cache --memfd-cache-id ckpt:1 -v4 -o r1.log -- \
		unshare -pf --mount-proc -- "$CRIU" restore -D "$IMG2" --restore-detached \
			--memfd-cache --memfd-cache-id ckpt:1 -v4 -o r2.log \
		>"$WORK/testserve-lazy.out" 2>"$WORK/testserve-lazy.err"
	cat "$WORK/testserve-lazy.out"
	if grep -q "$HIT_MARK" "$IMG2/r2.log" 2>/dev/null; then
		ok "run 2 borrowed a cached memfd ('$HIT_MARK' in r2.log)"
	else
		bad "run 2 did not HIT (no '$HIT_MARK' in $IMG2/r2.log)"
	fi
	if grep -q "$HIT_MARK" "$IMG2/r1.log" 2>/dev/null; then
		bad "run 1 unexpectedly HIT before any donation"
	else
		ok "run 1 was a clean MISS+donate"
	fi
	grep -q "STATS entries=[1-9]" "$WORK/testserve-lazy.out" && ok "server cached >=1 inode" || bad "server cached nothing"
else
	bad "step 2 dump failed"
fi

# ---- step 3: primer -> HIT ------------------------------------------------
note "step 3: primer -> HIT"
IMG3="$WORK/img-prime"
if dump_fixture "$IMG3"; then
	"$TESTSERVE" -- \
		"$CRIU" restore --memfd-cache-prime --memfd-cache --memfd-cache-id prime:1 \
			-D "$IMG3" -v4 -o prime.log -- \
		unshare -pf --mount-proc -- "$CRIU" restore -D "$IMG3" --restore-detached \
			--memfd-cache --memfd-cache-id prime:1 -v4 -o r3.log \
		>"$WORK/testserve-prime.out" 2>"$WORK/testserve-prime.err"
	cat "$WORK/testserve-prime.out"
	grep -qE "memfd-cache prime: [1-9][0-9]* donated" "$IMG3/prime.log" 2>/dev/null \
		&& ok "primer donated >=1 inode" || bad "primer donated nothing (see $IMG3/prime.log)"
	grep -q "$HIT_MARK" "$IMG3/r3.log" 2>/dev/null \
		&& ok "post-prime restore HIT" || bad "post-prime restore did not HIT"
else
	bad "step 3 dump failed"
fi

# ---- step 4: userns donate -> HIT -----------------------------------------
# A cached memfd carries the donor's resolved owner. On a userns restore the
# first opener's cr_fchpermat tolerates the chown EPERM only when the fd is
# already correctly owned -- which a HIT guarantees (owner is in the key). This
# step proves the donate->HIT cycle survives a userns restore without EPERM.
note "step 4: userns donate -> HIT (best-effort; needs userns-capable criu)"
IMG4="$WORK/img-userns"
# Dump the fixture from inside a user namespace mapped to root.
if dump_fixture "$IMG4" unshare -Urf --mount-proc --; then
	"$TESTSERVE" -- \
		unshare -Urpf --mount-proc -- "$CRIU" restore -D "$IMG4" --restore-detached \
			--memfd-cache --memfd-cache-id userns:1 -v4 -o ur1.log -- \
		unshare -Urpf --mount-proc -- "$CRIU" restore -D "$IMG4" --restore-detached \
			--memfd-cache --memfd-cache-id userns:1 -v4 -o ur2.log \
		>"$WORK/testserve-userns.out" 2>"$WORK/testserve-userns.err"
	cat "$WORK/testserve-userns.out"
	if grep -q "$HIT_MARK" "$IMG4/ur2.log" 2>/dev/null; then
		ok "userns run 2 HIT on a pre-owned fd"
	else
		bad "userns run 2 did not HIT (see $IMG4/ur2.log)"
	fi
	if grep -qiE "fchown.*EPERM|chmod.*EPERM|Operation not permitted" "$IMG4/ur2.log" 2>/dev/null; then
		bad "userns HIT hit an EPERM in the perms step (owner-in-key broken?)"
	else
		ok "no EPERM in the userns HIT perms step"
	fi
else
	echo "  note: userns dump failed; skipping (host may disallow nested userns)" >&2
fi

# ---- summary --------------------------------------------------------------
note "summary: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
