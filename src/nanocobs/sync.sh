#!/usr/bin/env bash
set -euo pipefail

repo_url="${NANOCOBS_REPO_URL:-https://github.com/charlesnicholson/nanocobs.git}"
branch="${NANOCOBS_BRANCH:-main}"
revision="${NANOCOBS_REVISION:-}"

usage() {
    echo "usage: $0 [--branch BRANCH] [--revision COMMIT]" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        -b|--branch)
            if [ "$#" -lt 2 ]; then
                usage
                exit 2
            fi
            branch="$2"
            shift 2
            ;;
        -r|--revision)
            if [ "$#" -lt 2 ]; then
                usage
                exit 2
            fi
            revision="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

vendor_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
tmp_dir="$(mktemp -d)"

cleanup() {
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

if [ -n "$revision" ]; then
    git clone --quiet "$repo_url" "$tmp_dir/nanocobs"
    git -C "$tmp_dir/nanocobs" checkout --quiet "$revision"
else
    git clone --quiet --branch "$branch" --single-branch "$repo_url" "$tmp_dir/nanocobs"
fi

commit="$(git -C "$tmp_dir/nanocobs" rev-parse HEAD)"

cp "$tmp_dir/nanocobs/cobs.c" "$vendor_dir/cobs.c"
cp "$tmp_dir/nanocobs/cobs.h" "$vendor_dir/cobs.h"

cat > "$vendor_dir/REVISION" <<EOF
url $repo_url
branch $branch
commit $commit
EOF

echo "Synced nanocobs $branch at $commit"
