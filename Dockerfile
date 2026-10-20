# Optra on Ubuntu 26.04, the reference system: everything its quick start installs, the engine built.
#   docker compose up --build        the whole system on a simulated market, the page on :5173
# The deps stage is also the dev container's (.devcontainer/devcontainer.json), which builds the
# workspace itself.
FROM ubuntu:26.04 AS deps
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake make ccache lld git curl ca-certificates \
      libarrow-dev libparquet-dev libprotobuf-dev protobuf-compiler libgrpc++-dev protobuf-compiler-grpc \
      libabsl-dev nlohmann-json3-dev libsqlite3-dev libgtest-dev 7zip nodejs npm \
    && rm -rf /var/lib/apt/lists/*
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh

FROM deps
WORKDIR /optra
COPY . .
RUN ./build.sh r \
    && (cd app/server && uv sync -q) \
    && (cd app/web && npm ci --no-audit --no-fund)
ENV WEB_HOST=0.0.0.0
EXPOSE 5173
CMD ["./scripts/dev.sh", "sim"]
