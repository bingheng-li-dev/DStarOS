#include "sdmmc.h"

#if defined(VF2)

#include "periph_layout.h"
#include "memtype.h"
#include "console.h"
#include "errorcode.h"
#include "stringops.h"

/* DesignWare MMC 寄存器偏移。VERID >= 2.40a 时数据 FIFO 在 0x200（本板实测 2.90a）。 */
#define SDMMC_CTRL                  0x000
#define SDMMC_CLKDIV                0x008
#define SDMMC_CLKENA                0x010
#define SDMMC_CTYPE                 0x018
#define SDMMC_BLKSIZ                0x01c
#define SDMMC_BYTCNT                0x020
#define SDMMC_CMDARG                0x028
#define SDMMC_CMD                   0x02c
#define SDMMC_RESP0                 0x030
#define SDMMC_RINTSTS               0x044
#define SDMMC_STATUS                0x048
#define SDMMC_BMOD                  0x080
#define SDMMC_DATA                  0x200

#define SDMMC_CTRL_FIFO_RESET       (1U << 1)
#define SDMMC_CTRL_DMA_RESET        (1U << 2)
#define SDMMC_CTRL_DMA_ENABLE       (1U << 5)
#define SDMMC_CTRL_USE_IDMAC        (1U << 25)
#define SDMMC_BMOD_FB               (1U << 1)
#define SDMMC_BMOD_DE               (1U << 7)

#define SDMMC_CMD_START             (1U << 31)
#define SDMMC_CMD_USE_HOLD_REG      (1U << 29)
#define SDMMC_CMD_PRV_DAT_WAIT      (1U << 13)
#define SDMMC_CMD_DATA_EXP          (1U << 9)
#define SDMMC_CMD_WRITE             (1U << 10)
#define SDMMC_CMD_RESP_CRC          (1U << 8)
#define SDMMC_CMD_RESP_EXP          (1U << 6)
#define SDMMC_CMD_SEND_STOP         (1U << 12)
#define SDMMC_CMD_STOP_ABORT        (1U << 14)

#define SDMMC_INT_RE                (1U << 1)
#define SDMMC_INT_CMD_DONE          (1U << 2)
#define SDMMC_INT_DTO               (1U << 3)
#define SDMMC_INT_RCRC              (1U << 6)
#define SDMMC_INT_DCRC              (1U << 7)
#define SDMMC_INT_RTO               (1U << 8)
#define SDMMC_INT_DRTO              (1U << 9)
#define SDMMC_INT_HTO               (1U << 10)
#define SDMMC_INT_FRUN              (1U << 11)
#define SDMMC_INT_HLE               (1U << 12)
#define SDMMC_INT_SBE               (1U << 13)
#define SDMMC_INT_ACD               (1U << 14)
#define SDMMC_INT_EBE               (1U << 15)
#define SDMMC_INT_CMD_ERR           (SDMMC_INT_RE | SDMMC_INT_RCRC | SDMMC_INT_HLE)
#define SDMMC_INT_DATA_ERR          (SDMMC_INT_DCRC | SDMMC_INT_DRTO | SDMMC_INT_HTO | \
                                     SDMMC_INT_FRUN | SDMMC_INT_SBE | SDMMC_INT_EBE)

#define SDMMC_STATUS_DATA_BUSY      (1U << 9)
#define SDMMC_STATUS_FCNT(s)        (((s) >> 17) & 0x1fff)

#define SD_CMD_STOP_TRANSMISSION    12
#define SD_CMD_SET_BLOCKLEN         16
#define SD_CMD_READ_SINGLE_BLOCK    17
#define SD_CMD_READ_MULTIPLE_BLOCK  18
#define SD_CMD_WRITE_BLOCK          24
#define SD_CMD_WRITE_MULTIPLE_BLOCK 25
#define SD_BLOCK_SIZE               512
#define SD_BLOCK_WORDS              (SD_BLOCK_SIZE / 4)
#define SD_R1_STATE(r1)             (((r1) >> 9) & 0xf)
#define SD_R1_STATE_TRAN            4
/* R1 里表示命令执行出错的位：OUT_OF_RANGE .. ERROR（不含 CARD_IS_LOCKED 这类状态位） */
#define SD_R1_ERROR_MASK            0xfdf80000U
/* 数据 FIFO 深度，单位是字。实测自 /soc/sdio1@16020000 的 fifo-depth */
#define SDMMC_FIFO_DEPTH            32U

/* 只要远大于一条命令 / 一个块的正常耗时即可 */
#define SDMMC_SPIN_LIMIT            20000000U
/* 自动发出的 CMD12 在数据结束后很快完成；等不到多半是数据阶段出错、控制器根本没发 */
#define SDMMC_ACD_SPIN_LIMIT        1000000U
/* 一次多块传输的块数上限，限制的是单次 PIO 忙等的时长 */
#define SDMMC_MAX_XFER_BLOCKS       128U

/* SDHC/SDXC 按块寻址，SDSC 按字节寻址。接手 U-Boot 的现成状态就拿不到 OCR，
 * 默认按块寻址，由 sdmmc_probe() 按分区首扇区的签名验证。 */
static bool sdmmc_block_addressing = true;
static uint32_t sdmmc_last_rintsts;
static bool sdmmc_multiblock = true;
static sdmmc_stats_t sdmmc_stats;

static inline uint32_t sdmmc_read(uint32_t off)
{
    return *(volatile uint32_t *)(pa_to_kva((phyAddr_t)SDMMC_PHYS_BASE) + off);
}

static inline void sdmmc_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(pa_to_kva((phyAddr_t)SDMMC_PHYS_BASE) + off) = val;
}

/**
 * @brief 忙等某个寄存器的指定位全部清零
 * @param[in] off  寄存器偏移
 * @param[in] mask 要等待清零的位
 * @retval true  在上限内清零
 * @retval false 超时
 */
static bool sdmmc_wait_clear(uint32_t off, uint32_t mask)
{
    for (uint32_t n = 0; n < SDMMC_SPIN_LIMIT; n++)
    {
        if ((sdmmc_read(off) & mask) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把数据通路从 IDMAC（DMA）切到 FIFO（PIO）
 * @retval ENO0_NO_ERROR  切换完成
 * @retval ENO30_TIMEDOUT DMA 复位位迟迟不自清
 * @details U-Boot 离开时 CTRL.USE_IDMAC = 1（实测 CTRL=0x02000000）。不清掉的话数据走
 *   DMA 描述符链、FIFO 永远是空的，表现是"命令成功但读不到数据"。
 *   做法照 Linux dw_mci_idmac_stop_dma()：清 USE_IDMAC、置自清的 DMA_RESET、
 *   再清 BMOD 里的 DE/FB。
 * @note 时钟、pinmux、卡的选中状态全部沿用 U-Boot 留下的，这里不碰。
 */
static int sdmmc_use_pio(void)
{
    uint32_t ctrl = sdmmc_read(SDMMC_CTRL);
    ctrl &= ~(SDMMC_CTRL_USE_IDMAC | SDMMC_CTRL_DMA_ENABLE);
    sdmmc_write(SDMMC_CTRL, ctrl | SDMMC_CTRL_DMA_RESET);
    if (!sdmmc_wait_clear(SDMMC_CTRL, SDMMC_CTRL_DMA_RESET))
    {
        return ENO30_TIMEDOUT;
    }
    sdmmc_write(SDMMC_BMOD, sdmmc_read(SDMMC_BMOD) & ~(SDMMC_BMOD_DE | SDMMC_BMOD_FB));
    return ENO0_NO_ERROR;
}

/**
 * @brief 发一条命令并等它结束（不等数据）
 * @param[in]  idx   命令号
 * @param[in]  arg   命令参数
 * @param[in]  flags 响应 / 数据相关的 CMD 寄存器位
 * @param[out] resp  非 NULL 时写入 RESP0（短响应）
 * @retval ENO0_NO_ERROR  命令完成且无命令层错误
 * @retval ENO4_BUSY      卡的数据线一直忙
 * @retval ENO30_TIMEDOUT 控制器没报命令完成，或卡无响应（RTO）
 * @retval ENO29_IO       响应错误 / 响应 CRC 错 / 硬件锁写错误
 * @details 写 CMD 之前先清空 RINTSTS（写 1 清），之后的完成与错误位都只属于这条命令；
 *   带数据的命令，数据阶段的位留给调用者去读，这里不再清。
 *   USE_HOLD_REG 在高速模式下是必需的，U-Boot 同样总是置上。
 *   带 STOP_ABORT 的命令（出错后手动补发的 CMD12）是用来打断正在进行的数据传输的，
 *   所以既不先等数据线空闲，也不置 PRV_DAT_WAIT。
 */
static int sdmmc_send_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *resp)
{
    bool abort = (flags & SDMMC_CMD_STOP_ABORT) != 0;
    if (!abort && !sdmmc_wait_clear(SDMMC_STATUS, SDMMC_STATUS_DATA_BUSY))
    {
        return ENO4_BUSY;
    }
    sdmmc_write(SDMMC_RINTSTS, 0xffffffffU);
    sdmmc_write(SDMMC_CMDARG, arg);
    sdmmc_write(SDMMC_CMD, SDMMC_CMD_START | SDMMC_CMD_USE_HOLD_REG |
                           (abort ? 0 : SDMMC_CMD_PRV_DAT_WAIT) | flags | idx);

    uint32_t st = 0;
    for (uint32_t n = 0; n < SDMMC_SPIN_LIMIT; n++)
    {
        st = sdmmc_read(SDMMC_RINTSTS);
        if ((st & SDMMC_INT_CMD_DONE) != 0)
        {
            break;
        }
    }
    sdmmc_last_rintsts = st;

    if ((st & SDMMC_INT_CMD_DONE) == 0 || (st & SDMMC_INT_RTO) != 0)
    {
        return ENO30_TIMEDOUT;
    }
    if ((st & SDMMC_INT_CMD_ERR) != 0)
    {
        return ENO29_IO;
    }
    if (resp != NULL)
    {
        *resp = sdmmc_read(SDMMC_RESP0);
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 为一次数据传输复位 FIFO，并设置块大小与总字节数
 * @param[in] count 块数
 * @retval ENO0_NO_ERROR  就绪
 * @retval ENO30_TIMEDOUT FIFO 复位位迟迟不自清
 */
static int sdmmc_prepare_data(uint32_t count)
{
    sdmmc_write(SDMMC_CTRL, sdmmc_read(SDMMC_CTRL) | SDMMC_CTRL_FIFO_RESET);
    if (!sdmmc_wait_clear(SDMMC_CTRL, SDMMC_CTRL_FIFO_RESET))
    {
        return ENO30_TIMEDOUT;
    }
    sdmmc_write(SDMMC_BLKSIZ, SD_BLOCK_SIZE);
    sdmmc_write(SDMMC_BYTCNT, count * SD_BLOCK_SIZE);
    return ENO0_NO_ERROR;
}

/**
 * @brief 结束一次多块传输：等控制器自动发出的 CMD12 完成，等不到就手动补发
 * @param[in] st 数据阶段最后一次读到的 RINTSTS（ACD 可能已经在里面）
 * @details 卡收到 CMD12 之前一直停在收发数据态，不接受任何读写命令，所以不论数据阶段成败，
 *   这条命令都必须发出去。控制器只在数据正常传完时自动发（完成时置 ACD）。
 *   CMD12 的响应是 R1b，发完还要等卡不忙。
 * @note 手动补发的次数计入 cmd12_manual；数据正常而它仍在增长，说明这个控制器不报 ACD。
 */
static void sdmmc_finish_multi(uint32_t st)
{
    for (uint32_t n = 0; (st & SDMMC_INT_ACD) == 0 && n < SDMMC_ACD_SPIN_LIMIT; n++)
    {
        st = sdmmc_read(SDMMC_RINTSTS);
    }
    if ((st & SDMMC_INT_ACD) == 0)
    {
        sdmmc_stats.cmd12_manual++;
        (void)sdmmc_send_cmd(SD_CMD_STOP_TRANSMISSION, 0,
                             SDMMC_CMD_RESP_EXP | SDMMC_CMD_RESP_CRC | SDMMC_CMD_STOP_ABORT, NULL);
    }
    (void)sdmmc_wait_clear(SDMMC_STATUS, SDMMC_STATUS_DATA_BUSY);
}

/**
 * @brief 读连续若干块（PIO）：1 块用 CMD17，多块用 CMD18 并由控制器自动发 CMD12
 * @param[in]  lba   起始块号
 * @param[out] buf   至少 count * 512 字节
 * @param[in]  count 块数，1..SDMMC_MAX_XFER_BLOCKS
 * @retval ENO0_NO_ERROR  读满且无数据层错误
 * @retval ENO29_IO       数据 CRC / 超时 / FIFO 溢出等错误，或读到的字数不足
 * @retval ENO30_TIMEDOUT 等不到数据传输结束（DTO）
 * @details FIFO 深度 32 个字（128 字节），一块 128 个字，**必然要分多次搬**：
 *   每轮读 STATUS 里的 FIFO 计数，有多少取多少；DTO 置位且 FIFO 取空才算结束。
 *   FIFO 里是小端 32 位字，按字节顺序拆回缓冲区。
 *
 *   多块时置 SEND_STOP（与 U-Boot 的 dwmmc 驱动相同），收尾见 sdmmc_finish_multi()。
 */
static int sdmmc_read_xfer(uint64_t lba, uint8_t *buf, uint32_t count)
{
    uint32_t arg = sdmmc_block_addressing ? (uint32_t)lba : (uint32_t)(lba * SD_BLOCK_SIZE);
    bool multi = count > 1;

    int ret = sdmmc_prepare_data(count);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    if (multi)
    {
        sdmmc_stats.cmd18++;
    }
    else
    {
        sdmmc_stats.cmd17++;
    }
    ret = sdmmc_send_cmd(multi ? SD_CMD_READ_MULTIPLE_BLOCK : SD_CMD_READ_SINGLE_BLOCK, arg,
                         SDMMC_CMD_RESP_EXP | SDMMC_CMD_RESP_CRC | SDMMC_CMD_DATA_EXP |
                         (multi ? SDMMC_CMD_SEND_STOP : 0), NULL);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }

    const uint32_t total = count * SD_BLOCK_WORDS;
    const uint64_t limit = (uint64_t)SDMMC_SPIN_LIMIT * count;
    uint32_t got = 0;
    uint32_t st = 0;
    for (uint64_t n = 0; n < limit; n++)
    {
        st = sdmmc_read(SDMMC_RINTSTS);
        if ((st & SDMMC_INT_DATA_ERR) != 0)
        {
            break;
        }
        for (uint32_t cnt = SDMMC_STATUS_FCNT(sdmmc_read(SDMMC_STATUS));
             cnt > 0 && got < total; cnt--, got++)
        {
            uint32_t w = sdmmc_read(SDMMC_DATA);
            uint8_t *p = buf + (uint64_t)got * 4;
            p[0] = (uint8_t)w;
            p[1] = (uint8_t)(w >> 8);
            p[2] = (uint8_t)(w >> 16);
            p[3] = (uint8_t)(w >> 24);
        }
        if ((st & SDMMC_INT_DTO) != 0 && SDMMC_STATUS_FCNT(sdmmc_read(SDMMC_STATUS)) == 0)
        {
            break;
        }
    }
    sdmmc_last_rintsts = st;
    if (multi)
    {
        sdmmc_finish_multi(st);
    }
    sdmmc_write(SDMMC_RINTSTS, 0xffffffffU);

    if ((st & SDMMC_INT_DATA_ERR) != 0)
    {
        return ENO29_IO;
    }
    if ((st & SDMMC_INT_DTO) == 0)
    {
        return ENO30_TIMEDOUT;
    }
    if (got != total)
    {
        return ENO29_IO;
    }
    sdmmc_stats.blocks_read += count;
    return ENO0_NO_ERROR;
}

/**
 * @brief 接手 U-Boot 留下的 SD 卡：切 PIO，并确认卡处于传输态
 * @retval ENO0_NO_ERROR 可以直接读块
 * @retval ENO30_TIMEDOUT / ENO29_IO / ENO4_BUSY 见 sdmmc_send_cmd()
 * @details 不重走 CMD0→ACMD41→CMD2→CMD3→CMD7 初始化序列，也不动时钟：
 *   U-Boot 最后一条命令是 CMD17（实测 STATUS 的 response_index = 17），卡大概率仍被选中。
 *   用 CMD16（SET_BLOCKLEN）验证——它只在传输态被接受，处于待机态的卡不会应答，
 *   所以一条命令就能区分"能直接接手"与"要重新初始化"。
 */
int sdmmc_init(void)
{
    int ret = sdmmc_use_pio();
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    uint32_t r1 = 0;
    ret = sdmmc_send_cmd(SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE,
                         SDMMC_CMD_RESP_EXP | SDMMC_CMD_RESP_CRC, &r1);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    return (SD_R1_STATE(r1) == SD_R1_STATE_TRAN) ? ENO0_NO_ERROR : ENO4_BUSY;
}

/**
 * @brief 读连续若干块
 * @param[in]  lba   起始块号
 * @param[out] buf   至少 count * 512 字节
 * @param[in]  count 块数
 * @return 同 sdmmc_read_xfer()，遇到第一段失败即返回
 * @note 按 SDMMC_MAX_XFER_BLOCKS 切段，每段一次传输；关掉多块（仅调试对比用）时逐块 CMD17。
 */
int sdmmc_read_blocks(uint64_t lba, uint8_t *buf, uint32_t count)
{
    while (count > 0)
    {
        uint32_t n = !sdmmc_multiblock ? 1 : (count > SDMMC_MAX_XFER_BLOCKS ? SDMMC_MAX_XFER_BLOCKS : count);
        int ret = sdmmc_read_xfer(lba, buf, n);
        if (ret != ENO0_NO_ERROR)
        {
            return ret;
        }
        lba += n;
        buf += (uint64_t)n * SD_BLOCK_SIZE;
        count -= n;
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 取已发出的读写命令计数
 * @param[out] out 计数快照
 * @note 不加锁：驱动的调用本身由 VFS 大锁串行化，这里只是调试统计。
 */
void sdmmc_get_stats(sdmmc_stats_t *out)
{
    *out = sdmmc_stats;
}

#if DEBUG_SDMMC_PROBE
/**
 * @brief 切换多块传输，只供板上对比单块 / 多块的耗时与命令数
 * @param[in] enable false 时读写都退回逐块 CMD17 / CMD24
 */
void sdmmc_set_multiblock(bool enable)
{
    sdmmc_multiblock = enable;
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/**
 * @brief SD 读通路的上板探针：逐步打印，坏在哪一步就停在哪一步
 * @details 1. 切 PIO 前后的 CTRL / BMOD 等寄存器；
 *   2. CMD16：应答与卡状态（4 = 传输态）；
 *   3. 读 LBA 0：签名 55aa，分区类型 0xee 表示 GPT，否则按 MBR 取第一个分区；
 *   4. 读第一个分区首扇区：签名 55aa 且在 0x36（FAT12/16）或 0x52（FAT32）处有 "FAT"。
 *      按块寻址对不上就换字节寻址再读一次（SDSC 卡）。
 * @note **只发读命令**。必须在 trap_init() 之后调用。
 */
void sdmmc_probe(void)
{
    static uint8_t blk[SD_BLOCK_SIZE];

    printf("sdmmc: ctrl=0x%08x bmod=0x%08x status=0x%08x clkena=0x%08x clkdiv=0x%08x ctype=0x%08x\n",
           sdmmc_read(SDMMC_CTRL), sdmmc_read(SDMMC_BMOD), sdmmc_read(SDMMC_STATUS),
           sdmmc_read(SDMMC_CLKENA), sdmmc_read(SDMMC_CLKDIV), sdmmc_read(SDMMC_CTYPE));

    int ret = sdmmc_use_pio();
    printf("sdmmc: pio ret=%d ctrl=0x%08x bmod=0x%08x\n",
           ret, sdmmc_read(SDMMC_CTRL), sdmmc_read(SDMMC_BMOD));
    if (ret != ENO0_NO_ERROR)
    {
        return;
    }

    uint32_t r1 = 0;
    ret = sdmmc_send_cmd(SD_CMD_SET_BLOCKLEN, SD_BLOCK_SIZE,
                         SDMMC_CMD_RESP_EXP | SDMMC_CMD_RESP_CRC, &r1);
    printf("sdmmc: CMD16 ret=%d rintsts=0x%08x r1=0x%08x state=%u (4=tran)\n",
           ret, sdmmc_last_rintsts, r1, SD_R1_STATE(r1));
    if (ret != ENO0_NO_ERROR)
    {
        return;
    }

    ret = sdmmc_read_xfer(0, blk, 1);
    printf("sdmmc: lba0 ret=%d rintsts=0x%08x sig=%02x%02x ptype=0x%02x\n",
           ret, sdmmc_last_rintsts, blk[510], blk[511], blk[450]);
    if (ret != ENO0_NO_ERROR)
    {
        return;
    }
    printf("sdmmc: lba0[0..15] =");
    for (int i = 0; i < 16; i++)
    {
        printf(" %02x", blk[i]);
    }
    printf("\n");

    uint64_t start;
    if (blk[450] == 0xee)
    {
        ret = sdmmc_read_xfer(1, blk, 1);
        printf("sdmmc: gpt header ret=%d sig=%c%c%c%c%c%c%c%c\n",
               ret, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
        if (ret != ENO0_NO_ERROR)
        {
            return;
        }
        uint64_t entries_lba = le64(blk + 72);
        ret = sdmmc_read_xfer(entries_lba, blk, 1);
        start = le64(blk + 32);
        printf("sdmmc: gpt entries lba=%lu ret=%d part1 first_lba=%lu\n",
               (unsigned long)entries_lba, ret, (unsigned long)start);
        if (ret != ENO0_NO_ERROR)
        {
            return;
        }
    }
    else
    {
        start = le32(blk + 454);
        printf("sdmmc: mbr part1 start_lba=%lu sectors=%u\n",
               (unsigned long)start, le32(blk + 458));
    }

    for (int attempt = 0; attempt < 2; attempt++)
    {
        ret = sdmmc_read_xfer(start, blk, 1);
        bool fat = blk[510] == 0x55 && blk[511] == 0xaa &&
                   (memcmp(blk + 0x36, "FAT", 3) == 0 || memcmp(blk + 0x52, "FAT", 3) == 0);
        printf("sdmmc: part1 lba=%lu addr=%s ret=%d rintsts=0x%08x sig=%02x%02x oem=%c%c%c%c%c%c%c%c fat=%d\n",
               (unsigned long)start, sdmmc_block_addressing ? "block" : "byte", ret, sdmmc_last_rintsts,
               blk[510], blk[511], blk[3], blk[4], blk[5], blk[6], blk[7], blk[8], blk[9], blk[10],
               fat ? 1 : 0);
        if (fat || ret != ENO0_NO_ERROR || !sdmmc_block_addressing)
        {
            break;
        }
        sdmmc_block_addressing = false;
    }
}
#endif

/**
 * @brief 写连续若干块（PIO）：1 块用 CMD24，多块用 CMD25 并由控制器自动发 CMD12
 * @param[in] lba   起始块号
 * @param[in] buf   count * 512 字节
 * @param[in] count 块数，1..SDMMC_MAX_XFER_BLOCKS
 * @retval ENO0_NO_ERROR  全部写出、卡回的 CRC 状态正确，且卡已编程完成
 * @retval ENO29_IO       R1 报错，或数据层错误（写方向的 DCRC 表示卡回的 CRC 状态令牌出错）
 * @retval ENO30_TIMEDOUT 等不到传输结束，或卡一直忙
 * @details 与读对称：FIFO 深度 32 个字，按 STATUS 里的 FIFO 计数算剩余空间，有多少填多少。
 *   DTO 之后卡还在内部编程，数据线保持忙——**必须等忙结束才算写完**，
 *   否则紧接着断电，这些块可能根本没落盘。
 */
static int sdmmc_write_xfer(uint64_t lba, const uint8_t *buf, uint32_t count)
{
    uint32_t arg = sdmmc_block_addressing ? (uint32_t)lba : (uint32_t)(lba * SD_BLOCK_SIZE);
    bool multi = count > 1;

    int ret = sdmmc_prepare_data(count);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    if (multi)
    {
        sdmmc_stats.cmd25++;
    }
    else
    {
        sdmmc_stats.cmd24++;
    }
    uint32_t r1 = 0;
    ret = sdmmc_send_cmd(multi ? SD_CMD_WRITE_MULTIPLE_BLOCK : SD_CMD_WRITE_BLOCK, arg,
                         SDMMC_CMD_RESP_EXP | SDMMC_CMD_RESP_CRC | SDMMC_CMD_DATA_EXP |
                         SDMMC_CMD_WRITE | (multi ? SDMMC_CMD_SEND_STOP : 0), &r1);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    if ((r1 & SD_R1_ERROR_MASK) != 0)
    {
        return ENO29_IO;
    }

    const uint32_t total = count * SD_BLOCK_WORDS;
    const uint64_t limit = (uint64_t)SDMMC_SPIN_LIMIT * count;
    uint32_t put = 0;
    uint32_t st = 0;
    for (uint64_t n = 0; n < limit; n++)
    {
        st = sdmmc_read(SDMMC_RINTSTS);
        if ((st & SDMMC_INT_DATA_ERR) != 0)
        {
            break;
        }
        for (uint32_t space = SDMMC_FIFO_DEPTH - SDMMC_STATUS_FCNT(sdmmc_read(SDMMC_STATUS));
             space > 0 && put < total; space--, put++)
        {
            const uint8_t *b = buf + (uint64_t)put * 4;
            sdmmc_write(SDMMC_DATA, (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        }
        if ((st & SDMMC_INT_DTO) != 0 && put == total)
        {
            break;
        }
    }
    sdmmc_last_rintsts = st;
    if (multi)
    {
        sdmmc_finish_multi(st);
    }
    sdmmc_write(SDMMC_RINTSTS, 0xffffffffU);

    if ((st & SDMMC_INT_DATA_ERR) != 0 || put != total)
    {
        return ENO29_IO;
    }
    if ((st & SDMMC_INT_DTO) == 0)
    {
        return ENO30_TIMEDOUT;
    }
    if (!sdmmc_wait_clear(SDMMC_STATUS, SDMMC_STATUS_DATA_BUSY))
    {
        return ENO30_TIMEDOUT;
    }
    sdmmc_stats.blocks_written += count;
    return ENO0_NO_ERROR;
}

/**
 * @brief 写连续若干块
 * @param[in] lba   起始块号
 * @param[in] buf   count * 512 字节
 * @param[in] count 块数
 * @return 同 sdmmc_write_xfer()，遇到第一段失败即返回
 * @note 切段方式与 sdmmc_read_blocks() 相同。
 */
int sdmmc_write_blocks(uint64_t lba, const uint8_t *buf, uint32_t count)
{
    while (count > 0)
    {
        uint32_t n = !sdmmc_multiblock ? 1 : (count > SDMMC_MAX_XFER_BLOCKS ? SDMMC_MAX_XFER_BLOCKS : count);
        int ret = sdmmc_write_xfer(lba, buf, n);
        if (ret != ENO0_NO_ERROR)
        {
            return ret;
        }
        lba += n;
        buf += (uint64_t)n * SD_BLOCK_SIZE;
        count -= n;
    }
    return ENO0_NO_ERROR;
}

#if DEBUG_SDMMC_WRITE_TEST
/**
 * @brief 写通路的上板回环测试：只碰分区之前的空闲扇区，不碰任何文件系统
 * @details 目标是 MBR 与第一个分区之间的空洞里的 LBA 4096（本卡第一个分区从 8192 起，FAT 用不到这里）。
 *   1. 确认 LBA0 是 MBR（不是 GPT）且第一个分区起点在目标之后，否则放弃；
 *   2. 读出目标块，**不是全零立即放弃**——说明被别的东西用着，不能覆盖；
 *   3. 写测试图案 → 读回逐字节比对；
 *   4. 写回全零 → 读回确认全零，恢复原状。
 * @note 任何一步失败都停下，不再继续写。必须在 trap_init() 之后调用。
 */
void sdmmc_write_test(void)
{
    static uint8_t pat[SD_BLOCK_SIZE];
    static uint8_t back[SD_BLOCK_SIZE];
    const uint64_t lba = 4096;

    int ret = sdmmc_init();
    printf("sdwrite: init ret=%d\n", ret);
    if (ret != ENO0_NO_ERROR)
    {
        return;
    }

    ret = sdmmc_read_xfer(0, back, 1);
    uint32_t part_start = (uint32_t)back[454] | ((uint32_t)back[455] << 8) |
                          ((uint32_t)back[456] << 16) | ((uint32_t)back[457] << 24);
    if (ret != ENO0_NO_ERROR || back[510] != 0x55 || back[511] != 0xaa || back[450] == 0xee ||
        part_start <= lba)
    {
        printf("sdwrite: SKIP (ret=%d ptype=0x%02x part_start=%u) - lba %lu not provably unused\n",
               ret, back[450], part_start, (unsigned long)lba);
        return;
    }

    ret = sdmmc_read_xfer(lba, back, 1);
    for (int i = 0; ret == ENO0_NO_ERROR && i < SD_BLOCK_SIZE; i++)
    {
        if (back[i] != 0)
        {
            printf("sdwrite: SKIP - lba %lu not all zero (byte %d = 0x%02x)\n",
                   (unsigned long)lba, i, back[i]);
            return;
        }
    }
    if (ret != ENO0_NO_ERROR)
    {
        printf("sdwrite: FAIL read original ret=%d\n", ret);
        return;
    }

    for (int i = 0; i < SD_BLOCK_SIZE; i++)
    {
        pat[i] = (uint8_t)(i * 7 + 0x5a);
    }
    ret = sdmmc_write_xfer(lba, pat, 1);
    printf("sdwrite: write pattern ret=%d rintsts=0x%08x\n", ret, sdmmc_last_rintsts);
    if (ret != ENO0_NO_ERROR)
    {
        return;
    }
    ret = sdmmc_read_xfer(lba, back, 1);
    int mismatch = -1;
    for (int i = 0; ret == ENO0_NO_ERROR && i < SD_BLOCK_SIZE; i++)
    {
        if (back[i] != pat[i])
        {
            mismatch = i;
            break;
        }
    }
    printf("sdwrite: read back ret=%d %s (first mismatch %d)\n", ret,
           (ret == ENO0_NO_ERROR && mismatch < 0) ? "MATCH" : "MISMATCH", mismatch);

    memset(pat, 0, sizeof(pat));
    int rret = sdmmc_write_xfer(lba, pat, 1);
    int zero = (rret == ENO0_NO_ERROR) ? sdmmc_read_xfer(lba, back, 1) : rret;
    for (int i = 0; zero == ENO0_NO_ERROR && i < SD_BLOCK_SIZE; i++)
    {
        if (back[i] != 0)
        {
            zero = ENO29_IO;
        }
    }
    printf("sdwrite: restore zeros write=%d verify=%d\n", rret, zero);
    printf("sdwrite: %s\n", (ret == ENO0_NO_ERROR && mismatch < 0 && zero == ENO0_NO_ERROR) ? "PASS" : "FAIL");
}
#endif

#endif
