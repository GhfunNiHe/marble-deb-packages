#!/usr/bin/env bash
# Publish release DEBs to the rolling "apt" release.
#
# The archive holds one version of each package: the previous asset for a
# package name is deleted before the new build is uploaded. RootFS CI pulls
# every asset from this tag.
set -euo pipefail

tag=apt
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
[ -n "${GH_TOKEN:-}" ] || { echo "GH_TOKEN is required" >&2; exit 1; }

debs=("$@")
((${#debs[@]} > 0)) || { echo "no DEBs to publish" >&2; exit 1; }

assets=""
if ! assets=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name' 2>/dev/null); then
    gh release create "$tag" --repo "$repo" --title "APT" --latest=false \
        --notes "Rolling archive of CI-built marble kernel and device packages."
    assets=""
fi

declare -A unique=()
for deb in "${debs[@]}"; do
    pkg=$(dpkg-deb -f "$deb" Package)
    [ -n "$pkg" ] || { echo "no Package field in $deb" >&2; exit 1; }
    if [ -n "${unique[$pkg]+x}" ]; then
        echo "duplicate package in publish set: $pkg" >&2
        exit 1
    fi
    unique["$pkg"]="$deb"
done

for pkg in "${!unique[@]}"; do
    while IFS= read -r old; do
        [ -n "$old" ] || continue
        case "$old" in
            "${pkg}_"*) gh release delete-asset "$tag" "$old" --repo "$repo" --yes ;;
        esac
    done <<< "$assets"
done

gh release upload "$tag" "${unique[@]}" --repo "$repo" --clobber
echo "published ${#unique[@]} packages to $repo@$tag"