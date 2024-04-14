/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/firmware.h>
#include <drm/drm_drv.h>

#include "amdgpu.h"
#include "amdgpu_ucode.h"
#include "amdgpu_vpe.h"
#include "soc15_common.h"
#include "vpe_v6_1.h"

#define AMDGPU_CSA_VPE_SIZE 	64
/* VPE CSA resides in the 4th page of CSA */
#define AMDGPU_CSA_VPE_OFFSET 	(4096 * 3)

static void vpe_set_ring_funcs(struct amdgpu_device *adev);

int amdgpu_vpe_psp_update_sram(struct amdgpu_device *adev)
{
	struct amdgpu_firmware_info ucode = {
		.ucode_id = AMDGPU_UCODE_ID_VPE,
		.mc_addr = adev->vpe.cmdbuf_gpu_addr,
		.ucode_size = 8,
	};

	return psp_execute_ip_fw_load(&adev->psp, &ucode);
}

int amdgpu_vpe_init_microcode(struct amdgpu_vpe *vpe)
{
	struct amdgpu_device *adev = vpe->ring.adev;
	const struct vpe_firmware_header_v1_0 *vpe_hdr;
	char fw_prefix[32], fw_name[64];
	int ret;

	amdgpu_ucode_ip_version_decode(adev, VPE_HWIP, fw_prefix, sizeof(fw_prefix));
	snprintf(fw_name, sizeof(fw_name), "amdgpu/%s.bin", fw_prefix);

	ret = amdgpu_ucode_request(adev, &adev->vpe.fw, fw_name);
	if (ret)
		goto out;

	vpe_hdr = (const struct vpe_firmware_header_v1_0 *)adev->vpe.fw->data;
	adev->vpe.fw_version = le32_to_cpu(vpe_hdr->header.ucode_version);
	adev->vpe.feature_version = le32_to_cpu(vpe_hdr->ucode_feature_version);

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP) {
		struct amdgpu_firmware_info *info;

		info = &adev->firmware.ucode[AMDGPU_UCODE_ID_VPE_CTX];
		info->ucode_id = AMDGPU_UCODE_ID_VPE_CTX;
		info->fw = adev->vpe.fw;
		adev->firmware.fw_size +=
			ALIGN(le32_to_cpu(vpe_hdr->ctx_ucode_size_bytes), PAGE_SIZE);

		info = &adev->firmware.ucode[AMDGPU_UCODE_ID_VPE_CTL];
		info->ucode_id = AMDGPU_UCODE_ID_VPE_CTL;
		info->fw = adev->vpe.fw;
		adev->firmware.fw_size +=
			ALIGN(le32_to_cpu(vpe_hdr->ctl_ucode_size_bytes), PAGE_SIZE);
	}

	return 0;
out:
	dev_err(adev->dev, "fail to initialize vpe microcode\n");
	release_firmware(adev->vpe.fw);
	adev->vpe.fw = NULL;
	return ret;
}

int amdgpu_vpe_ring_init(struct amdgpu_vpe *vpe)
{
	struct amdgpu_device *adev = container_of(vpe, struct amdgpu_device, vpe);
	struct amdgpu_ring *ring = &vpe->ring;
	int ret;

	ring->ring_obj = NULL;
	ring->use_doorbell = true;
	ring->vm_hub = AMDGPU_MMHUB0(0);
	ring->doorbell_index = (adev->doorbell_index.vpe_ring << 1);
	snprintf(ring->name, 4, "vpe");

	ret = amdgpu_ring_init(adev, ring, 1024, &vpe->trap_irq, 0,
			     AMDGPU_RING_PRIO_DEFAULT, NULL);
	if (ret)
		return ret;

	return 0;
}

int amdgpu_vpe_ring_fini(struct amdgpu_vpe *vpe)
{
	amdgpu_ring_fini(&vpe->ring);

	return 0;
}

static int vpe_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_vpe *vpe = &adev->vpe;

	switch (amdgpu_ip_version(adev, VPE_HWIP, 0)) {
	case IP_VERSION(6, 1, 0):
		vpe_v6_1_set_funcs(vpe);
		break;
	default:
		return -EINVAL;
	}

	vpe_set_ring_funcs(adev);
	vpe_set_regs(vpe);

	return 0;
}


static int vpe_common_init(struct amdgpu_vpe *vpe)
{
	struct amdgpu_device *adev = container_of(vpe, struct amdgpu_device, vpe);
	int r;

	r = amdgpu_bo_create_kernel(adev, PAGE_SIZE, PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_GTT,
				    &adev->vpe.cmdbuf_obj,
				    &adev->vpe.cmdbuf_gpu_addr,
				    (void **)&adev->vpe.cmdbuf_cpu_addr);
	if (r) {
		dev_err(adev->dev, "VPE: failed to allocate cmdbuf bo %d\n", r);
		return r;
	}

	return 0;
}

static int vpe_sw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_vpe *vpe = &adev->vpe;
	int ret;

	ret = vpe_common_init(vpe);
	if (ret)
		goto out;

	ret = vpe_irq_init(vpe);
	if (ret)
		goto out;

	ret = vpe_ring_init(vpe);
	if (ret)
		goto out;

	ret = vpe_init_microcode(vpe);
	if (ret)
		goto out;
out:
	return ret;
}

static int vpe_sw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_vpe *vpe = &adev->vpe;

	release_firmware(vpe->fw);
	vpe->fw = NULL;

	vpe_ring_fini(vpe);

	amdgpu_bo_free_kernel(&adev->vpe.cmdbuf_obj,
			      &adev->vpe.cmdbuf_gpu_addr,
			      (void **)&adev->vpe.cmdbuf_cpu_addr);

	return 0;
}

static int vpe_hw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_vpe *vpe = &adev->vpe;
	int ret;

	ret = vpe_load_microcode(vpe);
	if (ret)
		return ret;

	ret = vpe_ring_start(vpe);
	if (ret)
		return ret;

	return 0;
}

static int vpe_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct amdgpu_vpe *vpe = &adev->vpe;

	vpe_ring_stop(vpe);

	return 0;
}

static int vpe_suspend(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	return vpe_hw_fini(adev);
}

static int vpe_resume(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	return vpe_hw_init(adev);
}

static void vpe_ring_insert_nop(struct amdgpu_ring *ring, uint32_t count)
{
	int i;

	for (i = 0; i < count; i++)
		if (i == 0)
			amdgpu_ring_write(ring, ring->funcs->nop |
				VPE_CMD_NOP_HEADER_COUNT(count - 1));
		else
			amdgpu_ring_write(ring, ring->funcs->nop);
}

static uint64_t vpe_get_csa_mc_addr(struct amdgpu_ring *ring, uint32_t vmid)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t index = 0;
	uint64_t csa_mc_addr;

	if (amdgpu_sriov_vf(adev) || vmid == 0 || !adev->gfx.mcbp)
		return 0;

	csa_mc_addr = amdgpu_csa_vaddr(adev) + AMDGPU_CSA_VPE_OFFSET +
		      index * AMDGPU_CSA_VPE_SIZE;

	return csa_mc_addr;
}

static void vpe_ring_emit_ib(struct amdgpu_ring *ring,
			     struct amdgpu_job *job,
			     struct amdgpu_ib *ib,
			     uint32_t flags)
{
	uint32_t vmid = AMDGPU_JOB_GET_VMID(job);
	uint64_t csa_mc_addr = vpe_get_csa_mc_addr(ring, vmid);

	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_INDIRECT, 0) |
				VPE_CMD_INDIRECT_HEADER_VMID(vmid & 0xf));

	/* base must be 32 byte aligned */
	amdgpu_ring_write(ring, ib->gpu_addr & 0xffffffe0);
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, ib->length_dw);
	amdgpu_ring_write(ring, lower_32_bits(csa_mc_addr));
	amdgpu_ring_write(ring, upper_32_bits(csa_mc_addr));
}

static void vpe_ring_emit_fence(struct amdgpu_ring *ring, uint64_t addr,
				uint64_t seq, unsigned int flags)
{
	int i = 0;

	do {
		/* write the fence */
		amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_FENCE, 0));
		/* zero in first two bits */
		WARN_ON_ONCE(addr & 0x3);
		amdgpu_ring_write(ring, lower_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(addr));
		amdgpu_ring_write(ring, i == 0 ? lower_32_bits(seq) : upper_32_bits(seq));
		addr += 4;
	} while ((flags & AMDGPU_FENCE_FLAG_64BIT) && (i++ < 1));

	if (flags & AMDGPU_FENCE_FLAG_INT) {
		/* generate an interrupt */
		amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_TRAP, 0));
		amdgpu_ring_write(ring, 0);
	}

}

static void vpe_ring_emit_pipeline_sync(struct amdgpu_ring *ring)
{
	uint32_t seq = ring->fence_drv.sync_seq;
	uint64_t addr = ring->fence_drv.gpu_addr;

	/* wait for idle */
	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_POLL_REGMEM,
				VPE_POLL_REGMEM_SUBOP_REGMEM) |
				VPE_CMD_POLL_REGMEM_HEADER_FUNC(3) | /* equal */
				VPE_CMD_POLL_REGMEM_HEADER_MEM(1));
	amdgpu_ring_write(ring, addr & 0xfffffffc);
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, seq); /* reference */
	amdgpu_ring_write(ring, 0xffffffff); /* mask */
	amdgpu_ring_write(ring, VPE_CMD_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
				VPE_CMD_POLL_REGMEM_DW5_INTERVAL(4));
}

static void vpe_ring_emit_wreg(struct amdgpu_ring *ring, uint32_t reg, uint32_t val)
{
	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_REG_WRITE, 0));
	amdgpu_ring_write(ring,	reg << 2);
	amdgpu_ring_write(ring, val);
}

static void vpe_ring_emit_reg_wait(struct amdgpu_ring *ring, uint32_t reg,
				   uint32_t val, uint32_t mask)
{
	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_POLL_REGMEM,
				VPE_POLL_REGMEM_SUBOP_REGMEM) |
				VPE_CMD_POLL_REGMEM_HEADER_FUNC(3) | /* equal */
				VPE_CMD_POLL_REGMEM_HEADER_MEM(0));
	amdgpu_ring_write(ring, reg << 2);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, val); /* reference */
	amdgpu_ring_write(ring, mask); /* mask */
	amdgpu_ring_write(ring, VPE_CMD_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
				VPE_CMD_POLL_REGMEM_DW5_INTERVAL(10));
}

static void vpe_ring_emit_vm_flush(struct amdgpu_ring *ring, unsigned int vmid,
				   uint64_t pd_addr)
{
	amdgpu_gmc_emit_flush_gpu_tlb(ring, vmid, pd_addr);
}

static unsigned int vpe_ring_init_cond_exec(struct amdgpu_ring *ring)
{
	unsigned int ret;

	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_COND_EXE, 0));
	amdgpu_ring_write(ring, lower_32_bits(ring->cond_exe_gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(ring->cond_exe_gpu_addr));
	amdgpu_ring_write(ring, 1);
	ret = ring->wptr & ring->buf_mask;/* this is the offset we need patch later */
	amdgpu_ring_write(ring, 0x55aa55aa);/* insert dummy here and patch it later */

	return ret;
}

static void vpe_ring_patch_cond_exec(struct amdgpu_ring *ring, unsigned int offset)
{
	unsigned int cur;

	WARN_ON_ONCE(offset > ring->buf_mask);
	WARN_ON_ONCE(ring->ring[offset] != 0x55aa55aa);

	cur = (ring->wptr - 1) & ring->buf_mask;
	if (cur > offset)
		ring->ring[offset] = cur - offset;
	else
		ring->ring[offset] = (ring->buf_mask + 1) - offset + cur;
}

static int vpe_ring_preempt_ib(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_vpe *vpe = &adev->vpe;
	uint32_t preempt_reg = vpe->regs.queue0_preempt;
	int i, r = 0;

	/* assert preemption condition */
	amdgpu_ring_set_preempt_cond_exec(ring, false);

	/* emit the trailing fence */
	ring->trail_seq += 1;
	amdgpu_ring_alloc(ring, 10);
	vpe_ring_emit_fence(ring, ring->trail_fence_gpu_addr, ring->trail_seq, 0);
	amdgpu_ring_commit(ring);

	/* assert IB preemption */
	WREG32(vpe_get_reg_offset(vpe, ring->me, preempt_reg), 1);

	/* poll the trailing fence */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (ring->trail_seq ==
		    le32_to_cpu(*(ring->trail_fence_cpu_addr)))
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		r = -EINVAL;
		dev_err(adev->dev, "ring %d failed to be preempted\n", ring->idx);
	}

	/* deassert IB preemption */
	WREG32(vpe_get_reg_offset(vpe, ring->me, preempt_reg), 0);

	/* deassert the preemption condition */
	amdgpu_ring_set_preempt_cond_exec(ring, true);

	return r;
}

static int vpe_set_clockgating_state(void *handle,
				     enum amd_clockgating_state state)
{
	return 0;
}

static int vpe_set_powergating_state(void *handle,
				     enum amd_powergating_state state)
{
	return 0;
}

static uint64_t vpe_ring_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_vpe *vpe = &adev->vpe;
	uint64_t rptr;

	if (ring->use_doorbell) {
		rptr = atomic64_read((atomic64_t *)ring->rptr_cpu_addr);
		dev_dbg(adev->dev, "rptr/doorbell before shift == 0x%016llx\n", rptr);
	} else {
		rptr = RREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_rptr_hi));
		rptr = rptr << 32;
		rptr |= RREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_rptr_lo));
		dev_dbg(adev->dev, "rptr before shift [%i] == 0x%016llx\n", ring->me, rptr);
	}

	return (rptr >> 2);
}

static uint64_t vpe_ring_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_vpe *vpe = &adev->vpe;
	uint64_t wptr;

	if (ring->use_doorbell) {
		wptr = atomic64_read((atomic64_t *)ring->wptr_cpu_addr);
		dev_dbg(adev->dev, "wptr/doorbell before shift == 0x%016llx\n", wptr);
	} else {
		wptr = RREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_wptr_hi));
		wptr = wptr << 32;
		wptr |= RREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_wptr_lo));
		dev_dbg(adev->dev, "wptr before shift [%i] == 0x%016llx\n", ring->me, wptr);
	}

	return (wptr >> 2);
}

static void vpe_ring_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_vpe *vpe = &adev->vpe;

	if (ring->use_doorbell) {
		dev_dbg(adev->dev, "Using doorbell, \
			wptr_offs == 0x%08x, \
			lower_32_bits(ring->wptr) << 2 == 0x%08x, \
			upper_32_bits(ring->wptr) << 2 == 0x%08x\n",
			ring->wptr_offs,
			lower_32_bits(ring->wptr << 2),
			upper_32_bits(ring->wptr << 2));
		atomic64_set((atomic64_t *)ring->wptr_cpu_addr, ring->wptr << 2);
		WDOORBELL64(ring->doorbell_index, ring->wptr << 2);
	} else {
		dev_dbg(adev->dev, "Not using doorbell, \
			regVPEC_QUEUE0_RB_WPTR == 0x%08x, \
			regVPEC_QUEUE0_RB_WPTR_HI == 0x%08x\n",
			lower_32_bits(ring->wptr << 2),
			upper_32_bits(ring->wptr << 2));
		WREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_wptr_lo),
		       lower_32_bits(ring->wptr << 2));
		WREG32(vpe_get_reg_offset(vpe, ring->me, vpe->regs.queue0_rb_wptr_hi),
		       upper_32_bits(ring->wptr << 2));
	}
}

static int vpe_ring_test_ring(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	const uint32_t test_pattern = 0xdeadbeef;
	uint32_t index, i;
	uint64_t wb_addr;
	int ret;

	ret = amdgpu_device_wb_get(adev, &index);
	if (ret) {
		dev_err(adev->dev, "(%d) failed to allocate wb slot\n", ret);
		return ret;
	}

	adev->wb.wb[index] = 0;
	wb_addr = adev->wb.gpu_addr + (index * 4);

	ret = amdgpu_ring_alloc(ring, 4);
	if (ret) {
		dev_err(adev->dev, "amdgpu: dma failed to lock ring %d (%d).\n", ring->idx, ret);
		goto out;
	}

	amdgpu_ring_write(ring, VPE_CMD_HEADER(VPE_CMD_OPCODE_FENCE, 0));
	amdgpu_ring_write(ring, lower_32_bits(wb_addr));
	amdgpu_ring_write(ring, upper_32_bits(wb_addr));
	amdgpu_ring_write(ring, test_pattern);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (le32_to_cpu(adev->wb.wb[index]) == test_pattern)
			goto out;
		udelay(1);
	}

	ret = -ETIMEDOUT;
out:
	amdgpu_device_wb_free(adev, index);

	return ret;
}

static int vpe_ring_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	const uint32_t test_pattern = 0xdeadbeef;
	struct amdgpu_ib ib = {};
	struct dma_fence *f = NULL;
	uint32_t index;
	uint64_t wb_addr;
	int ret;

	ret = amdgpu_device_wb_get(adev, &index);
	if (ret) {
		dev_err(adev->dev, "(%d) failed to allocate wb slot\n", ret);
		return ret;
	}

	adev->wb.wb[index] = 0;
	wb_addr = adev->wb.gpu_addr + (index * 4);

	ret = amdgpu_ib_get(adev, NULL, 256, AMDGPU_IB_POOL_DIRECT, &ib);
	if (ret)
		goto err0;

	ib.ptr[0] = VPE_CMD_HEADER(VPE_CMD_OPCODE_FENCE, 0);
	ib.ptr[1] = lower_32_bits(wb_addr);
	ib.ptr[2] = upper_32_bits(wb_addr);
	ib.ptr[3] = test_pattern;
	ib.ptr[4] = VPE_CMD_HEADER(VPE_CMD_OPCODE_NOP, 0);
	ib.ptr[5] = VPE_CMD_HEADER(VPE_CMD_OPCODE_NOP, 0);
	ib.ptr[6] = VPE_CMD_HEADER(VPE_CMD_OPCODE_NOP, 0);
	ib.ptr[7] = VPE_CMD_HEADER(VPE_CMD_OPCODE_NOP, 0);
	ib.length_dw = 8;

	ret = amdgpu_ib_schedule(ring, 1, &ib, NULL, &f);
	if (ret)
		goto err1;

	ret = dma_fence_wait_timeout(f, false, timeout);
	if (ret <= 0) {
		ret = ret ? : -ETIMEDOUT;
		goto err1;
	}

	ret = (le32_to_cpu(adev->wb.wb[index]) == test_pattern) ? 0 : -EINVAL;

err1:
	amdgpu_ib_free(adev, &ib, NULL);
	dma_fence_put(f);
err0:
	amdgpu_device_wb_free(adev, index);

	return ret;
}

static const struct amdgpu_ring_funcs vpe_ring_funcs = {
	.type = AMDGPU_RING_TYPE_VPE,
	.align_mask = 0xf,
	.nop = VPE_CMD_HEADER(VPE_CMD_OPCODE_NOP, 0),
	.support_64bit_ptrs = true,
	.get_rptr = vpe_ring_get_rptr,
	.get_wptr = vpe_ring_get_wptr,
	.set_wptr = vpe_ring_set_wptr,
	.emit_frame_size =
		5 + /* vpe_ring_init_cond_exec */
		6 + /* vpe_ring_emit_pipeline_sync */
		10 + 10 + 10 + /* vpe_ring_emit_fence */
		/* vpe_ring_emit_vm_flush */
		SOC15_FLUSH_GPU_TLB_NUM_WREG * 3 +
		SOC15_FLUSH_GPU_TLB_NUM_REG_WAIT * 6,
	.emit_ib_size = 7 + 6,
	.emit_ib = vpe_ring_emit_ib,
	.emit_pipeline_sync = vpe_ring_emit_pipeline_sync,
	.emit_fence = vpe_ring_emit_fence,
	.emit_vm_flush = vpe_ring_emit_vm_flush,
	.emit_wreg = vpe_ring_emit_wreg,
	.emit_reg_wait = vpe_ring_emit_reg_wait,
	.emit_reg_write_reg_wait = amdgpu_ring_emit_reg_write_reg_wait_helper,
	.insert_nop = vpe_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
	.test_ring = vpe_ring_test_ring,
	.test_ib = vpe_ring_test_ib,
	.init_cond_exec = vpe_ring_init_cond_exec,
	.patch_cond_exec = vpe_ring_patch_cond_exec,
	.preempt_ib = vpe_ring_preempt_ib,
};

static void vpe_set_ring_funcs(struct amdgpu_device *adev)
{
	adev->vpe.ring.funcs = &vpe_ring_funcs;
}

const struct amd_ip_funcs vpe_ip_funcs = {
	.name = "vpe_v6_1",
	.early_init = vpe_early_init,
	.late_init = NULL,
	.sw_init = vpe_sw_init,
	.sw_fini = vpe_sw_fini,
	.hw_init = vpe_hw_init,
	.hw_fini = vpe_hw_fini,
	.suspend = vpe_suspend,
	.resume = vpe_resume,
	.soft_reset = NULL,
	.set_clockgating_state = vpe_set_clockgating_state,
	.set_powergating_state = vpe_set_powergating_state,
};

const struct amdgpu_ip_block_version vpe_v6_1_ip_block = {
	.type = AMD_IP_BLOCK_TYPE_VPE,
	.major = 6,
	.minor = 1,
	.rev = 0,
	.funcs = &vpe_ip_funcs,
};
