/* Copyright (c) 2011-2016, Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/termios.h>
#include <soc/qcom/smd.h>
#include <linux/netdevice.h>
#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/termios.h>

#include <soc/qcom/bam_dmux.h>

#include <linux/usb/msm_hsusb.h>
#include <linux/usb/usb_ctrl_qti.h>
#include <linux/usb_bam.h>

#include "usb_gadget_xport.h"
#include "u_rmnet.h"

#define BAM_N_PORTS	 2
#define BAM2BAM_N_PORTS	 4

static struct workqueue_struct *gbam_wq;
static int n_bam_ports;
static int n_bam2bam_ports;
static unsigned n_tx_req_queued;

static unsigned bam_ch_ids[BAM_N_PORTS] = {
	BAM_DMUX_USB_RMNET_0,
	BAM_DMUX_USB_DPL
};

static char bam_ch_names[BAM_N_PORTS][BAM_DMUX_CH_NAME_MAX_LEN];

static const enum ipa_client_type usb_prod[BAM2BAM_N_PORTS] = {
	IPA_CLIENT_USB_PROD, IPA_CLIENT_USB2_PROD,
	IPA_CLIENT_USB3_PROD, IPA_CLIENT_USB4_PROD
};
static const enum ipa_client_type usb_cons[BAM2BAM_N_PORTS] = {
	IPA_CLIENT_USB_CONS, IPA_CLIENT_USB2_CONS,
	IPA_CLIENT_USB3_CONS, IPA_CLIENT_USB4_CONS
};

#define BAM_PENDING_PKTS_LIMIT			220
#define BAM_MUX_TX_PKT_DROP_THRESHOLD		1000
#define BAM_MUX_RX_PKT_FCTRL_EN_TSHOLD		500
#define BAM_MUX_RX_PKT_FCTRL_DIS_TSHOLD		300
#define BAM_MUX_RX_PKT_FLOW_CTRL_SUPPORT	1

#define BAM_MUX_HDR				8

#define BAM_MUX_RX_Q_SIZE			128
#define BAM_MUX_TX_Q_SIZE			200
#define BAM_MUX_RX_REQ_SIZE			2048   /* Must be 1KB aligned */

#define DL_INTR_THRESHOLD			20
#define BAM_PENDING_BYTES_LIMIT			(50 * BAM_MUX_RX_REQ_SIZE)
#define BAM_PENDING_BYTES_FCTRL_EN_TSHOLD	(BAM_PENDING_BYTES_LIMIT / 3)

/* Extra buffer size to allocate for tx */
#define EXTRA_ALLOCATION_SIZE_U_BAM	128

static unsigned int bam_pending_pkts_limit = BAM_PENDING_PKTS_LIMIT;
module_param(bam_pending_pkts_limit, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_pending_bytes_limit = BAM_PENDING_BYTES_LIMIT;
module_param(bam_pending_bytes_limit, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_pending_bytes_fctrl_en_thold =
					BAM_PENDING_BYTES_FCTRL_EN_TSHOLD;
module_param(bam_pending_bytes_fctrl_en_thold, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_tx_pkt_drop_thld = BAM_MUX_TX_PKT_DROP_THRESHOLD;
module_param(bam_mux_tx_pkt_drop_thld, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_rx_fctrl_en_thld = BAM_MUX_RX_PKT_FCTRL_EN_TSHOLD;
module_param(bam_mux_rx_fctrl_en_thld, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_rx_fctrl_support = BAM_MUX_RX_PKT_FLOW_CTRL_SUPPORT;
module_param(bam_mux_rx_fctrl_support, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_rx_fctrl_dis_thld = BAM_MUX_RX_PKT_FCTRL_DIS_TSHOLD;
module_param(bam_mux_rx_fctrl_dis_thld, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_tx_q_size = BAM_MUX_TX_Q_SIZE;
module_param(bam_mux_tx_q_size, uint, S_IRUGO | S_IWUSR);

static unsigned int bam_mux_rx_q_size = BAM_MUX_RX_Q_SIZE;
module_param(bam_mux_rx_q_size, uint, S_IRUGO | S_IWUSR);

static unsigned long bam_mux_rx_req_size = BAM_MUX_RX_REQ_SIZE;
module_param(bam_mux_rx_req_size, ulong, S_IRUGO);

static unsigned int dl_intr_threshold = DL_INTR_THRESHOLD;
module_param(dl_intr_threshold, uint, S_IRUGO | S_IWUSR);

#define BAM_CH_OPENED			BIT(0)
#define BAM_CH_READY			BIT(1)
#define BAM_CH_WRITE_INPROGRESS		BIT(2)

enum u_bam_event_type {
	U_BAM_DISCONNECT_E = 0,
	U_BAM_CONNECT_E,
	U_BAM_SUSPEND_E,
	U_BAM_RESUME_E
};

struct sys2ipa_sw {
	void		*teth_priv;
	ipa_notify_cb	teth_cb;
};

struct bam_ch_info {
	unsigned long		flags;
	unsigned		id;

	struct list_head        tx_idle;
	struct sk_buff_head	tx_skb_q;

	struct list_head        rx_idle;
	struct sk_buff_head	rx_skb_q;
	struct sk_buff_head	rx_skb_idle;

	struct gbam_port	*port;
	struct work_struct	write_tobam_w;
	struct work_struct	write_tohost_w;

	struct usb_request	*rx_req;
	struct usb_request	*tx_req;

	u32			src_pipe_idx;
	u32			dst_pipe_idx;
	u8			src_connection_idx;
	u8			dst_connection_idx;
	enum usb_ctrl		usb_bam_type;

	enum transport_type trans;
	struct usb_bam_connect_ipa_params ipa_params;

	/* added to support sys to ipa sw UL path */
	struct sys2ipa_sw	ul_params;
	enum usb_bam_pipe_type	src_pipe_type;
	enum usb_bam_pipe_type	dst_pipe_type;

	/* stats */
	unsigned int		pending_pkts_with_bam;
	unsigned int		pending_bytes_with_bam;
	unsigned int		tohost_drp_cnt;
	unsigned int		tomodem_drp_cnt;
	unsigned int		tx_len;
	unsigned int		rx_len;
	unsigned long		to_modem;
	unsigned long		to_host;
	unsigned int		rx_flow_control_disable;
	unsigned int		rx_flow_control_enable;
	unsigned int		rx_flow_control_triggered;
	unsigned int		max_num_pkts_pending_with_bam;
	unsigned int		max_bytes_pending_with_bam;
	unsigned int		delayed_bam_mux_write_done;
	unsigned long		skb_expand_cnt;
};

struct gbam_port {
	bool			is_connected;
	enum u_bam_event_type	last_event;
	unsigned		port_num;
	spinlock_t		port_lock_ul;
	spinlock_t		port_lock_dl;
	spinlock_t		port_lock;

	struct grmnet		*port_usb;
	struct usb_gadget	*gadget;

	struct bam_ch_info	data_ch;

	struct work_struct	connect_w;
	struct work_struct	disconnect_w;
	struct work_struct	suspend_w;
	struct work_struct	resume_w;
};

static struct bam_portmaster {
	struct gbam_port *port;
	struct platform_driver pdrv;
} bam_ports[BAM_N_PORTS];

struct  u_bam_data_connect_info {
	u32 usb_bam_pipe_idx;
	u32 peer_pipe_idx;
	unsigned long usb_bam_handle;
};

struct gbam_port *bam2bam_ports[BAM2BAM_N_PORTS];
static void gbam_start_rx(struct gbam_port *port);
static void gbam_start_endless_rx(struct gbam_port *port);
static void gbam_start_endless_tx(struct gbam_port *port);
static void gbam_notify(void *p, int event, unsigned long data);
static void gbam_data_write_tobam(struct work_struct *w);

/*---------------misc functions---------------- */
static void gbam_free_requests(struct usb_ep *ep, struct list_head *head)
{
	struct usb_request	*req;

	while (!list_empty(head)) {
		req = list_entry(head->next, struct usb_request, list);
		list_del(&req->list);
		usb_ep_free_request(ep, req);
	}
}

static int gbam_alloc_requests(struct usb_ep *ep, struct list_head *head,
		int num,
		void (*cb)(struct usb_ep *ep, struct usb_request *),
		gfp_t flags)
{
	int i;
	struct usb_request *req;
	pr_debug("%s: ep:%pK head:%pK num:%d cb:%pK", __func__,
			ep, head, num, cb);

	for (i = 0; i < num; i++) {
		req = usb_ep_alloc_request(ep, flags);
		if (!req) {
			pr_debug("%s: req allocated:%d\n", __func__, i);
			return list_empty(head) ? -ENOMEM : 0;
		}
		req->complete = cb;
		list_add(&req->list, head);
	}

	return 0;
}

static inline dma_addr_t gbam_get_dma_from_skb(struct sk_buff *skb)
{
	return *((dma_addr_t *)(skb->cb));
}

/* This function should be called with port_lock_ul lock held */
static struct sk_buff *gbam_alloc_skb_from_pool(struct gbam_port *port)
{
	struct bam_ch_info *d;
	struct sk_buff *skb;
	dma_addr_t      skb_buf_dma_addr;
	struct usb_gadget *gadget;

	if (!port)
		return NULL;

	d = &port->data_ch;
	if (!d)
		return NULL;

	if (d->rx_skb_idle.qlen == 0) {
		/*
		 * In case skb idle pool is empty, we allow to allocate more
		 * skbs so we dynamically enlarge the pool size when needed.
		 * Therefore, in steady state this dynamic allocation will
		 * stop when the pool will arrive to its optimal size.
		 */
		pr_debug("%s: allocate skb\n", __func__);
		skb = alloc_skb(bam_mux_rx_req_size + BAM_MUX_HDR, GFP_ATOMIC);

		if (!skb) {
			pr_err("%s: alloc skb failed\n", __func__);
			goto alloc_exit;
		}

		skb_reserve(skb, BAM_MUX_HDR);

		if ((d->trans == USB_GADGET_XPORT_BAM2BAM_IPA)) {

			gadget = port->port_usb->gadget;

			skb_buf_dma_addr =
				dma_map_single(&gadget->dev, skb->data,
					bam_mux_rx_req_size, DMA_BIDIRECTIONAL);

			if (dma_mapping_error(&gadget->dev, skb_buf_dma_addr)) {
				pr_err("%s: Could not DMA map SKB buffer\n",
					__func__);
				skb_buf_dma_addr = DMA_ERROR_CODE;
			}
		} else {
			skb_buf_dma_addr = DMA_ERROR_CODE;
		}


		memcpy(skb->cb, &skb_buf_dma_addr,
			sizeof(skb_buf_dma_addr));

	} else {
		pr_debug("%s: pull skb from pool\n", __func__);
		skb = __skb_dequeue(&d->rx_skb_idle);
		if (skb_headroom(skb) < BAM_MUX_HDR)
			skb_reserve(skb, BAM_MUX_HDR);
	}

alloc_exit:
	return skb;
}

/* This function should be called with port_lock_ul lock held */
static void gbam_free_skb_to_pool(struct gbam_port *port, struct sk_buff *skb)
{
	struct bam_ch_info *d;

	if (!port)
		return;
	d = &port->data_ch;

	skb->len = 0;
	skb_reset_tail_pointer(skb);
	__skb_queue_tail(&d->rx_skb_idle, skb);
}

static void gbam_free_rx_skb_idle_list(struct gbam_port *port)
{
	struct bam_ch_info *d;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
	struct usb_gadget *gadget = NULL;

	if (!port)
		return;
	d = &port->data_ch;

	gadget = port->port_usb->gadget;

	while (d->rx_skb_idle.qlen > 0) {
		skb = __skb_dequeue(&d->rx_skb_idle);
		dma_addr = gbam_get_dma_from_skb(skb);

		if (gadget && dma_addr != DMA_ERROR_CODE) {
			dma_unmap_single(&gadget->dev, dma_addr,
				bam_mux_rx_req_size, DMA_BIDIRECTIONAL);

			dma_addr = DMA_ERROR_CODE;
			memcpy(skb->cb, &dma_addr,
				sizeof(dma_addr));
		}
		dev_kfree_skb_any(skb);
	}
}

/*----- sys2bam towards the IPA --------------- */
static void gbam_ipa_sys2bam_notify_cb(void *priv, enum ipa_dp_evt_type event,
		unsigned long data)
{
	struct sys2ipa_sw	*ul = (struct sys2ipa_sw *)priv;
	struct gbam_port	*port;
	struct bam_ch_info	*d;

	switch (event) {
	case IPA_WRITE_DONE:
		d = container_of(ul, struct bam_ch_info, ul_params);
		port = container_of(d, struct gbam_port, data_ch);
		/* call into bam_demux functionality that'll recycle the data */
		gbam_notify(port, BAM_DMUX_WRITE_DONE, data);
		break;
	case IPA_RECEIVE:
		/* call the callback given by tethering driver init function
		 * (and was given to ipa_connect)
		 */
		if (ul->teth_cb)
			ul->teth_cb(ul->teth_priv, event, data);
		break;
	default:
		/* unexpected event */
		pr_err("%s: unexpected event %d\n", __func__, event);
		break;
	}
}


/*--------------------------------------------- */

/*------------data_path----------------------------*/
static void gbam_write_data_tohost(struct gbam_port *port)
{
	unsigned long			flags;
	struct bam_ch_info		*d = &port->data_ch;
	struct sk_buff			*skb;
	struct sk_buff			*new_skb;
	int				ret;
	int				tail_room = 0;
	int				extra_alloc = 0;
	struct usb_request		*req;
	struct usb_ep			*ep;

	spin_lock_irqsave(&port->port_lock_dl, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		return;
	}

	ep = port->port_usb->in;

	while (!list_empty(&d->tx_idle)) {
		skb = __skb_dequeue(&d->tx_skb_q);
		if (!skb)
			break;

		/*
		 * Some UDC requires allocation of some extra bytes for
		 * TX buffer due to hardware requirement. Check if extra
		 * bytes are already there, otherwise allocate new buffer
		 * with extra bytes and do memcpy.
		 */
		if (port->gadget->extra_buf_alloc)
			extra_alloc = EXTRA_ALLOCATION_SIZE_U_BAM;
		tail_room = skb_tailroom(skb);
		if (tail_room < extra_alloc) {
			pr_debug("%s: tail_room  %d less than %d\n", __func__,
					tail_room, extra_alloc);
			new_skb = skb_copy_expand(skb, 0, extra_alloc -
					tail_room, GFP_ATOMIC);
			if (!new_skb) {
				pr_err("skb_copy_expand failed\n");
				break;
			}
			dev_kfree_skb_any(skb);
			skb = new_skb;
			d->skb_expand_cnt++;
		}

		req = list_first_entry(&d->tx_idle,
				struct usb_request,
				list);
		req->context = skb;
		req->buf = skb->data;
		req->length = skb->len;
		n_tx_req_queued++;
		if (n_tx_req_queued == dl_intr_threshold) {
			req->no_interrupt = 0;
			n_tx_req_queued = 0;
		} else {
			req->no_interrupt = 1;
		}

		/* Send ZLP in case packet length is multiple of maxpacksize */
		req->zero = 1;

		list_del(&req->list);

		spin_unlock(&port->port_lock_dl);
		ret = usb_ep_queue(ep, req, GFP_ATOMIC);
		spin_lock(&port->port_lock_dl);
		if (ret) {
			pr_err("%s: usb epIn failed with %d\n", __func__, ret);
			list_add(&req->list, &d->tx_idle);
			dev_kfree_skb_any(skb);
			break;
		}
		d->to_host++;
	}
	spin_unlock_irqrestore(&port->port_lock_dl, flags);
}

static void gbam_write_data_tohost_w(struct work_struct *w)
{
	struct bam_ch_info	*d;
	struct gbam_port	*port;

	d = container_of(w, struct bam_ch_info, write_tohost_w);
	port = d->port;

	gbam_write_data_tohost(port);
}

void gbam_data_recv_cb(void *p, struct sk_buff *skb)
{
	struct gbam_port	*port = p;
	struct bam_ch_info	*d = &port->data_ch;
	unsigned long		flags;

	if (!skb)
		return;

	pr_debug("%s: p:%pK#%d d:%pK skb_len:%d\n", __func__,
			port, port->port_num, d, skb->len);

	spin_lock_irqsave(&port->port_lock_dl, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		dev_kfree_skb_any(skb);
		return;
	}

	if (d->tx_skb_q.qlen > bam_mux_tx_pkt_drop_thld) {
		d->tohost_drp_cnt++;
		if (printk_ratelimit())
			pr_err("%s: tx pkt dropped: tx_drop_cnt:%u\n",
					__func__, d->tohost_drp_cnt);
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		dev_kfree_skb_any(skb);
		return;
	}

	__skb_queue_tail(&d->tx_skb_q, skb);
	spin_unlock_irqrestore(&port->port_lock_dl, flags);

	gbam_write_data_tohost(port);
}

void gbam_data_write_done(void *p, struct sk_buff *skb)
{
	struct gbam_port	*port = p;
	struct bam_ch_info	*d = &port->data_ch;
	unsigned long		flags;

	if (!skb)
		return;

	spin_lock_irqsave(&port->port_lock_ul, flags);

	d->pending_pkts_with_bam--;
	d->pending_bytes_with_bam -= skb->len;
	gbam_free_skb_to_pool(port, skb);

	pr_debug("%s:port:%pK d:%pK tom:%lu ppkt:%u pbytes:%u pno:%d\n", __func__,
			port, d, d->to_modem, d->pending_pkts_with_bam,
			d->pending_bytes_with_bam, port->port_num);

	spin_unlock_irqrestore(&port->port_lock_ul, flags);

	/*
	 * If BAM doesn't have much pending data then push new data from here:
	 * write_complete notify only to avoid any underruns due to wq latency
	 */
	if (d->pending_bytes_with_bam <= bam_pending_bytes_fctrl_en_thold) {
		gbam_data_write_tobam(&d->write_tobam_w);
	} else {
		d->delayed_bam_mux_write_done++;
		queue_work(gbam_wq, &d->write_tobam_w);
	}
}

/* This function should be called with port_lock_ul spinlock acquired */
static bool gbam_ul_bam_limit_reached(struct bam_ch_info *data_ch)
{
	unsigned int	curr_pending_pkts = data_ch->pending_pkts_with_bam;
	unsigned int	curr_pending_bytes = data_ch->pending_bytes_with_bam;
	struct sk_buff	*skb;

	if (curr_pending_pkts >= bam_pending_pkts_limit)
		return true;

	/* check if next skb length doesn't exceed pending_bytes_limit */
	skb = skb_peek(&data_ch->rx_skb_q);
	if (!skb)
		return false;

	if ((curr_pending_bytes + skb->len) > bam_pending_bytes_limit)
		return true;
	else
		return false;
}

static void gbam_data_write_tobam(struct work_struct *w)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	struct sk_buff		*skb;
	unsigned long		flags;
	int			ret;
	int			qlen;

	d = container_of(w, struct bam_ch_info, write_tobam_w);
	port = d->port;

	spin_lock_irqsave(&port->port_lock_ul, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		return;
	}
	/* Bail out if already in progress */
	if (test_bit(BAM_CH_WRITE_INPROGRESS, &d->flags)) {
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		return;
	}

	set_bit(BAM_CH_WRITE_INPROGRESS, &d->flags);

	while (!gbam_ul_bam_limit_reached(d) &&
			(d->trans != USB_GADGET_XPORT_BAM2BAM_IPA ||
			usb_bam_get_prod_granted(d->usb_bam_type,
					d->dst_connection_idx))) {
		skb =  __skb_dequeue(&d->rx_skb_q);
		if (!skb)
			break;

		d->pending_pkts_with_bam++;
		d->pending_bytes_with_bam += skb->len;
		d->to_modem++;

		pr_debug("%s: port:%pK d:%pK tom:%lu ppkts:%u pbytes:%u pno:%d\n",
				__func__, port, d,
				d->to_modem, d->pending_pkts_with_bam,
				d->pending_bytes_with_bam, port->port_num);

		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		if (d->src_pipe_type == USB_BAM_PIPE_SYS2BAM) {
			dma_addr_t         skb_dma_addr;
			struct ipa_tx_meta ipa_meta = {0x0};

			skb_dma_addr = gbam_get_dma_from_skb(skb);
			if (skb_dma_addr != DMA_ERROR_CODE) {
				ipa_meta.dma_address = skb_dma_addr;
				ipa_meta.dma_address_valid = true;
			}

			ret = ipa_tx_dp(usb_prod[port->port_num],
				skb,
				&ipa_meta);
		} else {
			ret = msm_bam_dmux_write(d->id, skb);
		}

		spin_lock_irqsave(&port->port_lock_ul, flags);
		if (ret) {
			pr_debug("%s: write error:%d\n", __func__, ret);
			d->pending_pkts_with_bam--;
			d->pending_bytes_with_bam -= skb->len;
			d->to_modem--;
			d->tomodem_drp_cnt++;
			gbam_free_skb_to_pool(port, skb);
			break;
		}
		if (d->pending_pkts_with_bam > d->max_num_pkts_pending_with_bam)
			d->max_num_pkts_pending_with_bam =
					d->pending_pkts_with_bam;
		if (d->pending_bytes_with_bam > d->max_bytes_pending_with_bam)
			d->max_bytes_pending_with_bam =
					d->pending_bytes_with_bam;
	}

	qlen = d->rx_skb_q.qlen;

	clear_bit(BAM_CH_WRITE_INPROGRESS, &d->flags);
	spin_unlock_irqrestore(&port->port_lock_ul, flags);

	if (qlen < bam_mux_rx_fctrl_dis_thld) {
		if (d->rx_flow_control_triggered) {
			d->rx_flow_control_disable++;
			d->rx_flow_control_triggered = 0;
		}
		gbam_start_rx(port);
	}
}
/*-------------------------------------------------------------*/

static void gbam_epin_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gbam_port	*port = ep->driver_data;
	struct bam_ch_info	*d;
	struct sk_buff		*skb = req->context;
	int			status = req->status;

	switch (status) {
	case 0:
		/* successful completion */
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* connection gone */
		dev_kfree_skb_any(skb);
		usb_ep_free_request(ep, req);
		return;
	default:
		pr_err("%s: data tx ep error %d\n",
				__func__, status);
		break;
	}

	dev_kfree_skb_any(skb);

	if (!port)
		return;

	spin_lock(&port->port_lock_dl);
	d = &port->data_ch;
	list_add_tail(&req->list, &d->tx_idle);
	spin_unlock(&port->port_lock_dl);

	queue_work(gbam_wq, &d->write_tohost_w);
}

static void
gbam_epout_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gbam_port	*port = ep->driver_data;
	struct bam_ch_info	*d = &port->data_ch;
	struct sk_buff		*skb = req->context;
	int			status = req->status;
	int			queue = 0;

	switch (status) {
	case 0:
		skb_put(skb, req->actual);
		queue = 1;
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* cable disconnection */
		spin_lock(&port->port_lock_ul);
		gbam_free_skb_to_pool(port, skb);
		spin_unlock(&port->port_lock_ul);
		req->buf = 0;
		usb_ep_free_request(ep, req);
		return;
	default:
		if (printk_ratelimit())
			pr_err("%s: %s response error %d, %d/%d\n",
				__func__, ep->name, status,
				req->actual, req->length);
		spin_lock(&port->port_lock_ul);
		gbam_free_skb_to_pool(port, skb);
		spin_unlock(&port->port_lock_ul);
		break;
	}

	spin_lock(&port->port_lock_ul);

	if (queue) {
		__skb_queue_tail(&d->rx_skb_q, skb);
		if ((d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) &&
			!usb_bam_get_prod_granted(d->usb_bam_type,
					d->dst_connection_idx)) {
			list_add_tail(&req->list, &d->rx_idle);
			spin_unlock(&port->port_lock_ul);
			return;
		} else
			queue_work(gbam_wq, &d->write_tobam_w);
	}

	/* TODO: Handle flow control gracefully by having
	 * having call back mechanism from bam driver
	 */
	if (bam_mux_rx_fctrl_support &&
		d->rx_skb_q.qlen >= bam_mux_rx_fctrl_en_thld) {
		if (!d->rx_flow_control_triggered) {
			d->rx_flow_control_triggered = 1;
			d->rx_flow_control_enable++;
		}
		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock_ul);
		return;
	}

	skb = gbam_alloc_skb_from_pool(port);
	if (!skb) {
		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock_ul);
		return;
	}
	spin_unlock(&port->port_lock_ul);

	req->buf = skb->data;
	req->dma = gbam_get_dma_from_skb(skb);
	req->length = bam_mux_rx_req_size;

	if (req->dma != DMA_ERROR_CODE)
		req->dma_pre_mapped = true;
	else
		req->dma_pre_mapped = false;

	req->context = skb;

	status = usb_ep_queue(ep, req, GFP_ATOMIC);
	if (status) {
		spin_lock(&port->port_lock_ul);
		gbam_free_skb_to_pool(port, skb);
		spin_unlock(&port->port_lock_ul);

		if (printk_ratelimit())
			pr_err("%s: data rx enqueue err %d\n",
					__func__, status);

		spin_lock(&port->port_lock_ul);
		list_add_tail(&req->list, &d->rx_idle);
		spin_unlock(&port->port_lock_ul);
	}
}

static void gbam_endless_rx_complete(struct usb_ep *ep, struct usb_request *req)
{
	int status = req->status;

	pr_debug("%s status: %d\n", __func__, status);
}

static void gbam_endless_tx_complete(struct usb_ep *ep, struct usb_request *req)
{
	int status = req->status;

	pr_debug("%s status: %d\n", __func__, status);
}

static void gbam_start_rx(struct gbam_port *port)
{
	struct usb_request		*req;
	struct bam_ch_info		*d;
	struct usb_ep			*ep;
	unsigned long			flags;
	int				ret;
	struct sk_buff			*skb;

	spin_lock_irqsave(&port->port_lock_ul, flags);
	if (!port->port_usb || !port->port_usb->out) {
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		return;
	}

	d = &port->data_ch;
	ep = port->port_usb->out;

	while (port->port_usb && !list_empty(&d->rx_idle)) {

		if (bam_mux_rx_fctrl_support &&
			d->rx_skb_q.qlen >= bam_mux_rx_fctrl_en_thld)
			break;

		req = list_first_entry(&d->rx_idle, struct usb_request, list);

		skb = gbam_alloc_skb_from_pool(port);
		if (!skb)
			break;

		list_del(&req->list);
		req->buf = skb->data;
		req->dma = gbam_get_dma_from_skb(skb);
		req->length = bam_mux_rx_req_size;

		if (req->dma != DMA_ERROR_CODE)
			req->dma_pre_mapped = true;
		else
			req->dma_pre_mapped = false;

		req->context = skb;

		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		ret = usb_ep_queue(ep, req, GFP_ATOMIC);
		spin_lock_irqsave(&port->port_lock_ul, flags);
		if (ret) {
			gbam_free_skb_to_pool(port, skb);

			if (printk_ratelimit())
				pr_err("%s: rx queue failed %d\n",
							__func__, ret);

			if (port->port_usb)
				list_add(&req->list, &d->rx_idle);
			else
				usb_ep_free_request(ep, req);
			break;
		}
	}

	spin_unlock_irqrestore(&port->port_lock_ul, flags);
}

static void gbam_start_endless_rx(struct gbam_port *port)
{
	struct bam_ch_info *d = &port->data_ch;
	int status;
	struct usb_ep *ep;
	unsigned long flags;

	spin_lock_irqsave(&port->port_lock_ul, flags);
	if (!port->port_usb || !d->rx_req) {
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		pr_err("%s: port->port_usb is NULL", __func__);
		return;
	}

	ep = port->port_usb->out;
	spin_unlock_irqrestore(&port->port_lock_ul, flags);
	pr_debug("%s: enqueue\n", __func__);
	status = usb_ep_queue(ep, d->rx_req, GFP_ATOMIC);
	if (status)
		pr_err("%s: error enqueuing transfer, %d\n", __func__, status);
}

static void gbam_start_endless_tx(struct gbam_port *port)
{
	struct bam_ch_info *d = &port->data_ch;
	int status;
	struct usb_ep *ep;
	unsigned long flags;

	spin_lock_irqsave(&port->port_lock_dl, flags);
	if (!port->port_usb || !d->tx_req) {
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		pr_err("%s: port->port_usb is NULL", __func__);
		return;
	}

	ep = port->port_usb->in;
	spin_unlock_irqrestore(&port->port_lock_dl, flags);
	pr_debug("%s: enqueue\n", __func__);
	status = usb_ep_queue(ep, d->tx_req, GFP_ATOMIC);
	if (status)
		pr_err("%s: error enqueuing transfer, %d\n", __func__, status);
}

static void gbam_stop_endless_rx(struct gbam_port *port)
{
	struct bam_ch_info *d = &port->data_ch;
	int status;
	unsigned long flags;
	struct usb_ep *ep;

	spin_lock_irqsave(&port->port_lock_ul, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		pr_err("%s: port->port_usb is NULL", __func__);
		return;
	}

	ep = port->port_usb->out;
	spin_unlock_irqrestore(&port->port_lock_ul, flags);
	pr_debug("%s: dequeue\n", __func__);
	status = usb_ep_dequeue(ep, d->rx_req);
	if (status)
		pr_err("%s: error dequeuing transfer, %d\n", __func__, status);
}

static void gbam_stop_endless_tx(struct gbam_port *port)
{
	struct bam_ch_info *d = &port->data_ch;
	int status;
	unsigned long flags;
	struct usb_ep *ep;

	spin_lock_irqsave(&port->port_lock_dl, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		pr_err("%s: port->port_usb is NULL", __func__);
		return;
	}

	ep = port->port_usb->in;
	spin_unlock_irqrestore(&port->port_lock_dl, flags);
	pr_debug("%s: dequeue\n", __func__);
	status = usb_ep_dequeue(ep, d->tx_req);
	if (status)
		pr_err("%s: error dequeuing transfer, %d\n", __func__, status);
}


/*
 * This function configured data fifo based on index passed to get bam2bam
 * configuration.
 */
static void configure_data_fifo(enum usb_ctrl bam_type, u8 idx,
		struct usb_ep *ep, enum usb_bam_pipe_type pipe_type)
{
	struct u_bam_data_connect_info bam_info;
	struct sps_mem_buffer data_fifo = {0};

	if (pipe_type == USB_BAM_PIPE_BAM2BAM) {
		get_bam2bam_connection_info(bam_type, idx,
				&bam_info.usb_bam_pipe_idx,
				NULL, &data_fifo, NULL);

		msm_data_fifo_config(ep,
				data_fifo.phys_base,
				data_fifo.size,
				bam_info.usb_bam_pipe_idx);
	}
}


static void gbam_start(void *param, enum usb_bam_pipe_dir dir)
{
	struct gbam_port *port = param;
	struct usb_gadget *gadget = NULL;
	struct bam_ch_info *d;
	unsigned long flags;

	if (port == NULL) {
		pr_err("%s: port is NULL\n", __func__);
		return;
	}

	spin_lock_irqsave(&port->port_lock, flags);
	if (port->port_usb == NULL) {
		pr_err("%s: port_usb is NULL, disconnected\n", __func__);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	gadget = port->port_usb->gadget;
	d = &port->data_ch;
	spin_unlock_irqrestore(&port->port_lock, flags);

	if (gadget == NULL) {
		pr_err("%s: gadget is NULL\n", __func__);
		return;
	}

	if (dir == USB_TO_PEER_PERIPHERAL) {
		if (port->data_ch.src_pipe_type == USB_BAM_PIPE_BAM2BAM)
			gbam_start_endless_rx(port);
		else {
			gbam_start_rx(port);
			queue_work(gbam_wq, &d->write_tobam_w);
		}
	} else {
		if (gadget_is_dwc3(gadget) &&
		    msm_dwc3_reset_ep_after_lpm(gadget)) {
			configure_data_fifo(d->usb_bam_type,
				d->dst_connection_idx,
				port->port_usb->in, d->dst_pipe_type);
		}
		gbam_start_endless_tx(port);
	}
}

static void gbam_stop(void *param, enum usb_bam_pipe_dir dir)
{
	struct gbam_port *port = param;

	if (dir == USB_TO_PEER_PERIPHERAL) {
		/*
		 * Only handling BAM2BAM, as there is no equivelant to
		 * gbam_stop_endless_rx() for the SYS2BAM use case
		 */
		if (port->data_ch.src_pipe_type == USB_BAM_PIPE_BAM2BAM)
			gbam_stop_endless_rx(port);
	} else {
		gbam_stop_endless_tx(port);
	}
}

static int _gbam_start_io(struct gbam_port *port, bool in)
{
	unsigned long		flags;
	int			ret = 0;
	struct usb_ep		*ep;
	struct list_head	*idle;
	unsigned		queue_size;
	spinlock_t		*spinlock;
	void		(*ep_complete)(struct usb_ep *, struct usb_request *);

	if (in)
		spinlock = &port->port_lock_dl;
	else
		spinlock = &port->port_lock_ul;

	spin_lock_irqsave(spinlock, flags);
	if (!port->port_usb) {
		spin_unlock_irqrestore(spinlock, flags);
		return -EBUSY;
	}

	if (in) {
		ep = port->port_usb->in;
		idle = &port->data_ch.tx_idle;
		queue_size = bam_mux_tx_q_size;
		ep_complete = gbam_epin_complete;
	} else {
		ep = port->port_usb->out;
		if (!ep)
			goto out;
		idle = &port->data_ch.rx_idle;
		queue_size = bam_mux_rx_q_size;
		ep_complete = gbam_epout_complete;
	}

	ret = gbam_alloc_requests(ep, idle, queue_size, ep_complete,
			GFP_ATOMIC);
out:
	spin_unlock_irqrestore(spinlock, flags);
	if (ret)
		pr_err("%s: allocation failed\n", __func__);

	return ret;
}

static void gbam_start_io(struct gbam_port *port)
{
	unsigned long		flags;

	pr_debug("%s: port:%pK\n", __func__, port);

	if (_gbam_start_io(port, true))
		return;

	if (_gbam_start_io(port, false)) {
		spin_lock_irqsave(&port->port_lock_dl, flags);
		if (port->port_usb)
			gbam_free_requests(port->port_usb->in,
				&port->data_ch.tx_idle);
		spin_unlock_irqrestore(&port->port_lock_dl, flags);
		return;
	}

	/* queue out requests */
	gbam_start_rx(port);
}

static void gbam_notify(void *p, int event, unsigned long data)
{
	struct gbam_port	*port = p;
	struct bam_ch_info *d;
	struct sk_buff *skb;

	if (port == NULL)
		pr_err("BAM DMUX notifying after channel close\n");

	switch (event) {
	case BAM_DMUX_RECEIVE:
		skb = (struct sk_buff *)data;
		if (port)
			gbam_data_recv_cb(p, skb);
		else
			dev_kfree_skb_any(skb);
		break;
	case BAM_DMUX_WRITE_DONE:
		skb = (struct sk_buff *)data;
		if (port)
			gbam_data_write_done(p, skb);
		else
			dev_kfree_skb_any(skb);
		break;
	case BAM_DMUX_TRANSMIT_SIZE:
		d = &port->data_ch;
		if (test_bit(BAM_CH_OPENED, &d->flags))
			pr_warn("%s, BAM channel opened already", __func__);
		bam_mux_rx_req_size = data;
		pr_debug("%s rx_req_size: %lu", __func__, bam_mux_rx_req_size);
		break;
	}
}

static void gbam_free_rx_buffers(struct gbam_port *port)
{
	struct sk_buff		*skb;
	unsigned long		flags;
	struct bam_ch_info	*d;

	spin_lock_irqsave(&port->port_lock_ul, flags);

	if (!port->port_usb || !port->port_usb->out)
		goto free_rx_buf_out;

	d = &port->data_ch;
	gbam_free_requests(port->port_usb->out, &d->rx_idle);

	while ((skb = __skb_dequeue(&d->rx_skb_q)))
		dev_kfree_skb_any(skb);

	gbam_free_rx_skb_idle_list(port);

free_rx_buf_out:
	spin_unlock_irqrestore(&port->port_lock_ul, flags);
}

static void gbam_free_tx_buffers(struct gbam_port *port)
{
	struct sk_buff		*skb;
	unsigned long		flags;
	struct bam_ch_info	*d;

	spin_lock_irqsave(&port->port_lock_dl, flags);

	if (!port->port_usb)
		goto free_tx_buf_out;

	d = &port->data_ch;
	gbam_free_requests(port->port_usb->in, &d->tx_idle);

	while ((skb = __skb_dequeue(&d->tx_skb_q)))
		dev_kfree_skb_any(skb);

free_tx_buf_out:
	spin_unlock_irqrestore(&port->port_lock_dl, flags);
}

static void gbam_free_buffers(struct gbam_port *port)
{
	gbam_free_rx_buffers(port);
	gbam_free_tx_buffers(port);
}

static void gbam_disconnect_work(struct work_struct *w)
{
	struct gbam_port *port =
			container_of(w, struct gbam_port, disconnect_w);
	struct bam_ch_info *d = &port->data_ch;

	if (!test_bit(BAM_CH_OPENED, &d->flags)) {
		pr_err("%s: Bam channel is not opened\n", __func__);
		goto exit;
	}

	msm_bam_dmux_close(d->id);
	clear_bit(BAM_CH_OPENED, &d->flags);
exit:
	return;
}

static void gbam2bam_disconnect_work(struct work_struct *w)
{
	struct gbam_port *port =
			container_of(w, struct gbam_port, disconnect_w);
	struct bam_ch_info *d;
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&port->port_lock, flags);

	if (!port->is_connected) {
		pr_debug("%s: Port already disconnected. Bailing out.\n",
			__func__);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	port->is_connected = false;
	d = &port->data_ch;

	/*
	 * Unlock the port here and not at the end of this work,
	 * because we do not want to activate usb_bam, ipa and
	 * tethe bridge logic in atomic context and wait uneeded time.
	 * Either way other works will not fire until end of this work
	 * and event functions (as bam_data_connect) will not influance
	 * while lower layers connect pipes, etc.
	*/
	spin_unlock_irqrestore(&port->port_lock, flags);

	if (d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		ret = usb_bam_disconnect_ipa(d->usb_bam_type, &d->ipa_params);
		if (ret)
			pr_err("%s: usb_bam_disconnect_ipa failed: err:%d\n",
				__func__, ret);
		usb_bam_free_fifos(d->usb_bam_type, d->src_connection_idx);
		usb_bam_free_fifos(d->usb_bam_type, d->dst_connection_idx);
		teth_bridge_disconnect(d->ipa_params.src_client);
		/*
		 * Decrement usage count which was incremented upon cable
		 * connect or cable disconnect in suspended state
		 */
		usb_gadget_autopm_put_async(port->gadget);
	}
}

static void gbam_connect_work(struct work_struct *w)
{
	struct gbam_port *port = container_of(w, struct gbam_port, connect_w);
	struct bam_ch_info *d = &port->data_ch;
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&port->port_lock_ul, flags);
	spin_lock(&port->port_lock_dl);
	if (!port->port_usb) {
		spin_unlock(&port->port_lock_dl);
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
		return;
	}
	spin_unlock(&port->port_lock_dl);
	spin_unlock_irqrestore(&port->port_lock_ul, flags);

	if (!test_bit(BAM_CH_READY, &d->flags)) {
		pr_err("%s: Bam channel is not ready\n", __func__);
		return;
	}

	ret = msm_bam_dmux_open(d->id, port, gbam_notify);
	if (ret) {
		pr_err("%s: unable open bam ch:%d err:%d\n",
				__func__, d->id, ret);
		return;
	}

	set_bit(BAM_CH_OPENED, &d->flags);

	gbam_start_io(port);

	pr_debug("%s: done\n", __func__);
}

static void gbam2bam_connect_work(struct work_struct *w)
{
	struct gbam_port *port = container_of(w, struct gbam_port, connect_w);
	struct usb_gadget *gadget = NULL;
	struct teth_bridge_connect_params connect_params;
	struct teth_bridge_init_params teth_bridge_params;
	struct bam_ch_info *d;
	u32 sps_params;
	int ret;
	unsigned long flags, flags_ul;

	spin_lock_irqsave(&port->port_lock, flags);

	if (port->last_event == U_BAM_DISCONNECT_E) {
		pr_debug("%s: Port is about to disconnected. Bailing out.\n",
			__func__);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	if (port->is_connected) {
		pr_debug("%s: Port already connected. Bail out.\n",
			__func__);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}
	port->is_connected = true;

	spin_lock_irqsave(&port->port_lock_ul, flags_ul);
	spin_lock(&port->port_lock_dl);
	if (!port->port_usb) {
		pr_debug("%s: usb cable is disconnected, exiting\n", __func__);
		spin_unlock(&port->port_lock_dl);
		spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	gadget = port->port_usb->gadget;
	if (!gadget) {
		spin_unlock(&port->port_lock_dl);
		spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_err("%s: port_usb.gadget is NULL, exiting\n", __func__);
		return;
	}
	d = &port->data_ch;

	/*
	 * Unlock the port here and not at the end of this work,
	 * because we do not want to activate usb_bam, ipa and
	 * tethe bridge logic in atomic context and wait uneeded time.
	 * Either way other works will not fire until end of this work
	 * and event functions (as bam_data_connect) will not influance
	 * while lower layers connect pipes, etc.
	*/
	spin_unlock(&port->port_lock_dl);
	spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);

	d->ipa_params.usb_connection_speed = gadget->speed;

	/*
	 * Invalidate prod and cons client handles from previous
	 * disconnect.
	 */
	d->ipa_params.cons_clnt_hdl = -1;
	d->ipa_params.prod_clnt_hdl = -1;

	if (usb_bam_get_pipe_type(d->usb_bam_type, d->ipa_params.src_idx,
			&d->src_pipe_type) ||
		usb_bam_get_pipe_type(d->usb_bam_type, d->ipa_params.dst_idx,
				&d->dst_pipe_type)) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_err("%s:usb_bam_get_pipe_type() failed\n", __func__);
		return;
	}
	if (d->dst_pipe_type != USB_BAM_PIPE_BAM2BAM) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_err("%s: no software preparation for DL not using bam2bam\n",
				__func__);
		return;
	}

	spin_unlock_irqrestore(&port->port_lock, flags);
	usb_bam_alloc_fifos(d->usb_bam_type, d->src_connection_idx);
	usb_bam_alloc_fifos(d->usb_bam_type, d->dst_connection_idx);

	spin_lock_irqsave(&port->port_lock, flags);
	/* check if USB cable is disconnected or not */
	if (!port || !port->port_usb) {
		pr_debug("%s: cable is disconnected.\n",
						 __func__);
		spin_unlock_irqrestore(&port->port_lock,
							flags);
		goto free_fifos;
	}
	if (gadget_is_dwc3(gadget)) {
		/* Configure for RX */
		configure_data_fifo(d->usb_bam_type, d->src_connection_idx,
				    port->port_usb->out, d->src_pipe_type);
		sps_params = MSM_SPS_MODE | MSM_DISABLE_WB | MSM_PRODUCER |
								d->src_pipe_idx;
		d->rx_req->length = 32*1024;
		d->rx_req->udc_priv = sps_params;
		msm_ep_config(port->port_usb->out, d->rx_req);

		/* Configure for TX */
		configure_data_fifo(d->usb_bam_type, d->dst_connection_idx,
				    port->port_usb->in, d->dst_pipe_type);
		sps_params = MSM_SPS_MODE | MSM_DISABLE_WB | d->dst_pipe_idx;
		d->tx_req->length = 32*1024;
		d->tx_req->udc_priv = sps_params;
		msm_ep_config(port->port_usb->in, d->tx_req);

	} else {
		/* Configure for RX */
		get_bam2bam_connection_info(d->usb_bam_type,
				d->src_connection_idx,
				&d->src_pipe_idx,
				NULL, NULL, NULL);
		sps_params = (MSM_SPS_MODE | d->src_pipe_idx |
				MSM_VENDOR_ID) & ~MSM_IS_FINITE_TRANSFER;
		d->rx_req->udc_priv = sps_params;

		/* Configure for TX */
		get_bam2bam_connection_info(d->usb_bam_type,
				d->dst_connection_idx,
				&d->dst_pipe_idx,
				NULL, NULL, NULL);
		sps_params = (MSM_SPS_MODE | d->dst_pipe_idx |
				MSM_VENDOR_ID) & ~MSM_IS_FINITE_TRANSFER;
		d->tx_req->udc_priv = sps_params;

	}

	teth_bridge_params.client = d->ipa_params.src_client;
	ret = teth_bridge_init(&teth_bridge_params);
	if (ret) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_err("%s:teth_bridge_init() failed\n", __func__);
		goto ep_unconfig;
	}

	/* Support for UL using system-to-IPA */
	if (d->src_pipe_type == USB_BAM_PIPE_SYS2BAM) {
		d->ul_params.teth_priv =
			teth_bridge_params.private_data;
		d->ul_params.teth_cb =
			teth_bridge_params.usb_notify_cb;
		d->ipa_params.notify = gbam_ipa_sys2bam_notify_cb;
		d->ipa_params.priv = &d->ul_params;
		d->ipa_params.reset_pipe_after_lpm = false;

	} else {
		d->ipa_params.notify =
			teth_bridge_params.usb_notify_cb;
		d->ipa_params.priv =
			teth_bridge_params.private_data;
		d->ipa_params.reset_pipe_after_lpm =
			(gadget_is_dwc3(gadget) &&
			 msm_dwc3_reset_ep_after_lpm(gadget));
	}
	d->ipa_params.ipa_ep_cfg.mode.mode = IPA_BASIC;
	d->ipa_params.skip_ep_cfg = teth_bridge_params.skip_ep_cfg;
	d->ipa_params.dir = USB_TO_PEER_PERIPHERAL;
	spin_unlock_irqrestore(&port->port_lock, flags);
	ret = usb_bam_connect_ipa(d->usb_bam_type, &d->ipa_params);
	if (ret) {
		pr_err("%s: usb_bam_connect_ipa failed: err:%d\n",
			__func__, ret);
		goto ep_unconfig;
	}

	spin_lock_irqsave(&port->port_lock, flags);
	/* check if USB cable is disconnected or not */
	if (port->last_event ==  U_BAM_DISCONNECT_E) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_debug("%s:%d: cable is disconnected.\n",
						 __func__, __LINE__);
		goto ep_unconfig;
	}

	/* Remove support for UL using system-to-IPA towards DL */
	if (d->src_pipe_type == USB_BAM_PIPE_SYS2BAM) {
		d->ipa_params.notify = d->ul_params.teth_cb;
		d->ipa_params.priv = d->ul_params.teth_priv;
	}
	if (d->dst_pipe_type == USB_BAM_PIPE_BAM2BAM)
		d->ipa_params.reset_pipe_after_lpm =
			(gadget_is_dwc3(gadget) &&
			 msm_dwc3_reset_ep_after_lpm(gadget));
	else
		d->ipa_params.reset_pipe_after_lpm = false;
	d->ipa_params.dir = PEER_PERIPHERAL_TO_USB;
	spin_unlock_irqrestore(&port->port_lock, flags);
	ret = usb_bam_connect_ipa(d->usb_bam_type, &d->ipa_params);
	if (ret) {
		pr_err("%s: usb_bam_connect_ipa failed: err:%d\n",
			__func__, ret);
		goto ep_unconfig;
	}

	spin_lock_irqsave(&port->port_lock, flags);
	/* check if USB cable is disconnected or not */
	if (port->last_event ==  U_BAM_DISCONNECT_E) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		pr_debug("%s:%d: cable is disconnected.\n",
						 __func__, __LINE__);
		goto ep_unconfig;
	}

	spin_unlock_irqrestore(&port->port_lock, flags);
	gqti_ctrl_update_ipa_pipes(port->port_usb, port->port_num,
					d->ipa_params.ipa_prod_ep_idx ,
					d->ipa_params.ipa_cons_ep_idx);

	connect_params.ipa_usb_pipe_hdl = d->ipa_params.prod_clnt_hdl;
	connect_params.usb_ipa_pipe_hdl = d->ipa_params.cons_clnt_hdl;
	connect_params.tethering_mode = TETH_TETHERING_MODE_RMNET;
	connect_params.client_type = d->ipa_params.src_client;
	ret = teth_bridge_connect(&connect_params);
	if (ret) {
		pr_err("%s:teth_bridge_connect() failed\n", __func__);
		goto ep_unconfig;
	}

	/* queue in & out requests */
	if (d->src_pipe_type == USB_BAM_PIPE_BAM2BAM) {
		gbam_start_endless_rx(port);
	} else {
		/* The use-case of UL (OUT) ports using sys2bam is based on
		 * partial reuse of the system-to-bam_demux code. The following
		 * lines perform the branching out of the standard bam2bam flow
		 * on the USB side of the UL channel
		 */
		if (_gbam_start_io(port, false)) {
			pr_err("%s: _gbam_start_io failed\n", __func__);
			return;
		}
		gbam_start_rx(port);
	}
	gbam_start_endless_tx(port);

	pr_debug("%s: done\n", __func__);
	return;

ep_unconfig:
	if (gadget_is_dwc3(gadget)) {
		spin_lock_irqsave(&port->port_lock, flags);
		/* check if USB cable is disconnected or not */
		if (port->port_usb) {
			msm_ep_unconfig(port->port_usb->in);
			msm_ep_unconfig(port->port_usb->out);
		}
		spin_unlock_irqrestore(&port->port_lock, flags);
	}
free_fifos:
	usb_bam_free_fifos(d->usb_bam_type, d->src_connection_idx);
	usb_bam_free_fifos(d->usb_bam_type, d->dst_connection_idx);

}

static int gbam_wake_cb(void *param)
{
	struct gbam_port	*port = (struct gbam_port *)param;
	struct usb_gadget	*gadget;
	unsigned long flags;

	spin_lock_irqsave(&port->port_lock, flags);
	if (!port->port_usb) {
		pr_debug("%s: usb cable is disconnected, exiting\n",
				__func__);
		spin_unlock_irqrestore(&port->port_lock, flags);
		return -ENODEV;
	}

	gadget = port->port_usb->gadget;
	spin_unlock_irqrestore(&port->port_lock, flags);

	pr_debug("%s: woken up by peer\n", __func__);

	return usb_gadget_wakeup(gadget);
}

static void gbam2bam_suspend_work(struct work_struct *w)
{
	struct gbam_port *port = container_of(w, struct gbam_port, suspend_w);
	struct bam_ch_info *d;
	int ret;
	unsigned long flags;

	pr_debug("%s: suspend work started\n", __func__);

	spin_lock_irqsave(&port->port_lock, flags);

	if ((port->last_event == U_BAM_DISCONNECT_E) ||
	    (port->last_event == U_BAM_RESUME_E)) {
		pr_debug("%s: Port is about to disconnect/resume. Bail out\n",
			__func__);
		goto exit;
	}

	d = &port->data_ch;

	ret = usb_bam_register_wake_cb(d->usb_bam_type, d->dst_connection_idx,
					gbam_wake_cb, port);
	if (ret) {
		pr_err("%s(): Failed to register BAM wake callback.\n",
			__func__);
		goto exit;
	}

	if (d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		usb_bam_register_start_stop_cbs(d->usb_bam_type,
			 d->dst_connection_idx, gbam_start, gbam_stop, port);

		/*
		 * release lock here because gbam_start() or
		 * gbam_stop() called from usb_bam_suspend()
		 * re-acquires port lock.
		 */
		spin_unlock_irqrestore(&port->port_lock, flags);
		usb_bam_suspend(d->usb_bam_type, &d->ipa_params);
		spin_lock_irqsave(&port->port_lock, flags);
	}

exit:
	/*
	 * Decrement usage count after IPA handshake is done to allow gadget
	 * parent to go to lpm. This counter was incremented upon cable connect
	 */
	usb_gadget_autopm_put_async(port->gadget);

	spin_unlock_irqrestore(&port->port_lock, flags);
}

static void gbam2bam_resume_work(struct work_struct *w)
{
	struct gbam_port *port = container_of(w, struct gbam_port, resume_w);
	struct bam_ch_info *d;
	struct usb_gadget *gadget = NULL;
	int ret;
	unsigned long flags;

	pr_debug("%s: resume work started\n", __func__);

	spin_lock_irqsave(&port->port_lock, flags);

	if (port->last_event == U_BAM_DISCONNECT_E || !port->port_usb) {
		pr_debug("%s: usb cable is disconnected, exiting\n",
			__func__);
		goto exit;
	}

	d = &port->data_ch;
	gadget = port->port_usb->gadget;

	ret = usb_bam_register_wake_cb(d->usb_bam_type, d->dst_connection_idx,
					NULL, NULL);
	if (ret) {
		pr_err("%s(): Failed to register BAM wake callback.\n",
			__func__);
		goto exit;
	}

	if (d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		if (gadget_is_dwc3(gadget) &&
			msm_dwc3_reset_ep_after_lpm(gadget)) {
				configure_data_fifo(d->usb_bam_type,
					d->src_connection_idx,
					port->port_usb->out, d->src_pipe_type);
				configure_data_fifo(d->usb_bam_type,
					d->dst_connection_idx,
					port->port_usb->in, d->dst_pipe_type);
				spin_unlock_irqrestore(&port->port_lock, flags);
				msm_dwc3_reset_dbm_ep(port->port_usb->in);
				spin_lock_irqsave(&port->port_lock, flags);
		}
		usb_bam_resume(d->usb_bam_type, &d->ipa_params);
	}

exit:
	spin_unlock_irqrestore(&port->port_lock, flags);
}

/* BAM data channel ready, allow attempt to open */
static int gbam_data_ch_probe(struct platform_device *pdev)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	int			i;
	unsigned long		flags;
	bool			do_work = false;

	pr_debug("%s: name:%s\n", __func__, pdev->name);

	for (i = 0; i < n_bam_ports; i++) {
		port = bam_ports[i].port;
		d = &port->data_ch;

		if (!strncmp(bam_ch_names[i], pdev->name,
					BAM_DMUX_CH_NAME_MAX_LEN)) {
			set_bit(BAM_CH_READY, &d->flags);

			/* if usb is online, try opening bam_ch */
			spin_lock_irqsave(&port->port_lock_ul, flags);
			spin_lock(&port->port_lock_dl);
			if (port->port_usb)
				do_work = true;
			spin_unlock(&port->port_lock_dl);
			spin_unlock_irqrestore(&port->port_lock_ul, flags);

			if (do_work)
				queue_work(gbam_wq, &port->connect_w);
			break;
		}
	}

	return 0;
}

/* BAM data channel went inactive, so close it */
static int gbam_data_ch_remove(struct platform_device *pdev)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	struct usb_ep		*ep_in = NULL;
	struct usb_ep		*ep_out = NULL;
	unsigned long		flags;
	int			i;

	pr_debug("%s: name:%s\n", __func__, pdev->name);

	for (i = 0; i < n_bam_ports; i++) {
		if (!strncmp(bam_ch_names[i], pdev->name,
					BAM_DMUX_CH_NAME_MAX_LEN)) {
			port = bam_ports[i].port;
			d = &port->data_ch;

			spin_lock_irqsave(&port->port_lock_ul, flags);
			spin_lock(&port->port_lock_dl);
			if (port->port_usb) {
				ep_in = port->port_usb->in;
				ep_out = port->port_usb->out;
			}
			spin_unlock(&port->port_lock_dl);
			spin_unlock_irqrestore(&port->port_lock_ul, flags);

			if (ep_in)
				usb_ep_fifo_flush(ep_in);
			if (ep_out)
				usb_ep_fifo_flush(ep_out);

			gbam_free_buffers(port);

			msm_bam_dmux_close(d->id);

			/* bam dmux will free all pending skbs */
			d->pending_pkts_with_bam = 0;
			d->pending_bytes_with_bam = 0;

			clear_bit(BAM_CH_READY, &d->flags);
			clear_bit(BAM_CH_OPENED, &d->flags);
		}
	}

	return 0;
}

static void gbam_port_free(int portno)
{
	struct gbam_port *port = bam_ports[portno].port;
	struct platform_driver *pdrv = &bam_ports[portno].pdrv;

	if (port) {
		kfree(port);
		platform_driver_unregister(pdrv);
	}
}

static void gbam2bam_port_free(int portno)
{
	struct gbam_port *port = bam2bam_ports[portno];

	kfree(port);
}

static int gbam_port_alloc(int portno)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	struct platform_driver	*pdrv;

	port = kzalloc(sizeof(struct gbam_port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->port_num = portno;

	/* port initialization */
	port->is_connected = false;
	spin_lock_init(&port->port_lock_ul);
	spin_lock_init(&port->port_lock_dl);
	spin_lock_init(&port->port_lock);
	INIT_WORK(&port->connect_w, gbam_connect_work);
	INIT_WORK(&port->disconnect_w, gbam_disconnect_work);

	/* data ch */
	d = &port->data_ch;
	d->port = port;
	INIT_LIST_HEAD(&d->tx_idle);
	INIT_LIST_HEAD(&d->rx_idle);
	INIT_WORK(&d->write_tobam_w, gbam_data_write_tobam);
	INIT_WORK(&d->write_tohost_w, gbam_write_data_tohost_w);
	skb_queue_head_init(&d->tx_skb_q);
	skb_queue_head_init(&d->rx_skb_q);
	skb_queue_head_init(&d->rx_skb_idle);
	d->id = bam_ch_ids[portno];

	bam_ports[portno].port = port;

	scnprintf(bam_ch_names[portno], BAM_DMUX_CH_NAME_MAX_LEN,
			"bam_dmux_ch_%d", bam_ch_ids[portno]);
	pdrv = &bam_ports[portno].pdrv;
	pdrv->probe = gbam_data_ch_probe;
	pdrv->remove = gbam_data_ch_remove;
	pdrv->driver.name = bam_ch_names[portno];
	pdrv->driver.owner = THIS_MODULE;

	platform_driver_register(pdrv);
	pr_debug("%s: port:%pK portno:%d\n", __func__, port, portno);

	return 0;
}

static int gbam2bam_port_alloc(int portno)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;

	port = kzalloc(sizeof(struct gbam_port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->port_num = portno;

	/* port initialization */
	port->is_connected = false;
	spin_lock_init(&port->port_lock_ul);
	spin_lock_init(&port->port_lock_dl);
	spin_lock_init(&port->port_lock);

	INIT_WORK(&port->connect_w, gbam2bam_connect_work);
	INIT_WORK(&port->disconnect_w, gbam2bam_disconnect_work);
	INIT_WORK(&port->suspend_w, gbam2bam_suspend_work);
	INIT_WORK(&port->resume_w, gbam2bam_resume_work);

	/* data ch */
	d = &port->data_ch;
	d->port = port;
	d->ipa_params.src_client = usb_prod[portno];
	d->ipa_params.dst_client = usb_cons[portno];
	bam2bam_ports[portno] = port;

	/* UL workaround requirements */
	skb_queue_head_init(&d->rx_skb_q);
	skb_queue_head_init(&d->rx_skb_idle);
	INIT_LIST_HEAD(&d->rx_idle);
	INIT_WORK(&d->write_tobam_w, gbam_data_write_tobam);

	pr_debug("%s: port:%pK portno:%d\n", __func__, port, portno);

	return 0;
}

#if defined(CONFIG_DEBUG_FS)
#define DEBUG_BUF_SIZE	1024
static ssize_t gbam_read_stats(struct file *file, char __user *ubuf,
		size_t count, loff_t *ppos)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	char			*buf;
	unsigned long		flags;
	int			ret;
	int			i;
	int			temp = 0;

	buf = kzalloc(sizeof(char) * DEBUG_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < n_bam_ports; i++) {
		port = bam_ports[i].port;
		if (!port)
			continue;
		spin_lock_irqsave(&port->port_lock_ul, flags);
		spin_lock(&port->port_lock_dl);

		d = &port->data_ch;

		temp += scnprintf(buf + temp, DEBUG_BUF_SIZE - temp,
				"#PORT:%d port:%pK data_ch:%pK#\n"
				"dpkts_to_usbhost: %lu\n"
				"dpkts_to_modem:  %lu\n"
				"dpkts_pwith_bam: %u\n"
				"dbytes_pwith_bam: %u\n"
				"to_usbhost_dcnt:  %u\n"
				"tomodem__dcnt:  %u\n"
				"rx_flow_control_disable_count: %u\n"
				"rx_flow_control_enable_count: %u\n"
				"rx_flow_control_triggered: %u\n"
				"max_num_pkts_pending_with_bam: %u\n"
				"max_bytes_pending_with_bam: %u\n"
				"delayed_bam_mux_write_done: %u\n"
				"tx_buf_len:	 %u\n"
				"rx_buf_len:	 %u\n"
				"data_ch_open:   %d\n"
				"data_ch_ready:  %d\n"
				"skb_expand_cnt: %lu\n",
				i, port, &port->data_ch,
				d->to_host, d->to_modem,
				d->pending_pkts_with_bam,
				d->pending_bytes_with_bam,
				d->tohost_drp_cnt, d->tomodem_drp_cnt,
				d->rx_flow_control_disable,
				d->rx_flow_control_enable,
				d->rx_flow_control_triggered,
				d->max_num_pkts_pending_with_bam,
				d->max_bytes_pending_with_bam,
				d->delayed_bam_mux_write_done,
				d->tx_skb_q.qlen, d->rx_skb_q.qlen,
				test_bit(BAM_CH_OPENED, &d->flags),
				test_bit(BAM_CH_READY, &d->flags),
				d->skb_expand_cnt);

		spin_unlock(&port->port_lock_dl);
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
	}

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, temp);

	kfree(buf);

	return ret;
}

static ssize_t gbam_reset_stats(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	int			i;
	unsigned long		flags;

	for (i = 0; i < n_bam_ports; i++) {
		port = bam_ports[i].port;
		if (!port)
			continue;

		spin_lock_irqsave(&port->port_lock_ul, flags);
		spin_lock(&port->port_lock_dl);

		d = &port->data_ch;

		d->to_host = 0;
		d->to_modem = 0;
		d->pending_pkts_with_bam = 0;
		d->pending_bytes_with_bam = 0;
		d->tohost_drp_cnt = 0;
		d->tomodem_drp_cnt = 0;
		d->rx_flow_control_disable = 0;
		d->rx_flow_control_enable = 0;
		d->rx_flow_control_triggered = 0;
		d->max_num_pkts_pending_with_bam = 0;
		d->max_bytes_pending_with_bam = 0;
		d->delayed_bam_mux_write_done = 0;
		d->skb_expand_cnt = 0;

		spin_unlock(&port->port_lock_dl);
		spin_unlock_irqrestore(&port->port_lock_ul, flags);
	}
	return count;
}

const struct file_operations gbam_stats_ops = {
	.read = gbam_read_stats,
	.write = gbam_reset_stats,
};

struct dentry *gbam_dent;
static void gbam_debugfs_init(void)
{
	struct dentry *dfile;

	if (gbam_dent)
		return;

	gbam_dent = debugfs_create_dir("usb_rmnet", 0);
	if (!gbam_dent || IS_ERR(gbam_dent))
		return;

	dfile = debugfs_create_file("status", 0444, gbam_dent, 0,
			&gbam_stats_ops);
	if (!dfile || IS_ERR(dfile)) {
		debugfs_remove(gbam_dent);
		gbam_dent = NULL;
		return;
	}
}
static void gbam_debugfs_remove(void)
{
	debugfs_remove_recursive(gbam_dent);
}
#else
static inline void gbam_debugfs_init(void) {}
static inline void gbam_debugfs_remove(void) {}
#endif

void gbam_disconnect(struct grmnet *gr, u8 port_num, enum transport_type trans)
{
	struct gbam_port	*port;
	unsigned long		flags, flags_ul, flags_dl;
	struct bam_ch_info	*d;

	pr_debug("%s: grmnet:%pK port#%d\n", __func__, gr, port_num);

	if (trans == USB_GADGET_XPORT_BAM2BAM) {
		pr_err("%s: invalid xport#%d\n", __func__, trans);
		return;
	}
	if (trans == USB_GADGET_XPORT_BAM_DMUX &&
		port_num >= n_bam_ports) {
		pr_err("%s: invalid bam portno#%d\n",
			   __func__, port_num);
		return;
	}

	if ((trans == USB_GADGET_XPORT_BAM2BAM_IPA) &&
				port_num >= n_bam2bam_ports) {
		pr_err("%s: invalid bam2bam portno#%d\n",
			   __func__, port_num);
		return;
	}

	if (!gr) {
		pr_err("%s: grmnet port is null\n", __func__);
		return;
	}
	if (trans == USB_GADGET_XPORT_BAM_DMUX)
		port = bam_ports[port_num].port;
	else
		port = bam2bam_ports[port_num];

	if (!port) {
		pr_err("%s: NULL port", __func__);
		return;
	}

	spin_lock_irqsave(&port->port_lock, flags);

	d = &port->data_ch;
	/* Already disconnected due to suspend with remote wake disabled */
	if (port->last_event == U_BAM_DISCONNECT_E) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}
	/*
	 * Suspend with remote wakeup enabled. Increment usage
	 * count when disconnect happens in suspended state.
	 * Corresponding decrement happens in the end of this
	 * function if IPA handshake is already done or it is done
	 * in disconnect work after finishing IPA handshake.
	 */
	if (port->last_event == U_BAM_SUSPEND_E)
		usb_gadget_autopm_get_noresume(port->gadget);

	port->port_usb = gr;

	if (trans == USB_GADGET_XPORT_BAM_DMUX)
		gbam_free_buffers(port);
	else if (trans == USB_GADGET_XPORT_BAM2BAM_IPA)
		gbam_free_rx_buffers(port);

	spin_lock_irqsave(&port->port_lock_ul, flags_ul);
	spin_lock(&port->port_lock_dl);
	port->port_usb = 0;
	n_tx_req_queued = 0;
	spin_unlock(&port->port_lock_dl);
	spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);

	usb_ep_disable(gr->in);
	if (trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		spin_lock_irqsave(&port->port_lock_dl, flags_dl);
		if (d->tx_req) {
			usb_ep_free_request(gr->in, d->tx_req);
			d->tx_req = NULL;
		}
		spin_unlock_irqrestore(&port->port_lock_dl, flags_dl);
	}
	/* disable endpoints */
	if (gr->out) {
		usb_ep_disable(gr->out);
		if (trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
			spin_lock_irqsave(&port->port_lock_ul, flags_ul);
			if (d->rx_req) {
				usb_ep_free_request(gr->out, d->rx_req);
				d->rx_req = NULL;
			}
			spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);
		}
	}

	/*
	 * Set endless flag to false as USB Endpoint is already
	 * disable.
	 */
	if (d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) {

		if (d->dst_pipe_type == USB_BAM_PIPE_BAM2BAM)
			gr->in->endless = false;

		if (d->src_pipe_type == USB_BAM_PIPE_BAM2BAM && gr->out)
			gr->out->endless = false;
	}

	gr->in->driver_data = NULL;
	if (gr->out)
		gr->out->driver_data = NULL;

	port->last_event = U_BAM_DISCONNECT_E;
	/* Disable usb irq for CI gadget. It will be enabled in
	 * usb_bam_disconnect_pipe() after disconnecting all pipes
	 * and USB BAM reset is done.
	 */
	if (!gadget_is_dwc3(port->gadget) &&
			(trans == USB_GADGET_XPORT_BAM2BAM_IPA))
		msm_usb_irq_disable(true);

	queue_work(gbam_wq, &port->disconnect_w);

	spin_unlock_irqrestore(&port->port_lock, flags);
}

int gbam_connect(struct grmnet *gr, u8 port_num,
		enum transport_type trans, u8 src_connection_idx,
		u8 dst_connection_idx)
{
	struct gbam_port	*port;
	struct bam_ch_info	*d;
	int			ret;
	unsigned long		flags, flags_ul;

	pr_debug("%s: grmnet:%pK port#%d\n", __func__, gr, port_num);

	if (!gr) {
		pr_err("%s: grmnet port is null\n", __func__);
		return -ENODEV;
	}

	if (!gr->gadget) {
		pr_err("%s: gadget handle not passed\n", __func__);
		return -EINVAL;
	}

	if (trans == USB_GADGET_XPORT_BAM2BAM) {
		pr_err("%s: invalid xport#%d\n", __func__, trans);
		return -EINVAL;
	}

	if (trans == USB_GADGET_XPORT_BAM_DMUX && port_num >= n_bam_ports) {
		pr_err("%s: invalid portno#%d\n", __func__, port_num);
		return -ENODEV;
	}

	if ((trans == USB_GADGET_XPORT_BAM2BAM_IPA)
			&& port_num >= n_bam2bam_ports) {
		pr_err("%s: invalid portno#%d\n", __func__, port_num);
		return -ENODEV;
	}

	if (trans == USB_GADGET_XPORT_BAM_DMUX)
		port = bam_ports[port_num].port;
	else
		port = bam2bam_ports[port_num];

	if (!port) {
		pr_err("%s: NULL port", __func__);
		return -ENODEV;
	}

	spin_lock_irqsave(&port->port_lock, flags);

	d = &port->data_ch;
	d->trans = trans;

	spin_lock_irqsave(&port->port_lock_ul, flags_ul);
	spin_lock(&port->port_lock_dl);
	port->port_usb = gr;
	port->gadget = port->port_usb->gadget;

	if (trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		d->rx_req = usb_ep_alloc_request(port->port_usb->out,
								GFP_ATOMIC);
		if (!d->rx_req) {
			pr_err("%s: RX request allocation failed\n", __func__);
			d->rx_req = NULL;
			spin_unlock(&port->port_lock_dl);
			spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);
			spin_unlock_irqrestore(&port->port_lock, flags);
			return -ENOMEM;
		}

		d->rx_req->context = port;
		d->rx_req->complete = gbam_endless_rx_complete;
		d->rx_req->length = 0;
		d->rx_req->no_interrupt = 1;

		d->tx_req = usb_ep_alloc_request(port->port_usb->in,
								GFP_ATOMIC);
		if (!d->tx_req) {
			pr_err("%s: TX request allocation failed\n", __func__);
			d->tx_req = NULL;
			usb_ep_free_request(port->port_usb->out, d->rx_req);
			d->rx_req = NULL;
			spin_unlock(&port->port_lock_dl);
			spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);
			spin_unlock_irqrestore(&port->port_lock, flags);
			return -ENOMEM;
		}

		d->tx_req->context = port;
		d->tx_req->complete = gbam_endless_tx_complete;
		d->tx_req->length = 0;
		d->tx_req->no_interrupt = 1;
	}

	if (d->trans == USB_GADGET_XPORT_BAM_DMUX) {
		d->to_host = 0;
		d->to_modem = 0;
		d->pending_pkts_with_bam = 0;
		d->pending_bytes_with_bam = 0;
		d->tohost_drp_cnt = 0;
		d->tomodem_drp_cnt = 0;
		d->rx_flow_control_disable = 0;
		d->rx_flow_control_enable = 0;
		d->rx_flow_control_triggered = 0;
		d->max_num_pkts_pending_with_bam = 0;
		d->max_bytes_pending_with_bam = 0;
		d->delayed_bam_mux_write_done = 0;
	}

	spin_unlock(&port->port_lock_dl);
	spin_unlock_irqrestore(&port->port_lock_ul, flags_ul);

	if (d->trans == USB_GADGET_XPORT_BAM2BAM_IPA) {
		d->src_connection_idx = src_connection_idx;
		d->dst_connection_idx = dst_connection_idx;
		d->usb_bam_type = usb_bam_get_bam_type(gr->gadget->name);
		d->ipa_params.src_pipe = &(d->src_pipe_idx);
		d->ipa_params.dst_pipe = &(d->dst_pipe_idx);
		d->ipa_params.src_idx = src_connection_idx;
		d->ipa_params.dst_idx = dst_connection_idx;

		/*
		 * Query pipe type using IPA src/dst index with
		 * usbbam driver. It is being set either as
		 * BAM2BAM or SYS2BAM.
		 */
		if (usb_bam_get_pipe_type(d->usb_bam_type,
				d->ipa_params.src_idx, &d->src_pipe_type) ||
		    usb_bam_get_pipe_type(d->usb_bam_type,
				d->ipa_params.dst_idx, &d->dst_pipe_type)) {
			pr_err("%s:usb_bam_get_pipe_type() failed\n",
				__func__);
			ret = -EINVAL;
			usb_ep_free_request(port->port_usb->out, d->rx_req);
			d->rx_req = NULL;
			usb_ep_free_request(port->port_usb->in, d->tx_req);
			d->tx_req = NULL;
			goto exit;
		}
		/*
		 * Check for pipe_type. If it is BAM2BAM, then it is required
		 * to disable Xfer complete and Xfer not ready interrupts for
		 * that particular endpoint. Hence it set endless flag based
		 * it which is considered into UDC driver while enabling
		 * USB Endpoint.
		 */
		if (d->dst_pipe_type == USB_BAM_PIPE_BAM2BAM)
			port->port_usb->in->endless = true;

		if (d->src_pipe_type == USB_BAM_PIPE_BAM2BAM)
			port->port_usb->out->endless = true;
	}

	ret = usb_ep_enable(gr->in);
	if (ret) {
		pr_err("%s: usb_ep_enable failed eptype:IN ep:%pK",
			__func__, gr->in);
		usb_ep_free_request(port->port_usb->out, d->rx_req);
		d->rx_req = NULL;
		usb_ep_free_request(port->port_usb->in, d->tx_req);
		d->tx_req = NULL;
		if (d->dst_pipe_type == USB_BAM_PIPE_BAM2BAM)
			port->port_usb->in->endless = false;

		if (d->src_pipe_type == USB_BAM_PIPE_BAM2BAM)
			port->port_usb->out->endless = false;
		goto exit;
	}
	gr->in->driver_data = port;

	/*
	 * DPL traffic is routed through BAM-DMUX on some targets.
	 * DPL function has only 1 IN endpoint. Add out endpoint
	 * checks for BAM-DMUX transport.
	 */
	if (gr->out) {
		ret = usb_ep_enable(gr->out);
		if (ret) {
			pr_err("%s: usb_ep_enable failed eptype:OUT ep:%pK",
					__func__, gr->out);
			gr->in->driver_data = 0;
			usb_ep_disable(gr->in);
			usb_ep_free_request(port->port_usb->out, d->rx_req);
			d->rx_req = NULL;
			usb_ep_free_request(port->port_usb->in, d->tx_req);
			d->tx_req = NULL;
			if (d->dst_pipe_type == USB_BAM_PIPE_BAM2BAM)
				port->port_usb->in->endless = false;

			if (d->src_pipe_type == USB_BAM_PIPE_BAM2BAM)
				port->port_usb->out->endless = false;
			goto exit;
		}
		gr->out->driver_data = port;
	}

	port->last_event = U_BAM_CONNECT_E;
	/*
	 * Increment usage count upon cable connect. Decrement after IPA
	 * handshake is done in disconnect work (due to cable disconnect)
	 * or in suspend work.
	 */
	if (trans == USB_GADGET_XPORT_BAM2BAM_IPA)
		usb_gadget_autopm_get_noresume(port->gadget);
	queue_work(gbam_wq, &port->connect_w);

	ret = 0;
exit:
	spin_unlock_irqrestore(&port->port_lock, flags);
	return ret;
}

void gbam_data_flush_workqueue(void)
{
	pr_debug("%s(): Flushing workqueue\n", __func__);
	flush_workqueue(gbam_wq);
}

int gbam_setup(unsigned int no_bam_port)
{
	int	i;
	int	ret;
	int	bam_port_start = n_bam_ports;
	int	total_bam_ports = bam_port_start + no_bam_port;

	pr_debug("%s: requested BAM ports:%d\n", __func__, no_bam_port);

	if (!no_bam_port || total_bam_ports > BAM_N_PORTS) {
		pr_err("%s: Invalid num of ports count:%d\n",
				__func__, no_bam_port);
		return -EINVAL;
	}

	if (!gbam_wq) {
		gbam_wq = alloc_workqueue("k_gbam", WQ_UNBOUND |
					WQ_MEM_RECLAIM, 1);
		if (!gbam_wq) {
			pr_err("%s: Unable to create workqueue gbam_wq\n",
					__func__);
			return -ENOMEM;
		}
	}

	for (i = bam_port_start; i < (bam_port_start + no_bam_port); i++) {
		n_bam_ports++;
		pr_debug("gbam_port_alloc called for %d\n", i);
		ret = gbam_port_alloc(i);
		if (ret) {
			n_bam_ports--;
			pr_err("%s: Unable to alloc port:%d\n", __func__, i);
			goto free_bam_ports;
		}
	}

	gbam_debugfs_init();

	return bam_port_start;

free_bam_ports:
	for (i = 0; i < n_bam_ports; i++)
		gbam_port_free(i);
	destroy_workqueue(gbam_wq);

	return ret;
}

int gbam2bam_setup(unsigned int no_bam2bam_port)
{
	int	i;
	int	ret;
	int	bam2bam_port_start = n_bam2bam_ports;
	int	total_bam2bam_ports = bam2bam_port_start + no_bam2bam_port;

	pr_debug("%s: requested BAM2BAM ports:%d\n", __func__, no_bam2bam_port);

	if (!no_bam2bam_port || total_bam2bam_ports > BAM2BAM_N_PORTS) {
		pr_err("%s: Invalid num of ports count:%d\n",
				__func__, no_bam2bam_port);
		return -EINVAL;
	}

	if (!gbam_wq) {
		gbam_wq = alloc_workqueue("k_gbam", WQ_UNBOUND |
					WQ_MEM_RECLAIM, 1);
		if (!gbam_wq) {
			pr_err("%s: Unable to create workqueue gbam_wq\n",
					__func__);
			return -ENOMEM;
		}
	}

	for (i = bam2bam_port_start; i < (bam2bam_port_start +
				no_bam2bam_port); i++) {
		n_bam2bam_ports++;
		ret = gbam2bam_port_alloc(i);
		if (ret) {
			n_bam2bam_ports--;
			pr_err("%s: Unable to alloc port:%d\n", __func__, i);
			goto free_bam2bam_ports;
		}
	}

	gbam_debugfs_init();

	return bam2bam_port_start;

free_bam2bam_ports:
	for (i = 0; i < n_bam2bam_ports; i++)
		gbam2bam_port_free(i);
	destroy_workqueue(gbam_wq);

	return ret;
}

void gbam_cleanup(void)
{
	gbam_debugfs_remove();
}

void gbam_suspend(struct grmnet *gr, u8 port_num, enum transport_type trans)
{
	struct gbam_port	*port;
	struct bam_ch_info *d;
	unsigned long flags;

	if (trans != USB_GADGET_XPORT_BAM2BAM_IPA)
		return;

	port = bam2bam_ports[port_num];

	if (!port) {
		pr_err("%s: NULL port", __func__);
		return;
	}

	spin_lock_irqsave(&port->port_lock, flags);

	d = &port->data_ch;

	pr_debug("%s: suspended port %d\n", __func__, port_num);

	port->last_event = U_BAM_SUSPEND_E;
	queue_work(gbam_wq, &port->suspend_w);

	spin_unlock_irqrestore(&port->port_lock, flags);
}

void gbam_resume(struct grmnet *gr, u8 port_num, enum transport_type trans)
{
	struct gbam_port	*port;
	struct bam_ch_info *d;
	unsigned long flags;

	if (trans != USB_GADGET_XPORT_BAM2BAM_IPA)
		return;

	port = bam2bam_ports[port_num];

	if (!port) {
		pr_err("%s: NULL port", __func__);
		return;
	}

	spin_lock_irqsave(&port->port_lock, flags);

	d = &port->data_ch;

	pr_debug("%s: resumed port %d\n", __func__, port_num);

	port->last_event = U_BAM_RESUME_E;
	/*
	 * Increment usage count here to disallow gadget parent suspend.
	 * This counter will decrement after IPA handshake is done in
	 * disconnect work (due to cable disconnect) or in bam_disconnect
	 * in suspended state.
	 */
	usb_gadget_autopm_get_noresume(port->gadget);
	queue_work(gbam_wq, &port->resume_w);

	spin_unlock_irqrestore(&port->port_lock, flags);
}

int gbam_mbim_connect(struct usb_gadget *g, struct usb_ep *in,
			struct usb_ep *out)
{
	struct grmnet *gr;

	gr = kzalloc(sizeof(*gr), GFP_ATOMIC);
	if (!gr)
		return -ENOMEM;
	gr->in = in;
	gr->out = out;
	gr->gadget = g;

	return gbam_connect(gr, 0, USB_GADGET_XPORT_BAM_DMUX, 0, 0);
}

void gbam_mbim_disconnect(void)
{
	struct gbam_port *port = bam_ports[0].port;
	struct grmnet *gr = port->port_usb;

	if (!gr) {
		pr_err("%s: port_usb is NULL\n", __func__);
		return;
	}

	gbam_disconnect(gr, 0, USB_GADGET_XPORT_BAM_DMUX);
	kfree(gr);
}

int gbam_mbim_setup(void)
{
	int ret = 0;

	/*
	 * MBIM requires only 1 USB_GADGET_XPORT_BAM_DMUX
	 * port. The port is always 0 and is shared
	 * between RMNET and MBIM.
	 */
	if (!n_bam_ports)
		ret = gbam_setup(1);

	return ret;
}
