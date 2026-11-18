#include "clock.h"
#include "apic.h"
#include "hal/cshim.h"
#include "printk.h"

#define PIT_HZ 1193182ULL

/* Snapshot taken at clock_init() time. */
static uint64_t boot_tick;           /* apic_get_ticks() value at init        */
static uint64_t boot_base_local_ns;  /* local wall ns at mono = 0 (UTC + tz)  */
static int64_t  boot_tz_offset;      /* seconds east of UTC                   */

/* Time: free-running TSC, not counted IRQs.  A tick is lost whenever IRQs
   stay off for more than one APIC period (spin_lock_irq, PIO disk reads,
   fb_flush, ...), so IRQ counting makes the clock run slow under load. */
static uint64_t g_tsc0;              /* TSC at clock_init()                   */
static uint64_t g_tsc_mult;          /* ns per TSC cycle * 2^32; 0 = none     */

/* 128-bit multiply keeps `cycles * g_tsc_mult` exact; GCC lowers it to a
   single `mul` (or a shift/add pair), so no libgcc helper is needed. */
__extension__ typedef unsigned __int128 u128_t;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* One PIT ch.2 one-shot of `count` PIT ticks; returns TSC cycles elapsed. */
static uint64_t pit_window_tsc(uint16_t count)
{
    uint8_t ctl = hal_port_in8(0x61);
    hal_port_out8(0x61, (uint8_t)(ctl & 0xFC));          /* gate low, speaker off */
    hal_port_out8(0x43, 0xB0);                           /* ch2, lo/hi, mode 0 */
    hal_port_out8(0x42, (uint8_t)(count & 0xFF));
    hal_port_out8(0x42, (uint8_t)(count >> 8));
    uint64_t t0 = rdtsc();
    hal_port_out8(0x61, (uint8_t)((ctl & 0xFC) | 1));    /* gate high: counting starts */
    while (!(hal_port_in8(0x61) & 0x20))
        hal_cpu_pause();
    uint64_t t1 = rdtsc();
    hal_port_out8(0x61, ctl);
    return t1 - t0;
}

/* Returns ns-per-cycle scaled by 2^32, or 0 if the result looks insane. */
static uint64_t tsc_calibrate(void)
{
    const uint16_t count = 59659;                        /* ~50 ms */
    uint64_t best = ~0ULL;
    unsigned long fl = hal_cpu_save_irq();               /* no ISR noise in the window */
    for (int i = 0; i < 4; i++) {
        uint64_t d = pit_window_tsc(count);
        if (d < best) best = d;                          /* least-delayed sample */
    }
    hal_cpu_restore_irq(fl);
    if (best == 0)
        return 0;

    uint64_t khz = best * PIT_HZ / ((uint64_t)count * 1000);
    printk("Clock: TSC calibrated to %lu kHz\n", (unsigned long)khz);
    if (khz < 100000 || khz > 10000000)
        return 0;

    /* ns per cycle * 2^32 = (1e9 * 2^32 / f_tsc), with f_tsc = best * PIT_HZ /
       count.  Computing it from the raw counts (instead of from `khz`) avoids
       quantising the frequency to 1 kHz, which would be worth ~280 ppm.
       All intermediates stay below 2^64: ~4.3e18 / PIT_HZ is ~3.6e12, times
       count is ~2.1e17. */
    return ((1000000000ULL << 32) / PIT_HZ) * count / best;
}

void clock_init(uint64_t boot_unix, int64_t tz_offset_sec)
{
    uint64_t mult = tsc_calibrate();

    unsigned long fl = hal_cpu_save_irq();               /* sched_tick() reads this from the ISR */
    boot_tz_offset     = tz_offset_sec;
    boot_tick          = apic_get_ticks();
    boot_base_local_ns = (uint64_t)((int64_t)boot_unix + tz_offset_sec) * 1000000000ULL;
    g_tsc0 = rdtsc();
    __asm__ volatile("" ::: "memory");
    g_tsc_mult = mult;                                   /* publish last */
    hal_cpu_restore_irq(fl);
}

/* Nanoseconds elapsed since clock_init(). */
uint64_t clock_mono_ns(void)
{
    if (g_tsc_mult)
        return (uint64_t)(((u128_t)(rdtsc() - g_tsc0) * g_tsc_mult) >> 32);
    return apic_ticks_to_ns(apic_get_ticks() - boot_tick);   /* pre-init fallback */
}

uint64_t clock_wall_sec(void)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    return wall_ns / 1000000000ULL;
}

void clock_wall_ns(uint64_t *sec_out, uint64_t *nsec_out)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    if (sec_out)  *sec_out  = wall_ns / 1000000000ULL;
    if (nsec_out) *nsec_out = wall_ns % 1000000000ULL;
}

int64_t clock_get_tz_offset(void)
{
    return boot_tz_offset;
}

uint64_t clock_wall_utc_sec(void)
{
    uint64_t wall_ns = boot_base_local_ns + clock_mono_ns();
    int64_t wall_sec = (int64_t)(wall_ns / 1000000000ULL);
    return (uint64_t)(wall_sec - boot_tz_offset);
}

void clock_set_wall_sec(uint64_t new_sec)
{
    uint64_t mono = clock_mono_ns();
    boot_base_local_ns = new_sec * 1000000000ULL - mono;
    printk("Clock: corrected wall -> %lu (tz %ld)\n",
           (unsigned long)new_sec, (long)boot_tz_offset);
}

void clock_set_utc_sec(uint64_t new_utc_sec)
{
    uint64_t new_wall = (uint64_t)((int64_t)new_utc_sec + boot_tz_offset);
    clock_set_wall_sec(new_wall);
}

int64_t clock_drift_from_rtc(uint64_t rtc_utc_sec)
{
    uint64_t utc = clock_wall_utc_sec();
    return (int64_t)utc - (int64_t)rtc_utc_sec;
}

int clock_resync_from_rtc(uint64_t rtc_utc_sec, uint64_t threshold_sec)
{
    if (rtc_utc_sec == 0)
        return -1;
    int64_t drift = clock_drift_from_rtc(rtc_utc_sec);
    int64_t abs_drift = drift < 0 ? -drift : drift;
    if ((uint64_t)abs_drift >= threshold_sec) {
        clock_set_utc_sec(rtc_utc_sec);
        printk("Clock: drift %ld s >= %lu, resynced to RTC %lu\n",
               (long)drift, (unsigned long)threshold_sec,
               (unsigned long)rtc_utc_sec);
        return 1;
    }
    return 0;
}
