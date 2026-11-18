# Imagen que imita una PC del laboratorio: Ubuntu 20.04 (Linux Mint 20.3 se basa en esto),
# g++ 9, glibc 2.31. El contexto de build es la raiz de DSM-System (ver docker-compose.yml).
FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        make \
        git \
        iproute2 \
        iputils-ping \
        netcat-openbsd \
        procps \
        tcpdump \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /DSM-System
COPY . .
RUN make clean

CMD ["sleep", "infinity"]
