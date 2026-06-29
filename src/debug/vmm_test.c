#include "console.h"
#include "vmm.h"
#include "pmm.h"
#include "errorcode.h"
#include "stringops.h"

static int vmm_pass = 0;
static int vmm_fail = 0;

static void check(const char *name, int cond)
{
    if (cond)
    {
        printf("  [PASS] %s\n", name);
        vmm_pass++;
    }
    else
    {
        printf("  [FAIL] %s\n", name);
        vmm_fail++;
    }
}

void vmm_test(void)
{
    printf("\n=== VMM test ===\n");

    /* --- 测试 1：alloc_page + KVA 读写 ---
     * 验证偏移映射建立正确：通过 pa_to_kva 取得的高位 VA 可以正常读写物理帧 */
    pframe_t *frame = alloc_page();
    check("alloc_page not NULL", frame != NULL);
    if (frame)
    {
        volatile uint64_t *p = (volatile uint64_t *)convert_pframe2kva(frame);
        p[0] = 0xdeadbeefcafe1234UL;
        p[1] = 0x0123456789abcdefUL;
        check("KVA p[0] write/read", p[0] == 0xdeadbeefcafe1234UL);
        check("KVA p[1] write/read", p[1] == 0x0123456789abcdefUL);
        dealloc(frame);
    }

    /* --- 测试 2：VMA 插入与查找 ---
     * 验证 vmm_vma_insert 保持 vm_start 升序，vmm_vma_get 命中/miss 逻辑正确 */
    mm_t *mm = vmm_mm_create();
    check("vmm_mm_create not NULL", mm != NULL);
    if (!mm)
        goto done;

    vma_t *v1 = vmm_vma_create(0x1000, 0x3000, VMP_R | VMP_W);
    vma_t *v2 = vmm_vma_create(0x5000, 0x7000, VMP_R | VMP_X);
    check("vmm_vma_create v1", v1 != NULL);
    check("vmm_vma_create v2", v2 != NULL);

    if (v1 && v2)
    {
        vmm_vma_insert(mm, v1);
        vmm_vma_insert(mm, v2);

        /* 区间内命中 */
        check("get mid v1 (0x1800)", vmm_vma_get(mm, 0x1800) == v1);
        check("get mid v2 (0x6000)", vmm_vma_get(mm, 0x6000) == v2);
        /* 区间起始（含）命中 */
        check("get v1 at vm_start (0x1000)", vmm_vma_get(mm, 0x1000) == v1);
        /* vm_end 不含 */
        check("get at v1 vm_end (0x3000) -> NULL", vmm_vma_get(mm, 0x3000) == NULL);
        /* 空洞与越界 */
        check("get in gap (0x4000) -> NULL", vmm_vma_get(mm, 0x4000) == NULL);
        check("get before all (0x0800) -> NULL", vmm_vma_get(mm, 0x0800) == NULL);
        check("get after all (0x8000) -> NULL", vmm_vma_get(mm, 0x8000) == NULL);
        check("map_count == 2", mm->map_count == 2);

        /* --- 测试 3：vmm_map_vma ---
         * 验证 v1 区间被即时分配物理帧，函数正常返回 */
        int ret = vmm_map_vma(mm, v1);
        check("vmm_map_vma v1 returns OK", ret == ENO0_NO_ERROR);
    }

    /* --- 测试 4：vmm_mm_destroy ---
     * 验证销毁时物理帧被归还、描述符被释放，不崩溃即通过 */
    vmm_mm_destroy(mm);
    check("vmm_mm_destroy no crash", 1);

done:
    printf("=== VMM test done: %d pass  %d fail ===\n\n", vmm_pass, vmm_fail);
}
