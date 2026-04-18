#!/usr/bin/env bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# native.profile — bwrap arg-string generator for the 'native' sandbox tier.
#
# Strict tier: rootfs read-only, the bundle mounted read-only on
# itself so binaries and resources resolve by their real absolute
# path, the bundle's per-app data directory mounted read-write,
# no device access. Network is unshared by default; callers pass
# unshare_net=0 to opt an app into host networking (when the bundle's
# declared network:outbound:* entitlements warrant it).
#
# Inputs (positional):
#   $1 = absolute path to the .appc bundle directory
#   $2 = absolute path to the app's writable data directory
#        (~/.local/share/moonbase/<bundle-id>/)
#   $3 = unshare_net: "1" (default) unshares the host netns,
#        "0" leaves the app on the host network.
#
# Output:
#   One bwrap arg per line on stdout. Consume from bash with:
#       mapfile -t args < <(native.profile "$bundle" "$data" 1)
#       bwrap "${args[@]}" -- "$inner_cmd" "$@"
#
# Exit:
#   0 on success. Non-zero if a required input is missing.

set -euo pipefail

bundle_path="${1:?native.profile: bundle path required}"
data_path="${2:?native.profile: data path required}"
unshare_net="${3:-1}"

printf '%s\n' \
    --ro-bind     /               /                 \
    --proc        /proc                             \
    --dev         /dev                              \
    --tmpfs       /tmp                              \
    --ro-bind     "$bundle_path"  "$bundle_path"    \
    --bind        "$data_path"    "$data_path"      \
    --unshare-user                                  \
    --unshare-ipc                                   \
    --unshare-pid                                   \
    --unshare-uts                                   \
    --unshare-cgroup                                \
    --die-with-parent                               \
    --new-session                                   \
    --clearenv                                      \
    --setenv      HOME            "$data_path"      \
    --setenv      PATH            /usr/bin:/bin

if [[ "$unshare_net" == "1" ]]; then
    printf '%s\n' --unshare-net
fi
