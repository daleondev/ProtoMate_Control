FROM archlinux:base-devel

# Use the same GCC 16 packages as the supported Arch host toolchain. A full
# upgrade keeps the compilers and their runtime libraries in sync.
RUN pacman -Syu --needed --noconfirm \
    arm-none-eabi-gcc \
    arm-none-eabi-gdb \
    arm-none-eabi-newlib \
    clang \
    cmake \
    gcc \
    gdb \
    git \
    librsvg \
    mesa \
    ncurses \
    ninja \
    openssh \
    openocd \
    patch \
    pkgconf \
    python \
    python-pip \
    ripgrep \
    ruby \
    ruby-erb \
    ruby-nokogiri \
    sdl2-compat \
    sigrok-cli \
    sigrok-firmware-fx2lafw \
    sudo \
    unzip \
    usbutils \
    xorg-server-xvfb \
    xorg-xauth \
    && pacman -Scc --noconfirm

# Keep pip packages isolated from Arch's externally managed system Python.
# This environment lives in the image, not in the host-mounted workspace.
ENV VIRTUAL_ENV=/opt/protomate-venv
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"
COPY scripts/requirements.txt /opt/protomate/requirements.txt
RUN python -m venv "${VIRTUAL_ENV}" \
    && python -m pip install --no-cache-dir -r /opt/protomate/requirements.txt \
    && python -m pip check

# Fail during image creation if a future repository update changes the major
# compiler version before the project's runtime port has been updated.
RUN test "$(gcc -dumpversion | cut -d. -f1)" = 16 \
    && test "$(arm-none-eabi-gcc -dumpversion | cut -d. -f1)" = 16 \
    && cmake --version \
    && pkg-config --exists sdl2 ncursesw \
    && rsvg-convert --version \
    && ruby -r nokogiri -r erb -r json -r fileutils -r pathname -e 'puts "Nokogiri #{Nokogiri::VERSION}; text converter imports ready"' \
    && python -c 'import numpy, PIL, serial; print("Python asset and hardware dependencies ready")'

RUN useradd --create-home --uid 1000 --user-group \
        --groups wheel,uucp,lock --shell /bin/bash vscode \
    && printf 'vscode ALL=(ALL) NOPASSWD:ALL\n' > /etc/sudoers.d/vscode \
    && chmod 0440 /etc/sudoers.d/vscode \
    && chown -R vscode:vscode "${VIRTUAL_ENV}"

ENV LANG=C.UTF-8

WORKDIR /workspace

CMD ["/bin/bash"]
