FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04

# ESP-IDF prerequisites (https://docs.espressif.com -> ESP-IDF Linux setup)
RUN apt-get update && apt-get install -y \
    python3-venv \
    python3-pip \
    python-is-python3 \
    udev \
    build-essential \
    git wget flex bison gperf \
    cmake ninja-build ccache \
    clang-format \
    libffi-dev libssl-dev dfu-util libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

# Hook tooling (pre-commit config uses language:system for clang-format and
# cppcheck; pre-commit itself would otherwise vanish on container rebuilds)
RUN pip3 install --no-cache-dir pre-commit


# Serial port access
RUN usermod -aG dialout vscode \
    && usermod -aG plugdev vscode \
    && mkdir -p /home/vscode/.claude && chown vscode:vscode /home/vscode/.claude

# Bake ESP-IDF v6.0.1 + ESP32-S2 toolchain into the image so container
# rebuilds start with a working environment (~3 GB layer, docker-cached).
# Activate in a shell with: source ~/esp/esp-idf/export.sh
USER vscode


# Create a script file sourced by both interactive and non-interactive bash shells
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV BASH_ENV="/home/vscode/.bash_env"
ENV NVM_DIR="/home/vscode/.nvm"

RUN touch "${BASH_ENV}" && echo '. "${BASH_ENV}"' >> ~/.bashrc
# Download and install nvm
RUN curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.5/install.sh | PROFILE="${BASH_ENV}" bash \
    && echo node > /home/vscode/.nvmrc \
    && . "${NVM_DIR}/nvm.sh" \
    && cd /home/vscode \
    && nvm install

RUN mkdir -p /home/vscode/esp \
    && git clone --branch v6.0.1 --depth 1 --recursive --shallow-submodules \
       https://github.com/espressif/esp-idf.git /home/vscode/esp/esp-idf \
    && /home/vscode/esp/esp-idf/install.sh esp32s2 \
    && rm -rf /home/vscode/.espressif/dist

#RUN jq '. + {"hasCompletedOnboarding": true}' /home/vscode/.claude.json
