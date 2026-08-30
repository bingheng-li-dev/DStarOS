#!/bin/bash
# 把 user/<name>.elf 转成内核里的字节数组 src/kernel/user_<name>_elf.c。
# 用法（在 user/ 目录下）：bash gen_elf_array.sh memtest
# 手工转录十六进制曾多次漏字节，一律走这个脚本。
set -e

name="$1"
if [ -z "$name" ]; then
    echo "usage: $0 <name>   # 例如 memtest，对应 user/memtest.elf" >&2
    exit 1
fi

src="$name.elf"
dst="../src/kernel/user_${name}_elf.c"

{
    echo "/* U 态 ${name} 测试程序（完整 ELF64，已 strip）。"
    echo " * 源码 user/${name}.c，经 user/Makefile + user/user.ld 产出，交 elf_load() 解析。"
    echo " * 本文件由 user/gen_elf_array.sh 生成，勿手工编辑。 */"
    echo "const unsigned char user_${name}_elf[] = {"
    od -An -tx1 -v "$src" | awk '{
        printf "   ";
        for (i = 1; i <= NF; i++) { printf " 0x%s,", $i }
        printf "\n"
    }'
    echo "};"
    echo "const unsigned long user_${name}_elf_len = sizeof(user_${name}_elf);"
} > "$dst"

echo "generated $dst ($(wc -c < "$src") bytes)"
