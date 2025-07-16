#include "max30003.h"
#include "ecg_processor.h"
#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define ECG_FIFO     			   0x21
#define EINT_STATUS_MASK           (1 << 23)
#define RRINT_STATUS_MASK          (1 << 10)
#define FIFO_OVF_MASK              0x7
#define FIFO_VALID_SAMPLE_MASK     0x0
#define FIFO_FAST_SAMPLE_MASK      0x1
#define ETAG_BITS_MASK             0x7

static SPI_HandleTypeDef *max_spi;
static UART_HandleTypeDef *max_uart;

#define CS_PORT GPIOB
#define CS_PIN  GPIO_PIN_6

#include "max30003.h"

// build the table the defines
const RegInfo_t max30003_regs[] = {
    { "INFO"     , INFO     },
    { "STATUS"   , STATUS   },
    { "EN_INT"   , EN_INT   },
    { "EN_INT2"  , EN_INT2  },
    { "MNGR_INT" , MNGR_INT },
    { "MNGR_DYN" , MNGR_DYN },
    { "SW_RST"   , SW_RST   },
    { "SYNCH"    , SYNCH    },
    { "FIFO_RST" , FIFO_RST },
    { "CNFG_GEN" , CNFG_GEN },
    { "CNFG_CAL" , CNFG_CAL },
    { "CNFG_EMUX", CNFG_EMUX},
    { "CNFG_ECG" , CNFG_ECG },
    { "CNFG_RTOR", CNFG_RTOR},
    { "CNFG_RTOR2",CNFG_RTOR2},
};

volatile bool fifo_int_flag = 0;
const size_t max30003_reg_count = sizeof(max30003_regs) / sizeof(max30003_regs[0]);
uint32_t ecg_sample[32];
uint32_t ETAG[32];


void MAX30003_Init(SPI_HandleTypeDef *hspi, UART_HandleTypeDef *uart) {
    max_spi = hspi;
    max_uart = uart;
}

static void cs_low(void)  { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(CS_PORT, CS_PIN, GPIO_PIN_SET); }

void MAX30003_WriteRegister(uint8_t reg, uint32_t data) {
    cs_low();

    uint8_t tx_buf[4];
    tx_buf[0] = (reg << 1);
    tx_buf[1] = (uint8_t)((0x00FF0000 & data) >> 16);
    tx_buf[2] = (uint8_t)((0x0000FF00 & data) >> 8);
    tx_buf[3] = (uint8_t)(0x000000FF & data);

    HAL_SPI_Transmit(max_spi, tx_buf, sizeof(tx_buf), HAL_MAX_DELAY);

    cs_high();
}

uint32_t MAX30003_ReadRegister(uint8_t reg) {
    cs_low();

    uint8_t tx_buf[4];
    tx_buf[0] = ((reg << 1) | 1);
    tx_buf[1] = 0xFF;
    tx_buf[2] = 0xFF;
    tx_buf[3] = 0xFF;

    uint8_t rx_buf[4]; 

    HAL_SPI_TransmitReceive(max_spi, tx_buf, rx_buf, 4, HAL_MAX_DELAY);

    uint32_t data = 0;
    data  = ((uint32_t)rx_buf[1]) << 16;
    data |= ((uint32_t)rx_buf[2]) <<  8;
    data |= ((uint32_t)rx_buf[3]);

    cs_high();

    return data;
}

uint8_t MAX30003_SanityCheck(void) {
    return (uint8_t)(MAX30003_ReadRegister(INFO) & 0xFF);
}

void MAX30003_InitRegisters(void) {
	// first need to reset
	MAX30003_WriteRegister(SW_RST, 0);

    // CNFG_GEN register
    // key settings are EN_ECG (1 to enable ECG front-end) and FMSTR (master clock divider 32kHz)
    CNFG_GEN_Reg_t cnfg_gen = { .all = 0 };
    cnfg_gen.bits.EN_ECG = 1;
    cnfg_gen.bits.RBIASN = 1;
    cnfg_gen.bits.RBIASP = 1;
    cnfg_gen.bits.EN_RBIAS = 1;
    cnfg_gen.bits.DCLOFF_IMAG = 2;
    cnfg_gen.bits.EN_DCLOFF = 1;
    MAX30003_WriteRegister(CNFG_GEN, cnfg_gen.all);

    // CNFG_ECG register (ALSO CRITICAL TO DEVICE FUNCTIONALITY)
    // might have to play around with these settings later to see what works best
    CNFG_ECG_Reg_t cnfg_ecg = { .all = 0 };
    cnfg_ecg.bits.DLPF = 1;
    cnfg_ecg.bits.DHPF = 1;
    cnfg_ecg.bits.GAIN = 2;
    cnfg_ecg.bits.RATE = 1; // 256
    MAX30003_WriteRegister(CNFG_ECG, cnfg_ecg.all);

    CNFG_RTOR_Reg_t cnfg_rtor1 = { .all = 0 };
    cnfg_rtor1.bits.EN_RTOR = 1;
    cnfg_rtor1.bits.PTSF = 1;
    cnfg_rtor1.bits.GAIN = 3;
    MAX30003_WriteRegister(CNFG_RTOR, cnfg_rtor1.all);

    EN_INT_Reg_t en_int = { .all = 0 };
    en_int.bits.EN_EINT = 1;
    en_int.bits.EN_RRINT = 1;
    en_int.bits.INTB_TYPE = 3;
    MAX30003_WriteRegister(EN_INT, en_int.all);

    MNGR_DYN_Reg_t mngr_dyn = { .all = 0 };
    mngr_dyn.bits.FAST = 0;
    MAX30003_WriteRegister(MNGR_DYN, mngr_dyn.all);

    // CNFG_EMUX register
    CNFG_EMUX_Reg_t cnfg_emux = { .all = 0 };
    cnfg_emux.bits.OPEN_N = 0;
    cnfg_emux.bits.OPEN_P = 0;
    MAX30003_WriteRegister(CNFG_EMUX, cnfg_emux.all);

    // sync
    MAX30003_WriteRegister(SYNCH, 0);

    // log all registers configuration
    MAX30003_DumpAllRegs(max_uart);
}


// this gets called for all EXTI[9:5] lines
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_8) {
        uint32_t status = MAX30003_ReadRegister(STATUS);

        if ((status & EINT_STATUS_MASK) == EINT_STATUS_MASK) {
            uint32_t counter = 0;

            do {
                uint32_t ecg_fifo = MAX30003_ReadRegister(ECG_FIFO);
                ecg_sample[counter] = ecg_fifo;
                ETAG[counter] = (ecg_fifo >> 3) & ETAG_BITS_MASK;
                counter++;
            } while (ETAG[counter-1] == FIFO_VALID_SAMPLE_MASK ||
                     ETAG[counter-1] == FIFO_FAST_SAMPLE_MASK);

            if (ETAG[counter - 1] == FIFO_OVF_MASK) {
                MAX30003_WriteRegister(FIFO_RST, 0);
            }

            for (uint32_t idx = 0; idx < counter; idx++) {
                ECG_Processor_ProcessSample(ecg_sample[idx]);
            }
        }
    }
}


void MAX30003_DumpAllRegs(UART_HandleTypeDef *uart) {
    char buf[64];
    for (size_t i = 0; i < max30003_reg_count; ++i) {
        uint32_t v = MAX30003_ReadRegister(max30003_regs[i].addr);
        int n = snprintf(buf, sizeof(buf),
                         "%-10s (0x%02X) = 0x%06lX\r\n",
                         max30003_regs[i].name,
                         max30003_regs[i].addr,
                         v);
        HAL_UART_Transmit(uart, (uint8_t*)buf, n, HAL_MAX_DELAY);
    }
}

void MAX30003_DumpReg(unsigned addr, UART_HandleTypeDef *uart) {
    char buf[64];
	uint32_t v = MAX30003_ReadRegister(addr);
	int n = snprintf(buf, sizeof(buf),
					 "%-10s (0x%02X) = 0x%06lX\r\n",
					 "REGISTER",
					 addr,
					 v);
	HAL_UART_Transmit(uart, (uint8_t*)buf, n, HAL_MAX_DELAY);
}
