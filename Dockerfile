# Builds the web game and the duel server on Debian 12, then runs the server.
#   docker build -t tetris . && docker run -p 8080:8080 tetris

FROM debian:bookworm AS web
RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake build-essential python3 xz-utils ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ARG EMSDK_VERSION=6.0.12
# The emsdk download server drops connections now and then, so retry the install.
RUN git clone --depth 1 https://github.com/emscripten-core/emsdk.git /emsdk \
    && for i in 1 2 3 4 5; do /emsdk/emsdk install ${EMSDK_VERSION} && break; sleep 10; done \
    && /emsdk/emsdk activate ${EMSDK_VERSION}
WORKDIR /src
COPY CMakeLists.txt ./
COPY assets assets
COPY src src
COPY tests tests
COPY web web
# emsdk_env.sh needs bash; Debian's /bin/sh is dash.
SHELL ["/bin/bash", "-c"]
RUN . /emsdk/emsdk_env.sh \
    && emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build-web -j

FROM debian:bookworm AS server
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ARG GO_VERSION=1.27.2
ARG TARGETARCH
RUN curl -fsSL https://go.dev/dl/go${GO_VERSION}.linux-${TARGETARCH:-amd64}.tar.gz | tar -C /usr/local -xz
ENV PATH=/usr/local/go/bin:$PATH
WORKDIR /src/server
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server ./
RUN go test ./... && CGO_ENABLED=0 go build -o /tetris-server .

FROM debian:bookworm-slim
COPY --from=server /tetris-server /usr/local/bin/tetris-server
COPY --from=web /src/build-web/index.html /src/build-web/index.js /src/build-web/index.wasm /srv/web/
EXPOSE 8080
USER nobody
ENTRYPOINT ["tetris-server", "-dir", "/srv/web"]
