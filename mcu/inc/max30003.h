#ifndef __MAX30003_H__
#define __MAX30003_H__

#include "stm32f4xx_hal.h"
#include <stdbool.h>

// Register addresses
#define INFO 		0x0F
#define STATUS      0x01
#define EN_INT      0x02
#define EN_INT2		0x03
#define MNGR_INT	0x04
#define MNGR_DYN    0x05
#define SW_RST		0x08
#define SYNCH       0x09
#define CNFG_GEN    0x10
#define CNFG_CAL	0x12
#define CNFG_EMUX   0x14
#define CNFG_ECG    0x15
#define CNFG_RTOR   0x1D
#define CNFG_RTOR2	0x1E
#define FIFO_RST    0x0A

// simple name+address pair
typedef struct {
    const char *name;
    uint8_t     addr;
} RegInfo_t;

// these symbols are defined in max30003.c
extern const RegInfo_t max30003_regs[];
extern const size_t   max30003_reg_count;

// STATUS register (0x01) (fixed) (read-only)
typedef union {
	uint32_t all;
	struct {
		uint32_t LDOFF_NL	    :1;
		uint32_t LDOFF_NH	    :1;
		uint32_t LDOFF_PL	    :1;
		uint32_t LDOFF_PH	    :1;
		uint32_t reserved4	    :4;
		uint32_t PLLINT		    :1;
		uint32_t SAMP		    :1;
		uint32_t RRINT		    :1;
		uint32_t LONINT		    :1;
		uint32_t reserved8      :8;
		uint32_t DCLOFFINT      :1;
		uint32_t FSTINT		    :1;
		uint32_t EOVF		    :1;
		uint32_t EINT		    :1;
		uint32_t reserved8_1    :8; // extra reserves since 24/32 bits are used in the register
	} bits;
} STATUS_Reg_t;

// EN_INT register (0x02) (fixed)
typedef union {
    uint32_t all;
    struct {
        uint32_t INTB_TYPE		:2;  // bits 0:1
        uint32_t reserved6		:6;
        uint32_t EN_PLLINT		:1;  // bit 8
        uint32_t EN_SAMP		:1;  // bit 9
        uint32_t EN_RRINT		:1;  // bit 10
        uint32_t EN_LONINT		:1;  // bit 11
        uint32_t reserved8		:8;
        uint32_t EN_DCLOFFINT	:1;
		uint32_t EN_FSTINT		:1;
		uint32_t EN_EOVF		:1;
		uint32_t EN_EINT		:1;
		uint32_t reserved8_1	:8;
    } bits;
} EN_INT_Reg_t;


// EN_INT2 register (0x03) (fixed)
typedef union {
    uint32_t all;
    struct {
        uint32_t INTB_TYPE		:2;  // bits 0:1
        uint32_t reserved6		:6;
        uint32_t EN_PLLINT		:1;  // bit 8
        uint32_t EN_SAMP		:1;  // bit 9
        uint32_t EN_RRINT		:1;  // bit 10
        uint32_t EN_LONINT		:1;  // bit 11
        uint32_t reserved8		:8;
        uint32_t EN_DCLOFFINT	:1;
		uint32_t EN_FSTINT		:1;
		uint32_t EN_EOVF		:1;
		uint32_t EN_EINT		:1;
		uint32_t reserved8_1	:8;
    } bits;
} EN_INT2_Reg_t;

// MNGR_INT register (0x04) (LLM-generated, sanity checked)
typedef union {
    uint32_t all;
    struct {
        uint32_t SAMP_IT      : 2;  // D[1:0]   Sample Sync Pulse frequency
        uint32_t CLR_SAMP     : 1;  // D[2]     Sample Sync Pulse clear behavior
        uint32_t reserved1    : 1;  // D[3]     unused
        uint32_t CLR_RRINT    : 2;  // D[5:4]   R-R Detect interrupt clear behavior
        uint32_t CLR_FAST     : 1;  // D[6]     FAST-mode interrupt clear behavior
        uint32_t reserved12   :12;  // D[18:7]  unused
        uint32_t EFIT         : 5;  // D[23:19] FIFO interrupt threshold (EFIT[4:0])
        uint32_t reserved8	  : 8;
    } bits;
} MNGR_INT_Reg_t;

// MNGR_DYN register (0x05)
typedef union {
	uint32_t all;
	struct {
		uint32_t reserved16		:16;
		uint32_t FAST_TH		:6;
		uint32_t FAST			:2;
		uint32_t reserved8		:8;
	} bits;
} MNGR_DYN_Reg_t;

// 24 zeroes array (FIFO_RST, SYNCH commands)
static const uint8_t zero24[3] = {0x00, 0x00, 0x00};

// CNFG_GEN register (0x10)
typedef union {
    uint32_t all;
    struct {
        uint32_t RBIASN      :1;  // bit 0
        uint32_t RBIASP      :1;  // bit 1
        uint32_t RBIASV      :2;  // bits 3:2
        uint32_t EN_RBIAS    :2;  // bits 5:4
        uint32_t DCLOFF_VTH  :2;  // bits 7:6
        uint32_t DCLOFF_IMAG :3;  // bits 10:8
        uint32_t DCLOFF_IPOL :1;  // bit 11
        uint32_t EN_DCLOFF   :2;  // bits 13:12
        uint32_t reserved5   :5;  // bits 18:14
        uint32_t EN_ECG      :1;  // bit 19
        uint32_t FMSTR       :2;  // bits 21:20
        uint32_t EN_ULP_LON  :2;  // bits 23:22
        uint32_t reserved8   :8;
    } bits;
} CNFG_GEN_Reg_t;

// CNFG_CAL register (0x12)
typedef union {
    uint32_t all;
    struct {
        uint32_t THIGH		:11; // bits 10:0
        uint32_t FIFTY		:1;  // bit 11
        uint32_t FCAL		:3;  // bits 14:12
        uint32_t reserved5  :5;
        uint32_t VMAG		:1;  // bit 20
        uint32_t VMODE		:1;  // bit 21
        uint32_t EN_VCAL	:1;  // bit 22
        uint32_t reserved9	:9;
    } bits;
} CNFG_CAL_Reg_t;

// CNFG_EMUX register (0x14)
typedef union {
    uint32_t all;
    struct {
    	uint32_t reserved16	:16; // bits 15:0
    	uint32_t CALN_SEL	:2;  // bits 17:16
    	uint32_t CALP_SEL	:2;  // bits 19:18
    	uint32_t OPEN_N		:1;  // bits 20
    	uint32_t OPEN_P		:1;  // bits 21
    	uint32_t reserved1	:1;
    	uint32_t POL		:1;  // bits 23
    	uint32_t reserved8	:8;
    } bits;
} CNFG_EMUX_Reg_t;

// CNFG_ECG register (0x15)
typedef union {
	uint32_t all;
	struct {
		uint32_t reserved12	:12;
		uint32_t DLPF		:2; // bits 13:12
		uint32_t DHPF		:1; // bits 14
		uint32_t reserved1	:1;
		uint32_t GAIN		:2; // bits 17:16
		uint32_t reserved4	:4;
		uint32_t RATE		:2; // bits 23:22
		uint32_t reserved8	:8;
	} bits;
} CNFG_ECG_Reg_t;

// CNFG_RTOR register (0x1D)
typedef union {
	uint32_t all;
	struct {
		uint32_t reserved8	    :8;
		uint32_t PTSF		    :4; // bits 8:11
		uint32_t PAVG		    :2; // bits 13:12
		uint32_t reserved1	    :1;
		uint32_t EN_RTOR	    :1; // bits 15
		uint32_t GAIN		    :4; // bits 19:16
		uint32_t WNDW		    :4; // bits 23:20
		uint32_t reserved8_1    :8;
	} bits;
} CNFG_RTOR_Reg_t;

// CNFG_RTOR2 register (0x1E)
typedef union {
	uint32_t all;
	struct {
		uint32_t reserved8	:8; // bits 7:0
		uint32_t RHSF		:3; // bits 11:8
		uint32_t reserved1	:1; // bits 13:12
		uint32_t RAVG		:2;
		uint32_t reserved2	:2; // bits 15
		uint32_t HOFF		:6; // bits 19:16
		uint32_t reserved10 :10;
	} bits;
} CNFG_RTOR2_Reg_t;


void MAX30003_Init(SPI_HandleTypeDef *hspi, UART_HandleTypeDef *uart);
void MAX30003_InitRegisters(void);
uint8_t  MAX30003_SanityCheck(void);
uint32_t MAX30003_ReadRegister(uint8_t reg);
void     MAX30003_WriteRegister(uint8_t reg, uint32_t data);
void MAX30003_BurstReadECG(uint32_t sampleCount, int32_t *outBuffer);
void MAX30003_DumpAllRegs(UART_HandleTypeDef *uart);
void MAX30003_DumpReg(unsigned addr, UART_HandleTypeDef *uart);

#endif /* __MAX30003_H__ */
