# CHIPS

## Building

CHIPS has no build system requirement and no dependencies beyond a C99 compiler. To use it in your project:

1. Clone the repository.
2. Add `include/` to your include path.
3. Compile the sources you need alongside your own:
   - `src/core.c`: always (framing, byte stuffing, CRC).
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
#define CHIPS_NO_LIBC_STRING
#define CHIPS_ASSERT(expr) my_assert_handler(expr)
#define CHIPS_CRC16_KERMIT_FOLD (4u)
#define CHIPS_MASTER_RX_CHUNK (64u)
#define CHIPS_SLAVE_RX_CHUNK (64u)
```

Define the macro as a quoted header name, and make sure the file's directory is on the include path:

```sh
cc -std=c99 -I include -I config -DCHIPS_CONFIG_FILE='"chips_config.h"' \
   -o app main.c src/core.c src/master.c
```

Use the same configuration for every file that includes a CHIPS header, including the library sources. The `*_RX_CHUNK` options change the size of `chips_master_t` and `chips_slave_t`, so a mismatch between translation units silently breaks the instance layout.

| Option | Default | Description |
|---|---|---|
| `CHIPS_CONFIG_FILE` | undefined | Quoted name of a header included before any default is applied. |
| `CHIPS_NO_LIBC_STRING` | undefined | Define to stop `<string.h>` from being included, e.g. for freestanding targets without a libc. |
| `CHIPS_ASSERT(expr)` | `assert(expr)` | Assertion used for API contract checks. Redirect it to your own fault handler, or define it as `((void)0)` to disable checks. When overridden, `<assert.h>` is not included. |
| `CHIPS_CRC16_KERMIT_FOLD` | `16u` | Bytes processed per CRC loop iteration. Higher values give more throughput, lower values give smaller code. |
| `CHIPS_MASTER_RX_CHUNK` | `256u` | Maximum bytes the master reads from its port per step. Sets the receive throughput ceiling (bytes per tick) and the size of the master's read buffer. |
| `CHIPS_SLAVE_RX_CHUNK` | `256u` | Same as above, for the slave. |

Each option can also be passed directly on the command line (e.g. `-DCHIPS_CRC16_KERMIT_FOLD=4u`) instead of through a config file.

## Integration

CHIPS is a request/response protocol between one master and one slave over a byte stream. Both sides are non-blocking state machines that you drive by calling a `step` function from your main loop or a periodic task. Complete, runnable examples are in `demos/`.

### 1. Initialize the CRC table

Call `chips_crc16_kermit_init()` once at startup, before anything encodes or decodes a frame.

### 2. Provide a port

A `chips_port_t` binds a state machine to your transport (UART, USB CDC, socket…) and a clock:

| Function | Contract |
|---|---|
| `write` | Accept up to `len` bytes and report how many in `*written`. Short writes are fine; the rest is retried on the next step. |
| `read` | Copy up to `cap` pending bytes and report how many in `*nread`. No data is `CHIPS_IO_OK` with `*nread = 0`, not an error. |
| `now_us` | Monotonic microseconds. Wrapping at 2^32 is handled. |

All three must return immediately, and `ctx` is passed to each of them unchanged. They are only called from inside `step`, so they need no locking against CHIPS itself. Return `CHIPS_IO_ERROR` only for real driver faults.

### 3. Master

```c
static chips_master_t master;
static uint8_t rsp[CHIPS_MAX_PAYLOAD];

chips_master_init(&master, &port, &(chips_master_config_t){ .resend_period_us = 20000 });

chips_master_submit(&master, CMD_GET_STATUS, NULL, 0, 200000 /* timeout, µs */, rsp);

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
- The request is resent every `resend_period_us` until a matching response arrives or the timeout expires.
- The response buffer is lent to the master until the transaction ends (`CHIPS_EV_DONE`, `chips_master_cancel()`, or re-init). Don't touch it in the meantime.

### 4. Slave

```c
static chips_slave_t slave;

chips_slave_init(&slave, &port, &(chips_slave_config_t){ .cache_window_us = 500000 });

for (;;) {
    if (chips_slave_step(&slave) & CHIPS_SLAVE_EV_REQUEST) {
        const chips_slave_request_t* req = chips_slave_request(&slave);
        /* handle req->cmd, req->payload, req->payload_len */
        chips_slave_respond(&slave, reply, reply_len);
    }
}
```

- You may answer later: the request (and its payload) stays valid until `chips_slave_respond()` or `chips_slave_abandon()`. New requests are ignored while one is pending.
- The last response is cached. A resent duplicate is answered from the cache without reaching the application, so handlers run once per request. Set `cache_window_us` to at least the master's longest timeout.
- Call `chips_slave_invalidate_cache()` after anything that makes the cached answer wrong (mode change, reset).

### Notes

- **Memory.** Instances are a few kilobytes (the slave holds two receive payload buffers) and contain no pointers to heap memory. Allocate them statically rather than on the stack.
- **Threading.** All calls on one instance must come from the same context. Instances are independent, and the CRC table is read-only after init.
- **Step rate.** Each step reads at most `CHIPS_*_RX_CHUNK` bytes, so step at least `line_rate / RX_CHUNK` times per second to keep up with the link.
- **Events.** `step` returns a bitmask of `CHIPS_EV_*` / `CHIPS_SLAVE_EV_*` flags. Beyond completion and new requests, they are diagnostic (errors, ignored frames) and useful for counters and logging.
