#!/bin/bash
# shellcheck disable=SC2002

set -x

# shellcheck source=test/others/env.sh
source ../env.sh

images_list=()

function gen_imgs {
	PID=$(../loop)
	if ! $CRIU dump -v4 -o dump.log -D ./ -t "$PID"; then
		echo "Failed to checkpoint process $PID"
		cat dump.log
		kill -9 "$PID"
		exit 1
	fi

	images_list=(./*.img)
	if [ "${#images_list[@]}" -eq 0 ]; then
		echo "Failed to generate images"
		exit 1
	fi
}

function run_test1 {
	for x in "${images_list[@]}"
	do
		echo "=== $x"
		if [[ $x == *pages* ]]; then
			echo "skip"
			continue
		fi

		echo "  -- to json"
		$CRIT decode -o "$x"".json" --pretty < "$x" || exit $?
		echo "  -- to img"
		$CRIT encode -i "$x"".json" > "$x"".json.img" || exit $?
		echo "  -- cmp"
		cmp "$x" "$x"".json.img" || exit $?

		echo "=== done"
	done
}


function run_test2 {
	PROTO_IN="${images_list[0]}"
	JSON_IN=$(mktemp -p ./ tmp.XXXXXXXXXX.json)
	OUT=$(mktemp -p ./ tmp.XXXXXXXXXX.log)

	# prepare
	${CRIT} decode -i "${PROTO_IN}" -o "${JSON_IN}"

	# show info about image
	${CRIT} info "${PROTO_IN}"

	# proto in - json out decode
	cat "${PROTO_IN}" | ${CRIT} decode || exit 1
	cat "${PROTO_IN}" | ${CRIT} decode -o "${OUT}" || exit 1
	cat "${PROTO_IN}" | ${CRIT} decode > "${OUT}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" -o "${OUT}" || exit 1
	${CRIT} decode -i "${PROTO_IN}" > "${OUT}" || exit 1
	${CRIT} decode < "${PROTO_IN}" || exit 1
	${CRIT} decode -o "${OUT}" < "${PROTO_IN}" || exit 1
	${CRIT} decode < "${PROTO_IN}" > "${OUT}" || exit 1

	# proto in - json out encode -> should fail
	cat "${PROTO_IN}" | ${CRIT} encode || true
	cat "${PROTO_IN}" | ${CRIT} encode -o "${OUT}" || true
	cat "${PROTO_IN}" | ${CRIT} encode > "${OUT}" || true
	${CRIT} encode -i "${PROTO_IN}" || true
	${CRIT} encode -i "${PROTO_IN}" -o "${OUT}" || true
	${CRIT} encode -i "${PROTO_IN}" > "${OUT}" || true

	# json in - proto out encode
	cat "${JSON_IN}" | ${CRIT} encode || exit 1
	cat "${JSON_IN}" | ${CRIT} encode -o "${OUT}" || exit 1
	cat "${JSON_IN}" | ${CRIT} encode > "${OUT}" || exit 1
	${CRIT} encode -i "${JSON_IN}" || exit 1
	${CRIT} encode -i "${JSON_IN}" -o "${OUT}" || exit 1
	${CRIT} encode -i "${JSON_IN}" > "${OUT}" || exit 1
	${CRIT} encode < "${JSON_IN}" || exit 1
	${CRIT} encode -o "${OUT}" < "${JSON_IN}" || exit 1
	${CRIT} encode < "${JSON_IN}" > "${OUT}" || exit 1

	# json in - proto out decode -> should fail
	cat "${JSON_IN}" | ${CRIT} decode || true
	cat "${JSON_IN}" | ${CRIT} decode -o "${OUT}" || true
	cat "${JSON_IN}" | ${CRIT} decode > "${OUT}" || true
	${CRIT} decode -i "${JSON_IN}" || true
	${CRIT} decode -i "${JSON_IN}" -o "${OUT}" || true
	${CRIT} decode -i "${JSON_IN}" > "${OUT}" || true

	# explore image directory
	${CRIT} x ./ ps || exit 1
	${CRIT} x ./ fds || exit 1
	${CRIT} x ./ mems || exit 1
	${CRIT} x ./ rss || exit 1
}

ZDTM_DIR="${BASE_DIR}/test/zdtm/static"

function dump_test_process {
	# Dump compress_pages02 (covers 8 mapping types: anonymous,
	# zero-filled, shared anonymous, file-backed private/shared,
	# memfd, read-only, PROT_NONE guard) into the given directory.
	# Prints the PID on success.
	local dir=$1; shift
	local pid

	if ! make -C "$ZDTM_DIR" compress_pages02 > /dev/null 2>&1; then
		echo "FAIL: failed to build compress_pages02"
		exit 1
	fi

	# Clean stale marker files and run from the test directory
	# so zdtm pidfile rename works (same filesystem).
	rm -f "$dir"/test.out* "$dir"/test.pid "$dir"/test.file

	if ! (cd "$dir" && "$ZDTM_DIR/compress_pages02" \
		--pidfile=test.pid \
		--outfile=test.out \
		--filename=test.file); then
		echo "FAIL: failed to start compress_pages02"
		cat "$dir/test.out" 2>/dev/null
		exit 1
	fi

	# Wait for pidfile
	local i=0
	while [ $i -lt 50 ]; do
		if [ -f "$dir/test.pid" ]; then
			break
		fi
		sleep 0.1
		i=$((i + 1))
	done

	pid=$(cat "$dir/test.pid" 2>/dev/null)
	if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
		echo "FAIL: test process didn't start"
		cat "$dir/test.out" 2>/dev/null
		exit 1
	fi

	if ! $CRIU dump -v4 -o dump.log -D "$dir" -t "$pid" --shell-job "$@"; then
		echo "FAIL: dump into $dir"
		cat "$dir/dump.log"
		kill -9 "$pid" 2>/dev/null
		exit 1
	fi
	echo "$pid"
}

function restore_and_verify {
	# Restore from directory, send SIGTERM so compress_pages02
	# verifies its own memory, then check the test output for PASS.
	local dir=$1
	local pid

	if ! $CRIU restore -v4 -o restore.log -D "$dir" --shell-job -d; then
		echo "FAIL: restore from $dir"
		cat "$dir/restore.log"
		exit 1
	fi

	# Get the root PID from the pstree image
	pid=$($CRIT decode -i "$dir/pstree.img" 2>/dev/null |
		grep -o '"pid": [0-9]*' | head -1 | grep -o '[0-9]*')

	if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
		echo "FAIL: restored process not running from $dir (pid=$pid)"
		exit 1
	fi

	# Send SIGTERM so compress_pages02 verifies all memory
	# regions (zero, pattern, shared, file, memfd, readonly,
	# guard) then writes PASS/FAIL to its output file.
	kill -TERM "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	sleep 0.5

	if ! grep -q PASS "$dir/test.out" 2>/dev/null; then
		echo "FAIL: memory verification failed after restore from $dir"
		cat "$dir/test.out" 2>/dev/null
		exit 1
	fi
}

function pages_checksum {
	# Print md5sum of all pages-*.img files in a directory.
	# This captures the raw page content for comparison.
	cat "$1"/pages-*.img | md5sum | awk '{print $1}'
}

function assert_inventory_version {
	local dir=$1
	local expected=$2

	PYTHONPATH="${BASE_DIR}/lib" python3 - "$dir" "$expected" <<'PY'
import os
import sys

import pycriu.images

directory = sys.argv[1]
expected = int(sys.argv[2])

with open(os.path.join(directory, "inventory.img"), "rb") as f:
	inv = pycriu.images.load(f)

actual = inv["entries"][0]["img_version"]
if actual != expected:
	print("FAIL: inventory version is %d, expected %d" %
	      (actual, expected))
	sys.exit(1)
PY
}

function make_sparse_pagemap_image {
	local dir=$1
	local compressed=$2

	rm -rf "$dir"
	mkdir -p "$dir"

	PYTHONPATH="${BASE_DIR}/lib" python3 - "$dir" "$compressed" <<'PY'
import os
import sys

import pycriu.images

PAGE_SIZE = 4096
PE_PARENT = 1 << 0
PE_PRESENT = 1 << 2

directory = sys.argv[1]
compressed = int(sys.argv[2])

with open(os.path.join(directory, "inventory.img"), "wb") as f:
    inv = {"magic": "INVENTORY", "entries": [{"img_version": 2}]}
    if compressed:
        inv["entries"][0]["img_version"] = 3
        inv["entries"][0]["compress"] = 1
    pycriu.images.dump(inv, f)

if compressed:
    # One raw present page, one non-present hole, one legacy parent entry
    # without flags, and one zero present page with no payload.
    pages = bytes([0x41]) * PAGE_SIZE
    entries = [
        {"pages_id": 1},
        {
            "vaddr": 0x100000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": PE_PRESENT,
            "compressed_size": [PAGE_SIZE],
            "total_compressed_size": PAGE_SIZE,
        },
        {
            "vaddr": 0x101000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": 0,
        },
        {
            "vaddr": 0x102000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "in_parent": True,
        },
        {
            "vaddr": 0x103000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": PE_PRESENT,
            "compressed_size": [0],
            "total_compressed_size": 0,
        },
    ]
else:
    # Two present pages are adjacent in pages.img, while the pagemap has a
    # non-present hole and a legacy parent reference between them.
    pages = bytes([0x41]) * PAGE_SIZE + bytes([0x42]) * PAGE_SIZE
    entries = [
        {"pages_id": 1},
        {
            "vaddr": 0x100000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": PE_PRESENT,
        },
        {
            "vaddr": 0x101000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": 0,
        },
        {
            "vaddr": 0x102000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "in_parent": True,
        },
        {
            "vaddr": 0x103000,
            "compat_nr_pages": 1,
            "nr_pages": 1,
            "flags": PE_PRESENT,
        },
    ]

with open(os.path.join(directory, "pages-1.img"), "wb") as f:
    f.write(pages)

with open(os.path.join(directory, "pagemap-1.img"), "wb") as f:
    pycriu.images.dump({"magic": "PAGEMAP", "entries": entries}, f)
PY
}

function make_threshold_pagemap_image {
	local dir=$1

	rm -rf "$dir"
	mkdir -p "$dir"

	PYTHONPATH="${BASE_DIR}/lib" python3 - "$dir" <<'PY'
import os
import random
import sys

import lz4.block
import pycriu.images

PAGE_SIZE = 4096
PAGE_COMPRESSION_THRESHOLD = PAGE_SIZE * 7 // 8
PE_PRESENT = 1 << 2

directory = sys.argv[1]
r = random.Random(0)
page = bytearray(r.randrange(256) for _ in range(PAGE_SIZE))

off = 32 * 17 % (PAGE_SIZE - 32)
page[off:off + 32] = b'\0' * 32

cs = len(lz4.block.compress(bytes(page), store_size=False,
                            acceleration=1))
if cs < PAGE_COMPRESSION_THRESHOLD or cs >= PAGE_SIZE:
    print("FAIL: threshold test page compressed to %d bytes" % cs)
    sys.exit(1)

with open(os.path.join(directory, "inventory.img"), "wb") as f:
    pycriu.images.dump({
        "magic": "INVENTORY",
        "entries": [{"img_version": 2}],
    }, f)

with open(os.path.join(directory, "pages-1.img"), "wb") as f:
    f.write(page)

with open(os.path.join(directory, "pagemap-1.img"), "wb") as f:
    pycriu.images.dump({
        "magic": "PAGEMAP",
        "entries": [
            {"pages_id": 1},
            {
                "vaddr": 0x100000,
                "compat_nr_pages": 1,
                "nr_pages": 1,
                "flags": PE_PRESENT,
            },
        ],
    }, f)
PY
}

function make_truncated_uncompressed_entry_image {
	local dir=$1

	rm -rf "$dir"
	mkdir -p "$dir"

	PYTHONPATH="${BASE_DIR}/lib" python3 - "$dir" <<'PY'
import os
import sys

import pycriu.images

PAGE_SIZE = 4096
PE_PRESENT = 1 << 2

directory = sys.argv[1]

with open(os.path.join(directory, "inventory.img"), "wb") as f:
    pycriu.images.dump({
        "magic": "INVENTORY",
        "entries": [{
            "img_version": 3,
            "compress": 1,
        }],
    }, f)

with open(os.path.join(directory, "pages-1.img"), "wb") as f:
    f.write(bytes([0x41]) * PAGE_SIZE)

with open(os.path.join(directory, "pagemap-1.img"), "wb") as f:
    pycriu.images.dump({
        "magic": "PAGEMAP",
        "entries": [
            {"pages_id": 1},
            {
                "vaddr": 0x100000,
                "compat_nr_pages": 2,
                "nr_pages": 2,
                "flags": PE_PRESENT,
            },
        ],
    }, f)
PY
}

function assert_sparse_pagemap_payload {
	local dir=$1
	local expected_size=$2
	local expected_md5=$3
	local actual_size
	local actual_md5

	actual_size=$(stat -c %s "$dir/pages-1.img")
	if [ "$actual_size" != "$expected_size" ]; then
		echo "FAIL: $dir/pages-1.img size is $actual_size, expected $expected_size"
		exit 1
	fi

	actual_md5=$(md5sum "$dir/pages-1.img" | awk '{print $1}')
	if [ "$actual_md5" != "$expected_md5" ]; then
		echo "FAIL: $dir/pages-1.img checksum is $actual_md5, expected $expected_md5"
		exit 1
	fi
}

function assert_pagemap_compressed_size {
	local dir=$1
	local expected=$2

	PYTHONPATH="${BASE_DIR}/lib" python3 - "$dir" "$expected" <<'PY'
import os
import sys

import pycriu.images

directory = sys.argv[1]
expected = int(sys.argv[2])

with open(os.path.join(directory, "pagemap-1.img"), "rb") as f:
	pm = pycriu.images.load(f)

actual = pm["entries"][1]["compressed_size"][0]
if actual != expected:
	print("FAIL: compressed_size is %d, expected %d" %
	      (actual, expected))
	sys.exit(1)
PY
}

function run_test_compress {
	echo "=== compress/decompress tests ==="

	# -------------------------------------------------------
	# Test 1: dump -c -> decompress -> restore
	# -------------------------------------------------------
	echo "  -- Test 1: compressed dump -> decompress -> restore"
	dump_test_process comp/ -c > /dev/null

	$CRIT decompress comp/ || exit 1
	assert_inventory_version comp/ 2

	# Verify backup files exist
	ls comp/pages-*.img.bak > /dev/null 2>&1 || { echo "FAIL: no pages backup"; exit 1; }
	ls comp/pagemap-*.img.bak > /dev/null 2>&1 || { echo "FAIL: no pagemap backup"; exit 1; }
	ls comp/inventory.img.bak > /dev/null 2>&1 || { echo "FAIL: no inventory backup"; exit 1; }

	restore_and_verify comp/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 2: dump uncompressed -> compress -> restore
	# -------------------------------------------------------
	echo "  -- Test 2: uncompressed dump -> compress -> restore"
	dump_test_process uncomp/ > /dev/null

	$CRIT compress uncomp/ --in-place || exit 1
	assert_inventory_version uncomp/ 3

	# Verify no backup files with --in-place
	if ls uncomp/*.bak > /dev/null 2>&1; then
		echo "FAIL: backup files created with --in-place"
		exit 1
	fi

	restore_and_verify uncomp/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 3: compress already compressed, decompress already decompressed
	# -------------------------------------------------------
	echo "  -- Test 3: compress already compressed, decompress already decompressed"
	$CRIT compress uncomp/ --in-place 2>&1 | grep -q "already compressed" || {
		echo "FAIL: compress should report already compressed"
		exit 1
	}
	$CRIT decompress comp/ 2>&1 | grep -q "already decompressed" || {
		echo "FAIL: decompress should report already decompressed"
		exit 1
	}
	echo "     PASS"

	# -------------------------------------------------------
	# Test 4: compress -> decompress -> compress produces same pages
	# -------------------------------------------------------
	echo "  -- Test 4: compress -> decompress -> compress stability"
	rm -rf cdc/
	mkdir -p cdc/
	dump_test_process cdc/ > /dev/null

	$CRIT compress cdc/ --in-place || exit 1
	local sum1
	sum1=$(pages_checksum cdc/)

	$CRIT decompress cdc/ --in-place || exit 1
	assert_inventory_version cdc/ 2
	$CRIT compress cdc/ --in-place || exit 1
	assert_inventory_version cdc/ 3
	local sum2
	sum2=$(pages_checksum cdc/)

	if [ "$sum1" != "$sum2" ]; then
		echo "FAIL: compress->decompress->compress changed pages data"
		echo "  first:  $sum1"
		echo "  second: $sum2"
		exit 1
	fi

	restore_and_verify cdc/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 5: decompress -> compress -> decompress produces same pages
	# -------------------------------------------------------
	echo "  -- Test 5: decompress -> compress -> decompress stability"
	rm -rf dcd/
	mkdir -p dcd/
	dump_test_process dcd/ -c > /dev/null

	$CRIT decompress dcd/ --in-place || exit 1
	assert_inventory_version dcd/ 2
	local sum3
	sum3=$(pages_checksum dcd/)

	$CRIT compress dcd/ --in-place || exit 1
	assert_inventory_version dcd/ 3
	$CRIT decompress dcd/ --in-place || exit 1
	assert_inventory_version dcd/ 2
	local sum4
	sum4=$(pages_checksum dcd/)

	if [ "$sum3" != "$sum4" ]; then
		echo "FAIL: decompress->compress->decompress changed pages data"
		echo "  first:  $sum3"
		echo "  second: $sum4"
		exit 1
	fi

	restore_and_verify dcd/
	echo "     PASS"

	# -------------------------------------------------------
	# Test 6: CRIT must not consume pages payload for sparse
	# pagemap entries without PE_PRESENT, including legacy
	# in_parent entries with no flags field.
	# -------------------------------------------------------
	echo "  -- Test 6: sparse pagemap entries have no pages payload"
	local raw_md5
	local zero_md5
	raw_md5=$(PYTHONPATH="${BASE_DIR}/lib" python3 - <<'PY'
import hashlib
print(hashlib.md5(bytes([0x41]) * 4096 + bytes(4096)).hexdigest())
PY
)
	zero_md5=$(PYTHONPATH="${BASE_DIR}/lib" python3 - <<'PY'
import hashlib
print(hashlib.md5(bytes([0x41]) * 4096 + bytes([0x42]) * 4096).hexdigest())
PY
)

	make_sparse_pagemap_image sparse-compress/ 0
	$CRIT compress sparse-compress/ --in-place || exit 1
	assert_inventory_version sparse-compress/ 3
	$CRIT decompress sparse-compress/ --in-place || exit 1
	assert_inventory_version sparse-compress/ 2
	assert_sparse_pagemap_payload sparse-compress/ 8192 "$zero_md5"

	make_sparse_pagemap_image sparse-decompress/ 1
	$CRIT decompress sparse-decompress/ --in-place || exit 1
	assert_inventory_version sparse-decompress/ 2
	assert_sparse_pagemap_payload sparse-decompress/ 8192 "$raw_md5"
	echo "     PASS"

	# -------------------------------------------------------
	# Test 7: CRIT uses the same 7/8 raw fallback threshold
	# as CRIU for marginally compressible pages.
	# -------------------------------------------------------
	echo "  -- Test 7: marginally compressible pages are stored raw"
	make_threshold_pagemap_image threshold-compress/
	local threshold_md5
	threshold_md5=$(md5sum threshold-compress/pages-1.img | awk '{print $1}')

	$CRIT compress threshold-compress/ --in-place || exit 1
	assert_inventory_version threshold-compress/ 3
	assert_pagemap_compressed_size threshold-compress/ 4096
	assert_sparse_pagemap_payload threshold-compress/ 4096 "$threshold_md5"
	echo "     PASS"

	# -------------------------------------------------------
	# Test 8: CRIT must fail explicitly on a truncated
	# uncompressed payload copied through by decompress.
	# -------------------------------------------------------
	echo "  -- Test 8: truncated uncompressed payload is rejected"
	make_truncated_uncompressed_entry_image truncated-decompress/
	if $CRIT decompress truncated-decompress/ --in-place \
			> truncated-decompress.log 2>&1; then
		echo "FAIL: decompress accepted truncated pages payload"
		cat truncated-decompress.log
		exit 1
	fi
	grep -q "short read" truncated-decompress.log || {
		echo "FAIL: decompress did not report a short read"
		cat truncated-decompress.log
		exit 1
	}
	echo "     PASS"

	echo "=== compress/decompress: ALL PASS ==="
}

${CRIT} --version

gen_imgs
run_test1
run_test2

# Skip compress/decompress tests if lz4 or CRIU compression is unavailable
if python3 -c "import lz4.block" 2>/dev/null && $CRIU check --feature compress 2>/dev/null; then
	mkdir -p comp/ uncomp/
	run_test_compress
else
	echo "=== Skipping compress/decompress tests (lz4 or CRIU compression not available) ==="
fi
