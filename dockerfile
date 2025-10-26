FROM debian:13.1-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    python3 \
    python3-pip \
    git \
    curl \
    npm \
    nodejs \
    && rm -rf /var/lib/apt/lists/*


WORKDIR /opt
RUN git clone https://github.com/emscripten-core/emsdk.git && \
    cd emsdk && \
    ./emsdk install latest && \
    ./emsdk activate latest

ENV PATH="/opt/emsdk:/opt/emsdk/upstream/emscripten:${PATH}"

# Create symlink for hardcoded path in Makefile
RUN ln -s /opt/emsdk/upstream/emscripten /usr/lib/emscripten

WORKDIR /app

COPY package*.json ./
RUN npm install

# Install esbuild globally because it's called globally in Makefile
RUN npm install -g esbuild

COPY . .

EXPOSE 8080

