/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/*
 * CHIPS over Linux file descriptors: the Magic 8-Ball satellite.
 *
 * A ground station (master) and a satellite (slave) talk over a
 * non-blocking socketpair. Ground asks yes/no questions; the satellite
 * consults its onboard oracle and answers.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "chips/core.h"
#include "chips/master.h"
#include "chips/port.h"
#include "chips/slave.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static chips_io_status_t fd_read(void* ctx, uint8_t* buf, size_t cap, size_t* nread) {
    ssize_t n = read(*(int*)ctx, buf, cap);
    *nread = 0;
    if (n < 0)
        return would_block() ? CHIPS_IO_OK : CHIPS_IO_ERROR;
    *nread = (size_t)n;
    return CHIPS_IO_OK;
}

static chips_io_status_t fd_write(void* ctx, const uint8_t* data, size_t len, size_t* written) {
    ssize_t n = write(*(int*)ctx, data, len);
    *written = 0;
    if (n < 0)
        return would_block() ? CHIPS_IO_OK : CHIPS_IO_ERROR;
    *written = (size_t)n;
    return CHIPS_IO_OK;
}

static chips_time_t mono_now(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (chips_time_t){.tv_sec = (uint32_t)ts.tv_sec, .tv_nsec = (uint32_t)ts.tv_nsec};
}

static double ms(chips_time_t t) {
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static chips_port_t fd_port(int* fd) {
    return (chips_port_t){.write = fd_write, .read = fd_read, .now = mono_now, .ctx = fd};
}

static void nap(void) {
    nanosleep(&(struct timespec){.tv_nsec = 100000}, NULL);
}

enum { CMD_ASK = 0x08 };

static const char* const questions[] = {
    "Will the launch be on time?", "Is Gerardo watching?",   "Should I fix that bug tonight?",
    "Will the CRC ever fail?",     "Is the CI trustworthy?",
};

static const char* const answers[] = {
    "Telemetry says yes.",
    "Negative, ground.",
    "All systems nominal: absolutely.",
    "Signal unclear. Ask again next orbit.",
    "The stars say maybe.",
    "Outlook: cosmic.",
    "Houston, that's a no.",
    "Affirmative. Proceed to launch.",
};

static void oracle(chips_slave_t* sat) {
    const chips_slave_request_t* rq = chips_slave_request(sat);
    unsigned sum = 0;
    for (size_t i = 0; i < rq->payload_len; ++i)
        sum += rq->payload[i];
    const char* a = answers[sum % COUNT(answers)];
    chips_slave_respond(sat, (const uint8_t*)a, strlen(a));
}

static atomic_bool sat_running = true;
static unsigned long sat_requests;

static void* sat_thread(void* arg) {
    chips_slave_t* sat = arg;
    while (atomic_load(&sat_running)) {
        if (chips_slave_step(sat) & CHIPS_SLAVE_EV_REQUEST) {
            sat_requests++;
            oracle(sat);
        }
        nap();
    }
    return NULL;
}

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }

    static chips_master_t ground;
    static chips_slave_t sat;
    static uint8_t ground_rx[CHIPS_MAX_PAYLOAD];

    const chips_uart_params_t uart = {.baud = 115200u, .data_bits = 8u, .parity = false, .stop_bits = 1u};
    const chips_time_t app_latency = {.tv_nsec = 5000000u};
    const chips_timing_t timing = {.resend_period = chips_uart_resend_period(&uart, app_latency), .max_resends = 3u};
    printf("resend period %.3f ms, up to %u resends, cache window %.3f ms\n\n", ms(timing.resend_period),
           (unsigned)timing.max_resends, ms(chips_timing_cache_window(&timing)));

    const chips_port_t ground_port = fd_port(&sv[0]);
    const chips_port_t sat_port = fd_port(&sv[1]);
    chips_master_init(&ground, &ground_port, &timing);
    chips_slave_init(&sat, &sat_port, &timing);

    pthread_t sat_tid;
    if (pthread_create(&sat_tid, NULL, sat_thread, &sat) != 0) {
        perror("pthread_create");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }

    unsigned long answered = 0, resends = 0;

    for (size_t q = 0; q < COUNT(questions); q++) {
        printf("ground > %s\n", questions[q]);
        chips_master_submit(&ground, CMD_ASK, (const uint8_t*)questions[q], strlen(questions[q]), ground_rx);

        unsigned long copies = 0;
        uint32_t ev;
        do {
            ev = chips_master_step(&ground);
            copies += (ev & CHIPS_EV_TX_FRAME) != 0;
            if (!(ev & CHIPS_EV_DONE))
                nap();
        } while (!(ev & CHIPS_EV_DONE));
        if (copies > 1) {
            resends += copies - 1;
        }

        const chips_master_result_t* r = chips_master_result(&ground);
        if (r->outcome == CHIPS_OUTCOME_OK) {
            answered++;
            printf("sat    > %.*s  (%.3f ms)\n\n", (int)r->payload_len, (const char*)r->payload, ms(r->latency));
        } else {
            printf("sat    > ... no answer (outcome %d)\n\n", (int)r->outcome);
        }
    }

    atomic_store(&sat_running, false);
    pthread_join(sat_tid, NULL);

    printf("%lu/%zu answered, %lu resends, %lu requests seen by the satellite\n", answered, COUNT(questions), resends,
           sat_requests);

    close(sv[0]);
    close(sv[1]);
    return 0;
}
