#include <sys/stat.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#define STAGE SIM
#include "state.h"
#include "simulator.h"
#include "pipeline.h"
#include "if_stage.h"
#include "id_stage.h"
#include "ex_stage.h"
#include "cpu/core.h"
#include "cpu/misc.h"
#include "cpu/operations/opcodes.h"
#include "gdb.h"


////////////////////////////////////////////////////////////////////////////////
// GLOBALS
////////////////////////////////////////////////////////////////////////////////

#ifdef DEBUG1
#define pthread_mutex_lock(_m)\
	do {\
		int ret = pthread_mutex_lock((_m));\
		if (ret) {\
			perror("Locking "VAL2STR(_m));\
			exit(ret);\
		}\
	} while (0)
#define pthread_mutex_unlock(_m)\
	do {\
		int ret = pthread_mutex_unlock((_m));\
		if (ret) {\
			perror("Unlocking "VAL2STR(_m));\
			exit(ret);\
		}\
	} while (0)
#endif

#define NSECS_PER_SEC 1000000000

static volatile sig_atomic_t sigint = 0;
static volatile bool shell_running = false;

/* Tick Control */
EXPORT sem_t ticker_ready_sem;
EXPORT sem_t start_tick_sem;
EXPORT sem_t end_tick_sem;
EXPORT sem_t end_tock_sem;
#define NUM_TICKERS	3	// IF, ID, EX

/* Peripherals */
static void join_periph_threads (void);

/* UARTs */
#define INVALID_CLIENT (UINT_MAX-1)

// Thread object running the polling UART peripheral
pthread_t poll_uart_pthread;

volatile bool poll_uart_shutdown = false;

static void *poll_uart_thread(void *);
#ifdef DEBUG1
static pthread_mutex_t poll_uart_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t poll_uart_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_cond_t  poll_uart_cond  = PTHREAD_COND_INITIALIZER;
static uint32_t poll_uart_client = INVALID_CLIENT;

// circular buffer
// head is location to read valid data from
// tail is location to write data to
// characters are lost if not read fast enough
// head == tail --> buffer is full (not that it matters)
// head == NULL --> buffer if empty

// these should be treated as char's, but are stored as uint32_t
// to unify state tracking code
static uint32_t poll_uart_buffer[POLL_UART_BUFSIZE];
static uint32_t *poll_uart_head = NULL;
static uint32_t *poll_uart_tail = poll_uart_buffer;

static const struct timespec poll_uart_baud_sleep =\
		{0, (NSECS_PER_SEC/POLL_UART_BAUD)*8};	// *8 bytes vs bits

uint8_t poll_uart_status_read();
void poll_uart_status_write();
uint8_t poll_uart_rxdata_read();
void poll_uart_txdata_write(uint8_t val);

/* Config */

EXPORT int gdb_port = -1;
#define GDB_ATTACHED (gdb_port != -1)
EXPORT int slowsim = 0;
#ifdef DEBUG2
EXPORT int printcycles = 1;
EXPORT int raiseonerror = 1;
#else
EXPORT int printcycles = 0;
EXPORT int raiseonerror = 0;
#endif
EXPORT int limitcycles = -1;
EXPORT int showledwrites = 0;
EXPORT unsigned dumpatpc = -3;
EXPORT int dumpatcycle = -1;
EXPORT int dumpallcycles = 0;
EXPORT int returnr0 = 0;
EXPORT int usetestflash = 0;

/* Test Flash */

// utils/bintoarray.sh
#define STATIC_ROM_NUM_BYTES (80 * 4)
static uint32_t static_rom[80] = {0x20003FFC,0x55,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0x51,0xBF00E7FE,0xF862F000,0xBF00E7FA,0xB083B480,0xF244AF00,0xF2C41300,0x603B0300,0x1304F244,0x300F2C4,0x683B607B,0xB2DB781B,0x302F003,0xD0F82B00,0x781B687B,0x4618B2DB,0x70CF107,0xBC8046BD,0xBF004770,0xB085B480,0x4603AF00,0xF24471FB,0xF2C41300,0x60BB0300,0x1308F244,0x300F2C4,0x68BB60FB,0xB2DB781B,0x304F003,0xD1F82B00,0x79FA68FB,0xF107701A,0x46BD0714,0x4770BC80,0xB082B580,0x6078AF00,0x687BE008,0x4618781B,0xFFD8F7FF,0xF103687B,0x607B0301,0x781B687B,0xD1F22B00,0x708F107,0xBD8046BD,0x181B4B05,0x2A006818,0x2201BF03,0x4390408A,0x60184310,0x30004770,0x40001000,0xB082B580,0xF7FFAF00,0x4603FF9B,0x79FB71FB,0xF7FF4618,0xF04FFFB3,0x46180300,0x708F107,0xBD8046BD};

/* State */

static int opcode_masks;

EXPORT int cycle = -1;

#define ADDR_TO_IDX(_addr, _bot) ((_addr - _bot) >> 2)

/* Ignore mode transitions for now */
static uint32_t reg[SP_REG];	// SP,LR,PC not held here, so 13 registers
static uint32_t sp_process;
static uint32_t sp_main __attribute__ ((unused));
static uint32_t *sp = &sp_process;
static uint32_t lr;

#define SP	(*sp)
#define LR	(lr)
#ifdef NO_PIPELINE
#define PC	(pre_if_PC)
#else
#define PC	(id_ex_PC)
#endif

#ifdef A_PROFILE
// (|= 0x0010) Start is user mode
// (|= 0x0020) CPU only supports thumb (for now)
static uint32_t apsr = 0x0030;

// "Current" program status register
//   0-4: M[4:0]	// Mode field: 10000 user  10001 FIQ 10010 IRQ 10011 svc
			//             10111 abort 11011 und 11111 sys
//     5: T		// Thumb bit
//     6: F		// Fast Interrupt disable (FIQ interrupts)
//     7: I		// Interrupt disable (IRQ interrupts)
//     8: A		// Asynchronous abort disable
//     9: E		// Endianness execution state bit (0: little, 1: big)
// 10-15: IT[7:2]	// it-bits
// 16-19: GE[3:0]	// Greater than or equal flags for SIMD instructions
// 20-23: <reserved>
//    24: J		// Jazelle bit
// 25-26: IT[1:0]	// it-bits
//    27: Q		// Cumulative saturation
//    28: V		// Overflow
//    29: C		// Carry
//    30: Z		// Zero
//    31: N		// Negative
static uint32_t *cpsr = &apsr;
#define CPSR		(*cpsr)
#define	T_BIT		(CPSR & 0x0020)

#define ITSTATE			( ((CPSR & 0xfc00) >> 8) | ((CPSR & 0x06000000) >> 25) )

#endif // A

#ifdef M_PROFILE

static uint32_t apsr;
//  0-15: <reserved>
// 16-19: GE[3:0]	// for DSP extension
// 20-26: <reserved>
//    27: Q		// Cumulative saturation
//    28: V		// Overflow
//    29: C		// Carry
//    30: Z		// Zero
//    31: N		// Negative
static uint32_t ipsr = 0x0;
//   0-8: 0 or Exception Number
//  9-31: <reserved>
static uint32_t epsr = 0x01000000;
//   0-9: <reserved>
// 10-15: ICI/IT	//
// 16-23: <reserved>
//    24: T		// Thumb bit
// 25-26: ICI/IT	//
// 27-31: <reserved>
#define T_BIT		(epsr & 0x01000000)

#define CPSR		(apsr)
#define APSR		(apsr)
#define IPSR		(ipsr)
#define EPSR		(epsr)
#define IAPSR		(IPSR | APSR)
#define EAPSR		(EPSR | APSR)
#define XPSR		(IPSR | APSR | EPSR)
#define IEPSR		(IPSR | EPSR)

#define ITSTATE		( ((EPSR & 0xfc00) >> 8) | ((EPSR & 0x06000000) >> 25) )

static uint32_t primask __attribute__ ((unused)) = 0x0;
//     0: priority	The exception mask register, a 1-bit register.
//			Setting PRIMASK to 1 raises the execution priority to 0.
static uint32_t basepri __attribute__ ((unused)) = 0x0;
/* The base priority mask, an 8-bit register. BASEPRI changes the priority
 * level required for exception preemption. It has an effect only when BASEPRI
 * has a lower value than the unmasked priority level of the currently
 * executing software.  The number of implemented bits in BASEPRI is the same
 * as the number of implemented bits in each field of the priority registers,
 * and BASEPRI has the same format as those fields.  For more information see
 * Maximum supported priority value on page B1-636.  A value of zero disables
 * masking by BASEPRI.
 */
static uint32_t faultmask __attribute__ ((unused)) = 0x0;
/* The fault mask, a 1-bit register. Setting FAULTMASK to 1 raises the
 * execution priority to -1, the priority of HardFault. Only privileged
 * software executing at a priority below -1 can set FAULTMASK to 1. This means
 * HardFault and NMI handlers cannot set FAULTMASK to 1. Returning from any
 * exception except NMI clears FAULTMASK to 0.
 */

static uint32_t control __attribute__ ((unused)) = 0x0;
//     0: nPRIV, thread mode only (0 == privileged, 1 == unprivileged)
//     1: SPSEL, thread mode only (0 == use SP_main, 1 == use SP_process)
//     2: FPCA, (1 if FP extension active)

#endif // M


/* Peripherals */
enum LED_color {
	RED = 0,
	GRN = 1,
	BLU = 2,
	LED_COLORS,
};
static uint32_t leds[LED_COLORS] = {0};

////////////////////////////////////////////////////////////////////////////////
// CORE
////////////////////////////////////////////////////////////////////////////////

// state flags #defines:
// Bottom byte aligns with enum stage
#define STATE_STALL_PRE		PRE
#define STATE_STALL_IF		IF
#define STATE_STALL_ID		ID
#define STATE_STALL_EX		EX
#define STATE_STALL_PIPE	PIPE
#define STATE_STALL_SIM		SIM
#define STATE_STALL_UNK		UNK
#define STATE_STALL_MASK	0xff

#define STATE_IO_BARRIER	0x100
#ifndef NO_PIPELINE
#define STATE_PIPELINE_FLUSH	0x1000
#define STATE_PIPELINE_RUNNING	0x8000
#endif
#define STATE_LED_WRITTEN	0x10000
#define STATE_LED_WRITING	0x80000
#define STATE_BLOCKING_ASYNC	0x800000

#define STATE_DEBUGGING		0x1000000

#define STATE_LOCK_HELD_MASK	0x888800

struct state_change {
	struct state_change *prev;
	struct state_change *next;

	/* Holds changes made __during__ this cycle; there may be multiple
	   list entries per cycle. Thus, to return to the beginning of a
	   cycle n (e.g. before it had executed), the list must have traversed
	   such that the list head reaches an entry of cycle less than n */
	int cycle;
	enum stage g;
	uint32_t *flags;
	uint32_t *loc;
	uint32_t val;
	uint32_t prev_val;
	uint32_t **ploc;
	uint32_t *pval;
	uint32_t *prev_pval;
#ifdef DEBUG1
	const char* file;
	const char* func;
	int line;
	const char* target;
#endif
};

static uint32_t state_start_flags = 0;
static struct state_change state_start = {
	.prev = NULL,
	.next = NULL,
	.cycle = -1,
	.g = UNK,
	.flags = &state_start_flags,
	.loc = NULL,
	.val = 0,
	.prev_val = 0,
	.ploc = NULL,
	.pval = NULL,
	.prev_pval = NULL,
#ifdef DEBUG1
	.file = NULL,
	.func = NULL,
	.line = -1,
	.target = NULL,
#endif
};

static struct state_change* state_head = &state_start;
static struct state_change* cycle_head = NULL;

static uint32_t* state_flags_cur = &state_start_flags;

static struct op* o_hack = NULL;

#ifndef NO_PIPELINE
static uint32_t state_pipeline_new_pc = -1;
#endif

#ifdef DEBUG1
static pthread_mutex_t state_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

EXPORT void state_async_block_start(void) {
	pthread_mutex_lock(&state_mutex);
	DBG2("async_block started\n");
	*state_flags_cur |= STATE_BLOCKING_ASYNC;
}

EXPORT void state_async_block_end(void) {
	*state_flags_cur &= ~STATE_BLOCKING_ASYNC;
	DBG2("async_block ended\n");
	pthread_mutex_unlock(&state_mutex);
}

static void state_block_async_io(void) {
	pthread_mutex_lock(&state_mutex);
	*state_flags_cur |= STATE_BLOCKING_ASYNC;
}

static void state_unblock_async_io(void) {
	*state_flags_cur &= ~STATE_BLOCKING_ASYNC;
	pthread_mutex_unlock(&state_mutex);
}

static void state_start_tick() {
	state_block_async_io();
#ifdef DEBUG2
	flockfile(stdout); flockfile(stderr);
	printf("\n%08d TICK TICK TICK TICK TICK TICK TICK TICK TICK\n", cycle);
	funlockfile(stderr); funlockfile(stdout);
#endif
	DBG2("CLOCK --> high (cycle %08d)\n", cycle);
	//assert((state_head->cycle+1) == cycle);
	cycle_head = state_head;

	if (NULL != state_head->next) {
		WARN("Simulator re-excuting at cycle %d\n", cycle);
		WARN("Discarding all future state\n");

		struct state_change* s = state_head->next;
		while (s->next != NULL) {
			s = s->next;
			if (s->cycle != s->prev->cycle)
				free(s->prev->flags);
			free(s->prev);
		}
		free(s->flags);
		free(s);

		state_head->next = NULL;
	}

	state_flags_cur = malloc(sizeof(uint32_t));
	assert((NULL != state_flags_cur) && "OOM");
	*state_flags_cur = 0;

	o_hack = NULL;

	state_unblock_async_io();
}

static void _state_tock() {
	if (!(*state_flags_cur & STATE_STALL_ID)) {
		if (cycle > 0) {
			assert((NULL != o_hack) &&
					"nothing set instruction for EX this cycle");
		}
		id_ex_o = o_hack;
	}

	struct state_change* orig_cycle_head = cycle_head;

	while (NULL != cycle_head) {
		assert(cycle_head->cycle == cycle);
		if (cycle_head->g & *cycle_head->flags) {
#ifdef DEBUG1
			// This write has been stalled, skip it
			;
#else
			// This write has been stalled, remove it
			struct state_change* delete_me = cycle_head;

			// shouldn't be able to stall most initial state
			// XXX: what about after a replay? (no... this is ok)
			assert(NULL != cycle_head->prev);
			cycle_head->prev->next = cycle_head->next;
			// but we could be the last state...
			if (NULL != cycle_head->next) {
				cycle_head->next->prev = cycle_head->prev;
			}
			// update checking bookkeeping
			if (cycle_head == orig_cycle_head) {
				orig_cycle_head = cycle_head->next;
				if (NULL == orig_cycle_head) {
					WARN("stall generated empty cycle\n");
				}
			}
			// back the head up so the loop can advance it
			cycle_head = cycle_head->prev;
			free(delete_me);
#endif
		}
		else if (cycle_head->loc) {
			DBG2("(%s::%s:%d: %s: %p = %08x (was %08x)\n",
					cycle_head->file, cycle_head->func,
					cycle_head->line, cycle_head->target,
					cycle_head->loc, cycle_head->val,
					cycle_head->prev_val);
			*cycle_head->loc = cycle_head->val;
		} else if (cycle_head->ploc) {
			DBG2("(%s::%s:%d: %s: %p = %p (was %p)\n",
					cycle_head->file, cycle_head->func,
					cycle_head->line, cycle_head->target,
					cycle_head->ploc, cycle_head->pval,
					cycle_head->prev_pval);
			*cycle_head->ploc = cycle_head->pval;
		} else {
			ERR(E_UNKNOWN, "loc and ploc NULL?\n");
		}
		cycle_head = cycle_head->next;
	}

	// Not going to do better than O(2n) for detecting duplicates in an
	// unsorted list
	while (NULL != orig_cycle_head) {
		if (orig_cycle_head->loc) {
#ifdef NO_PIPELINE
			// Allow the PC to alias for branches
			if (orig_cycle_head->loc == &pre_if_PC) {
				orig_cycle_head = orig_cycle_head->next;
				continue;
			}
#endif
			if (*orig_cycle_head->loc != orig_cycle_head->val) {
#ifdef DEBUG1
				ERR(E_UNPREDICTABLE, "(%s): %p was aliased, expected %08x, got %08x\n",
						orig_cycle_head->target,
#else
				ERR(E_UNPREDICTABLE, "loc %p was aliased, expected %08x, got %08x\n",
#endif
						orig_cycle_head->loc,
						orig_cycle_head->val,
						*orig_cycle_head->loc
#ifdef DEBUG1 // Appease ()'s balance
				   );
#else
				   );
#endif
			}
		} else {
			// But async stuff is allowed to alias
			;
		}
		orig_cycle_head = orig_cycle_head->next;
	}
}

static void print_leds_line();

static void state_tock() {
	state_block_async_io();

#ifdef DEBUG2
	flockfile(stdout); flockfile(stderr);
	printf("\n%08d TOCK TOCK TOCK TOCK TOCK TOCK TOCK TOCK TOCK\n", cycle);
	funlockfile(stderr); funlockfile(stdout);
#endif
	DBG2("CLOCK --> low (cycle %08d)\n", cycle);
	// cycle_head is state_head from end of previous cycle here
	cycle_head = cycle_head->next;
	assert((NULL != cycle_head) && "nothing written this cycle?");

	_state_tock();

#ifndef NO_PIPELINE
	if (*state_flags_cur & STATE_PIPELINE_FLUSH) {
		// Leverage the existing state mechanism to simplify replay,
		// conceptually adding extra writes to the end of this cycle
		// that alias the previous writes to actually flush the pipeline
		cycle_head = state_head;
		*cycle_head->flags |= STATE_PIPELINE_RUNNING;
		o_hack = NULL;
		pipeline_flush(state_pipeline_new_pc);
		cycle_head = cycle_head->next;

		_state_tock();

		*state_flags_cur &= ~STATE_PIPELINE_RUNNING;
	}
#endif

	if (*state_flags_cur & STATE_LED_WRITTEN) {
		if (showledwrites) {
			print_leds_line();
		}
	}

	state_unblock_async_io();
}

EXPORT uint32_t state_read(enum stage g __attribute__ ((unused)), uint32_t *loc) {
	return *loc;
}

EXPORT uint32_t state_read_async(enum stage g __attribute__ ((unused)), uint32_t *loc) {
	uint32_t ret;
	state_async_block_start();
	ret = state_read(g, loc);
	state_async_block_end();
	return ret;
}

EXPORT uint32_t* state_read_p(enum stage g __attribute__ ((unused)), uint32_t **loc) {
	return *loc;
}

#ifdef DEBUG1
static void _state_write_dbg(enum stage g, uint32_t *loc, uint32_t val,
		uint32_t** ploc, uint32_t* pval,
		const char *file, const char* func,
		const int line, const char *target) {
#else
static void _state_write(enum stage g, uint32_t *loc, uint32_t val,
		uint32_t** ploc, uint32_t* pval) {
#endif
	// If a debugger is changing values, write them directly. We do not track debugger
	// writes (as I'm not sure the best architecture / usage model around that), so for
	// now it's quite probable / possible that writing values with the debugger could do
	// some strange things for replay.
	if (*state_flags_cur & STATE_DEBUGGING) {
		if (loc)
			*loc = val;
		else
			*ploc = pval;
		return;
	}

#ifndef DEBUG1
	// don't track state we won't write anyways, except in debug mode
	// race for flags is OK here, since this check is always racing anyway
	if (*state_flags_cur & g & STATE_STALL_MASK) {
		return;
	}
#endif
	if (!(*state_flags_cur & STATE_LOCK_HELD_MASK))
		pthread_mutex_lock(&state_mutex);

	assert((NULL != loc) || (NULL != ploc));

	// Record:
	struct state_change* s = malloc(sizeof(struct state_change));
	assert((NULL != s) && "OOM");

	DBG2("cycle: %08d\t(%s): loc %p val %08x\n",
			cycle, target, loc, val);

	s->prev = state_head;
	s->next = NULL;
	s->cycle = cycle;
	s->g = g;
	s->flags = state_flags_cur;
	s->loc = loc;
	s->val = val;
	s->prev_val = (loc) ? *loc : 0;
	s->ploc = ploc;
	s->pval = pval;
	s->prev_pval = (ploc) ? *ploc : NULL;
#ifdef DEBUG1
	s->file = file;
	s->func = func;
	s->line = line;
	s->target = target;
#endif
	state_head->next = s;
	state_head = s;

#ifdef NO_PIPELINE
	if (true) {
		DBG1("  SW: %s:%d\t%s = %08x\n", file, line, target, val);
#else
	if (*state_flags_cur & STATE_BLOCKING_ASYNC) {
#endif
		if (loc) {
			*loc = val;
		} else {
			assert(NULL != ploc);
			*ploc = pval;
		}
	}

	if (!(*state_flags_cur & STATE_LOCK_HELD_MASK))
		pthread_mutex_unlock(&state_mutex);
}

#ifdef DEBUG1
EXPORT void state_write_dbg(enum stage g, uint32_t *loc, uint32_t val,
		const char *file, const char* func,
		const int line, const char *target) {
	return _state_write_dbg(g, loc, val, NULL, NULL, file, func, line, target);
}

EXPORT void state_write_p_dbg(enum stage g, uint32_t **ploc, uint32_t *pval,
		const char *file, const char* func,
		const int line, const char *target) {
	return _state_write_dbg(g, NULL, 0, ploc, pval, file, func, line, target);
}

EXPORT void state_write_async_dbg(enum stage g, uint32_t *loc, uint32_t val,
		const char *file, const char* func,
		const int line, const char *target) {
	state_async_block_start();
	state_write_dbg(g, loc, val, file, func, line, target);
	state_async_block_end();
}
#else
EXPORT void state_write(enum stage g, uint32_t *loc, uint32_t val) {
	return _state_write(g, loc, val, NULL, NULL);
}

EXPORT void state_write_p(enum stage g, uint32_t **ploc, uint32_t *pval) {
	return _state_write(g, NULL, 0, ploc, pval);
}

EXPORT void state_write_async(enum stage g, uint32_t *loc, uint32_t val) {
	state_async_block_start();
	state_write(g, loc, val);
	state_async_block_end();
}
#endif

EXPORT void state_write_op(enum stage g __attribute__ ((unused)), struct op **loc, struct op *val) {
	if (!(*state_flags_cur & STATE_LOCK_HELD_MASK))
		pthread_mutex_lock(&state_mutex);
	// Lazy hack since every other bit of preserved state is a uint32_t[*]
	// At some point in time state saving will likely have to be generalized,
	// until then, however, this will suffice
	assert(loc == &id_ex_o);
	o_hack = val;
#ifdef NO_PIPELINE
	id_ex_o = val;
#endif
	if (!(*state_flags_cur & STATE_LOCK_HELD_MASK))
		pthread_mutex_unlock(&state_mutex);
}

EXPORT void stall(enum stage g) {
	pthread_mutex_lock(&state_mutex);
#ifdef DEBUG1
	assert((g <= STATE_STALL_MASK) && "stalling illegal stage");
#endif
	if (g & ~(PRE|IF|ID)) {
		ERR(E_UNPREDICTABLE, "Stalling for non PRE|IF|ID needs namespace fix\n");
	}
	*state_flags_cur |= g;
	pthread_mutex_unlock(&state_mutex);
}

#ifndef NO_PIPELINE
static void state_pipeline_flush(uint32_t new_pc) {
	pthread_mutex_lock(&state_mutex);
	*state_flags_cur |= STATE_PIPELINE_FLUSH;
	state_pipeline_new_pc = new_pc;
	pthread_mutex_unlock(&state_mutex);
}
#endif

static void state_led_write(enum LED_color led, uint32_t val) {
	pthread_mutex_lock(&state_mutex);
	*state_flags_cur |= STATE_LED_WRITING;
	*state_flags_cur |= STATE_LED_WRITTEN;

	SW(&leds[led], val);

	*state_flags_cur &= ~STATE_LED_WRITING;
	pthread_mutex_unlock(&state_mutex);
}

EXPORT void state_enter_debugging(void) {
	*state_flags_cur |= STATE_DEBUGGING;
}

EXPORT void state_exit_debugging(void) {
	*state_flags_cur &= ~STATE_DEBUGGING;
}

// Returns 0 on success
// Returns >0 on tolerable error (e.g. seek past end)
// Returns <0 on catastrophic error
EXPORT int state_seek(int target_cycle) {
	// This assertion relies on *something* being written every cycle,
	// which should hold since the PC is written every cycle at least.
	// However, if we ever go cycle-accurate, much of the state_seek
	// logic will require a revisit
	assert(state_head->cycle == cycle);

	if (target_cycle > cycle) {
		DBG2("seeking forward from %d to %d\n", cycle, target_cycle);
		while (target_cycle > cycle) {
			if (state_head->next == NULL) {
				WARN("Request to seek to cycle %d failed\n",
						target_cycle);
				WARN("State is only known up to cycle %d\n",
						state_head->cycle);
				WARN("Simulator left at cycle %d\n", cycle);
				return 1;
			}

			state_head = state_head->next;
			if (state_head->loc) {
				*(state_head->loc) = state_head->val;
			} else {
				*(state_head->ploc) = state_head->pval;
			}
			if (
					(NULL == state_head->next) ||
					(state_head->next->cycle > state_head->cycle)
			   ) {
				cycle++;
			}
		}
		return 0;
	} else if (target_cycle == cycle) {
		WARN("Request to seek to current cycle ignored\n");
		return 1;
	} else {
		DBG2("seeking backward from %d to %d\n", cycle, target_cycle);
		while (state_head->cycle > target_cycle) {
			if (*state_head->flags & STATE_IO_BARRIER) {
				WARN("Cannot rewind past I/O access\n");
				WARN("Simulator left at cycle %08d\n", cycle);
				return 1;
			}

			assert(NULL != state_head->prev);

			DBG2("cycle: %d target: %d head: %d\n",
					cycle, target_cycle, state_head->cycle);

			if (state_head->loc) {
				DBG2("Resetting %p to %08x\n",
						state_head->loc, state_head->prev_val);
				*(state_head->loc) = state_head->prev_val;
			} else {
				DBG2("Resetting %p to %p\n",
						state_head->ploc, state_head->prev_pval);
				*(state_head->ploc) = state_head->prev_pval;
			}

			state_head = state_head->prev;

			if (cycle != state_head->cycle) {
				cycle = state_head->cycle;
			}
		}

		// One last bit of fixup since we don't actually track
		// the op pointer
		id_ex_o = find_op(id_ex_inst, false);

		return 0;
	}
}

////////////////////////////////////////////////////////////////////////////////
// STATUS FUNCTIONS
////////////////////////////////////////////////////////////////////////////////

#define DIVIDERe printf("\r================================================================================\n")
#define DIVIDERd printf("--------------------------------------------------------------------------------\n")

static void print_leds_line(void) {
	int i;

	printf("Cycle: %8d ", cycle);

	printf("| R: ");
	for (i = 0; i < 8; i++)
		printf("%d ", !!(leds[RED] & (1 << i)));

	printf("| G: ");
	for (i = 0; i < 8; i++)
		printf("%d ", !!(leds[GRN] & (1 << i)));

	printf("| B: ");
	for (i = 0; i < 8; i++)
		printf("%d ", !!(leds[BLU] & (1 << i)));

	printf("|\n");
}

static void print_reg_state_internal(void) {
	int i;

	printf("Registers:\t\t\t");
	printf("\t  N: %d  Z: %d  C: %d  V: %d  ",
			!!(CPSR & xPSR_N),
			!!(CPSR & xPSR_Z),
			!!(CPSR & xPSR_C),
			!!(CPSR & xPSR_V)
	      );
	printf("| ITSTATE: %02x\n", ITSTATE);
	for (i=0; i<12; ) {
		printf("\tr%02d: %8x\tr%02d: %8x\tr%02d: %8x\tr%02d: %8x\n",
				i, reg[i], i+1, reg[i+1],
				i+2, reg[i+2], i+3, reg[i+3]);
		i+=4;
	}
	printf("\tr12: %8x\t SP: %8x\t LR: %8x\t PC: %8x\n",
			reg[12], SP, LR, PC);
}

static void print_reg_state(void) {
	DIVIDERe;
	print_leds_line();
	DIVIDERd;
	print_reg_state_internal();
}

#ifndef NO_PIPELINE
static void print_stages(void) {
	printf("Stages:\n");
	printf("\tPC's:\tPRE_IF %08x, IF_ID %08x, ID_EX %08x\n",
			pre_if_PC, if_id_PC, id_ex_PC);
}
#endif

static const char *get_dump_name(char c) {
	static char name[] = "/tmp/373rom.\0\0\0\0\0\0\0\0\0";
	if ('\0' == name[strlen("/tmp/373rom.")])
		if (0 != getlogin_r(name + strlen("/tmp/373rom."), 9))
			perror("getting username for rom/ram dump");
	name[strlen("/tmp/373r")] = c;
	return name;
}

static void print_full_state(void) {
	DIVIDERe;
	print_leds_line();

	DIVIDERd;
	print_reg_state_internal();

#ifndef NO_PIPELINE
	DIVIDERd;
	print_stages();
#endif

	DIVIDERd;

	{
		const char *file;
		size_t i;

#ifdef PRINT_ROM_EN
		file = get_dump_name('o');
		FILE* romfp = fopen(file, "w");
		if (romfp) {
			uint32_t rom[ROMSIZE >> 2] = {0};
			for (i = ROMBOT; i < ROMSIZE; i += 4)
				rom[i/4] = read_word(i);

			i = fwrite(rom, ROMSIZE, 1, romfp);
			printf("Wrote %8zu bytes to %-29s "\
					"(Use 'hexdump -C' to view)\n",
					i*ROMSIZE, file);
			fclose(romfp);
		} else {
			perror("No ROM dump");
		}
#endif

		// rom --> ram
		file = get_dump_name('a');

		FILE* ramfp = fopen(file, "w");
		if (ramfp) {
			uint32_t ram[RAMSIZE >> 2] = {0};
			for (i = RAMBOT; i < RAMSIZE; i += 4)
				ram[i/4] = read_word(i);

			i = fwrite(ram, RAMSIZE, 1, ramfp);
			printf("Wrote %8zu bytes to %-29s "\
					"(Use 'hexdump -C' to view)\n",
					i*RAMSIZE, file);
			fclose(ramfp);
		} else {
			perror("No RAM dump");
		}
	}

	DIVIDERe;
}

static void _shell(void) {
	static char buf[100];

	// protect <EOF> replay
	buf[0] = '\0';
	printf("> ");
	if (buf != fgets(buf, 100, stdin)) {
		buf[0] = 'h';
		buf[1] = '\0';
	}

	switch (buf[0]) {
		case '\n':
			dumpatpc = -3;
			printf("%p, %p\n", state_head, state_head->next);
			if (NULL == state_head->next) {
				dumpatcycle = cycle + 1;
				return;
			} else {
				state_seek(cycle + 1);
				print_full_state();
				return _shell();
			}

		case 'p':
			sscanf(buf, "%*s %x", &dumpatpc);
			return;

		case 'b':
			sprintf(buf, "s %d", cycle - 1);
			// fall thru

		case 's':
		{
			int ret;
			int target;
			ret = sscanf(buf, "%*s %d", &target);

			if (-1 == ret) {
				target = cycle + 1;
			} else if (1 != ret) {
				WARN("Error parsing input (ret %d?)\n", ret);
				return _shell();
			}

			if (target < 0) {
				WARN("Ignoring seek to negative cycle\n");
			} else if (target == cycle) {
				WARN("Ignoring seek to current cycle\n");
			} else {
				state_seek(target);
				print_full_state();
			}
			return _shell();
		}

		case 'q':
		case 't':
			exit(EXIT_SUCCESS);

		case 'r':
		{
			const char *file;

#ifdef PRINT_ROM_EN
			if (buf[1] == 'o') {
				file = get_dump_name('o');
			} else
#endif
			if (buf[1] == 'a') {
				file = get_dump_name('a');
			} else {
				file = NULL;
				buf[1] = '\0';
				// now fall through 'c' to help
			}

			if (file) {
				char *cmd;
				assert(-1 != asprintf(&cmd, "hexdump -C %s", file));
				FILE *out = popen(cmd, "r");

				char buf[100];
				while ( fgets(buf, 99, out) ) {
					printf("%s", buf);
				}

				pclose(out);

				return _shell();
			}
		}

		case 'c':
			if (buf[1] == 'y') {
				int requested_cycle;
				sscanf(buf, "%*s %d", &requested_cycle);
				if (requested_cycle < cycle) {
					WARN("Request to execute into the past ignored\n");
					WARN("Did you mean 'seek %d'?\n", requested_cycle);
					return _shell();
				} else if (requested_cycle == cycle) {
					WARN("Request to execute to current cycle ignored\n");
					return _shell();
				} else {
					dumpatcycle = requested_cycle;
					return;
				}
			} else if (buf[1] == 'o') {
				dumpatcycle = 0;
				dumpatpc = 0;
				return;
			}
			// not 'cy' or 'co', fall thru

		case 'h':
		default:
			printf(">> The following commands are recognized:\n");
			printf("   <none>		Advance 1 cycle\n");
			printf("   pc HEX_ADDR		Stop at pc\n");
			printf("   cycle INTEGER	Stop at cycle\n");
			printf("   seek [INTEGER]	Seek to cycle\n");
			printf("                        (forward 1 cycle default)\n");
#ifdef PRINT_ROM_EN
			printf("   rom			Print ROM contents\n");
#endif
			printf("   ram			Print RAM contents\n");
			printf("   continue		Continue\n");
			printf("   terminate		Terminate Simulation\n");
			return _shell();
	}
}

static void shell(void) {
	if (GDB_ATTACHED) {
#ifdef DEBUG1
		print_full_state();
		stop_and_wait_for_gdb();
		print_full_state();
		return;
#else
		return stop_and_wait_for_gdb();
#endif
	}

	print_full_state();
	shell_running = true;
	_shell();
	shell_running = false;
}

/* These are the functions called into by the student simulator project */

/* ARMv7-M implementations treat SP bits [1:0] as RAZ/WI.
 * ARM strongly recommends that software treats SP bits [1:0]
 * as SBZP for maximum portability across ARMv7 profiles.
 */

EXPORT uint32_t CORE_reg_read(int r) {
	assert(r >= 0 && r < 16 && "CORE_reg_read");
	if (r == SP_REG) {
		return SR(&SP) & 0xfffffffc;
	} else if (r == LR_REG) {
		return SR(&LR);
	} else if (r == PC_REG) {
#ifdef NO_PIPELINE
		return SR(&id_ex_PC) & 0xfffffffe;
#else
		return SR(&PC) & 0xfffffffe;
#endif
	} else {
		return SR(&reg[r]);
	}
}

EXPORT void CORE_reg_write(int r, uint32_t val) {
	assert(r >= 0 && r < 16 && "CORE_reg_write");
	if (r == SP_REG) {
		SW(&SP, val & 0xfffffffc);
	} else if (r == LR_REG) {
		SW(&LR, val);
	} else if (r == PC_REG) {
#ifdef NO_PIPELINE
		/*
		if (*state_flags_cur & STATE_DEBUGGING) {
			SW(&pre_if_PC, val & 0xfffffffe);
			SW(&if_id_PC, val & 0xfffffffe);
			SW(&id_ex_PC, val & 0xfffffffe);
		} else {
		*/
			SW(&pre_if_PC, val & 0xfffffffe);
		//}
#else
		if (*state_flags_cur & STATE_DEBUGGING) {
			state_pipeline_flush(val & 0xfffffffe);
		} else {
			// Only flush if the new PC differs from predicted in pipeline:
			if (((SR(&if_id_PC) & 0xfffffffe) - 4) == (val & 0xfffffffe)) {
				DBG2("Predicted PC correctly (%08x)\n", val);
			} else {
				state_pipeline_flush(val & 0xfffffffe);
				DBG2("Predicted PC incorrectly\n");
				DBG2("Pred: %08x, val: %08x\n", SR(&if_id_PC), val);
			}
		}
#endif
	}
	else {
		SW(&(reg[r]), val);
	}
}

EXPORT uint32_t CORE_cpsr_read(void) {
	return SR(&CPSR);
}

EXPORT void CORE_cpsr_write(uint32_t val) {
	if (in_ITblock()) {
		DBG1("WARN update of cpsr in IT block\n");
	}
#ifdef M_PROFILE
	if (val & 0x07f0ffff) {
		DBG1("WARN update of reserved CPSR bits\n");
	}
#endif
	SW(&CPSR, val);
}

#ifdef M_PROFILE
EXPORT uint32_t CORE_ipsr_read(void) {
	return SR(&IPSR);
}

EXPORT void CORE_ipsr_write(uint32_t val) {
	SW(&IPSR, val);
}

EXPORT uint32_t CORE_epsr_read(void) {
	return SR(&EPSR);
}

EXPORT void CORE_epsr_write(uint32_t val) {
	SW(&EPSR, val);
}
#endif

EXPORT uint32_t CORE_red_led_read(void) {
	return SR(&leds[RED]);
}

EXPORT void CORE_red_led_write(uint32_t val) {
	SW(&leds[RED],val);
	state_led_write(RED, val);
}

EXPORT uint32_t CORE_grn_led_read(void) {
	return SR(&leds[GRN]);
}

EXPORT void CORE_grn_led_write(uint32_t val) {
	SW(&leds[GRN],val);
	state_led_write(GRN, val);
}

EXPORT uint32_t CORE_blu_led_read(void) {
	return SR(&leds[BLU]);
}

EXPORT void CORE_blu_led_write(uint32_t val) {
	state_led_write(BLU, val);
}

EXPORT uint8_t CORE_poll_uart_status_read() {
	return poll_uart_status_read();
}

EXPORT void CORE_poll_uart_status_write(uint8_t val) {
	return poll_uart_status_write(val);
}

EXPORT uint8_t CORE_poll_uart_rxdata_read() {
	return poll_uart_rxdata_read();
}

EXPORT void CORE_poll_uart_txdata_write(uint8_t val) {
	return poll_uart_txdata_write(val);
}

EXPORT void CORE_WARN_real(const char *f, int l, const char *msg) {
	WARN("%s:%d\t%s\n", f, l, msg);
}

EXPORT void CORE_ERR_read_only_real(const char *f, int l, uint32_t addr) {
	print_full_state();
	ERR(E_READONLY, "%s:%d\t%#08x is read-only\n", f, l, addr);
}

EXPORT void CORE_ERR_write_only_real(const char *f, int l, uint32_t addr) {
	print_full_state();
	ERR(E_WRITEONLY, "%s:%d\t%#08x is write-only\n", f, l, addr);
}

EXPORT void CORE_ERR_invalid_addr_real(const char *f, int l, uint8_t is_write, uint32_t addr) {
	WARN("CORE_ERR_invalid_addr %s address: 0x%08x\n",
			is_write ? "writing":"reading", addr);
	WARN("Dumping Core...\n");
	print_full_state();
	ERR(E_INVALID_ADDR, "%s:%d\tTerminating due to invalid addr\n", f, l);
}

EXPORT void CORE_ERR_illegal_instr_real(const char *f, int l, uint32_t inst) {
	WARN("CORE_ERR_illegal_instr, inst: %04x\n", inst);
	WARN("Dumping core...\n");
	print_full_state();
	ERR(E_UNKNOWN, "%s:%d\tUnknown inst\n", f, l);
}

EXPORT void CORE_ERR_unpredictable_real(const char *f, int l, const char *opt_msg) {
	ERR(E_UNPREDICTABLE, "%s:%d\tCORE_ERR_unpredictable -- %s\n", f, l, opt_msg);
}

EXPORT void CORE_ERR_not_implemented_real(const char *f, int l, const char *opt_msg) {
	ERR(E_NOT_IMPLEMENTED, "%s:%d\tCORE_ERR_not_implemented -- %s\n", f, l, opt_msg);
}


////////////////////////////////////////////////////////////////////////////////
// SIMULATOR
////////////////////////////////////////////////////////////////////////////////

EXPORT struct op *ops = NULL;

static int _register_opcode_mask(uint32_t ones_mask, uint32_t zeros_mask,
		bool is16, void *fn, const char* fn_name, va_list va_args) {
	struct op *o = malloc(sizeof(struct op));

	o->prev = NULL;
	o->next = ops;		// ops is NULL on first pass, this is fine
	o->ones_mask = ones_mask;
	o->zeros_mask = zeros_mask;
	o->is16 = is16;
	if (is16)
		o->fn.fn16 = fn;
	else
		o->fn.fn32 = fn;
	o->name = fn_name;

	o->ex_cnt = 0;
	o->ex_ones = NULL;
	o->ex_zeros = NULL;

	ones_mask  = va_arg(va_args, uint32_t);
	zeros_mask = va_arg(va_args, uint32_t);
	while (ones_mask || zeros_mask) {
		// Make the assumption that callers will have one, at most
		// two exceptions; go with the simple realloc scheme
		unsigned idx = o->ex_cnt;

		o->ex_cnt++;
		o->ex_ones  = realloc(o->ex_ones,  o->ex_cnt * sizeof(uint32_t));
		assert(NULL != o->ex_ones && "realloc");
		o->ex_zeros = realloc(o->ex_zeros, o->ex_cnt * sizeof(uint32_t));
		assert(NULL != o->ex_zeros && "realloc");

		o->ex_ones[idx]  = ones_mask;
		o->ex_zeros[idx] = zeros_mask;

		ones_mask  = va_arg(va_args, uint32_t);
		zeros_mask = va_arg(va_args, uint32_t);
	}

	// Add new element to list head b/c that's easy and it doesn't matter
	if (NULL == ops) {
		ops = o;
	} else {
		ops->prev = o;
		ops = o;
	}

	return ++opcode_masks;
}

static int register_opcode_mask_ex(uint32_t ones_mask, uint32_t zeros_mask,
		bool is16, void *fn, const char* fn_name, va_list va_args) {

	if (is16)
		assert(0xffff0000 == (0xffff0000 & zeros_mask));

	if (NULL == ops) {
		// first registration
		return _register_opcode_mask(ones_mask, zeros_mask,
				is16, fn, fn_name, va_args);
	}

	struct op* o = ops;

	// XXX: Make better
	while (NULL != o) {
		if (
				(o->ones_mask  == ones_mask) &&
				(o->zeros_mask == zeros_mask)
		   ) {
			break;
		}
		o = o->next;
	}
	if (o) {
		ERR(E_BAD_OPCODE, "Duplicate opcode mask.\n"\
				"\tExisting  registration: 1's %x, 0's %x (%s)\n"\
				"\tAttempted registration: 1's %x, 0's %x (%s)\n"\
				, o->ones_mask, ones_mask, o->name\
				, o->zeros_mask, zeros_mask, o->name);
	}

	return _register_opcode_mask(ones_mask, zeros_mask,
			is16, fn, fn_name, va_args);
}

EXPORT int register_opcode_mask_16_ex_real(uint16_t ones_mask, uint16_t zeros_mask,
		void (*fn) (uint16_t), const char *fn_name, ...) {
	va_list va_args;
	va_start(va_args, fn_name);

	int ret;

	ret = register_opcode_mask_ex(ones_mask, 0xffff0000 | zeros_mask,
			true, fn, fn_name, va_args);

	va_end(va_args);

	return ret;
}

EXPORT int register_opcode_mask_16_real(uint16_t ones_mask, uint16_t zeros_mask,
		void (*fn) (uint16_t), const char* fn_name) {
	return register_opcode_mask_16_ex_real(ones_mask, zeros_mask,
			fn, fn_name, 0, 0);
}

EXPORT int register_opcode_mask_32_ex_real(uint32_t ones_mask, uint32_t zeros_mask,
		void (*fn) (uint32_t), const char* fn_name, ...) {

	if ((zeros_mask & 0xffff0000) == 0) {
		WARN("%s registered zeros_mask requiring none of the top 4 bytes\n",
				fn_name);
		ERR(E_BAD_OPCODE, "Use register_opcode_mask_16 instead");
	}

	va_list va_args;
	va_start(va_args, fn_name);

	int ret;
	ret = register_opcode_mask_ex(ones_mask, zeros_mask,
			false, fn, fn_name, va_args);

	va_end(va_args);

	return ret;
}

EXPORT int register_opcode_mask_32_real(uint32_t ones_mask, uint32_t zeros_mask,
		void (*fn) (uint32_t), const char *fn_name) {
	return register_opcode_mask_32_ex_real(ones_mask, zeros_mask, fn, fn_name, 0, 0);
}

static int sim_execute(void) {
	// XXX: What if the debugger wants to execute the same instruction two cycles in a row? How do we allow this?
	static uint32_t prev_pc = STALL_PC;
	if ((prev_pc == PC) && (prev_pc != STALL_PC)) {
		if (GDB_ATTACHED) {
			INFO("Simulator determined PC 0x%08x is branch to self, breaking for gdb.\n", PC);
			shell();
		} else {
			INFO("Simulator determined PC 0x%08x is branch to self, terminating.\n", PC);
			sim_terminate();
		}
	} else {
		prev_pc = PC;
	}
#ifdef NO_PIPELINE
	// Not the common code path, try to catch if things have changed
	assert(NUM_TICKERS == 3);

	cycle++;

	state_start_tick();
	DBG1("tick_if\n");
	tick_if();
	DBG1("tick_id\n");
	tick_id();
	DBG1("tick_ex\n");
	tick_ex();
	state_tock();

#else
	// Now we're committed to starting the next cycle
	cycle++;

	int i;

	// Start a clock tick
	state_start_tick();

	// Don't let stages steal wakeups
	for (i = 0; i < NUM_TICKERS; i++) {
		sem_wait(&ticker_ready_sem);
	}

	// Start off each stage
	for (i = 0; i < NUM_TICKERS; i++) {
		sem_post(&start_tick_sem);
	}

	// Wait for all stages to complete
	for (i = 0; i < NUM_TICKERS; i++) {
		sem_wait(&end_tick_sem);
	}

	// Latch hardware
	state_tock();

	// Notify stages this cycle is complete
	for (i = 0; i < NUM_TICKERS; i++) {
		sem_post(&end_tock_sem);
	}
#endif

	return SUCCESS;
}

static void sim_reset(void) __attribute__ ((noreturn));
static void sim_reset(void) {
	int ret;

	if (GDB_ATTACHED) {
		gdb_init(gdb_port);
#ifdef DEBUG1
		print_full_state();
		wait_for_gdb();
		print_full_state();
#else
		wait_for_gdb();
#endif
	}

	INFO("Asserting reset pin\n");
	cycle = 0;
	state_start_tick();
	reset();
	state_tock();
	INFO("De-asserting reset pin\n");

	INFO("Entering main loop...\n");
	do {
		// Slow things down?
		if (slowsim) {
			static struct timespec s = {0, NSECS_PER_SEC/10};
			nanosleep(&s, NULL);
		}

		if (sigint) {
			sigint = 0;
			shell();
		} else
		if ((limitcycles != -1) && limitcycles <= cycle) {
			ERR(E_UNKNOWN, "Cycle limit (%d) reached.\n", limitcycles);
		} else
		if (dumpatcycle == cycle) {
			shell();
		} else
		if ((dumpatpc & 0xfffffffe) == (PC & 0xfffffffe)) {
			shell();
		} else
		if (dumpallcycles) {
			print_reg_state();
		}
	} while (SUCCESS == (ret = sim_execute()) );

	WARN("Simulation terminated with error code: %u\n", ret);
	WARN("Dumping core...\n");
	print_full_state();
	ERR(ret, "Terminating\n");
}

EXPORT void sim_terminate(void) {
	join_periph_threads();
	print_full_state();
	if (returnr0) {
		DBG2("Return code is r0: %08x\n", reg[0]);
		exit(reg[0]);
	}
	exit(EXIT_SUCCESS);
}

static void power_on(void) __attribute__ ((noreturn));
static void power_on(void) {
	INFO("Powering on processor...\n");
	sim_reset();
}

static void* core_thread(void *unused __attribute__ ((unused)) ) {
	power_on();
}

static void load_opcodes(void) {
	INFO("Registered %d opcode mask%s\n", opcode_masks,
			(opcode_masks == 1) ? "":"s");

#ifndef NO_PIPELINE
	// Fake instructions used to propogate pipeline exceptions
	register_opcode_mask_real(INST_HAZARD, ~INST_HAZARD, pipeline_exception, "Pipeline Excpetion");
#endif
}

static void* sig_thread(void *arg) {
	sigset_t *set = (sigset_t *) arg;
	int s, sig;

	for (;;) {
		s = sigwait(set, &sig);
		if (s != 0) {
			ERR(E_UNKNOWN, "sigwait failed?\n");
		}

		if (sig == SIGINT) {
			if (shell_running) {
				flockfile(stdout); flockfile(stderr);
				printf("\nQuit\n");
				exit(0);
			} else {
				if (sigint == 1) {
					flockfile(stdout); flockfile(stderr);
					printf("\nQuit\n");
					exit(0);
				}
				INFO("Caught SIGINT, again to quit\n");
				sigint = 1;
			}
		} else {
			ERR(E_UNKNOWN, "caught unknown signal %d\n", sig);
		}
	}
}

EXPORT void simulator(const char *flash_file, uint16_t polluartport) {
	// Init uninit'd globals
#ifndef NO_PIPELINE
	assert(0 == sem_init(&ticker_ready_sem, 0, 0));
	assert(0 == sem_init(&start_tick_sem, 0, 0));
	assert(0 == sem_init(&end_tick_sem, 0, 0));
	assert(0 == sem_init(&end_tock_sem, 0, 0));
#endif

	// Read in flash
	if (usetestflash) {
		flash_ROM(static_rom, STATIC_ROM_NUM_BYTES);
		INFO("Loaded internal test flash\n");
	} else {
		if (NULL == flash_file) {
			if (GDB_ATTACHED) {
				WARN("No binary image specified, you will have to 'load' one with gdb\n");
			} else {
				ERR(E_BAD_FLASH, "--flash or --usetestflash required, see --help\n");
			}
		} else {
			int flashfd = open(flash_file, O_RDONLY);
			ssize_t ret;

			if (-1 == flashfd) {
				ERR(E_BAD_FLASH, "Could not open file %s for reading\n",
						flash_file);
			}

			uint32_t rom[ROMSIZE >> 2] = {0};

			assert (ROMSIZE < SSIZE_MAX);
			ret = read(flashfd, rom, ROMSIZE);
			if (ret < 0) {
				WARN("%s\n", strerror(errno));
				ERR(E_BAD_FLASH, "Failed to read flash file %s\n", flash_file);
			}
			flash_ROM(rom, ret);
			INFO("Succesfully loaded flash ROM: %s\n", flash_file);
		}
	}

	load_opcodes();

	// Prep signal-related stuff:
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	assert(0 == pthread_sigmask(SIG_BLOCK, &set, NULL));

	// Spawn signal handling thread
	pthread_t sig_pthread;
	pthread_create(&sig_pthread, NULL, &sig_thread, (void *) &set);

	// Spawn uart thread
	pthread_mutex_lock(&poll_uart_mutex);
	pthread_create(&poll_uart_pthread, NULL, poll_uart_thread, &polluartport);
	pthread_cond_wait(&poll_uart_cond, &poll_uart_mutex);
	pthread_mutex_unlock(&poll_uart_mutex);

	// Spawn pipeline stage threads
#ifndef NO_PIPELINE
	pthread_t ifstage_pthread;
	pthread_t idstage_pthread;
	pthread_t exstage_pthread;
	pthread_create(&ifstage_pthread, NULL, ticker, tick_if);
	pthread_create(&idstage_pthread, NULL, ticker, tick_id);
	pthread_create(&exstage_pthread, NULL, ticker, tick_ex);
#endif

	// Simulator CORE thread
	pthread_t core_pthread;
	pthread_create(&core_pthread, NULL, core_thread, NULL);

	// Everything is up and running now
	pthread_join(core_pthread, NULL);

	// Should not get here, proper exit comes from self-branch detection
	ERR(E_UNKNOWN, "core thread terminated unexpectedly\n");
}

static void join_periph_threads(void) {
	INFO("Shutting down the polling uart peripheral\n");
	poll_uart_shutdown = true;
	pthread_join(poll_uart_pthread, NULL);
}


////////////////////////////////////////////////////////////////////////////////
// UART THREAD(S)
////////////////////////////////////////////////////////////////////////////////

static void *poll_uart_thread(void *arg_v) {
	uint16_t port = *((uint16_t *) arg_v);

	int sock;
	struct sockaddr_in server;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (-1 == sock) {
		ERR(E_UNKNOWN, "Creating UART device: %s\n", strerror(errno));
	}

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);
	int on = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	if (-1 == bind(sock, (struct sockaddr *) &server, sizeof(server))) {
		ERR(E_UNKNOWN, "Creating UART device: %s\n", strerror(errno));
	}

	if (-1 == listen(sock, 1)) {
		ERR(E_UNKNOWN, "Creating UART device: %s\n", strerror(errno));
	}

	INFO("UART listening on port %d (use `nc -4 localhost %d` to communicate)\n",
			port, port);

	pthread_cond_signal(&poll_uart_cond);

	while (1) {
		int client;
		while (1) {
			fd_set set;
			struct timeval timeout;

			FD_ZERO(&set);
			FD_SET(sock, &set);

			timeout.tv_sec = 0;
			timeout.tv_usec = 100000;

			if (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
				client = accept(sock, NULL, 0);
				break;
			} else {
				if (poll_uart_shutdown) {
					INFO("Polling UART device shut down\n");
					pthread_exit(NULL);
				}
			}
		}

		if (-1 == client) {
			ERR(E_UNKNOWN, "UART device failure: %s\n", strerror(errno));
		}

		INFO("UART connected\n");

		static const char *welcome = "\
>>MSG<< You are now connected to the UART polling device\n\
>>MSG<< Lines prefixed with '>>MSG<<' are sent from this\n\
>>MSG<< UART <--> network bridge, not the connected device\n\
>>MSG<< This device has a "VAL2STR(POLL_UART_BUFSIZE)" byte buffer\n\
>>MSG<< This device operates at "VAL2STR(POLL_UART_BAUD)" baud\n\
>>MSG<< To send a message, simply type and press the return key\n\
>>MSG<< All characters, up to and including the \\n will be sent\n";

		if (-1 == send(client, welcome, strlen(welcome), 0)) {
			ERR(E_UNKNOWN, "%d UART: %s\n", __LINE__, strerror(errno));
		}

		pthread_mutex_lock(&poll_uart_mutex);
		SW_A(&poll_uart_client, client);
		pthread_mutex_unlock(&poll_uart_mutex);

		static uint8_t c;
		static int ret;
		while (1) {
			// n.b. If the baud rate is set to a speed s.t. polling
			// becomes CPU intensive (not likely..), this could be
			// replaced with select + self-pipe
			nanosleep(&poll_uart_baud_sleep, NULL);

			ret = recv(client, &c, 1, MSG_DONTWAIT);

			if (poll_uart_shutdown) {
				SW_A(&poll_uart_client, INVALID_CLIENT);
				static const char *goodbye = "\
\n\
>>MSG<< An extra newline has been printed before this line\n\
>>MSG<< The polling UART device has shut down. Good bye.\n";
				send(client, goodbye, strlen(goodbye), 0);
				close(client);
				INFO("Polling UART disconnected from client\n");
				INFO("Polling UART device shut down\n");
				pthread_exit(NULL);
			}

			if (ret != 1) {
				// Common case: poll
				if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
					continue;
				}

				break;
			}

			pthread_mutex_lock(&poll_uart_mutex);
			state_async_block_start();

			uint32_t* head = SRP(&poll_uart_head);
			uint32_t* tail = SRP(&poll_uart_tail);

			DBG1("recv start\thead: %td, tail: %td\n",
					(head)?head - poll_uart_buffer:-1,
					tail - poll_uart_buffer);

			SW(tail, c);
			if (NULL == head) {
				head = tail;
				SWP(&poll_uart_head, head);
			}
			tail++;
			if (tail == (poll_uart_buffer + POLL_UART_BUFSIZE))
				tail = poll_uart_buffer;
			SWP(&poll_uart_tail, tail);

			DBG1("recv end\thead: %td, tail: %td\t[%td=%c]\n",
					(head)?head - poll_uart_buffer:-1,
					tail - poll_uart_buffer,
					tail-poll_uart_buffer-1, *(tail-1));

			state_async_block_end();
			pthread_mutex_unlock(&poll_uart_mutex);
		}

		if (ret == 0) {
			INFO("UART client has closed connection"\
				"(no more data in but you can still send)\n");
			pthread_mutex_lock(&poll_uart_mutex);
			// Dodge small race window to miss wakeup
			if (SR_A(&poll_uart_client) != INVALID_CLIENT)
				pthread_cond_wait(&poll_uart_cond, &poll_uart_mutex);
		} else {
			WARN("Lost connection to UART client (%s)\n", strerror(errno));
			pthread_mutex_lock(&poll_uart_mutex);
		}
		SW_A(&poll_uart_client, INVALID_CLIENT);
		pthread_mutex_unlock(&poll_uart_mutex);
	}
}

uint8_t poll_uart_status_read() {
	uint8_t ret = 0;

	pthread_mutex_lock(&poll_uart_mutex);
	state_async_block_start();
	ret |= (SRP(&poll_uart_head) != NULL) << POLL_UART_RXBIT; // data avail?
	ret |= (SR(&poll_uart_client) == INVALID_CLIENT) << POLL_UART_TXBIT; // tx busy?
	state_async_block_end();
	pthread_mutex_unlock(&poll_uart_mutex);

	// For lock contention
	nanosleep(&poll_uart_baud_sleep, NULL);

	return ret;
}

void poll_uart_status_write() {
	pthread_mutex_lock(&poll_uart_mutex);
	state_async_block_start();
	SWP(&poll_uart_head, NULL);
	SWP(&poll_uart_tail, poll_uart_buffer);
	state_async_block_end();
	pthread_mutex_unlock(&poll_uart_mutex);

	// For lock contention
	nanosleep(&poll_uart_baud_sleep, NULL);
}

uint8_t poll_uart_rxdata_read() {
	uint8_t ret;

#ifdef DEBUG1
	int idx;
#endif

	pthread_mutex_lock(&poll_uart_mutex);
	state_async_block_start();
	if (NULL == SRP(&poll_uart_head)) {
		DBG1("Poll UART RX attempt when RX Pending was false\n");
		ret = SR(&poll_uart_buffer[3]); // eh... rand? 3, why not?
	} else {
#ifdef DEBUG1
		idx = SRP(&poll_uart_head) - poll_uart_buffer;
#endif
		uint32_t* head = SRP(&poll_uart_head);
		uint32_t* tail = SRP(&poll_uart_tail);

		ret = *head;

		head++;
		if (head == (poll_uart_buffer + POLL_UART_BUFSIZE))
			head = poll_uart_buffer;
		if (head == tail)
			head = NULL;

		DBG1("rxdata end\thead: %td, tail: %td\t[%td=%c]\n",
				(head)?head - poll_uart_buffer:-1,
				tail - poll_uart_buffer,
				(head)?head-poll_uart_buffer:-1,(head)?*head:'\0');

		SWP(&poll_uart_head, head);
	}
	state_async_block_end();
	pthread_mutex_unlock(&poll_uart_mutex);

	DBG1("UART read byte: %c %x\tidx: %d\n", ret, ret, idx);

	return ret;
}

void poll_uart_txdata_write(uint8_t val) {
	DBG1("UART write byte: %c %x\n", val, val);

	static int ret;

	pthread_mutex_lock(&poll_uart_mutex);
	uint32_t client = SR_A(&poll_uart_client);
	if (INVALID_CLIENT == client) {
		DBG1("Poll UART TX ignored as client is busy\n");
		// XXX warn? no, grade
		// no connected client (TX is busy...) so drop
	}
	else if (-1 ==  ( ret = send(client, &val, 1, 0))  ) {
		WARN("%d UART: %s\n", __LINE__, strerror(errno));
		SW_A(&poll_uart_client, INVALID_CLIENT);
		pthread_cond_signal(&poll_uart_cond);
	}
	pthread_mutex_unlock(&poll_uart_mutex);

	DBG2("UART byte sent %c\n", val);
}
