# Build environment for the boot menu: an armel (ARMv7-A, soft-float ABI) C cross compiler and a glibc 2.13 sysroot,
# the C library of the unit. The menu is plain C, so Debian's packaged cross GCC is enough; it links against the old
# sysroot (--sysroot) and tools/build.sh checks that no symbol newer than glibc 2.13 slips in.
#
#   docker build -t bootmenu-toolchain:1 toolchain      (a few minutes; CI caches it)
#
# Also in the image, for the tests: a native gcc (test programs), qemu-user (runs the ARM build) and Python.

ARG DEBIAN=debian:bookworm-slim

FROM ${DEBIAN} AS sysroot
RUN apt-get update && apt-get install -y --no-install-recommends debootstrap ca-certificates && rm -rf /var/lib/apt/lists/*
# Debian wheezy (eglibc 2.13, armel) is archived and its signatures have expired, hence --no-check-gpg. --foreign only
# unpacks the packages; no ARM code runs.
RUN debootstrap --arch=armel --foreign --variant=minbase --no-check-gpg --include=libc6-dev,linux-libc-dev \
      wheezy /sysroot http://archive.debian.org/debian \
 && for deb in /sysroot/var/cache/apt/archives/*.deb; do dpkg-deb -x "$deb" /sysroot; done \
 && rm -rf /sysroot/var/cache/apt/archives/*.deb /sysroot/debootstrap
# Absolute symlinks (e.g. libm.so -> /lib/arm-linux-gnueabi/libm.so.6) must point inside the sysroot.
RUN cd /sysroot && find . -type l | while read -r link; do \
      target=$(readlink "$link"); case "$target" in /*) \
        ln -sfn "$(realpath -m --relative-to="/sysroot/$(dirname "$link")" "/sysroot$target")" "$link";; esac; done

FROM ${DEBIAN}
RUN apt-get update && apt-get install -y --no-install-recommends gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi \
      gcc libc6-dev qemu-user file python3 ca-certificates && rm -rf /var/lib/apt/lists/*
COPY --from=sysroot /sysroot /opt/sp16/sysroot
COPY check-glibc.sh /usr/local/bin/sp16-check-glibc
RUN chmod +x /usr/local/bin/sp16-check-glibc
