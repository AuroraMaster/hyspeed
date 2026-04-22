#!/usr/bin/env bash
set -euo pipefail

args=()
for arg in "$@"; do
  case "$arg" in
    -mfunction-return=* | \
    -mstack-protector-guard-symbol=* | \
    -fconserve-stack | \
    -fmin-function-alignment=* | \
    -mrecord-mcount | \
    -ftrivial-auto-var-init=zero | \
    -fsanitize=*bounds-strict* | \
    -fsanitize-recover=*bounds-strict* | \
    -fno-sanitize-recover=*bounds-strict* | \
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

exec "${KERNEL_CLANG:-clang}" \
  -Wno-unused-command-line-argument \
  -Wno-unknown-warning-option \
  -Wno-gnu-variable-sized-type-not-at-end \
  "${args[@]}"
