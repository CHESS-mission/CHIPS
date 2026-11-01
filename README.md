# CHIPS

## Building

CHIPS has no build system requirement and no dependencies beyond a C99 compiler. To use it in your project:

1. Clone the repository.
2. Add `include/` to your include path.
3. Compile the sources you need alongside your own:
   - `src/core.c`: always (framing, byte stuffing, CRC).
   - `src/link.c`: with the master or the slave (port I/O shared by both).
   - `src/master.c`: if the device sends requests.
   - `src/slave.c`: if the device answers requests.

```c
#include <chips/core.h>   /* framing only */
#include <chips/master.h> /* or */
#include <chips/slave.h>
```

For example, to build the demo from the repository root:

```sh
cc -std=c99 -I include -o chips_demo demos/core_demo.c src/core.c
```

### Development (CMake)

The CMake setup exists for working on CHIPS itself, to build the unit tests and demos. It is not a supported way to consume the library, so integrate it as described above.

| Option | Default | Description |
|---|---|---|
| `CHIPS_BUILD_TESTS` | `OFF` | Build the unit tests (requires GoogleTest). |
| `CHIPS_BUILD_DEMOS` | `OFF` | Build the demos in `demos/`. |

```sh
cmake -S . -B build -DCHIPS_BUILD_TESTS=ON -DCHIPS_BUILD_DEMOS=ON
cmake --build build
ctest --test-dir build
```

## Configuration

CHIPS can be tuned at compile time. Put your overrides in a header of your own and point `CHIPS_CONFIG_FILE` at it. `chips/core.h` includes that file before applying its defaults, and `chips/master.h` and `chips/slave.h` include `chips/core.h` first, so anything you define there takes precedence everywhere.

```c
/* chips_config.h */
#define CHIPS_ASSERT(expr) my_assert_handler(expr)
#define CHIPS_CRC16_KERMIT_FOLD (4u)
#define CHIPS_RX_CHUNK (64u)
```

Define the macro as a quoted header name, and make sure the file's directory is on the include path:

```sh
cc -std=c99 -I include -I config -DCHIPS_CONFIG_FILE='"chips_config.h"' \
   -o app main.c src/core.c src/link.c src/master.c
```

Use the same configuration for every file that includes a CHIPS header, including the library sources. `CHIPS_RX_CHUNK` changes the size of `chips_master_t` and `chips_slave_t`, so a mismatch between translation units silently breaks the instance layout.

| Option | Default | Description |
|---|---|---|
| `CHIPS_CONFIG_FILE` | undefined | Quoted name of a header included before any default is applied. |
| `CHIPS_ASSERT(expr)` | `assert(expr)` | Assertion used for API contract checks. Redirect it to your own fault handler, or define it as `((void)0)` to disable checks. When overridden, `<assert.h>` is not included. Functions that report bad arguments in their result (`chips_encode`, `chips_decode`, `chips_master_submit`, `chips_slave_respond`, ...) check them regardless; every other precondition is checked only by this macro. |
| `CHIPS_CRC16_KERMIT_FOLD` | `1u` | Bytes processed per CRC loop iteration, 1..16. Each step costs a 512-byte lookup table (8 KB at 16), for more throughput. |
| `CHIPS_NO_LIBC_STRING` | undefined | Disable the use of glibc string functions |
| `CHIPS_RX_CHUNK` | `256u` | Maximum bytes the master or slave reads from its port per step. Sets the receive throughput ceiling (bytes per tick) and the size of each instance's read buffer. |

Each option can also be passed directly on the command line (e.g. `-DCHIPS_CRC16_KERMIT_FOLD=4u`) instead of through a config file.

## Integration

CHIPS is a request/response protocol between one master and one slave over a byte stream. Both sides are non-blocking state machines that you drive by calling a `step` function from your main loop or a periodic task. Complete, runnable examples are in `demos/`. The wire protocol is specified in [docs/PROTOCOL.md](docs/PROTOCOL.md).

### 1. Provide a port

A `chips_port_t` binds a state machine to your transport and a clock:

| Function | Contract |
|---|---|
| `write` | Accept up to `len` bytes and report how many in `*written`. Short writes are fine; the rest is retried on the next step. |
| `read` | Copy up to `cap` pending bytes and report how many in `*nread`. No data is `CHIPS_IO_OK` with `*nread = 0`, not an error. |
| `now` | Monotonic time as a `chips_time_t`: seconds and nanoseconds, like `struct timespec` (`tv_nsec` below 10^9). Wrapping of `tv_sec` at 2^32 is handled. |

All three must return immediately, and `ctx` is passed to each of them unchanged. They are only called from inside `step`, so they need no locking against CHIPS itself. Return `CHIPS_IO_ERROR` only for real driver faults.

### 2. Choose the timing

Both sides take a `chips_timing_t` at init, and both must use the same values: `resend_period` and `max_resends` are `RESEND_PERIOD` and `MAX_RESENDS` of [PROTOCOL.md §3](docs/PROTOCOL.md#3-timing-parameters), which defines what they mean and how to choose them. `chips_timing_valid()` checks a configuration; both init functions assert it.

All durations are `chips_time_t` values, like the clock readings; `chips_time_sub()` and `chips_time_cmp()` subtract and compare them.

On a UART, `chips_uart_resend_period()` implements the formula of §3.1. Pass it the latency that is not time on the wire: slave handling time, step periods and driver delays.

```c
const chips_uart_params_t uart = {.baud = 115200, .data_bits = 8, .parity = false, .stop_bits = 1};
const chips_timing_t timing = {
    .resend_period = chips_uart_resend_period(&uart, (chips_time_t){.tv_nsec = 10000000}), /* 367.29 ms */
    .max_resends = 2,
};
```

### 3. Master

```c
static chips_master_t master;
static uint8_t rsp[CHIPS_MAX_PAYLOAD];

chips_master_init(&master, &port, &timing);

chips_master_submit(&master, CMD_GET_STATUS, NULL, 0, rsp);

for (;;) {
    if (chips_master_step(&master) & CHIPS_EV_DONE) {
        const chips_master_result_t* r = chips_master_result(&master);
        if (r->outcome == CHIPS_OUTCOME_OK) {
            /* use r->payload, r->payload_len */
        }
        break;
    }
}
```

- One request is in flight at a time; `submit` returns `CHIPS_SUBMIT_BUSY` otherwise.
- The request is resent until a matching response arrives or the deadline passes ([PROTOCOL.md §4.2](docs/PROTOCOL.md#42-transaction)).
- The response buffer is lent to the master until the transaction ends (`CHIPS_EV_DONE`, `chips_master_cancel()`, or re-init). Don't touch it in the meantime.

### 4. Slave

```c
static chips_slave_t slave;

chips_slave_init(&slave, &port, &timing);

for (;;) {
    if (chips_slave_step(&slave) & CHIPS_SLAVE_EV_REQUEST) {
        const chips_slave_request_t* req = chips_slave_request(&slave);
        /* handle req->cmd, req->payload, req->payload_len */
        chips_slave_respond(&slave, reply, reply_len);
    }
}
```

- You may answer later: the request (and its payload) stays valid until `chips_slave_respond()` or `chips_slave_abandon()`. New requests are ignored while one is pending.
- The last response is cached. A resent duplicate is answered from the cache without reaching the application, so handlers run once per request.
- Call `chips_slave_invalidate_cache()` after anything that makes the cached answer wrong (mode change, reset).

### Notes

- **Memory.** Instances are a few kilobytes (the slave holds two receive payload buffers) and contain no pointers to heap memory. Allocate them statically rather than on the stack.
- **Threading.** All calls on one instance must come from the same context. Instances are independent and share only constant data.
- **Step rate.** Each step reads at most `CHIPS_RX_CHUNK` bytes, so step at least `line_rate / CHIPS_RX_CHUNK` times per second to keep up with the link.
- **Events.** `step` returns a bitmask of `CHIPS_EV_*` / `CHIPS_SLAVE_EV_*` flags. Beyond completion and new requests, they are diagnostic (errors, ignored frames) and useful for counters and logging.
