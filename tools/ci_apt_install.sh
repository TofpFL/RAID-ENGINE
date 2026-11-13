#!/bin/bash
# Installs Ubuntu packages on a GitHub Actions runner:
#   tools/ci_apt_install.sh <package>...
# The runners fetch from their own Ubuntu mirror list (/etc/apt/apt-mirrors.txt).
# When the ubuntu.com mirrors cannot be reached (as when their names stopped
# resolving on the runners), the packages come from other official Ubuntu
# mirrors instead. apt still checks Ubuntu's signatures, whatever the mirror.
set -u

missing=()
for p in "$@"; do
  dpkg -s "$p" > /dev/null 2>&1 || missing+=("$p")
done
if [ ${#missing[@]} -eq 0 ]; then
  echo "apt: $* already installed"
  exit 0
fi

codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
opts=(-o Acquire::Retries=1 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15)

# A quick look at the mirror's index for this release: a mirror whose name does
# not resolve fails here in seconds instead of once per package.
reachable() {
  curl -sf --max-time 10 -o /dev/null "${1%/}/dists/$codename/Release"
}

attempt() {
  sudo apt-get "${opts[@]}" update -q && sudo apt-get "${opts[@]}" install -y -q "${missing[@]}"
}

use_mirror() {
  if [ -f /etc/apt/apt-mirrors.txt ]; then
    echo "$1" | sudo tee /etc/apt/apt-mirrors.txt > /dev/null
  fi
  sudo sed -i -E "s#https?://[a-z.]*ubuntu\.com/ubuntu/?#$1#g" \
    /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources 2> /dev/null
  true
}

if reachable http://azure.archive.ubuntu.com/ubuntu/ && attempt; then
  exit 0
fi
for mirror in http://mirrors.edge.kernel.org/ubuntu/ http://mirror.us.leaseweb.net/ubuntu/ \
              http://ftp.halifax.rwth-aachen.de/ubuntu/ http://archive.ubuntu.com/ubuntu/; do
  if ! reachable "$mirror"; then
    echo "apt: $mirror cannot be reached"
    continue
  fi
  echo "::warning::apt: the runner's Ubuntu mirror cannot be reached, using $mirror"
  use_mirror "$mirror"
  attempt && exit 0
done
echo "::error::apt: no Ubuntu mirror could install ${missing[*]}"
exit 1
