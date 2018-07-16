#include <stdbool.h>
#include <stdint.h>

#include "cpu/core.h" 
#include "cpu/recryptor/recryptor.h"
#include "cpu/m3_prc_v9/memmap.h"

#include "core/simulator.h"

const uint8_t NUM_SUBBANK[] = {8,2,4,2};
const uint8_t NUM_PREVTOT_SUBBANK[] = {0,8,10,14};
const char    *OpNames[] = {"AND","OR","XOR","COPY","NOT","SF1","SF4","LS64","RS64","ROTL64","XROTX","KEY","SS","MC","InvalidOp"};

const int LIM_ADDR_OFFSET = 0x2000;  

int recryptor_cnt = 0;

bool recryptor_FSM = 0; // 1 if running recryptor continuous functions
int  recryptor_FSM_fin_addr = 0x10000;
int  recryptor_FSM_fin_data = 0xabcd;

struct recryptor_action {
    void (*fn)(uint32_t,uint32_t,bool);
    uint32_t value;
    struct recryptor_action* next;
};

    //void * fn(void);

struct recryptor_action_list {
    struct recryptor_action* head;
    struct recryptor_action* tail;
};

struct recryptor_action_list* recryptor_state = NULL;

void pushRecryptorAction(struct recryptor_action* action) {
   if (recryptor_state== NULL) {
	recryptor_state = (struct recryptor_action_list*) malloc(sizeof(struct recryptor_action_list));
        recryptor_state->head = action;
        recryptor_state->tail = action;
    } else {
        recryptor_state->tail->next = action;
        recryptor_state->tail = action;
    }
}

void popRecryptorAction(void) {
  if (recryptor_state == NULL) {
	printf("No more recryptor to do!\n");
  } else {
    	struct recryptor_action* prev_head = recryptor_state->head;
	recryptor_state->head = recryptor_state->head->next;
	free(prev_head);
  }
}

//void addRecryptorAction((void*) fn(uint32_t,uint32_t,bool), uint32_t value) {
void addRecryptorAction( void (*fn)(uint32_t,uint32_t,bool), uint32_t value) {
	// Create a new struct
	struct recryptor_action* action = (struct recryptor_action*) malloc(sizeof(struct recryptor_action));
	action->fn= fn;
	action->value = value;
	action->next= NULL;

	pushRecryptorAction(action);
}

/* In-memory Single-cycle execution */
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
	if(1) printf("Cycle: %" PRId64 " - Recryptor: addrA = %#x, addrB = %#x, addrC = %#x, Bank = %x, Op = %s\n", cycle, addrA, addrB, addrC, bank,OpNames[op-1]);

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
	if(1) printf("Recryptor Count: %d\n",recryptor_cnt);
	
}

void recryptor_tick() {

     if (recryptor_state != NULL) { 
	if (recryptor_state->head != NULL) {
          struct recryptor_action *nextAction = recryptor_state->head;
          //nextAction->fn(nextAction->args);
          nextAction->fn(RECRYPTOR_DECODER_ADDR, nextAction->value, false);
          popRecryptorAction();
/* DEBUG SEG-FAULT
*/
	} else {
		if(recryptor_FSM) {
			recryptor_FSM = 0;
			printf("Cycle: %" PRId64 " release recryptor_FSM\n",cycle);
	 		write_word(recryptor_FSM_fin_addr,recryptor_FSM_fin_data);
		}
	}
     }
}

void recryptor_decoder_eccirt(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {
	assert((addr == (RECRYPTOR_DECODER_ECCIRT)));

	recryptor_FSM = 1;

    	//printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);

	// Decode base address
	//int addr_ir  = ((val)     & 0x7F) << 8;
	//int addr_irt = ((val>> 8) & 0x7F) << 8;
	//int FB_POLYN = ((val>>16) & 0x7FF);
	//int NUM_PLN_Minus1 = ((val>>27) & 0x1);
	int Idrir  = ((val)     & 0x7F);
	int Idrirt = ((val>> 8) & 0x7F);
	int bank   = ((val>>28) & 0xF);

	int value;
	// precompute table t[1] = b
	value = (Idrir + ((Idrirt+1)<<16) + (1<<23) + (4<<24) + (bank<<28));
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[2] = t[1] << 1
	value = (Idrir + ((Idrirt+2)<<16) + (1<<23) + (6<<24) + (bank<<28));
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[3] = t[1] ^ t[2]
	value = ((Idrirt+2) + ((Idrirt+1)<<8) + ((Idrirt+3)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[4] = t[2] << 1
	value = ((Idrirt+2) + ((Idrirt+4)<<16) + (1<<23) + (6<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[5] = t[1] ^ t[4]
	value = ((Idrirt+4) + ((Idrirt+1)<<8) + ((Idrirt+5)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// precompute table t[6] = t[2] ^ t[4] 
	value = ((Idrirt+4) + ((Idrirt+2)<<8) + ((Idrirt+6)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[7] = t[1] ^ t[6]
	value = ((Idrirt+6) + ((Idrirt+1)<<8) + ((Idrirt+7)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[8] = t[4] << 1
	value = ((Idrirt+4) + ((Idrirt+8)<<16) + (1<<23) + (6<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	/*
	// t[8] = t[8] ^ ir_t[u]
	u = (addr_ir[FB_DIGS-1] >> (FB_MOD-3)) & 0x1; // grab the 3rd bit  
	if (u==1) {
			((Idrirt+8) + ((Idrir)<<8) + ((Idrirt+8)<<16) + (1<<23) + (3<<24) + (bank<<28)),
	}
			// else if u ==0, no need for xor 
	*/

	// precompute table t[9] = t[1] ^ t[8]
	value = ((Idrirt+8) + ((Idrirt+1)<<8) + ((Idrirt+9)<<16) + (1<<23) + (3<<24) + (bank<<28));
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[10] = t[2] ^ t[8]
	value = ((Idrirt+8) + ((Idrirt+2)<<8) + ((Idrirt+10)<<16) + (1<<23) + (3<<24) + (bank<<28));
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[11] = t[1] ^ t[10]
	value = ((Idrirt+10) + ((Idrirt+1)<<8) + ((Idrirt+11)<<16) + (1<<23) + (3<<24) + (bank<<28));  
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[12] = t[4] ^ t[8]
	value = ((Idrirt+8) + ((Idrirt+4)<<8) + ((Idrirt+12)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[13] = t[1] ^ t[12]
	value = ((Idrirt+12) + ((Idrirt+1)<<8) + ((Idrirt+13)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// why this is wrong !!! precompute table t[14] = t[1] ^ t[13] 
	// precompute table t[14] = t[2] ^ t[12] 
	value = ((Idrirt+12) + ((Idrirt+2)<<8) + ((Idrirt+14)<<16) + (1<<23) + (3<<24) + (bank<<28));
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[15] = t[1] ^ t[14]
	value = ((Idrirt+14) + ((Idrirt+1)<<8) + ((Idrirt+15)<<16) + (1<<23) + (3<<24) + (bank<<28)); 
        addRecryptorAction(&recryptor_decoder_wr, value);

}


void recryptor_decoder_eccrdt(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {
	assert((addr == (RECRYPTOR_DECODER_ECCIRT)));

	recryptor_FSM = 1;

	int Idrb   = ((val)     & 0x7F);
	int Idrt   = ((val>> 8) & 0x7F);
	uint8_t dataB_MSB = ((val>>16) & 0x7);
	//printf("\n\nMSB: %x\n\n",dataB_MSB);
	//int NUM_PLN= ((val>>27) & 0x1);
	int bank   = ((val>>28) & 0xF);

	// hack FIXED addr_IR = 0x 6900 !!!! 
	//int Idrir  = ((val>>16) & 0x7F);
	int Idrir  = ((0x6800>>8) & 0x7F); 

	int value;
	// precompute table t[1] = b
	value = Idrb + ((Idrt+1)<<16) + (1<<23) + (4<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[2] = t[1] << 1
	value = Idrb + ((Idrt+2)<<16) + (1<<23) + (6<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// t[2] = t[2] ^ ir_t[u]
	//u = ( *(addr_b + 28) >> 8) & 0x1; //grab the first bit // Another option is to use addr_t
	//u = (addr_b[FB_DIGS-1] >> (FB_MOD-1)) & 0x1;  
	if (( dataB_MSB >> 2) & 0x1) {
		value =(Idrt+2) + (Idrir<<8) + ((Idrt+2)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        	addRecryptorAction(&recryptor_decoder_wr, value);
	}
	// else if u ==0, no need for xor 


	// precompute table t[3] = t[1] ^ t[2]
	value = (Idrt+2) + ((Idrt+1)<<8) + ((Idrt+3)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[4] = t[2] << 1
	value = (Idrt+2) + ((Idrt+4)<<16) + (1<<23) + (6<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// t[4] = t[4] ^ ir_t[u]
	//u = (addr_b[FB_DIGS-1] >> (FB_MOD-2)) & 0x1; // grab the 2nd bit  
	if (( dataB_MSB >> 1) & 0x1) {
		value = (Idrt+4) + ((Idrir)<<8) + ((Idrt+4)<<16) + (1<<23) + (3<<24) + (bank<<28); 
       		addRecryptorAction(&recryptor_decoder_wr, value);
	}
	// else if u ==0, no need for xor 

	// precompute table t[5] = t[1] ^ t[4]
	value = (Idrt+4) + ((Idrt+1)<<8) + ((Idrt+5)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// precompute table t[6] = t[2] ^ t[4] 
	value = (Idrt+4) + ((Idrt+2)<<8) + ((Idrt+6)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[7] = t[1] ^ t[6]
	value = (Idrt+6) + ((Idrt+1)<<8) + ((Idrt+7)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[8] = t[4] << 1
	value = (Idrt+4) + ((Idrt+8)<<16) + (1<<23) + (6<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// t[8] = t[8] ^ ir_t[u]
	//u = (addr_b[FB_DIGS-1] >> (FB_MOD-3)) & 0x1; // grab the 3rd bit  
	if (dataB_MSB & 0x1) {
		value = (Idrt+8) + ((Idrir)<<8) + ((Idrt+8)<<16) + (1<<23) + (3<<24) + (bank<<28);
	        addRecryptorAction(&recryptor_decoder_wr, value);
	}
			// else if u ==0, no need for xor 

	// precompute table t[9] = t[1] ^ t[8]
	value = (Idrt+8) + ((Idrt+1)<<8) + ((Idrt+9)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[10] = t[2] ^ t[8]
	value = (Idrt+8) + ((Idrt+2)<<8) + ((Idrt+10)<<16) + (1<<23) + (3<<24) + (bank<<28);  
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[11] = t[1] ^ t[10]
	value = (Idrt+10) + ((Idrt+1)<<8) + ((Idrt+11)<<16) + (1<<23) + (3<<24) + (bank<<28);  
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[12] = t[4] ^ t[8]
	value = (Idrt+8) + ((Idrt+4)<<8) + ((Idrt+12)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[13] = t[1] ^ t[12]
	value = (Idrt+12) + ((Idrt+1)<<8) + ((Idrt+13)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

	// use this xor instead of shift, to avoid increased 1 bit !!!
	// why this is wrong !!! precompute table t[14] = t[1] ^ t[13] 
	// precompute table t[14] = t[2] ^ t[12] 
	value = (Idrt+12) + ((Idrt+2)<<8) + ((Idrt+14)<<16) + (1<<23) + (3<<24) + (bank<<28);
        addRecryptorAction(&recryptor_decoder_wr, value);

	// precompute table t[15] = t[1] ^ t[14]
	value = (Idrt+14) + ((Idrt+1)<<8) + ((Idrt+15)<<16) + (1<<23) + (3<<24) + (bank<<28); 
        addRecryptorAction(&recryptor_decoder_wr, value);

}

void recryptor_decoder_eccexe(uint32_t addr, uint32_t val, bool debugger __attribute__ ((unused)) ) {
	assert((addr == (RECRYPTOR_DECODER_ECCEXE)));
    	printf("HERE I AM! addr = %#x, val = %#x\n", addr, val);
}
