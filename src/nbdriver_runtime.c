/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_log.h"
#include "nbdriver_runtime.h"

struct nbdriver_runtime_state {
	u64 handle;
	u64 token_lo;
	u64 token_hi;
	u64 generation;
	u32 flags;
	u32 probe_mask;
	s32 error;
};

static DEFINE_MUTEX(nbdriver_runtime_lock);
static atomic64_t nbdriver_runtime_next_handle = ATOMIC64_INIT(0);
static struct nbdriver_runtime_state nbdriver_runtime;

int nbdriver_runtime_init(void)
{
	struct nbdriver_runtime_state state;

	memset(&state, 0, sizeof(state));
	state.handle = (u64)atomic64_inc_return(&nbdriver_runtime_next_handle);
	get_random_bytes(&state.token_lo, sizeof(state.token_lo));
	get_random_bytes(&state.token_hi, sizeof(state.token_hi));
	state.generation = state.handle;
	state.flags = NBDRIVER_RUNTIME_READY |
		NBDRIVER_RUNTIME_RESIDENT |
		NBDRIVER_RUNTIME_PROBES;
	/* The module entry probe is mandatory for the anonymous-FD handshake. */
	state.probe_mask = 1U;
	mutex_lock(&nbdriver_runtime_lock);
	nbdriver_runtime = state;
	mutex_unlock(&nbdriver_runtime_lock);
	nbd_info("stage=runtime status=ready handle=%llu generation=%llu\n",
		(unsigned long long)state.handle,
		(unsigned long long)state.generation);
	return 0;
}

void nbdriver_runtime_exit(void)
{
	mutex_lock(&nbdriver_runtime_lock);
	nbdriver_runtime.flags = 0;
	nbdriver_runtime.error = 0;
	nbdriver_runtime.probe_mask = 0;
	mutex_unlock(&nbdriver_runtime_lock);
	nbd_info("stage=runtime status=stopped\n");
}

long nbdriver_runtime_ioctl(unsigned long argument)
{
	struct nbdriver_runtime_status status;

	if (!argument)
		return -EFAULT;
	memset(&status, 0, sizeof(status));
	mutex_lock(&nbdriver_runtime_lock);
	status.version = NBDRIVER_ABI_VERSION;
	status.size = sizeof(status);
	status.flags = nbdriver_runtime.flags;
	status.error = nbdriver_runtime.error;
	status.handle = nbdriver_runtime.handle;
	status.token_lo = nbdriver_runtime.token_lo;
	status.token_hi = nbdriver_runtime.token_hi;
	status.generation = nbdriver_runtime.generation;
	status.probe_mask = nbdriver_runtime.probe_mask;
	mutex_unlock(&nbdriver_runtime_lock);
	return copy_to_user((void __user *)argument, &status, sizeof(status))
		? -EFAULT : 0;
}

long nbdriver_runtime_control_ioctl(unsigned long argument)
{
	struct nbdriver_runtime_control control;
	long ret = 0;

	if (!argument || copy_from_user(&control, (void __user *)argument,
				       sizeof(control)))
		return -EFAULT;
	if (control.version != NBDRIVER_ABI_VERSION ||
	    control.size != sizeof(control))
		return -EPROTO;
	mutex_lock(&nbdriver_runtime_lock);
	if (control.handle != nbdriver_runtime.handle ||
	    control.token_lo != nbdriver_runtime.token_lo ||
	    control.token_hi != nbdriver_runtime.token_hi) {
		ret = -ESTALE;
	} else if (control.command == NBDRIVER_RUNTIME_CONTROL_QUIESCE) {
		nbdriver_runtime.flags &= ~NBDRIVER_RUNTIME_READY;
		nbdriver_runtime.flags |= NBDRIVER_RUNTIME_QUIESCED;
	} else if (control.command == NBDRIVER_RUNTIME_CONTROL_RESUME) {
		nbdriver_runtime.flags |= NBDRIVER_RUNTIME_READY;
		nbdriver_runtime.flags &= ~NBDRIVER_RUNTIME_QUIESCED;
	} else {
		ret = -EINVAL;
	}
	control.error = (s32)ret;
	mutex_unlock(&nbdriver_runtime_lock);
	if (copy_to_user((void __user *)argument, &control, sizeof(control)))
		return -EFAULT;
	return ret;
}

bool nbdriver_runtime_quiesced(void)
{
	bool quiesced;
	mutex_lock(&nbdriver_runtime_lock);
	quiesced = (nbdriver_runtime.flags & NBDRIVER_RUNTIME_QUIESCED) != 0;
	mutex_unlock(&nbdriver_runtime_lock);
	return quiesced;
}

u32 nbdriver_runtime_features(void)
{
	return NBDRIVER_FEATURE_RUNTIME_STATUS;
}
