/*
 * flash_stub.c -- RAM-resident FPEC exerciser for CH32V307 (v3).
 *
 * The WCH-LinkE SDI flash-write channel fails
 * ("error writing to flash at address 0x00000000"), so this stub
 * performs erase/program through the on-chip FPEC registers while
 * the core runs from SRAM.
 *
 * Calling convention (set by the debugger):
 *   a0 = src   : SRAM address of the payload
 *   a1 = len   : byte count
 *   a2 = dst   : flash address to operate on
 *   a3 = mode  : 0 = page erase only
 *                1 = half-word program only
 *                2 = page erase then program
 *                3 = mass erase
 *
 * Result words (read back by the debugger):
 *   0x20000FF0 : 0x600D0000 | bytes handled (low 16 bits) on success
 *                0xBAD00000 | error code on failure
 *   0x20000FF4 : progress (bytes handled so far)
 *   0x20000FF8 : FLASH->WPR sampled before programming
 *   0x20000FFC : FLASH->OBR sampled before programming
 *   0x20000FE0 : FLASH->CTLR sampled after unlock
 *   0x20000FE4 : FLASH->STATR sampled after unlock
 */

#define FLASH_R_BASE 0x40022000u
#define REG(off) (*(volatile unsigned int *)(FLASH_R_BASE + (off)))

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define SR_BSY 0x00000001u
#define SR_WR_BSY 0x00000002u
#define SR_WRPRTERR 0x00000010u

#define CR_PG 0x00000001u
#define CR_PER 0x00000002u
#define CR_MER 0x00000004u
#define CR_STRT 0x00000040u
#define CR_LOCK 0x00000080u
#define CR_PAGE_PG 0x00010000u
#define CR_PG_STRT 0x00200000u

#define TIMEOUT 2000000u

#define RESULT (*(volatile unsigned int *)0x20000FF0u)
#define PROGRESS (*(volatile unsigned int *)0x20000FF4u)
#define WPR_SNAP (*(volatile unsigned int *)0x20000FF8u)
#define OBR_SNAP (*(volatile unsigned int *)0x20000FFCu)
#define CTLR_SNAP (*(volatile unsigned int *)0x20000FE0u)
#define STATR_SNAP (*(volatile unsigned int *)0x20000FE4u)

static unsigned int wait_busy(void)
{
    unsigned int n = TIMEOUT;

    while (REG(0x0C) & SR_BSY)
    {
        if (--n == 0u)
            return 1u;
    }
    if (REG(0x0C) & SR_WRPRTERR)
        return 2u;
    return 0u;
}

static void fail(unsigned int code)
{
    RESULT = 0xBAD00000u | code;
    for (;;)
        ;
}

static void erase_page(unsigned int addr)
{
    unsigned int st;

    REG(0x10) |= CR_PER;
    REG(0x14) = addr & 0xFFFFFC00u;
    REG(0x10) |= CR_STRT;
    st = wait_busy();
    REG(0x10) &= ~CR_PER;
    if (st != 0u)
        fail(0xE0u + st);
}

static void program_range(unsigned int src, unsigned int dst, unsigned int len)
{
    unsigned int i;
    unsigned int st;

    for (i = 0; i < len; i += 2u)
    {
        /* 与 WCH 库 FLASH_ProgramHalfWord 一致：每个半字单独置/清 CR_PG。
         * CR_PG 一直保持打开时，写完 1 KB（512 个半字）后 BSY 不再清除，
         * 后续写入永久挂死（PROGRESS 停在 0xFFE）。 */
        REG(0x10) |= CR_PG;
        *(volatile unsigned short *)(dst + i) = *(volatile unsigned short *)(src + i);
        st = wait_busy();
        REG(0x10) &= ~CR_PG;
        if (st != 0u)
        {
            fail(0xE0u + st);
        }
        if ((i & 0xFFFu) == 0xFFEu)
            PROGRESS = i;
    }
}

static void fast_program_page(unsigned int src, unsigned int page)
{
    unsigned int i;
    unsigned int st;

    REG(0x10) |= CR_PAGE_PG;
    while (REG(0x0C) & SR_BSY)
        ;
    while (REG(0x0C) & SR_WR_BSY)
        ;

    for (i = 0; i < 64u; i++)
    {
        *(volatile unsigned int *)(page + i * 4u) =
            *(volatile unsigned int *)(src + i * 4u);
        st = wait_busy();
        if (st != 0u)
        {
            REG(0x10) &= ~CR_PAGE_PG;
            fail(0xE0u + st);
        }
    }

    REG(0x10) |= CR_PG_STRT;
    st = wait_busy();
    REG(0x10) &= ~CR_PAGE_PG;
    if (st != 0u)
        fail(0xE0u + st);
}

void stub_main(unsigned int src, unsigned int len, unsigned int dst, unsigned int mode)
{
    unsigned int off;
    unsigned int st;

    RESULT = 0;
    PROGRESS = 0;
    WPR_SNAP = REG(0x20);
    OBR_SNAP = REG(0x1C);

    if (mode == 6u)
    {
        /* CSR probe: dump CSR 0x300 / 0x800 around set/clear operations.
         * Results at 0x20000FC0..0x20000FE7 (pairs: 0x300, 0x800):
         *   d[0..1] initial, d[2..3] after csrw 0x800=0x6088,
         *   d[4..5] after csrw 0x300=0x6088, d[6..7] after csrc 0x800,0x88,
         *   d[8..9] after csrs 0x800,0x88 */
        volatile unsigned int *d = (volatile unsigned int *)0x20000FC0u;
        unsigned int a, b;

        __asm volatile("csrr %0, 0x300" : "=r"(a));
        __asm volatile("csrr %0, 0x800" : "=r"(b));
        d[0] = a; d[1] = b;

        __asm volatile("li t0, 0x6088\n\tcsrw 0x800, t0" ::: "t0");
        __asm volatile("csrr %0, 0x300" : "=r"(a));
        __asm volatile("csrr %0, 0x800" : "=r"(b));
        d[2] = a; d[3] = b;

        __asm volatile("li t0, 0x6088\n\tcsrw 0x300, t0" ::: "t0");
        __asm volatile("csrr %0, 0x300" : "=r"(a));
        __asm volatile("csrr %0, 0x800" : "=r"(b));
        d[4] = a; d[5] = b;

        __asm volatile("csrc 0x800, %0" :: "r"(0x88u));
        __asm volatile("csrr %0, 0x300" : "=r"(a));
        __asm volatile("csrr %0, 0x800" : "=r"(b));
        d[6] = a; d[7] = b;

        __asm volatile("csrs 0x800, %0" :: "r"(0x88u));
        __asm volatile("csrr %0, 0x300" : "=r"(a));
        __asm volatile("csrr %0, 0x800" : "=r"(b));
        d[8] = a; d[9] = b;

        RESULT = 0x600D0006u;
        for (;;)
            ;
    }

    REG(0x04) = FLASH_KEY1;
    REG(0x04) = FLASH_KEY2;

    CTLR_SNAP = REG(0x10);
    STATR_SNAP = REG(0x0C);
    if (CTLR_SNAP & CR_LOCK)
        fail(0x10u);

    if (mode == 4u)
    {
        /* read 8 words from flash into SRAM for debugger inspection */
        for (off = 0; off < 8u; off++)
        {
            *(volatile unsigned int *)(0x20000FD0u + off * 4u) =
                *(volatile unsigned int *)(dst + off * 4u);
        }
        PROGRESS = 8u;
    }
    else if (mode == 3u)
    {
        REG(0x10) |= CR_MER;
        REG(0x10) |= CR_STRT;
        st = wait_busy();
        REG(0x10) &= ~CR_MER;
        if (st != 0u)
            fail(0xE0u + st);
        PROGRESS = len;
    }
    else
    {
        if (mode != 1u)
        {
            for (off = 0; off < len; off += 1024u)
            {
                erase_page(dst + off);
                PROGRESS = off;
            }
        }
        if (mode == 5u)
        {
            /* erase 1 KB sectors, then fast-program 256 B pages */
            for (off = 0; off < len; off += 1024u)
                erase_page(dst + off);
            for (off = 0; off < len; off += 256u)
            {
                fast_program_page(src + off, dst + off);
                PROGRESS = off;
            }
            PROGRESS = len;
        }
        else if (mode != 0u)
        {
            program_range(src, dst, len);
            PROGRESS = len;
        }
    }

    REG(0x10) |= CR_LOCK;
    RESULT = 0x600D0000u | (len & 0xFFFFu);

    for (;;)
        ;
}
