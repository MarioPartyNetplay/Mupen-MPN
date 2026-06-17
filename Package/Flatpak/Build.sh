#!/usr/bin/env bash
# Build a local Flatpak bundle and write a detailed log to flatpak-build.log.
#
# Usage:
#   ./Package/Flatpak/Build.sh [output.flatpak]
#
# Environment overrides:
#   FLATPAK_BUILD_DIR   build directory (default: /tmp/flatpak-build-dir)
#   FLATPAK_STATE_DIR   flatpak-builder state (default: <repo>/.flatpak-builder)
#   FLATPAK_REPO_DIR    flatpak repo path (default: /tmp/flatpak-repo)
#   FLATPAK_LOG_FILE    log file path (default: <repo>/flatpak-build.log)

set -euo pipefail

export FLATPAK_FANCY_OUTPUT=0

script_dir="$(cd "$(dirname "$0")" && pwd)"
toplvl_dir="$(realpath "$script_dir/../..")"
manifest_path="$script_dir/../flatpak.yml"
log_file="${FLATPAK_LOG_FILE:-$toplvl_dir/flatpak-build.log}"
bundle_output="${1:-$toplvl_dir/MupenMPN-linux.flatpak}"
build_dir="${FLATPAK_BUILD_DIR:-/tmp/flatpak-build-dir}"
state_dir="${FLATPAK_STATE_DIR:-$toplvl_dir/.flatpak-builder}"
repo_dir="${FLATPAK_REPO_DIR:-/tmp/flatpak-repo}"
local_manifest="$(mktemp /tmp/flatpak-local.XXXXXX.yml)"
app_id="org.mariopartynetplay.RMG-MPN"

cleanup() {
    rm -f "$local_manifest"
}
trap cleanup EXIT

timestamp() {
    date -u '+%Y-%m-%d %H:%M:%S UTC'
}

log() {
    echo "[$(timestamp)] $*" | tee -a "$log_file"
}

log_section() {
    {
        echo ""
        echo "========== $* =========="
        echo ""
    } | tee -a "$log_file"
}

log_kv() {
    printf '[%s] %-24s %s\n' "$(timestamp)" "$1" "$2" | tee -a "$log_file"
}

run_logged() {
    local label="$1"
    shift

    log_section "$label"
    log "command: $*"

    local start_ts
    start_ts="$(date +%s)"

    set +e
    "$@" 2>&1 | tee -a "$log_file"
    local exit_code="${PIPESTATUS[0]}"
    set -e

    local elapsed=$(( $(date +%s) - start_ts ))
    if [[ "$exit_code" -eq 0 ]]; then
        log "finished: $label (${elapsed}s, exit 0)"
    else
        log "FAILED: $label (${elapsed}s, exit $exit_code)"
        dump_build_errors
        exit "$exit_code"
    fi
}

dump_build_errors() {
    log_section "Build error context"

    local build_root="$state_dir/build"
    if [[ ! -d "$build_root" ]]; then
        log "no module build directory at $build_root"
        return
    fi

    local latest_log=""
    local latest_mtime=0
    local module_dir log_file_path mtime

    while IFS= read -r -d '' module_dir; do
        log_file_path="$module_dir/.flatpak-builder/build.log"
        if [[ -f "$log_file_path" ]]; then
            mtime="$(stat -c %Y "$log_file_path" 2>/dev/null || stat -f %m "$log_file_path")"
            if (( mtime > latest_mtime )); then
                latest_mtime="$mtime"
                latest_log="$log_file_path"
            fi
        fi
    done < <(find "$build_root" -mindepth 1 -maxdepth 1 -type d -print0 2>/dev/null)

    if [[ -z "$latest_log" ]]; then
        log "no module build.log files found under $build_root"
        ls -la "$build_root" 2>&1 | tee -a "$log_file" || true
        return
    fi

    log "last module build log: $latest_log"
    {
        echo "--- tail -n 80 $latest_log ---"
        tail -n 80 "$latest_log"
        echo "--- end tail ---"
    } | tee -a "$log_file"
}

write_local_manifest() {
    log_section "Prepare local manifest"

    if [[ ! -f "$manifest_path" ]]; then
        log "ERROR: manifest not found: $manifest_path"
        exit 1
    fi

    sed \
        -e "s|type: git|type: dir|" \
        -e "s|url: https://github.com/MarioPartyNetplay/Mupen-MPN.git|path: $toplvl_dir|" \
        "$manifest_path" > "$local_manifest"

    log_kv "manifest source" "$manifest_path"
    log_kv "local manifest" "$local_manifest"
    log_kv "source tree" "$toplvl_dir"
    log "RMG-MPN sources section:"
    awk '/name: RMG-MPN/,/^  - name:|^modules:/ { print }' "$local_manifest" | tee -a "$log_file"
}

collect_environment() {
    log_section "Environment"

    log_kv "hostname" "$(hostname 2>/dev/null || echo unknown)"
    log_kv "user" "$(whoami 2>/dev/null || echo unknown)"
    log_kv "pwd" "$(pwd)"
    log_kv "repo" "$toplvl_dir"
    log_kv "bundle output" "$bundle_output"
    log_kv "build dir" "$build_dir"
    log_kv "state dir" "$state_dir"
    log_kv "repo dir" "$repo_dir"
    log_kv "app id" "$app_id"

    if command -v git >/dev/null 2>&1 && git -C "$toplvl_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        log_kv "git branch" "$(git -C "$toplvl_dir" rev-parse --abbrev-ref HEAD)"
        log_kv "git commit" "$(git -C "$toplvl_dir" rev-parse HEAD)"
        log_kv "git describe" "$(git -C "$toplvl_dir" describe --tags --always --dirty 2>/dev/null || true)"
        if ! git -C "$toplvl_dir" diff --quiet || ! git -C "$toplvl_dir" diff --cached --quiet; then
            log "warning: working tree has uncommitted changes"
            git -C "$toplvl_dir" status --short 2>&1 | tee -a "$log_file"
        fi
    fi

    for tool in flatpak flatpak-builder cmake ninja pkg-config python3; do
        if command -v "$tool" >/dev/null 2>&1; then
            log_kv "$tool" "$("$tool" --version 2>&1 | head -n 1)"
        else
            log_kv "$tool" "not found"
        fi
    done

    if command -v df >/dev/null 2>&1; then
        log "disk space:"
        df -h "$toplvl_dir" /tmp 2>&1 | tee -a "$log_file" || true
    fi
}

ensure_flathub() {
    if flatpak remote-list --columns=name | grep -qx flathub; then
        log "flathub remote already configured"
        return
    fi

    run_logged "Add flathub remote" \
        flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
}

main() {
    : > "$log_file"

    log_section "Flatpak build started"
    collect_environment
    write_local_manifest
    ensure_flathub

    run_logged "flatpak-builder" \
        flatpak-builder \
            --verbose \
            --state-dir="$state_dir" \
            --repo="$repo_dir" \
            --force-clean \
            --install-deps-from=flathub \
            "$build_dir" \
            "$local_manifest"

    run_logged "flatpak build-bundle" \
        flatpak build-bundle "$repo_dir" "$bundle_output" "$app_id"

    log_section "Flatpak build finished"
    log_kv "bundle" "$bundle_output"
    ls -lh "$bundle_output" 2>&1 | tee -a "$log_file"
}

main "$@"
