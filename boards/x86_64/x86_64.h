#ifndef X86_64_H
#define X86_64_H

#include "../board.h"

/*
 * Test-only pulse recorder, x86_64 board only.
 *
 * Every transmit() call on this board appends the pulse it was asked
 * to send instead of driving any GPIO. This lets host tests decode
 * the recorded stream back into 0/1/F/sync frames and assert on the
 * exact codeword a socket_send() call produced.
 *
 * The recorder does no locking of its own: it relies on the library
 * serializing every transmit() call for the duration of one
 * socket_send()/socket_send_codeword() (board_init() -> ... ->
 * board_exit()), and on tests reading it only after all senders for
 * the current test have finished. It must never be called
 * concurrently with a send in flight.
 */
#include <stddef.h>

#define X86_64_RECORDER_MAX_PULSES 16384U

struct x86_64_pulse {
	uint32_t nhigh;
	uint32_t nlow;
	uint32_t pulse_len;
};

/*
 * Clears every recorded pulse, the overflow flag and the
 * board_init()/board_exit() call counters.
 */
void x86_64_recorder_reset(void);

/* Number of pulses recorded since the last reset. */
size_t x86_64_recorder_count(void);

/*
 * Returns the pulse at index, or NULL when index is out of range
 * (index >= x86_64_recorder_count()).
 */
const struct x86_64_pulse *x86_64_recorder_get(size_t index);

/*
 * Set once the recorder buffer filled up and a pulse had to be
 * dropped. Sticky until the next x86_64_recorder_reset(). The
 * recorder never aborts or exits the process on overflow -- this
 * board also backs long-running x86_64 builds of rsssend and
 * rsswitchd, which must keep working regardless of how many pulses
 * they have sent.
 */
int x86_64_recorder_overflowed(void);

/* Number of board_init() calls since the last reset. */
size_t x86_64_board_init_count(void);

/* Number of board_exit() calls since the last reset. */
size_t x86_64_board_exit_count(void);

/*
 * Arms one-shot failure injection: once the recorder holds index
 * pulses, the next transmit() call returns -1 without recording and
 * disarms itself, so a caller that (incorrectly) ignores the error
 * and keeps sending observably diverges from one that aborts -- later
 * pulses are then recorded again instead of every call failing
 * forever. x86_64_recorder_reset() also disarms it.
 */
void x86_64_recorder_fail_at(size_t index);

#endif /* X86_64_H */
