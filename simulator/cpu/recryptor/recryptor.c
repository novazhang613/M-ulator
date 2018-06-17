#include <stdbool.h>
#include <stdint.h>

#include "cpu/core.h" 
#include "cpu/recryptor/recryptor.h"

void recryptor_decoder_wr(uint32_t addr, uint32_t val,
		bool debugger __attribute__ ((unused)) ) {

    printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);
    //*(DECODER) = Idra + (Idrb<<8) + (Idrc<<16) + (1<<23) + (3<<24) + (3<<28); // 1<<23+3<<24

	// Decode base address
	int addrA = ((val)     & 0x7F) << 8;
	int addrB = ((val>> 8) & 0x7F) << 8;
	int addrC = ((val>>16) & 0x7F) << 8;
    	printf("Recryptor: addrA = %#x, addrB = %#x, addrC = %#x\n", addrA, addrB, addrC);

	// Decode #subBanks 
	recryptor_op op = (recryptor_op)((val>>24) & 0xF);
	int bank = ((val>>28) & 0xF);
    	printf("recryptor: op = %#x, bank = %#x\n", op, bank); 
	int block = 0;
	switch (bank) {
	  case(0x1) : block = 8;break;
	  case(0x3) : block = 8+2;break;
	  case(0x7) : block = 8+2+4;break;
	  case(0x15): block = 16;break;  
	  default:  block = 0;
	}
	printf("Recryptor: block = %d\n",block);

	// Operations on subBanks
        int i;
	for (i=0;i<block;i++) {	
	 	uint32_t dataA = read_word(addrA + i * 4 );
	 	uint32_t dataB = read_word(addrB + i * 4 );
	 	//uint32_t dataC = dataA ^ dataB; 
		uint32_t dataC;
		switch (op) {
			case XR:
				dataC = dataA ^ dataB;
				break;
			default: 
				dataC = dataA;
		}

    	 	printf("Recryptor: dataA = %#x, dataB = %#x, dataC = %#x\n", dataA, dataB, dataC);
	 	write_word(addrC + i * 4, dataC);
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
}

