# ==========================================
# Stage 1: Build Environment
# ==========================================
FROM ubuntu:24.04 AS builder

# Prevent tzdata/keyboard interactive prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies: Compiler, CMake, Ninja, Git (for FetchContent), and CA certs
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the entire project into the container
COPY . .

# Configure the project using Ninja and a Release build
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_CLIENT=OFF

# Build ONLY the signaling server.
# This will also trigger the POST_BUILD step to download cloudflared.
RUN cmake --build build --target pong_signaling

# ==========================================
# Stage 2: Minimal Runtime Environment
# ==========================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# certbot for TLS, cron for auto-renewal, ca-certificates for outbound TLS
RUN apt-get update && apt-get install -y \
    ca-certificates \
    certbot \
    cron \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the compiled signaling server from the builder stage
COPY --from=builder /src/build/signaling/pong_signaling /app/pong_signaling

# Copy the downloaded cloudflared binary from the builder stage
COPY --from=builder /src/build/signaling/cloudflared /app/cloudflared

COPY entrypoint.sh /app/entrypoint.sh

RUN chmod +x /app/cloudflared /app/entrypoint.sh

# Port 80  — needed for Let's Encrypt HTTP-01 challenge
# Port 9000 — signaling server (ws:// or wss://)
EXPOSE 80 9000

# Set DOMAIN and CERT_EMAIL at runtime to enable TLS:
#
#   docker run -d -p 80:80 -p 9000:9000 \
#     -v letsencrypt:/etc/letsencrypt \
#     -e DOMAIN=lukarbonite.zapto.org \
#     -e CERT_EMAIL=you@example.com \
#     pong-signaling
#
# Omit DOMAIN to run plain ws:// (no port 80 needed).
# The named volume keeps the cert across container restarts.

ENTRYPOINT ["/app/entrypoint.sh"]
