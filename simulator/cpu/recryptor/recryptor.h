
#include <core/common.h>

#define REC_DEBUG 0

typedef enum _recryptor_op {
    AN = 1,
    OR,
    XR,
    CP,
    NOT,
    SF1,
    SF4,
    LS64,
    RS64,
    ROTL64,
    XROTX,
    KEY,
    SS,
    MC,
    InvalidOp
} recryptor_op;

/* In-memory Single-cycle execution */
void recryptor_decoder_wr(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ); 

/* In-memory Multiple-cycle executions */
void recryptor_decoder_eccirt(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ); 
void recryptor_decoder_eccrdt(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ); 
void recryptor_decoder_eccexe(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ); 

void recryptor_mem_rd(uint32_t Rshift, uint32_t addr, bool debugger __attribute__ ((unused)) );
/* In-memory memory offset for intermediate data */
extern const int LIM_ADDR_OFFSET;    
 
extern const uint8_t NUM_SUBBANK[];
extern const uint8_t NUM_PREVTOT_SUBBANK[];
extern const char    *OpNames[]; 

/* total # of In-memory executions */
extern int recryptor_cnt;

/* In addition to pipeline_tick() for each cycle */
void recryptor_tick(void);

// HARDWARE
#define ADDR_A  	0x00005500
#define ADDR_B  	0x00005600
#define ADDR_C  	0x00005700
#define ADDR_T  	0x00005800
#define ADDR_IR 	0x00006800  
#define ADDR_IR_T 	0x00006900

#define IDRA ((ADDR_A >> 8) & 0x7F)
#define IDRB ((ADDR_B >> 8) & 0x7F)
#define IDRC ((ADDR_C >> 8) & 0x7F)
#define IDRT ((ADDR_T >> 8) & 0x7F)
#define IDRIR ((ADDR_IR >> 8) & 0x7F)
#define IDRIRT ((ADDR_IR_T >> 8) & 0x7F)

#define BANK 1
// RELIC
/** Irreducible polynomial size in bits. */
#define FB_POLYN 233

/** Size of word in this architecture. */
#define WORD     32

/* Size in bits of a digit.*/
#define DIGIT	(WORD)
#define DIGIT_LOG		5

/**
 * Precision in bits of a binary field element.
 */
#define FB_BITS 	((int)FB_POLYN)

/**
 * Size in bits of a digit.
 */
#define FB_DIGIT	((int)DIGIT)

/**
 * Logarithm of the digit size in base 2.
 */
#define FB_DIG_LOG	((int)DIGIT_LOG)

/**
 * Size in digits of a block sufficient to store a binary field element.
 */
#define FB_DIGS		((int)((FB_BITS)/(FB_DIGIT) + (FB_BITS % FB_DIGIT > 0)))
#define FB_MOD 		((int)(FB_BITS % FB_DIGIT))

