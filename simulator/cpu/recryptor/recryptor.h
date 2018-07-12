
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

void recryptor_decoder_wr(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ); 

extern const int LIM_ADDR_OFFSET;    
 
extern const uint8_t NUM_SUBBANK[];
extern const uint8_t NUM_PREVTOT_SUBBANK[];
extern const char    *OpNames[]; 
extern int recryptor_cnt;
