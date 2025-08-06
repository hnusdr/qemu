# SMMUv3 Test Device Documentation

## Overview

The SMMUv3 Test Device is a custom platform device designed to test the secure translation flow in QEMU's SMMUv3 implementation. Since the TF-A SMMUv3 Test Engine does not support QEMU and no secure device assignment feature exists yet, this custom platform device was created to exercise the secure translation path.

## Architecture and Design

### Purpose and Motivation

- **Avoid Complex Code Addition**: To avoid adding extensive code to Hafnium, the device reuses the latter half of the SMMU memory region starting from address `0x09060000`.
- **Platform Device Integration**: Unlike typical PCIe devices that use SMMU, this platform device's realize function is simplified by removing many PCIe-related complex operations.
- **Secure Translation Testing**: The device enables testing of secure translation paths and IOMMU region functionality through controlled DMA accesses.

### Memory Layout

- **Base Address**: `0x09060000` (reuses SMMU's latter memory region)
- **Size**: 4KB MMIO region
- **STE Entry GPA**: `0xe166040` (must be within secure RAM range [0xe000000, 0xeffffff])
- **CD Entry GPA**: `0xe166080`

## Device Structure

### SMMUV3TestState Structure

```c
struct SMMUV3TestState {
    SysBusDevice parent_obj;
    
    MemoryRegion iomem;         // MMIO region for device registers
    qemu_irq irq;               // Interrupt line
    uint32_t debug;             // Debug flag
    
    IOMMUMemoryRegion *dma_mr;  // IOMMU memory region for DMA translations
    AddressSpace *dma_as;       // Address space for DMA operations
    SMMUDevice *sdev;           // Associated SMMU device
    
    // Device registers
    uint64_t con;               // Control register (7 bits)
    uint32_t sid;               // Stream ID
};
```

### Stream ID Configuration

The device uses a hardcoded Stream ID and modifies the `smmu_get_sid` function to bypass PCIe-related logic:

```c
static inline uint16_t smmu_get_sid(SMMUDevice *sdev)
{
#if ENABLE_PLAT_DEV_SMMU
    return 0 << 8 | sdev->devfn;  // Platform device path
#else
    return PCI_BUILD_BDF(pci_bus_num(sdev->bus), sdev->devfn);  // PCIe path
#endif
}
```

## Translation Configuration

The device supports three translation modes through compile-time configuration:

### 1. Stage 1 Only Translation (`TEST_STAGE1_ONLY`)

- **Guest Virtual Address (GVA)**: `0x8080604567`
- **Guest Physical Address (GPA)**: `0xecba567`
- **Translation Table Base**: `0xe4d0000`
- **Configuration**: STE_CONFIG = `0x5`

### 2. Stage 2 Only Translation (`TEST_STAGE2_ONLY`)

- **Guest Virtual Address**: `0x8080604567`
- **Guest Physical Address**: `0xecba567`
- **Stage 2 VTTB**: `0xe4d0000`
- **Configuration**: STE_CONFIG = `0x6`

### 3. Nested Translation (`TEST_STAGE_NESTED`)

- **Guest Virtual Address**: `0x8080604567`
- **Guest Physical Address**: `0xecba567`
- **Stage 1 VTTB**: `0xe4d0000`
- **Stage 2 VTTB**: `0xe4d0000`
- **Configuration**: Both Stage 1 and Stage 2 enabled

## Field Setting Macros

The device provides comprehensive macros for setting Stream Table Entry (STE) and Context Descriptor (CD) fields:

### STE Field Macros

```c
#define STE_VALID_SET(ste, val)     // Set valid bit
#define STE_CONFIG_SET(ste, val)    // Set configuration
#define STE_S1FMT_SET(ste, val)     // Set Stage 1 format
#define STE_CTXPTR_SET(ste, val)    // Set context pointer
#define STE_S2VMID_SET(ste, val)    // Set Stage 2 VMID
#define STE_S2T0SZ_SET(ste, val)    // Set Stage 2 T0SZ
#define STE_S2SL0_SET(ste, val)     // Set Stage 2 starting level
#define STE_S2TG_SET(ste, val)      // Set Stage 2 translation granule
#define STE_S2PS_SET(ste, val)      // Set Stage 2 physical address size
#define STE_S2AA64_SET(ste, val)    // Set Stage 2 AArch64 format
```

### CD Field Macros

```c
#define CD_VALID_SET(cd, val)       // Set valid bit
#define CD_TSZ_SET(cd, sel, val)    // Set translation size
#define CD_TG_SET(cd, sel, val)     // Set translation granule
#define CD_EPD_SET(cd, sel, val)    // Set entry point disable
#define CD_IPS_SET(cd, val)         // Set intermediate physical address size
#define CD_TTB_SET(cd, sel, val)    // Set translation table base
#define CD_ASID_SET(cd, val)        // Set address space identifier
```

## Page Table Configuration

### 4-Level Page Table Structure

The device configures a 4-level page table structure (Levels 0-3) with the following characteristics:

- **Page Size**: 4KB
- **Address Width**: 48-bit
- **Starting Level**: 0 (configurable)
- **Translation Granule**: 4KB (TG = 0)

### Page Table Entries

```c
#define TEST_LEVEL_0_TABLE_VAL      0x000000000e4d1003UL
#define TEST_LEVEL_1_TABLE_VAL      0x000000000e4d2003UL
#define TEST_LEVEL_2_TABLE_VAL      0x000000000e4d3003UL
#define TEST_LEVEL_3_TABLE_VAL      0x40000000ecba743ULL  // AP[2:1] = 0b01
```

### Address Calculation

The device calculates page table addresses using index extraction:

```c
#define TEST_LEVEL_0_TABLE_INDEX    (TEST_GVA >> 39) & 0x1ff
#define TEST_LEVEL_1_TABLE_INDEX    (TEST_GVA >> 30) & 0x1ff
#define TEST_LEVEL_2_TABLE_INDEX    (TEST_GVA >> 21) & 0x1ff
#define TEST_LEVEL_3_TABLE_INDEX    (TEST_GVA >> 12) & 0x1ff
```

## Device Operations

### MMIO Write Handler

The device implements a write handler that triggers translation setup and DMA testing:

```c
static void smmuv3_test_write(void *opaque, hwaddr addr, uint64_t value, unsigned int size)
```

When Hafnium writes to the control register (`SMMUV3TestR_CON`), the device:

1. **Initializes Translation Data**: Sets up STE and CD structures
2. **Populates Page Tables**: Writes page table entries to memory
3. **Performs DMA Operations**: Executes test DMA reads/writes through the IOMMU
4. **Validates Caches**: Tests Secure Configuration and Translation cache accuracy

In the Hafnium environment, this MMIO write is triggered after SMMU initialization completes (S_INIT). For example, in `hafnium/src/arch/aarch64/arm_smmuv3/arm_smmuv3.c`, the function `smmuv3_inv_cfg_tlbs` performs:

```c
mmio_write32((void *)0x9060000, 0x1234);
```

### DMA Testing

The device performs DMA operations using QEMU's address space interfaces:

```c
static void smmuv3_test_fill_data(AddressSpace *as, uint64_t write_addr, uint64_t write_val)
{
    MemTxAttrs attrs = (MemTxAttrs) { .secure = 1 };
    MemTxResult ret;
    
    ret = address_space_write(as, write_addr, attrs, &write_val, sizeof(write_val));
    ret = address_space_read(as, write_addr, attrs, &write_val, sizeof(write_val));
}
```

## Nested Translation Support

For nested translation testing, the device configures complex page table hierarchies:

### Context Descriptor Translation

- **CD S2 Level 0-3 Addresses**: Handle CD entry translation through Stage 2
- **TTB Translation**: Translates Stage 1 translation table base addresses

### IOVA Translation Chain

The device supports nested translation chains where:
1. Stage 1 translates GVA to IPA
2. Stage 2 translates IPA to PA
3. Multiple levels of page tables are traversed for each stage

## Integration with SMMU

### Device Registration

The device registers with the SMMU through:

```c
s->dma_as = smmu_plat_dev_find_add_as(g_smmuv3_state, SMMUV3TEST_SID);
```

### Modifications to SMMU Core

- **Reduced Stream ID Size**: Modified for platform device compatibility
- **Simplified Queue Management**: Reduced queue sizes for testing
- **Platform Device Support**: Added conditional compilation for platform vs. PCIe devices

## Usage and Testing

### Triggering Tests

1. **Hafnium Initialization**: After Hafnium completes initialization (writes `S_INIT`)
2. **MMIO Write**: Write to address `0x09060000` to trigger the test device
3. **Translation Exercise**: The device automatically performs translation tests
4. **Cache Validation**: Multiple DMA operations test cache coherency

### Test Scenarios

The device supports testing:
- **Single Stage Translation**: Stage 1 or Stage 2 only
- **Nested Translation**: Both stages enabled
- **Cache Behavior**: Secure Configuration and Translation cache accuracy
- **Error Handling**: Translation fault scenarios
- **Performance**: Translation latency and throughput

## Build Configuration

The device is conditionally compiled using:

```c
#if ENABLE_PLAT_DEV_SMMU
// SMMUv3 test device code
#endif
```

This allows the feature to be enabled/disabled at build time without affecting the main SMMU implementation.

## Conclusion

The SMMUv3 Test Device provides a comprehensive testing framework for secure translation flows in QEMU's SMMUv3 implementation. By simulating a platform device with IOMMU capabilities, it enables thorough testing of translation tables, caching mechanisms, and nested translation scenarios without requiring complex modifications to existing hypervisor code.
