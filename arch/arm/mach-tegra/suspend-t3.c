/*
 * arch/arm/mach-tegra/suspend-t3.c
 *
 * Tegra3 suspend
 *
 * Copyright (c) 2010-2011, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/delay.h>

#include <mach/gpio.h>
#include <mach/iomap.h>
#include <mach/irqs.h>
#include <mach/suspend.h>
#include <asm/hardware/gic.h>

#include <trace/events/power.h>

#include "clock.h"
#include "gpio-names.h"
#include "power.h"
#include "tegra3_emc.h"
#include "dvfs.h"


#define CAR_CCLK_BURST_POLICY \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x20)

#define CAR_SUPER_CCLK_DIVIDER \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x24)

#define CAR_CCLKG_BURST_POLICY \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x368)

#define CAR_SUPER_CCLKG_DIVIDER \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x36C)

#define CAR_CCLKLP_BURST_POLICY \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x370)
#define PLLX_DIV2_BYPASS_LP	(1<<16)

#define CAR_SUPER_CCLKLP_DIVIDER \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x374)

#define CAR_BOND_OUT_V \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x390)
#define CAR_BOND_OUT_V_CPU_G	(1<<0)
#define CAR_BOND_OUT_V_CPU_LP	(1<<1)

#define CAR_CLK_ENB_V_SET \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x440)
#define CAR_CLK_ENB_V_CPU_G	(1<<0)
#define CAR_CLK_ENB_V_CPU_LP	(1<<1)

#define CAR_RST_CPUG_CMPLX_SET \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x450)

#define CAR_RST_CPUG_CMPLX_CLR \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x454)

#define CAR_RST_CPULP_CMPLX_SET \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x458)

#define CAR_RST_CPULP_CMPLX_CLR \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x45C)

#define CAR_CLK_CPUG_CMPLX_SET \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x460)

#define CAR_CLK_CPUG_CMPLX_CLR \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x464)

#define CAR_CLK_CPULP_CMPLX_SET \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x468)

#define CAR_CLK_CPULP_CMPLX_CLR \
	(IO_ADDRESS(TEGRA_CLK_RESET_BASE) + 0x46C)


void tegra_suspend_dram(bool lp0_ok, unsigned int flags);

static int cluster_switch_prolog_clock(unsigned int flags)
{
	u32 reg;
	u32 CclkBurstPolicy;
	u32 SuperCclkDivier;

	/* Read the bond out register containing the G and LP CPUs. */
	reg = readl(CAR_BOND_OUT_V);

	/* Sync G-PLLX divider bypass with LP (no effect on G, just to prevent
	   LP settings overwrite by save/restore code */
	CclkBurstPolicy = ~PLLX_DIV2_BYPASS_LP & readl(CAR_CCLKG_BURST_POLICY);
	CclkBurstPolicy |= PLLX_DIV2_BYPASS_LP & readl(CAR_CCLKLP_BURST_POLICY);
	writel(CclkBurstPolicy, CAR_CCLKG_BURST_POLICY);

	/* Switching to G? */
	if (flags & TEGRA_POWER_CLUSTER_G) {
		/* Do the G CPUs exist? */
		if (reg & CAR_BOND_OUT_V_CPU_G)
			return -ENXIO;

		/* Keep G CPU clock policy set by upper layer, with the
		   exception of the transition via LP1 */
		if (flags & TEGRA_POWER_SDRAM_SELFREFRESH) {
			/* In LP1 power mode come up on CLKM (oscillator) */
			CclkBurstPolicy = readl(CAR_CCLKG_BURST_POLICY);
			CclkBurstPolicy |= ~0xF;
			SuperCclkDivier = 0;

			writel(CclkBurstPolicy, CAR_CCLKG_BURST_POLICY);
			writel(SuperCclkDivier, CAR_SUPER_CCLKG_DIVIDER);
		}

		/* Hold G CPUs 1-3 in reset after the switch */
		reg = CPU_RESET(1) | CPU_RESET(2) | CPU_RESET(3);
		writel(reg, CAR_RST_CPUG_CMPLX_SET);

		/* Take G CPU 0 out of reset after the switch */
		reg = CPU_RESET(0);
		writel(reg, CAR_RST_CPUG_CMPLX_CLR);

		/* Disable the clocks on G CPUs 1-3 after the switch */
		reg = CPU_CLOCK(1) | CPU_CLOCK(2) | CPU_CLOCK(3);
		writel(reg, CAR_CLK_CPUG_CMPLX_SET);

		/* Enable the clock on G CPU 0 after the switch */
		reg = CPU_CLOCK(0);
		writel(reg, CAR_CLK_CPUG_CMPLX_CLR);

		/* Enable the G CPU complex clock after the switch */
		reg = CAR_CLK_ENB_V_CPU_G;
		writel(reg, CAR_CLK_ENB_V_SET);
	}
	/* Switching to LP? */
	else if (flags & TEGRA_POWER_CLUSTER_LP) {
		/* Does the LP CPU exist? */
		if (reg & CAR_BOND_OUT_V_CPU_LP)
			return -ENXIO;

		/* Keep LP CPU clock policy set by upper layer, with the
		   exception of the transition via LP1 */
		if (flags & TEGRA_POWER_SDRAM_SELFREFRESH) {
			/* In LP1 power mode come up on CLKM (oscillator) */
			CclkBurstPolicy = readl(CAR_CCLKLP_BURST_POLICY);
			CclkBurstPolicy |= ~0xF;
			SuperCclkDivier = 0;

			writel(CclkBurstPolicy, CAR_CCLKLP_BURST_POLICY);
			writel(SuperCclkDivier, CAR_SUPER_CCLKLP_DIVIDER);
		}

		/* Take the LP CPU ut of reset after the switch */
		reg = CPU_RESET(0);
		writel(reg, CAR_RST_CPULP_CMPLX_CLR);

		/* Enable the clock on the LP CPU after the switch */
		reg = CPU_CLOCK(0);
		writel(reg, CAR_CLK_CPULP_CMPLX_CLR);

		/* Enable the LP CPU complex clock after the switch */
		reg = CAR_CLK_ENB_V_CPU_LP;
		writel(reg, CAR_CLK_ENB_V_SET);
	}

	return 0;
}

void tegra_cluster_switch_prolog(unsigned int flags)
{
	unsigned int target_cluster = flags & TEGRA_POWER_CLUSTER_MASK;
	unsigned int current_cluster = is_lp_cluster()
					? TEGRA_POWER_CLUSTER_LP
					: TEGRA_POWER_CLUSTER_G;
	u32 reg;

	/* Read the flow controler CSR register and clear the CPU switch
	   and immediate flags. If an actual CPU switch is to be performed,
	   re-write the CSR register with the desired values. */
	reg = readl(FLOW_CTRL_CPUx_CSR(0));
	reg &= ~(FLOW_CTRL_CPU_CSR_IMMEDIATE_WAKE |
		 FLOW_CTRL_CPU_CSR_SWITCH_CLUSTER);

	/* Program flow controller for immediate wake if requested */
	if (flags & TEGRA_POWER_CLUSTER_IMMEDIATE)
		reg |= FLOW_CTRL_CPU_CSR_IMMEDIATE_WAKE;

	/* Do nothing if no switch actions requested */
	if (!target_cluster)
		goto done;

	if ((current_cluster != target_cluster) ||
		(flags & TEGRA_POWER_CLUSTER_FORCE)) {
		if (current_cluster != target_cluster) {
			// Set up the clocks for the target CPU.
			if (cluster_switch_prolog_clock(flags)) {
				/* The target CPU does not exist */
				goto done;
			}

			/* Set up the flow controller to switch CPUs. */
			reg |= FLOW_CTRL_CPU_CSR_SWITCH_CLUSTER;
		}
	}

done:
	writel(reg, FLOW_CTRL_CPUx_CSR(0));
}

static void cluster_switch_epilog_gic(void)
{
	/* Nothing to do if currently running on the LP CPU. */
	if (is_lp_cluster())
		return;

	/* Reprogram the interrupt affinity because the on the LP CPU,
	   the interrupt distributor affinity regsiters are stubbed out
	   by ARM (reads as zero, writes ignored). So when the LP CPU
	   context save code runs, the affinity registers will read
	   as all zero. This causes all interrupts to be effectively
	   disabled when back on the G CPU because they aren't routable
	   to any CPU. See bug 667720 for details. */
	tegra_irq_affinity_to_cpu0();
}

void tegra_cluster_switch_epilog(unsigned int flags)
{
	u32 reg;

	/* Make sure the switch and immediate flags are cleared in
	   the flow controller to prevent undesirable side-effects
	   for future users of the flow controller. */
	reg = readl(FLOW_CTRL_CPUx_CSR(0));
	reg &= ~(FLOW_CTRL_CPU_CSR_IMMEDIATE_WAKE |
		 FLOW_CTRL_CPU_CSR_SWITCH_CLUSTER);
	writel(reg, FLOW_CTRL_CPUx_CSR(0));

	/* Perform post-switch clean-up of the interrupt distributor */
	cluster_switch_epilog_gic();

	#if DEBUG_CLUSTER_SWITCH
	if (tegra_cluster_debug) {
		/* FIXME: clock functions below are taking mutex */
		struct clk *c = tegra_get_clock_by_name(
			is_lp_cluster() ? "cpu_lp" : "cpu_g");
		DEBUG_CLUSTER(("%s: %s freq %lu\r\n", __func__,
			is_lp_cluster() ? "LP" : "G", clk_get_rate(c)));
	}
	#endif
}

int tegra_cluster_control(unsigned int us, unsigned int flags)
{
	static ktime_t last_g2lp;

	unsigned int target_cluster = flags & TEGRA_POWER_CLUSTER_MASK;
	unsigned int current_cluster = is_lp_cluster()
					? TEGRA_POWER_CLUSTER_LP
					: TEGRA_POWER_CLUSTER_G;
	unsigned long irq_flags;

	if ((target_cluster == TEGRA_POWER_CLUSTER_MASK) || !target_cluster)
		return -EINVAL;

	if (num_online_cpus() > 1)
		return -EBUSY;

	if ((current_cluster == target_cluster)
	&& !(flags & TEGRA_POWER_CLUSTER_FORCE))
		return -EEXIST;

	if (target_cluster == TEGRA_POWER_CLUSTER_G)
		if (!is_g_cluster_present())
			return -EPERM;

	trace_power_start(POWER_PSTATE, target_cluster, 0);

	if (flags & TEGRA_POWER_CLUSTER_IMMEDIATE)
		us = 0;

	if ((current_cluster != target_cluster) && (!timekeeping_suspended)) {
		ktime_t now = ktime_get();
		if (target_cluster == TEGRA_POWER_CLUSTER_G) {
			s64 t = ktime_to_us(ktime_sub(now, last_g2lp));
			s64 t_off = tegra_cpu_power_off_time();
			if (t_off > t)
				udelay((unsigned int)(t_off - t));

			tegra_dvfs_rail_on(tegra_cpu_rail, now);

		} else {
			last_g2lp = now;
			tegra_dvfs_rail_off(tegra_cpu_rail, now);
		}
	}

	DEBUG_CLUSTER(("%s(LP%d): %s->%s %s %s %d\r\n", __func__,
		(flags & TEGRA_POWER_SDRAM_SELFREFRESH) ? 1 : 2,
		is_lp_cluster() ? "LP" : "G",
		(target_cluster == TEGRA_POWER_CLUSTER_G) ? "G" : "LP",
		(flags & TEGRA_POWER_CLUSTER_IMMEDIATE) ? "immediate" : "",
		(flags & TEGRA_POWER_CLUSTER_FORCE) ? "force" : "",
	        us));

	local_irq_save(irq_flags);
#ifdef CONFIG_PM
	if (flags & TEGRA_POWER_SDRAM_SELFREFRESH) {
		if (us)
			tegra_lp2_set_trigger(us);

		tegra_suspend_dram(false, flags);

		if (us)
			tegra_lp2_set_trigger(0);
	} else
#endif
		tegra_suspend_lp2(us, flags);
	local_irq_restore(irq_flags);

	DEBUG_CLUSTER(("%s: %s\r\n", __func__, is_lp_cluster() ? "LP" : "G"));

	return 0;
}

#ifdef CONFIG_PM
void __init lp0_suspend_init(void)
{
	/* Nothing to do for Tegra3 */
}
#endif


#define NUM_WAKE_EVENTS 39

static int tegra_wake_event_irq[NUM_WAKE_EVENTS] = {
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PO5),	/* wake0 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PV1),	/* wake1 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PL1),	/* wake2 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PB6),	/* wake3 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PN7),	/* wake4 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PBB6),	/* wake5 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PU5),	/* wake6 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PU6),	/* wake7 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PC7),	/* wake8 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS2),	/* wake9 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PAA1),	/* wake10 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PW3),	/* wake11 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PW2),	/* wake12 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PY6),	/* wake13 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PDD3),	/* wake14 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PJ2),	/* wake15 */
	INT_RTC,				/* wake16 */
	INT_KBC,				/* wake17 */
	INT_EXTERNAL_PMU,			/* wake18 */
	-EINVAL, /* TEGRA_USB1_VBUS, */		/* wake19 */
	-EINVAL, /* TEGRA_USB2_VBUS, */		/* wake20 */
	-EINVAL, /* TEGRA_USB1_ID, */		/* wake21 */
	-EINVAL, /* TEGRA_USB2_ID, */		/* wake22 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PI5),	/* wake23 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PV0),	/* wake24 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS4),	/* wake25 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS5),	/* wake26 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS0),	/* wake27 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS6),	/* wake28 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PS7),	/* wake29 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PN2),	/* wake30 */
	-EINVAL, /* not used */			/* wake31 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PO4),	/* wake32 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PJ0),	/* wake33 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PK2),	/* wake34 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PI6),	/* wake35 */
	TEGRA_GPIO_TO_IRQ(TEGRA_GPIO_PBB1),	/* wake36 */
	-EINVAL, /* TEGRA_USB3_VBUS, */		/* wake37 */
	-EINVAL, /* TEGRA_USB3_ID, */		/* wake38 */
};

int tegra_irq_to_wake(int irq)
{
	int i;
	for (i = 0; i < NUM_WAKE_EVENTS; i++)
		if (tegra_wake_event_irq[i] == irq)
			return i;

	return -EINVAL;
}

int tegra_wake_to_irq(int wake)
{
	if (wake < 0)
		return -EINVAL;

	if (wake >= NUM_WAKE_EVENTS)
		return -EINVAL;

	return tegra_wake_event_irq[wake];
}

void tegra_lp0_suspend_mc(void)
{
	/* Since memory frequency after LP0 is restored to boot rate,
	   mc timing is saved during init, not on entry to LP0. Keep
	   this hook just in case, anyway */
}

void tegra_lp0_resume_mc(void)
{
	tegra_mc_timing_restore();
}

void tegra_lp0_cpu_mode(bool enter)
{
	static bool entered_on_g = false;
	unsigned int flags;

	if (enter)
		entered_on_g = !is_lp_cluster();

	if (entered_on_g) {
		flags = enter ? TEGRA_POWER_CLUSTER_LP : TEGRA_POWER_CLUSTER_G;
		flags |= TEGRA_POWER_CLUSTER_IMMEDIATE;
		tegra_cluster_control(0, flags);
	}
}