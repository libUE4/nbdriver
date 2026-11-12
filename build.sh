#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
action="${1:-build}"
if (($#)); then
    shift
fi

case "$action" in
    build) exec make -C "$project_dir" all "$@" ;;
    clean) exec make -C "$project_dir" clean "$@" ;;
    help|-h|--help) exec make -C "$project_dir" help ;;
    *) printf '用法: %s {build|clean|help} [KDIR=/path/to/kernel/build]\n' "$0" >&2; exit 2 ;;
esac
