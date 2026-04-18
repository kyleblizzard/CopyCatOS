#!/usr/bin/env bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# webview.profile — bwrap arg-string generator for the 'webview' sandbox tier.
#
# Everything native.profile grants, plus /dev/dri so WPE WebKit can
# allocate GBM buffers and hand MoonRock zero-copy DMA-BUF surfaces.
# This is the only tier that touches the GPU device node; plain
# 'native' apps rendering through Cairo don't need it, and GL apps
# that aren't web views get their EGL context through the MoonRock
# IPC path rather than device-node access.
#
# Inputs (positional):
#   $1 = absolute path to the .appc bundle directory
#   $2 = absolute path to the app's writable data directory
#        (~/.local/share/moonbase/<bundle-id>/)
#   $3 = unshare_net: "1" (default) unshares the host netns,
#        "0" leaves the app on the host network.

set -euo pipefail

bundle_path="${1:?webview.profile: bundle path required}"
data_path="${2:?webview.profile: data path required}"
unshare_net="${3:-1}"

printf '%s\n' \
    --ro-bind     /               /                 \
    --proc        /proc                             \
    --dev         /dev                              \
    --tmpfs       /tmp                              \
    --ro-bind     "$bundle_path"  "$bundle_path"    \
    --bind        "$data_path"    "$data_path"      \
    --dev-bind    /dev/dri        /dev/dri          \
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
