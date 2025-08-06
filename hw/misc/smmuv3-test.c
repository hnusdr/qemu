
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h" /* Required for address_space_init */
#include "hw/arm/smmuv3.h"      /* Required for TYPE_SMMUV3_IOMMU_MEMORY_REGION */
#include "hw/arm/smmuv3-internal.h"
#if ENABLE_PLAT_DEV_SMMU
#include "hw/misc/smmuv3-test.h"
/* 
 * Generic field setting macro that can be used for any structure field
 * @param obj: structure pointer (STE * or CD *)
 * @param word_idx: word index
 * @param shift: bit offset
 * @param width: bit width
 * @param val: value to set
 */
#define FIELD_SET(obj, word_idx, shift, width, val) do {                 \
    uint32_t mask = (1U << (width)) - 1;                                 \
    (obj)->word[word_idx] = ((obj)->word[word_idx] & ~(mask << (shift))) | \
                          (((val) & mask) << (shift));                   \
} while (0)

/* 
 * Compile-time calculated STE field setting macros
 * Field-specific macros with embedded field configuration parameters
 */
#define STE_VALID_SET(ste, val) \
    ((ste)->word[0] = ((ste)->word[0] & ~(0x1 << 0)) | (((val) & 0x1) << 0))
#define STE_CONFIG_SET(ste, val) \
    ((ste)->word[0] = ((ste)->word[0] & ~(0x7 << 1)) | (((val) & 0x7) << 1))
#define STE_S1FMT_SET(ste, val) \
    ((ste)->word[0] = ((ste)->word[0] & ~(0x3 << 4)) | (((val) & 0x3) << 4))
#define STE_CTXPTR_SET(ste, val)                                   \
    ((ste)->word[0] = ((ste)->word[0] & ~(0xffffffc0)) | ((val) & 0xffffffc0))
#define STE_S1CDMAX_SET(ste, val) \
    ((ste)->word[1] = ((ste)->word[1] & ~(0x1f << 27)) | (((val) & 0x1f) << 27))
#define STE_S1STALLD_SET(ste, val) \
    ((ste)->word[2] = ((ste)->word[2] & ~(0x1 << 27)) | (((val) & 0x1) << 27))
#define STE_EATS_SET(ste, val) \
    ((ste)->word[2] = ((ste)->word[2] & ~(0x3 << 28)) | (((val) & 0x3) << 28))
#define STE_STRW_SET(ste, val) \
    ((ste)->word[2] = ((ste)->word[2] & ~(0x3 << 30)) | (((val) & 0x3) << 30))
#define STE_NSCFG_SET(ste, val) \
    ((ste)->word[2] = ((ste)->word[2] & ~(0x3 << 14)) | (((val) & 0x3) << 14))
#define STE_S2VMID_SET(ste, val) \
    ((ste)->word[4] = ((ste)->word[4] & ~0xffff) | ((val) & 0xffff))
#define STE_S2T0SZ_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~0x3f) | ((val) & 0x3f))
#define STE_S2SL0_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x3 << 6)) | (((val) & 0x3) << 6))
#define STE_S2TG_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x3 << 14)) | (((val) & 0x3) << 14))
#define STE_S2PS_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x7 << 16)) | (((val) & 0x7) << 16))
#define STE_S2AA64_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 19)) | (((val) & 0x1) << 19))
#define STE_S2ENDI_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 20)) | (((val) & 0x1) << 20))
#define STE_S2AFFD_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 21)) | (((val) & 0x1) << 21))
#define STE_S2HD_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 23)) | (((val) & 0x1) << 23))
#define STE_S2HA_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 24)) | (((val) & 0x1) << 24))
#define STE_S2S_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 25)) | (((val) & 0x1) << 25))
#define STE_S2R_SET(ste, val) \
    ((ste)->word[5] = ((ste)->word[5] & ~(0x1 << 26)) | (((val) & 0x1) << 26))
#define STE_S2TTB_SET(ste, val) do { \
    /* Write low 28 bits of val to high 28 bits of word[6], preserve low 4 bits */ \
    (ste)->word[6] = ((ste)->word[6] & 0x0000000f) | (((val) & 0x0fffffff) << 4); \
    /* Write high 16 bits of val to low 16 bits of word[7] */ \
    (ste)->word[7] = ((ste)->word[7] & ~0x0000ffff) | (((val) >> 28) & 0x0000ffff); \
} while (0)

#define STE_S_S2TG_SET(ste, val) \
    ((ste)->word[9] = ((ste)->word[9] & ~(0x3 << 14)) | (((val) & 0x3) << 14))
#define STE_S_S2SL0_SET(ste, val) \
    ((ste)->word[9] = ((ste)->word[9] & ~(0x3 << 6)) | (((val) & 0x3) << 6))
#define STE_S_S2T0SZ_SET(ste, val) \
    ((ste)->word[9] = ((ste)->word[9] & ~(0x3f << 0)) | (((val) & 0x3f) << 0))
#define STE_S_S2PS_SET(ste, val) \
    ((ste)->word[9] = ((ste)->word[9] & ~(0x7 << 16)) | (((val) & 0x7) << 16))


#define STE_S_S2TTB_SET(ste, val) do { \
    /* S_S2TTB[31:4] is stored in word[12][31:4]. No shift needed, just masking. */ \
    (ste)->word[12] = ((ste)->word[12] & 0x0000000f) | ((val) & 0xfffffff0); \
    /* S_S2TTB[47:32] is stored in word[13][15:0]. Right shift by 32 bits. */ \
    (ste)->word[13] = ((ste)->word[13] & 0xffff0000) | (((val) >> 32) & 0x0000ffff); \
} while (0)

#define STE_S2TTB(x)                                    \
    ((extract64((x)->word[7], 0, 16) << 32) |           \
     ((x)->word[6] & 0xfffffff0))

#define STE_S_S2TTB(x)                                  \
    ((extract64((x)->word[13], 0, 16) << 32) |           \
     ((x)->word[12] & 0xfffffff0))


/* 
 * Compile-time calculated CD field setting macros
 * Some important CD field setting macros
 */
#define CD_VALID_SET(cd, val) \
    ((cd)->word[0] = ((cd)->word[0] & ~(0x1 << 31)) | (((val) & 0x1) << 31))
#define CD_TSZ_SET(cd, sel, val) \
    ((cd)->word[0] = ((cd)->word[0] & ~(0x3F << ((sel) * 16 + 0))) | (((val) & 0x3F) << ((sel) * 16 + 0)))
#define CD_TG_SET(cd, sel, val) \
    ((cd)->word[0] = ((cd)->word[0] & ~(0x3 << ((sel) * 16 + 6))) | (((val) & 0x3) << ((sel) * 16 + 6)))
#define CD_EPD_SET(cd, sel, val) \
    ((cd)->word[0] = ((cd)->word[0] & ~(0x1 << ((sel) * 16 + 14))) | (((val) & 0x1) << ((sel) * 16 + 14)))
#define CD_ENDI_SET(cd, val) \
    ((cd)->word[0] = ((cd)->word[0] & ~(0x1 << 15)) | (((val) & 0x1) << 15))
#define CD_IPS_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x7 << 0)) | (((val) & 0x7) << 0))
#define CD_AFFD_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 3)) | (((val) & 0x1) << 3))
#define CD_HD_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 10)) | (((val) & 0x1) << 10))
#define CD_HA_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 11)) | (((val) & 0x1) << 11))

#define CD_TTB_SET(cd, sel, val) do { \
    (cd)->word[(sel) * 2 + 2] = ((cd)->word[(sel) * 2 + 2] & 0x0000000F) | ((val) & 0xFFFFFFF0); \
    (cd)->word[(sel) * 2 + 3] = ((cd)->word[(sel) * 2 + 3] & 0xFFF80000) | (((val) >> 32) & 0x0007FFFF); \
} while (0)

#define CD_HAD_SET(cd, sel, val) \
    ((cd)->word[(sel) * 2 + 2] = ((cd)->word[(sel) * 2 + 2] & ~(0x1 << 1)) | (((val) & 0x1) << 1))
#define CD_TTB0_SET(cd, val) \
    ((cd)->word[2] = ((cd)->word[2] & ~0xFFFFFFFC) | ((val) & 0xFFFFFFFC))
#define CD_TTB1_SET(cd, val) \
    ((cd)->word[3] = ((cd)->word[3] & ~0xFFFFFFFC) | ((val) & 0xFFFFFFFC))
#define CD_MAIR0_SET(cd, val) \
    ((cd)->word[6] = (val))
#define CD_MAIR1_SET(cd, val) \
    ((cd)->word[7] = (val))
#define CD_TCR_T0SZ_SET(cd, val) \
    ((cd)->word[4] = ((cd)->word[4] & ~0x3F) | ((val) & 0x3F))
#define CD_ASID_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0xFFFF << 16)) | (((val) & 0xFFFF) << 16))
#define CD_S_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 12)) | (((val) & 0x1) << 12))
#define CD_R_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 13)) | (((val) & 0x1) << 13))
#define CD_A_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 14)) | (((val) & 0x1) << 14))
#define CD_AARCH64_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x1 << 9)) | (((val) & 0x1) << 9))
#define CD_TBI_SET(cd, val) \
    ((cd)->word[1] = ((cd)->word[1] & ~(0x3 << 6)) | (((val) & 0x3) << 6))

/* 
 * Generic function to set STE field
 * @param obj: structure pointer (STE * or CD *)
 * @param word_idx: word index
 * @param shift: bit offset
 * @param mask: bit mask
 * @param val: value to set
 */
static inline void smmuv3_set_field(void *obj, int word_idx, int shift, int mask, uint32_t val)
{
    uint32_t *words = (uint32_t *)obj;
    words[word_idx] = (words[word_idx] & ~(mask << shift)) | ((val & mask) << shift);
}

/* Use more generic function to set STE field */
static inline void ste_set_field(STE *ste, int word_idx, int shift, int width, uint32_t val)
{
    uint32_t mask = (1U << width) - 1;
    smmuv3_set_field(ste, word_idx, shift, mask, val);
}

/* Use more generic function to set CD field */
static inline void cd_set_field(CD *cd, int word_idx, int shift, int width, uint32_t val)
{
    uint32_t mask = (1U << width) - 1;
    smmuv3_set_field(cd, word_idx, shift, mask, val);
}

extern SMMUState *g_smmuv3_state;
SMMUDevice *smmuv3_test_dev;
#define STE_OR_CD_ENTRY_BYTES 64 /* 64 bytes per STE entry */
#define STE_S2T0SZ_VAL 0x14

/* GPA must locate inside virt.secure-ram [0xe000000, 0xeffffff] */
#define TEST_STE_ENTRY_GPA 0xe166040
#define TEST_CD_ENTRY_GPA 0xe166080

#define TEST_START_LEVEL 0
#define TEST_STAGE2_ONLY 0
#define TEST_STAGE1_ONLY 0
#define TEST_STAGE_NESTED 1


#if defined(TEST_STAGE1_ONLY) && TEST_STAGE1_ONLY
/************ start of TEST_STAGE1_ONLY ************/

#define TEST_GVA 0x8080604567
#define TEST_GPA 0xecba567

#define TEST_S1VTTB TEST_S2VTTB
#define TEST_S2VTTB 0xe4d0000

#define TEST_LEVEL_0_TABLE_VAL      0x000000000e4d1003UL
#define TEST_LEVEL_1_TABLE_VAL      0x000000000e4d2003UL
#define TEST_LEVEL_2_TABLE_VAL      0x000000000e4d3003UL
#define TEST_LEVEL_3_TABLE_VAL      0x40000000ecba743ULL      /* AP[2:1] = 0b01 */

#define TEST_LEVEL_0_TABLE_INDEX    (TEST_GVA >> 39) & 0x1ff
#define TEST_LEVEL_1_TABLE_INDEX    (TEST_GVA >> 30) & 0x1ff
#define TEST_LEVEL_2_TABLE_INDEX    (TEST_GVA >> 21) & 0x1ff
#define TEST_LEVEL_3_TABLE_INDEX    (TEST_GVA >> 12) & 0x1ff

#define TEST_LEVEL_0_TABLE_ADDR_CALC     (TEST_LEVEL_0_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_0_TABLE_INDEX * 8)
#define TEST_LEVEL_1_TABLE_ADDR_CALC     (TEST_LEVEL_1_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_1_TABLE_INDEX * 8)
#define TEST_LEVEL_2_TABLE_ADDR_CALC     (TEST_LEVEL_2_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_2_TABLE_INDEX * 8)
#define TEST_LEVEL_3_TABLE_ADDR_CALC     (TEST_LEVEL_3_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_3_TABLE_INDEX * 8)

#define TEST_LEVEL_0_TABLE_ADDR  0xe4d0008
#define TEST_LEVEL_1_TABLE_ADDR  0xe4d1010
#define TEST_LEVEL_2_TABLE_ADDR  0xe4d2018
#define TEST_LEVEL_3_TABLE_ADDR  0xe4d3020

/************ end of TEST_STAGE1_ONLY ************/
#elif defined(TEST_STAGE2_ONLY) && TEST_STAGE2_ONLY
/************ start of TEST_STAGE2_ONLY ************/
#if TEST_START_LEVEL == 0
/* Page walk from level 0 */
/*
Page table level | Write Address |     Value Written

       L0        | 0xe14d000     |      0xe14d803

       L1        | 0xe14e010     |      0xe14e803

       L2        | 0xe14e830     |      0xe14f003

       L3        | 0xe14f400     |  0x400000000e080a7c3
*/

// #define TEST_GVA 0x8080604040        //    , 0x8080604567
#define TEST_GVA 0x8080604567
#define TEST_GPA 0xecba567

// #define TEST_GPA 0x100080000567
// #define TEST_GPA  0x8090040   //    ,   0x800800007c3
// #define TEST_GPA 0x800800007c3

#define TEST_S2VTTB 0xe4d0000

#define TEST_LEVEL_0_TABLE_VAL      0x000000000e4d1003UL
#define TEST_LEVEL_1_TABLE_VAL      0x000000000e4d2003UL
#define TEST_LEVEL_2_TABLE_VAL      0x000000000e4d3003UL
#define TEST_LEVEL_3_TABLE_VAL      0x40000000ecba7c3ULL
// #define TEST_LEVEL_3_TABLE_VAL      0x4001000800007c3ULL
// #define TEST_LEVEL_3_TABLE_VAL      0x60000008090687ULL   //  , 0x4000800800007c3ULL

#define TEST_LEVEL_0_TABLE_INDEX    (TEST_GVA >> 39) & 0x1ff
#define TEST_LEVEL_1_TABLE_INDEX    (TEST_GVA >> 30) & 0x1ff
#define TEST_LEVEL_2_TABLE_INDEX    (TEST_GVA >> 21) & 0x1ff
#define TEST_LEVEL_3_TABLE_INDEX    (TEST_GVA >> 12) & 0x1ff

#define TEST_LEVEL_0_TABLE_ADDR_CALC     (TEST_S2VTTB & ~0xfffUL) + (TEST_LEVEL_0_TABLE_INDEX * 8)
#define TEST_LEVEL_1_TABLE_ADDR_CALC     (TEST_LEVEL_0_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_1_TABLE_INDEX * 8)
#define TEST_LEVEL_2_TABLE_ADDR_CALC     (TEST_LEVEL_1_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_2_TABLE_INDEX * 8)
#define TEST_LEVEL_3_TABLE_ADDR_CALC     (TEST_LEVEL_2_TABLE_VAL & ~0xfffUL) + (TEST_LEVEL_3_TABLE_INDEX * 8)

#define TEST_LEVEL_0_TABLE_ADDR  0xe4d0008
#define TEST_LEVEL_1_TABLE_ADDR  0xe4d1010
#define TEST_LEVEL_2_TABLE_ADDR  0xe4d2018
#define TEST_LEVEL_3_TABLE_ADDR  0xe4d3020

#elif TEST_START_LEVEL == 1
/* Page walk from level 1 */
#define TEST_GVA                    0x1a4b3c50

#define TEST_LEVEL_1_TABLE_VAL      0xe14d000
#define TEST_LEVEL_2_TABLE_VAL      0xe14e003
#define TEST_LEVEL_3_TABLE_VAL      0xe14f003
#define TEST_LEAF_TABLE_VAL         0xe8767c3

#define TEST_LEVEL_1_TABLE_INDEX    (TEST_GVA >> 30) & 0x1ff
#define TEST_LEVEL_2_TABLE_INDEX    (TEST_GVA >> 21) & 0x1ff
#define TEST_LEVEL_3_TABLE_INDEX    (TEST_GVA >> 12) & 0x1ff

#define TEST_LEVEL_1_TABLE_ADDR     (TEST_LEVEL_1_TABLE_VAL & ~0xfff) + (TEST_LEVEL_1_TABLE_INDEX * 8)
#define TEST_LEVEL_2_TABLE_ADDR     (TEST_LEVEL_2_TABLE_VAL & ~0xfff) + (TEST_LEVEL_2_TABLE_INDEX * 8)
#define TEST_LEVEL_3_TABLE_ADDR     (TEST_LEVEL_3_TABLE_VAL & ~0xfff) + (TEST_LEVEL_3_TABLE_INDEX * 8)

#endif

/************ end of TEST_STAGE2_ONLY ************/
#elif defined(TEST_STAGE_NESTED) && TEST_STAGE_NESTED
/************ start of TEST_STAGE_NESTED ************/

#define TEST_GVA 0x8080604567
#define TEST_GPA 0xecba567

#define TEST_S1VTTB TEST_S2VTTB
#define TEST_S2VTTB 0xe4d0000

#define TEST_LEVEL_0_TABLE_VAL      0x000000000e4d1003UL
#define TEST_LEVEL_1_TABLE_VAL      0x000000000e4d2003UL
#define TEST_LEVEL_2_TABLE_VAL      0x000000000e4d3003UL
#define TEST_LEVEL_3_TABLE_VAL      0x40000000ecba743ULL      /* AP[2:1] = 0b01 */

#define TEST_LEVEL_0_TABLE_INDEX    (TEST_GVA >> 39) & 0x1ff
#define TEST_LEVEL_1_TABLE_INDEX    (TEST_GVA >> 30) & 0x1ff
#define TEST_LEVEL_2_TABLE_INDEX    (TEST_GVA >> 21) & 0x1ff
#define TEST_LEVEL_3_TABLE_INDEX    (TEST_GVA >> 12) & 0x1ff

#define TEST_LEVEL_0_TABLE_ADDR  0xe4d0008
#define TEST_LEVEL_1_TABLE_ADDR  0xe4d1010
#define TEST_LEVEL_2_TABLE_ADDR  0xe4d2018
#define TEST_LEVEL_3_TABLE_ADDR  0xe4d3020

/************* Macros for nested translation of Context Descriptor (CD) *************/
/* Page table address of CD for nested translation (Stage 2)*/
#define TEST_CD_S2_LEVEL_0_TABLE_ADDR  0xe4d0000    /* (((TEST_CD_ENTRY_GPA >> 39) & 0x1ff) * 8) */
#define TEST_CD_S2_LEVEL_1_TABLE_ADDR  0xe4d1000    /* (((TEST_CD_ENTRY_GPA >> 30) & 0x1ff) * 8) */
#define TEST_CD_S2_LEVEL_2_TABLE_ADDR  0xe4d2380    /* (((TEST_CD_ENTRY_GPA >> 21) & 0x1ff) * 8) */
#define TEST_CD_S2_LEVEL_3_TABLE_ADDR  0xe4d3b30    /* (((TEST_CD_ENTRY_GPA >> 12) & 0x1ff) * 8) */

/* CD's IOVA as GPA and level below 3 is identical to TEST_LEVEL_0/1/2_TABLE_VAL*/
#define TEST_CD_S2_LEVEL_3_TABLE_VAL   0x40000000e166743ULL

/* Page table address of TTBx in CD for nested translation (Stage 2) */
/* TEST_S1VTTB as IPA and output is PA (Using CD_TTB_SET to set S1VTTB)*/
#define TEST_CD_TTB_S2_LEVEL_2_TABLE_ADDR  0xe4d2390    /* (((TEST_S1VTTB >> 21) & 0x1ff) * 8) */
#define TEST_CD_TTB_S2_LEVEL_3_TABLE_ADDR  0xe4d3680    /* (((TEST_S1VTTB >> 12) & 0x1ff) * 8) */

#define TEST_CD_TTB_S2_LEVEL_3_TABLE_VAL   0x40000000e4d0743ULL
/************* END of macros for nested translation of Context Descriptor (CD) *************/

/************* Macros for nested translation of IOVA *************/
/* Finally, we start to walk page of IOVA. It's a loop as showed below:
 * 1. Previous level translation input: IOVA(TEST_GVA) , output: IPA(PTE value reading from secure RAM);
 * 2. Do smmu_ptw_64_s2 to translate from IPA to PA_BASE, which is as the base addr of the next level walking;
 * 3. Next level translation input: PA_BASE, output: PA
 * 
*/

/* Stage 2-Level 3 in Stage 1-Level 0 , IPA = 0xe4d1000*/
#define TEST_S1_L0_IN_S2_L3_TABLE_ADDR          0xe4d3688
#define TEST_S1_L0_IN_S2_L3_TABLE_VAL           0x40000000e4d1743ULL /* IPA as PA*/

/* Stage 2-Level 3 in Stage 1-Level 1 , IPA = 0xe4d2000*/
#define TEST_S1_L1_IN_S2_L3_TABLE_ADDR          0xe4d3690
#define TEST_S1_L1_IN_S2_L3_TABLE_VAL           0x40000000e4d2743ULL /* IPA as PA*/

/* Stage 2-Level 3 in Stage 1-Level 2 , IPA = 0xe4d3000*/
#define TEST_S1_L2_IN_S2_L3_TABLE_ADDR          0xe4d3698
#define TEST_S1_L2_IN_S2_L3_TABLE_VAL           0x40000000e4d3743ULL /* IPA as PA*/

/* Stage 2-Level 2 in Stage 1-Level 3 */
/* IPA(0xe4d3000) cannot share the same table with previous level, so we need to use a new table*/
#define TEST_S1_L3_IN_S2_L2_TABLE_ADDR          0xe4d23b0
#define TEST_S1_L3_IN_S2_L2_TABLE_VAL           0xe4d3003
/* Stage 2-Level 3 in Stage 1-Level 3 */
#define TEST_S1_L3_IN_S2_L3_TABLE_ADDR          0xe4d35d0
#define TEST_S1_L3_IN_S2_L3_TABLE_VAL           0x40000000ecba7c3ULL /* IPA as PA*/

/************* END of macros for nested translation of IOVA *************/

/************ end of TEST_STAGE_NESTED ************/
#endif

static void smmuv3_test_fill_data(AddressSpace *as,
                                  uint64_t write_addr,
                                  uint64_t write_val) {
    MemTxAttrs attrs = (MemTxAttrs) { .secure = 1 };
    int ret = 0;

    ret = address_space_write(as, write_addr, attrs, &write_val, sizeof(write_val));
    printf("Write addr 0x%lx with value 0x%lx , ret code %d\n", write_addr, write_val, ret);
    ret = address_space_read(as, write_addr, attrs, &write_val, sizeof(write_val));
    printf("====>>> Now read addr, value 0x%lx , ret code %d\n", write_val, ret);
}

static void smmuv3_init_trans_data(AddressSpace *as) {
    MemTxAttrs attrs = (MemTxAttrs) { .secure = 1 };
    MemTxResult ret = 0;
    /* 1. Fill Stream Table Entry (STE) */
    STE ste;
    memset(&ste, 0, sizeof(STE));
    
    /* Set basic configuration */
    // ste.word[0] = 0x8574200d;
    // ste.word[1] = 0x3;
    // ste.word[2] = 0xd6;

    /* Use different methods to set STE fields */
    uint64_t vttb = TEST_S2VTTB;
    /* Method 1: Use dedicated macro to set S2AA64 field */
    STE_VALID_SET(&ste, 1);
    STE_CONFIG_SET(&ste, 0x6);
    STE_S2T0SZ_SET(&ste, STE_S2T0SZ_VAL);
    STE_S2SL0_SET(&ste, 0x2);   /* Start level 0*/
    STE_S2TG_SET(&ste, 0);      /* 4KB */
    STE_S2PS_SET(&ste, 0x5);    /* 48 bits ?*/
    STE_S2AA64_SET(&ste, 1);    /* Enable S2AA64, indicates using 64-bit address format */
    STE_S2ENDI_SET(&ste, 0);    /* Little Endian */
    STE_S2AFFD_SET(&ste, 0);    /* AF Fault Disable */

    STE_S_S2T0SZ_SET(&ste, STE_S2T0SZ_VAL);
    STE_S_S2SL0_SET(&ste, 0x2);   /* Start level */
    STE_S_S2TG_SET(&ste, 0);      /* 4KB */
    STE_S_S2PS_SET(&ste, 0x5);    /* 48 bits ?*/
    STE_S_S2TTB_SET(&ste, vttb); /* walk from level 1*/

    uint64_t write_addr = 0, write_val = 0;

    write_addr = TEST_LEVEL_0_TABLE_ADDR;
    write_val = TEST_LEVEL_0_TABLE_VAL;
    printf("Begin write level 0 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_LEVEL_1_TABLE_ADDR;
    write_val = TEST_LEVEL_1_TABLE_VAL;
    printf("Begin write level 1 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_LEVEL_2_TABLE_ADDR;
    write_val = TEST_LEVEL_2_TABLE_VAL;
    printf("Begin write level 2 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_LEVEL_3_TABLE_ADDR;
    write_val = TEST_LEVEL_3_TABLE_VAL;
    printf("Begin write level 3 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

#if TEST_STAGE1_ONLY | TEST_STAGE_NESTED
    /* 2. Fill Context Descriptor (CD) */
    CD cd;
    memset(&cd, 0, sizeof(CD));

    cd.word[0] = 0xc0003510;
    cd.word[1] = 0x0001e204;
    cd.word[2] = 0x05749000;
    cd.word[3] = 0x00000001;
    cd.word[4] = 0x00000000;
    cd.word[5] = 0x00000000;
    cd.word[6] = 0xf404ff44;
    cd.word[7] = 0xffffffff;
    cd.word[8] = 0x00000000;
    cd.word[9] = 0x00000000;
    cd.word[10] = 0x00000000;
    cd.word[11] = 0x00000000;
    cd.word[12] = 0x00000000;
    cd.word[13] = 0x00000000;
    cd.word[14] = 0x00000000;
    cd.word[15] = 0x00000000;

    /* print all necessary fields*/
    printf("CD_VALID: 0x%x\n", CD_VALID(&cd));
    printf("CD_ASID: 0x%x\n", CD_ASID(&cd));
    printf("CD_TTB(0): 0x%llx\n", CD_TTB(&cd, 0));
    printf("CD_TTB(1): 0x%llx\n", CD_TTB(&cd, 1));
    printf("CD_HAD(0): 0x%x\n", CD_HAD(&cd, 0));
    printf("CD_HAD(1): 0x%x\n", CD_HAD(&cd, 1));
    printf("CD_TSZ(0): 0x%x\n", CD_TSZ(&cd, 0));
    printf("CD_TSZ(1): 0x%x\n", CD_TSZ(&cd, 1));
    printf("CD_TG(0): 0x%x\n", CD_TG(&cd, 0));
    printf("CD_TG(1): 0x%x\n", CD_TG(&cd, 1));
    printf("CD_EPD(0): 0x%x\n", CD_EPD(&cd, 0));
    printf("CD_EPD(1): 0x%x\n", CD_EPD(&cd, 1));
    printf("CD_ENDI: 0x%x\n", CD_ENDI(&cd));
    printf("CD_IPS: 0x%x\n", CD_IPS(&cd));
    printf("CD_AFFD: 0x%x\n", CD_AFFD(&cd));
    printf("CD_TBI: 0x%x\n", CD_TBI(&cd));
    printf("CD_HD: 0x%x\n", CD_HD(&cd));
    printf("CD_HA: 0x%x\n", CD_HA(&cd));
    printf("CD_S: 0x%x\n", CD_S(&cd));
    printf("CD_R: 0x%x\n", CD_R(&cd));
    printf("CD_A: 0x%x\n", CD_A(&cd));
    printf("CD_AARCH64: 0x%x\n", CD_AARCH64(&cd));

    CD_ASID_SET(&cd, 0x1e20);  /* Set ASID field */
    CD_AARCH64_SET(&cd, 1);       /* Set AA64 field*/
    CD_VALID_SET(&cd, 1);
    CD_A_SET(&cd, 1);
    CD_S_SET(&cd, 0);
    CD_HD_SET(&cd, 0);
    CD_HA_SET(&cd, 0);
    uint64_t s1_vttb = TEST_S1VTTB;
    CD_TTB_SET(&cd, 0, s1_vttb);
    
    ret = address_space_write(as, TEST_CD_ENTRY_GPA, attrs, &cd, STE_OR_CD_ENTRY_BYTES);
    printf("Write CD entry : %d\n", ret);

    CD read_cd;
    ret = address_space_read(as, TEST_CD_ENTRY_GPA, attrs, &read_cd, STE_OR_CD_ENTRY_BYTES);
    printf("Read CD entry : %d\n", ret);

    uint32_t read_cd_asid = CD_ASID(&read_cd);
    printf("CD.ASID : %d\n", read_cd_asid);
    uint32_t read_cd_aarch64 = CD_AARCH64(&read_cd);
    printf("CD.AARCH64 : %d\n", read_cd_aarch64);
    uint32_t read_cd_valid = CD_VALID(&read_cd);
    printf("CD.VALID : %d\n", read_cd_valid);
    uint32_t read_cd_a = CD_A(&read_cd);
    printf("CD.A : %d\n", read_cd_a);
    uint32_t read_cd_s = CD_S(&read_cd);
    printf("CD.S : %d\n", read_cd_s);
    uint32_t read_cd_hd = CD_HD(&read_cd);
    printf("CD.HD : %d\n", read_cd_hd);
    uint32_t read_cd_ha = CD_HA(&read_cd);
    printf("CD.HA : %d\n", read_cd_ha);


    STE_CTXPTR_SET(&ste, TEST_CD_ENTRY_GPA);
#if defined(TEST_STAGE_NESTED) && TEST_STAGE_NESTED
    STE_CONFIG_SET(&ste, 0x7);

    /* Fill CD content in nested translation */
    /* IOVA == address of CD == TEST_S2VTTB */
    write_addr = TEST_CD_S2_LEVEL_0_TABLE_ADDR;
    write_val = TEST_LEVEL_0_TABLE_VAL; /* Use identical page table value */
    printf("Begin write level 0 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_CD_S2_LEVEL_1_TABLE_ADDR;
    write_val = TEST_LEVEL_1_TABLE_VAL; /* Use identical page table value */
    printf("Begin write level 1 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_CD_S2_LEVEL_2_TABLE_ADDR;
    write_val = TEST_LEVEL_2_TABLE_VAL; /* Use identical page table value */
    printf("Begin write level 2 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_CD_S2_LEVEL_3_TABLE_ADDR;
    write_val = TEST_CD_S2_LEVEL_3_TABLE_VAL;
    printf("Begin write level 3 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    /* Fill page tables of CD.TTBx in nested translation */
    /* IPA == PA == TEST_S1VTTB */
    write_addr = TEST_CD_TTB_S2_LEVEL_2_TABLE_ADDR;
    write_val = TEST_LEVEL_2_TABLE_VAL; /* Use identical page table value */
    printf("Begin write level 2 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_CD_TTB_S2_LEVEL_3_TABLE_ADDR;
    write_val = TEST_CD_TTB_S2_LEVEL_3_TABLE_VAL;
    printf("Begin write level 3 PTE\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    /* Fill page tables of Stage 1 and Stage 2 . We share some tables between them*/
    write_addr = TEST_S1_L0_IN_S2_L3_TABLE_ADDR;
    write_val = TEST_S1_L0_IN_S2_L3_TABLE_VAL;
    printf("Begin write level 3 PTE in PTW\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_S1_L1_IN_S2_L3_TABLE_ADDR;
    write_val = TEST_S1_L1_IN_S2_L3_TABLE_VAL;
    printf("Begin write level 3 PTE in PTW\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_S1_L2_IN_S2_L3_TABLE_ADDR;
    write_val = TEST_S1_L2_IN_S2_L3_TABLE_VAL;
    printf("Begin write level 3 PTE in PTW\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_S1_L3_IN_S2_L2_TABLE_ADDR;
    write_val = TEST_S1_L3_IN_S2_L2_TABLE_VAL;
    printf("Begin write level 3 PTE in S2\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

    write_addr = TEST_S1_L3_IN_S2_L3_TABLE_ADDR;
    write_val = TEST_S1_L3_IN_S2_L3_TABLE_VAL;
    printf("Begin write level 3 PTE in S2\n");
    smmuv3_test_fill_data(as, write_addr, write_val);

#elif defined(TEST_STAGE1_ONLY) && TEST_STAGE1_ONLY
    STE_CONFIG_SET(&ste, 0x5);
#endif
#endif

    ret = address_space_write(as, TEST_STE_ENTRY_GPA, attrs, &ste, STE_OR_CD_ENTRY_BYTES);
    printf("Write STE entry : %d\n", ret);

}

static void smmuv3_test_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    SMMUV3TestState *s = SMMUV3_TEST(opaque);
    if (s->debug) {
        qemu_log_mask(LOG_GUEST_ERROR, "-> SMMUV3Test WRITE: 0x%" HWADDR_PRIx "=%ld (0x%lx)\n", addr, value, value);
    }
    switch (addr) {
    case SMMUV3TestR_CON:
        s->con = value;

        smmuv3_init_trans_data(smmu_get_address_space(true));
        MemTxAttrs attrs = (MemTxAttrs) { .secure = 1 };
        uint32_t data_to_write = 0x88888888;
        MemTxResult ret = address_space_write(s->dma_as, TEST_GVA, attrs,
                                              &data_to_write,
                                              sizeof(data_to_write));
        printf("write done: 0x%x , ret %d\n", data_to_write, ret);

        address_space_write(s->dma_as, TEST_GVA, attrs,
                            &data_to_write, sizeof(data_to_write));
        printf("===>>> second write done: 0x%x , ret %d\n", data_to_write, ret);

        uint32_t data_to_read = 0;
        address_space_read(s->dma_as, TEST_GVA, attrs,
                            &data_to_read, sizeof(data_to_read));
        printf("----===>>> read done: 0x%x, ret %d\n", data_to_read, ret);
        printf("\n");
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
}

static uint64_t smmuv3_test_read(void *opaque, hwaddr addr, unsigned size)
{
    SMMUV3TestState *s = SMMUV3_TEST(opaque);
    uint64_t read_val = 0;
    switch (addr) {
    case SMMUV3TestR_CON:
        read_val = s->con;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
    }
    if (s->debug) {
        qemu_log_mask(LOG_GUEST_ERROR, "<- SMMUV3Test READ : 0x%" HWADDR_PRIx "=%ld (0x%lx)\n", addr, read_val, read_val);
    }
    return read_val;
}

static const MemoryRegionOps smmuv3_test_ops = {
    .read = smmuv3_test_read,
    .write = smmuv3_test_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};


static void smmuv3_test_realize(DeviceState *dev, Error **errp)
{
    SMMUV3TestState *s = SMMUV3_TEST(dev);
    char *dma_mr_name;

    /* Initialize the MMIO region for device registers */
    memory_region_init_io(&s->iomem, OBJECT(dev), &smmuv3_test_ops, s,
                          TYPE_SMMUV3_TEST, 0x1000); /* Size: 4KB */
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /* Create a unique name for the IOMMU memory region */
    dma_mr_name = g_strdup_printf("%s.dma", TYPE_SMMUV3_TEST);
    s->dma_as = smmu_plat_dev_find_add_as(g_smmuv3_state, SMMUV3TEST_SID);
    /* Clean up the allocated name string */
    g_free(dma_mr_name);
}

static const VMStateDescription vmstate_smmuv3_test = {
    .name = TYPE_SMMUV3_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(con, SMMUV3TestState),
        VMSTATE_END_OF_LIST()
    }
};

static Property smmuv3_test_properties[] = {
     DEFINE_PROP_UINT32("debug", SMMUV3TestState, debug, 0),
};

static void smmuv3_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = smmuv3_test_realize;
    dc->vmsd = &vmstate_smmuv3_test;
    device_class_set_props(dc, smmuv3_test_properties);
}

static const TypeInfo smmuv3_test_info = {
    .name = TYPE_SMMUV3_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SMMUV3TestState),
    .class_init = smmuv3_test_class_init,
};

static void smmuv3_test_register_types(void)
{
    type_register_static(&smmuv3_test_info);
}

type_init(smmuv3_test_register_types)
#endif