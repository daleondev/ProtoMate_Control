#include "platform.h"
#include "main.h"

extern uint32_t _sdata, _edata, _sidata, _sbss, _ebss, g_pfnVectors[];
volatile uint32_t giga_boot_status;

void giga_safe_outputs(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIOC->BSRR = GPIO_PIN_7; /* Existing interface: HIGH disables all drivers. */
    GPIOA->BSRR = (GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3) << 16;
    GPIO_InitTypeDef pins = { .Pin = GPIO_PIN_7, .Mode = GPIO_MODE_OUTPUT_PP,
        .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW };
    HAL_GPIO_Init(GPIOC, &pins);
    pins.Pin = GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3;
    HAL_GPIO_Init(GPIOA, &pins);
}

void giga_fail(uint32_t reason)
{
    __disable_irq();
    giga_safe_outputs();
    giga_boot_status = reason;
    ((volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS)->status = reason;
    __DSB();
#if defined(GIGA_MEMORY_HELPER)
    __BKPT(0); /* Report a helper failure immediately to the attached debugger. */
#endif
    for (;;) { __WFI(); }
}

void Error_Handler(void) { giga_fail(0x100); }
void HardFault_Handler(void) { giga_fail(0x101); }
void MemManage_Handler(void) { giga_fail(0x102); }
void BusFault_Handler(void) { giga_fail(0x103); }
void UsageFault_Handler(void) { giga_fail(0x104); }
void SysTick_Handler(void)
{
    HAL_IncTick();
    /* Also bounds Cube's voltage-ready loops, which otherwise have no timeout. */
    if (giga_boot_status < 0x20 && HAL_GetTick() > 5000) { giga_fail(0x105); }
}

#if !defined(GIGA_MEMORY_HELPER)
static void prepare_jump(uint32_t vectors)
{
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    for (unsigned i = 0; i < 8; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR = vectors;
    __DSB(); __ISB();
}

#endif

#if defined(CORE_CM7)
void PeriphCommonClock_Config(void);
void SystemClock_Config(void);
#if !defined(GIGA_MEMORY_HELPER)
static void load_image(const GigaImage *image)
{
    for (uint32_t i = 0; i < image->segment_count; ++i) {
        const GigaSegment *s = &image->segments[i];
        uint8_t *to = (uint8_t *)s->run;
        const uint8_t *from = (const uint8_t *)s->load;
        for (uint32_t j = 0; j < s->size; ++j) {
            to[j] = s->flags == GIGA_ZERO ? 0 : from[j];
        }
    }
    __DSB();
    const uint32_t *vectors = (const uint32_t *)image->vectors;
    if (vectors[0] != image->stack || vectors[1] != image->entry) { giga_fail(0x303); }
}
#endif
#endif

void giga_start(void)
{
    /* Never call a clock-resetting SystemInit in an externally loaded image. */
    SCB->CPACR |= 0xFu << 20;
    __DSB(); __ISB();
#if defined(CORE_CM7)
    SCB_DisableICache();
    SCB_DisableDCache();
    SCB->ITCMCR = SCB_ITCMCR_EN_Msk;
    SCB->DTCMCR = SCB_DTCMCR_EN_Msk;
    __DSB(); __ISB();
#endif
    uint32_t *to = &_sdata;
    for (const uint32_t *from = &_sidata; to < &_edata;) { *to++ = *from++; }
    for (to = &_sbss; to < &_ebss;) { *to++ = 0; }
    SCB->VTOR = (uint32_t)g_pfnVectors;
    __HAL_RCC_HSEM_CLK_ENABLE();
    __HAL_RCC_D2SRAM1_CLK_ENABLE();
    __HAL_RCC_D2SRAM2_CLK_ENABLE();
    __HAL_RCC_D2SRAM3_CLK_ENABLE();
    volatile GigaHandoff *h = (volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
#if defined(CORE_CM4)
    /* Always require a fresh M7 release; retained SRAM is not a boot request. */
    HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(0));
    HAL_PWREx_ClearPendingEvent();
    HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);
    __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(0));
    HAL_HSEM_DeactivateNotification(__HAL_HSEM_SEMID_TO_MASK(0));
    __DMB();
    if (h->magic != GIGA_HANDOFF_MAGIC || !(h->m4_entry & 1u)) { giga_fail(0x401); }
    giga_configure_mpu(h->m4_flags & GIGA_RAM_DEBUG);
    prepare_jump(h->m4_vectors);
    giga_jump(h->m4_entry, h->m4_stack);
#else
    giga_safe_outputs();
    SystemCoreClockUpdate();
    __enable_irq();
    if (HAL_Init() != HAL_OK) { giga_fail(0x106); }
    giga_boot_status = 1;
#if !defined(GIGA_MEMORY_HELPER)
    const uint32_t deadline = HAL_GetTick();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) {
        if (HAL_GetTick() - deadline >= 1000) { giga_fail(0x107); }
    }
#endif
    h->magic = 0;
    h->status = 0;
    SystemClock_Config();
    PeriphCommonClock_Config();
    giga_peripheral_clocks_initialize();
    giga_configure_mpu(0);
    giga_boot_status = 0x20;
#if defined(GIGA_MEMORY_HELPER)
    /* A factory flash backup must not depend on working SDRAM. Explicit
       memory tests and flashing still require the complete SDRAM check. */
    h->memory_test = 0;
    if (h->maintenance_request == GIGA_MAINTENANCE_TEST_SDRAM) {
        giga_memories_initialize();
        giga_boot_status = 0x21;
        h->memory_test = giga_sdram_test() ? 1 : 0;
        if (!h->memory_test) { giga_fail(0x212); }
    } else {
        giga_qspi_initialize();
    }
#else
    giga_memories_initialize();
    giga_boot_status = 0x21;
#endif
#if defined(GIGA_MEMORY_HELPER)
    giga_boot_status = 0x600D;
    h->status = 0x600D;
    __DSB();
    __BKPT(0);
    for (;;) { __WFI(); }
#else
    const GigaManifest *manifest = (const GigaManifest *)GIGA_MANIFEST_ADDRESS;
    if (!giga_manifest_valid(manifest)) { giga_fail(0x301); }
    for (unsigned i = 0; i < 2; ++i) {
        const GigaImage *image = &manifest->images[i];
        if (!giga_image_valid(image, manifest->pair_id, (const void *)image->storage, image->size)) {
            giga_fail(0x302);
        }
    }
    load_image(&manifest->images[0]);
    load_image(&manifest->images[1]);
    const GigaImage *m4 = &manifest->images[0], *m7 = &manifest->images[1];
    h->m4_entry = m4->entry;
    h->m4_vectors = m4->vectors;
    h->m4_stack = m4->stack;
    h->m4_flags = m4->flags;
    h->status = 0x600D;
    prepare_jump(m7->vectors);
    giga_jump(m7->entry, m7->stack);
#endif
#endif
}
