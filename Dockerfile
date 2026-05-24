# docker build --platform linux/arm64 -t piece-libretro:aarch64 .
FROM debian:trixie AS builder

# libretro core + PFI tools only:
#   - no libsdl3-dev (no X11/Wayland cascade)
#   - no libcli11-dev (CLI11 is only used by the SDL/headless frontends)
#   - no googletest (unit tests are disabled in libretro-only mode)
#   - libusb-1.0-0-dev is kept so ripper builds
RUN apt-get update \
	&& apt-get install -y build-essential gcc g++ git cmake \
		libusb-1.0-0-dev pkg-config \
	&& apt-get clean

ADD . /src
RUN mkdir /src/build

WORKDIR /src/build

RUN cmake ../src -DCMAKE_BUILD_TYPE=Release -DPIECE_LIBRETRO_ONLY=ON

RUN make -j

# docker run --rm piece-libretro:aarch64 tar -C /out -cvf - . > piece-libretro-aarch64.tar
FROM debian:trixie-slim AS runner

RUN mkdir -p /out /out/libretro /out/tools
WORKDIR /out

COPY --from=builder /src/README.md /out

COPY --from=builder /src/build/libretro/piece_libretro.so /out/libretro
COPY --from=builder /src/build/libretro/piece_libretro.info /out/libretro
COPY --from=builder /src/src/libretro/README.md /out/libretro

COPY --from=builder /src/build/tools/pfar /out/tools
COPY --from=builder /src/build/tools/mkpfi /out/tools
COPY --from=builder /src/build/tools/ripper /out/tools
