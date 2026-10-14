#include "platform.h"
#include "quadspi.h"
#include "fmc.h"

static void command(uint32_t opcode, uint8_t *data, uint32_t count, int write)
{
    QSPI_CommandTypeDef c = {0};
    c.Instruction = opcode;
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.AddressMode = QSPI_ADDRESS_NONE;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    c.DataMode = count ? QSPI_DATA_1_LINE : QSPI_DATA_NONE;
    c.NbData = count;
    c.DdrMode = QSPI_DDR_MODE_DISABLE;
    c.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    c.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    if (HAL_QSPI_Command(&hqspi, &c, 100) != HAL_OK) { giga_fail(0x201); }
    if (count && (write ? HAL_QSPI_Transmit(&hqspi, data, 100)
                        : HAL_QSPI_Receive(&hqspi, data, 100)) != HAL_OK) { giga_fail(0x202); }
}

void giga_qspi_initialize(void)
{
    MX_QUADSPI_Init();
    command(0x66, 0, 0, 0); /* reset-enable, reset; internal/RAM code only */
    command(0x99, 0, 0, 0);
    HAL_Delay(1);
    command(0xAB, 0, 0, 0); /* release deep power-down */
    HAL_Delay(1);
    uint8_t id[3];
    command(0x9F, id, sizeof id, 0);
    volatile GigaHandoff *h = (volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
    h->qspi_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
    /* Adesto/Renesas manufacturer, 128 Mbit density. No writes to unknown parts. */
    if (h->qspi_id != 0x1F8901u) { giga_fail(0x203); }
    QSPI_CommandTypeDef c = {0};
    c.Instruction = 0x5A;
    c.InstructionMode = QSPI_INSTRUCTION_1_LINE;
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.AddressSize = QSPI_ADDRESS_24_BITS;
    c.DataMode = QSPI_DATA_1_LINE;
    c.DummyCycles = 8;
    c.NbData = 4;
    c.DdrHoldHalfCycle = QSPI_DDR_HHC_ANALOG_DELAY;
    c.SIOOMode = QSPI_SIOO_INST_EVERY_CMD;
    uint8_t sfdp[4];
    if (HAL_QSPI_Command(&hqspi, &c, 100) != HAL_OK ||
        HAL_QSPI_Receive(&hqspi, sfdp, 100) != HAL_OK) { giga_fail(0x204); }
    h->qspi_sfdp = (uint32_t)sfdp[0] | ((uint32_t)sfdp[1] << 8) |
        ((uint32_t)sfdp[2] << 16) | ((uint32_t)sfdp[3] << 24);
    if (h->qspi_sfdp != 0x50444653u) { giga_fail(0x205); }
    uint8_t sr2;
    command(0x35, &sr2, 1, 0);
    if (!(sr2 & 2u)) {
        /* Volatile QE only: do not modify factory nonvolatile status/protection. */
        uint8_t wanted = sr2 | 2u;
        command(0x50, 0, 0, 0);
        command(0x31, &wanted, 1, 1);
        HAL_Delay(1);
        command(0x35, &sr2, 1, 0);
        if (sr2 != wanted) { giga_fail(0x206); }
    }
    c.Instruction = 0x6B;
    c.DataMode = QSPI_DATA_4_LINES;
    c.NbData = 0;
#if defined(GIGA_MEMORY_HELPER)
    /* The debug AP can read ahead past the final requested word. Give its
       mapped reads a guard window so a complete 16 MiB dump does not stall
       the bus at the controller's address limit. Flash commands stay 24-bit;
       the physical device and the saved backup are still exactly 16 MiB. */
    MODIFY_REG(hqspi.Instance->DCR, QUADSPI_DCR_FSIZE, 24u << QUADSPI_DCR_FSIZE_Pos);
#endif
    QSPI_MemoryMappedTypeDef map = { .TimeOutActivation = QSPI_TIMEOUT_COUNTER_DISABLE };
    if (HAL_QSPI_MemoryMapped(&hqspi, &c, &map) != HAL_OK) { giga_fail(0x207); }
}

static void sdram_command(uint32_t mode, uint32_t refresh, uint32_t mode_register)
{
    FMC_SDRAM_CommandTypeDef c = {
        .CommandMode = mode, .CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1,
        .AutoRefreshNumber = refresh, .ModeRegisterDefinition = mode_register
    };
    if (HAL_SDRAM_SendCommand(&hsdram1, &c, 100) != HAL_OK) { giga_fail(0x210); }
}

void giga_memories_initialize(void)
{
    giga_qspi_initialize();
    MX_FMC_Init();
    sdram_command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0);
    HAL_Delay(1);
    sdram_command(FMC_SDRAM_CMD_PALL, 1, 0);
    sdram_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0);
    /* Burst length 1, sequential, CAS3, standard operating mode, single write. */
    sdram_command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x230);
    if (HAL_SDRAM_ProgramRefreshRate(&hsdram1, 1542) != HAL_OK) { giga_fail(0x211); }
}

static int sdram_failure(uint32_t phase, volatile uint32_t *address,
                         uint32_t expected, uint32_t actual, uint32_t started)
{
    volatile GigaHandoff *h = (volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
    h->sdram_phase = phase;
    h->sdram_address = (uint32_t)address;
    h->sdram_expected = expected;
    h->sdram_actual = actual;
    h->sdram_elapsed_ms = HAL_GetTick() - started;
    return 0;
}

static uint32_t sdram_pattern(uint32_t state)
{
    /* Reproducible xorshift32: exercise varying data across all banks/rows. */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int giga_sdram_test(void)
{
    /* Destructive maintenance test, before applications/graphics own SDRAM.
       Every word tested in both directions, including each bank/address bit. */
    volatile uint32_t *ram = (volatile uint32_t *)0xC0000000u;
    const uint32_t words = (8u * 1024u * 1024u) / sizeof(uint32_t);
    volatile GigaHandoff *h = (volatile GigaHandoff *)GIGA_HANDOFF_ADDRESS;
    const uint32_t started = HAL_GetTick();
    h->sdram_phase = 0;
    h->sdram_address = 0;
    h->sdram_expected = 0;
    h->sdram_actual = 0;
    h->sdram_elapsed_ms = 0;
    h->sdram_bytes = 0;
    for (unsigned phase = 1; phase <= 2; ++phase) {
        h->sdram_phase = phase;
        for (unsigned bit = 0; bit < 32; ++bit) {
            const uint32_t expected = phase == 1 ? 1u << bit : ~(1u << bit);
            ram[0] = expected;
            __DSB();
            const uint32_t actual = ram[0];
            if (actual != expected) { return sdram_failure(phase, ram, expected, actual, started); }
        }
    }
    /* Check NBL0/NBL1 byte masking and halfword stores on the 16-bit bus. */
    h->sdram_phase = 3;
    ram[0] = 0;
    uint32_t expected = 0;
    volatile uint8_t *bytes = (volatile uint8_t *)ram;
    for (unsigned lane = 0; lane < 4; ++lane) {
        const uint8_t value = (uint8_t)(0xA5u ^ (lane * 0x11u));
        bytes[lane] = value;
        expected |= (uint32_t)value << (8 * lane);
        __DSB();
        const uint32_t actual = ram[0];
        if (actual != expected) { return sdram_failure(3, ram, expected, actual, started); }
    }
    h->sdram_phase = 4;
    volatile uint16_t *halfwords = (volatile uint16_t *)ram;
    for (unsigned lane = 0; lane < 2; ++lane) {
        const uint16_t value = lane == 0 ? 0xA55Au : 0x3CC3u;
        halfwords[lane] = value;
        expected = (expected & ~(0xFFFFu << (16 * lane))) | ((uint32_t)value << (16 * lane));
        __DSB();
        const uint32_t actual = ram[0];
        if (actual != expected) { return sdram_failure(4, ram, expected, actual, started); }
    }
    h->sdram_phase = 5;
    for (uint32_t i = 0; i < words; ++i) { ram[i] = i ^ 0xA55A3CC3u; }
    __DSB();
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t expected = i ^ 0xA55A3CC3u;
        const uint32_t actual = ram[i];
        if (actual != expected) { return sdram_failure(5, &ram[i], expected, actual, started); }
        ram[i] = ~(i ^ 0xA55A3CC3u);
    }
    __DSB();
    h->sdram_phase = 6;
    for (uint32_t i = words; i-- != 0;) {
        const uint32_t expected = ~(i ^ 0xA55A3CC3u);
        const uint32_t actual = ram[i];
        if (actual != expected) { return sdram_failure(6, &ram[i], expected, actual, started); }
        ram[i] = 0;
    }
    __DSB();
    h->sdram_phase = 7;
    uint32_t pattern = 0x12345678u;
    for (uint32_t i = 0; i < words; ++i) { ram[i] = pattern = sdram_pattern(pattern); }
    __DSB();
    HAL_Delay(250); /* Several complete 64 ms refresh intervals, no SDRAM access. */
    pattern = 0x12345678u;
    for (uint32_t i = 0; i < words; ++i) {
        pattern = sdram_pattern(pattern);
        const uint32_t actual = ram[i];
        if (actual != pattern) { return sdram_failure(7, &ram[i], pattern, actual, started); }
        ram[i] = 0;
    }
    __DSB();
    h->sdram_phase = 8;
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t actual = ram[i];
        if (actual != 0) { return sdram_failure(8, &ram[i], 0, actual, started); }
    }
    h->sdram_phase = 0;
    h->sdram_elapsed_ms = HAL_GetTick() - started;
    h->sdram_bytes = words * sizeof(uint32_t);
    return 1;
}
