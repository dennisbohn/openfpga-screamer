# Bare-metal RISC-V toolchain for the firmware: Debian trixie's GCC 14, binutils and picolibc.
# Base image pinned by tag and digest, apt pinned to a fixed snapshot.debian.org timestamp,
# so every build gets the same packages. tools/fw.sh builds it as screamer-fw:trixie-20261005.
FROM debian:trixie-20261005-slim@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
ARG SNAPSHOT=20261005T000000Z
RUN rm -f /etc/apt/sources.list.d/debian.sources \
 && echo "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${SNAPSHOT} trixie main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Retries=5 update \
 && apt-get install -y --no-install-recommends \
        gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf picolibc-riscv64-unknown-elf \
        make python3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
