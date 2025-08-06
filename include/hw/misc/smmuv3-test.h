#ifndef HW_MISC_SMMUV3_TEST_H
#define HW_MISC_SMMUV3_TEST_H

#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"
#include "system/memory.h" /* Required for IOMMUMemoryRegion and AddressSpace */
#include "hw/arm/smmuv3.h"
#include "hw/arm/virt.h"

#if ENABLE_PLAT_DEV_SMMU
#define TYPE_SMMUV3_TEST "smmuv3-test"
OBJECT_DECLARE_SIMPLE_TYPE(SMMUV3TestState, SMMUV3_TEST)

#define SMMUV3TestR_CON 0
#define SMMUV3TEST_SID 0x1

struct SMMUV3TestState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion iomem;     /* Memory region for device's own registers (MMIO) */
    qemu_irq irq;
    uint32_t debug;

    /*
     * ADDED: An IOMMU MemoryRegion to handle DMA translations and the
     * corresponding AddressSpace that will be exposed to other devices.
     */
    IOMMUMemoryRegion *dma_mr;
    AddressSpace *dma_as;
    SMMUDevice *sdev;

    /* registers below */
    uint64_t con; /* control, 7bits */
    uint32_t sid;
    /* registers above */
};
#endif
#endif /* HW_MISC_SMMUV3_TEST_H */
