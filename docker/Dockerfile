FROM ubuntu:24.04

# Install the compiler and CMake needed to build the gateway in the image.
RUN apt-get update \
    && apt-get install -y build-essential cmake \
    && rm -rf /var/lib/apt/lists/*

# Keep the source and generated build files inside the application directory.
WORKDIR /app
COPY . .

# Configure and compile the C++ executable during the image build.
RUN cmake -S . -B build \
    && cmake --build build

# Document the HTTP and consumer-protocol ports used by the default configuration.
EXPOSE 8080 9090

# Start the compiled gateway when a container is launched.
CMD ["./build/message_gateway"]
