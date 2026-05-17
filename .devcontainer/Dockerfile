FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-22.04

RUN apt-get update && apt-get install -y \
    python3-venv \
    python3-pip \
    python-is-python3 \
    udev \
    build-essential \
    git \
    && rm -rf /var/lib/apt/lists/*

# Create a script file sourced by both interactive and non-interactive bash shells
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
ENV BASH_ENV "${HOME}/.bash_env"
RUN touch "${BASH_ENV}"
RUN echo '. "${BASH_ENV}"' >> ~/.bashrc

# Download and install nvm
RUN curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.4/install.sh | PROFILE="${BASH_ENV}" bash
RUN echo node > .nvmrc
RUN nvm install

# Serial port access
RUN usermod -aG dialout vscode \
    && usermod -aG plugdev vscode \
    && mkdir -p /home/vscode/.claude && chown vscode:vscode /home/vscode/.claude \
    && mkdir -p /workspaces/.pio && chown vscode:vscode /workspaces/.pio
# Install PlatformIO as vscode user so ~/.platformio lands in /home/vscode
USER vscode
RUN pip3 install --user platformio

ENV PATH="/home/vscode/.local/bin:${PATH}"
