#include "platform.h"

static void region(uint32_t number, uint32_t base, uint32_t size, uint32_t execute, uint32_t cache, uint32_t access)
{
    MPU_Region_InitTypeDef r = {0};
    r.Enable = MPU_REGION_ENABLE;
    r.Number = number;
    r.BaseAddress = base;
    r.Size = size;
    r.TypeExtField = MPU_TEX_LEVEL1;
    r.AccessPermission = access;
    r.DisableExec = execute ? MPU_INSTRUCTION_ACCESS_ENABLE : MPU_INSTRUCTION_ACCESS_DISABLE;
    r.IsShareable = cache ? MPU_ACCESS_NOT_SHAREABLE : MPU_ACCESS_SHAREABLE;
    r.IsCacheable = cache ? MPU_ACCESS_CACHEABLE : MPU_ACCESS_NOT_CACHEABLE;
    r.IsBufferable = cache ? MPU_ACCESS_BUFFERABLE : MPU_ACCESS_NOT_BUFFERABLE;
#if defined(CORE_CM4)
    /* Cover data/MSP and the SRAM2 data alias as XN. Execution uses the
       separate 0x10000000 instruction alias for SRAM1 and part of SRAM2. */
    if (number == 7) { r.SubRegionDisable = 0x03; }
#endif
    HAL_MPU_ConfigRegion(&r);
}
void giga_configure_mpu(uint32_t ram_debug)
{
    HAL_MPU_Disable();
    /* Shared queues: normal memory, not Device, for C++ atomics and byte access. */
    region(0, 0x38000000, MPU_REGION_SIZE_64KB, 0, 0, MPU_REGION_FULL_ACCESS);
    region(1, 0x90000000, MPU_REGION_SIZE_16MB, 1, 1, MPU_REGION_PRIV_RO_URO);
    region(2, 0xC0000000, MPU_REGION_SIZE_8MB, 0, 0, MPU_REGION_FULL_ACCESS);
    region(3, 0xC0400000, MPU_REGION_SIZE_2MB, ram_debug, 0, MPU_REGION_FULL_ACCESS);
    region(4, 0xC0600000, MPU_REGION_SIZE_2MB, 0, 1, MPU_REGION_FULL_ACCESS);
    /* The maintenance helper executes here without any flash dependency. */
#if defined(GIGA_MEMORY_HELPER)
    region(5, 0x24000000, MPU_REGION_SIZE_256KB, 1, 0, MPU_REGION_FULL_ACCESS);
#else
    region(5, 0x24000000, MPU_REGION_SIZE_256KB, 0, 1, MPU_REGION_FULL_ACCESS);
#endif
    region(6, 0x24040000, MPU_REGION_SIZE_256KB, 0, 0, MPU_REGION_FULL_ACCESS);
#if defined(CORE_CM4)
    region(7, 0x30000000, MPU_REGION_SIZE_512KB, 0, 0, MPU_REGION_FULL_ACCESS);
#else
    region(7, 0x20000000, MPU_REGION_SIZE_128KB, 0, 0, MPU_REGION_FULL_ACCESS);
#endif
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
    __DSB();
    __ISB();
}
