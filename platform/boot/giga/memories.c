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

static void qspi_initialize(void)
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
    qspi_initialize();
    MX_FMC_Init();
    sdram_command(FMC_SDRAM_CMD_CLK_ENABLE, 1, 0);
    HAL_Delay(1);
    sdram_command(FMC_SDRAM_CMD_PALL, 1, 0);
    sdram_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8, 0);
    /* Burst length 1, sequential, CAS3, standard operating mode, single write. */
    sdram_command(FMC_SDRAM_CMD_LOAD_MODE, 1, 0x230);
    if (HAL_SDRAM_ProgramRefreshRate(&hsdram1, 1542) != HAL_OK) { giga_fail(0x211); }
}

int giga_sdram_test(void)
{
    /* Destructive maintenance test, before applications/graphics own SDRAM.
       Every word tested in both directions, including each bank/address bit. */
    volatile uint32_t *ram = (volatile uint32_t *)0xC0000000u;
    const uint32_t words = (8u * 1024u * 1024u) / sizeof(uint32_t);
    for (unsigned bit = 0; bit < 32; ++bit) {
        ram[0] = 1u << bit;
        __DSB();
        if (ram[0] != (1u << bit)) { return 0; }
    }
    for (uint32_t i = 0; i < words; ++i) { ram[i] = i ^ 0xA55A3CC3u; }
    __DSB();
    for (uint32_t i = 0; i < words; ++i) {
        if (ram[i] != (i ^ 0xA55A3CC3u)) { return 0; }
        ram[i] = ~(i ^ 0xA55A3CC3u);
    }
    __DSB();
    for (uint32_t i = words; i-- != 0;) {
        if (ram[i] != ~(i ^ 0xA55A3CC3u)) { return 0; }
        ram[i] = 0;
    }
    __DSB();
    return 1;
}
