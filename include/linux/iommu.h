/*
 * Copyright (C) 2007-2008 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <joerg.roedel@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#ifndef __LINUX_IOMMU_H
#define __LINUX_IOMMU_H

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/types.h>
#include <trace/events/iommu.h>

#define IOMMU_READ	(1 << 0)
#define IOMMU_WRITE	(1 << 1)
#define IOMMU_CACHE	(1 << 2) /* DMA cache coherency */
#define IOMMU_NOEXEC	(1 << 3)
#define IOMMU_PRIV	(1 << 4)
#define IOMMU_DEVICE	(1 << 5) /* Indicates access to device memory */

struct iommu_ops;
struct iommu_group;
struct bus_type;
struct device;
struct iommu_domain;
struct notifier_block;

/* iommu fault flags */
#define IOMMU_FAULT_READ                (1 << 0)
#define IOMMU_FAULT_WRITE               (1 << 1)
#define IOMMU_FAULT_TRANSLATION         (1 << 2)
#define IOMMU_FAULT_PERMISSION          (1 << 3)
#define IOMMU_FAULT_EXTERNAL            (1 << 4)
#define IOMMU_FAULT_TRANSACTION_STALLED (1 << 5)

typedef int (*iommu_fault_handler_t)(struct iommu_domain *,
			struct device *, unsigned long, int, void *);

struct iommu_domain_geometry {
	dma_addr_t aperture_start; /* First address that can be mapped    */
	dma_addr_t aperture_end;   /* Last address that can be mapped     */
	bool force_aperture;       /* DMA only allowed in mappable range? */
};

struct iommu_pgtbl_info {
	void *pmds;
};

struct iommu_domain {
	const struct iommu_ops *ops;
	void *priv;
	iommu_fault_handler_t handler;
	void *handler_token;
	struct iommu_domain_geometry geometry;
};

enum iommu_cap {
	IOMMU_CAP_CACHE_COHERENCY,	/* IOMMU can enforce cache coherent DMA
					   transactions */
	IOMMU_CAP_INTR_REMAP,		/* IOMMU supports interrupt isolation */
};

/*
 * Following constraints are specifc to FSL_PAMUV1:
 *  -aperture must be power of 2, and naturally aligned
 *  -number of windows must be power of 2, and address space size
 *   of each window is determined by aperture size / # of windows
 *  -the actual size of the mapped region of a window must be power
 *   of 2 starting with 4KB and physical address must be naturally
 *   aligned.
 * DOMAIN_ATTR_FSL_PAMUV1 corresponds to the above mentioned contraints.
 * The caller can invoke iommu_domain_get_attr to check if the underlying
 * iommu implementation supports these constraints.
 */

enum iommu_attr {
	DOMAIN_ATTR_GEOMETRY,
	DOMAIN_ATTR_PAGING,
	DOMAIN_ATTR_WINDOWS,
	DOMAIN_ATTR_FSL_PAMU_STASH,
	DOMAIN_ATTR_FSL_PAMU_ENABLE,
	DOMAIN_ATTR_FSL_PAMUV1,
	DOMAIN_ATTR_NESTING,	/* two stages of translation */
	DOMAIN_ATTR_COHERENT_HTW_DISABLE,
	DOMAIN_ATTR_PT_BASE_ADDR,
	DOMAIN_ATTR_SECURE_VMID,
	DOMAIN_ATTR_ATOMIC,
	DOMAIN_ATTR_CONTEXT_BANK,
	DOMAIN_ATTR_TTBR0,
	DOMAIN_ATTR_CONTEXTIDR,
	DOMAIN_ATTR_PROCID,
	DOMAIN_ATTR_DYNAMIC,
	DOMAIN_ATTR_NON_FATAL_FAULTS,
	DOMAIN_ATTR_S1_BYPASS,
	DOMAIN_ATTR_FAST,
	DOMAIN_ATTR_PGTBL_INFO,
	DOMAIN_ATTR_MAX,
};

extern struct dentry *iommu_debugfs_top;

#ifdef CONFIG_IOMMU_API

/**
 * struct iommu_ops - iommu ops and capabilities
 * @domain_init: init iommu domain
 * @domain_destroy: destroy iommu domain
 * @attach_dev: attach device to an iommu domain
 * @detach_dev: detach device from an iommu domain
 * @map: map a physically contiguous memory region to an iommu domain
 * @unmap: unmap a physically contiguous memory region from an iommu domain
 * @map_sg: map a scatter-gather list of physically contiguous memory chunks
 * to an iommu domain
 * @iova_to_phys: translate iova to physical address
 * @iova_to_phys_hard: translate iova to physical address using IOMMU hardware
 * @add_device: add device to iommu grouping
 * @remove_device: remove device from iommu grouping
 * @domain_get_attr: Query domain attributes
 * @domain_set_attr: Change domain attributes
 * @of_xlate: add OF master IDs to iommu grouping
 * @pgsize_bitmap: bitmap of supported page sizes
 * @priv: per-instance data private to the iommu driver
 * @get_pgsize_bitmap: gets a bitmap of supported page sizes for a domain
 *                     This takes precedence over @pgsize_bitmap.
 * @trigger_fault: trigger a fault on the device attached to an iommu domain
 * @reg_read: read an IOMMU register
 * @reg_write: write an IOMMU register
 * @tlbi_domain: Invalidate all TLBs covering an iommu domain
 * @enable_config_clocks: Enable all config clocks for this domain's IOMMU
 * @disable_config_clocks: Disable all config clocks for this domain's IOMMU
 * @priv: per-instance data private to the iommu driver
 */
struct iommu_ops {
	bool (*capable)(enum iommu_cap);
	int (*domain_init)(struct iommu_domain *domain);
	void (*domain_destroy)(struct iommu_domain *domain);
	int (*attach_dev)(struct iommu_domain *domain, struct device *dev);
	void (*detach_dev)(struct iommu_domain *domain, struct device *dev);
	int (*map)(struct iommu_domain *domain, unsigned long iova,
		   phys_addr_t paddr, size_t size, int prot);
	size_t (*unmap)(struct iommu_domain *domain, unsigned long iova,
		     size_t size);
	size_t (*map_sg)(struct iommu_domain *domain, unsigned long iova,
			 struct scatterlist *sg, unsigned int nents, int prot);
	int (*map_range)(struct iommu_domain *domain, unsigned long iova,
			struct scatterlist *sg, size_t len, int prot);
	int (*unmap_range)(struct iommu_domain *domain, unsigned long iova,
			size_t len);

	phys_addr_t (*iova_to_phys)(struct iommu_domain *domain, dma_addr_t iova);
	phys_addr_t (*iova_to_phys_hard)(struct iommu_domain *domain,
					 dma_addr_t iova);
	int (*add_device)(struct device *dev);
	void (*remove_device)(struct device *dev);
	int (*device_group)(struct device *dev, unsigned int *groupid);
	int (*domain_get_attr)(struct iommu_domain *domain,
			       enum iommu_attr attr, void *data);
	int (*domain_set_attr)(struct iommu_domain *domain,
			       enum iommu_attr attr, void *data);

	/* Window handling functions */
	int (*domain_window_enable)(struct iommu_domain *domain, u32 wnd_nr,
				    phys_addr_t paddr, u64 size, int prot);
	void (*domain_window_disable)(struct iommu_domain *domain, u32 wnd_nr);
	/* Set the numer of window per domain */
	int (*domain_set_windows)(struct iommu_domain *domain, u32 w_count);
	/* Get the numer of window per domain */
	u32 (*domain_get_windows)(struct iommu_domain *domain);
	int (*dma_supported)(struct iommu_domain *domain, struct device *dev,
			     u64 mask);
	void (*trigger_fault)(struct iommu_domain *domain, unsigned long flags);
	unsigned long (*reg_read)(struct iommu_domain *domain,
				  unsigned long offset);
	void (*reg_write)(struct iommu_domain *domain, unsigned long val,
			  unsigned long offset);
	void (*tlbi_domain)(struct iommu_domain *domain);
	int (*enable_config_clocks)(struct iommu_domain *domain);
	void (*disable_config_clocks)(struct iommu_domain *domain);

#ifdef CONFIG_OF_IOMMU
	int (*of_xlate)(struct device *dev, struct of_phandle_args *args);
#endif

	unsigned long (*get_pgsize_bitmap)(struct iommu_domain *domain);
	unsigned long pgsize_bitmap;
	void *priv;
};

#define IOMMU_GROUP_NOTIFY_ADD_DEVICE		1 /* Device added */
#define IOMMU_GROUP_NOTIFY_DEL_DEVICE		2 /* Pre Device removed */
#define IOMMU_GROUP_NOTIFY_BIND_DRIVER		3 /* Pre Driver bind */
#define IOMMU_GROUP_NOTIFY_BOUND_DRIVER		4 /* Post Driver bind */
#define IOMMU_GROUP_NOTIFY_UNBIND_DRIVER	5 /* Pre Driver unbind */
#define IOMMU_GROUP_NOTIFY_UNBOUND_DRIVER	6 /* Post Driver unbind */

extern int bus_set_iommu(struct bus_type *bus, const struct iommu_ops *ops);
extern bool iommu_present(struct bus_type *bus);
extern bool iommu_capable(struct bus_type *bus, enum iommu_cap cap);
extern struct iommu_domain *iommu_domain_alloc(struct bus_type *bus);
extern struct iommu_group *iommu_group_get_by_id(int id);
extern void iommu_domain_free(struct iommu_domain *domain);
extern int iommu_attach_device(struct iommu_domain *domain,
			       struct device *dev);
extern void iommu_detach_device(struct iommu_domain *domain,
				struct device *dev);
extern size_t iommu_pgsize(unsigned long pgsize_bitmap,
			   unsigned long addr_merge, size_t size);
extern int iommu_map(struct iommu_domain *domain, unsigned long iova,
		     phys_addr_t paddr, size_t size, int prot);
extern size_t iommu_unmap(struct iommu_domain *domain, unsigned long iova,
		       size_t size);
extern int iommu_map_range(struct iommu_domain *domain, unsigned long iova,
		    struct scatterlist *sg, size_t len, int prot);
extern int iommu_unmap_range(struct iommu_domain *domain, unsigned long iova,
		      size_t len);
extern size_t default_iommu_map_sg(struct iommu_domain *domain, unsigned long iova,
				struct scatterlist *sg, unsigned int nents,
				int prot);
extern phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova);
extern phys_addr_t iommu_iova_to_phys_hard(struct iommu_domain *domain,
					   dma_addr_t iova);
extern void iommu_set_fault_handler(struct iommu_domain *domain,
			iommu_fault_handler_t handler, void *token);
extern void iommu_trigger_fault(struct iommu_domain *domain,
				unsigned long flags);
extern unsigned long iommu_reg_read(struct iommu_domain *domain,
				    unsigned long offset);
extern void iommu_reg_write(struct iommu_domain *domain, unsigned long offset,
			    unsigned long val);

extern int iommu_attach_group(struct iommu_domain *domain,
			      struct iommu_group *group);
extern void iommu_detach_group(struct iommu_domain *domain,
			       struct iommu_group *group);
extern struct iommu_group *iommu_group_alloc(void);
extern void *iommu_group_get_iommudata(struct iommu_group *group);
extern void iommu_group_set_iommudata(struct iommu_group *group,
				      void *iommu_data,
				      void (*release)(void *iommu_data));
extern int iommu_group_set_name(struct iommu_group *group, const char *name);
extern int iommu_group_add_device(struct iommu_group *group,
				  struct device *dev);
extern void iommu_group_remove_device(struct device *dev);
extern int iommu_group_for_each_dev(struct iommu_group *group, void *data,
				    int (*fn)(struct device *, void *));
extern struct iommu_group *iommu_group_get(struct device *dev);
extern struct iommu_group *iommu_group_find(const char *name);
extern void iommu_group_put(struct iommu_group *group);
extern int iommu_group_register_notifier(struct iommu_group *group,
					 struct notifier_block *nb);
extern int iommu_group_unregister_notifier(struct iommu_group *group,
					   struct notifier_block *nb);
extern int iommu_group_id(struct iommu_group *group);
extern struct iommu_group *iommu_group_get_for_dev(struct device *dev);

extern int iommu_domain_get_attr(struct iommu_domain *domain, enum iommu_attr,
				 void *data);
extern int iommu_domain_set_attr(struct iommu_domain *domain, enum iommu_attr,
				 void *data);
struct device *iommu_device_create(struct device *parent, void *drvdata,
				   const struct attribute_group **groups,
				   const char *fmt, ...);
void iommu_device_destroy(struct device *dev);
int iommu_device_link(struct device *dev, struct device *link);
void iommu_device_unlink(struct device *dev, struct device *link);

/* Window handling function prototypes */
extern int iommu_domain_window_enable(struct iommu_domain *domain, u32 wnd_nr,
				      phys_addr_t offset, u64 size,
				      int prot);
extern void iommu_domain_window_disable(struct iommu_domain *domain, u32 wnd_nr);
/**
 * report_iommu_fault() - report about an IOMMU fault to the IOMMU framework
 * @domain: the iommu domain where the fault has happened
 * @dev: the device where the fault has happened
 * @iova: the faulting address
 * @flags: mmu fault flags (e.g. IOMMU_FAULT_READ/IOMMU_FAULT_WRITE/...)
 *
 * This function should be called by the low-level IOMMU implementations
 * whenever IOMMU faults happen, to allow high-level users, that are
 * interested in such events, to know about them.
 *
 * This event may be useful for several possible use cases:
 * - mere logging of the event
 * - dynamic TLB/PTE loading
 * - if restarting of the faulting device is required
 *
 * Returns 0 on success and an appropriate error code otherwise (if dynamic
 * PTE/TLB loading will one day be supported, implementations will be able
 * to tell whether it succeeded or not according to this return value).
 *
 * Specifically, -ENOSYS is returned if a fault handler isn't installed
 * (though fault handlers can also return -ENOSYS, in case they want to
 * elicit the default behavior of the IOMMU drivers).

 * Client fault handler returns -EBUSY to signal to the IOMMU driver
 * that the client will take responsibility for any further fault
 * handling, including clearing fault status registers or retrying
 * the faulting transaction.
 */
static inline int report_iommu_fault(struct iommu_domain *domain,
		struct device *dev, unsigned long iova, int flags)
{
	int ret = -ENOSYS;

	/*
	 * if upper layers showed interest and installed a fault handler,
	 * invoke it.
	 */
	if (domain->handler)
		ret = domain->handler(domain, dev, iova, flags,
						domain->handler_token);

	trace_io_page_fault(dev, iova, flags);
	return ret;
}

static inline size_t iommu_map_sg(struct iommu_domain *domain,
				  unsigned long iova, struct scatterlist *sg,
				  unsigned int nents, int prot)
{
	size_t ret;

	trace_map_sg_start(iova, nents);
	ret = domain->ops->map_sg(domain, iova, sg, nents, prot);
	trace_map_sg_end(iova, nents);
	return ret;
}

extern int iommu_dma_supported(struct iommu_domain *domain, struct device *dev,
			       u64 mask);

static inline void iommu_tlbiall(struct iommu_domain *domain)
{
	if (domain->ops->tlbi_domain)
		domain->ops->tlbi_domain(domain);
}

static inline int iommu_enable_config_clocks(struct iommu_domain *domain)
{
	if (domain->ops->enable_config_clocks)
		return domain->ops->enable_config_clocks(domain);
	return 0;
}

static inline void iommu_disable_config_clocks(struct iommu_domain *domain)
{
	if (domain->ops->disable_config_clocks)
		domain->ops->disable_config_clocks(domain);
}

#else /* CONFIG_IOMMU_API */

struct iommu_ops {};
struct iommu_group {};

static inline bool iommu_present(struct bus_type *bus)
{
	return false;
}

static inline bool iommu_capable(struct bus_type *bus, enum iommu_cap cap)
{
	return false;
}

static inline struct iommu_domain *iommu_domain_alloc(struct bus_type *bus)
{
	return NULL;
}

static inline struct iommu_group *iommu_group_get_by_id(int id)
{
	return NULL;
}

static inline void iommu_domain_free(struct iommu_domain *domain)
{
}

static inline int iommu_attach_device(struct iommu_domain *domain,
				      struct device *dev)
{
	return -ENODEV;
}

static inline void iommu_detach_device(struct iommu_domain *domain,
				       struct device *dev)
{
}

static inline int iommu_map(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, int gfp_order, int prot)
{
	return -ENODEV;
}

static inline int iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			      int gfp_order)
{
	return -ENODEV;
}

static inline int iommu_map_range(struct iommu_domain *domain,
				unsigned long iova,
				struct scatterlist *sg, size_t len, int prot)
{
	return -ENODEV;
}

static inline int iommu_unmap_range(struct iommu_domain *domain,
				unsigned long iova, size_t len)
{
	return -ENODEV;
}

static inline size_t iommu_map_sg(struct iommu_domain *domain,
				  unsigned long iova, struct scatterlist *sg,
				  unsigned int nents, int prot)
{
	return -ENODEV;
}

static inline int iommu_domain_window_enable(struct iommu_domain *domain,
					     u32 wnd_nr, phys_addr_t paddr,
					     u64 size, int prot)
{
	return -ENODEV;
}

static inline void iommu_domain_window_disable(struct iommu_domain *domain,
					       u32 wnd_nr)
{
}

static inline phys_addr_t iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	return 0;
}

static inline phys_addr_t iommu_iova_to_phys_hard(struct iommu_domain *domain,
						  dma_addr_t iova)
{
	return 0;
}

static inline void iommu_set_fault_handler(struct iommu_domain *domain,
				iommu_fault_handler_t handler, void *token)
{
}

static inline void iommu_trigger_fault(struct iommu_domain *domain,
				       unsigned long flags)
{
}

static inline unsigned long iommu_reg_read(struct iommu_domain *domain,
					   unsigned long offset)
{
	return 0;
}

static inline void iommu_reg_write(struct iommu_domain *domain,
				   unsigned long val, unsigned long offset)
{
}

static inline int iommu_attach_group(struct iommu_domain *domain,
				     struct iommu_group *group)
{
	return -ENODEV;
}

static inline void iommu_detach_group(struct iommu_domain *domain,
				      struct iommu_group *group)
{
}

static inline struct iommu_group *iommu_group_alloc(void)
{
	return ERR_PTR(-ENODEV);
}

static inline void *iommu_group_get_iommudata(struct iommu_group *group)
{
	return NULL;
}

static inline void iommu_group_set_iommudata(struct iommu_group *group,
					     void *iommu_data,
					     void (*release)(void *iommu_data))
{
}

static inline int iommu_group_set_name(struct iommu_group *group,
				       const char *name)
{
	return -ENODEV;
}

static inline int iommu_group_add_device(struct iommu_group *group,
					 struct device *dev)
{
	return -ENODEV;
}

static inline void iommu_group_remove_device(struct device *dev)
{
}

static inline int iommu_group_for_each_dev(struct iommu_group *group,
					   void *data,
					   int (*fn)(struct device *, void *))
{
	return -ENODEV;
}

static inline struct iommu_group *iommu_group_get(struct device *dev)
{
	return NULL;
}

static inline struct iommu_group *iommu_group_find(const char *name)
{
	return NULL;
}

static inline void iommu_group_put(struct iommu_group *group)
{
}

static inline int iommu_group_register_notifier(struct iommu_group *group,
						struct notifier_block *nb)
{
	return -ENODEV;
}

static inline int iommu_group_unregister_notifier(struct iommu_group *group,
						  struct notifier_block *nb)
{
	return 0;
}

static inline int iommu_group_id(struct iommu_group *group)
{
	return -ENODEV;
}

static inline int iommu_domain_get_attr(struct iommu_domain *domain,
					enum iommu_attr attr, void *data)
{
	return -EINVAL;
}

static inline int iommu_domain_set_attr(struct iommu_domain *domain,
					enum iommu_attr attr, void *data)
{
	return -EINVAL;
}

static inline struct device *iommu_device_create(struct device *parent,
					void *drvdata,
					const struct attribute_group **groups,
					const char *fmt, ...)
{
	return ERR_PTR(-ENODEV);
}

static inline void iommu_device_destroy(struct device *dev)
{
}

static inline int iommu_device_link(struct device *dev, struct device *link)
{
	return -EINVAL;
}

static inline void iommu_device_unlink(struct device *dev, struct device *link)
{
}

static inline int iommu_dma_supported(struct iommu_domain *domain,
		struct device *dev, u64 mask)
{
	return -EINVAL;
}

static inline void iommu_tlbiall(struct iommu_domain *domain)
{
}

static inline int iommu_enable_config_clocks(struct iommu_domain *domain)
{
	return 0;
}

static inline void iommu_disable_config_clocks(struct iommu_domain *domain)
{
}

#endif /* CONFIG_IOMMU_API */

#endif /* __LINUX_IOMMU_H */
