#!/usr/bin/env bash
set -euo pipefail

admin=$1
sqlite=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/cloud-admin-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

mkdir -p "$test_root/storage/objects" "$test_root/storage/tmp" \
    "$test_root/storage/trash"
"$sqlite" "$test_root/source.db" \
    'CREATE TABLE files(storage_key TEXT NOT NULL, size INTEGER NOT NULL, sha256 BLOB NOT NULL);'

key=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
printf abc >"$test_root/storage/objects/$key"
"$sqlite" "$test_root/source.db" \
    "INSERT INTO files(storage_key,size,sha256) VALUES('$key',3,X'$sha256');"

check_output=$(
    "$admin" check --database "$test_root/source.db" \
        --storage-root "$test_root/storage")
grep -q '"status":"ok"' <<<"$check_output"

backup_output=$(
    "$admin" backup --database "$test_root/source.db" \
        --storage-root "$test_root/storage" \
        --output "$test_root/backup.pcbackup")
grep -q '"status":"ok"' <<<"$backup_output"
grep -q '"object_files":1' <<<"$backup_output"

set +e
"$admin" backup --database "$test_root/source.db" \
    --storage-root "$test_root/storage" \
    --output "$test_root/backup.pcbackup" >/dev/null 2>&1
existing_backup_status=$?
set -e
test "$existing_backup_status" -eq 1
"$admin" backup --database "$test_root/source.db" \
    --storage-root "$test_root/storage" \
    --output "$test_root/backup.pcbackup" --force >/dev/null

restore_output=$(
    "$admin" restore --input "$test_root/backup.pcbackup" \
        --database "$test_root/restored.db" \
        --storage-root "$test_root/restored-storage")
grep -q '"status":"ok"' <<<"$restore_output"
"$sqlite" "$test_root/restored.db" \
    "SELECT storage_key || ':' || size || ':' || lower(hex(sha256)) FROM files;" |
    grep -qx "$key:3:$sha256"
cmp "$test_root/storage/objects/$key" \
    "$test_root/restored-storage/objects/$key"
"$admin" check --database "$test_root/restored.db" \
    --storage-root "$test_root/restored-storage" |
    grep -q '"status":"ok"'

set +e
"$admin" restore --input "$test_root/backup.pcbackup" \
    --database "$test_root/restored.db" \
    --storage-root "$test_root/restored-storage" >/dev/null 2>&1
existing_restore_status=$?
set -e
test "$existing_restore_status" -eq 1

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

printf xy >"$test_root/storage/objects/$key"
set +e
inconsistent_output=$(
    "$admin" check --database "$test_root/source.db" \
        --storage-root "$test_root/storage")
inconsistent_status=$?
set -e
test "$inconsistent_status" -eq 2
grep -q '"size_mismatches":1' <<<"$inconsistent_output"

set +e
"$admin" backup --database "$test_root/source.db" \
    --storage-root "$test_root/storage" \
    --output "$test_root/inconsistent.pcbackup" >/dev/null 2>&1
inconsistent_backup_status=$?
set -e
test "$inconsistent_backup_status" -eq 1
test ! -e "$test_root/inconsistent.pcbackup"

"$sqlite" "$test_root/source.db" \
    "UPDATE files SET storage_key='not-a-real-storage-key';"
set +e
invalid_key_output=$(
    "$admin" check --database "$test_root/source.db" \
        --storage-root "$test_root/storage")
invalid_key_status=$?
set -e
test "$invalid_key_status" -eq 2
grep -q '"invalid_storage_keys":1' <<<"$invalid_key_output"
