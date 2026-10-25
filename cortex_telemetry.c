#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

int main(void) {
    uint64_t ctr_el0;
    uint64_t dczid_el0;
    uint64_t cntfrq_el0;
    uint64_t cntpct_start, cntpct_end;

    /* 1. Read Cache Type Register (CTR_EL0) */
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));

    /* 2. Read Data Cache Zero ID Register (DCZID_EL0) */
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid_el0));

    /* 3. Read Hardware Timer Frequency & Physical Counter */
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq_el0));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cntpct_start));
    
    usleep(1000); /* Wait ~1 millisecond */
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cntpct_end));

    /* Decode CTR_EL0 cache lines:
     * Bits [3:0]   = L1 instruction cache line size (log2 of 4-byte words)
     * Bits [19:16] = L1 data cache line size (log2 of 4-byte words)
     */
    uint32_t icache_line_bytes = 4 << (ctr_el0 & 0xF);
    uint32_t dcache_line_bytes = 4 << ((ctr_el0 >> 16) & 0xF);

    /* Decode DCZID_EL0 block size:
     * Bits [3:0]   = Log2 of 4-byte words cleared by the 'dc zva' instruction
     * Bit  [4]     = 0 if DC ZVA is permitted at EL0, 1 if prohibited
     */
    uint32_t dcz_bytes = 4 << (dczid_el0 & 0xF);
    int dcz_prohibited = (dczid_el0 >> 4) & 1;

    printf("\n==================================================\n");
    printf("   ARM Cortex-A53 Telemetry Engine (Module 1)     \n");
    printf("==================================================\n");
    printf("Hardware Timer Frequency (CNTFRQ_EL0) : %lu MHz\n", cntfrq_el0 / 1000000);
    printf("Timer Ticks elapsed in ~1ms           : %lu ticks\n", cntpct_end - cntpct_start);
    printf("--------------------------------------------------\n");
    printf("L1 Instruction Cache Line Size        : %u bytes\n", icache_line_bytes);
    printf("L1 Data Cache Line Size               : %u bytes\n", dcache_line_bytes);
    printf("DC ZVA Hardware Zeroing Block Size    : %u bytes (%s)\n", 
           dcz_bytes, dcz_prohibited ? "Blocked" : "Enabled");
    printf("==================================================\n\n");

    return 0;
}
