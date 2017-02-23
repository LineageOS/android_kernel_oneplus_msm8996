/**
 * core.c - DesignWare USB3 DRD Controller Core file
 *
 * Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 *	    Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2  of
 * the License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>

#include "platform_data.h"
#include "core.h"
#include "gadget.h"
#include "io.h"

#include "debug.h"

/* -------------------------------------------------------------------------- */

void dwc3_set_mode(struct dwc3 *dwc, u32 mode)
{
	u32 reg;

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~(DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG));
	reg |= DWC3_GCTL_PRTCAPDIR(mode);
	/*
	 * Set this bit so that device attempts three more times at SS, even
	 * if it failed previously to operate in SS mode.
	 */
	reg |= DWC3_GCTL_U2RSTECN;
	reg &= ~(DWC3_GCTL_SOFITPSYNC);
	reg &= ~(DWC3_GCTL_PWRDNSCALEMASK);
	reg |= DWC3_GCTL_PWRDNSCALE(2);
	reg |= DWC3_GCTL_U2EXIT_LFPS;
	dwc3_writel(dwc->regs, DWC3_GCTL, reg);

	if (mode == DWC3_GCTL_PRTCAP_OTG || mode == DWC3_GCTL_PRTCAP_HOST) {
		/*
		 * Allow ITP generated off of ref clk based counter instead
		 * of UTMI/ULPI clk based counter, when superspeed only is
		 * active so that UTMI/ULPI PHY can be suspened.
		 *
		 * Starting with revision 2.50A, GFLADJ_REFCLK_LPM_SEL is used
		 * instead.
		 */
		if (dwc->revision < DWC3_REVISION_250A) {
			reg = dwc3_readl(dwc->regs, DWC3_GCTL);
			reg |= DWC3_GCTL_SOFITPSYNC;
			dwc3_writel(dwc->regs, DWC3_GCTL, reg);
		} else {
			reg = dwc3_readl(dwc->regs, DWC3_GFLADJ);
			reg |= DWC3_GFLADJ_REFCLK_LPM_SEL;
			dwc3_writel(dwc->regs, DWC3_GFLADJ, reg);
		}
	}
}

/**
 * Peforms initialization of HS and SS PHYs.
 * If used as a part of POR or init sequence it is recommended
 * that we should perform hard reset of the PHYs prior to invoking
 * this function.
 * @dwc: pointer to our context structure
*/
static int dwc3_init_usb_phys(struct dwc3 *dwc)
{
	int		ret;

	/* Bring up PHYs */
	ret = usb_phy_init(dwc->usb2_phy);
	if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb2_phy) returned %d\n",
				__func__, ret);
		return ret;
	}

	ret = usb_phy_init(dwc->usb3_phy);
	if (ret == -EBUSY) {
		/*
		 * Setting Max speed as high when USB3 PHY initialiation
		 * is failing and USB superspeed can't be supported.
		 */
		dwc->maximum_speed = USB_SPEED_HIGH;
	} else if (ret) {
		pr_err("%s: usb_phy_init(dwc->usb3_phy) returned %d\n",
				__func__, ret);
		return ret;
	}
	ret = phy_init(dwc->usb2_generic_phy);
	if (ret < 0)
		return ret;

	ret = phy_init(dwc->usb3_generic_phy);
	if (ret < 0) {
		phy_exit(dwc->usb2_generic_phy);
		return ret;
	}

	return 0;
}

/**
 * dwc3_core_reset - Issues core soft reset and PHY reset
 * @dwc: pointer to our context structure
 */
static int dwc3_core_reset(struct dwc3 *dwc)
{
	int		ret;

	/* Reset PHYs */
	usb_phy_reset(dwc->usb2_phy);
	usb_phy_reset(dwc->usb3_phy);

	/* Initialize PHYs */
	ret = dwc3_init_usb_phys(dwc);
	if (ret) {
		pr_err("%s: dwc3_init_phys returned %d\n",
				__func__, ret);
		return ret;
	}

	dwc3_notify_event(dwc, DWC3_CONTROLLER_RESET_EVENT, 0);

	dwc3_notify_event(dwc, DWC3_CONTROLLER_POST_RESET_EVENT, 0);

	return 0;
}

/**
 * dwc3_free_one_event_buffer - Frees one event buffer
 * @dwc: Pointer to our controller context structure
 * @evt: Pointer to event buffer to be freed
 */
static void dwc3_free_one_event_buffer(struct dwc3 *dwc,
		struct dwc3_event_buffer *evt)
{
	dma_free_coherent(dwc->dev, evt->length, evt->buf, evt->dma);
}

/**
 * dwc3_alloc_one_event_buffer - Allocates one event buffer structure
 * @dwc: Pointer to our controller context structure
 * @length: size of the event buffer
 *
 * Returns a pointer to the allocated event buffer structure on success
 * otherwise ERR_PTR(errno).
 */
static struct dwc3_event_buffer *dwc3_alloc_one_event_buffer(struct dwc3 *dwc,
		unsigned length, enum event_buf_type type)
{
	struct dwc3_event_buffer	*evt;

	evt = devm_kzalloc(dwc->dev, sizeof(*evt), GFP_KERNEL);
	if (!evt)
		return ERR_PTR(-ENOMEM);

	evt->dwc	= dwc;
	evt->length	= length;
	evt->type	= type;
	evt->buf	= dma_alloc_coherent(dwc->dev, length,
			&evt->dma, GFP_KERNEL);
	if (!evt->buf)
		return ERR_PTR(-ENOMEM);

	return evt;
}

/**
 * dwc3_free_event_buffers - frees all allocated event buffers
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_free_event_buffers(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int i;

	for (i = 0; i < dwc->num_event_buffers; i++) {
		evt = dwc->ev_buffs[i];
		if (evt)
			dwc3_free_one_event_buffer(dwc, evt);
	}
}

/**
 * dwc3_alloc_event_buffers - Allocates @num event buffers of size @length
 * @dwc: pointer to our controller context structure
 * @length: size of event buffer
 *
 * Returns 0 on success otherwise negative errno. In the error case, dwc
 * may contain some buffers allocated but not all which were requested.
 */
static int dwc3_alloc_event_buffers(struct dwc3 *dwc, unsigned length)
{
	int	i;
	int	j = 0;

	dwc->num_event_buffers = dwc->num_normal_event_buffers +
		dwc->num_gsi_event_buffers;

	dwc->ev_buffs = devm_kzalloc(dwc->dev,
			sizeof(*dwc->ev_buffs) * dwc->num_event_buffers,
			GFP_KERNEL);
	if (!dwc->ev_buffs)
		return -ENOMEM;

	for (i = 0; i < dwc->num_normal_event_buffers; i++) {
		struct dwc3_event_buffer	*evt;

		evt = dwc3_alloc_one_event_buffer(dwc, length,
				EVT_BUF_TYPE_NORMAL);
		if (IS_ERR(evt)) {
			dev_err(dwc->dev, "can't allocate event buffer\n");
			return PTR_ERR(evt);
		}
		dwc->ev_buffs[j++] = evt;
	}

	for (i = 0; i < dwc->num_gsi_event_buffers; i++) {
		struct dwc3_event_buffer	*evt;

		evt = dwc3_alloc_one_event_buffer(dwc, length,
				EVT_BUF_TYPE_GSI);
		if (IS_ERR(evt)) {
			dev_err(dwc->dev, "can't allocate event buffer\n");
			return PTR_ERR(evt);
		}
		dwc->ev_buffs[j++] = evt;
	}

	return 0;
}

/**
 * dwc3_event_buffers_setup - setup our allocated event buffers
 * @dwc: pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_event_buffers_setup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int				n;

	for (n = 0; n < dwc->num_event_buffers; n++) {
		evt = dwc->ev_buffs[n];
		dev_dbg(dwc->dev, "Event buf %pK dma %08llx length %d\n",
				evt->buf, (unsigned long long) evt->dma,
				evt->length);

		memset(evt->buf, 0, evt->length);

		evt->lpos = 0;

		dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(n),
				lower_32_bits(evt->dma));

		if (evt->type == EVT_BUF_TYPE_NORMAL) {
			dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(n),
					upper_32_bits(evt->dma));
			dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(n),
					DWC3_GEVNTSIZ_SIZE(evt->length));
		} else {
			dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(n),
				DWC3_GEVNTADRHI_EVNTADRHI_GSI_EN(
					DWC3_GEVENT_TYPE_GSI) |
				DWC3_GEVNTADRHI_EVNTADRHI_GSI_IDX(n));

			dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(n),
				DWC3_GEVNTCOUNT_EVNTINTRPTMASK |
				((evt->length) & 0xffff));
		}

		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(n), 0);
	}

	return 0;
}

static void dwc3_event_buffers_cleanup(struct dwc3 *dwc)
{
	struct dwc3_event_buffer	*evt;
	int				n;

	for (n = 0; n < dwc->num_event_buffers; n++) {
		evt = dwc->ev_buffs[n];

		evt->lpos = 0;

		dwc3_writel(dwc->regs, DWC3_GEVNTADRLO(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTADRHI(n), 0);
		dwc3_writel(dwc->regs, DWC3_GEVNTSIZ(n), DWC3_GEVNTSIZ_INTMASK
				| DWC3_GEVNTSIZ_SIZE(0));
		dwc3_writel(dwc->regs, DWC3_GEVNTCOUNT(n), 0);
	}
}

static int dwc3_alloc_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	dwc->scratchbuf = kmalloc_array(dwc->nr_scratch,
			DWC3_SCRATCHBUF_SIZE, GFP_KERNEL);
	if (!dwc->scratchbuf)
		return -ENOMEM;

	return 0;
}

static int dwc3_setup_scratch_buffers(struct dwc3 *dwc)
{
	dma_addr_t scratch_addr;
	u32 param;
	int ret;

	if (!dwc->has_hibernation)
		return 0;

	if (!dwc->nr_scratch)
		return 0;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return 0;

	scratch_addr = dma_map_single(dwc->dev, dwc->scratchbuf,
			dwc->nr_scratch * DWC3_SCRATCHBUF_SIZE,
			DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dwc->dev, scratch_addr)) {
		dev_err(dwc->dev, "failed to map scratch buffer\n");
		ret = -EFAULT;
		goto err0;
	}

	dwc->scratch_addr = scratch_addr;

	param = lower_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO, param);
	if (ret < 0)
		goto err1;

	param = upper_32_bits(scratch_addr);

	ret = dwc3_send_gadget_generic_command(dwc,
			DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI, param);
	if (ret < 0)
		goto err1;

	return 0;

err1:
	dma_unmap_single(dwc->dev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);

err0:
	return ret;
}

static void dwc3_free_scratch_buffers(struct dwc3 *dwc)
{
	if (!dwc->has_hibernation)
		return;

	if (!dwc->nr_scratch)
		return;

	 /* should never fall here */
	if (!WARN_ON(dwc->scratchbuf))
		return;

	dma_unmap_single(dwc->dev, dwc->scratch_addr, dwc->nr_scratch *
			DWC3_SCRATCHBUF_SIZE, DMA_BIDIRECTIONAL);
	kfree(dwc->scratchbuf);
}

static void dwc3_core_num_eps(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	dwc->num_in_eps = DWC3_NUM_IN_EPS(parms);
	dwc->num_out_eps = DWC3_NUM_EPS(parms) - dwc->num_in_eps;

	dev_vdbg(dwc->dev, "found %d IN and %d OUT endpoints\n",
			dwc->num_in_eps, dwc->num_out_eps);
}

static void dwc3_cache_hwparams(struct dwc3 *dwc)
{
	struct dwc3_hwparams	*parms = &dwc->hwparams;

	parms->hwparams0 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS0);
	parms->hwparams1 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS1);
	parms->hwparams2 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS2);
	parms->hwparams3 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS3);
	parms->hwparams4 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS4);
	parms->hwparams5 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS5);
	parms->hwparams6 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS6);
	parms->hwparams7 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS7);
	parms->hwparams8 = dwc3_readl(dwc->regs, DWC3_GHWPARAMS8);
}

/**
 * dwc3_core_init - Low-level initialization of DWC3 Core
 * @dwc: Pointer to our controller context structure
 *
 * Returns 0 on success otherwise negative errno.
 */
int dwc3_core_init(struct dwc3 *dwc)
{
	unsigned long		timeout;
	u32			hwparams4 = dwc->hwparams.hwparams4;
	u32			reg;
	int			ret;

	reg = dwc3_readl(dwc->regs, DWC3_GSNPSID);
	/* This should read as U3 followed by revision number */
	if ((reg & DWC3_GSNPSID_MASK) != 0x55330000) {
		dev_err(dwc->dev, "this is not a DesignWare USB3 DRD Core\n");
		ret = -ENODEV;
		goto err0;
	}
	dwc->revision = reg;

	/* Handle USB2.0-only core configuration */
	if (DWC3_GHWPARAMS3_SSPHY_IFC(dwc->hwparams.hwparams3) ==
			DWC3_GHWPARAMS3_SSPHY_IFC_DIS) {
		if (dwc->maximum_speed == USB_SPEED_SUPER)
			dwc->maximum_speed = USB_SPEED_HIGH;
	}

	ret = dwc3_core_reset(dwc);
	if (ret)
		goto err0;

	/* issue device SoftReset too */
	timeout = jiffies + msecs_to_jiffies(500);
	dwc3_writel(dwc->regs, DWC3_DCTL, DWC3_DCTL_CSFTRST);
	do {
		reg = dwc3_readl(dwc->regs, DWC3_DCTL);
		if (!(reg & DWC3_DCTL_CSFTRST))
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(dwc->dev, "Reset Timed Out\n");
			ret = -ETIMEDOUT;
			goto err0;
		}

		cpu_relax();
	} while (true);

	reg = dwc3_readl(dwc->regs, DWC3_GCTL);
	reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
	reg &= ~DWC3_GCTL_DISSCRAMBLE;

	switch (DWC3_GHWPARAMS1_EN_PWROPT(dwc->hwparams.hwparams1)) {
	case DWC3_GHWPARAMS1_EN_PWROPT_CLK:
		/**
		 * WORKAROUND: DWC3 revisions between 2.10a and 2.50a have an
		 * issue which would cause xHCI compliance tests to fail.
		 *
		 * Because of that we cannot enable clock gating on such
		 * configurations.
		 *
		 * Refers to:
		 *
		 * STAR#9000588375: Clock Gating, SOF Issues when ref_clk-Based
		 * SOF/ITP Mode Used
		 */
		if ((dwc->dr_mode == USB_DR_MODE_HOST ||
				dwc->dr_mode == USB_DR_MODE_OTG) &&
				(dwc->revision >= DWC3_REVISION_210A &&
				dwc->revision <= DWC3_REVISION_250A))
			reg |= DWC3_GCTL_DSBLCLKGTNG | DWC3_GCTL_SOFITPSYNC;
		else
			reg &= ~DWC3_GCTL_DSBLCLKGTNG;
		break;
	case DWC3_GHWPARAMS1_EN_PWROPT_HIB:
		/* enable hibernation here */
		dwc->nr_scratch = DWC3_GHWPARAMS4_HIBER_SCRATCHBUFS(hwparams4);
		break;
	default:
		dev_dbg(dwc->dev, "No power optimization available\n");
	}

	/*
	 * WORKAROUND: DWC3 revisions <1.90a have a bug
	 * where the device can fail to connect at SuperSpeed
	 * and falls back to high-speed mode which causes
	 * the device to enter a Connect/Disconnect loop
	 */
	if (dwc->revision < DWC3_REVISION_190A)
		reg |= DWC3_GCTL_U2RSTECN;

	dwc3_core_num_eps(dwc);

	/*
	 * Workaround: Disable internal clock gating always, as there
	 * is a known HW bug that causes the internal RAM clock to get
	 * stuck when entering low power modes. Revisit when there is
	 * a way to differentiate HW that no longer needs this.
	 */
	if (dwc->disable_clk_gating) {
		dev_dbg(dwc->dev, "Disabling controller clock gating.\n");
		reg |= DWC3_GCTL_DSBLCLKGTNG;
	}

	dwc3_writel(dwc->regs, DWC3_GCTL, reg);

	ret = dwc3_alloc_scratch_buffers(dwc);
	if (ret)
		goto err1;

	ret = dwc3_setup_scratch_buffers(dwc);
	if (ret)
		goto err2;

	/*
	 * clear Elastic buffer mode in GUSBPIPE_CTRL(0) register, otherwise
	 * it results in high link errors and could cause SS mode transfer
	 * failure.
	 */
	if (!dwc->nominal_elastic_buffer) {
		reg = dwc3_readl(dwc->regs, DWC3_GUSB3PIPECTL(0));
		reg &= ~DWC3_GUSB3PIPECTL_ELASTIC_BUF_MODE;
		dwc3_writel(dwc->regs, DWC3_GUSB3PIPECTL(0), reg);
	}

	return 0;

err2:
	dwc3_free_scratch_buffers(dwc);

err1:
	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);

err0:
	return ret;
}

static void dwc3_core_exit(struct dwc3 *dwc)
{
	dwc3_free_scratch_buffers(dwc);
	usb_phy_shutdown(dwc->usb2_phy);
	usb_phy_shutdown(dwc->usb3_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);
}

static int dwc3_core_get_phy(struct dwc3 *dwc)
{
	struct device		*dev = dwc->dev;
	struct device_node	*node = dev->of_node;
	int ret;

	if (node) {
		dwc->usb2_phy = devm_usb_get_phy_by_phandle(dev, "usb-phy", 0);
		dwc->usb3_phy = devm_usb_get_phy_by_phandle(dev, "usb-phy", 1);
	} else {
		dwc->usb2_phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
		dwc->usb3_phy = devm_usb_get_phy(dev, USB_PHY_TYPE_USB3);
	}

	if (IS_ERR(dwc->usb2_phy)) {
		ret = PTR_ERR(dwc->usb2_phy);
		if (ret == -ENXIO || ret == -ENODEV) {
			dwc->usb2_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb2 phy configured\n");
			return ret;
		}
	}

	if (IS_ERR(dwc->usb3_phy)) {
		ret = PTR_ERR(dwc->usb3_phy);
		if (ret == -ENXIO || ret == -ENODEV) {
			dwc->usb3_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb3 phy configured\n");
			return ret;
		}
	}

	dwc->usb2_generic_phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(dwc->usb2_generic_phy)) {
		ret = PTR_ERR(dwc->usb2_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV) {
			dwc->usb2_generic_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb2 phy configured\n");
			return ret;
		}
	}

	dwc->usb3_generic_phy = devm_phy_get(dev, "usb3-phy");
	if (IS_ERR(dwc->usb3_generic_phy)) {
		ret = PTR_ERR(dwc->usb3_generic_phy);
		if (ret == -ENOSYS || ret == -ENODEV) {
			dwc->usb3_generic_phy = NULL;
		} else if (ret == -EPROBE_DEFER) {
			return ret;
		} else {
			dev_err(dev, "no usb3 phy configured\n");
			return ret;
		}
	}

	return 0;
}

static int dwc3_core_init_mode(struct dwc3 *dwc)
{
	struct device *dev = dwc->dev;

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);
		break;
	case USB_DR_MODE_HOST:
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_HOST);
		break;
	case USB_DR_MODE_OTG:
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_OTG);
		break;
	default:
		dev_err(dev, "Unsupported mode of operation %d\n", dwc->dr_mode);
		return -EINVAL;
	}

	return 0;
}

static void dwc3_core_exit_mode(struct dwc3 *dwc)
{
	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
		dwc3_gadget_exit(dwc);
		break;
	case USB_DR_MODE_HOST:
		dwc3_host_exit(dwc);
		break;
	case USB_DR_MODE_OTG:
		dwc3_gadget_exit(dwc);
		dwc3_host_exit(dwc);
		break;
	default:
		/* do nothing */
		break;
	}
}

/* XHCI reset, resets other CORE registers as well, re-init those */
void dwc3_post_host_reset_core_init(struct dwc3 *dwc)
{
	dwc3_core_init(dwc);
	dwc3_gadget_restart(dwc);
	dwc3_notify_event(dwc, DWC3_CONTROLLER_POST_INITIALIZATION_EVENT, 0);
}

static void (*notify_event)(struct dwc3 *, unsigned, unsigned);
void dwc3_set_notifier(void (*notify)(struct dwc3 *, unsigned, unsigned))
{
	notify_event = notify;
}
EXPORT_SYMBOL(dwc3_set_notifier);

int dwc3_notify_event(struct dwc3 *dwc, unsigned event, unsigned value)
{
	int ret = 0;

	if (dwc->notify_event)
		dwc->notify_event(dwc, event, value);
	else
		ret = -ENODEV;

	return ret;
}
EXPORT_SYMBOL(dwc3_notify_event);

int dwc3_core_pre_init(struct dwc3 *dwc)
{
	int ret;

	dwc3_cache_hwparams(dwc);
	if (!dwc->ev_buffs) {
		ret = dwc3_alloc_event_buffers(dwc, DWC3_EVENT_BUFFERS_SIZE);
		if (ret) {
			dev_err(dwc->dev, "failed to allocate event buffers\n");
			ret = -ENOMEM;
			goto err_alloc;
		}
	}

	ret = dwc3_core_init(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to initialize core\n");
		goto err0;
	}

	ret = dwc3_event_buffers_setup(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to setup event buffers\n");
		goto err0;
	}

	ret = dwc3_core_init_mode(dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to set mode with dwc3 core\n");
		goto err1;
	}

	return ret;

err1:
	dwc3_event_buffers_cleanup(dwc);
err0:
	dwc3_core_exit(dwc);
	dwc3_free_event_buffers(dwc);
err_alloc:
	return ret;
}

#define DWC3_ALIGN_MASK		(16 - 1)

static int dwc3_probe(struct platform_device *pdev)
{
	struct device		*dev = &pdev->dev;
	struct dwc3_platform_data *pdata = dev_get_platdata(dev);
	struct device_node	*node = dev->of_node;
	struct resource		*res;
	struct dwc3		*dwc;
	u8			lpm_nyet_threshold;
	u8			hird_threshold;
	u32			num_evt_buffs;
	int			irq;

	int			ret;

	void __iomem		*regs;
	void			*mem;

	mem = devm_kzalloc(dev, sizeof(*dwc) + DWC3_ALIGN_MASK, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	dwc = PTR_ALIGN(mem, DWC3_ALIGN_MASK + 1);
	dwc->mem = mem;
	dwc->dev = dev;

	dwc->notify_event = notify_event;
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!res) {
		dev_err(dev, "missing IRQ\n");
		return -ENODEV;
	}
	dwc->xhci_resources[1].start = res->start;
	dwc->xhci_resources[1].end = res->end;
	dwc->xhci_resources[1].flags = res->flags;
	dwc->xhci_resources[1].name = res->name;

	irq = platform_get_irq(to_platform_device(dwc->dev), 0);
	ret = devm_request_irq(dev, irq, dwc3_interrupt, IRQF_SHARED, "dwc3",
			dwc);
	if (ret) {
		dev_err(dwc->dev, "failed to request irq #%d --> %d\n",
				irq, ret);
		return -ENODEV;
	}

	dwc->irq = irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "missing memory resource\n");
		return -ENODEV;
	}

	dwc->reg_phys = res->start;
	dwc->xhci_resources[0].start = res->start;
	dwc->xhci_resources[0].end = dwc->xhci_resources[0].start +
					DWC3_XHCI_REGS_END;
	dwc->xhci_resources[0].flags = res->flags;
	dwc->xhci_resources[0].name = res->name;

	res->start += DWC3_GLOBALS_REGS_START;

	/*
	 * Request memory region but exclude xHCI regs,
	 * since it will be requested by the xhci-plat driver.
	 */
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	dwc->regs	= regs;
	dwc->regs_size	= resource_size(res);
	/*
	 * restore res->start back to its original value so that,
	 * in case the probe is deferred, we don't end up getting error in
	 * request the memory region the next time probe is called.
	 */
	res->start -= DWC3_GLOBALS_REGS_START;

	/* default to highest possible threshold */
	lpm_nyet_threshold = 0xff;

	/*
	 * default to assert utmi_sleep_n and use maximum allowed HIRD
	 * threshold value of 0b1100
	 */
	hird_threshold = 12;

	if (node) {
		dwc->maximum_speed = of_usb_get_maximum_speed(node);
		dwc->has_lpm_erratum = of_property_read_bool(node,
				"snps,has-lpm-erratum");
		of_property_read_u8(node, "snps,lpm-nyet-threshold",
				&lpm_nyet_threshold);
		dwc->is_utmi_l1_suspend = of_property_read_bool(node,
				"snps,is-utmi-l1-suspend");
		of_property_read_u8(node, "snps,hird-threshold",
				&hird_threshold);

		dwc->needs_fifo_resize = of_property_read_bool(node,
				"tx-fifo-resize");
		dwc->dr_mode = of_usb_get_dr_mode(node);
		dwc->nominal_elastic_buffer = of_property_read_bool(node,
				"snps,nominal-elastic-buffer");
		dwc->usb3_u1u2_disable = of_property_read_bool(node,
				"snps,usb3-u1u2-disable");
		dwc->enable_bus_suspend = of_property_read_bool(node,
						"snps,bus-suspend-enable");

		dwc->disable_clk_gating = of_property_read_bool(node,
					"snps,disable-clk-gating");

		dwc->num_normal_event_buffers = 1;
		ret = of_property_read_u32(node,
			"snps,num-normal-evt-buffs", &num_evt_buffs);
		if (!ret)
			dwc->num_normal_event_buffers = num_evt_buffs;

		ret = of_property_read_u32(node,
			"snps,num-gsi-evt-buffs", &num_evt_buffs);
		if (!ret)
			dwc->num_gsi_event_buffers = num_evt_buffs;

		if (dwc->enable_bus_suspend) {
			pm_runtime_set_autosuspend_delay(dev, 500);
			pm_runtime_use_autosuspend(dev);
		}
	} else if (pdata) {
		dwc->maximum_speed = pdata->maximum_speed;
		dwc->has_lpm_erratum = pdata->has_lpm_erratum;
		if (pdata->lpm_nyet_threshold)
			lpm_nyet_threshold = pdata->lpm_nyet_threshold;
		dwc->is_utmi_l1_suspend = pdata->is_utmi_l1_suspend;
		if (pdata->hird_threshold)
			hird_threshold = pdata->hird_threshold;

		dwc->needs_fifo_resize = pdata->tx_fifo_resize;
		dwc->dr_mode = pdata->dr_mode;
	}

	/* default to superspeed if no maximum_speed passed */
	if (dwc->maximum_speed == USB_SPEED_UNKNOWN)
		dwc->maximum_speed = USB_SPEED_SUPER;

	dwc->lpm_nyet_threshold = lpm_nyet_threshold;

	dwc->hird_threshold = hird_threshold
		| (dwc->is_utmi_l1_suspend << 4);

	ret = dwc3_core_get_phy(dwc);
	if (ret)
		return ret;

	spin_lock_init(&dwc->lock);
	init_waitqueue_head(&dwc->wait_linkstate);
	platform_set_drvdata(pdev, dwc);

	dev->dma_mask	= dev->parent->dma_mask;
	dev->dma_parms	= dev->parent->dma_parms;
	dma_set_coherent_mask(dev, dev->parent->coherent_dma_mask);

	pm_runtime_no_callbacks(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	if (IS_ENABLED(CONFIG_USB_DWC3_HOST))
		dwc->dr_mode = USB_DR_MODE_HOST;
	else if (IS_ENABLED(CONFIG_USB_DWC3_GADGET))
		dwc->dr_mode = USB_DR_MODE_PERIPHERAL;

	if (dwc->dr_mode == USB_DR_MODE_UNKNOWN) {
		dwc->dr_mode = USB_DR_MODE_OTG;
		dwc->is_drd = true;
	}

	ret = phy_power_on(dwc->usb2_generic_phy);
	if (ret < 0)
		goto err;

	ret = phy_power_on(dwc->usb3_generic_phy);
	if (ret < 0)
		goto err_usb2phy_power;

	ret = dwc3_debugfs_init(dwc);
	if (ret) {
		dev_err(dev, "failed to initialize debugfs\n");
		goto err_usb3phy_power;
	}

	/* Hardcode number of eps */
	dwc->num_in_eps = 16;
	dwc->num_out_eps = 16;

	if (dwc->dr_mode == USB_DR_MODE_OTG ||
		dwc->dr_mode == USB_DR_MODE_PERIPHERAL) {
		ret = dwc3_gadget_init(dwc);
		if (ret) {
			dev_err(dev, "failed to initialize gadget\n");
			goto err_usb3phy_power;
		}
	}

	if (dwc->dr_mode == USB_DR_MODE_OTG ||
		dwc->dr_mode ==  USB_DR_MODE_HOST) {
		ret = dwc3_host_init(dwc);
		if (ret) {
			dev_err(dev, "failed to initialize host\n");
			goto err_gadget_exit;
		}
	}
	dwc3_notify_event(dwc, DWC3_CONTROLLER_POST_INITIALIZATION_EVENT, 0);

	return 0;

err_gadget_exit:
	if (dwc->dr_mode == USB_DR_MODE_OTG)
		dwc3_gadget_exit(dwc);
err_usb3phy_power:
	phy_power_off(dwc->usb3_generic_phy);

err_usb2phy_power:
	phy_power_off(dwc->usb2_generic_phy);
err:
	return ret;
}

static int dwc3_remove(struct platform_device *pdev)
{
	struct dwc3	*dwc = platform_get_drvdata(pdev);

	dwc3_debugfs_exit(dwc);
	dwc3_core_exit_mode(dwc);
	dwc3_event_buffers_cleanup(dwc);
	dwc3_free_event_buffers(dwc);

	phy_power_off(dwc->usb2_generic_phy);
	phy_power_off(dwc->usb3_generic_phy);

	dwc3_core_exit(dwc);

	pm_runtime_disable(&pdev->dev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int dwc3_prepare(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	unsigned long	flags;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_PREPARE_EVENT, 0))
		return 0;

	spin_lock_irqsave(&dwc->lock, flags);

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		dwc3_gadget_prepare(dwc);
		/* FALLTHROUGH */
	case USB_DR_MODE_HOST:
	default:
		dwc3_event_buffers_cleanup(dwc);
		break;
	}

	spin_unlock_irqrestore(&dwc->lock, flags);

	return 0;
}

static void dwc3_complete(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	unsigned long	flags;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_COMPLETE_EVENT, 0))
		return;

	spin_lock_irqsave(&dwc->lock, flags);

	dwc3_event_buffers_setup(dwc);
	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		dwc3_gadget_complete(dwc);
		/* FALLTHROUGH */
	case USB_DR_MODE_HOST:
	default:
		break;
	}

	spin_unlock_irqrestore(&dwc->lock, flags);
}

static int dwc3_suspend(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	unsigned long	flags;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_SUSPEND_EVENT, 0))
		return 0;

	spin_lock_irqsave(&dwc->lock, flags);

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		dwc3_gadget_suspend(dwc);
		/* FALLTHROUGH */
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	dwc->gctl = dwc3_readl(dwc->regs, DWC3_GCTL);
	spin_unlock_irqrestore(&dwc->lock, flags);

	usb_phy_shutdown(dwc->usb3_phy);
	usb_phy_shutdown(dwc->usb2_phy);
	phy_exit(dwc->usb2_generic_phy);
	phy_exit(dwc->usb3_generic_phy);

	return 0;
}

static int dwc3_resume(struct device *dev)
{
	struct dwc3	*dwc = dev_get_drvdata(dev);
	unsigned long	flags;
	int		ret;

	/* Check if platform glue driver handling PM, if not then handle here */
	if (!dwc3_notify_event(dwc, DWC3_CORE_PM_RESUME_EVENT, 0))
		return 0;

	usb_phy_init(dwc->usb3_phy);
	usb_phy_init(dwc->usb2_phy);
	ret = phy_init(dwc->usb2_generic_phy);
	if (ret < 0)
		return ret;

	ret = phy_init(dwc->usb3_generic_phy);
	if (ret < 0)
		goto err_usb2phy_init;

	spin_lock_irqsave(&dwc->lock, flags);

	dwc3_writel(dwc->regs, DWC3_GCTL, dwc->gctl);

	switch (dwc->dr_mode) {
	case USB_DR_MODE_PERIPHERAL:
	case USB_DR_MODE_OTG:
		dwc3_gadget_resume(dwc);
		/* FALLTHROUGH */
	case USB_DR_MODE_HOST:
	default:
		/* do nothing */
		break;
	}

	spin_unlock_irqrestore(&dwc->lock, flags);

	pm_runtime_disable(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;

err_usb2phy_init:
	phy_exit(dwc->usb2_generic_phy);

	return ret;
}

static const struct dev_pm_ops dwc3_dev_pm_ops = {
	.prepare	= dwc3_prepare,
	.complete	= dwc3_complete,

	SET_SYSTEM_SLEEP_PM_OPS(dwc3_suspend, dwc3_resume)
};

#define DWC3_PM_OPS	&(dwc3_dev_pm_ops)
#else
#define DWC3_PM_OPS	NULL
#endif

#ifdef CONFIG_OF
static const struct of_device_id of_dwc3_match[] = {
	{
		.compatible = "snps,dwc3"
	},
	{
		.compatible = "synopsys,dwc3"
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_match);
#endif

static struct platform_driver dwc3_driver = {
	.probe		= dwc3_probe,
	.remove		= dwc3_remove,
	.driver		= {
		.name	= "dwc3",
		.of_match_table	= of_match_ptr(of_dwc3_match),
		.pm	= DWC3_PM_OPS,
	},
};

module_platform_driver(dwc3_driver);

MODULE_ALIAS("platform:dwc3");
MODULE_AUTHOR("Felipe Balbi <balbi@ti.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 DRD Controller Driver");
