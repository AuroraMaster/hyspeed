#!/usr/bin/env bash
set -euo pipefail

args=()
for arg in "$@"; do
  case "$arg" in
    -mfunction-return=* | \
    -fconserve-stack | \
    -mrecord-mcount | \
    -ftrivial-auto-var-init=zero | \
    -mharden-sls=* | \
    -Wno-maybe-uninitialized | \
    -Wno-alloc-size-larger-than | \
    -Wimplicit-fallthrough=*)
      ;;
    *)
      args+=("$arg")
      ;;
  esac
done

exec clang \
  -Wno-unused-command-line-argument \
  -Wno-unknown-warning-option \
  -Wno-gnu-variable-sized-type-not-at-end \
  "${args[@]}"
