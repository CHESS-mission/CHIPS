# CHIPS

## Building

CHIPS has no build system requirement and no dependencies beyond a C99 compiler. To use it in your project:

1. Clone the repository.
2. Add `include/` to your include path.
3. Compile `src/core.c` alongside your sources.

```c
#include <chips/core.h>
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

CHIPS can be tuned at compile time. Put your overrides in a header of your own and point `CHIPS_CONFIG_FILE` at it. `chips/core.h` includes that file before applying its defaults, so anything you define there takes precedence.

```c
/* chips_config.h */
#define CHIPS_NO_LIBC_STRING
#define CHIPS_ASSERT(expr) my_assert_handler(expr)
#define CHIPS_CRC16_KERMIT_FOLD (4u)
```

Define the macro as a quoted header name, and make sure the file's directory is on the include path:

```sh
cc -std=c99 -I include -I config -DCHIPS_CONFIG_FILE='"chips_config.h"' -o app main.c src/core.c
```

Use the same `CHIPS_CONFIG_FILE` for every file that includes `chips/core.h`, including `src/core.c`, so the library and your code agree.

| Option | Default | Description |
|---|---|---|
| `CHIPS_NO_LIBC_STRING` | undefined | Define to stop `<string.h>` from being included, e.g. for freestanding targets without a libc. |
| `CHIPS_ASSERT(expr)` | `assert(expr)` | Assertion used for API contract checks. Redirect it to your own fault handler, or define it as `((void)0)` to disable checks. When overridden, `<assert.h>` is not included. |
| `CHIPS_CRC16_KERMIT_FOLD` | `16u` | Bytes processed per CRC loop iteration. Higher values give more throughput, lower values give smaller code. |

Each option can also be passed directly on the command line (e.g. `-DCHIPS_CRC16_KERMIT_FOLD=4u`) instead of through a config file.
