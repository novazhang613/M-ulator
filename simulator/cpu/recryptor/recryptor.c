#include <stdbool.h>
#include <stdint.h>

#include "cpu/core.h" 
#include "cpu/recryptor/recryptor.h"
#include "cpu/m3_prc_v9/memmap.h"

const uint8_t NUM_SUBBANK[] = {8,2,4,2};
const uint8_t NUM_PREVTOT_SUBBANK[] = {0,8,10,14};
const char    *OpNames[] = {"AND","OR","XOR","COPY","NOT","SF1","SF4","LS64","RS64","ROTL64","XROTX","KEY","SS","MC","InvalidOp"};

const int LIM_ADDR_OFFSET = 0x2000; 

int recryptor_cnt = 0;

void recryptor_decoder_wr(uint32_t addr, uint32_t val,
		bool debugger __attribute__ ((unused)) ) {

	assert((addr == RECRYPTOR_DECODER_ADDR));

    	//printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);

	// Decode base address
	int addrA = (((val)     & 0x7F) << 8) + LIM_ADDR_OFFSET;
	int addrB = (((val>> 8) & 0x7F) << 8) + LIM_ADDR_OFFSET;
	int addrC = (((val>>16) & 0x7F) << 8) + LIM_ADDR_OFFSET;

	recryptor_op op = (recryptor_op)((val>>24) & 0xF);
	int bank = ((val>>28) & 0xF);
	// Debug
	if(0) printf("Recryptor: addrA = %#x, addrB = %#x, addrC = %#x, Bank = %x, Op = %s\n", addrA, addrB, addrC, bank,OpNames[op-1]);

	uint8_t b;
	bool sh1 = 0;
	uint8_t sh4 = 0;

	for (b =0; b<4; b++) {
		
		if (bank & (1<<b)) {
			uint8_t subb; 
			//printf("subbank %d has %d of subbanks will be executed operation %s\n",b,NUM_SUBBANK[b],OpNames[op-1]);

			for(subb=0; subb < NUM_SUBBANK[b]; subb++) {

				uint32_t byte_offset = ( subb + NUM_PREVTOT_SUBBANK[b]) * 4;
				//printf("byte_offset:%d  ",byte_offset);

	 			uint32_t dataA = read_word(addrA + byte_offset );
	 			uint32_t dataB = read_word(addrB + byte_offset );
				uint32_t dataC;

				switch (op) {
					case AN:
						dataC = dataA & dataB;
						break;
					case OR:
						dataC = dataA | dataB;
						break;
					case XR:
						dataC = dataA ^ dataB;
						break;
					case CP:
						dataC = dataA;
						break;
					case NOT:
						dataC = ~dataA;
						break;
					case SF1:
						dataC = (dataA<<1 | sh1);
						sh1 = (dataA>>31) & 0x1; 
						break;
					case SF4:
						dataC = (dataA<<4 | sh4);
						sh4 = (dataA>>28) & 0xF;
						break;
					default: 
						dataC = dataA;
				}

	 			write_word(addrC + byte_offset, dataC);
				// Debug
    	 			if(0) printf("	dataA = %08x, dataB = %08x, dataC = %08x\n", dataA, dataB, dataC);
			} 
		}
	}

	recryptor_cnt++;
	// Debug
	if(0) printf("Recryptor Count: %d\n",recryptor_cnt);
	
}
/*
    uint32_t start = 0xa0000140;
    uint32_t function = 0xa0000144;
    uint32_t bitwidth = 0xa0000148;
    if (addr == start) {
        start_recryotr();
    } else if (addr == function) {
   set_function();
    }
*/

#if 0
	int block = 0;
	switch (bank) {
	  case(0x1) : block = 8;break;
	  case(0x3) : block = 8+2;break;
	  case(0x7) : block = 8+2+4;break;
	  case(0x15): block = 16;break;  
	  default:  block = 0;
	}
	printf("Recryptor: block = %d\n",block);
#endif
