FROM ubuntu:24.04
WORKDIR /root/riscv

RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g' \
    /etc/apt/sources.list.d/ubuntu.sources \
    && apt clean \
    && apt update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        vim \
        gdb-multiarch \
        qemu-system-riscv64 \
        make \
        wget \
        file \
        locales \
        binutils \
        pipx \
    && pipx install pygments \
    && rm -rf /var/lib/apt/lists/*

# 启用 UTF-8 编码，修复 GEF 提示符
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# 安装 GDB插件GEF
RUN wget -O ~/.gdbinit-gef.py -q https://gef.blah.cat/py \
    && echo source ~/.gdbinit-gef.py >> ~/.gdbinit

COPY . .

#rustsbi所需gcc和rust编译环境暂不放进image