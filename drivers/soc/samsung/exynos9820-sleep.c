// SPDX-License-Identifier: GPL-2.0
/*
 * Exynos 9820 system sleep (suspend to RAM, "deep"), after the stock kernel's exynos-pm and PMUCAL SYS_SLEEP
 * (drivers/soc/samsung/exynos-pm.c, cal-if/exynos9820/flexpmu_cal_system_exynos9820.h in Samsung's 4.14).
 *
 * The SoC is not powered down by this code but by the firmware: the kernel arms the PMU (the wake-up sources,
 * the SLEEP hint in CPU0's CPU_INFORM, CPU0's power-down on WFI) and makes a PSCI CPU_SUSPEND for the whole
 * system; EL3 takes CPU0 down, the APM sees the hint with every CPU down and runs its SOC_SLEEP_DOWN (and
 * MIF_SLEEP_DOWN, DRAM self-refresh, when no other master holds the MIF). A wake-up source the PMU lets through
 * brings it back up and EL3 restarts CPU0 at cpu_resume. When something is already pending the firmware does not
 * go down and the call returns: an early wake-up, harmless.
 *
 * Until it is proven, it is off: the "mode" parameter (0) refuses deep sleep, and s2idle stays the kernel's
 * default (mem_sleep_default=s2idle on the command line). The other modes, for testing:
 *   1  the whole deep path (CPUs off, syscore) but no PMU arming and no PSCI call (pm_test=core does the same
 *      short of ->enter)
 *   2  dry run: the PMU armed as for a sleep, then disarmed as after an early wake-up, without the PSCI call
 *   3  sleep
 * debugfs exynos9820-sleep/regs shows the PMU's sleep registers and the firmware's sleep counters,
 * exynos9820-sleep/last what the last deep sleep did.
 */

#include <linux/console.h>
#include <linux/cpu_pm.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/psci.h>
#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/timekeeping.h>

#include <asm/arch_timer.h>
#include <asm/cacheflush.h>
#include <asm/suspend.h>

#include "exynos-pmu.h"

/* PMU (0x15860000) */
#define PMU_EINT_WAKEUP_MASK		0x060c	/* gpa0..gpa3, 8 bits each (EINT 0..31): 1 = masked */
#define PMU_EINT_WAKEUP_MASK2		0x061c	/* gpa4 (EINT 32, 33) */
#define PMU_CPU_INFORM0			0x0860
#define CPU_INFORM_SLEEP		4
#define PMU_CLUSTER0_CPU0_INT_EN	0x1044
#define PMU_CP_INT_EN			0x3044
#define PMU_TOP_OUT			0x3920
#define PMU_WAKEUP_INT_EN		0x3944	/* 1 = this source may wake the SoC */
#define PMU_WAKEUP_STAT			0x3950
#define PMU_WAKEUP2_STAT		0x3954
#define PMU_WAKEUP2_INT_EN		0x3964
#define PMU_SYSTEM_CTRL			0x3a10
/* the PMU's atomic aliases: a write of a bit's NUMBER sets or clears that one bit */
#define PMU_SET_BIT(reg)		((reg) | 0xc000)
#define PMU_CLR_BIT(reg)		((reg) | 0x8000)

/* PMU_INTR_GEN (0x15870000): group N's enable / pending / clear. The DT's syscon for it is 4 KiB, and PMUCAL
 * clears groups 26, 27 and 31 (0x1a0c..0x1f0c), so it is mapped here, 8 KiB. */
#define PMU_INTR_GEN_BASE		0x15870000
#define PMU_INTR_GEN_SIZE		0x2000
#define GRP_INTR_BID_ENABLE(n)		((n) * 0x100)
#define GRP_INTR_BID_UPEND(n)		((n) * 0x100 + 0x8)
#define GRP_INTR_BID_CLEAR(n)		((n) * 0x100 + 0xc)

/* WAKEUP_INT_EN bits (stock DT exynos-pm wakeup_stat names) */
#define WAKEUP_MAILBOX_CP2AP		BIT(8)
#define WAKEUP_S_MAILBOX_CP2AP		BIT(10)
#define WAKEUP_EINT			BIT(12)

/* EINT_WAKEUP_MASK bits: 8 * the bank (gpa0..gpa3) + the pin */
#define EINT_GPA0_1_PMIC		BIT(1)	/* s2mps19: its RTC alarm */
#define EINT_GPA2_4_POWER_KEY		BIT(20)

/* The firmware's own record of its sleeps, in the APM's SRAM (stock exynos-flexpmu-dbg) */
#define FLEXPMU_DBG_BASE		0x0204bc00
#define FLEXPMU_DBG_SIZE		0x400
#define FLEXPMU_MIF_REQUEST		0x0ac	/* bits 23:16: the masters holding the MIF */
#define FLEXPMU_EARLY_WAKEUP		0x168
#define FLEXPMU_APSOC_DOWN		0x16c
#define FLEXPMU_MIF_DOWN		0x17c

/* PSCI CPU_SUSPEND for the whole system: stock's psci_suspend_customized_finisher for PSCI_SYSTEM_SLEEP,
 * power level 3 (the system), id 0, type 0. Samsung's EL3 keys on the power level. */
#define PSCI_SYSTEM_SLEEP_STATE		0x03000000

enum { MODE_OFF, MODE_PATH, MODE_DRY, MODE_SLEEP };

static int mode;
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "0 no deep sleep, 1 path only (no PMU, no PSCI), 2 dry run (PMU, no PSCI), 3 sleep");

/* What may wake it. EINT: the PMIC (its RTC alarm) and the power key; through WAKEUP_INT_EN only the EINT
 * gate. The modem (WAKEUP_INT_EN bits 8 and 10) comes once a sleep has been seen to work. */
static uint eint_wake = EINT_GPA0_1_PMIC | EINT_GPA2_4_POWER_KEY;
module_param(eint_wake, uint, 0644);
MODULE_PARM_DESC(eint_wake, "EINT 0..31 that wake the SoC (bit 8 * gpa bank + pin; 1 = wakes)");
static uint wakeup_int_en = WAKEUP_EINT;
module_param(wakeup_int_en, uint, 0644);
MODULE_PARM_DESC(wakeup_int_en, "PMU WAKEUP_INT_EN in sleep (bit 12 EINT, 8/10 the modem's mailboxes)");

/*
 * For debugging a sleep: the pstore console stays live through it, so a stall shows its last lines in
 * console-ramoops after the watchdog's reset. no_console_suspend cannot do it: it makes suspend switch to a text
 * VT, and the first line printed there makes fbcon take the display over (white screen, 2026-11-08).
 */
static bool pstore_console;
module_param(pstore_console, bool, 0644);
MODULE_PARM_DESC(pstore_console, "Keep the ramoops console printing through the sleep (debugging)");

struct sleep_ctx {
	void __iomem *pmu;
	void __iomem *intr;		/* PMU_INTR_GEN */
	void __iomem *cmu_cpucl0;	/* 0x1d020000 */
	void __iomem *cmu_core;		/* 0x1a030000 */
	void __iomem *cmu_top;		/* 0x1a240000 */
	void __iomem *flexpmu;
};

static struct sleep_ctx ctx;

/* CP_INT_EN as the sleep found it: bit 6 is set at boot already (0xf3), so the exit leaves it set, not clear */
static u32 cp_int_en;

/* What the last deep sleep did */
static struct {
	int mode;
	int ret;
	u32 wakeup_stat, wakeup2_stat;
	u32 early_wakeup[2], apsoc_down[2], mif_down[2], mif_request[2];
	u64 at_ns;
	u64 clean_ticks;	/* sleep_clean_ram(), in arch timer ticks */
} last;

/* ---- register access: straight-line, no WFI (CPU0 is armed to power down on WFI from enter_sleep on) ---- */

static u32 pmu_read(u32 off)
{
	return readl_relaxed(ctx.pmu + off);
}

static void pmu_write(u32 off, u32 val)
{
	writel_relaxed(val, ctx.pmu + off);
}

static void pmu_rmw(u32 off, u32 mask, u32 val)
{
	pmu_write(off, (pmu_read(off) & ~mask) | (val & mask));
}

static void pmu_set_bit(u32 off, unsigned int bit)
{
	writel_relaxed(bit, ctx.pmu + PMU_SET_BIT(off));
}

static void pmu_clr_bit(u32 off, unsigned int bit)
{
	writel_relaxed(bit, ctx.pmu + PMU_CLR_BIT(off));
}

static void rmw(void __iomem *base, u32 off, u32 mask, u32 val)
{
	writel_relaxed((readl_relaxed(base + off) & ~mask) | (val & mask), base + off);
}

/* PMUCAL's CLEAR_PEND: clear what is pending (of mask) in a PMU_INTR_GEN group */
static void intr_clear_pend(unsigned int grp, u32 mask)
{
	u32 pend = readl_relaxed(ctx.intr + GRP_INTR_BID_UPEND(grp));

	writel_relaxed(pend & mask, ctx.intr + GRP_INTR_BID_CLEAR(grp));
}

static u32 flexpmu_read(u32 off)
{
	return ctx.flexpmu ? readl_relaxed(ctx.flexpmu + off) : 0;
}

static void read_counters(int i)
{
	last.early_wakeup[i] = flexpmu_read(FLEXPMU_EARLY_WAKEUP);
	last.apsoc_down[i] = flexpmu_read(FLEXPMU_APSOC_DOWN);
	last.mif_down[i] = flexpmu_read(FLEXPMU_MIF_DOWN);
	last.mif_request[i] = (flexpmu_read(FLEXPMU_MIF_REQUEST) >> 16) & 0xff;
}

/* QCH_CON of CMU_CPUCL0's SCLK and of CMU_CORE's PPC/PPMU for the CPU clusters: 2 and 4 while the SoC sleeps. Stock
 * runs them at 6 (pmucal_lpm_init) and puts 6 back; here they run at S-Boot's 2, so what was there goes back. */
static const u32 core_qch[] = { 0x30b4, 0x30b8, 0x30bc, 0x30c0, 0x30e0, 0x30e4, 0x30e8, 0x30ec };
static u32 qch_saved[1 + ARRAY_SIZE(core_qch)];

static void qch_sleep(void)
{
	qch_saved[0] = readl_relaxed(ctx.cmu_cpucl0 + 0x30b4);
	rmw(ctx.cmu_cpucl0, 0x30b4, 0x7, 2);
	for (int i = 0; i < ARRAY_SIZE(core_qch); i++) {
		qch_saved[i + 1] = readl_relaxed(ctx.cmu_core + core_qch[i]);
		rmw(ctx.cmu_core, core_qch[i], 0x7, 4);
	}
}

static void qch_restore(void)
{
	writel_relaxed(qch_saved[0], ctx.cmu_cpucl0 + 0x30b4);
	for (int i = 0; i < ARRAY_SIZE(core_qch); i++)
		writel_relaxed(qch_saved[i + 1], ctx.cmu_core + core_qch[i]);
}

/* pmucal_lpm_init's PMU settings, Linux's in stock and never made here: the sequencer's waits for the PMIC's rails and
 * the TCXO on the way back up (S-Boot leaves SUB, CPUCL2 and the TCXO's at 0) and BURNIN_CTRL. Set for the sleep,
 * and S-Boot's put back after it. */
static const struct {
	u32 off, mask, val;
} lpm_init[] = {
	{ 0x0a08, GENMASK(1, 0), 0x2 },		/* BURNIN_CTRL */
	{ 0x0a08, GENMASK(23, 16), 0 },
	{ 0x0e00, ~0u, 0x7d0 },			/* EXT_REGULATOR_MAIN_DURATION */
	{ 0x0e04, ~0u, 0x528 },			/* EXT_REGULATOR_SUB_DURATION */
	{ 0x0e08, ~0u, 0x8c },			/* EXT_REGULATOR_CPUCL2_DURATION */
	{ 0x0e0c, ~0u, 0 },			/* EXT_REGULATOR_G3D_DURATION */
	{ 0x0284, ~0u, 0x66c },			/* TCXO_DURATION3 */
};
static const u32 lpm_regs[] = { 0x0a08, 0x0e00, 0x0e04, 0x0e08, 0x0e0c, 0x0284 };
static u32 lpm_saved[ARRAY_SIZE(lpm_regs)];

static void lpm_sleep(void)
{
	for (int i = 0; i < ARRAY_SIZE(lpm_regs); i++)
		lpm_saved[i] = pmu_read(lpm_regs[i]);
	for (int i = 0; i < ARRAY_SIZE(lpm_init); i++)
		pmu_rmw(lpm_init[i].off, lpm_init[i].mask, lpm_init[i].val);
}

static void lpm_restore(void)
{
	for (int i = 0; i < ARRAY_SIZE(lpm_regs); i++)
		pmu_write(lpm_regs[i], lpm_saved[i]);
}

/* EARLY_WAKEUP_DPU_CTRL/DEST in CMU_TOP's space: stock's exit writes what pmucal_lpm_init set (CTRL bit 0, DEST
 * 0x0f001048), which Linux never set here, so what S-Boot left goes back */
static u32 dpu_saved[2];

static void early_wakeup_dpu_save(void)
{
	dpu_saved[0] = readl_relaxed(ctx.cmu_top + 0x0884);
	dpu_saved[1] = readl_relaxed(ctx.cmu_top + 0x08a0);
}

static void early_wakeup_dpu_restore(void)
{
	writel_relaxed(dpu_saved[0], ctx.cmu_top + 0x0884);
	writel_relaxed(dpu_saved[1], ctx.cmu_top + 0x08a0);
}

/* ---- the stock sequences (flexpmu_cal_system_exynos9820.h) ---- */

/* exynos_set_wakeupmask: the wake-up sources, fresh for every sleep */
static void arm_wakeup(void)
{
	pmu_write(PMU_WAKEUP_STAT, 0);
	pmu_write(PMU_WAKEUP2_STAT, 0);
	pmu_write(PMU_EINT_WAKEUP_MASK, ~eint_wake);
	pmu_write(PMU_EINT_WAKEUP_MASK2, ~0u);
	pmu_write(PMU_WAKEUP_INT_EN, wakeup_int_en);
	pmu_write(PMU_WAKEUP2_INT_EN, 0);
}

/* enter_sleep (13 entries): CPU0 powers down on its WFI, the CPU clusters' Q-channels let go */
static void enter_sleep(void)
{
	cp_int_en = pmu_read(PMU_CP_INT_EN);
	pmu_set_bit(PMU_CP_INT_EN, 6);
	rmw(ctx.intr, GRP_INTR_BID_ENABLE(2), BIT(0), BIT(0));
	intr_clear_pend(1, BIT(0));
	pmu_rmw(PMU_CLUSTER0_CPU0_INT_EN, BIT(3), BIT(3));
	qch_sleep();
}

/* what exit_sleep and earlywkup_sleep share: the wake-up sources off, the pending wake-ups cleared, CPU0's
 * power-down disarmed, the Q-channels back */
static void disarm(void)
{
	if (!(cp_int_en & BIT(6)))
		pmu_clr_bit(PMU_CP_INT_EN, 6);
	pmu_write(PMU_WAKEUP_INT_EN, 0);
	pmu_write(PMU_WAKEUP2_INT_EN, 0);
	intr_clear_pend(26, BIT(6));
	intr_clear_pend(27, ~0u);
	intr_clear_pend(31, ~0u);
	rmw(ctx.intr, GRP_INTR_BID_ENABLE(2), BIT(0), 0);
}

/* TOP_OUT bits 10..19 whose pads still hold their sleep levels: all of them after a real exit, released by
 * exynos9820_sleep_release_pads() and sleep_finish() */
#define TOP_OUT_PADS			GENMASK(19, 10)
static u32 pads_held;
static DEFINE_SPINLOCK(pads_lock);

/* exit_sleep (54 entries) after a real sleep. Left out for now: the sysregs' DRCG_EN/MEMCLK and the UFS links'
 * UFSP DRCG (dynamic clock gating enables, rows 21-44), which reach blocks that may still be unclocked here. */
static void exit_sleep(void)
{
	disarm();
	intr_clear_pend(2, BIT(0));
	pmu_rmw(PMU_CLUSTER0_CPU0_INT_EN, BIT(3), 0);
	qch_restore();
	early_wakeup_dpu_restore();
	pads_held = TOP_OUT_PADS;
}

/* earlywkup_sleep (22 entries) when the firmware did not go down */
static void exit_early(void)
{
	disarm();
	intr_clear_pend(1, BIT(0));
	pmu_rmw(PMU_CLUSTER0_CPU0_INT_EN, BIT(3), 0);
	qch_restore();
	early_wakeup_dpu_restore();
	pmu_rmw(PMU_SYSTEM_CTRL, BIT(14), 0);
	intr_clear_pend(4, BIT(0));
}

/*
 * All of RAM cleaned to memory by VA, last thing before the firmware call. Without it the SoC's sleep lost the
 * writes of its last moments (2026-11-08, kernel #39: the vmalloc tree and a task changed as the other CPUs went
 * offline came back stale; #40: the hrtimer tree, and the printk ring's newest lines mangled), while the resume
 * context, which cpu_suspend cleans to memory itself, was fine: the firmware does not write all the caches back for
 * this state. A set/way clean of every level (#40) did not help either: it reaches only this CPU's own caches, and
 * the kernel runs at EL1 under Samsung's EL2, which may trap set/way. By VA reaches every coherent cache.
 */
static void sleep_clean_ram(void)
{
	phys_addr_t start, end;
	u64 i, t0;

	t0 = __arch_counter_get_cntvct();
	for_each_mem_range(i, &start, &end)
		dcache_clean_poc((unsigned long)__va(start), (unsigned long)__va(end));
	last.clean_ticks = __arch_counter_get_cntvct() - t0;
	dcache_clean_poc((unsigned long)&last, (unsigned long)(&last + 1));
}

static int sleep_finisher(unsigned long state)
{
	sleep_clean_ram();
	return psci_ops.cpu_suspend(state, __pa_symbol(cpu_resume));
}

/* ---- the platform suspend ops ---- */

static int sleep_begin(suspend_state_t state)
{
	/* Not armed: deep sleep fails at once, before anything is frozen. s2idle comes here too (there are no s2idle
	 * ops), and passes: refused, it was refused every time (2026-11-08, kernel #35). */
	if (state != PM_SUSPEND_MEM)
		return 0;
	if (READ_ONCE(mode) == MODE_OFF)
		return -EPERM;
	/* the SoC sleep takes the power of blocks whose drivers ask pm_suspend_via_firmware() (the PCIe RC) */
	if (READ_ONCE(mode) == MODE_SLEEP)
		pm_set_suspend_via_firmware();
	/* the time here: in ->enter, timekeeping is suspended and reading it WARNs */
	last.at_ns = ktime_get_boottime_ns();
	return 0;
}

/* pstore_console: after console_suspend_all(), which the core does between ->begin and here */
static int sleep_prepare(void)
{
	struct console *con;

	if (!READ_ONCE(pstore_console))
		return 0;
	console_list_lock();
	for_each_console(con)
		if (!strcmp(con->name, "ramoops"))
			console_srcu_write_flags(con, con->flags & ~CON_SUSPENDED);
	console_list_unlock();
	return 0;
}

/* CPU0 alone, IRQs off, every syscore suspended (the GIC distributor saved) */
static int sleep_enter(suspend_state_t state)
{
	int m = READ_ONCE(mode), ret = 0;

	last.mode = m;
	read_counters(0);
	if (m == MODE_PATH) {
		last.ret = 0;
		return 0;
	}

	lpm_sleep();
	early_wakeup_dpu_save();
	arm_wakeup();
	pmu_write(PMU_CPU_INFORM0, CPU_INFORM_SLEEP);
	enter_sleep();

	if (m == MODE_SLEEP)
		ret = cpu_suspend(PSCI_SYSTEM_SLEEP_STATE, sleep_finisher);
	else
		ret = -EAGAIN;	/* the dry run: as if the firmware had refused */

	/* before anything reads WAKEUP_STAT back: exit clears the sources, not the status */
	last.wakeup_stat = pmu_read(PMU_WAKEUP_STAT);
	last.wakeup2_stat = pmu_read(PMU_WAKEUP2_STAT);
	if (ret == 0)
		exit_sleep();
	else
		exit_early();
	pmu_write(PMU_CPU_INFORM0, CPU_INFORM_CLEAR);
	lpm_restore();
	read_counters(1);
	last.ret = ret;

	pr_info("exynos9820-sleep: %s: ret %d, wakeup stat %08x/%08x, APSOC down %u->%u, MIF down %u->%u, early wake-ups %u->%u, MIF held by %02x, RAM cleaned in %llu us\n",
		m == MODE_SLEEP ? "sleep" : "dry run", ret, last.wakeup_stat, last.wakeup2_stat,
		last.apsoc_down[0], last.apsoc_down[1], last.mif_down[0], last.mif_down[1],
		last.early_wakeup[0], last.early_wakeup[1], last.mif_request[1],
		div_u64(last.clean_ticks * USEC_PER_SEC, arch_timer_get_cntfrq()));

	/* an early wake-up is a sleep that did not happen; the dry run is meant not to */
	return m == MODE_DRY ? 0 : ret;
}

/*
 * exit_sleep's last rows, TOP_OUT bits 10..19, release the pads' retention: until then every pad of a block that
 * lost power holds the level it had going to sleep. They come here, after dpm_resume_early, because only then has
 * pinctrl (a late/early PM op) written the GPIO banks back; released at the firmware's return, as exit_sleep has
 * them, each output pad of those banks would show the bank's reset state for a while. Stock gets the same order
 * from its pinctrl being a syscore op that runs before exynos-pm's exit, and mainline's own retention_ctrl lets go
 * after the banks are restored too. A driver that needs its pads in its own noirq resume (the PCIe RC: PERST#)
 * lets go of its group earlier, with exynos9820_sleep_release_pads().
 */
static void sleep_finish(void)
{
	exynos9820_sleep_release_pads(TOP_OUT_PADS);
}

/**
 * exynos9820_sleep_release_pads - let go of pads still held since a SoC sleep, ahead of sleep_finish()
 * @mask: TOP_OUT bits (10..19)
 *
 * For a driver that drives its pads in its own noirq resume: the PCIe RC's PERST# stays at its sleep level until
 * the pads are released, and the Wi-Fi chip then never leaves reset. Only for the pads of a pinctrl that has
 * already written its banks back (a noirq op, ordered before its consumers). Nothing after an s2idle or an early
 * wake-up.
 *
 * Return: the bits of @mask that were still held (and are released now).
 */
u32 exynos9820_sleep_release_pads(u32 mask)
{
	unsigned long flags;
	u32 now;

	spin_lock_irqsave(&pads_lock, flags);
	now = pads_held & mask;
	for (unsigned int bit = 10; bit <= 19; bit++)
		if (now & BIT(bit))
			pmu_set_bit(PMU_TOP_OUT, bit);
	pads_held &= ~now;
	spin_unlock_irqrestore(&pads_lock, flags);
	return now;
}
EXPORT_SYMBOL_GPL(exynos9820_sleep_release_pads);

static const struct platform_suspend_ops sleep_ops = {
	.valid = suspend_valid_only_mem,
	.begin = sleep_begin,
	.prepare = sleep_prepare,
	.enter = sleep_enter,
	.finish = sleep_finish,
};

/* exynos-pmu vetoes CPU_PM_ENTER during system suspend (no C2 without its hint); CPU0's own, from the cpu_pm
 * syscore, must pass. This syscore runs before cpu_pm's on the way down (registered later) and after it on the
 * way up. */
static int sleep_syscore_suspend(void *data)
{
	exynos_pmu_set_system_sleep(true);
	return 0;
}

static void sleep_syscore_resume(void *data)
{
	exynos_pmu_set_system_sleep(false);
}

static const struct syscore_ops sleep_syscore_ops = {
	.suspend = sleep_syscore_suspend,
	.resume = sleep_syscore_resume,
};

static struct syscore sleep_syscore = {
	.ops = &sleep_syscore_ops,
};

/* ---- debugfs ---- */

static const struct {
	const char *name;
	u32 off;
} shown[] = {
	{ "EINT_WAKEUP_MASK", PMU_EINT_WAKEUP_MASK },
	{ "EINT_WAKEUP_MASK2", PMU_EINT_WAKEUP_MASK2 },
	{ "CPU_INFORM0", PMU_CPU_INFORM0 },
	{ "CLUSTER0_CPU0_INT_EN", PMU_CLUSTER0_CPU0_INT_EN },
	{ "CP_INT_EN", PMU_CP_INT_EN },
	{ "TOP_OUT", PMU_TOP_OUT },
	{ "WAKEUP_INT_EN", PMU_WAKEUP_INT_EN },
	{ "WAKEUP_STAT", PMU_WAKEUP_STAT },
	{ "WAKEUP2_STAT", PMU_WAKEUP2_STAT },
	{ "WAKEUP2_INT_EN", PMU_WAKEUP2_INT_EN },
	{ "SYSTEM_CTRL", PMU_SYSTEM_CTRL },
	/* pmucal_lpm_init's settle times and burn-in: does S-Boot set them? (0 = the SoC may wake before its rails) */
	{ "BURNIN_CTRL", 0x0a08 },
	{ "EXT_REGULATOR_MAIN_DURATION", 0x0e00 },
	{ "EXT_REGULATOR_SUB_DURATION", 0x0e04 },
	{ "EXT_REGULATOR_CPUCL2_DURATION", 0x0e08 },
	{ "EXT_REGULATOR_G3D_DURATION", 0x0e0c },
	{ "TCXO_DURATION3", 0x0284 },
};

static int regs_show(struct seq_file *s, void *unused)
{
	for (int i = 0; i < ARRAY_SIZE(shown); i++)
		seq_printf(s, "%-30s %04x  %08x\n", shown[i].name, shown[i].off, pmu_read(shown[i].off));
	for (unsigned int grp = 1; grp <= 31; grp++) {
		if (grp != 1 && grp != 2 && grp != 4 && grp != 26 && grp != 27 && grp != 31)
			continue;
		seq_printf(s, "INTR_GEN GRP%-2u enable %08x pending %08x\n", grp,
			   readl_relaxed(ctx.intr + GRP_INTR_BID_ENABLE(grp)),
			   readl_relaxed(ctx.intr + GRP_INTR_BID_UPEND(grp)));
	}
	seq_printf(s, "QCH CPUCL0 SCLK %x, CORE", readl_relaxed(ctx.cmu_cpucl0 + 0x30b4) & 7);
	for (int i = 0; i < ARRAY_SIZE(core_qch); i++)
		seq_printf(s, " %x", readl_relaxed(ctx.cmu_core + core_qch[i]) & 7);
	seq_printf(s, "\nEARLY_WAKEUP_DPU CTRL %08x DEST %08x", readl_relaxed(ctx.cmu_top + 0x0884),
		   readl_relaxed(ctx.cmu_top + 0x08a0));
	seq_printf(s, "\nflexpmu: early wake-ups %u, APSOC down %u, MIF down %u, MIF held by %02x\n",
		   flexpmu_read(FLEXPMU_EARLY_WAKEUP), flexpmu_read(FLEXPMU_APSOC_DOWN),
		   flexpmu_read(FLEXPMU_MIF_DOWN), (flexpmu_read(FLEXPMU_MIF_REQUEST) >> 16) & 0xff);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int last_show(struct seq_file *s, void *unused)
{
	if (!last.at_ns) {
		seq_puts(s, "no deep sleep yet\n");
		return 0;
	}
	seq_printf(s, "mode %d at %llu ms (boot time): ret %d\nwakeup stat %08x, wakeup2 stat %08x\n"
		   "early wake-ups %u -> %u\nAPSOC down %u -> %u\nMIF down %u -> %u\nMIF held by %02x -> %02x\n",
		   last.mode, last.at_ns / NSEC_PER_MSEC, last.ret, last.wakeup_stat, last.wakeup2_stat,
		   last.early_wakeup[0], last.early_wakeup[1], last.apsoc_down[0], last.apsoc_down[1],
		   last.mif_down[0], last.mif_down[1], last.mif_request[0], last.mif_request[1]);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(last);

/* ---- init ---- */

static void __iomem *map(phys_addr_t pa, size_t size)
{
	void __iomem *va = ioremap(pa, size);

	if (!va)
		pr_err("exynos9820-sleep: cannot map %pa\n", &pa);
	return va;
}

static int __init exynos9820_sleep_init(void)
{
	struct device_node *pmu_np;
	struct dentry *dir;

	if (!of_machine_is_compatible("samsung,exynos9820"))
		return 0;
	if (!psci_ops.cpu_suspend) {
		pr_err("exynos9820-sleep: no PSCI CPU_SUSPEND\n");
		return -ENODEV;
	}

	pmu_np = of_find_compatible_node(NULL, NULL, "samsung,exynos9820-pmu");
	if (!pmu_np)
		return -ENODEV;
	ctx.pmu = of_iomap(pmu_np, 0);
	of_node_put(pmu_np);
	if (!ctx.pmu)
		return -ENODEV;

	ctx.intr = map(PMU_INTR_GEN_BASE, PMU_INTR_GEN_SIZE);
	ctx.cmu_cpucl0 = map(0x1d020000, 0x4000);
	ctx.cmu_core = map(0x1a030000, 0x4000);
	ctx.cmu_top = map(0x1a240000, 0x1000);
	ctx.flexpmu = map(FLEXPMU_DBG_BASE, FLEXPMU_DBG_SIZE);
	if (!ctx.intr || !ctx.cmu_cpucl0 || !ctx.cmu_core || !ctx.cmu_top)
		return -ENOMEM;

	dir = debugfs_create_dir("exynos9820-sleep", NULL);
	debugfs_create_file("regs", 0400, dir, NULL, &regs_fops);
	debugfs_create_file("last", 0400, dir, NULL, &last_fops);

	register_syscore(&sleep_syscore);
	suspend_set_ops(&sleep_ops);
	pr_info("exynos9820-sleep: deep sleep available (mode %d)\n", mode);
	return 0;
}
late_initcall(exynos9820_sleep_init);
