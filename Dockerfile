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

# Install CA certificates (Required for MbedTLS/WebRTC and cloudflared to make secure connections)
RUN apt-get update && apt-get install -y \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the compiled signaling server from the builder stage
COPY --from=builder /src/build/signaling/pong_signaling /app/pong_signaling

# Copy the downloaded cloudflared binary from the builder stage
COPY --from=builder /src/build/signaling/cloudflared /app/cloudflared

# Ensure cloudflared has execute permissions
RUN chmod +x /app/cloudflared

# Expose the port the signaling server listens on locally
EXPOSE 9000

# Run the signaling server
CMD ["./pong_signaling"]