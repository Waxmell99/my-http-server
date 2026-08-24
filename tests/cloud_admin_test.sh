#!/usr/bin/env bash
set -euo pipefail

admin=$1
sqlite=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/cloud-admin-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p "$test_root/storage/objects" "$test_root/storage/tmp" \
    "$test_root/storage/trash"
"$sqlite" "$test_root/source.db" \
    'CREATE TABLE files(storage_key TEXT NOT NULL, size INTEGER NOT NULL);'

backup_output=$(
    "$admin" backup --database "$test_root/source.db" \
        --output "$test_root/backup.db")
grep -q '"status":"ok"' <<<"$backup_output"
"$sqlite" "$test_root/backup.db" \
    "SELECT count(*) FROM sqlite_master WHERE name='files';" | grep -qx 1

check_output=$(
    "$admin" check --database "$test_root/source.db" \
        --storage-root "$test_root/storage")
grep -q '"status":"ok"' <<<"$check_output"

touch -d '2 days ago' "$test_root/storage/tmp/stale.upload"
dry_run_output=$(
    "$admin" cleanup --storage-root "$test_root/storage" \
        --older-than 3600)
grep -q '"mode":"dry-run"' <<<"$dry_run_output"
test -e "$test_root/storage/tmp/stale.upload"

apply_output=$(
    "$admin" cleanup --storage-root "$test_root/storage" \
        --older-than 3600 --apply)
grep -q '"removed":1' <<<"$apply_output"
test ! -e "$test_root/storage/tmp/stale.upload"

key=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
"$sqlite" "$test_root/source.db" \
    "INSERT INTO files(storage_key,size) VALUES('$key',3);"
printf xy >"$test_root/storage/objects/$key"
set +e
inconsistent_output=$(
    "$admin" check --database "$test_root/source.db" \
        --storage-root "$test_root/storage")
inconsistent_status=$?
set -e
test "$inconsistent_status" -eq 2
grep -q '"size_mismatches":1' <<<"$inconsistent_output"
