/* Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#ifndef GSI_H
#define GSI_H

#include <linux/device.h>
#include <linux/types.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/msm_gsi.h>

#define GSI_MAX_CHAN      31
#define GSI_MAX_EVT_RING  23
#define GSI_NO_EVT_ERINDEX 31

#define gsi_readl(c)	({ u32 __v = readl_relaxed(c); __iormb(); __v; })
#define gsi_writel(v, c)	({ __iowmb(); writel_relaxed((v), (c)); })

#define GSIERR(fmt, args...) \
		dev_err(gsi_ctx->dev, "%s:%d " fmt, __func__, __LINE__, ## args)
#define GSIDBG(fmt, args...) \
		dev_dbg(gsi_ctx->dev, "%s:%d " fmt, __func__, __LINE__, ## args)

enum gsi_evt_ring_state {
	GSI_EVT_RING_STATE_NOT_ALLOCATED = 0x0,
	GSI_EVT_RING_STATE_ALLOCATED = 0x1,
	GSI_EVT_RING_STATE_ERROR = 0xf
};

enum gsi_chan_state {
	GSI_CHAN_STATE_NOT_ALLOCATED = 0x0,
	GSI_CHAN_STATE_ALLOCATED = 0x1,
	GSI_CHAN_STATE_STARTED = 0x2,
	GSI_CHAN_STATE_STOPPED = 0x3,
	GSI_CHAN_STATE_STOP_IN_PROC = 0x4,
	GSI_CHAN_STATE_ERROR = 0xf
};

struct gsi_ring_ctx {
	spinlock_t slock;
	unsigned long base_va;
	uint64_t base;
	uint64_t wp;
	uint64_t rp;
	uint64_t wp_local;
	uint64_t rp_local;
	uint16_t len;
	uint8_t elem_sz;
	uint16_t max_num_elem;
	uint64_t end;
};

struct gsi_chan_dp_stats {
	unsigned long ch_below_lo;
	unsigned long ch_below_hi;
	unsigned long ch_above_hi;
	unsigned long empty_time;
	unsigned long last_timestamp;
};

struct gsi_chan_stats {
	unsigned long queued;
	unsigned long completed;
	unsigned long callback_to_poll;
	unsigned long poll_to_callback;
	unsigned long invalid_tre_error;
	unsigned long poll_ok;
	unsigned long poll_empty;
	struct gsi_chan_dp_stats dp;
};

struct gsi_chan_ctx {
	struct gsi_chan_props props;
	enum gsi_chan_state state;
	struct gsi_ring_ctx ring;
	void **user_data;
	struct gsi_evt_ctx *evtr;
	struct mutex mlock;
	struct completion compl;
	bool allocated;
	atomic_t poll_mode;
	union __packed gsi_channel_scratch scratch;
	struct gsi_chan_stats stats;
	bool enable_dp_stats;
	bool print_dp_stats;
};

struct gsi_evt_stats {
	unsigned long completed;
};

struct gsi_evt_ctx {
	struct gsi_evt_ring_props props;
	enum gsi_evt_ring_state state;
	uint8_t id;
	struct gsi_ring_ctx ring;
	struct mutex mlock;
	struct completion compl;
	struct gsi_chan_ctx *chan;
	atomic_t chan_ref_cnt;
	union __packed gsi_evt_scratch scratch;
	struct gsi_evt_stats stats;
};

struct gsi_ee_scratch {
	union __packed {
		struct {
			uint32_t resvd1:15;
			uint32_t max_usb_pkt_size:1;
			uint32_t resvd2:8;
			uint32_t mhi_base_chan_idx:8;
		} s;
		uint32_t val;
	} word0;
	uint32_t word1;
};

struct gsi_ctx {
	void __iomem *base;
	struct device *dev;
	struct gsi_per_props per;
	bool per_registered;
	struct gsi_chan_ctx chan[GSI_MAX_CHAN];
	struct gsi_evt_ctx evtr[GSI_MAX_EVT_RING];
	struct mutex mlock;
	spinlock_t slock;
	unsigned long evt_bmap;
	bool enabled;
	atomic_t num_chan;
	atomic_t num_evt_ring;
	struct gsi_ee_scratch scratch;
	int num_ch_dp_stats;
	struct workqueue_struct *dp_stat_wq;
};

enum gsi_re_type {
	GSI_RE_XFER = 0x2,
	GSI_RE_IMMD_CMD = 0x3,
};

struct __packed gsi_tre {
	uint64_t buffer_ptr;
	uint16_t buf_len;
	uint16_t resvd1;
	uint16_t chain:1;
	uint16_t resvd4:7;
	uint16_t ieob:1;
	uint16_t ieot:1;
	uint16_t bei:1;
	uint16_t resvd3:5;
	uint8_t re_type;
	uint8_t resvd2;
};

struct __packed gsi_xfer_compl_evt {
	uint64_t xfer_ptr;
	uint16_t len;
	uint8_t resvd1;
	uint8_t code;  /* see gsi_chan_evt */
	uint16_t resvd;
	uint8_t type;
	uint8_t chid;
};

enum gsi_err_type {
	GSI_ERR_TYPE_GLOB = 0x1,
	GSI_ERR_TYPE_CHAN = 0x2,
	GSI_ERR_TYPE_EVT = 0x3,
};

enum gsi_err_code {
	GSI_INVALID_TRE_ERR = 0x1,
	GSI_OUT_OF_BUFFERS_ERR = 0x2,
	GSI_OUT_OF_RESOURCES_ERR = 0x3,
	GSI_UNSUPPORTED_INTER_EE_OP_ERR = 0x4,
	GSI_EVT_RING_EMPTY_ERR = 0x5,
	GSI_NON_ALLOCATED_EVT_ACCESS_ERR = 0x6,
	GSI_HWO_1_ERR = 0x8
};

struct __packed gsi_log_err {
	uint32_t arg3:4;
	uint32_t arg2:4;
	uint32_t arg1:4;
	uint32_t code:4;
	uint32_t resvd:3;
	uint32_t virt_idx:5;
	uint32_t err_type:4;
	uint32_t ee:4;
};

enum gsi_ch_cmd_opcode {
	GSI_CH_ALLOCATE = 0x0,
	GSI_CH_START = 0x1,
	GSI_CH_STOP = 0x2,
	GSI_CH_RESET = 0x9,
	GSI_CH_DE_ALLOC = 0xa,
	GSI_CH_DB_STOP = 0xb,
};

enum gsi_evt_ch_cmd_opcode {
	GSI_EVT_ALLOCATE = 0x0,
	GSI_EVT_RESET = 0x9,  /* TODO: is this valid? */
	GSI_EVT_DE_ALLOC = 0xa,
};

extern struct gsi_ctx *gsi_ctx;
void gsi_debugfs_init(void);
uint16_t gsi_find_idx_from_addr(struct gsi_ring_ctx *ctx, uint64_t addr);
void gsi_update_ch_dp_stats(struct gsi_chan_ctx *ctx, uint16_t used);

#endif
