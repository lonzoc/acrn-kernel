/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Ke Yu
 *    Kevin Tian <kevin.tian@intel.com>
 *    Dexuan Cui
 *
 * Contributors:
 *    Tina Zhang <tina.zhang@intel.com>
 *    Min He <min.he@intel.com>
 *    Niu Bing <bing.niu@intel.com>
 *    Zhi Wang <zhi.a.wang@intel.com>
 *
 */

#include "i915_drv.h"
#include "gvt.h"

/**
 * intel_vgpu_gpa_to_mmio_offset - translate a GPA to MMIO offset
 * @vgpu: a vGPU
 * @gpa: guest physical address
 * Returns:
 * Zero on success, negative error code if failed
 */
int intel_vgpu_gpa_to_mmio_offset(struct intel_vgpu *vgpu, u64 gpa)
{
	u64 gttmmio_gpa = intel_vgpu_get_bar_gpa(vgpu, PCI_BASE_ADDRESS_0);
	return gpa - gttmmio_gpa;
}

#define reg_is_mmio(gvt, reg)  \
	(reg >= 0 && reg < gvt->device_info.mmio_size)

#define reg_is_gtt(gvt, reg)   \
	(reg >= gvt->device_info.gtt_start_offset \
	 && reg < gvt->device_info.gtt_start_offset + gvt_ggtt_sz(gvt))

static void failsafe_emulate_mmio_rw(struct intel_vgpu *vgpu, uint64_t pa,
		void *p_data, unsigned int bytes, bool read)
{
	struct intel_gvt *gvt = NULL;
	void *pt = NULL;
	unsigned int offset = 0;

	if (!vgpu || !p_data)
		return;

	gvt = vgpu->gvt;
	mutex_lock(&vgpu->vgpu_lock);
	offset = intel_vgpu_gpa_to_mmio_offset(vgpu, pa);
	if (reg_is_mmio(gvt, offset)) {
		if (read)
			intel_vgpu_default_mmio_read(vgpu, offset, p_data,
					bytes);
		else
			intel_vgpu_default_mmio_write(vgpu, offset, p_data,
					bytes);
	} else if (reg_is_gtt(gvt, offset)) {
		offset -= gvt->device_info.gtt_start_offset;
		pt = vgpu->gtt.ggtt_mm->ggtt_mm.virtual_ggtt + offset;
		if (read)
			memcpy(p_data, pt, bytes);
		else
			memcpy(pt, p_data, bytes);

	}
	mutex_unlock(&vgpu->vgpu_lock);
}

/**
 * intel_vgpu_emulate_mmio_read - emulate MMIO read
 * @vgpu: a vGPU
 * @pa: guest physical address
 * @p_data: data return buffer
 * @bytes: access data length
 *
 * Returns:
 * Zero on success, negative error code if failed
 */
int intel_vgpu_emulate_mmio_read(struct intel_vgpu *vgpu, uint64_t pa,
		void *p_data, unsigned int bytes)
{
	struct intel_gvt *gvt = vgpu->gvt;
	unsigned int offset = 0;
	int ret = -EINVAL;

	if (vgpu->failsafe) {
		failsafe_emulate_mmio_rw(vgpu, pa, p_data, bytes, true);
		return 0;
	}
	mutex_lock(&vgpu->vgpu_lock);

	offset = intel_vgpu_gpa_to_mmio_offset(vgpu, pa);

	if (WARN_ON(bytes > 8))
		goto err;

	if (reg_is_gtt(gvt, offset)) {
		if (WARN_ON(!IS_ALIGNED(offset, 4) && !IS_ALIGNED(offset, 8)))
			goto err;
		if (WARN_ON(bytes != 4 && bytes != 8))
			goto err;
		if (WARN_ON(!reg_is_gtt(gvt, offset + bytes - 1)))
			goto err;

		ret = intel_vgpu_emulate_ggtt_mmio_read(vgpu, offset,
				p_data, bytes);
		if (ret)
			goto err;
		goto out;
	}

	if (WARN_ON_ONCE(!reg_is_mmio(gvt, offset))) {
		ret = intel_gvt_hypervisor_read_gpa(vgpu, pa, p_data, bytes);
		goto out;
	}

	if (WARN_ON(!reg_is_mmio(gvt, offset + bytes - 1)))
		goto err;

	if (!intel_gvt_mmio_is_unalign(gvt, offset)) {
		if (WARN_ON(!IS_ALIGNED(offset, bytes)))
			goto err;
	}

	ret = intel_vgpu_mmio_reg_rw(vgpu, offset, p_data, bytes, true);
	if (ret < 0)
		goto err;

	intel_gvt_mmio_set_accessed(gvt, offset);
	ret = 0;
	goto out;

err:
	gvt_vgpu_err("fail to emulate MMIO read %08x len %d\n",
			offset, bytes);
out:
	mutex_unlock(&vgpu->vgpu_lock);
	return ret;
}

/**
 * intel_vgpu_emulate_mmio_write - emulate MMIO write
 * @vgpu: a vGPU
 * @pa: guest physical address
 * @p_data: write data buffer
 * @bytes: access data length
 *
 * Returns:
 * Zero on success, negative error code if failed
 */
int intel_vgpu_emulate_mmio_write(struct intel_vgpu *vgpu, uint64_t pa,
		void *p_data, unsigned int bytes)
{
	struct intel_gvt *gvt = vgpu->gvt;
	unsigned int offset = 0;
	int ret = -EINVAL;

	if (vgpu->failsafe) {
		failsafe_emulate_mmio_rw(vgpu, pa, p_data, bytes, false);
		return 0;
	}

	mutex_lock(&vgpu->vgpu_lock);

	offset = intel_vgpu_gpa_to_mmio_offset(vgpu, pa);

	if (WARN_ON(bytes > 8))
		goto err;

	if (reg_is_gtt(gvt, offset)) {
		if (WARN_ON(!IS_ALIGNED(offset, 4) && !IS_ALIGNED(offset, 8)))
			goto err;
		if (WARN_ON(bytes != 4 && bytes != 8))
			goto err;
		if (WARN_ON(!reg_is_gtt(gvt, offset + bytes - 1)))
			goto err;

		ret = intel_vgpu_emulate_ggtt_mmio_write(vgpu, offset,
				p_data, bytes);
		if (ret)
			goto err;
		goto out;
	}

	if (WARN_ON_ONCE(!reg_is_mmio(gvt, offset))) {
		ret = intel_gvt_hypervisor_write_gpa(vgpu, pa, p_data, bytes);
		goto out;
	}

	ret = intel_vgpu_mmio_reg_rw(vgpu, offset, p_data, bytes, false);
	if (ret < 0)
		goto err;

	if (vgpu->entire_nonctxmmio_checked
		&& intel_gvt_mmio_is_non_context(vgpu->gvt, offset)
		&& vgpu_vreg(vgpu, offset) != gvt_host_reg(gvt, offset)) {
		gvt_err("vgpu%d unexpected non-context MMIO change at 0x%x:0x%x,0x%x\n",
			vgpu->id, offset, vgpu_vreg(vgpu, offset),
			gvt_host_reg(gvt, offset));
	}

	intel_gvt_mmio_set_accessed(gvt, offset);
	ret = 0;
	goto out;
err:
	gvt_vgpu_err("fail to emulate MMIO write %08x len %d\n", offset,
		     bytes);
out:
	mutex_unlock(&vgpu->vgpu_lock);
	return ret;
}


/**
 * intel_vgpu_reset_mmio - reset virtual MMIO space
 * @vgpu: a vGPU
 * @dmlr: vGPU Device Model Level Reset or GT Reset
 */
void intel_vgpu_reset_mmio(struct intel_vgpu *vgpu, bool dmlr)
{
	struct intel_gvt *gvt = vgpu->gvt;
	const struct intel_gvt_device_info *info = &gvt->device_info;
	void  *mmio = gvt->firmware.mmio;
	struct drm_i915_private *dev_priv = gvt->dev_priv;

	if (dmlr) {
		memcpy(vgpu->mmio.vreg, mmio, info->mmio_size);
		memcpy(vgpu->mmio.sreg, mmio, info->mmio_size);

		vgpu_vreg_t(vgpu, GEN6_GT_THREAD_STATUS_REG) = 0;

		/* set the bit 0:2(Core C-State ) to C0 */
		vgpu_vreg_t(vgpu, GEN6_GT_CORE_STATUS) = 0;

		if (IS_BROXTON(vgpu->gvt->dev_priv)) {
			vgpu_vreg_t(vgpu, BXT_P_CR_GT_DISP_PWRON) &=
				    ~(BIT(0) | BIT(1));
			vgpu_vreg_t(vgpu, BXT_PORT_CL1CM_DW0(DPIO_PHY0)) &=
				    ~PHY_POWER_GOOD;
			vgpu_vreg_t(vgpu, BXT_PORT_CL1CM_DW0(DPIO_PHY1)) &=
				    ~PHY_POWER_GOOD;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL_FAMILY(DPIO_PHY0)) &=
				    ~BIT(30);
			vgpu_vreg_t(vgpu, BXT_PHY_CTL_FAMILY(DPIO_PHY1)) &=
				    ~BIT(30);
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_A)) &=
				    ~BXT_PHY_LANE_ENABLED;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_A)) |=
				    BXT_PHY_CMNLANE_POWERDOWN_ACK |
				    BXT_PHY_LANE_POWERDOWN_ACK;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_B)) &=
				    ~BXT_PHY_LANE_ENABLED;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_B)) |=
				    BXT_PHY_CMNLANE_POWERDOWN_ACK |
				    BXT_PHY_LANE_POWERDOWN_ACK;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_C)) &=
				    ~BXT_PHY_LANE_ENABLED;
			vgpu_vreg_t(vgpu, BXT_PHY_CTL(PORT_C)) |=
				    BXT_PHY_CMNLANE_POWERDOWN_ACK |
				    BXT_PHY_LANE_POWERDOWN_ACK;
		}
	} else {
#define GVT_GEN8_MMIO_RESET_OFFSET		(0x44200)
		/* only reset the engine related, so starting with 0x44200
		 * interrupt include DE,display mmio related will not be
		 * touched
		 */
		memcpy(vgpu->mmio.vreg, mmio, GVT_GEN8_MMIO_RESET_OFFSET);
		memcpy(vgpu->mmio.sreg, mmio, GVT_GEN8_MMIO_RESET_OFFSET);
	}

	/* below vreg init value are got from handler.c,
	 * which won't change during vgpu life cycle
	 */
	vgpu_vreg(vgpu, 0xe651c) = 1 << 17;
	vgpu_vreg(vgpu, 0xe661c) = 1 << 17;
	vgpu_vreg(vgpu, 0xe671c) = 1 << 17;
	vgpu_vreg(vgpu, 0xe681c) = 1 << 17;
	vgpu_vreg(vgpu, 0xe6c04) = 3;
	vgpu_vreg(vgpu, 0xe6e1c) = 0x2f << 16;

	if (HAS_HUC_UCODE(dev_priv)) {
		mmio_hw_access_pre(dev_priv);
		vgpu_vreg_t(vgpu, HUC_STATUS2) = I915_READ(HUC_STATUS2);
		mmio_hw_access_post(dev_priv);
	}
	/* Non-context MMIOs need entire check again if mmio/vgpu reset */
	vgpu->entire_nonctxmmio_checked = false;
}

/**
 * intel_vgpu_init_mmio - init MMIO  space
 * @vgpu: a vGPU
 *
 * Returns:
 * Zero on success, negative error code if failed
 */
int intel_vgpu_init_mmio(struct intel_vgpu *vgpu)
{
	const struct intel_gvt_device_info *info = &vgpu->gvt->device_info;

	BUILD_BUG_ON(sizeof(struct gvt_shared_page) != PAGE_SIZE);

	vgpu->mmio.sreg = vzalloc(info->mmio_size);
	vgpu->mmio.vreg = intel_gvt_allocate_vreg(vgpu);
	if (!vgpu->mmio.vreg)
		return -ENOMEM;

	vgpu->mmio.shared_page = (struct gvt_shared_page *) __get_free_pages(
			GFP_KERNEL, 0);
	if (!vgpu->mmio.shared_page) {
		intel_gvt_free_vreg(vgpu);
		vgpu->mmio.vreg = NULL;
		return -ENOMEM;
	}

	intel_vgpu_reset_mmio(vgpu, true);

	return 0;
}

/**
 * intel_vgpu_clean_mmio - clean MMIO space
 * @vgpu: a vGPU
 *
 */
void intel_vgpu_clean_mmio(struct intel_vgpu *vgpu)
{
	vfree(vgpu->mmio.sreg);
	intel_gvt_free_vreg(vgpu);
	free_pages((unsigned long) vgpu->mmio.shared_page, 0);
	vgpu->mmio.vreg = vgpu->mmio.sreg = vgpu->mmio.shared_page = NULL;
}
