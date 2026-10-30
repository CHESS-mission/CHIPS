/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 EPFL Spacecraft Team
 */

/*
 * Scenario (requests from a pretend master):
 *   seq 1  PING             -> answered "PONG"
 *   seq 2  ECHO "hello"     -> answered "hello"
 *   seq 2  ECHO "hello"     -> duplicate, replayed from the cache
 *   seq 3  cmd 0x55         -> unknown, abandoned (no reply)
 *   seq 4  ECHO "oops"      -> corrupted on the wire, rejected by CRC
 */

#include <stdio.h>
#include <string.h>

#include "chips/core.h"
#include "chips/port.h"
#include "chips/slave.h"

#define RX_PER_STEP 1u
#define TX_PER_STEP 16u

typedef struct {
    uint8_t rx[512];
    size_t rx_len;
    size_t rx_pos;
    uint32_t t_us;
} sim_t;

static chips_io_status_t sim_read(void* ctx, uint8_t* buf, size_t cap, size_t* nread) {
    sim_t* sim = ctx;
    size_t n = sim->rx_len - sim->rx_pos;
    if (n > cap)
        n = cap;
    if (n > RX_PER_STEP)
        n = RX_PER_STEP;

    memcpy(buf, sim->rx + sim->rx_pos, n);
    sim->rx_pos += n;
    *nread = n;
    return CHIPS_IO_OK;
}

static chips_io_status_t sim_write(void* ctx, const uint8_t* data, size_t len, size_t* written) {
    (void)ctx;
    size_t n = len < TX_PER_STEP ? len : TX_PER_STEP;

    printf("    tx:");
    for (size_t i = 0; i < n; i++)
        printf(" %02X", data[i]);
    printf("\n");

    *written = n;
    return CHIPS_IO_OK;
}

static uint32_t sim_now_us(void* ctx) {
    return ((sim_t*)ctx)->t_us;
}

static size_t script(sim_t* sim, uint8_t seq, uint8_t cmd, const char* text) {
    static uint8_t wire[CHIPS_WIRE_MAX];
    size_t n = chips_encode(seq, CHIPS_DIR_REQUEST, cmd, (const uint8_t*)text, strlen(text), wire);
    size_t at = sim->rx_len;
    memcpy(sim->rx + at, wire, n);
    sim->rx_len += n;
    return at;
}

enum { CMD_PING = 0x01, CMD_ECHO = 0x02 };

/* Totals derived from event bits and from what the application did. An event
 * bit fires at most once per step, so each count is "steps in which it
 * happened"; with RX_PER_STEP == 1 that equals the number of occurrences. */
typedef struct {
    unsigned long requests;
    unsigned long responses;
    unsigned long abandoned;
    unsigned long retransmits;
    unsigned long bad_frames;
    unsigned long tx_frames;
} tally_t;

static void handle_request(chips_slave_t* s, tally_t* t) {
    const chips_slave_request_t* req = chips_slave_request(s);
    printf("    app: seq=%u cmd=0x%02X len=%zu -> ", req->seq, req->cmd, req->payload_len);

    switch (req->cmd) {
        case CMD_PING:
            printf("PONG\n");
            if (chips_slave_respond(s, (const uint8_t*)"PONG", 4) == CHIPS_RESPOND_OK)
                t->responses++;
            break;
        case CMD_ECHO:
            printf("echo %.*s\n", (int)req->payload_len, (const char*)req->payload);
            /* req->payload points into the slave's receive buffer (no copy);
             * passing it straight back is allowed. */
            if (chips_slave_respond(s, req->payload, req->payload_len) == CHIPS_RESPOND_OK)
                t->responses++;
            break;
        default:
            printf("unknown, abandon\n");
            if (chips_slave_abandon(s) == CHIPS_RESPOND_OK)
                t->abandoned++;
            break;
    }
}

static void print_events(uint32_t ev) {
    static const char* names[] = {"REQUEST",  "TX_FRAME",     "RETRANSMIT", "TX_ERROR",
                                  "RX_ERROR", "RX_BAD_FRAME", "RX_IGNORED"};
    for (unsigned i = 0; i < 7; i++)
        if (ev & (1u << i))
            printf(" %s", names[i]);
    printf("\n");
}

int main(void) {
    static sim_t sim;
    static chips_slave_t slave;

    script(&sim, 1, CMD_PING, "");
    script(&sim, 2, CMD_ECHO, "hello");
    script(&sim, 2, CMD_ECHO, "hello");
    script(&sim, 3, 0x55, "");
    size_t bad = script(&sim, 4, CMD_ECHO, "oops");
    sim.rx[bad + 3] ^= 0x01;

    const chips_port_t port = {
        .write = sim_write,
        .read = sim_read,
        .now_us = sim_now_us,
        .ctx = &sim,
    };
    chips_slave_init(&slave, &port);

    tally_t t = {0};

    for (unsigned step = 1; step <= 200; step++) {
        sim.t_us += 1000u;

        uint32_t ev = chips_slave_step(&slave);
        if (ev != CHIPS_SLAVE_EV_NONE) {
            printf("step %2u:", step);
            print_events(ev);
        }
        t.retransmits += (ev & CHIPS_SLAVE_EV_RETRANSMIT) != 0;
        t.bad_frames += (ev & CHIPS_SLAVE_EV_RX_BAD_FRAME) != 0;
        t.tx_frames += (ev & CHIPS_SLAVE_EV_TX_FRAME) != 0;
        if (ev & CHIPS_SLAVE_EV_REQUEST) {
            t.requests++;
            handle_request(&slave, &t);
        }
    }

    printf("\nrequests=%lu responses=%lu abandoned=%lu duplicates=%lu bad_frames=%lu tx_frames=%lu\n", t.requests,
           t.responses, t.abandoned, t.retransmits, t.bad_frames, t.tx_frames);
    return 0;
}
