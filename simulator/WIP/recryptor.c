#include <stdbool.h>
#include <stdint.h>

#include "cpu/core.h" 
#include "cpu/recryptor/recryptor.h"
#include "cpu/m3_prc_v9/memmap.h"

const uint8_t NUM_SUBBANK[] = {8,2,4,2};
const uint8_t NUM_PREVTOT_SUBBANK[] = {0,8,10,14};
const char    *OpNames[] = {"AND","OR","XOR","COPY","NOT","SF1","SF4","LS64","RS64","ROTL64","XROTX","KEY","SS","MC","InvalidOp"};

int recryptor_cnt = 0;

struct recryptor_action {
    (void*) fn(void);
    uint32_t value;
    bool flag;
    struct recryptor_action* next;
};

struct recrytor_action_list {
    struct recryptor_action* head;
    struct recryptor_action* tail;
};

struct recryptor_action_list* recryptor_state = NULL;

void pushRecryptorAction(struct recryptor_state* action) {
   if (recryptor_state->head == NULL) {
        recryptor_state->head = action;
        recryptor_state->tail = action;
    } else {
        recryptor_state->tail->next = action;
        recryptor_state->tail = action;
    }
}

void popRecryptorAction() {
   /* I will let my baobao write this part*/
}

void addRecryptorAction((void*) fn(void), uint32_t value, bool flag) {
    struct recryptor_action* action = (struct recryptor_action*) malloc(sizeof(struct recryptor_action));
   action->fn= fn;
   action->value = value;
  action->flag = flag;
  action->next= NULL;
}

void recryptor_decoder_wr(uint32_t addr, uint32_t val,
		bool debugger __attribute__ ((unused)) ) {

	assert((addr == RECRYPTOR_DECODER_ADDR));

    	//printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);

	// Decode base address
	int addrA = ((val)     & 0x7F) << 8;
	int addrB = ((val>> 8) & 0x7F) << 8;
	int addrC = ((val>>16) & 0x7F) << 8;

	recryptor_op op = (recryptor_op)((val>>24) & 0xF);
	int bank = ((val>>28) & 0xF);
	// Debug
	if(1) printf("Recryptor: addrA = %#x, addrB = %#x, addrC = %#x, Bank = %x, Op = %s\n", addrA, addrB, addrC, bank,OpNames[op-1]);

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
    	 			if(1) printf("	dataA = %08x, dataB = %08x, dataC = %08x\n", dataA, dataB, dataC);
			} 
		}
	}

	recryptor_cnt++;
	// Debug
	if(0) printf("Recryptor Count: %d\n",recryptor_cnt);
	
}


void recryptor_decoder_ecc_irTable(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {
	assert((addr == (RECRYPTOR_DECODER_ADDR+1)));

    	//printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);

	// Decode base address
	//int addr_ir  = ((val)     & 0x7F) << 8;
	//int addr_irt = ((val>> 8) & 0x7F) << 8;
	//int FB_POLYN = ((val>>16) & 0x7FF);
	//int NUM_PLN_Minus1 = ((val>>27) & 0x1);
	int bank     = ((val>>28) & 0xF);

	int Idrir  = ((val)     & 0x7F);
	int Idrirt = ((val>> 8) & 0x7F);

	// precompute table t[1] = b
        addRecryptorAction(&recryptor_decoder_wr, value, flag);
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
			     	(Idrir + ((Idrirt+1)<<16) + (1<<23) + (4<<24) + (bank<<28)),
			     	false);

	// precompute table t[2] = t[1] << 1
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
				(Idrir + ((Idrirt+2)<<16) + (1<<23) + (6<<24) + (bank<<28)),
			     	false);

	// precompute table t[3] = t[1] ^ t[2]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+2) + ((Idrirt+1)<<8) + ((Idrirt+3)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);
#if 0
	// precompute table t[4] = t[2] << 1
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+2) + ((Idrirt+4)<<16) + (1<<23) + (6<<24) + (bank<<28)), 
			     	false);

	// precompute table t[5] = t[1] ^ t[4]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+4) + ((Idrirt+1)<<8) + ((Idrirt+5)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// precompute table t[6] = t[2] ^ t[4] 
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+4) + ((Idrirt+2)<<8) + ((Idrirt+6)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// precompute table t[7] = t[1] ^ t[6]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+6) + ((Idrirt+1)<<8) + ((Idrirt+7)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// precompute table t[8] = t[4] << 1
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+4) + ((Idrirt+8)<<16) + (1<<23) + (6<<24) + (bank<<28)), 
			     	false);

	/*
	// t[8] = t[8] ^ ir_t[u]
	u = (addr_ir[FB_DIGS-1] >> (FB_MOD-3)) & 0x1; // grab the 3rd bit  
	if (u==1) {
			((Idrirt+8) + ((Idrir)<<8) + ((Idrirt+8)<<16) + (1<<23) + (3<<24) + (bank<<28)),
	}
			// else if u ==0, no need for xor 
	*/

	// precompute table t[9] = t[1] ^ t[8]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+8) + ((Idrirt+1)<<8) + ((Idrirt+9)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// precompute table t[10] = t[2] ^ t[8]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+8) + ((Idrirt+2)<<8) + ((Idrirt+10)<<16) + (1<<23) + (3<<24) + (bank<<28)),  
			     	false);

	// precompute table t[11] = t[1] ^ t[10]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+10) + ((Idrirt+1)<<8) + ((Idrirt+11)<<16) + (1<<23) + (3<<24) + (bank<<28)),  
			     	false);

	// precompute table t[12] = t[4] ^ t[8]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+8) + ((Idrirt+4)<<8) + ((Idrirt+12)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// precompute table t[13] = t[1] ^ t[12]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+12) + ((Idrirt+1)<<8) + ((Idrirt+13)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// why this is wrong !!! precompute table t[14] = t[1] ^ t[13] 
	// precompute table t[14] = t[2] ^ t[12] 
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+12) + ((Idrirt+2)<<8) + ((Idrirt+14)<<16) + (1<<23) + (3<<24) + (bank<<28)),
			     	false);

	// precompute table t[15] = t[1] ^ t[14]
	recryptor_decoder_wr(RECRYPTOR_DECODER_ADDR, 
	((Idrirt+14) + ((Idrirt+1)<<8) + ((Idrirt+15)<<16) + (1<<23) + (3<<24) + (bank<<28)), 
			     	false);
#endif
} 

void recryptor_decoder_ecc_bTable(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {

	assert((addr == (RECRYPTOR_DECODER_ADDR+2)));

    	printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);
} 
void recryptor_decoder_ecc_compute(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {

	assert((addr == (RECRYPTOR_DECODER_ADDR+3)));

    	printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);

} 
void recryptor_tick() {
     if (recryptor_state->head != NULL) {
          struct recryptor_action *nextAction = recryptor_state->head;
          nextAction->fn(nextAction->args);
          popACtion(recryptor_state);
     }
}
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
