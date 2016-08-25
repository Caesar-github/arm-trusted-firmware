/*
 * Copyright (c) 2016, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch_helpers.h>
#include <assert.h>
#include <bakery_lock.h>
#include <debug.h>
#include <delay_timer.h>
#include <errno.h>
#include <mmio.h>
#include <platform.h>
#include <platform_def.h>
#include <plat_private.h>
#include <rk3399_def.h>
#include <pmu_sram.h>
#include <soc.h>
#include <pmu.h>
#include <pmu_com.h>
#include <dmc.h>
#include <console.h>
static struct psram_data_t *psram_sleep_cfg =
	(struct psram_data_t *)PSRAM_DT_BASE;

static uint32_t cpu_warm_boot_addr;

/*
 * There are two ways to powering on or off on core.
 * 1) Control it power domain into on or off in PMU_PWRDN_CON reg,
 *    it is core_pwr_pd mode
 * 2) Enable the core power manage in PMU_CORE_PM_CON reg,
 *     then, if the core enter into wfi, it power domain will be
 *     powered off automatically. it is core_pwr_wfi or core_pwr_wfi_int mode
 * so we need core_pm_cfg_info to distinguish which method be used now.
 */

static uint32_t core_pm_cfg_info[PLATFORM_CORE_COUNT]
#if USE_COHERENT_MEM
__attribute__ ((section("tzfw_coherent_mem")))
#endif
;/* coheront */

static void pmu_bus_idle_req(uint32_t bus, uint32_t idle)
{
	uint32_t bus_id = BIT(bus);
	uint32_t bus_req;
	uint32_t wait_cnt = 0;
	uint32_t bus_state, bus_ack;

	if (idle)
		bus_req = BIT(bus);
	else
		bus_req = idle;

	bus_state = bus_req;
	bus_ack = bus_req;

	mmio_clrsetbits_32(PMU_BASE + PMU_BUS_IDLE_REQ, bus_id, bus_req);

	while (((mmio_read_32(PMU_BASE + PMU_BUS_IDLE_ST) & bus_id)
	       != bus_state) &&  ((mmio_read_32(PMU_BASE + PMU_BUS_IDLE_ACK) &
	       bus_id) != bus_ack)) {
		wait_cnt++;
		if (!(wait_cnt % 1000)) {
			INFO("%s:st=%x(%x)\n", __func__,
			     mmio_read_32(PMU_BASE + PMU_BUS_IDLE_ST),
			     bus_state);
			INFO("%s:st=%x(%x)\n", __func__,
			     mmio_read_32(PMU_BASE + PMU_BUS_IDLE_ACK),
			     bus_ack);
		}
	}
}

struct pmu_slpdata_s pmu_slpdata;

static void qos_save(void)
{
	if (pmu_power_domain_st(PD_GPU) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.gpu_qos, GPU);
	if (pmu_power_domain_st(PD_ISP0) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.isp0_m0_qos, ISP0_M0);
		RESTORE_QOS(pmu_slpdata.isp0_m1_qos, ISP0_M1);
	}
	if (pmu_power_domain_st(PD_ISP1) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.isp1_m0_qos, ISP1_M0);
		RESTORE_QOS(pmu_slpdata.isp1_m1_qos, ISP1_M1);
	}
	if (pmu_power_domain_st(PD_VO) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.vop_big_r, VOP_BIG_R);
		RESTORE_QOS(pmu_slpdata.vop_big_w, VOP_BIG_W);
		RESTORE_QOS(pmu_slpdata.vop_little, VOP_LITTLE);
	}
	if (pmu_power_domain_st(PD_HDCP) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.hdcp_qos, HDCP);
	if (pmu_power_domain_st(PD_GMAC) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.gmac_qos, GMAC);
	if (pmu_power_domain_st(PD_CCI) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.cci_m0_qos, CCI_M0);
		RESTORE_QOS(pmu_slpdata.cci_m1_qos, CCI_M1);
	}
	if (pmu_power_domain_st(PD_SD) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.sdmmc_qos, SDMMC);
	if (pmu_power_domain_st(PD_EMMC) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.emmc_qos, EMMC);
	if (pmu_power_domain_st(PD_SDIOAUDIO) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.sdio_qos, SDIO);
	if (pmu_power_domain_st(PD_GIC) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.gic_qos, GIC);
	if (pmu_power_domain_st(PD_RGA) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.rga_r_qos, RGA_R);
		RESTORE_QOS(pmu_slpdata.rga_w_qos, RGA_W);
	}
	if (pmu_power_domain_st(PD_IEP) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.iep_qos, IEP);
	if (pmu_power_domain_st(PD_USB3) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.usb_otg0_qos, USB_OTG0);
		RESTORE_QOS(pmu_slpdata.usb_otg1_qos, USB_OTG1);
	}
	if (pmu_power_domain_st(PD_PERIHP) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.usb_host0_qos, USB_HOST0);
		RESTORE_QOS(pmu_slpdata.usb_host1_qos, USB_HOST1);
		RESTORE_QOS(pmu_slpdata.perihp_nsp_qos, PERIHP_NSP);
	}
	if (pmu_power_domain_st(PD_PERILP) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.dmac0_qos, DMAC0);
		RESTORE_QOS(pmu_slpdata.dmac1_qos, DMAC1);
		RESTORE_QOS(pmu_slpdata.dcf_qos, DCF);
		RESTORE_QOS(pmu_slpdata.crypto0_qos, CRYPTO0);
		RESTORE_QOS(pmu_slpdata.crypto1_qos, CRYPTO1);
		RESTORE_QOS(pmu_slpdata.perilp_nsp_qos, PERILP_NSP);
		RESTORE_QOS(pmu_slpdata.perilpslv_nsp_qos, PERILPSLV_NSP);
		RESTORE_QOS(pmu_slpdata.peri_cm1_qos, PERI_CM1);
	}
	if (pmu_power_domain_st(PD_VDU) == pmu_pd_on)
		RESTORE_QOS(pmu_slpdata.video_m0_qos, VIDEO_M0);
	if (pmu_power_domain_st(PD_VCODEC) == pmu_pd_on) {
		RESTORE_QOS(pmu_slpdata.video_m1_r_qos, VIDEO_M1_R);
		RESTORE_QOS(pmu_slpdata.video_m1_w_qos, VIDEO_M1_W);
	}
}

static void qos_restore(void)
{
	if (pmu_power_domain_st(PD_GPU) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.gpu_qos, GPU);
	if (pmu_power_domain_st(PD_ISP0) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.isp0_m0_qos, ISP0_M0);
		SAVE_QOS(pmu_slpdata.isp0_m1_qos, ISP0_M1);
	}
	if (pmu_power_domain_st(PD_ISP1) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.isp1_m0_qos, ISP1_M0);
		SAVE_QOS(pmu_slpdata.isp1_m1_qos, ISP1_M1);
	}
	if (pmu_power_domain_st(PD_VO) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.vop_big_r, VOP_BIG_R);
		SAVE_QOS(pmu_slpdata.vop_big_w, VOP_BIG_W);
		SAVE_QOS(pmu_slpdata.vop_little, VOP_LITTLE);
	}
	if (pmu_power_domain_st(PD_HDCP) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.hdcp_qos, HDCP);
	if (pmu_power_domain_st(PD_GMAC) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.gmac_qos, GMAC);
	if (pmu_power_domain_st(PD_CCI) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.cci_m0_qos, CCI_M0);
		SAVE_QOS(pmu_slpdata.cci_m1_qos, CCI_M1);
	}
	if (pmu_power_domain_st(PD_SD) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.sdmmc_qos, SDMMC);
	if (pmu_power_domain_st(PD_EMMC) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.emmc_qos, EMMC);
	if (pmu_power_domain_st(PD_SDIOAUDIO) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.sdio_qos, SDIO);
	if (pmu_power_domain_st(PD_GIC) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.gic_qos, GIC);
	if (pmu_power_domain_st(PD_RGA) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.rga_r_qos, RGA_R);
		SAVE_QOS(pmu_slpdata.rga_w_qos, RGA_W);
	}
	if (pmu_power_domain_st(PD_IEP) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.iep_qos, IEP);
	if (pmu_power_domain_st(PD_USB3) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.usb_otg0_qos, USB_OTG0);
		SAVE_QOS(pmu_slpdata.usb_otg1_qos, USB_OTG1);
	}
	if (pmu_power_domain_st(PD_PERIHP) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.usb_host0_qos, USB_HOST0);
		SAVE_QOS(pmu_slpdata.usb_host1_qos, USB_HOST1);
		SAVE_QOS(pmu_slpdata.perihp_nsp_qos, PERIHP_NSP);
	}
	if (pmu_power_domain_st(PD_PERILP) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.dmac0_qos, DMAC0);
		SAVE_QOS(pmu_slpdata.dmac1_qos, DMAC1);
		SAVE_QOS(pmu_slpdata.dcf_qos, DCF);
		SAVE_QOS(pmu_slpdata.crypto0_qos, CRYPTO0);
		SAVE_QOS(pmu_slpdata.crypto1_qos, CRYPTO1);
		SAVE_QOS(pmu_slpdata.perilp_nsp_qos, PERILP_NSP);
		SAVE_QOS(pmu_slpdata.perilpslv_nsp_qos, PERILPSLV_NSP);
		SAVE_QOS(pmu_slpdata.peri_cm1_qos, PERI_CM1);
	}
	if (pmu_power_domain_st(PD_VDU) == pmu_pd_on)
		SAVE_QOS(pmu_slpdata.video_m0_qos, VIDEO_M0);
	if (pmu_power_domain_st(PD_VDU) == pmu_pd_on) {
		SAVE_QOS(pmu_slpdata.video_m1_r_qos, VIDEO_M1_R);
		SAVE_QOS(pmu_slpdata.video_m1_w_qos, VIDEO_M1_W);
	}
}

static int pmu_set_power_domain(uint32_t pd_id, uint32_t pd_state)
{
	if (pmu_power_domain_st(pd_id) == pd_state)
		goto out;

	if (pd_state == pmu_pd_off) {
		switch (pd_id) {
		case PD_GPU:
			pmu_bus_idle_req(BUS_ID_GPU, BUS_IDLE);
		break;
		case PD_VIO:
			pmu_bus_idle_req(BUS_ID_VIO, BUS_IDLE);
		break;
		case PD_ISP0:
			pmu_bus_idle_req(BUS_ID_ISP0, BUS_IDLE);
		break;
		case PD_ISP1:
			pmu_bus_idle_req(BUS_ID_ISP1, BUS_IDLE);
		break;
		case PD_VO:
			pmu_bus_idle_req(BUS_ID_VOPB, BUS_IDLE);
			pmu_bus_idle_req(BUS_ID_VOPL, BUS_IDLE);
		break;
		case PD_HDCP:
			pmu_bus_idle_req(BUS_ID_HDCP, BUS_IDLE);
		break;
		case PD_TCPD0:
		break;
		case PD_TCPD1:
		break;
		case PD_GMAC:
			pmu_bus_idle_req(BUS_ID_GMAC, BUS_IDLE);
		break;
		case PD_CCI:
			pmu_bus_idle_req(BUS_ID_CCIM0, BUS_IDLE);
			pmu_bus_idle_req(BUS_ID_CCIM1, BUS_IDLE);
		break;
		case PD_SD:
			pmu_bus_idle_req(BUS_ID_SD, BUS_IDLE);
		break;
		case PD_EMMC:
			pmu_bus_idle_req(BUS_ID_EMMC, BUS_IDLE);
		break;
		case PD_EDP:
			pmu_bus_idle_req(BUS_ID_EDP, BUS_IDLE);
		break;
		case PD_SDIOAUDIO:
			pmu_bus_idle_req(BUS_ID_SDIOAUDIO, BUS_IDLE);
		break;
		case PD_GIC:
			pmu_bus_idle_req(BUS_ID_GIC, BUS_IDLE);
		break;
		case PD_RGA:
			pmu_bus_idle_req(BUS_ID_RGA, BUS_IDLE);
		break;
		case PD_VCODEC:
			pmu_bus_idle_req(BUS_ID_VCODEC, BUS_IDLE);
		break;
		case PD_VDU:
			pmu_bus_idle_req(BUS_ID_VDU, BUS_IDLE);
		break;
		case PD_IEP:
			pmu_bus_idle_req(BUS_ID_IEP, BUS_IDLE);
		break;
		case PD_USB3:
			pmu_bus_idle_req(BUS_ID_USB3, BUS_IDLE);
		break;
		case PD_PERIHP:
			pmu_bus_idle_req(BUS_ID_PERIHP, BUS_IDLE);
		break;
		default:
		break;
		}
	}

	pmu_power_domain_ctr(pd_id, pd_state);

	if (pd_state == pmu_pd_on) {
		switch (pd_id) {
		case PD_GPU:
			pmu_bus_idle_req(BUS_ID_GPU, BUS_ACTIVE);
		break;
		case PD_VIO:
		break;
		case PD_ISP0:
			pmu_bus_idle_req(BUS_ID_ISP0, BUS_ACTIVE);
		break;
		case PD_ISP1:
			pmu_bus_idle_req(BUS_ID_ISP1, BUS_ACTIVE);
		break;
		case PD_VO:
			pmu_bus_idle_req(BUS_ID_VOPB, BUS_ACTIVE);
			pmu_bus_idle_req(BUS_ID_VOPL, BUS_ACTIVE);
		break;
		case PD_HDCP:
			pmu_bus_idle_req(BUS_ID_HDCP, BUS_ACTIVE);
		break;
		case PD_TCPD0:
		break;
		case PD_TCPD1:
		break;
		case PD_SD:
			pmu_bus_idle_req(BUS_ID_SD, BUS_ACTIVE);
		break;
		case PD_GMAC:
			pmu_bus_idle_req(BUS_ID_GMAC, BUS_ACTIVE);
		break;
		case PD_CCI:
			pmu_bus_idle_req(BUS_ID_CCIM0, BUS_ACTIVE);
			pmu_bus_idle_req(BUS_ID_CCIM1, BUS_ACTIVE);
		break;
		case PD_EMMC:
			pmu_bus_idle_req(BUS_ID_EMMC, BUS_ACTIVE);
		break;
		case PD_EDP:
			pmu_bus_idle_req(BUS_ID_EDP, BUS_ACTIVE);
		break;
		case PD_SDIOAUDIO:
			pmu_bus_idle_req(BUS_ID_SDIOAUDIO, BUS_ACTIVE);
		break;
		case PD_GIC:
			pmu_bus_idle_req(BUS_ID_GIC, BUS_ACTIVE);
		break;
		case PD_RGA:
			pmu_bus_idle_req(BUS_ID_RGA, BUS_ACTIVE);
		break;
		case PD_VCODEC:
			pmu_bus_idle_req(BUS_ID_VCODEC, BUS_ACTIVE);
		break;
		case PD_VDU:
			pmu_bus_idle_req(BUS_ID_VDU, BUS_ACTIVE);
		break;
		case PD_IEP:
			pmu_bus_idle_req(BUS_ID_IEP, BUS_ACTIVE);
		break;
		case PD_USB3:
			pmu_bus_idle_req(BUS_ID_USB3, BUS_ACTIVE);
		break;
		case PD_PERIHP:
			pmu_bus_idle_req(BUS_ID_PERIHP, BUS_ACTIVE);
		break;

		default:
		break;
		}
	}

out:
	return 0;
}

static uint32_t pmu_powerdomain_state;

static void pmu_power_domains_suspend(void)
{
	clk_gate_con_save();
	clk_gate_con_disable();
	qos_save();
	pmu_powerdomain_state = mmio_read_32(PMU_BASE + PMU_PWRDN_ST);
	pmu_set_power_domain(PD_GPU, pmu_pd_off);
	pmu_set_power_domain(PD_TCPD0, pmu_pd_off);
	pmu_set_power_domain(PD_TCPD1, pmu_pd_off);
	pmu_set_power_domain(PD_VO, pmu_pd_off);
	pmu_set_power_domain(PD_ISP0, pmu_pd_off);
	pmu_set_power_domain(PD_ISP1, pmu_pd_off);
	pmu_set_power_domain(PD_HDCP, pmu_pd_off);
	pmu_set_power_domain(PD_SDIOAUDIO, pmu_pd_off);
	pmu_set_power_domain(PD_GMAC, pmu_pd_off);
	pmu_set_power_domain(PD_EDP, pmu_pd_off);
	pmu_set_power_domain(PD_IEP, pmu_pd_off);
	pmu_set_power_domain(PD_RGA, pmu_pd_off);
	pmu_set_power_domain(PD_VCODEC, pmu_pd_off);
	pmu_set_power_domain(PD_VDU, pmu_pd_off);
	clk_gate_con_restore();
}

static void pmu_power_domains_resume(void)
{
	clk_gate_con_save();
	clk_gate_con_disable();
	if (!(pmu_powerdomain_state & BIT(PD_VDU)))
		pmu_set_power_domain(PD_VDU, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_VCODEC)))
		pmu_set_power_domain(PD_VCODEC, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_RGA)))
		pmu_set_power_domain(PD_RGA, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_IEP)))
		pmu_set_power_domain(PD_IEP, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_EDP)))
		pmu_set_power_domain(PD_EDP, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_GMAC)))
		pmu_set_power_domain(PD_GMAC, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_SDIOAUDIO)))
		pmu_set_power_domain(PD_SDIOAUDIO, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_HDCP)))
		pmu_set_power_domain(PD_HDCP, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_ISP1)))
		pmu_set_power_domain(PD_ISP1, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_ISP0)))
		pmu_set_power_domain(PD_ISP0, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_VO)))
		pmu_set_power_domain(PD_VO, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_TCPD1)))
		pmu_set_power_domain(PD_TCPD1, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_TCPD0)))
		pmu_set_power_domain(PD_TCPD0, pmu_pd_on);
	if (!(pmu_powerdomain_state & BIT(PD_GPU)))
		pmu_set_power_domain(PD_GPU, pmu_pd_on);
	qos_restore();
	clk_gate_con_restore();
}

void rk3399_flash_l2_b(void)
{
	uint32_t wait_cnt = 0;

	mmio_setbits_32(PMU_BASE + PMU_SFT_CON, BIT(L2_FLUSH_REQ_CLUSTER_B));
	dsb();

	while (!(mmio_read_32(PMU_BASE + PMU_CORE_PWR_ST) &
		 BIT(L2_FLUSHDONE_CLUSTER_B))) {
		wait_cnt++;
		if (!(wait_cnt % MAX_WAIT_CONUT))
			WARN("%s:reg %x,wait\n", __func__,
			     mmio_read_32(PMU_BASE + PMU_CORE_PWR_ST));
	}

	mmio_clrbits_32(PMU_BASE + PMU_SFT_CON, BIT(L2_FLUSH_REQ_CLUSTER_B));
}

static void pmu_scu_b_pwrdn(void)
{
	uint32_t wait_cnt = 0;

	if ((mmio_read_32(PMU_BASE + PMU_PWRDN_ST) &
	     (BIT(PMU_A72_B0_PWRDWN_ST) | BIT(PMU_A72_B1_PWRDWN_ST))) !=
	     (BIT(PMU_A72_B0_PWRDWN_ST) | BIT(PMU_A72_B1_PWRDWN_ST))) {
		ERROR("%s: not all cpus is off\n", __func__);
		return;
	}

	rk3399_flash_l2_b();

	mmio_setbits_32(PMU_BASE + PMU_SFT_CON, BIT(ACINACTM_CLUSTER_B_CFG));

	while (!(mmio_read_32(PMU_BASE + PMU_CORE_PWR_ST) &
		 BIT(STANDBY_BY_WFIL2_CLUSTER_B))) {
		wait_cnt++;
		if (!(wait_cnt % MAX_WAIT_CONUT))
			ERROR("%s:wait cluster-b l2(%x)\n", __func__,
			      mmio_read_32(PMU_BASE + PMU_CORE_PWR_ST));
	}
}

static void pmu_scu_b_pwrup(void)
{
	mmio_clrbits_32(PMU_BASE + PMU_SFT_CON, BIT(ACINACTM_CLUSTER_B_CFG));
}
extern uint32_t __bl31_ssram_code, __bl31_sram_lma_start;
extern uint32_t __bl31_esram_code;
extern uint32_t __bl31_ssram_data, __bl31_sram_data_lma_start;
extern uint32_t __bl31_esram_data;
void  (*pmu_ddr_suspend)(void);
void  (*pmu_ddr_resume)(void);
void plat_rockchip_pmusram_prepare(void)
{
	uint32_t *sram_dst, *sram_src;
	size_t sram_size = 2;

	/*
	 * pmu sram code and data prepare
	 */
	sram_dst = (uint32_t *)PMUSRAM_BASE;
	sram_src = (uint32_t *)&pmu_cpuson_entrypoint_start;
	sram_size = (uint32_t *)&pmu_cpuson_entrypoint_end -
		    (uint32_t *)sram_src;

	u32_align_cpy(sram_dst, sram_src, sram_size);

	sram_dst = (uint32_t *)(PMUSRAM_BASE + SIZE_K(1));
	sram_src = (uint32_t *)&pmu_ddr_test_start;
	sram_size = (uint32_t *)&pmu_ddr_test_end -
		    (uint32_t *)sram_src;
	u32_align_cpy(sram_dst, sram_src, sram_size);
	pmu_ddr_suspend = (void(*) ())(PMUSRAM_BASE + 1024);
	sram_dst = (uint32_t *)(PMUSRAM_BASE + 1280);
	sram_src = (uint32_t *)&pmu_ddr_resume_satrt;
	sram_size = (uint32_t *)&pmu_ddr_resume_end -
		    (uint32_t *)sram_src;
	u32_align_cpy(sram_dst, sram_src, sram_size);
	pmu_ddr_resume = (void(*) ())(PMUSRAM_BASE + 1280);

	psram_sleep_cfg->sp = PSRAM_DT_BASE;
	sram_dst = &__bl31_ssram_code;
	sram_src = &__bl31_sram_lma_start;
	sram_size = (uint32_t *)&__bl31_esram_code - sram_dst;

	u32_align_cpy(sram_dst, sram_src, sram_size);
	sram_dst = &__bl31_ssram_data;
	sram_src = &__bl31_sram_data_lma_start;
	sram_size = (uint32_t *)&__bl31_esram_data - sram_dst;
	u32_align_cpy(sram_dst, sram_src, sram_size);
}

static inline uint32_t get_cpus_pwr_domain_cfg_info(uint32_t cpu_id)
{
	return core_pm_cfg_info[cpu_id];
}

static inline void set_cpus_pwr_domain_cfg_info(uint32_t cpu_id, uint32_t value)
{
	core_pm_cfg_info[cpu_id] = value;
#if !USE_COHERENT_MEM
	flush_dcache_range((uintptr_t)&core_pm_cfg_info[cpu_id],
			   sizeof(uint32_t));
#endif
}

static int cpus_power_domain_on(uint32_t cpu_id)
{
	uint32_t cfg_info;
	uint32_t cpu_pd = PD_CPUL0 + cpu_id;
	/*
	  * There are two ways to powering on or off on core.
	  * 1) Control it power domain into on or off in PMU_PWRDN_CON reg
	  * 2) Enable the core power manage in PMU_CORE_PM_CON reg,
	  *     then, if the core enter into wfi, it power domain will be
	  *     powered off automatically.
	  */

	cfg_info = get_cpus_pwr_domain_cfg_info(cpu_id);

	if (cfg_info == core_pwr_pd) {
		/* disable core_pm cfg */
		mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id),
			      CORES_PM_DISABLE);
		/* if the cores have be on, power off it firstly */
		if (pmu_power_domain_st(cpu_pd) == pmu_pd_on) {
			mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id), 0);
			pmu_power_domain_ctr(cpu_pd, pmu_pd_off);
		}

		pmu_power_domain_ctr(cpu_pd, pmu_pd_on);
	} else {
		if (pmu_power_domain_st(cpu_pd) == pmu_pd_on) {
			WARN("%s: cpu%d is not in off,!\n", __func__, cpu_id);
			return -EINVAL;
		}

		mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id),
			      BIT(core_pm_sft_wakeup_en));
		dsb();
	}

	return 0;
}

static int cpus_power_domain_off(uint32_t cpu_id, uint32_t pd_cfg)
{
	uint32_t cpu_pd;
	uint32_t core_pm_value;

	cpu_pd = PD_CPUL0 + cpu_id;
	if (pmu_power_domain_st(cpu_pd) == pmu_pd_off)
		return 0;

	if (pd_cfg == core_pwr_pd) {
		if (check_cpu_wfie(cpu_id, CKECK_WFEI_MSK))
			return -EINVAL;

		/* disable core_pm cfg */
		mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id),
			      CORES_PM_DISABLE);

		set_cpus_pwr_domain_cfg_info(cpu_id, pd_cfg);
		pmu_power_domain_ctr(cpu_pd, pmu_pd_off);
	} else {
		set_cpus_pwr_domain_cfg_info(cpu_id, pd_cfg);

		core_pm_value = BIT(core_pm_en);
		if (pd_cfg == core_pwr_wfi_int)
			core_pm_value |= BIT(core_pm_int_wakeup_en);
		mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id),
			      core_pm_value);
		dsb();
	}

	return 0;
}

static inline int clst_pwr_domain_suspend(plat_local_state_t lvl_state)
{
	uint32_t cpu_id = plat_my_core_pos();
	uint32_t pll_id, clst_st_msk, clst_st_chk_msk, pmu_st;

	assert(cpu_id < PLATFORM_CORE_COUNT);

	if (lvl_state == PLAT_MAX_RET_STATE  ||
	    lvl_state == PLAT_MAX_OFF_STATE) {
		if (cpu_id < PLATFORM_CLUSTER0_CORE_COUNT) {
			pll_id = ALPLL_ID;
			clst_st_msk = CLST_L_CPUS_MSK;
		} else {
			pll_id = ABPLL_ID;
			clst_st_msk = CLST_B_CPUS_MSK <<
				       PLATFORM_CLUSTER0_CORE_COUNT;
		}

		clst_st_chk_msk = clst_st_msk & ~(BIT(cpu_id));

		pmu_st = mmio_read_32(PMU_BASE + PMU_PWRDN_ST);

		pmu_st &= clst_st_msk;

		if (pmu_st == clst_st_chk_msk) {

			clst_warmboot_data[pll_id] = PMU_CLST_RET;

			pmu_st = mmio_read_32(PMU_BASE + PMU_PWRDN_ST);
			pmu_st &= clst_st_msk;
			if (pmu_st == clst_st_chk_msk)
				return 0;
			/*
			 * it is mean that others cpu is up again,
			 * we must resume the cfg at once.
			 */
			mmio_write_32(CRU_BASE + CRU_PLL_CON(pll_id, 3),
				      PLL_NOMAL_MODE);
			clst_warmboot_data[pll_id] = 0;
		}
	}

	return -1;
}

static int clst_pwr_domain_resume(plat_local_state_t lvl_state)
{
	uint32_t cpu_id = plat_my_core_pos();
	uint32_t pll_id, pll_st;

	assert(cpu_id < PLATFORM_CORE_COUNT);

	if (lvl_state == PLAT_MAX_RET_STATE ||
	    lvl_state == PLAT_MAX_OFF_STATE) {
		if (cpu_id < PLATFORM_CLUSTER0_CORE_COUNT)
			pll_id = ALPLL_ID;
		else
			pll_id = ABPLL_ID;

		pll_st = mmio_read_32(CRU_BASE + CRU_PLL_CON(pll_id, 3)) >>
				 PLL_MODE_SHIFT;

		if (pll_st != NORMAL_MODE) {
			WARN("%s: clst (%d) is in error mode (%d)\n",
			     __func__, pll_id, pll_st);
			return -1;
		}
	}

	return 0;
}

static void nonboot_cpus_off(void)
{
	uint32_t boot_cpu, cpu;

	boot_cpu = plat_my_core_pos();

	/* turn off noboot cpus */
	for (cpu = 0; cpu < PLATFORM_CORE_COUNT; cpu++) {
		if (cpu == boot_cpu)
			continue;
		cpus_power_domain_off(cpu, core_pwr_pd);
	}
}

static int cores_pwr_domain_on(unsigned long mpidr, uint64_t entrypoint)
{
	uint32_t cpu_id = plat_core_pos_by_mpidr(mpidr);

	assert(cpuson_flags[cpu_id] == 0);
	cpuson_flags[cpu_id] = PMU_CPU_HOTPLUG;
	cpuson_entry_point[cpu_id] = entrypoint;
	dsb();

	cpus_power_domain_on(cpu_id);

	return 0;
}

static int cores_pwr_domain_off(uint32_t lvl, plat_local_state_t lvl_state)
{
	uint32_t cpu_id = plat_my_core_pos();

	switch (lvl) {
	case MPIDR_AFFLVL0:
		cpus_power_domain_off(cpu_id, core_pwr_wfi);
		break;
	case MPIDR_AFFLVL1:
		clst_pwr_domain_suspend(lvl_state);
		break;
	default:
		break;
	}

	return 0;
}

static int cores_pwr_domain_suspend(void)
{
	uint32_t cpu_id = plat_my_core_pos();

	assert(cpuson_flags[cpu_id] == 0);
	cpuson_flags[cpu_id] = PMU_CPU_AUTO_PWRDN;
	cpuson_entry_point[cpu_id] = (uintptr_t)psci_entrypoint;
	dsb();

	cpus_power_domain_off(cpu_id, core_pwr_wfi_int);

	return 0;
}

static int hlvl_pwr_domain_suspend(uint32_t lvl, plat_local_state_t lvl_state)
{
	switch (lvl) {
	case MPIDR_AFFLVL1:
		clst_pwr_domain_suspend(lvl_state);
		break;
	default:
		break;
	}

	return 0;
}

static int cores_pwr_domain_on_finish(uint32_t lvl,
				      plat_local_state_t lvl_state)
{
	uint32_t cpu_id = plat_my_core_pos();

	switch (lvl) {
	case MPIDR_AFFLVL0:
		/* Disable core_pm */
		mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id),
			      CORES_PM_DISABLE);
		break;
	case MPIDR_AFFLVL1:
		clst_pwr_domain_resume(lvl_state);
		break;
	default:
		break;
	}

	return 0;
}

static int cores_pwr_domain_resume(void)
{
	uint32_t cpu_id = plat_my_core_pos();

	/* Disable core_pm */
	mmio_write_32(PMU_BASE + PMU_CORE_PM_CON(cpu_id), CORES_PM_DISABLE);

	return 0;
}

static int hlvl_pwr_domain_resume(uint32_t lvl, plat_local_state_t lvl_state)
{
	switch (lvl) {
	case MPIDR_AFFLVL1:
		clst_pwr_domain_resume(lvl_state);
	default:
		break;
	}

	return 0;
}

extern void sgrf_init(void);

uint32_t pwm2_iomux;
static void pwm_regulator_suspend(void)
{
	pwm2_iomux = mmio_read_32(PMUGRF_BASE + PMUGRF_GPIO1C_IOMUX);
	mmio_write_32(PMUGRF_BASE + PMUGRF_GPIO1C_IOMUX, 0x00c00000);
}
static void pwm_regulator_resume(void)
{
	mmio_write_32(PMUGRF_BASE + PMUGRF_GPIO1C_IOMUX,
		      0x00c00000 | pwm2_iomux);
}

static void pvtm_32k_enable(void)
{
	/* not gating pvtm*/
	mmio_write_32(PMUCRU_BASE + 0x0100, (0x10000 << 7));
	/*reset pvtm*/
	mmio_write_32(PMUCRU_BASE + 0x0114, (0x10001 << 11));
	udelay(5);
	mmio_write_32(PMUCRU_BASE + 0x0114, (0x10000 << 11));
	/*init pvtm*/
	mmio_write_32(PMUGRF_BASE + 0x0240, 0x00020002);
	udelay(1);
	mmio_write_32(PMUGRF_BASE + 0x0244, 0x00000200); /*0x0e244*/
	udelay(1);
	mmio_write_32(PMUGRF_BASE + 0x0240, 0xff090009);

	INFO("---xxxx,,0x%x\n", mmio_read_32(PMUGRF_BASE + 0x0240));
	/*suspend clock from internal, pvtm*/
	mmio_write_32(PMUGRF_BASE + 0x0180, 0x00010001);
}

uint32_t center_pd_enable = 1;
uint32_t pmu_debug_enable;
uint32_t debug_iomux;
uint32_t interrupt_debug = 1;
uint32_t debug_ddr_suspend;
uint32_t pll_suspend_enable;
#define PVTM_ENABLE		0

static void sys_slp_config(void)
{
	uint32_t slp_mode_cfg = 0;

	INFO("the debug info:\n");
	INFO("CENTER_PD: %d\n", center_pd_enable);
	INFO("PMU DEBUG: %d\n", pmu_debug_enable);
	INFO("interrupt DEBUG: %d\n", interrupt_debug);
	INFO("PVTM_ENABLE: 0x%x\n", PVTM_ENABLE);
	if (center_pd_enable) {
		set_abpll();
		pmu_ddr_suspend();
	}
	debug_iomux = mmio_read_32(PMUGRF_BASE);
	if (pmu_debug_enable) {
		mmio_write_32(PMUGRF_BASE, 0xfff0aaa0);
		mmio_write_32(PMUGRF_BASE + 0x040, 0xfff00000);
		INFO("PMUGRF - iomux: 0x%x\n", mmio_read_32(PMUGRF_BASE));
	}

	/*cci force wake up*/
	mmio_write_32(GRF_BASE + 0x0e210, WMSK_BIT(8));

	mmio_write_32(PMU_BASE + PMU_CCI500_CON,
		      BIT_WITH_WMSK(PMU_CLR_PREQ_CCI500_HW) |
		      BIT_WITH_WMSK(PMU_CLR_QREQ_CCI500_HW) |
		      BIT_WITH_WMSK(PMU_QGATING_CCI500_CFG));

	mmio_write_32(PMU_BASE + PMU_ADB400_CON,
		      BIT_WITH_WMSK(PMU_CLR_CORE_L_HW) |
		      BIT_WITH_WMSK(PMU_CLR_CORE_L_2GIC_HW) |
		      BIT_WITH_WMSK(PMU_CLR_GIC2_CORE_L_HW));

	mmio_write_32(PMUGRF_BASE + PMUGRF_GPIO1A_IOMUX,
		      BIT_WITH_WMSK(AP_PWROFF));

	slp_mode_cfg = BIT(PMU_PWR_MODE_EN) |
		       BIT(PMU_POWER_OFF_REQ_CFG) |
		       BIT(PMU_CPU0_PD_EN) |
		       BIT(PMU_L2_FLUSH_EN) |
		       BIT(PMU_L2_IDLE_EN) |
		       BIT(PMU_SCU_PD_EN) |
		       BIT(PMU_CCI_PD_EN) |
		       BIT(PMU_CLK_CORE_SRC_GATE_EN) |
		       BIT(PMU_ALIVE_USE_LF) |
		       BIT(PMU_SREF0_ENTER_EN) |
		       BIT(PMU_SREF1_ENTER_EN) |
		       BIT(PMU_DDRC0_GATING_EN) |
		       BIT(PMU_DDRC1_GATING_EN) |
		       BIT(PMU_DDRIO0_RET_EN) |
		       BIT(PMU_DDRIO1_RET_EN) |
		       BIT(PMU_DDRIO_RET_HW_DE_REQ) |
		       BIT(PMU_CENTER_PD_EN) |
		       BIT(PMU_PLL_PD_EN) |
		       BIT(PMU_CLK_CENTER_SRC_GATE_EN) |
		       BIT(PMU_OSC_DIS) |
		       BIT(PMU_PMU_USE_LF);

	mmio_setbits_32(PMU_BASE + PMU_WKUP_CFG4, BIT(PMU_PWM_WKUP_EN));
	mmio_setbits_32(PMU_BASE + PMU_WKUP_CFG4, BIT(PMU_GPIO_WKUP_EN));
	mmio_write_32(PMU_BASE + PMU_PWRMODE_CON, slp_mode_cfg);

	mmio_write_32(PMU_BASE + PMU_SCU_L_PWRDN_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_SCU_L_PWRUP_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_SCU_B_PWRDN_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_SCU_B_PWRUP_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_CENTER_PWRDN_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_CENTER_PWRUP_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_WAKEUP_RST_CLR_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_OSC_CNT, CYCL_32K_CNT_MS(5));
	mmio_write_32(PMU_BASE + PMU_DDRIO_PWRON_CNT, CYCL_32K_CNT_MS(2));
	mmio_write_32(PMU_BASE + PMU_PLLLOCK_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_PLLRST_CNT, CYCL_32K_CNT_MS(1));
	mmio_write_32(PMU_BASE + PMU_STABLE_CNT, CYCL_32K_CNT_MS(4));

	mmio_write_32(PMU_BASE + PMU_PLL_CON, PLL_PD_HW);
	mmio_write_32(PMUGRF_BASE + 0x120, 0x00030001);
#if PVTM_ENABLE
	pvtm_32k_enable();
#else
	if (0)
		pvtm_32k_enable();
	mmio_write_32(PMUGRF_BASE + PMUGRF_SOC_CON0, EXTERNAL_32K);
	mmio_write_32(PMUGRF_BASE, IOMUX_CLK_32K); /*32k iomux*/
#endif
}
static void sys_slp_unconfig(void)
{
	mmio_write_32(PMU_BASE + PMU_WKUP_CFG4, 0x00);
	mmio_write_32(PMU_BASE + PMU_PWRMODE_CON, 0x00);

}
static void set_hw_idle(uint32_t hw_idle)
{
	mmio_setbits_32(PMU_BASE + PMU_BUS_CLR, hw_idle);
}

static void clr_hw_idle(uint32_t hw_idle)
{
	mmio_clrbits_32(PMU_BASE + PMU_BUS_CLR, hw_idle);
}

uint32_t wakeup_count;
int skip_suspend;

static int sys_pwr_domain_suspend(void)
{
	uint32_t wait_cnt = 0;
	uint32_t status = 0;
	uint32_t wkup_status;

	wkup_status = mmio_read_32(PMU_BASE + PMU_WAKEUP_STATUS);
	mmio_write_32(PMU_BASE + PMU_WAKEUP_STATUS, wkup_status);

	pmu_power_domains_suspend();
	set_hw_idle(BIT(PMU_CLR_CENTER1) |
		    BIT(PMU_CLR_ALIVE) |
		    BIT(PMU_CLR_MSCH0) |
		    BIT(PMU_CLR_MSCH1) |
		    BIT(PMU_CLR_CCIM0) |
		    BIT(PMU_CLR_CCIM1) |
		    BIT(PMU_CLR_CENTER) |
		    BIT(PMU_CLR_PERILP) |
		    BIT(PMU_CLR_PMU));
	if (center_pd_enable)
		dmc_save();
	sys_slp_config();

	mmio_write_32(SGRF_BASE + SGRF_SOC_CON0_1(1),
		      (PMUSRAM_BASE >> CPU_BOOT_ADDR_ALIGN) |
		      CPU_BOOT_ADDR_WMASK);

	pmu_scu_b_pwrdn();

	mmio_write_32(PMU_BASE + PMU_ADB400_CON,
		      BIT_WITH_WMSK(PMU_PWRDWN_REQ_CORE_B_2GIC_SW) |
		      BIT_WITH_WMSK(PMU_PWRDWN_REQ_CORE_B_SW) |
		      BIT_WITH_WMSK(PMU_PWRDWN_REQ_GIC2_CORE_B_SW));
	dsb();
	status =
		BIT(PMU_PWRDWN_REQ_CORE_B_2GIC_SW_ST) |
		BIT(PMU_PWRDWN_REQ_CORE_B_SW_ST) |
		BIT(PMU_PWRDWN_REQ_GIC2_CORE_B_SW_ST);
	while ((mmio_read_32(PMU_BASE +
	       PMU_ADB400_ST) & status) != status) {
		wait_cnt++;
		if (!(wait_cnt % MAX_WAIT_CONUT)) {
			ERROR("%s:wait cluster-b l2(%x)\n", __func__,
			      mmio_read_32(PMU_BASE + PMU_ADB400_ST));
			panic();
		}
	}
	mmio_setbits_32(PMU_BASE + PMU_PWRDN_CON, BIT(PMU_SCU_B_PWRDWN_EN));

	INFO("the test count: %d\n", wakeup_count++);

	pwm_regulator_suspend();
	if (pll_suspend_enable) {
		plls_suspend();
		mmio_write_32(PMUCRU_BASE + 0x80, 0x001f0000);
		INFO("PMU_PCLK_div_con[4:0] = 0x%x\n",
		     mmio_read_32(PMUCRU_BASE + 0x80));
		INFO("PPLL CON0 : 0x%x\n", mmio_read_32(PMUCRU_BASE));
		INFO("PPLL CON1 : 0x%x\n", mmio_read_32(PMUCRU_BASE + 0x04));
		INFO("PPLL CON2 : 0x%x\n", mmio_read_32(PMUCRU_BASE + 0x08));
		INFO("PPLL CON3 : 0x%x\n", mmio_read_32(PMUCRU_BASE + 0x0c));
		INFO("PPLL CON4 : 0x%x\n", mmio_read_32(PMUCRU_BASE + 0x10));
		INFO("PMU_PCLK_div_con[4:0] = 0x%x\n",
		     mmio_read_32(PMUCRU_BASE + 0x80));
	}

	if ((interrupt_debug == 0x01) &&
	    mmio_read_32(PMU_BASE + PMU_WAKEUP_STATUS)) {
		INFO("can't sleep , the wake up interupt\n");
		skip_suspend = 1;
	}
	INFO("suspend end\n");
	return 0;
}

static void report_wakeup_source(void)
{
	uint32_t wkup_status;

	wkup_status =  mmio_read_32(PMU_BASE + PMU_WAKEUP_STATUS);
	INFO("RK3399 The wake up information:\n");
	INFO("wake up sattus: 0x%x\n",
	     mmio_read_32(PMU_BASE + PMU_WAKEUP_STATUS));
	if (wkup_status & BIT(0))
		INFO("CLUSTER L interrupt wakeup\n");
	if (wkup_status & BIT(1))
		INFO("CLUSTER B interrupt wakeup\n");
	if (wkup_status & BIT(2)) {
		INFO("GPIO interrupt wakeup\n");
		INFO("GPIO0: 0x%x\n", mmio_read_32(GPIO0_BASE + 0x040));
		INFO("GPIO1: 0x%x\n", mmio_read_32(GPIO1_BASE + 0x040));
	}
	if (wkup_status & BIT(3))
		INFO("SDIO interrupt wakeup\n");
	if (wkup_status & BIT(4))
		INFO("SDMMC interrupt wakeup\n");
	if (wkup_status & BIT(6))
		INFO("timer interrupt wakeup\n");
	if (wkup_status & BIT(7))
		INFO("USBDEV detect wakeup\n");
	if (wkup_status & BIT(8))
		INFO("M0 software interrupt wakeup\n");
	if (wkup_status & BIT(9))
		INFO("M0 wdt interrupt wakeup\n");
	if (wkup_status & BIT(10))
		INFO("timer out interrupt wakeup\n");
	if (wkup_status & BIT(11))
		INFO("PWM interrupt wakeup\n");
	if (wkup_status & BIT(13))
		INFO("pcie interrupt wakeup\n");
}

static int sys_pwr_domain_resume(void)
{
	uint32_t wait_cnt = 0;
	uint32_t status = 0;

	debug_ddr_suspend = 0;
	pwm_regulator_resume();

	console_init(PLAT_RK_UART_BASE, PLAT_RK_UART_CLOCK,
		     PLAT_RK_UART_BAUDRATE);
	sys_slp_unconfig();
	report_wakeup_source();
	console_uninit();
	mmio_write_32(SGRF_BASE + SGRF_SOC_CON0_1(1),
		      (cpu_warm_boot_addr >> CPU_BOOT_ADDR_ALIGN) |
		      CPU_BOOT_ADDR_WMASK);

	mmio_write_32(PMU_BASE + PMU_CCI500_CON,
		      WMSK_BIT(PMU_CLR_PREQ_CCI500_HW) |
		      WMSK_BIT(PMU_CLR_QREQ_CCI500_HW) |
		      WMSK_BIT(PMU_QGATING_CCI500_CFG));
	dsb();
	mmio_clrbits_32(PMU_BASE + PMU_PWRDN_CON,
			BIT(PMU_SCU_B_PWRDWN_EN));

	mmio_write_32(PMU_BASE + PMU_ADB400_CON,
		      WMSK_BIT(PMU_PWRDWN_REQ_CORE_B_2GIC_SW) |
		      WMSK_BIT(PMU_PWRDWN_REQ_CORE_B_SW) |
		      WMSK_BIT(PMU_PWRDWN_REQ_GIC2_CORE_B_SW) |
		      WMSK_BIT(PMU_CLR_CORE_L_HW) |
		      WMSK_BIT(PMU_CLR_CORE_L_2GIC_HW) |
		      WMSK_BIT(PMU_CLR_GIC2_CORE_L_HW));

	status = BIT(PMU_PWRDWN_REQ_CORE_B_2GIC_SW_ST) |
		BIT(PMU_PWRDWN_REQ_CORE_B_SW_ST) |
		BIT(PMU_PWRDWN_REQ_GIC2_CORE_B_SW_ST);

	while ((mmio_read_32(PMU_BASE +
	   PMU_ADB400_ST) & status)) {
		wait_cnt++;
		if (!(wait_cnt % MAX_WAIT_CONUT)) {
			ERROR("%s:wait cluster-b l2(%x)\n", __func__,
			      mmio_read_32(PMU_BASE + PMU_ADB400_ST));
			panic();
		}
	}

	pmu_scu_b_pwrup();

	pmu_power_domains_resume();
	if (center_pd_enable) {
		restore_dpll();
		pmu_ddr_resume();
		restore_abpll();
	}
	clr_hw_idle(BIT(PMU_CLR_CENTER1) |
				BIT(PMU_CLR_ALIVE) |
				BIT(PMU_CLR_MSCH0) |
				BIT(PMU_CLR_MSCH1) |
				BIT(PMU_CLR_CCIM0) |
				BIT(PMU_CLR_CCIM1) |
				BIT(PMU_CLR_CENTER) |
				BIT(PMU_CLR_PERILP) |
				BIT(PMU_CLR_PMU));

	plat_rockchip_gic_cpuif_enable();
	sgrf_init();
	return 0;
}

static struct rockchip_pm_ops_cb pm_ops = {
	.cores_pwr_dm_on = cores_pwr_domain_on,
	.cores_pwr_dm_off = cores_pwr_domain_off,
	.cores_pwr_dm_on_finish = cores_pwr_domain_on_finish,
	.cores_pwr_dm_suspend = cores_pwr_domain_suspend,
	.cores_pwr_dm_resume = cores_pwr_domain_resume,
	.hlvl_pwr_dm_suspend = hlvl_pwr_domain_suspend,
	.hlvl_pwr_dm_resume = hlvl_pwr_domain_resume,
	.sys_pwr_dm_suspend = sys_pwr_domain_suspend,
	.sys_pwr_dm_resume = sys_pwr_domain_resume,
	.sys_gbl_soft_reset = soc_global_soft_reset,
};

void plat_rockchip_pmu_init(void)
{
	uint32_t cpu;

	rockchip_pd_lock_init();
	plat_setup_rockchip_pm_ops(&pm_ops);

	/* register requires 32bits mode, switch it to 32 bits */
	cpu_warm_boot_addr = (uint64_t)platform_cpu_warmboot;

	for (cpu = 0; cpu < PLATFORM_CORE_COUNT; cpu++)
		cpuson_flags[cpu] = 0;

	for (cpu = 0; cpu < PLATFORM_CLUSTER_COUNT; cpu++)
		clst_warmboot_data[cpu] = 0;

	psram_sleep_cfg->ddr_func = (uint64_t)dmc_restore;
	psram_sleep_cfg->ddr_data = (uint64_t)&sdram_configs[0];
	if (center_pd_enable)
		psram_sleep_cfg->ddr_flag = 0x01;
	else
		psram_sleep_cfg->ddr_flag = 0x00;
	psram_sleep_cfg->boot_mpidr = read_mpidr_el1() & 0xffff;

	/* cpu boot from pmusram */
	mmio_write_32(SGRF_BASE + SGRF_SOC_CON0_1(1),
		      (cpu_warm_boot_addr >> CPU_BOOT_ADDR_ALIGN) |
		      CPU_BOOT_ADDR_WMASK);
	mmio_write_32(PMU_BASE + PMU_NOC_AUTO_ENA, NOC_AUTO_ENABLE);

	nonboot_cpus_off();

	INFO("%s(%d): pd status %x\n", __func__, __LINE__,
	     mmio_read_32(PMU_BASE + PMU_PWRDN_ST));
}
