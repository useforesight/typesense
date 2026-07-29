# syntax=docker/dockerfile:1.7

FROM ubuntu:20.04 AS builder

ARG TARGETARCH
ARG BAZELISK_SHA256
ARG BAZELISK_VERSION=v1.29.0

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        g++-10 \
        gcc-10 \
        git \
        lld \
        locales \
        m4 \
        make \
        zlib1g-dev \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

RUN curl --fail --location --silent --show-error \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${TARGETARCH}" \
        --output /usr/local/bin/bazel \
    && echo "${BAZELISK_SHA256}  /usr/local/bin/bazel" | sha256sum --check \
    && chmod 0755 /usr/local/bin/bazel \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 30 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-10 30 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 30 \
    && update-alternatives --set cc /usr/bin/gcc \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 30 \
    && update-alternatives --set c++ /usr/bin/g++

WORKDIR /src
COPY . .

RUN --mount=type=cache,target=/root/.cache/bazel \
    bazel build @com_google_protobuf//:protobuf_headers \
        @com_google_protobuf//:protobuf_lite \
        @com_google_protobuf//:protobuf \
        @com_google_protobuf//:protoc \
    && bazel build //:typesense-server \
    && cp bazel-bin/typesense-server /tmp/typesense-server

FROM ubuntu:22.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/typesense-server /opt/typesense-server
RUN chmod 0755 /opt/typesense-server

EXPOSE 8108
STOPSIGNAL SIGINT
ENTRYPOINT ["/opt/typesense-server"]
