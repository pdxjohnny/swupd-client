#!/usr/bin/env bats

load "../../swupdlib"

setup() {
  clean_test_dir
  create_manifest_tar 10 MoM
  create_manifest_tar 10 test-bundle
  create_manifest_tar 10 os-core
}

teardown() {
  clean_tars 10
}

@test "search with non existant library" {
  run sudo sh -c "$SWUPD search $SWUPD_OPTS -l libtest-nohit"

  echo "$output"
  [ "${lines[0]}" = "Attempting to download version string to memory" ]
  [ "${lines[1]}" = "Searching for 'libtest-nohit'" ]
  echo "$output" | grep -q 'Search term not found.'
  [ $? -eq 0 ]
}

# vi: ft=sh ts=8 sw=2 sts=2 et tw=80
