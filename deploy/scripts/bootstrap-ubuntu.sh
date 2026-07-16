#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: sudo ./deploy/scripts/bootstrap-ubuntu.sh --domain example.com [options]

Prepare an Ubuntu 22.04/24.04 host for the future QuizPane static site and
Release proxy. This script installs infrastructure only; it does not deploy a
website or proxy executable.

Options:
  --domain DOMAIN       Required public domain (ASCII or punycode hostname).
  --email EMAIL         Email used by Certbot when --issue-cert is selected.
  --issue-cert          Obtain and install a Let's Encrypt certificate now.
  --configure-firewall  Allow OpenSSH and Nginx Full through UFW.
  --help                Show this help.
EOF
}

domain=""
email=""
issue_cert=0
configure_firewall=0
while (($#)); do
  case "$1" in
    --domain) domain="${2:?--domain requires a value}"; shift 2 ;;
    --email) email="${2:?--email requires a value}"; shift 2 ;;
    --issue-cert) issue_cert=1; shift ;;
    --configure-firewall) configure_firewall=1; shift ;;
    --help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi
if [[ ! "$domain" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] || [[ "$domain" == *..* ]]; then
  echo "--domain must be an ASCII or punycode hostname." >&2
  exit 2
fi
if (( issue_cert )) && [[ -z "$email" ]]; then
  echo "--issue-cert requires --email." >&2
  exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
deploy_dir="$(cd -- "$script_dir/.." && pwd)"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  nginx certbot python3-certbot-nginx ufw curl ca-certificates openssl \
  gettext-base rsync

if ! id -u quizpane >/dev/null 2>&1; then
  adduser --system --group --home /srv/quizpane --shell /usr/sbin/nologin quizpane
fi

install -d -m 0755 /srv/quizpane/site/releases /srv/quizpane/releases /var/lib/quizpane
install -d -m 0755 /opt/quizpane/release-proxy/releases /var/www/certbot
chown -R quizpane:quizpane /srv/quizpane/releases /var/lib/quizpane /opt/quizpane/release-proxy
install -d -o www-data -g www-data -m 0755 /var/cache/nginx/quizpane-meta

env_file=/etc/quizpane/release-proxy.env
if [[ ! -e "$env_file" ]]; then
  install -d -m 0750 /etc/quizpane
  install -m 0600 "$deploy_dir/env/release-proxy.env.example" "$env_file"
  secret="$(openssl rand -hex 32)"
  sed -i "s/^WEBHOOK_SECRET=.*/WEBHOOK_SECRET=${secret}/" "$env_file"
  echo "Created $env_file. Add GITHUB_TOKEN before production traffic if desired."
else
  echo "Keeping existing $env_file."
fi

export QUIZPANE_DOMAIN="$domain"
envsubst '${QUIZPANE_DOMAIN}' < "$deploy_dir/nginx/quizpane.conf.template" \
  > /etc/nginx/sites-available/quizpane.conf
install -m 0644 "$deploy_dir/nginx/quizpane-global.conf" /etc/nginx/conf.d/quizpane-global.conf
install -m 0644 "$deploy_dir/systemd/quizpane-release-proxy.service" \
  /etc/systemd/system/quizpane-release-proxy.service
ln -sfn /etc/nginx/sites-available/quizpane.conf /etc/nginx/sites-enabled/quizpane.conf

# Keep nginx valid and informative until a real static artifact is deployed.
if [[ ! -e /srv/quizpane/site/current ]]; then
  install -d -m 0755 /srv/quizpane/site/releases/bootstrap
  cat > /srv/quizpane/site/releases/bootstrap/index.html <<'EOF'
<!doctype html><meta charset="utf-8"><title>QuizPane</title><p>QuizPane website is being prepared.</p>
EOF
  ln -sfn releases/bootstrap /srv/quizpane/site/current
fi

nginx -t
systemctl enable nginx
systemctl reload nginx
systemctl daemon-reload
systemctl enable quizpane-release-proxy.service

if (( configure_firewall )); then
  ufw allow OpenSSH
  ufw allow 'Nginx Full'
fi

if (( issue_cert )); then
  certbot --nginx --non-interactive --agree-tos --redirect --email "$email" -d "$domain"
fi

cat <<EOF
Bootstrap complete for ${domain}.
Next: deploy a static artifact and the release-proxy executable with
  sudo ./deploy/scripts/install-artifacts.sh --site-dist /path/to/site-dist --proxy-binary /path/to/quizpane-release-proxy --version vX.Y.Z
EOF
