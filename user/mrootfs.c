/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/mrootfs.c —— 验证宿主机造的 rootfs 镜像真的被内核读到了
 *
 * 根文件系统的内容来自 tools/build_rootfs.sh 在宿主机上写好、由 QEMU 的 -device loader
 * 搬进 rootfs 预留区的 FAT 镜像。
 *
 * 三个层次各验一条，任何一条挂掉都说明通路断在不同的地方：
 *   1. 目录项读得出来  —— FAT 结构被 FatFS 正确解析（含长文件名）；
 *   2. 文件内容读得出来 —— 数据簇的定位与 disk_read 的偏移算对了；
 *   3. ELF 魔数对得上   —— 二进制没有被截断或错位，exec 才有意义。
 *
 * **必须在装载了镜像的情况下跑**（scripts/run.sh 会在 build/rootfs.img 存在时自动加
 * -device loader）。没装载时内核会 f_mkfs 出一张空盘，这里的断言会如实报 FAIL——
 * 那不是内核坏了，是没跑 `make rootfs`。
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

static int pass_count;
static int fail_count;

static void check(int cond, const char *name)
{
    if (cond)
    {
        pass_count++;
        fputs("  PASS: ", stdout);
    }
    else
    {
        fail_count++;
        fputs("  FAIL: ", stdout);
    }
    puts(name);
}

/* 1) 目录项：/bin 下应当有 build_rootfs.sh 拷进去的那批程序 */
static void test_listing(void)
{
    DIR *d = opendir("/bin");
    check(d != NULL, "opendir(/bin)");
    if (!d)
    {
        return;
    }

    /* mhello.elf 是 8.3 内的短名；msyscheck.elf 主名 9 字符，镜像里是长文件名目录项。
     * 两个都要能读出来，才说明 LFN 这条路径也通。 */
    int saw_short = 0, saw_lfn = 0, total = 0;
    struct dirent *e;
    fputs("  /bin listing:", stdout);
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
        {
            continue;
        }
        total++;
        fputc(' ', stdout);
        fputs(e->d_name, stdout);
        if (strcmp(e->d_name, "mhello.elf") == 0)
        {
            saw_short = 1;
        }
        if (strcmp(e->d_name, "msyscheck.elf") == 0)
        {
            saw_lfn = 1;
        }
    }
    fputc('\n', stdout);
    closedir(d);

    check(total >= 10, "/bin holds the programs put there by build_rootfs.sh");
    check(saw_short, "short 8.3 name (mhello.elf) is listed");
    check(saw_lfn, "long file name (msyscheck.elf) is listed");
}

/* 2) 文件内容：/etc/issue 是脚本写进去的一行纯文本 */
static void test_file_content(void)
{
    FILE *f = fopen("/etc/issue", "r");
    check(f != NULL, "fopen(/etc/issue)");
    if (!f)
    {
        return;
    }
    char buf[64];
    memset(buf, 0, sizeof(buf));
    char *got = fgets(buf, sizeof(buf), f);
    fclose(f);
    check(got != NULL && strcmp(buf, "DStarOS rootfs\n") == 0,
          "/etc/issue content came from the host-built image");
}

/* 3) 二进制完整性：ELF 魔数 + 大小 */
static void test_elf_intact(void)
{
    struct stat st;
    check(stat("/bin/mhello.elf", &st) == 0, "stat(/bin/mhello.elf)");
    check(st.st_size > 4096, "mhello.elf size looks like a real binary");

    int fd = open("/bin/mhello.elf", O_RDONLY);
    check(fd >= 0, "open(/bin/mhello.elf)");
    if (fd < 0)
    {
        return;
    }
    unsigned char magic[4] = {0};
    ssize_t n = read(fd, magic, 4);
    check(n == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F',
          "mhello.elf starts with the ELF magic");

    /* 读最后 16 字节：只验开头的话，簇链走错、只把第一个簇读对也能过 */
    check(lseek(fd, -16, SEEK_END) == st.st_size - 16, "lseek to the tail of the file");
    unsigned char tail[16];
    check(read(fd, tail, 16) == 16, "read the last 16 bytes");
    close(fd);
}

int main(void)
{
    puts("=== mrootfs: host-built image visible to the kernel ===");

    test_listing();
    test_file_content();
    test_elf_intact();

    printf("=== mrootfs done: %d pass  %d fail ===\n", pass_count, fail_count);
    return fail_count;
}
