// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2015-2021, Linaro Limited
 * Copyright (c) 2014, STMicroelectronics International N.V.
 */

#include <config.h>
#include <drivers/wdt.h>
#include <kernel/boot.h>
#include <kernel/misc.h>
#include <kernel/notif.h>
#include <kernel/tee_l2cc_mutex.h>
#include <kernel/virtualization.h>
#include <mm/core_mmu.h>
#include <optee_msg.h>
#include <sm/optee_smc.h>
#include <tee/entry_fast.h>

#ifdef CFG_CORE_RESERVED_SHM
static void tee_entry_get_shm_config(struct thread_smc_args *args)
{
	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = default_nsec_shm_paddr;
	args->a2 = default_nsec_shm_size;
	/* Should this be TEESMC cache attributes instead? */
	args->a3 = core_mmu_is_shm_cached();
}
#endif

static void tee_entry_fastcall_l2cc_mutex(struct thread_smc_args *args)
{
	TEE_Result ret;
#ifdef ARM32
	paddr_t pa = 0;

	switch (args->a1) {
	case OPTEE_SMC_L2CC_MUTEX_GET_ADDR:
		ret = tee_get_l2cc_mutex(&pa);
		reg_pair_from_64(pa, &args->a2, &args->a3);
		break;
	case OPTEE_SMC_L2CC_MUTEX_SET_ADDR:
		pa = reg_pair_to_64(args->a2, args->a3);
		ret = tee_set_l2cc_mutex(&pa);
		break;
	case OPTEE_SMC_L2CC_MUTEX_ENABLE:
		ret = tee_enable_l2cc_mutex();
		break;
	case OPTEE_SMC_L2CC_MUTEX_DISABLE:
		ret = tee_disable_l2cc_mutex();
		break;
	default:
		args->a0 = OPTEE_SMC_RETURN_EBADCMD;
		return;
	}
#else
	ret = TEE_ERROR_NOT_SUPPORTED;
#endif
	if (ret == TEE_ERROR_NOT_SUPPORTED)
		args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
	else if (ret)
		args->a0 = OPTEE_SMC_RETURN_EBADADDR;
	else
		args->a0 = OPTEE_SMC_RETURN_OK;
}

static void tee_entry_exchange_capabilities(struct thread_smc_args *args)
{
	bool dyn_shm_en __maybe_unused = false;

	/*
	 * Currently we ignore OPTEE_SMC_NSEC_CAP_UNIPROCESSOR.
	 *
	 * The memory mapping of shared memory is defined as normal
	 * shared memory for SMP systems and normal memory for UP
	 * systems. Currently we map all memory as shared in secure
	 * world.
	 *
	 * When translation tables are created with shared bit cleared for
	 * uniprocessor systems we'll need to check
	 * OPTEE_SMC_NSEC_CAP_UNIPROCESSOR.
	 */

	if (args->a1 & ~OPTEE_SMC_NSEC_CAP_UNIPROCESSOR) {
		/* Unknown capability. */
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = 0;
#ifdef CFG_CORE_RESERVED_SHM
	args->a1 |= OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM;
#endif
	if (IS_ENABLED(CFG_VIRTUALIZATION))
		args->a1 |= OPTEE_SMC_SEC_CAP_VIRTUALIZATION;
	args->a1 |= OPTEE_SMC_SEC_CAP_MEMREF_NULL;
	if (IS_ENABLED(CFG_CORE_ASYNC_NOTIF)) {
		args->a1 |= OPTEE_SMC_SEC_CAP_ASYNC_NOTIF;
		args->a2 = NOTIF_VALUE_MAX;
	}
	DMSG("Asynchronous notifications are %sabled",
	     IS_ENABLED(CFG_CORE_ASYNC_NOTIF) ? "en" : "dis");

#if defined(CFG_CORE_DYN_SHM)
	dyn_shm_en = core_mmu_nsec_ddr_is_defined();
	if (dyn_shm_en)
		args->a1 |= OPTEE_SMC_SEC_CAP_DYNAMIC_SHM;
#endif

	DMSG("Dynamic shared memory is %sabled", dyn_shm_en ? "en" : "dis");

	args->a1 |= OPTEE_SMC_SEC_CAP_RPC_ARG;
	args->a3 = THREAD_RPC_MAX_NUM_PARAMS;
}

static void tee_entry_disable_shm_cache(struct thread_smc_args *args)
{
	uint64_t cookie;

	if (!thread_disable_prealloc_rpc_cache(&cookie)) {
		args->a0 = OPTEE_SMC_RETURN_EBUSY;
		return;
	}

	if (!cookie) {
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = cookie >> 32;
	args->a2 = cookie;
}

static void tee_entry_enable_shm_cache(struct thread_smc_args *args)
{
	if (thread_enable_prealloc_rpc_cache())
		args->a0 = OPTEE_SMC_RETURN_OK;
	else
		args->a0 = OPTEE_SMC_RETURN_EBUSY;
}

static void tee_entry_boot_secondary(struct thread_smc_args *args)
{
#if defined(CFG_BOOT_SECONDARY_REQUEST)
	if (!boot_core_release(args->a1, (paddr_t)(args->a3)))
		args->a0 = OPTEE_SMC_RETURN_OK;
	else
		args->a0 = OPTEE_SMC_RETURN_EBADCMD;
#else
	args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
#endif
}

static void tee_entry_get_thread_count(struct thread_smc_args *args)
{
	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = CFG_NUM_THREADS;
}

#if defined(CFG_VIRTUALIZATION)
static void tee_entry_vm_created(struct thread_smc_args *args)
{
	uint16_t guest_id = args->a1;

	/* Only hypervisor can issue this request */
	if (args->a7 != HYP_CLNT_ID) {
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	if (virt_guest_created(guest_id))
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
	else
		args->a0 = OPTEE_SMC_RETURN_OK;
}

static void tee_entry_vm_destroyed(struct thread_smc_args *args)
{
	uint16_t guest_id = args->a1;

	/* Only hypervisor can issue this request */
	if (args->a7 != HYP_CLNT_ID) {
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	if (virt_guest_destroyed(guest_id))
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
	else
		args->a0 = OPTEE_SMC_RETURN_OK;
}
#endif

/* Note: this function is weak to let platforms add special handling */
void __weak tee_entry_fast(struct thread_smc_args *args)
{
	__tee_entry_fast(args);
}

static void get_async_notif_value(struct thread_smc_args *args)
{
	bool value_valid = false;
	bool value_pending = false;

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = notif_get_value(&value_valid, &value_pending);
	args->a2 = 0;
	if (value_valid)
		args->a2 |= OPTEE_SMC_ASYNC_NOTIF_VALID;
	if (value_pending)
		args->a2 |= OPTEE_SMC_ASYNC_NOTIF_PENDING;
}

static void get_it_value(struct thread_smc_args *args)
{
	bool value_valid = false;
	bool value_pending = false;

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = it_get_value(&value_valid, &value_pending);
	args->a2 = 0;
	if (value_valid)
		args->a2 |= OPTEE_SMC_IT_NOTIF_VALID;
	if (value_pending)
		args->a2 |= OPTEE_SMC_IT_NOTIF_PENDING;
}

static void set_it_mask(struct thread_smc_args *args)
{
	uint32_t it_value = args->a1;
	bool masked = args->a2;

	args->a0 = OPTEE_SMC_RETURN_OK;
	it_set_mask(it_value, masked);
}

/*
 * If tee_entry_fast() is overridden, it's still supposed to call this
 * function.
 */
void __tee_entry_fast(struct thread_smc_args *args)
{
	switch (args->a0) {

	/* Generic functions */
	case OPTEE_SMC_CALLS_COUNT:
		tee_entry_get_api_call_count(args);
		break;
	case OPTEE_SMC_CALLS_UID:
		tee_entry_get_api_uuid(args);
		break;
	case OPTEE_SMC_CALLS_REVISION:
		tee_entry_get_api_revision(args);
		break;
	case OPTEE_SMC_CALL_GET_OS_UUID:
		tee_entry_get_os_uuid(args);
		break;
	case OPTEE_SMC_CALL_GET_OS_REVISION:
		tee_entry_get_os_revision(args);
		break;

	/* OP-TEE specific SMC functions */
#ifdef CFG_CORE_RESERVED_SHM
	case OPTEE_SMC_GET_SHM_CONFIG:
		tee_entry_get_shm_config(args);
		break;
#endif
	case OPTEE_SMC_L2CC_MUTEX:
		tee_entry_fastcall_l2cc_mutex(args);
		break;
	case OPTEE_SMC_EXCHANGE_CAPABILITIES:
		tee_entry_exchange_capabilities(args);
		break;
	case OPTEE_SMC_DISABLE_SHM_CACHE:
		tee_entry_disable_shm_cache(args);
		break;
	case OPTEE_SMC_ENABLE_SHM_CACHE:
		tee_entry_enable_shm_cache(args);
		break;
	case OPTEE_SMC_BOOT_SECONDARY:
		tee_entry_boot_secondary(args);
		break;
	case OPTEE_SMC_GET_THREAD_COUNT:
		tee_entry_get_thread_count(args);
		break;

#if defined(CFG_VIRTUALIZATION)
	case OPTEE_SMC_VM_CREATED:
		tee_entry_vm_created(args);
		break;
	case OPTEE_SMC_VM_DESTROYED:
		tee_entry_vm_destroyed(args);
		break;
#endif

	case OPTEE_SMC_ENABLE_ASYNC_NOTIF:
		if (IS_ENABLED(CFG_CORE_ASYNC_NOTIF)) {
			notif_deliver_atomic_event(NOTIF_EVENT_STARTED);
			args->a0 = OPTEE_SMC_RETURN_OK;
		} else {
			args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		}
		break;
	case OPTEE_SMC_GET_ASYNC_NOTIF_VALUE:
		if (IS_ENABLED(CFG_CORE_ASYNC_NOTIF))
			get_async_notif_value(args);
		else
			args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		break;

	case OPTEE_SMC_GET_IT_NOTIF_VALUE:
		if (IS_ENABLED(CFG_CORE_ASYNC_NOTIF))
			get_it_value(args);
		else
			args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		break;

	case OPTEE_SMC_SET_IT_NOTIF_MASK:
		if (IS_ENABLED(CFG_CORE_ASYNC_NOTIF))
			set_it_mask(args);
		else
			args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		break;
#if defined(CFG_WDT_SM_HANDLER)
	case OPTEE_SMC_WATCHDOG:
			__wdt_sm_handler(args);
		break;
#endif
	default:
		args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		break;
	}
}

size_t tee_entry_generic_get_api_call_count(void)
{
	/*
	 * All the different calls handled in this file. If the specific
	 * target has additional calls it will call this function and
	 * add the number of calls the target has added.
	 */
	size_t ret = 12;

	if (IS_ENABLED(CFG_VIRTUALIZATION))
		ret += 2;

	return ret;
}

void __weak tee_entry_get_api_call_count(struct thread_smc_args *args)
{
	args->a0 = tee_entry_generic_get_api_call_count();
}

void __weak tee_entry_get_api_uuid(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_UID_0;
	args->a1 = OPTEE_MSG_UID_1;
	args->a2 = OPTEE_MSG_UID_2;
	args->a3 = OPTEE_MSG_UID_3;
}

void __weak tee_entry_get_api_revision(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_REVISION_MAJOR;
	args->a1 = OPTEE_MSG_REVISION_MINOR;
}

void __weak tee_entry_get_os_uuid(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_OS_OPTEE_UUID_0;
	args->a1 = OPTEE_MSG_OS_OPTEE_UUID_1;
	args->a2 = OPTEE_MSG_OS_OPTEE_UUID_2;
	args->a3 = OPTEE_MSG_OS_OPTEE_UUID_3;
}

void __weak tee_entry_get_os_revision(struct thread_smc_args *args)
{
	args->a0 = CFG_OPTEE_REVISION_MAJOR;
	args->a1 = CFG_OPTEE_REVISION_MINOR;
	args->a2 = TEE_IMPL_GIT_SHA1;
}
