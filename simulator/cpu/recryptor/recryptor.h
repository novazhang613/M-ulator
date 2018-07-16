
#include <core/common.h>

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

/* In-memory memory offset for intermediate data */
extern const int LIM_ADDR_OFFSET;    
 
extern const uint8_t NUM_SUBBANK[];
extern const uint8_t NUM_PREVTOT_SUBBANK[];
extern const char    *OpNames[]; 

/* total # of In-memory executions */
extern int recryptor_cnt;

/* In addition to pipeline_tick() for each cycle */
void recryptor_tick(void);
