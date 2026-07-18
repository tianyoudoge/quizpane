#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: sudo ./deploy/scripts/install-artifacts.sh --version VERSION [options]

Atomically install a built static site and/or the future Release proxy binary
onto a host already prepared by bootstrap-ubuntu.sh.

Options:
  --version VERSION      Required deployment label (letters, digits, . _ and -).
  --site-dist DIRECTORY  Built static site directory; must contain index.html.
  --proxy-binary FILE    Executable release-proxy binary.
  --no-restart           Copy artifacts but do not reload nginx or restart proxy.
  --help                 Show this help.
EOF
}

version=""
site_dist=""
proxy_binary=""
restart=1
while (($#)); do
  case "$1" in
    --version) version="${2:?--version requires a value}"; shift 2 ;;
    --site-dist) site_dist="${2:?--site-dist requires a value}"; shift 2 ;;
    --proxy-binary) proxy_binary="${2:?--proxy-binary requires a value}"; shift 2 ;;
    --no-restart) restart=0; shift ;;
    --help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi
if [[ ! "$version" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]]; then
  echo "--version contains unsupported characters." >&2
  exit 2
fi
if [[ -z "$site_dist" && -z "$proxy_binary" ]]; then
  echo "Specify --site-dist and/or --proxy-binary." >&2
  exit 2
fi
if [[ -n "$site_dist" && ( ! -d "$site_dist" || ! -f "$site_dist/index.html" ) ]]; then
  echo "--site-dist must be a directory containing index.html." >&2
  exit 2
fi
if [[ -n "$proxy_binary" && ( ! -f "$proxy_binary" || ! -x "$proxy_binary" ) ]]; then
  echo "--proxy-binary must be an executable file." >&2
  exit 2
fi
if [[ ! -f /etc/systemd/system/quizpane-release-proxy.service ]]; then
  echo "Bootstrap is missing; run bootstrap-ubuntu.sh first." >&2
  exit 1
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
deploy_dir="$(cd -- "$script_dir/.." && pwd)"

switch_current() {
  local base="$1" target="$2"
  ln -s "$target" "$base/.current-next"
  mv -Tf "$base/.current-next" "$base/current"
}

site_changed=0
proxy_changed=0
previous_proxy_target=""
if [[ -n "$site_dist" ]]; then
  destination="/srv/quizpane/site/releases/$version"
  if [[ -e "$destination" ]]; then
    echo "Static site deployment already exists: $destination" >&2
    exit 1
  fi
  staging="$(mktemp -d /srv/quizpane/site/releases/.staging.XXXXXX)"
  trap 'rm -rf "${staging:-}"' EXIT
  rsync -a --delete "$site_dist/" "$staging/"
  find "$staging" -type d -exec chmod 0755 {} +
  find "$staging" -type f -exec chmod 0644 {} +
  chown -R root:root "$staging"
  mv "$staging" "$destination"
  staging=""
  switch_current /srv/quizpane/site "releases/$version"
  site_changed=1
fi

if [[ -n "$proxy_binary" ]]; then
  destination="/opt/quizpane/release-proxy/releases/$version"
  if [[ -e "$destination" ]]; then
    echo "Proxy deployment already exists: $destination" >&2
    exit 1
  fi
  install -d -o quizpane -g quizpane -m 0755 "$destination"
  install -o quizpane -g quizpane -m 0755 "$proxy_binary" "$destination/quizpane-release-proxy"
  previous_proxy_target="$(readlink /opt/quizpane/release-proxy/current 2>/dev/null || true)"
  switch_current /opt/quizpane/release-proxy "releases/$version"
  proxy_changed=1
fi

if (( restart )); then
  if (( site_changed )); then
    nginx -t
    systemctl reload nginx
  fi
  if (( proxy_changed )); then
    # service unit 也是代理部署的一部分。若只更新脚本、保留首次 bootstrap 时
    # 的旧 unit，ExecStart/安全限制修复不会生效，甚至会持续重启一个失效软链接。
    install -m 0644 "$deploy_dir/systemd/quizpane-release-proxy.service" \
      /etc/systemd/system/quizpane-release-proxy.service
    systemctl daemon-reload
    if ! systemctl restart quizpane-release-proxy.service; then
      if [[ -n "$previous_proxy_target" ]]; then
        switch_current /opt/quizpane/release-proxy "$previous_proxy_target"
        systemctl restart quizpane-release-proxy.service || true
      else
        rm -f /opt/quizpane/release-proxy/current
      fi
      echo "Proxy failed to start; restored the previous proxy link. Inspect: journalctl -u quizpane-release-proxy -n 100" >&2
      exit 1
    fi
    if ! curl --fail --silent --show-error http://127.0.0.1:8787/healthz >/dev/null; then
      if [[ -n "$previous_proxy_target" ]]; then
        switch_current /opt/quizpane/release-proxy "$previous_proxy_target"
        systemctl restart quizpane-release-proxy.service || true
      else
        rm -f /opt/quizpane/release-proxy/current
        systemctl stop quizpane-release-proxy.service || true
      fi
      echo "Proxy health check failed; restored the previous proxy link." >&2
      exit 1
    fi
  fi
fi

echo "Installed deployment $version."
