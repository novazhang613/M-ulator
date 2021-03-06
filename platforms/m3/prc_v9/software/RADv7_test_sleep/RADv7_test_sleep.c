//****************************************************************************************************
//Author:       Gyouho Kim
//              ZhiYoong Foo
//Description:  Derived from Pstack_ondemand_v1.8.c
//****************************************************************************************************
#include "mbus.h"
#include "PRCv9.h"
#include "SNSv5.h"
#include "HRVv1.h"
#include "RADv7.h"

// uncomment this for debug mbus message
#define DEBUG_MBUS_MSG
// uncomment this for debug radio message
//#define DEBUG_RADIO_MSG

// uncomment this to only transmit average
//#define TX_AVERAGE

#define RAD_ADDR 0x4

// CDC parameters
#define	MBUS_DELAY 100 //Amount of delay between successive messages
#define	LDO_DELAY 500 // 1000: 150msec
#define CDC_TIMEOUT_COUNT 500
#define WAKEUP_PERIOD_RESET 2
#define WAKEUP_PERIOD_LDO 1

// Pstack states
#define	PSTK_IDLE       0x0
#define PSTK_CDC_RST    0x1
#define PSTK_CDC_READ   0x3
#define PSTK_LDO1   	0x4
#define PSTK_LDO2  	 	0x5

// Radio configurations
#define RAD_BIT_DELAY       13     //40      //0x54    //Radio tuning: Delay between bits sent (16 bits / packet)
#define RAD_PACKET_DELAY 	2000      //1000    //Radio tuning: Delay between packets sent (3 packets / sample)
#define RAD_PACKET_NUM      1      //How many times identical data will be TXed

// CDC configurations
#define NUM_SAMPLES         3      //Number of CDC samples to take (only 2^n allowed for averaging: 2, 4, 8, 16...)
#define NUM_SAMPLES_TX      1      //Number of CDC samples to be TXed (processed by process_data)
#define NUM_SAMPLES_2PWR    0      //NUM_SAMPLES = 2^NUM_SAMPLES_2PWR - used for averaging

#define CDC_STORAGE_SIZE 0  

//***************************************************
// Global variables
//***************************************************
	//Test Declerations
	// "static" limits the variables to this file, giving compiler more freedom
	// "volatile" should only be used for MMIO
	uint32_t enumerated;
	uint8_t Pstack_state;
	uint32_t cdc_data[NUM_SAMPLES];
	uint32_t cdc_data_tx[NUM_SAMPLES_TX];
	uint32_t cdc_data_index;
	uint32_t cdc_reset_timeout_count;
	uint32_t exec_count;
	uint32_t meas_count;
	uint32_t exec_count_irq;
	uint32_t MBus_msg_flag;
	volatile snsv5_r0_t snsv5_r0;
	volatile snsv5_r1_t snsv5_r1;
	volatile snsv5_r2_t snsv5_r2;
	volatile snsv5_r18_t snsv5_r18; // For LDO's
  
	uint32_t WAKEUP_PERIOD_CONT; 
	uint32_t WAKEUP_PERIOD_CONT_INIT; 

	uint32_t cdc_storage[CDC_STORAGE_SIZE] = {0};
	uint32_t cdc_storage_count;
	uint32_t radio_tx_count;
	uint32_t radio_tx_option;
	uint32_t radio_tx_numdata;
	uint8_t  radio_ready;

	volatile radv7_r0_t radv7_r0 = RADv7_R0_DEFAULT;
	volatile radv7_r1_t radv7_r1 = RADv7_R1_DEFAULT;
	volatile radv7_r2_t radv7_r2 = RADv7_R2_DEFAULT;
	volatile radv7_r3_t radv7_r3 = RADv7_R3_DEFAULT;
	volatile radv7_r4_t radv7_r4 = RADv7_R4_DEFAULT;
	volatile radv7_r5_t radv7_r5 = RADv7_R5_DEFAULT;
	volatile radv7_r6_t radv7_r6 = RADv7_R6_DEFAULT;
	volatile radv7_r7_t radv7_r7 = RADv7_R7_DEFAULT;
	volatile radv7_r8_t radv7_r8 = RADv7_R8_DEFAULT;
	volatile radv7_r9_t radv7_r9 = RADv7_R9_DEFAULT;

//***************************************************
//Interrupt Handlers
//Must clear pending bit!
//***************************************************
void handler_ext_int_0(void) __attribute__ ((interrupt ("IRQ")));
void handler_ext_int_1(void) __attribute__ ((interrupt ("IRQ")));
void handler_ext_int_2(void) __attribute__ ((interrupt ("IRQ")));
void handler_ext_int_3(void) __attribute__ ((interrupt ("IRQ")));
void handler_ext_int_0(void){
    *((volatile uint32_t *) 0xE000E280) = 0x1;
    MBus_msg_flag = 0x10;
}
void handler_ext_int_1(void){
    *((volatile uint32_t *) 0xE000E280) = 0x2;
    MBus_msg_flag = 0x11;
}
void handler_ext_int_2(void){
    *((volatile uint32_t *) 0xE000E280) = 0x4;
    MBus_msg_flag = 0x12;
}
void handler_ext_int_3(void){
    *((volatile uint32_t *) 0xE000E280) = 0x8;
    MBus_msg_flag = 0x13;
}

inline static void set_pmu_sleep_clk_low(){
    // PRCv9 Default: 0x8F770049
    *((volatile uint32_t *) 0xA200000C) = 0x8F77003B; // 0x8F77003B: use GOC x0.6-2
}
inline static void set_pmu_sleep_clk_high(){
    // PRCv9 Default: 0x8F770049
    *((volatile uint32_t *) 0xA200000C) = 0x8F77004B; // 0x8F77004B: use GOC x10-25
}
static void process_data(){
    uint8_t i;
    uint8_t j;
    uint32_t temp;
    // bubble sort
    for( i=0; i<NUM_SAMPLES; ++i ){
        for( j=i+1; j<NUM_SAMPLES; ++j){
            if( cdc_data[j] > cdc_data[i] ){
                temp = cdc_data[i];
                cdc_data[i] = cdc_data[j];
                cdc_data[j] = temp;
            }
        }
    }

    // Assuming NUM_SAMPLES = 3, NUM_SAMPLES_TX = 1
    cdc_data_tx[0] = cdc_data[1];

}

//***************************************************
// End of Program Sleep Operation
//***************************************************
static void operation_sleep(void){

  // Reset wakeup counter
  // This is required to go back to sleep!!
  //set_wakeup_timer (0xFFF, 0x0, 0x1);
  *((volatile uint32_t *) 0xA2000014) = 0x1;

  // Reset IRQ10VEC
  *((volatile uint32_t *) IRQ10VEC/*IMSG0*/) = 0;

  // Go to Sleep
  sleep();
  while(1);

}

static void operation_sleep_noirqreset(void){

  // Reset wakeup counter
  // This is required to go back to sleep!!
  *((volatile uint32_t *) 0xA2000014) = 0x1;

  // Go to Sleep
  sleep();
  while(1);

}

static void operation_sleep_notimer(void){
    
  // Disable Timer
  set_wakeup_timer (0, 0x0, 0x0);
  // Go to sleep without timer
  operation_sleep();

}

static void operation_init(void){
  
    // Set PMU Strength & division threshold
    // Change PMU_CTRL Register
    // PRCv9 Default: 0x8F770049
    // Decrease 5x division switching threshold
    *((volatile uint32_t *) 0xA200000C) = 0x8F77004B;
  
    // Speed up GOC frontend to match PMU frequency
    // PRCv9 Default: 0x00202903
    *((volatile uint32_t *) 0xA2000008) = 0x00202508;
  
    delay(100);
  
    //Enumerate & Initialize Registers
    Pstack_state = PSTK_IDLE; 	//0x0;
    enumerated = 0xDEADBEEF;
    cdc_data_index = 0;
    exec_count = 0;
    exec_count_irq = 0;
    MBus_msg_flag = 0;
    //Enumeration
    enumerate(RAD_ADDR);
    delay(MBUS_DELAY*10);


    // Radio Settings --------------------------------------
    //radv7_r0_t radv7_r0 = RADv7_R0_DEFAULT;

    radv7_r0.RADIO_TUNE_CURRENT_LIMITER = 0x2F; //Current Limiter 2F = 30uA, 1F = 3uA
    radv7_r0.RADIO_TUNE_FREQ1 = 0x0; //Tune Freq 1
    radv7_r0.RADIO_TUNE_FREQ2 = 0x0; //Tune Freq 2 //0x0,0x0 = 902MHz on Pblr005
    radv7_r0.RADIO_TUNE_TX_TIME = 0x7; //Tune TX Time

    write_mbus_register(RAD_ADDR,0,radv7_r0.as_int);
    delay(MBUS_DELAY);

	// Continuous Mode
	/*
    radv7_r0.RADIO_TUNE_CURRENT_LIMITER = 0x3E; //Current Limiter 2F = 30uA, 1F = 3uA
    radv7_r0.RADIO_TUNE_POWER = 0x1F; 
    radv7_r0.RADIO_EXT_CTRL_EN = 1; 
    radv7_r0.RADIO_EXT_OSC_ENB = 0; 
    write_mbus_register(RAD_ADDR,0,radv7_r0.as_int);
    delay(MBUS_DELAY);
	*/
	
	// Return address
	radv7_r8.RAD_FSM_IRQ_REPLY_ADDRESS = 0x16;
    write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
    delay(MBUS_DELAY);
 
	// FSM data length setups
	radv7_r6.RAD_FSM_H_LEN = 16; // N
	radv7_r6.RAD_FSM_D_LEN = 15; // N-1
	radv7_r6.RAD_FSM_C_LEN = 10;
    write_mbus_register(RAD_ADDR,6,radv7_r6.as_int);
    delay(MBUS_DELAY);

	// Configure SCRO
	radv7_r1.SCRO_FREQ_DIV = 2;
	radv7_r1.SCRO_AMP_I_LEVEL_SEL = 2; // Default 2
	radv7_r1.SCRO_I_LEVEL_SELB = 0x60; // Default 0x6F
    write_mbus_register(RAD_ADDR,1,radv7_r1.as_int);
    delay(MBUS_DELAY);

	// LFSR Seed
	radv7_r7.RAD_FSM_SEED = 4;
    write_mbus_register(RAD_ADDR,7,radv7_r7.as_int);
    delay(MBUS_DELAY);

    // Initialize other global variables
    WAKEUP_PERIOD_CONT = 3;   // 1: 2-4 sec with PRCv9
    WAKEUP_PERIOD_CONT_INIT = 1;   // 0x1E (30): ~1 min with PRCv9
    cdc_storage_count = 0;
    radio_tx_count = 0;
    radio_tx_option = 0;
	radio_ready = 0;
    
	//while(1);
    // Go to sleep without timer
    //operation_sleep_notimer();
}

static void operation_radio(){

	if (!radio_ready){

		//write_mbus_register(RAD_ADDR,0,radv7_r0.as_int);
		//delay(MBUS_DELAY);

		// Write Data
		radv7_r3.RAD_FSM_DATA = 0xFAFAFA;
		radv7_r4.RAD_FSM_DATA = 0xF0F0F0;
		radv7_r5.RAD_FSM_DATA = 0xF0F0F0;
		write_mbus_register(RAD_ADDR,3,radv7_r3.as_int);
		delay(MBUS_DELAY);
		write_mbus_register(RAD_ADDR,4,radv7_r4.as_int);
		delay(MBUS_DELAY);
		write_mbus_register(RAD_ADDR,5,radv7_r5.as_int);
		delay(MBUS_DELAY);

		// Release FSM Sleep - Requires >2s stabilization time
		radv7_r8.RAD_FSM_SLEEP = 0;
		write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
		delay(MBUS_DELAY);

		radio_ready = 1;
		set_wakeup_timer(2, 0x1, 0x0);
		operation_sleep();

	}else{

//    delay(20000); // this is required to stabilize ref gen

		// Release SCRO Reset
		radv7_r2.SCRO_RESET = 0;
		write_mbus_register(RAD_ADDR,2,radv7_r2.as_int);
		delay(MBUS_DELAY);

		// Enable SCRO
		radv7_r2.SCRO_ENABLE = 1;
		write_mbus_register(RAD_ADDR,2,radv7_r2.as_int);
		delay(MBUS_DELAY);
		

		// Release FSM Isolate
		radv7_r8.RAD_FSM_ISOLATE = 0;
		write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
		delay(MBUS_DELAY);

		// Release FSM Reset
		radv7_r8.RAD_FSM_RESETn = 1;
		write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
		delay(MBUS_DELAY);

		// Fire off data
		radv7_r8.RAD_FSM_ENABLE = 1;
		write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
		delay(MBUS_DELAY);
			

		delay(5000);
		
		read_mbus_register(RAD_ADDR,0x8,0x77);
		delay(MBUS_DELAY*5);
		
		radio_ready = 0;

		// Turn off everything
		radv7_r2.SCRO_ENABLE = 0;
		radv7_r2.SCRO_RESET = 1;
		write_mbus_register(RAD_ADDR,2,radv7_r2.as_int);
		delay(MBUS_DELAY);
		radv7_r8.RAD_FSM_SLEEP = 1;
		radv7_r8.RAD_FSM_ISOLATE = 1;
		radv7_r8.RAD_FSM_RESETn = 0;
		radv7_r8.RAD_FSM_ENABLE = 0;
		write_mbus_register(RAD_ADDR,8,radv7_r8.as_int);
		delay(MBUS_DELAY);
		
		set_wakeup_timer(1, 0x1, 0x0);
		operation_sleep();
	
	}
}

//***************************************************************************************
// MAIN function starts here             
//***************************************************************************************
int main() {
  
    //Clear All Pending Interrupts
    *((volatile uint32_t *) 0xE000E280) = 0xF;
    //Enable Interrupts
    *((volatile uint32_t *) 0xE000E100) = 0xF;
  
	//Config watchdog timer to about 10 sec: 1,000,000 with default PRCv9
	//config_timer( timer_id, go, roi, init_val, sat_val )
	// FIXME
	config_timer( 0, 0, 0, 0, 1000000 );

    // Initialization sequence
	//FIXME
    if (enumerated != 0xDEADBEEF){
        // Set up PMU/GOC register in PRC layer (every time)
        // Enumeration & RAD/SNS layer register configuration
        exec_count = 0;
		meas_count = 0;
        operation_init();
    }

    // Proceed to continuous mode
    while(1){
        operation_radio();
    }

    // Should not reach here
    operation_sleep_notimer();

    while(1);
}

