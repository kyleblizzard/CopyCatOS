#!/usr/bin/env bash
# CopyCatOS — by Kyle Blizzard at Blizzard.show

# native.profile — bwrap arg-string generator for the 'native' sandbox tier.
#
# Strict tier. Sandbox privacy is first-class here:
#
#   * /  is ro-bind so /usr, /lib, /bin, /etc resolve normally.
#   * The user's real home is overlaid with an empty tmpfs, so
#     /home/<user>/.ssh, /home/<user>/Documents, and every sibling
#     app's data dir vanish from the sandbox view. Nothing the app
#     can read on /home belongs to any other app or the user's
#     private files — unless an explicit filesystem entitlement
#     re-binds it after this script runs (see moonbase-launch).
#   * /proc and /dev are pid-/sandbox-local.
#   * /tmp is a fresh tmpfs.
#   * The bundle directory itself is mounted read-only on its real
#     absolute path — Info.appc stays consistent with what the
#     launcher validated.
#   * The bundle's per-app data directory is the only writable
#     region.  $HOME inside the sandbox points to it.
#   * Network is unshared by default; callers pass unshare_net=0 to
#     opt an app into host networking (when the bundle's declared
#     network:outbound:* entitlements warrant it).
#
# Inputs (positional):
#   $1 = absolute path to the .appc bundle directory
#   $2 = absolute path to the app's writable data directory
#        (~/.local/share/moonbase/<bundle-id>/)
#   $3 = unshare_net: "1" (default) unshares the host netns,
#        "0" leaves the app on the host network.
#   $4 = host $HOME — the real user home to overlay with a tmpfs.
#        Falls back to $HOME when omitted so the script stays usable
#        by hand; the launcher always passes it explicitly.
#
# Output:
#   One bwrap arg per line on stdout. Consume from bash with:
#       mapfile -t args < <(native.profile "$bundle" "$data" 1 "$HOME")
#       bwrap "${args[@]}" -- "$inner_cmd" "$@"
#
# Exit:
#   0 on success. Non-zero if a required input is missing.

set -euo pipefail

bundle_path="${1:?native.profile: bundle path required}"
data_path="${2:?native.profile: data path required}"
unshare_net="${3:-1}"
host_home="${4:-${HOME:-}}"

# PID-namespace unshare is on by default. The launcher sets
# MOONBASE_UNSHARE_PID=0 when the bundle declares system:process-list,
# so Activity Monitor-class apps can see host PIDs through /proc.
unshare_pid="${MOONBASE_UNSHARE_PID:-1}"

printf '%s\n' \
    --ro-bind     /               /                 \
    --proc        /proc                             \
    --dev         /dev                              \
    --tmpfs       /tmp

# Overlay the user's real home with an empty tmpfs BEFORE binding the
# app's data dir and any entitlement-scoped subdirs on top. Skip the
# overlay if we can't tell where home is — better to run a little less
# sandboxed than to fail the launch.
if [[ -n "$host_home" ]]; then
    printf '%s\n' --tmpfs "$host_home"
fi

printf '%s\n' \
    --ro-bind     "$bundle_path"  "$bundle_path"    \
    --bind        "$data_path"    "$data_path"      \
    --unshare-user                                  \
    --unshare-ipc                                   \
    --unshare-uts                                   \
    --unshare-cgroup                                \
    --die-with-parent                               \
    --new-session                                   \
    --clearenv                                      \
    --setenv      HOME            "$data_path"      \
    --setenv      PATH            /usr/bin:/bin

if [[ "$unshare_pid" == "1" ]]; then
    printf '%s\n' --unshare-pid
fi

if [[ "$unshare_net" == "1" ]]; then
    printf '%s\n' --unshare-net
fi
