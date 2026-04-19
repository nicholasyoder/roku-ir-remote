#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <sched.h>
#include <stdint.h>

#define PORT "/dev/ttyS0"

// NEC timing (nanoseconds)
#define HDR_MARK   9000000L
#define HDR_SPACE  4500000L
#define BIT_MARK    562500L
#define ONE_SPACE  1687500L
#define ZER_SPACE   562500L
#define TRAIL_MARK  562500L

// Target carrier frequency
#define TARGET_HZ        38000
#define TARGET_PERIOD_NS (1000000000L / TARGET_HZ)   // 26315 ns
#define TARGET_HALF_NS   (TARGET_PERIOD_NS / 2)      // 13157 ns

// Calibration settings
#define CAL_CYCLES       2000
#define CAL_TOLERANCE_HZ 200

// Set by calibrate_carrier()
static long half_period_ns = TARGET_HALF_NS;

int fd;

typedef struct {
    const char *name;
    uint8_t addr_lo;   // sent first on wire
    uint8_t addr_hi;   // sent second on wire
    uint8_t cmd_lo;    // sent third on wire  (= ~cmd_hi)
    uint8_t cmd_hi;    // sent fourth on wire (= ~cmd_lo)
} RokuCmd;

// Address: 0xC2EA. Wire order: addr_lo=0xEA first, addr_hi=0xC2 second.
//
// COMMAND BYTE ORDER NOTE:
// The ESPHome RC108 capture reports "command=0x6699" for Up.
// ESPHome reconstructs command as (wire_byte3 << 8) | wire_byte2.
// So command=0x6699 means wire_byte2=0x99, wire_byte3=0x66.
// Therefore: cmd_lo (wire byte 2, sent first) = 0x99
//            cmd_hi (wire byte 3, sent last)  = 0x66
// i.e. cmd_lo is the LOW byte of the reported command value,
//      cmd_hi is the HIGH byte.
//
// Command table derived from RC108 hardware capture (newscrewdriver.com):
//   reported       cmd_lo  cmd_hi
//   command=0x6699  0x99    0x66   up
//   command=0x4CB3  0xB3    0x4C   down
//   command=0x619E  0x9E    0x61   left
//   command=0x52AD  0xAD    0x52   right
//   command=0x55AA  0xAA    0x55   select
//   command=0x19E6  0xE6    0x19   back
//   command=0x7C83  0x83    0x7C   home
//   command=0x07F8  0xF8    0x07   replay
//   command=0x1EE1  0xE1    0x1E   star
//   command=0x4BB4  0xB4    0x4B   rewind
//   command=0x33CC  0xCC    0x33   play
//   command=0x2AD5  0xD5    0x2A   forward
//   command=0x34CB  0xCB    0x34   netflix
RokuCmd ROKU_CMDS[] = {
    // Navigation (verified against RC108 hardware capture)
    {"up",          0xEA, 0xC2, 0x99, 0x66},
    {"down",        0xEA, 0xC2, 0xB3, 0x4C},
    {"left",        0xEA, 0xC2, 0x9E, 0x61},
    {"right",       0xEA, 0xC2, 0xAD, 0x52},
    {"select",      0xEA, 0xC2, 0xAA, 0x55},
    {"back",        0xEA, 0xC2, 0xE6, 0x19},
    {"home",        0xEA, 0xC2, 0x83, 0x7C},

    // Playback (verified against RC108 hardware capture)
    {"replay",      0xEA, 0xC2, 0xF8, 0x07},
    {"star",        0xEA, 0xC2, 0xE1, 0x1E},
    {"rewind",      0xEA, 0xC2, 0xB4, 0x4B},
    {"play",        0xEA, 0xC2, 0xCC, 0x33},
    {"forward",     0xEA, 0xC2, 0xD5, 0x2A},
    {"netflix",     0xEA, 0xC2, 0xCB, 0x34},

    // Volume [UNVERIFIED] - apply same byte swap to previously guessed values
    {"vol_up",      0xEA, 0xC2, 0xF0, 0x0F},
    {"vol_dn",      0xEA, 0xC2, 0xEF, 0x10},
    {"mute",        0xEA, 0xC2, 0xFB, 0x04},

    // Streaming apps [UNVERIFIED]
    {"hulu",        0xEA, 0xC2, 0xCD, 0x32},
    {"sling",       0xEA, 0xC2, 0xA7, 0x58},
    {"vudu",        0xEA, 0xC2, 0x88, 0x77},

    {NULL, 0, 0, 0, 0}
};

// ---- Timing primitives ------------------------------------------------------

static inline long now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void carrier_on() {
    int bit = TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &bit);
}

static void carrier_off() {
    int bit = TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &bit);
}

void burst(long duration_ns) {
    long end = now_ns() + duration_ns;
    while (now_ns() < end) {
        carrier_on();
        long half_end = now_ns() + half_period_ns;
        while (now_ns() < half_end);
        carrier_off();
        half_end = now_ns() + half_period_ns;
        while (now_ns() < half_end);
    }
}

// carrier_off() BEFORE capturing deadline so ioctl latency doesn't eat into space
void space(long duration_ns) {
    carrier_off();
    long end = now_ns() + duration_ns;
    while (now_ns() < end);
}

// ---- Calibration ------------------------------------------------------------

static double measure_freq(long half_ns, int cycles) {
    long start = now_ns();
    for (int i = 0; i < cycles; i++) {
        carrier_on();
        long half_end = now_ns() + half_ns;
        while (now_ns() < half_end);
        carrier_off();
        half_end = now_ns() + half_ns;
        while (now_ns() < half_end);
    }
    long elapsed = now_ns() - start;
    double period_us = (double)elapsed / cycles / 1000.0;
    return 1000000.0 / period_us;
}

void calibrate_carrier() {
    printf("Auto-calibrating carrier to %d Hz...\n", TARGET_HZ);

    long lo = 1L, hi = TARGET_HALF_NS * 3, best = TARGET_HALF_NS;
    double best_err = 1e9;

    for (int iter = 0; iter < 20; iter++) {
        long mid = (lo + hi) / 2;
        double freq = measure_freq(mid, CAL_CYCLES);
        double err = freq - TARGET_HZ;
        printf("  half=%ld ns -> %.0f Hz (err %+.0f)\n", mid, freq, err);
        double abs_err = err < 0 ? -err : err;
        if (abs_err < best_err) { best_err = abs_err; best = mid; }
        if (err > 0) lo = mid; else hi = mid;
        if (hi - lo <= 100L) break;
    }
    half_period_ns = best;

    // Fine-tune: walk +-500 ns in 50 ns steps
    long fine_best = half_period_ns;
    double fine_err = 1e9;
    for (long delta = -500L; delta <= 500L; delta += 50L) {
        long candidate = half_period_ns + delta;
        if (candidate < 1) continue;
        double freq = measure_freq(candidate, CAL_CYCLES);
        double abs_err = freq - TARGET_HZ; if (abs_err < 0) abs_err = -abs_err;
        if (abs_err < fine_err) { fine_err = abs_err; fine_best = candidate; }
    }
    half_period_ns = fine_best;

    double final_freq = measure_freq(half_period_ns, CAL_CYCLES * 2);
    double abs_err = final_freq - TARGET_HZ; if (abs_err < 0) abs_err = -abs_err;
    printf("\nCalibration result:\n");
    printf("  half_period_ns = %ld ns\n", half_period_ns);
    printf("  Target:  %d Hz\n", TARGET_HZ);
    printf("  Actual:  %.0f Hz  (error %+.0f Hz)\n", final_freq, final_freq - TARGET_HZ);
    if (abs_err <= CAL_TOLERANCE_HZ)
        printf("  Status:  OK (within +/-%d Hz)\n\n", CAL_TOLERANCE_HZ);
    else
        printf("  WARNING: outside +/-%d Hz. IR may still work.\n\n", CAL_TOLERANCE_HZ);
}

// ---- Frame transmission -----------------------------------------------------

// Send 4 raw bytes as a NEC frame, LSB first per byte.
void send_raw_frame(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t bytes[4] = { b0, b1, b2, b3 };
    burst(HDR_MARK);
    space(HDR_SPACE);
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < 8; i++) {
            burst(BIT_MARK);
            space(((bytes[b] >> i) & 1) ? ONE_SPACE : ZER_SPACE);
        }
    burst(TRAIL_MARK);
    carrier_off();
}

// ---- Diagnostics ------------------------------------------------------------

// Print a frame's bits as they appear on the wire (LSB first per byte),
// plus what an ESPHome/LIRC NEC decoder would reconstruct from them.
// ESPHome decodes: address = (b1<<8)|b0,  command = (b3<<8)|b2
void dump_frame(const char *label, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    uint8_t bytes[4] = { b0, b1, b2, b3 };
    const char *names[4] = { "addr_lo", "addr_hi", "cmd_lo ", "cmd_hi " };
    printf("  [%s] wire frame (LSB-first per byte):\n", label);
    for (int b = 0; b < 4; b++) {
        printf("    %s 0x%02X = ", names[b], bytes[b]);
        for (int i = 0; i < 8; i++) printf("%d", (bytes[b] >> i) & 1);
        printf("  (complement=0x%02X)\n", (~bytes[b]) & 0xFF);
    }
    uint16_t addr16 = ((uint16_t)b1 << 8) | b0;
    uint16_t cmd16  = ((uint16_t)b3 << 8) | b2;
    printf("    => NEC decoder sees: address=0x%04X  command=0x%04X\n", addr16, cmd16);
    if ((b2 ^ b3) == 0xFF)
        printf("    => cmd bytes ARE complements (standard NEC)\n");
    else
        printf("    => cmd bytes are NOT complements (NECext)\n");
    printf("\n");
}

static uint8_t bitrev(uint8_t x) {
    x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4);
    x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
    x = ((x & 0xAA) >> 1) | ((x & 0x55) << 1);
    return x;
}

// Send 8 byte-order/bit-order variants of a command, 2 seconds apart.
// Watch the Roku and note which V# triggers the correct button.
void send_variants(uint8_t addr_lo, uint8_t addr_hi, uint8_t cmd_lo, uint8_t cmd_hi) {
    uint8_t addr_c = (~addr_lo) & 0xFF;
    uint8_t cmd_c  = (~cmd_lo)  & 0xFF;

    struct { const char *desc; uint8_t b[4]; } v[] = {
        {"V1 [addr_lo addr_hi cmd_lo cmd_hi]                    <-- CURRENT",
            {addr_lo, addr_hi, cmd_lo, cmd_hi}},
        {"V2 [addr_lo ~addr_lo cmd_lo ~cmd_lo]                   plain NEC",
            {addr_lo, addr_c, cmd_lo, cmd_c}},
        {"V3 [addr_hi addr_lo cmd_lo cmd_hi]                     addr bytes swapped",
            {addr_hi, addr_lo, cmd_lo, cmd_hi}},
        {"V4 [addr_lo addr_hi cmd_hi cmd_lo]                     cmd bytes swapped",
            {addr_lo, addr_hi, cmd_hi, cmd_lo}},
        {"V5 [addr_hi addr_lo cmd_hi cmd_lo]                     both swapped",
            {addr_hi, addr_lo, cmd_hi, cmd_lo}},
        {"V6 [addr_hi ~addr_hi cmd_lo ~cmd_lo]                   plain NEC, hi as addr",
            {addr_hi, (~addr_hi)&0xFF, cmd_lo, cmd_c}},
        {"V7 [addr_lo addr_hi rev(cmd_lo) rev(cmd_hi)]           cmd bits reversed",
            {addr_lo, addr_hi, bitrev(cmd_lo), bitrev(cmd_hi)}},
        {"V8 [rev(addr_lo) rev(addr_hi) rev(cmd_lo) rev(cmd_hi)] all bits reversed",
            {bitrev(addr_lo), bitrev(addr_hi), bitrev(cmd_lo), bitrev(cmd_hi)}},
    };

    int n = (int)(sizeof(v) / sizeof(v[0]));
    printf("\n=== VARIANT PROBE ===\n");
    printf("Sending %d variants with 2s gap. Watch Roku -- note which V# triggers it.\n\n", n);
    for (int i = 0; i < n; i++) {
        printf("--- %s\n", v[i].desc);
        dump_frame("frame", v[i].b[0], v[i].b[1], v[i].b[2], v[i].b[3]);
        send_raw_frame(v[i].b[0], v[i].b[1], v[i].b[2], v[i].b[3]);
        fflush(stdout);
        sleep(2);
    }
    printf("=== VARIANT PROBE DONE ===\n\n");
}

void send_necext(uint8_t addr_lo, uint8_t addr_hi, uint8_t cmd_lo, uint8_t cmd_hi) {
    // Enable dump_frame for debugging.
    // dump_frame("TX", addr_lo, addr_hi, cmd_lo, cmd_hi);
    send_raw_frame(addr_lo, addr_hi, cmd_lo, cmd_hi);
}

// ---- UI ---------------------------------------------------------------------

const RokuCmd *get_command(const char *name) {
    for (int i = 0; ROKU_CMDS[i].name != NULL; i++)
        if (strcmp(ROKU_CMDS[i].name, name) == 0)
            return &ROKU_CMDS[i];
    return NULL;
}

void print_help() {
    printf("Buttons:\n");
    for (int i = 0; ROKU_CMDS[i].name != NULL; i++) {
        printf("  %-12s", ROKU_CMDS[i].name);
        if ((i + 1) % 4 == 0) printf("\n");
    }
    printf("\nDiagnostic commands:\n");
    printf("  probe <btn>   Send 8 byte-order variants (2s apart).\n");
    printf("                Try: probe up -- note which V# moves cursor up.\n");
    printf("  raw <hex32>   Send a raw 32-bit frame.  e.g: raw EAC29966\n");
    printf("                Wire byte order: first pair=addr_lo, last pair=cmd_hi.\n");
    printf("  dump <btn>    Print wire frame without transmitting.\n");
    printf("  recal         Re-run carrier calibration.\n");
    printf("  ?             This help.\n");
    printf("  q             Quit.\n\n");
}

int main() {
    fd = open(PORT, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("Failed to open port"); return 1; }

    struct sched_param sp = { .sched_priority = 99 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        perror("Warning: could not set realtime priority (try sudo)");
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        perror("Warning: mlockall failed");

    carrier_off();
    calibrate_carrier();
    print_help();
    printf("Roku IR controller ready.\n\n");

    char input[128];
    while (1) {
        printf("Button: ");
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "q") == 0) break;
        if (strcmp(input, "?") == 0) { print_help(); continue; }
        if (strcmp(input, "recal") == 0) { calibrate_carrier(); continue; }

        if (strncmp(input, "probe ", 6) == 0) {
            const RokuCmd *cmd = get_command(input + 6);
            if (!cmd) printf("Unknown button: %s\n", input + 6);
            else send_variants(cmd->addr_lo, cmd->addr_hi, cmd->cmd_lo, cmd->cmd_hi);
            continue;
        }

        if (strncmp(input, "raw ", 4) == 0) {
            unsigned int raw = 0;
            if (sscanf(input + 4, "%8x", &raw) == 1) {
                uint8_t b0 = (raw >> 24) & 0xFF;
                uint8_t b1 = (raw >> 16) & 0xFF;
                uint8_t b2 = (raw >>  8) & 0xFF;
                uint8_t b3 = (raw >>  0) & 0xFF;
                dump_frame("raw TX", b0, b1, b2, b3);
                send_raw_frame(b0, b1, b2, b3);
            } else {
                printf("  Usage: raw <8 hex digits>  e.g. raw EAC29966\n");
            }
            continue;
        }

        if (strncmp(input, "dump ", 5) == 0) {
            const RokuCmd *cmd = get_command(input + 5);
            if (!cmd) printf("Unknown button: %s\n", input + 5);
            else dump_frame(cmd->name, cmd->addr_lo, cmd->addr_hi,
                            cmd->cmd_lo, cmd->cmd_hi);
            continue;
        }

        const RokuCmd *cmd = get_command(input);
        if (!cmd) { printf("Unknown: '%s'  (? for help)\n", input); continue; }
        send_necext(cmd->addr_lo, cmd->addr_hi, cmd->cmd_lo, cmd->cmd_hi);
        printf("  Sent: %s\n", input);
    }

    carrier_off();
    close(fd);
    return 0;
}
