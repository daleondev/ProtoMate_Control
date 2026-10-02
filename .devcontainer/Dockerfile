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
    ninja \
    openssh \
    openocd \
    python \
    sudo \
    unzip \
    && pacman -Scc --noconfirm

# Fail during image creation if a future repository update changes the major
# compiler version before the project's runtime port has been updated.
RUN test "$(gcc -dumpversion | cut -d. -f1)" = 16 \
    && test "$(arm-none-eabi-gcc -dumpversion | cut -d. -f1)" = 16 \
    && cmake --version

RUN useradd --create-home --uid 1000 --user-group \
        --groups wheel,uucp,lock --shell /bin/bash vscode \
    && printf 'vscode ALL=(ALL) NOPASSWD:ALL\n' > /etc/sudoers.d/vscode \
    && chmod 0440 /etc/sudoers.d/vscode

ENV LANG=C.UTF-8

WORKDIR /workspace

CMD ["/bin/bash"]
