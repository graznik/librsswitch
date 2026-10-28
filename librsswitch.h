/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef LIBRSSWITCH_H
#define LIBRSSWITCH_H

#include <stdint.h>

/*
 * Marks the functions that make up the public interface of the shared
 * library. The library is compiled with -fvisibility=hidden, so every
 * declaration without this marker stays internal to the library and is
 * not callable from a program that links it.
 */
#if defined(__GNUC__)
#define RSS_API __attribute__((visibility("default")))
#else
#define RSS_API
#endif

/*
 * The version of the library, as the numbers below and as the string
 * LIBRSSWITCH_VERSION built from them. It is the version of the
 * library itself and independent of the tags of the project.
 *
 * MAJOR is the SONAME of the shared library and changes only for an
 * incompatible ABI change. MINOR grows when API is added, PATCH with
 * fixes that change no interface. Each number is a plain decimal
 * constant on a line of its own, so that a build script can read it.
 *
 * These are the values of the header a program was compiled with.
 * rss_version() reports the library that is actually loaded.
 */
#define LIBRSSWITCH_VERSION_MAJOR 1
#define LIBRSSWITCH_VERSION_MINOR 0
#define LIBRSSWITCH_VERSION_PATCH 0

#define RSS_STR_(x) #x
#define RSS_STR(x) RSS_STR_(x)
#define LIBRSSWITCH_VERSION RSS_STR(LIBRSSWITCH_VERSION_MAJOR) "." \
			    RSS_STR(LIBRSSWITCH_VERSION_MINOR) "." \
			    RSS_STR(LIBRSSWITCH_VERSION_PATCH)

/*
 * Thread safety
 *
 * rss_send(), rss_send_codeword() and rss_gpio_config() are
 * safe to call concurrently from multiple threads of the same
 * process. Every send session -- board_init(),
 * all repetitions, board_exit() -- is serialized by one library-wide
 * lock, so concurrent sends never interleave their pulses; a calling
 * thread simply blocks until the previous send session completes.
 * rss_gpio_config() takes the same lock around its store, so a
 * config change is ordered against sends that start after it returns
 * (it does not affect a session already in progress). It checks the
 * string before it takes the lock and keeps a copy, so the string need
 * not outlive the call.
 *
 * This guarantee is process-local only: two separate processes
 * driving the same GPIO line (for example rsssend run once while
 * rsswitchd is active) are not serialized against each other.
 *
 * The library is not async-signal-safe -- never call any of these
 * functions from a signal handler. Cancelling a thread while it is
 * inside a send is not supported; doing so may leave the lock held.
 *
 * rss_addr_limits() and rss_gpio_config_check() take no lock at
 * all and read no shared state. The first only reads the same constant
 * tables rss_send() checks addresses against; the second only
 * parses the string it is given, and neither touches gpio_config or
 * opens the board. They are safe to call concurrently with any of the
 * functions above, from any thread.
 *
 * rss_last_error() takes no lock either: it reads state that is
 * private to the calling thread, so it is safe from any thread and a
 * send in another thread never changes what it returns.
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sets the GPIO config string passed to board_init() on every
 * subsequent rss_send() and rss_send_codeword() call. NULL restores
 * the board's compiled-in default, which is also what is used if this
 * function is never called. Format is board-specific -- see the README
 * section "GPIO configuration"; the libgpiod boards parse it in
 * boards/gpiod/gpiod_config.c.
 *
 * The string is checked as rss_gpio_config_check() checks it, and
 * then copied into a buffer of the library: at most 79 characters fit,
 * and the caller's string is not kept, so it may be changed, freed or
 * go out of scope as soon as the call returns. A string of 80 or more
 * characters is refused without being read further than that.
 *
 * Returns 0 on success, with rss_last_error() set to RSS_ERR_NONE.
 * Returns -1 with rss_last_error() RSS_ERR_ARG for a string that is
 * too long or malformed, and the previous config stays in effect, or
 * with RSS_ERR_OTHER if the library's lock fails. The cause of a
 * rejected string is logged. A running send is not affected, and the
 * call waits for it to finish before it stores the copy.
 */
RSS_API int32_t rss_gpio_config(const char *config);

/*
 * Checks a GPIO config string without storing it and without touching
 * the GPIO. Returns 0 for NULL (the board default) and for a config
 * the board can parse, and -1 for a malformed one, with the cause
 * logged by the board. Well-formed does not mean the chip exists: only
 * the format is checked, and the first rss_send() still reports a
 * missing chip. rss_gpio_config() makes the same check itself; this
 * function is for a caller that wants to know without storing. It
 * takes no lock, never touches the board and leaves rss_last_error()
 * unchanged.
 */
RSS_API int32_t rss_gpio_config_check(const char *config);

/*
 * Sends a switch command for one of the two built-in device tables.
 *
 * dev is RSS_REV_008345 or RSS_POLLIN_2605 (see enum rss_type below). group
 * is 0-3 for REV 008345 and 0-31 for Pollin 2605. socket is 0-2 for
 * REV 008345 and 0-31 for Pollin 2605. data is 0 for off, 1 for on.
 *
 * Arguments are fully validated before board_init() or any GPIO
 * access: an invalid dev, group, socket or data is logged and never
 * touches the board.
 *
 * On success, the device's codeword is sent NTIMES (10) times, each
 * repetition followed by a sync pulse. The stub board prints the
 * codeword to stdout.
 *
 * Returns 0 on success, -1 for an invalid argument or a board error;
 * rss_last_error() then tells why. Serialized with
 * rss_send_codeword() by the same lock -- see "Thread safety"
 * above.
 */
RSS_API int32_t rss_send(uint32_t dev, uint32_t group,
			 uint32_t socket, uint32_t data);

/*
 * Reports the valid address range for dev: groups 0 to *ngroups - 1,
 * sockets 0 to *nsockets - 1, data 0 to *ndata - 1 -- the same bounds
 * rss_send() enforces internally, read from the same constant
 * tables. Lets a caller such as rsswitchd validate an address itself
 * before calling rss_send(), without hand-copying the table sizes,
 * so the two checks cannot drift apart.
 *
 * Returns 0 and fills *ngroups, *nsockets and *ndata, or -1 for an
 * unknown dev or a NULL pointer, leaving the outputs untouched. Takes
 * no lock and never touches the board -- see "Thread safety" above.
 */
RSS_API int32_t rss_addr_limits(uint32_t dev, uint32_t *ngroups,
				uint32_t *nsockets, uint32_t *ndata);

/*
 * Bounds for rss_send_codeword()'s tristate argument: byte length
 * before the first NUL, in [RSS_CODEWORD_MIN_LEN,
 * RSS_CODEWORD_MAX_LEN]. 12 is the PT2260/PT2262 tri-state word
 * length -- both supported chips use a 12-symbol codeword.
 */
#define RSS_CODEWORD_MIN_LEN 1U
#define RSS_CODEWORD_MAX_LEN 12U

/*
 * Bounds for rss_send_codeword()'s pulse_len argument, in
 * microseconds. Wide enough for both supported chips at their
 * documented 350us pulse, narrow enough that a typo cannot hold the
 * GPIO line high for more than a few milliseconds per pulse.
 */
#define RSS_PULSE_LEN_MIN_US 50U
#define RSS_PULSE_LEN_MAX_US 2000U

/*
 * Sends a raw tri-state codeword, instead of one of the two built-in
 * device tables rss_send() encodes.
 *
 * tristate's length (the number of bytes before its first NUL) must
 * be within [RSS_CODEWORD_MIN_LEN, RSS_CODEWORD_MAX_LEN]. Every
 * one of those bytes must be the ASCII character '0', '1' or
 * uppercase 'F' -- lowercase, whitespace, and any other byte
 * (including a multi-byte UTF-8 sequence) is rejected. tristate is
 * never modified or truncated: it is sent exactly as given, or
 * rejected in full.
 *
 * pulse_len is in microseconds and must be within
 * [RSS_PULSE_LEN_MIN_US, RSS_PULSE_LEN_MAX_US].
 *
 * On success, tristate is sent NTIMES (10 -- the same repeat count
 * rss_send() uses), each repetition followed by a sync pulse.
 * Input is fully validated before board_init() or any GPIO access;
 * an invalid tristate or pulse_len never touches the board. The stub
 * board prints the codeword to stdout, exactly as it does for
 * rss_send().
 *
 * Returns 0 on success, -1 on error (invalid input, or a board
 * error); rss_last_error() then tells why. Serialized with
 * rss_send() by the same lock -- see "Thread safety" above.
 */
RSS_API int32_t rss_send_codeword(const char *tristate, uint32_t pulse_len);

/*
 * Why a send failed. The values are part of the library's ABI: they
 * never change, and a later cause is only ever added after
 * RSS_ERR_ARG.
 *
 * RSS_ERR_NONE:    no failure.
 * RSS_ERR_OTHER:   a failure the library cannot classify, including
 *                     every board that only reports a plain -1.
 * RSS_ERR_NOCHIP:  the GPIO chip does not exist.
 * RSS_ERR_BUSY:    the line is in use by another program or process.
 * RSS_ERR_DENIED:  no permission to open the chip.
 * RSS_ERR_BADLINE: the line offset is not valid for the chip.
 * RSS_ERR_IO:      driving the line failed during the send.
 * RSS_ERR_ARG:     the call was rejected before the board was
 *                     touched: an invalid dev, group, socket, data,
 *                     codeword or pulse_len, or a GPIO config string
 *                     that rss_gpio_config() refused.
 */
enum rss_err {
		 RSS_ERR_NONE = 0,
		 RSS_ERR_OTHER = 1,
		 RSS_ERR_NOCHIP = 2,
		 RSS_ERR_BUSY = 3,
		 RSS_ERR_DENIED = 4,
		 RSS_ERR_BADLINE = 5,
		 RSS_ERR_IO = 6,
		 RSS_ERR_ARG = 7
};

/*
 * Returns the cause of the calling thread's last rss_send(),
 * rss_send_codeword() or rss_gpio_config() call: RSS_ERR_NONE after a
 * success, the reason after a failure. Each of these calls sets it,
 * so a success never leaves a stale cause behind; every other library
 * function leaves it unchanged. Before the thread's first such call
 * it is RSS_ERR_NONE.
 * It takes no lock and never blocks. The cause is per thread: a send
 * in another thread never changes it.
 */
RSS_API enum rss_err rss_last_error(void);

enum rss_type {
		  RSS_REV_008345,
		  RSS_POLLIN_2605
};

/*
 * Returns the version of the library that is actually loaded, as
 * "MAJOR.MINOR.PATCH" in static storage that the caller must not
 * modify or free. It may differ from LIBRSSWITCH_VERSION of the
 * header a program was compiled with, when the program runs against
 * another build of the library. Never returns NULL; takes no lock and
 * never touches the board.
 */
RSS_API const char *rss_version(void);

/*
 * Sets the function that receives the library's log messages, which
 * the library and its boards otherwise write to the system log with
 * syslog(). The messages are errors and, in a build made with DEBUG,
 * notices. fn is called as fn(priority, msg, ctx) with the ctx given
 * here:
 *
 * - priority is a syslog value, LOG_ERR or LOG_NOTICE.
 * - msg is finished text, never a format string, of at most 255 bytes
 *   plus its NUL; a longer message is cut. It is valid only during
 *   the call, so a handler that wants to keep it copies it.
 *
 * fn == NULL restores the default, which passes msg to syslog().
 *
 * Calling rules. Set the handler once, before the first send, and
 * before other threads use the library; the pair {fn, ctx} is stored
 * and read atomically, so changing it later never mixes the function
 * of one call with the context of another, but a message already on
 * its way still goes to the handler it started with. The handler
 * runs synchronously in the thread that called into the library, so
 * it can block that thread, and it may run with the library's send
 * lock held and from several threads at once (rss_gpio_config_check()
 * logs without any lock). The handler must therefore be thread-safe
 * and must not call any librsswitch function.
 */
RSS_API void rss_set_log_handler(void (*fn)(int priority, const char *msg, void *ctx),
				 void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIBRSSWITCH_H */
