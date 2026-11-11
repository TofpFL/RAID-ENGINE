#!/bin/bash
# Installs Ubuntu packages on a GitHub Actions runner:
#   tools/ci_apt_install.sh <package>...
# The runners fetch from their own Ubuntu mirror list (/etc/apt/apt-mirrors.txt).
# When that mirror cannot be reached (as when azure.archive.ubuntu.com stopped
# resolving on the runners), the same packages come from other official Ubuntu
# mirrors instead, so the build does not fail on someone else's outage.
set -u
opts=(-o Acquire::Retries=1 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15)

attempt() {
  sudo apt-get "${opts[@]}" update -q && sudo apt-get "${opts[@]}" install -y -q "$@"
}

attempt "$@" && exit 0
for mirror in http://archive.ubuntu.com/ubuntu/ http://mirrors.edge.kernel.org/ubuntu/ http://mirror.us.leaseweb.net/ubuntu/; do
  echo "::warning::apt: the runner's Ubuntu mirror failed, trying $mirror"
  if [ -f /etc/apt/apt-mirrors.txt ]; then
    echo "$mirror" | sudo tee /etc/apt/apt-mirrors.txt > /dev/null
  else
    sudo sed -i -E "s#https?://[a-z.]*ubuntu\.com/ubuntu/?#$mirror#g" /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2> /dev/null
  fi
  attempt "$@" && exit 0
done
echo "::error::apt: no Ubuntu mirror could install $*"
exit 1
