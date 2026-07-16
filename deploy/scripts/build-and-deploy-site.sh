#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: ./deploy/scripts/build-and-deploy-site.sh [options]

Build website/dist locally and install it as the live static site. Run this
from the repository root. Two modes:

  Local (default): build here, then run install-artifacts.sh with sudo on
  this machine. Use when this script runs directly on the target server.

  Remote (--remote-host): build here, rsync the result to the remote host,
  then run install-artifacts.sh there over SSH. Use from a CI runner or a
  developer machine that is not the server itself.

Options:
  --version LABEL       Deployment label passed to install-artifacts.sh.
                         Defaults to "site-<UTC timestamp>".
  --remote-host HOST    SSH destination (e.g. deploy@quizpane.example.com).
                         Requires passwordless sudo for install-artifacts.sh.
  --remote-repo PATH    Path to this repository checkout on the remote host.
                         Required with --remote-host.
  --no-restart          Forwarded to install-artifacts.sh: copy files but
                         skip the nginx reload.
  --help                Show this help.
EOF
}

version=""
remote_host=""
remote_repo=""
no_restart=0
while (($#)); do
  case "$1" in
    --version) version="${2:?--version requires a value}"; shift 2 ;;
    --remote-host) remote_host="${2:?--remote-host requires a value}"; shift 2 ;;
    --remote-repo) remote_repo="${2:?--remote-repo requires a value}"; shift 2 ;;
    --no-restart) no_restart=1; shift ;;
    --help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -n "$remote_host" && -z "$remote_repo" ]]; then
  echo "--remote-host requires --remote-repo." >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"

if [[ -z "$version" ]]; then
  version="site-$(date -u +%Y%m%d%H%M%S)"
fi

command -v node >/dev/null || { echo "node is required to build the site." >&2; exit 1; }

echo "Building website/dist..."
node "$repo_root/website/scripts/build-site.mjs"

install_args=(--version "$version")
if (( no_restart )); then
  install_args+=(--no-restart)
fi
install_args+=(--site-dist)

if [[ -z "$remote_host" ]]; then
  echo "Installing locally as version $version..."
  sudo "$repo_root/deploy/scripts/install-artifacts.sh" "${install_args[@]}" "$repo_root/website/dist"
  exit 0
fi

echo "Syncing website/dist to $remote_host:$remote_repo/website/dist..."
ssh "$remote_host" "mkdir -p '$remote_repo/website'"
rsync -az --delete "$repo_root/website/dist/" "$remote_host:$remote_repo/website/dist/"

remote_cmd="sudo $(printf '%q' "$remote_repo/deploy/scripts/install-artifacts.sh")"
for arg in "${install_args[@]}" "$remote_repo/website/dist"; do
  remote_cmd+=" $(printf '%q' "$arg")"
done

echo "Installing on $remote_host as version $version..."
ssh "$remote_host" "$remote_cmd"
