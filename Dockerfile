# Build stage
FROM alpine:latest AS dependencies

# Install dependencies
RUN apk add --no-cache \
    binutils \
    clang \
    coreutils \
    curl \
    gcc \
    git \
    linux-headers \
    liburing-dev \
    lld \
    make \
    mingw-w64-binutils \
    mingw-w64-gcc \
    musl-dev

FROM dependencies AS build

# Copy source files
WORKDIR /wibo
COPY . /wibo

# Target platform (automatically set by Docker buildx)
ARG TARGETPLATFORM

# Build type (release, debug)
ARG BUILD_TYPE=release

# Enable link-time optimization (LTO) for release builds (1, 0)
ARG LTO=1

# Version string (if not provided, defaults to "unknown")
ARG WIBO_VERSION

# Build static binary, then run the fixture tests against it
RUN case "$TARGETPLATFORM" in \
        linux/amd64) ARCH=x86_64; TARGET_ARCH_FLAGS=; CC="clang --target=x86_64-alpine-linux-musl" ;; \
        linux/386) ARCH=i686; TARGET_ARCH_FLAGS=; CC="clang --target=i586-alpine-linux-musl" ;; \
        *) echo "Error: Unsupported platform '$TARGETPLATFORM'. Supported platforms: linux/amd64, linux/386" >&2; exit 1 ;; \
    esac; \
    if [ "$BUILD_TYPE" = release ]; then LTO_VALUE=$LTO; else LTO_VALUE=0; fi; \
    echo "Building for $TARGETPLATFORM ($BUILD_TYPE, $ARCH)" \
    && make CONFIG="$BUILD_TYPE" ARCH="$ARCH" CC="$CC" TARGET_ARCH_FLAGS="$TARGET_ARCH_FLAGS" \
        LTO="$LTO_VALUE" WIBO_ENABLE_LIBURING=1 WIBO_LIBURING_STATIC=1 \
        EXTRA_LDFLAGS="-fuse-ld=lld -static" WIBO_VERSION="$WIBO_VERSION" all test \
    && WIBO_OUT=$(make -s print-wibo CONFIG="$BUILD_TYPE" ARCH="$ARCH") \
    && mkdir -p /wibo/out \
    && cp "$WIBO_OUT" /wibo/out/wibo \
    && ( [ "$BUILD_TYPE" != release ] || strip -g /wibo/out/wibo )

# Export binary (usage: docker build --target export --output build .)
FROM scratch AS export

COPY --from=build /wibo/out/wibo .

# Runnable container
FROM alpine:latest

COPY --from=build /wibo/out/wibo /usr/local/bin/wibo
CMD ["/usr/local/bin/wibo"]
