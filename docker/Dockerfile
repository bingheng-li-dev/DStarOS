FROM ubuntu:24.04
WORKDIR /root/riscv

# 基础镜像还没有 ca-certificates，这里只能用 http；软件包由 apt 的 GPG 签名校验
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g' \
    /etc/apt/sources.list.d/ubuntu.sources \
    && apt clean \
    && apt update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        wget \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# musl 交叉工具链（riscv64 / rv64gc / lp64d）。
# 用途：编用户态程序与 BusyBox；内核仍由 bind mount 进来的 kendryte 裸机工具链编译。
ARG MUSL_TC=riscv64-lp64d--musl--stable-2026.08-1
ARG MUSL_TC_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/riscv64-lp64d/tarballs
RUN wget -q "$MUSL_TC_URL/$MUSL_TC.tar.xz" \
    && wget -q "$MUSL_TC_URL/$MUSL_TC.sha256" \
    && sha256sum -c "$MUSL_TC.sha256" \
    && mkdir -p /root/riscv/toolchain-musl \
    && tar -xf "$MUSL_TC.tar.xz" -C /root/riscv/toolchain-musl --strip-components=1 \
    && if [ -x /root/riscv/toolchain-musl/relocate-sdk.sh ]; then \
           /root/riscv/toolchain-musl/relocate-sdk.sh; \
       fi \
    && rm -f "$MUSL_TC.tar.xz" "$MUSL_TC.sha256"
ENV PATH=/root/riscv/toolchain-musl/bin:$PATH

# build-essential / libncurses-dev 是给 BusyBox 用的：它要用宿主 gcc 编自己的
# 构建工具，menuconfig 要 ncurses。git/curl 用于拉源码
RUN apt update \
    && apt-get install -y --no-install-recommends \
        vim \
        less \
        gdb-multiarch \
        qemu-system-riscv64 \
        make \
        file \
        locales \
        binutils \
        pipx \
        git \
        curl \
        patch \
        bzip2 \
        build-essential \
        libncurses-dev \
        dosfstools \
        mtools \
    && pipx install pygments \
    && rm -rf /var/lib/apt/lists/*

# 启用 UTF-8 编码，修复 GEF 提示符
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# 安装 GDB插件GEF
# 只是调试辅助：下载失败时跳过，不让整个镜像构建失败
RUN if wget -q --tries=2 --timeout=20 -O ~/.gdbinit-gef.py https://gef.blah.cat/py; then \
        echo source ~/.gdbinit-gef.py >> ~/.gdbinit; \
    else \
        rm -f ~/.gdbinit-gef.py; \
        echo "GEF download failed, skipped"; \
    fi
