#include "acpi.h"
#include "printk.h"
#include "string.h"
#include "io.h"

#include <stddef.h>

static struct acpi_rsdp *g_rsdp;
static int g_revision;
static struct acpi_madt *g_madt;

/* Power-control registers, cached from the FACP (SystemIO ports). */
static uint32_t g_pm1a_cnt;
static uint32_t g_pm1b_cnt;
static struct acpi_generic_address g_reset_reg;
static int g_reset_present;
static uint8_t g_reset_value;
/* ACPI enable: smi_cmd port + acpi_enable byte (FACP offsets 56 / 57). */
static uint32_t g_smi_cmd;
static uint8_t  g_acpi_enable;
static int g_acpi_enable_present;

struct ioapic_info {
    uint64_t addr;
    uint32_t gsi_base;
};
static struct ioapic_info g_ioapics[8];
static int g_ioapic_count;

struct iso_info {
    uint8_t  irq;
    uint32_t gsi;
    uint16_t flags;
};
static struct iso_info g_isos[16];
static int g_iso_count;

static uint8_t acpi_checksum(const void *table, uint32_t length)
{
    uint8_t sum = 0;
    const uint8_t *p = (const uint8_t *)table;
    for (uint32_t i = 0; i < length; i++)
        sum += p[i];
    return sum;
}

static void *acpi_find_table(const char *signature)
{
    if (!g_rsdp)
        return 0;

    uint32_t entry_count;
    uint64_t *entry_ptr;

    if (g_revision > 0)
    {
        if (!g_rsdp->xsdt_addr)
            return 0;
        struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)(uintptr_t)g_rsdp->xsdt_addr;
        if (__builtin_memcmp(xsdt->signature, "XSDT", 4) != 0)
            return 0;
        entry_count = (xsdt->length - sizeof(*xsdt)) / 8;
        entry_ptr = (uint64_t *)((uintptr_t)xsdt + sizeof(*xsdt));
    }
    else
    {
        if (!g_rsdp->rsdt_addr)
            return 0;
        struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)(uintptr_t)g_rsdp->rsdt_addr;
        if (__builtin_memcmp(rsdt->signature, "RSDT", 4) != 0)
            return 0;
        entry_count = (rsdt->length - sizeof(*rsdt)) / 4;
        uint32_t *entry32 = (uint32_t *)((uintptr_t)rsdt + sizeof(*rsdt));
        for (uint32_t i = 0; i < entry_count; i++)
        {
            struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry32[i];
            if (__builtin_memcmp(hdr->signature, signature, 4) == 0)
                return hdr;
        }
        return 0;
    }

    for (uint32_t i = 0; i < entry_count; i++)
    {
        struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entry_ptr[i];
        if (__builtin_memcmp(hdr->signature, signature, 4) == 0)
            return hdr;
    }
    return 0;
}

void acpi_init(void *rsdp_addr)
{
    if (!rsdp_addr)
    {
        printk("ACPI: no RSDP\n");
        return;
    }
    g_rsdp = (struct acpi_rsdp *)rsdp_addr;
    if (__builtin_memcmp(g_rsdp->signature, "RSD PTR ", 8) != 0)
    {
        printk("ACPI: bad RSDP signature at %p, scanning memory...\n", rsdp_addr);
        /* Scan low memory (the traditional RSDP location on real PCs) for a
           valid RSDP, in case the bootloader could not provide one. */
        struct acpi_rsdp *found = 0;
        for (uintptr_t base = 0; base < 0x100000; base += 16)
        {
            struct acpi_rsdp *cand = (struct acpi_rsdp *)(uintptr_t)base;
            if (__builtin_memcmp(cand->signature, "RSD PTR ", 8) == 0)
            {
                found = cand;
                break;
            }
        }
        if (!found)
        {
            printk("ACPI: no valid RSDP found\n");
            g_rsdp = 0;
            return;
        }
        printk("ACPI: found RSDP at %p\n", found);
        g_rsdp = found;
        rsdp_addr = found;
    }
    g_revision = g_rsdp->revision;
    uint32_t rsdp_len = g_revision > 0 ? 36 : 20;
    if (acpi_checksum(g_rsdp, rsdp_len) != 0)
    {
        printk("ACPI: RSDP checksum failed\n");
        g_rsdp = 0;
        return;
    }
    printk("ACPI: RSDP v%u oem=%c%c%c%c%c%c\n", g_revision,
           g_rsdp->oem[0], g_rsdp->oem[1], g_rsdp->oem[2],
           g_rsdp->oem[3], g_rsdp->oem[4], g_rsdp->oem[5]);

    if (g_revision > 0 && (!g_rsdp->xsdt_addr))
    {
        printk("ACPI: XSDT address is zero\n");
        g_rsdp = 0;
        return;
    }

    g_madt = (struct acpi_madt *)acpi_find_table("APIC");
    if (!g_madt)
    {
        printk("ACPI: MADT not found\n");
        return;
    }
    if (acpi_checksum(g_madt, g_madt->header.length) != 0)
    {
        printk("ACPI: MADT checksum failed\n");
        g_madt = 0;
        return;
    }
    printk("ACPI: MADT lapic=0x%x flags=%u\n",
           g_madt->local_apic_addr, g_madt->flags);

    uint32_t remaining = g_madt->header.length - sizeof(*g_madt);
    uint8_t *ptr = g_madt->entries;
    while (remaining >= 2)
    {
        uint8_t type = ptr[0];
        uint8_t len  = ptr[1];
        if (len < 2 || len > remaining)
            break;
        switch (type)
        {
        case MADT_TYPE_IO_APIC:
        {
            struct madt_io_apic *ioapic = (struct madt_io_apic *)ptr;
            if (g_ioapic_count < 8)
            {
                g_ioapics[g_ioapic_count].addr = ioapic->ioapic_addr;
                g_ioapics[g_ioapic_count].gsi_base = ioapic->gsi_base;
                g_ioapic_count++;
                printk("ACPI: IOAPIC id=%u addr=0x%x gsi_base=%u\n",
                       ioapic->ioapic_id, ioapic->ioapic_addr, ioapic->gsi_base);
            }
            break;
        }
        case MADT_TYPE_ISO:
        {
            struct madt_iso *iso = (struct madt_iso *)ptr;
            if (g_iso_count < 16)
            {
                g_isos[g_iso_count].irq = iso->irq_src;
                g_isos[g_iso_count].gsi = iso->gsi;
                g_isos[g_iso_count].flags = iso->flags;
                g_iso_count++;
                printk("ACPI: ISO IRQ%u -> GSI%u flags=0x%x\n",
                       iso->irq_src, iso->gsi, iso->flags);
            }
            break;
        }
        }
        ptr += len;
        remaining -= len;
    }

    if (g_ioapic_count == 0)
        printk("ACPI: no IOAPICs found in MADT\n");
    else
        printk("ACPI: %d IOAPIC(s), %d ISO(s)\n", g_ioapic_count, g_iso_count);

    /* Cache the power-control registers from the FACP so sys_reboot /
       sys_poweroff do not need to re-walk the RSDT. Firmware may provide an
       older/smaller FACP than our struct, so check each field's offset +
       size against header.length before reading it. */
    {
        struct acpi_fadt *fadt = (struct acpi_fadt *)acpi_find_table("FACP");
        if (fadt)
        {
            /* pm1a_cnt_blk (off 64, 4 bytes) + pm1b_cnt_blk (off 68, 4 bytes). */
            if (fadt->header.length >= offsetof(struct acpi_fadt, pm1b_cnt_blk) + 4)
            {
                g_pm1a_cnt = fadt->pm1a_cnt_blk;
                g_pm1b_cnt = fadt->pm1b_cnt_blk;
            }
            /* smi_cmd (off 56, 4 bytes) + acpi_enable (off 57, 1 byte). */
            if (fadt->header.length >= offsetof(struct acpi_fadt, acpi_enable) + 1)
            {
                g_smi_cmd = fadt->smi_cmd;
                g_acpi_enable = fadt->acpi_enable;
                g_acpi_enable_present = 1;
            }
            /* reset_reg (off 116, 12 bytes) + reset_value (off 128, 1 byte). */
            if (fadt->header.length >= offsetof(struct acpi_fadt, reset_reg) +
                                          sizeof(struct acpi_generic_address))
            {
                g_reset_reg = fadt->reset_reg;
                g_reset_present = 1;
                if (fadt->header.length >= offsetof(struct acpi_fadt, reset_value) + 1)
                    g_reset_value = fadt->reset_value;
            }
            printk("ACPI: FACP pm1a_cnt=0x%x pm1b_cnt=0x%x smi_cmd=0x%x acpi_enable=0x%x%s\n",
                   g_pm1a_cnt, g_pm1b_cnt, g_smi_cmd, g_acpi_enable,
                   g_reset_present ? " reset_reg=yes" : "");
        }
        else
        {
            printk("ACPI: FACP not found — power-off via ACPI S5 unavailable\n");
        }
    }
}

/* Short I/O pause so the write hits the chipset before we continue. */
static void acpi_pause(void)
{
    for (volatile int i = 0; i < 100000; i++)
        ;
}

/* Cold reboot. Tries the ACPI reset register first (needs SCI disabled on
   most firmware — not necessary for QEMU), then the 8042 keyboard-controller
   reset pulse and a final null-pointer fault as a last resort. */
void acpi_reboot(void)
{
    printk("ACPI: reboot requested\n");

    if (g_reset_present)
    {
        if (g_reset_reg.space_id == 1 && g_reset_reg.address < 0x10000ULL)
            outb((uint16_t)g_reset_reg.address, g_reset_value);
        else if (g_reset_reg.space_id == 0 &&
                 g_reset_reg.address < 0x100000000ULL)
            *(volatile uint8_t *)(uintptr_t)g_reset_reg.address = g_reset_value;
        acpi_pause();
    }

    /* 8042 "pulse reset line" (works on QEMU and most PCs). */
    outb(0x64, 0xFE);
    acpi_pause();

    /* Last resort: triple fault via a null write. */
    *(volatile uint8_t *)0 = 0;
    for (;;)
        hal_cpu_halt();
}

/* Program one PM1x counter register for sleep state S5 (power-off). Per the
   ACPI spec we write SLP_TYP (bits 10-12) with SLP_EN (bit 13) clear, then
   set SLP_EN in a second write; the chipset latches and cuts power. The
   SLP_TYP value for S5 comes from the firmware's _S5 AML — we do not
   interpret AML, so we try every SLP_TYP value (0-7) in case the firmware's
   S5 differs from 7. QEMU ignores S5 unless ACPI is enabled first
   (SCI_EN + smi_cmd, see acpi_enable_acpi()). */
static void acpi_program_s5(uint16_t port)
{
    uint16_t val;

    if (!port)
        return;
    for (int slp_typ = 0; slp_typ < 8; slp_typ++)
    {
        val = inw(port);
        val &= ~(uint16_t)((0x07u << 10) | (1u << 13)); /* clear SLP_TYP + SLP_EN */
        val |= (uint16_t)((uint16_t)slp_typ << 10);     /* set SLP_TYP candidate */
        outw(port, val);
        acpi_pause();
        val |= (uint16_t)(1u << 13);                    /* set SLP_EN */
        outw(port, val);
        acpi_pause();
    }
}

/* Enable ACPI before touching PM1_CNT: write acpi_enable to smi_cmd (if the
   FACP provides it), then set SCI_EN (bit 0) in PM1a_CNT/PM1b_CNT so the
   chipset actually honours SLP_EN. QEMU ignores S5 unless ACPI is enabled. */
static void acpi_enable_acpi(void)
{
    if (g_acpi_enable_present && g_smi_cmd)
        outb((uint16_t)g_smi_cmd, g_acpi_enable);
    acpi_pause();

    if (g_pm1a_cnt)
    {
        uint16_t v = inw((uint16_t)g_pm1a_cnt);
        v |= 1u;                       /* SCI_EN */
        outw((uint16_t)g_pm1a_cnt, v);
    }
    if (g_pm1b_cnt)
    {
        uint16_t v = inw((uint16_t)g_pm1b_cnt);
        v |= 1u;                       /* SCI_EN */
        outw((uint16_t)g_pm1b_cnt, v);
    }
    acpi_pause();
}

void acpi_poweroff(void)
{
    printk("ACPI: power-off requested\n");

    /* 1. Enable ACPI before touching PM1_CNT: write acpi_enable to smi_cmd
       (if the FACP provides it), then set SCI_EN (bit 0) in PM1a_CNT/
       PM1b_CNT so the chipset actually honours SLP_EN. QEMU ignores S5
       unless ACPI is enabled. */
    acpi_enable_acpi();

    /* 2. ACPI S5 on the FACP's PM1 control blocks. */
    if (g_pm1a_cnt)
        acpi_program_s5((uint16_t)g_pm1a_cnt);
    acpi_pause();
    if (g_pm1b_cnt)
        acpi_program_s5((uint16_t)g_pm1b_cnt);
    acpi_pause();

    /* 3. Some firmware (and QEMU) expose PM1_CNT at well-known ports that
       differ from the FACP. Try the common QEMU/PIIX4 PM1a_CNT port. */
    acpi_program_s5(0x8004);

    /* 4. APM power-off: write 0x00 to port 0xb2. QEMU honours this directly
       and needs no ACPI enable sequence. Try it last, before the reset
       fallback, since it shares the smi_cmd port used in step 1. */
    outb(0xB2, 0x00);
    acpi_pause();

    /* No ACPI power control (or firmware ignored us): degrade to a reset. */
    printk("ACPI: S5 unavailable/ignored, resetting instead\n");
    acpi_reboot();
    for (;;)
        hal_cpu_halt();
}

int acpi_ioapic_count(void) { return g_ioapic_count; }

int acpi_ioapic_info(int idx, uint64_t *addr, uint32_t *gsi_base)
{
    if (idx < 0 || idx >= g_ioapic_count)
        return -1;
    if (addr) *addr = g_ioapics[idx].addr;
    if (gsi_base) *gsi_base = g_ioapics[idx].gsi_base;
    return 0;
}

int acpi_iso_lookup(uint8_t irq, uint32_t *gsi, uint16_t *flags)
{
    for (int i = 0; i < g_iso_count; i++)
    {
        if (g_isos[i].irq == irq)
        {
            if (gsi) *gsi = g_isos[i].gsi;
            if (flags) *flags = g_isos[i].flags;
            return 0;
        }
    }
    return -1;
}