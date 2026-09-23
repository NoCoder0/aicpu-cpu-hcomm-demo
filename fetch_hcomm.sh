#!/bin/bash
set -eo pipefail
cd "$(dirname "$(readlink -f "$0")")"
work=$PWD
repo=https://github.com/NoCoder0/hcomm.git
branch=feat/aicpu-cpu-rdma
source_dir="$work/build/hcomm"
mkdir -p "$work/build"

if [ ! -e "$source_dir" ]; then
    git clone --depth 1 --single-branch --branch "$branch" "$repo" "$source_dir"
else
    test -d "$source_dir/.git" || { echo "Not a standalone hcomm clone: $source_dir" >&2; exit 1; }
    test "$(git -C "$source_dir" remote get-url origin)" = "$repo" || {
        echo "Unexpected hcomm origin in $source_dir" >&2; exit 1;
    }
    test "$(git -C "$source_dir" branch --show-current)" = "$branch" || {
        echo "Expected hcomm branch $branch" >&2; exit 1;
    }
    test -z "$(git -C "$source_dir" status --porcelain)" || {
        echo "hcomm has local changes; keep or commit them before updating: $source_dir" >&2; exit 1;
    }
    git -C "$source_dir" fetch origin "refs/heads/$branch:refs/remotes/origin/$branch"
    git -C "$source_dir" merge-base --is-ancestor HEAD "origin/$branch" || {
        echo "hcomm has local commits or divergent history; cannot update to origin/$branch" >&2; exit 1;
    }
    git -C "$source_dir" merge --ff-only "origin/$branch"
fi
git -C "$source_dir" rev-parse HEAD > "$work/build/hcomm-source.commit"
printf 'hcomm source: %s (%s)\n' "$source_dir" "$(cat "$work/build/hcomm-source.commit")"
