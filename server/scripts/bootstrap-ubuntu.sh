#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -eq 0 ]]; then
    echo "Run this script as a sudo-capable non-root user, not as root." >&2
    exit 1
fi

source /etc/os-release
if [[ ${ID} != "ubuntu" || ( ${VERSION_ID} != "22.04" && ${VERSION_ID} != "24.04" ) ]]; then
    echo "Ubuntu 22.04 or 24.04 is required. Detected ${PRETTY_NAME}." >&2
    exit 1
fi

sudo apt-get update
sudo apt-get install -y ca-certificates curl

for package in docker.io docker-compose docker-compose-v2 docker-doc docker-buildx podman-docker containerd runc; do
    sudo apt-get remove -y "${package}" 2>/dev/null || true
done

sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
    -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc

sudo tee /etc/apt/sources.list.d/docker.sources >/dev/null <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: ${UBUNTU_CODENAME:-$VERSION_CODENAME}
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF

sudo apt-get update
sudo apt-get install -y \
    docker-ce \
    docker-ce-cli \
    containerd.io \
    docker-buildx-plugin \
    docker-compose-plugin

sudo systemctl enable --now docker.service containerd.service
sudo docker run --rm hello-world
sudo docker version
sudo docker compose version

cat <<'EOF'

Docker is installed. This deployment intentionally uses sudo for Docker because
membership in the docker group grants root-equivalent privileges.
EOF