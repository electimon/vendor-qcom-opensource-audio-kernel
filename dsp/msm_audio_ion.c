/*
 * Copyright (c) 2013-2018, The Linux Foundation. All rights reserved.
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

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/export.h>
#include <linux/ion_kernel.h>
#include <ipc/apr.h>
#include <asm/dma-iommu.h>
#include <dsp/msm_audio_ion.h>

#define MSM_AUDIO_ION_PROBED (1 << 0)

#define MSM_AUDIO_ION_PHYS_ADDR(alloc_data) \
	alloc_data->table->sgl->dma_address

#define MSM_AUDIO_ION_VA_START 0x10000000
#define MSM_AUDIO_ION_VA_LEN 0x0FFFFFFF

#define MSM_AUDIO_SMMU_SID_OFFSET 32

struct msm_audio_ion_private {
	bool audioheap_enabled;
	struct device *cb_dev;
	struct dma_iommu_mapping *mapping;
	u8 device_status;
	struct list_head alloc_list;
	struct mutex list_mutex;
	u64 smmu_sid_bits;
	u32 smmu_version;
};

struct msm_audio_alloc_data {
	size_t len;
	void *vaddr;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
	struct list_head list;
};

static struct msm_audio_ion_private msm_audio_ion_data = {0,};

static void msm_audio_ion_add_allocation(
	struct msm_audio_ion_private *msm_audio_ion_data,
	struct msm_audio_alloc_data *alloc_data)
{
	/*
	 * Since these APIs can be invoked by multiple
	 * clients, there is need to make sure the list
	 * of allocations is always protected
	 */
	mutex_lock(&(msm_audio_ion_data->list_mutex));
	list_add_tail(&(alloc_data->list),
		      &(msm_audio_ion_data->alloc_list));
	mutex_unlock(&(msm_audio_ion_data->list_mutex));
}

static int msm_audio_dma_buf_map(struct dma_buf *dma_buf,
				 dma_addr_t *addr, size_t *len)
{

	struct msm_audio_alloc_data *alloc_data;
	struct device *cb_dev;
	int rc = 0;

	cb_dev = msm_audio_ion_data.cb_dev;

	/* Data required per buffer mapping */
	alloc_data = kzalloc(sizeof(*alloc_data), GFP_KERNEL);
	if (!alloc_data)
		return -ENOMEM;

	alloc_data->dma_buf = dma_buf;
	alloc_data->len = dma_buf->size;
	*len = dma_buf->size;

	/* Attach the dma_buf to context bank device */
	alloc_data->attach = dma_buf_attach(alloc_data->dma_buf,
					    cb_dev);
	if (IS_ERR(alloc_data->attach)) {
		rc = PTR_ERR(alloc_data->attach);
		dev_err(cb_dev,
			"%s: Fail to attach dma_buf to CB, rc = %d\n",
			__func__, rc);
		goto err_attach;
	}

	/*
	 * Get the scatter-gather list.
	 * There is no info as this is a write buffer or
	 * read buffer, hence the request is bi-directional
	 * to accommodate both read and write mappings.
	 */
	alloc_data->table = dma_buf_map_attachment(alloc_data->attach,
				DMA_BIDIRECTIONAL);
	if (IS_ERR(alloc_data->table)) {
		rc = PTR_ERR(alloc_data->table);
		dev_err(cb_dev,
			"%s: Fail to map attachment, rc = %d\n",
			__func__, rc);
		goto err_map_attach;
	}

	/* physical address from mapping */
	*addr = MSM_AUDIO_ION_PHYS_ADDR(alloc_data);

	msm_audio_ion_add_allocation(&msm_audio_ion_data,
				     alloc_data);
	return rc;

err_map_attach:
	dma_buf_detach(alloc_data->dma_buf,
		       alloc_data->attach);
err_attach:
	kfree(alloc_data);

	return rc;
}

static int msm_audio_dma_buf_unmap(struct dma_buf *dma_buf)
{
	int rc = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct list_head *ptr, *next;
	struct device *cb_dev = msm_audio_ion_data.cb_dev;
	bool found = false;

	/*
	 * Though list_for_each_safe is delete safe, lock
	 * should be explicitly acquired to avoid race condition
	 * on adding elements to the list.
	 */
	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_safe(ptr, next,
			    &(msm_audio_ion_data.alloc_list)) {

		alloc_data = list_entry(ptr, struct msm_audio_alloc_data,
					list);

		if (alloc_data->dma_buf == dma_buf) {
			found = true;
			dma_buf_unmap_attachment(alloc_data->attach,
						 alloc_data->table,
						 DMA_BIDIRECTIONAL);

			dma_buf_detach(alloc_data->dma_buf,
				       alloc_data->attach);

			dma_buf_put(alloc_data->dma_buf);

			list_del(&(alloc_data->list));
			kfree(alloc_data);
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	if (!found) {
		dev_err(cb_dev,
			"%s: cannot find allocation, dma_buf %pK",
			__func__, dma_buf);
		rc = -EINVAL;
	}

	return rc;
}

static int msm_audio_ion_get_phys(struct dma_buf *dma_buf,
				  dma_addr_t *addr, size_t *len)
{
	int rc = 0;

	rc = msm_audio_dma_buf_map(dma_buf, addr, len);
	if (rc) {
		pr_err("%s: failed to map DMA buf, err = %d\n",
			__func__, rc);
		goto err;
	}
	/* Append the SMMU SID information to the IOVA address */
	*addr |= msm_audio_ion_data.smmu_sid_bits;

	pr_debug("phys=%pK, len=%zd, rc=%d\n", &(*addr), *len, rc);
err:
	return rc;
}

static void *msm_audio_ion_map_kernel(struct dma_buf *dma_buf)
{
	int i = 0;
	int rc = 0;
	void *addr = NULL;
	unsigned int pg_cnt = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;

	rc = dma_buf_begin_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	if (rc) {
		pr_err("%s: kmap dma_buf_begin_cpu_access fail\n", __func__);
		goto exit;
	}

	pg_cnt = dma_buf->size / PAGE_SIZE;
	if (dma_buf->size % PAGE_SIZE)
		pg_cnt++;

	if (pg_cnt == 0) {
		pr_err("%s: Page count is NULL\n", __func__);
		goto exit;
	}

	/* Map the first page, and store the address to addr */
	addr = dma_buf_kmap(dma_buf, 0);
	if (!addr) {
		pr_err("%s: mapping kernel buffer failed for page 0\n",
		       __func__);
		goto exit;
	}
	/* Map remaining pages */
	for (i = 1; i < pg_cnt; i++) {
		if (!dma_buf_kmap(dma_buf, i)) {
			pr_err("%s: mapping kernel buffer failed for page %d\n",
			       __func__, i);
			goto err;
		}
	}

	/*
	 * TBD: remove the below section once new API
	 * for mapping kernel virtual address is available.
	 */
	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_entry(alloc_data, &(msm_audio_ion_data.alloc_list),
			    list) {
		if (alloc_data->dma_buf == dma_buf) {
			alloc_data->vaddr = addr;
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	return addr;

err:
	for (; i > 0; i--)
		dma_buf_kunmap(dma_buf, i - 1, addr);
	addr = NULL;
exit:
	return addr;
}

static void msm_audio_ion_unmap_kernel(struct dma_buf *dma_buf)
{
	int i, rc = 0;
	unsigned int pg_cnt = 0;
	void *vaddr = NULL;
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct device *cb_dev = msm_audio_ion_data.cb_dev;

	/*
	 * TBD: remove the below section once new API
	 * for unmapping kernel virtual address is available.
	 */
	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_entry(alloc_data, &(msm_audio_ion_data.alloc_list),
			    list) {
		if (alloc_data->dma_buf == dma_buf) {
			vaddr = alloc_data->vaddr;
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	if (!vaddr) {
		dev_err(cb_dev,
			"%s: cannot find allocation for dma_buf %pK",
			__func__, dma_buf);
		goto err;
	}

	pg_cnt = dma_buf->size / PAGE_SIZE;
	if (dma_buf->size % PAGE_SIZE)
		pg_cnt++;

	for (i = 0; i < pg_cnt; i++)
		dma_buf_kunmap(dma_buf, i, vaddr);

	rc = dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	if (rc) {
		dev_err(cb_dev, "%s: kmap dma_buf_end_cpu_access fail\n",
			__func__);
		goto err;
	}

err:
	return;
}

static int msm_audio_ion_map_buf(struct dma_buf *dma_buf, dma_addr_t *paddr,
				 size_t *plen, void *vaddr)
{
	int rc = 0;

	rc = msm_audio_ion_get_phys(dma_buf, paddr, plen);
	if (rc) {
		pr_err("%s: ION Get Physical for AUDIO failed, rc = %d\n",
				__func__, rc);
		goto err;
	}

	vaddr = msm_audio_ion_map_kernel(dma_buf);
	if (IS_ERR_OR_NULL((void *)vaddr)) {
		pr_err("%s: ION memory mapping for AUDIO failed\n", __func__);
		rc = -ENOMEM;
		goto err;
	}

err:
	return rc;
}

/**
 * msm_audio_ion_alloc -
 *        Allocs ION memory for given client name
 *
 * @dma_buf: dma_buf for the ION memory
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * vaddr: virtual address to be assigned
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_alloc(struct dma_buf **dma_buf, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, void **vaddr)
{
	int rc = -EINVAL;
	unsigned long err_ion_ptr = 0;

	if (!(msm_audio_ion_data.device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s:probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}
	if (!dma_buf || !paddr || !vaddr || !bufsz || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	*dma_buf = ion_alloc(bufsz, ION_HEAP(ION_SYSTEM_HEAP_ID), 0);
	if (IS_ERR_OR_NULL((void *)(*dma_buf))) {
		if (IS_ERR((void *)(*dma_buf)))
			err_ion_ptr = PTR_ERR((int *)(*dma_buf));
		pr_err("%s:ION alloc fail err ptr=%ld\n",
			__func__, err_ion_ptr);
		rc = -ENOMEM;
		goto err;
	}

	rc = msm_audio_ion_map_buf(*dma_buf, paddr, plen, *vaddr);
	if (rc) {
		pr_err("%s: failed to map ION buf, rc = %d\n", __func__, rc);
		goto err_dma_buf;
	}
	pr_debug("%s: mapped address = %pK, size=%zd\n", __func__,
		*vaddr, bufsz);

	memset((void *)*vaddr, 0, bufsz);

	return rc;

err_dma_buf:
	dma_buf_put(*dma_buf);
err:
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_alloc);

/**
 * msm_audio_ion_import-
 *        Import ION buffer with given file descriptor
 *
 * @dma_buf: dma_buf for the ION memory
 * @fd: file descriptor for the ION memory
 * @ionflag: flags associated with ION buffer
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * vaddr: virtual address to be assigned
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_import(struct dma_buf **dma_buf, int fd,
			unsigned long *ionflag, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, void **vaddr)
{
	int rc = 0;

	if (!(msm_audio_ion_data.device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s:probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!dma_buf || !paddr || !vaddr || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	/* bufsz should be 0 and fd shouldn't be 0 as of now */
	*dma_buf = dma_buf_get(fd);
	pr_debug("%s: dma_buf =%pK, fd=%d\n", __func__, *dma_buf, fd);
	if (IS_ERR_OR_NULL((void *)(*dma_buf))) {
		pr_err("%s: dma_buf_get failed\n", __func__);
		rc = -EINVAL;
		goto err;
	}

	if (ionflag != NULL) {
		rc = dma_buf_get_flags(*dma_buf, ionflag);
		if (rc) {
			pr_err("%s: could not get flags for the dma_buf\n",
				__func__);
			goto err_ion_flag;
		}
	}

	rc = msm_audio_ion_map_buf(*dma_buf, paddr, plen, *vaddr);
	if (rc) {
		pr_err("%s: failed to map ION buf, rc = %d\n", __func__, rc);
		goto err_ion_flag;
	}
	pr_debug("%s: mapped address = %pK, size=%zd\n", __func__,
		*vaddr, bufsz);

	return 0;

err_ion_flag:
	dma_buf_put(*dma_buf);
err:
	*dma_buf = NULL;
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_import);

/**
 * msm_audio_ion_free -
 *        fress ION memory for given client and handle
 *
 * @dma_buf: dma_buf for the ION memory
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_free(struct dma_buf *dma_buf)
{
	if (!dma_buf) {
		pr_err("%s: dma_buf invalid\n", __func__);
		return -EINVAL;
	}

	msm_audio_ion_unmap_kernel(dma_buf);

	msm_audio_dma_buf_unmap(dma_buf);

	return 0;
}
EXPORT_SYMBOL(msm_audio_ion_free);

/**
 * msm_audio_ion_mmap -
 *       Audio ION memory map
 *
 * @abuff: audio buf pointer
 * @vma: virtual mem area
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_mmap(struct audio_buffer *abuff,
		       struct vm_area_struct *vma)
{
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct sg_table *table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	unsigned int i;
	struct page *page;
	int ret = 0;
	bool found = false;
	struct device *cb_dev = msm_audio_ion_data.cb_dev;

	mutex_lock(&(msm_audio_ion_data.list_mutex));
	list_for_each_entry(alloc_data, &(msm_audio_ion_data.alloc_list),
			    list) {
		if (alloc_data->dma_buf == abuff->dma_buf) {
			found = true;
			table = alloc_data->table;
			break;
		}
	}
	mutex_unlock(&(msm_audio_ion_data.list_mutex));

	if (!found) {
		dev_err(cb_dev,
			"%s: cannot find allocation, dma_buf %pK",
			__func__, abuff->dma_buf);
		return -EINVAL;
	}
	/* uncached */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	/* We need to check if a page is associated with this sg list because:
	 * If the allocation came from a carveout we currently don't have
	 * pages associated with carved out memory. This might change in the
	 * future and we can remove this check and the else statement.
	 */
	page = sg_page(table->sgl);
	if (page) {
		pr_debug("%s: page is NOT null\n", __func__);
		for_each_sg(table->sgl, sg, table->nents, i) {
			unsigned long remainder = vma->vm_end - addr;
			unsigned long len = sg->length;

			page = sg_page(sg);

			if (offset >= len) {
				offset -= len;
				continue;
			} else if (offset) {
				page += offset / PAGE_SIZE;
				len -= offset;
				offset = 0;
			}
			len = min(len, remainder);
			pr_debug("vma=%pK, addr=%x len=%ld vm_start=%x vm_end=%x vm_page_prot=%lu\n",
				vma, (unsigned int)addr, len,
				(unsigned int)vma->vm_start,
				(unsigned int)vma->vm_end,
				(unsigned long)pgprot_val(vma->vm_page_prot));
			remap_pfn_range(vma, addr, page_to_pfn(page), len,
					vma->vm_page_prot);
			addr += len;
			if (addr >= vma->vm_end)
				return 0;
		}
	} else {
		pr_debug("%s: page is NULL\n", __func__);
		ret = -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(msm_audio_ion_mmap);

/**
 * msm_audio_ion_cache_operations-
 *       Cache operations on cached Audio ION buffers
 *
 * @abuff: audio buf pointer
 * @cache_op: cache operation to be performed
 *
 * Returns 0 on success or error on failure
 */
int msm_audio_ion_cache_operations(struct audio_buffer *abuff, int cache_op)
{
	unsigned long ionflag = 0;
	int rc = 0;

	if (!abuff) {
		pr_err("%s: Invalid params: %pK\n", __func__, abuff);
		return -EINVAL;
	}
	rc = dma_buf_get_flags(abuff->dma_buf, &ionflag);
	if (rc) {
		pr_err("%s: dma_buf_get_flags failed: %d\n", __func__, rc);
		goto cache_op_failed;
	}

	/* Has to be CACHED */
	if (ionflag & ION_FLAG_CACHED) {
		/* MSM_AUDIO_ION_INV_CACHES or MSM_AUDIO_ION_CLEAN_CACHES */
		switch (cache_op) {
		case MSM_AUDIO_ION_INV_CACHES:
		case MSM_AUDIO_ION_CLEAN_CACHES:
			dma_buf_begin_cpu_access(abuff->dma_buf,
						  DMA_BIDIRECTIONAL);
			dma_buf_end_cpu_access(abuff->dma_buf,
						  DMA_BIDIRECTIONAL);
			break;
		default:
			pr_err("%s: Invalid cache operation %d\n",
			       __func__, cache_op);
		}
	} else {
		pr_err("%s: Cache ops called on uncached buffer: %pK\n",
			__func__, abuff->dma_buf);
		rc = -EINVAL;
	}

cache_op_failed:
	return rc;
}
EXPORT_SYMBOL(msm_audio_ion_cache_operations);

/**
 * msm_audio_populate_upper_32_bits -
 *        retrieve upper 32bits of 64bit address
 *
 * @pa: 64bit physical address
 *
 */
u32 msm_audio_populate_upper_32_bits(dma_addr_t pa)
{
	if (sizeof(dma_addr_t) == sizeof(u32))
		return upper_32_bits(msm_audio_ion_data.smmu_sid_bits);
	else
		return upper_32_bits(pa);
}
EXPORT_SYMBOL(msm_audio_populate_upper_32_bits);

static int msm_audio_smmu_init(struct device *dev)
{
	struct dma_iommu_mapping *mapping;
	int ret;

	mapping = arm_iommu_create_mapping(&platform_bus_type,
					   MSM_AUDIO_ION_VA_START,
					   MSM_AUDIO_ION_VA_LEN);
	if (IS_ERR(mapping))
		return PTR_ERR(mapping);

	ret = arm_iommu_attach_device(dev, mapping);
	if (ret) {
		dev_err(dev, "%s: Attach failed, err = %d\n",
			__func__, ret);
		goto fail_attach;
	}

	msm_audio_ion_data.cb_dev = dev;
	msm_audio_ion_data.mapping = mapping;
	INIT_LIST_HEAD(&msm_audio_ion_data.alloc_list);
	mutex_init(&(msm_audio_ion_data.list_mutex));

	return 0;

fail_attach:
	arm_iommu_release_mapping(mapping);
	return ret;
}

static const struct of_device_id msm_audio_ion_dt_match[] = {
	{ .compatible = "qcom,msm-audio-ion" },
	{ }
};
MODULE_DEVICE_TABLE(of, msm_audio_ion_dt_match);

static int msm_audio_ion_probe(struct platform_device *pdev)
{
	int rc = 0;
	const char *msm_audio_ion_smmu = "qcom,smmu-version";
	const char *msm_audio_ion_smmu_sid_mask = "qcom,smmu-sid-mask";
	enum apr_subsys_state q6_state;
	struct device *dev = &pdev->dev;
	u64 smmu_sid = 0;
	u64 smmu_sid_mask = 0;
	struct of_phandle_args iommuspec;

	if (dev->of_node == NULL) {
		dev_err(dev,
			"%s: device tree is not found\n",
			__func__);
		return 0;
	}

	rc = of_property_read_u32(dev->of_node,
				msm_audio_ion_smmu,
				&msm_audio_ion_data.smmu_version);
	if (rc) {
		dev_err(dev,
			"%s: qcom,smmu_version missing in DT node\n",
			__func__);
		return rc;
	}
	dev_dbg(dev, "%s: SMMU version is (%d)", __func__,
			msm_audio_ion_data.smmu_version);
	q6_state = apr_get_q6_state();
	if (q6_state == APR_SUBSYS_DOWN) {
		dev_dbg(dev,
			"defering %s, adsp_state %d\n",
			__func__, q6_state);
		return -EPROBE_DEFER;
	}
	dev_dbg(dev, "%s: adsp is ready\n", __func__);

	/* Get SMMU SID information from Devicetree */
	rc = of_property_read_u64(dev->of_node,
				  msm_audio_ion_smmu_sid_mask,
				  &smmu_sid_mask);
	if (rc) {
		dev_err(dev,
			"%s: qcom,smmu-sid-mask missing in DT node, using default\n",
			__func__);
		smmu_sid_mask = 0xFFFFFFFFFFFFFFFF;
	}
	rc = of_parse_phandle_with_args(dev->of_node, "iommus",
					"#iommu-cells", 0, &iommuspec);
	if (rc)
		dev_err(dev, "%s: could not get smmu SID, ret = %d\n",
			__func__, rc);
	else
		smmu_sid = (iommuspec.args[0] & smmu_sid_mask);

	msm_audio_ion_data.smmu_sid_bits =
		smmu_sid << MSM_AUDIO_SMMU_SID_OFFSET;

	if (msm_audio_ion_data.smmu_version == 0x2) {
		rc = msm_audio_smmu_init(dev);
	} else {
		dev_err(dev, "%s: smmu version invalid %d\n",
			__func__, msm_audio_ion_data.smmu_version);
		rc = -EINVAL;
	}
	if (rc)
		dev_err(dev, "%s: smmu init failed, err = %d\n",
			__func__, rc);

	if (!rc)
		msm_audio_ion_data.device_status |= MSM_AUDIO_ION_PROBED;

	return rc;
}

static int msm_audio_ion_remove(struct platform_device *pdev)
{
	struct dma_iommu_mapping *mapping;
	struct device *audio_cb_dev;

	mapping = msm_audio_ion_data.mapping;
	audio_cb_dev = msm_audio_ion_data.cb_dev;

	if (audio_cb_dev && mapping) {
		arm_iommu_detach_device(audio_cb_dev);
		arm_iommu_release_mapping(mapping);
	}

	msm_audio_ion_data.device_status = 0;
	return 0;
}

static struct platform_driver msm_audio_ion_driver = {
	.driver = {
		.name = "msm-audio-ion",
		.owner = THIS_MODULE,
		.of_match_table = msm_audio_ion_dt_match,
	},
	.probe = msm_audio_ion_probe,
	.remove = msm_audio_ion_remove,
};

int __init msm_audio_ion_init(void)
{
	return platform_driver_register(&msm_audio_ion_driver);
}

void msm_audio_ion_exit(void)
{
	platform_driver_unregister(&msm_audio_ion_driver);
}

MODULE_DESCRIPTION("MSM Audio ION module");
MODULE_LICENSE("GPL v2");
