FROM priyatam19/compiler-opts-lli-stats:llvm21

USER root

#Install the generic tools
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
    linux-tools-generic linux-tools-common && \
    rm -rf /var/lib/apt/lists/*

RUN rm -f /usr/bin/perf && \
    ln -s $(find /usr/lib/linux-tools -name perf | head -n 1) /usr/bin/perf

WORKDIR /work