# Start it: docker compose up --build
# Tighter limits: SERVER_MEM=64m SERVER_CPUS=0.5 docker compose up
# Watch memory and CPU live while the benchmark runs: docker stats raw_http_server
# Check whether it was killed for running out of memory: docker inspect -f '{{.State.OOMKilled}}' raw_http_server
# ASan build: docker compose build --build-arg BUILD_TYPE=Debug --build-arg ENABLE_ASAN=ON

FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG BUILD_TYPE=Release
ARG ENABLE_ASAN=OFF
RUN cmake -B /build -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DENABLE_ASAN=${ENABLE_ASAN} \
    && cmake --build /build -j"$(nproc)"

FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends libssl3t64 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /build/http_server /usr/local/bin/http_server

WORKDIR /app
COPY .env /app/.env

CMD ["http_server"]
