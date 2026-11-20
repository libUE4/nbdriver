// SPDX-License-Identifier: GPL-2.0-only
/* Touchscreen collector plus one kernel-owned virtual slot.  The kprobe
 * observes physical input while the ioctl path appends a reserved slot to the
 * same MT device.  The virtual slot is owned by the anon FD that initialized
 * it and is released automatically when that FD dies.
 */
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_touch.h"

#define NBDRIVER_TOUCH_SLOTS NBDRIVER_TOUCH_MAX_CONTACTS
#define NBDRIVER_TOUCH_VIRTUAL_MAX 4U

typedef int (*nbdriver_class_for_each_device_fn)(struct class *,
	struct device *, const void *, int (*)(struct device *, void *));

struct nbdriver_touch_slot_state {
	bool down;
	nbdriver_s32 tracking_id;
	nbdriver_s32 x;
	nbdriver_s32 y;
	nbdriver_s32 pressure;
};

static DEFINE_SPINLOCK(nbdriver_touch_lock);
static struct kprobe nbdriver_touch_probe;
static struct input_dev *nbdriver_touch_device;
static bool nbdriver_touch_multi;
static bool nbdriver_touch_registered;
static bool nbdriver_touch_probe_registered;
static const char *nbdriver_touch_backend;
static unsigned int nbdriver_touch_slot;
static nbdriver_s32 nbdriver_touch_x_min;
static nbdriver_s32 nbdriver_touch_x_max;
static nbdriver_s32 nbdriver_touch_y_min;
static nbdriver_s32 nbdriver_touch_y_max;
static nbdriver_u32 nbdriver_touch_flags;
static nbdriver_u64 nbdriver_touch_sequence;
static nbdriver_u64 nbdriver_touch_timestamp_ns;
static struct nbdriver_touch_slot_state
	nbdriver_touch_slots[NBDRIVER_TOUCH_SLOTS];

struct nbdriver_touch_virtual_state {
	struct input_dev *dev;
	unsigned int original_slots;
	unsigned int physical_slots;
	unsigned int virtual_slots;
	unsigned int original_mt_flags;
	nbdriver_s32 x_min, x_max, y_min, y_max;
	nbdriver_u32 screen_width, screen_height;
	struct input_absinfo original_slot_abs;
	bool have_slot_abs;
	bool had_btn_touch;
	bool had_btn_finger;
	bool had_btn_doubletap;
	struct file *owner;
	bool active[NBDRIVER_TOUCH_VIRTUAL_MAX];
	int tracking_ids[NBDRIVER_TOUCH_VIRTUAL_MAX];
	int next_tracking_id;
	bool initialized;
};

static DEFINE_MUTEX(nbdriver_touch_virtual_lock);
static struct nbdriver_touch_virtual_state nbdriver_touch_virtual;

static bool nbdriver_touch_candidate(struct input_dev *dev)
{
	if (!dev || !test_bit(EV_ABS, dev->evbit))
		return false;
	if (test_bit(ABS_MT_POSITION_X, dev->absbit) &&
	    test_bit(ABS_MT_POSITION_Y, dev->absbit))
		return true;
	return test_bit(ABS_X, dev->absbit) && test_bit(ABS_Y, dev->absbit) &&
		(test_bit(BTN_TOUCH, dev->keybit) ||
		 test_bit(BTN_TOOL_FINGER, dev->keybit));
}

static const struct input_absinfo *nbdriver_touch_abs(
	struct input_dev *dev, unsigned int code)
{
	if (!dev || !dev->absinfo || code > ABS_MAX)
		return NULL;
	return &dev->absinfo[code];
}

static void nbdriver_touch_reset_slots_locked(void)
{
	unsigned int i;

	for (i = 0; i < NBDRIVER_TOUCH_SLOTS; ++i) {
		nbdriver_touch_slots[i].down = false;
		nbdriver_touch_slots[i].tracking_id = -1;
		nbdriver_touch_slots[i].x = 0;
		nbdriver_touch_slots[i].y = 0;
		nbdriver_touch_slots[i].pressure = 0;
	}
	nbdriver_touch_slot = 0;
}

static bool nbdriver_touch_bind_locked(struct input_dev *dev)
{
	const struct input_absinfo *x;
	const struct input_absinfo *y;

	if (!nbdriver_touch_candidate(dev))
		return false;
	nbdriver_touch_multi = test_bit(ABS_MT_POSITION_X, dev->absbit) &&
		test_bit(ABS_MT_POSITION_Y, dev->absbit);
	if (nbdriver_touch_multi && (!dev->mt || dev->mt->num_slots < 1 ||
		dev->mt->num_slots > NBDRIVER_TOUCH_SLOTS))
		return false;
	x = nbdriver_touch_abs(dev, nbdriver_touch_multi ? ABS_MT_POSITION_X : ABS_X);
	y = nbdriver_touch_abs(dev, nbdriver_touch_multi ? ABS_MT_POSITION_Y : ABS_Y);
	if (!x || !y || x->maximum <= x->minimum || y->maximum <= y->minimum)
		return false;
	nbdriver_touch_device = dev;
	nbdriver_touch_x_min = x->minimum;
	nbdriver_touch_x_max = x->maximum;
	nbdriver_touch_y_min = y->minimum;
	nbdriver_touch_y_max = y->maximum;
	nbdriver_touch_flags = NBDRIVER_TOUCH_F_VALID |
		(nbdriver_touch_multi ? NBDRIVER_TOUCH_F_MT : 0);
	nbdriver_touch_sequence = 0;
	nbdriver_touch_timestamp_ns = 0;
	nbdriver_touch_reset_slots_locked();
	return true;
}

static int NBDRIVER_NO_CFI nbdriver_touch_pre(struct kprobe *probe,
	struct pt_regs *regs)
{
	struct input_dev *dev;
	unsigned int type;
	unsigned int code;
	int value;
	unsigned long irq_flags;
	struct nbdriver_touch_slot_state *slot;
	const struct input_absinfo *pressure;

	(void)probe;
	if (!regs)
		return 0;
	dev = (struct input_dev *)(unsigned long)regs->regs[0];
	type = (unsigned int)regs->regs[1];
	code = (unsigned int)regs->regs[2];
	value = (int)regs->regs[3];
	if (!dev)
		return 0;

	spin_lock_irqsave(&nbdriver_touch_lock, irq_flags);
	if (!nbdriver_touch_device && !nbdriver_touch_bind_locked(dev)) {
		spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
		return 0;
	}
	if (dev != nbdriver_touch_device) {
		spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
		return 0;
	}
	if (type == EV_ABS) {
		if (nbdriver_touch_multi) {
			if (code == ABS_MT_SLOT) {
				if (value >= 0 && value < NBDRIVER_TOUCH_SLOTS)
					nbdriver_touch_slot = (unsigned int)value;
			} else {
				slot = &nbdriver_touch_slots[nbdriver_touch_slot];
				if (code == ABS_MT_TRACKING_ID) {
					slot->tracking_id = value;
					slot->down = value >= 0;
				} else if (code == ABS_MT_POSITION_X) {
					slot->x = value;
				} else if (code == ABS_MT_POSITION_Y) {
					slot->y = value;
				} else if (code == ABS_MT_PRESSURE) {
					slot->pressure = value;
				}
			}
		} else if (code == ABS_X) {
			nbdriver_touch_slots[0].x = value;
		} else if (code == ABS_Y) {
			nbdriver_touch_slots[0].y = value;
		}
	} else if (type == EV_KEY && code == BTN_TOUCH && !nbdriver_touch_multi) {
		slot = &nbdriver_touch_slots[0];
		slot->down = value != 0;
		slot->tracking_id = value != 0 ? 0 : -1;
	} else if (type == EV_SYN && code == SYN_DROPPED) {
		nbdriver_touch_flags |= NBDRIVER_TOUCH_F_SYN_DROPPED;
		nbdriver_touch_reset_slots_locked();
	} else if (type == EV_SYN && code == SYN_REPORT) {
		nbdriver_touch_flags &= ~NBDRIVER_TOUCH_F_SYN_DROPPED;
		nbdriver_touch_sequence++;
		nbdriver_touch_timestamp_ns = ktime_get_ns();
	}
	pressure = nbdriver_touch_abs(dev, ABS_MT_PRESSURE);
	if (pressure && pressure->maximum <= pressure->minimum &&
	    nbdriver_touch_slot < NBDRIVER_TOUCH_SLOTS)
		nbdriver_touch_slots[nbdriver_touch_slot].pressure = 0;
	spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
	return 0;
}

static void nbdriver_touch_fill_locked(struct nbdriver_touch_snapshot *out)
{
	unsigned int i;

	*out = (struct nbdriver_touch_snapshot) {
		.version = NBDRIVER_TOUCH_ABI_VERSION,
		.size = sizeof(*out),
		.flags = nbdriver_touch_flags,
		.count = 0,
		.x_min = nbdriver_touch_x_min,
		.x_max = nbdriver_touch_x_max,
		.y_min = nbdriver_touch_y_min,
		.y_max = nbdriver_touch_y_max,
		.sequence = nbdriver_touch_sequence,
		.timestamp_ns = nbdriver_touch_timestamp_ns,
	};
	for (i = 0; i < NBDRIVER_TOUCH_SLOTS; ++i) {
		const struct nbdriver_touch_slot_state *slot = &nbdriver_touch_slots[i];
		if (!slot->down || out->count >= NBDRIVER_TOUCH_MAX_CONTACTS)
			continue;
		out->contacts[out->count++] = (struct nbdriver_touch_contact) {
			.tracking_id = slot->tracking_id,
			.x = slot->x,
			.y = slot->y,
			.pressure = slot->pressure,
			.flags = NBDRIVER_TOUCH_CONTACT_DOWN,
		};
	}
}

static int nbdriver_touch_match_device(struct device *device, void *data)
{
	struct input_dev *dev = to_input_dev(device);
	struct input_dev **result = data;

	if (!dev || !result || !nbdriver_touch_candidate(dev))
		return 0;
	if (!dev->mt || dev->mt->num_slots < 2 ||
	    dev->mt->num_slots > NBDRIVER_TOUCH_SLOTS)
		return 0;
	*result = dev;
	return 1;
}

static struct input_dev *nbdriver_touch_find_device(void)
{
	struct input_dev *dev = NULL;
	nbdriver_class_for_each_device_fn for_each_device;
	struct class *input_cls;

	/* The kernel already owns the input class; enumerate it instead of opening
	 * or creating an evdev node from userspace. */
	for_each_device = (nbdriver_class_for_each_device_fn)
		nbdriver_ksym_lookup_optional("class_for_each_device");
	input_cls = (struct class *)nbdriver_ksym_lookup_optional("input_class");
	if (!for_each_device || !input_cls)
		return NULL;
	for_each_device(input_cls, NULL, &dev, nbdriver_touch_match_device);
	return dev;
}

static unsigned int nbdriver_touch_virtual_active_count_locked(void)
{
	unsigned int i;
	unsigned int count = 0;

	for (i = 0; i < nbdriver_touch_virtual.virtual_slots; ++i)
		if (nbdriver_touch_virtual.active[i])
			++count;
	return count;
}

static unsigned int nbdriver_touch_physical_active_count(
	struct input_dev *dev, unsigned int slots)
{
	unsigned int i;
	unsigned int count = 0;

	if (!dev || !dev->mt)
		return 0;
	for (i = 0; i < slots; ++i)
		if (input_mt_get_value(&dev->mt->slots[i], ABS_MT_TRACKING_ID) != -1)
			++count;
	return count;
}

static int nbdriver_touch_virtual_coordinate(nbdriver_s32 value,
	nbdriver_u32 screen, nbdriver_s32 minimum, nbdriver_s32 maximum)
{
	long long scaled;

	if (!screen || maximum <= minimum || value < 0 ||
	    (nbdriver_u32)value > screen)
		return -EINVAL;
	scaled = (long long)value * (maximum - minimum);
	return minimum + (int)(scaled / screen);
}

static int nbdriver_touch_virtual_report_locked(unsigned int slot,
	int x, int y, bool touching)
{
	struct input_dev *dev = nbdriver_touch_virtual.dev;
	struct input_mt *mt;
	unsigned int hardware_slot;
	unsigned int old_slot;
	unsigned int total;

	if (!dev || !dev->mt || slot >= nbdriver_touch_virtual.virtual_slots)
		return -ENODEV;
	mt = dev->mt;
	hardware_slot = nbdriver_touch_virtual.physical_slots + slot;
	old_slot = mt->slot;
	if (old_slot >= nbdriver_touch_virtual.physical_slots)
		old_slot = 0;
	/* The physical driver owns only the prefix. Temporarily expose the full
	 * original range for this one virtual transaction, then restore it before
	 * returning to the physical stream. */
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.original_slots);
	input_event(dev, EV_ABS, ABS_MT_SLOT, (int)hardware_slot);
	input_event(dev, EV_ABS, ABS_MT_TRACKING_ID,
		touching ? nbdriver_touch_virtual.tracking_ids[slot] : -1);
	if (touching) {
		input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
		input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);
	}
	input_event(dev, EV_ABS, ABS_MT_SLOT, (int)old_slot);
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.physical_slots);
	total = nbdriver_touch_physical_active_count(dev,
		nbdriver_touch_virtual.physical_slots) +
		nbdriver_touch_virtual_active_count_locked();
	if (nbdriver_touch_virtual.had_btn_touch)
		input_event(dev, EV_KEY, BTN_TOUCH, total > 0);
	if (nbdriver_touch_virtual.had_btn_finger)
		input_event(dev, EV_KEY, BTN_TOOL_FINGER, total == 1);
	if (nbdriver_touch_virtual.had_btn_doubletap)
		input_event(dev, EV_KEY, BTN_TOOL_DOUBLETAP, total >= 2);
	input_event(dev, EV_SYN, SYN_REPORT, 0);
	return 0;
}

static void nbdriver_touch_virtual_reset_locked(void)
{
	unsigned int i;
	memset(&nbdriver_touch_virtual, 0, sizeof(nbdriver_touch_virtual));
	for (i = 0; i < NBDRIVER_TOUCH_VIRTUAL_MAX; ++i)
		nbdriver_touch_virtual.tracking_ids[i] = -1;
	nbdriver_touch_virtual.next_tracking_id = 40000;
}

static int nbdriver_touch_virtual_init_locked(
	struct file *owner, struct nbdriver_touch_virtual_request *request)
{
	struct input_dev *dev;
	struct input_mt *mt;
	unsigned int slots;

	if (!request->virtual_slots ||
	    request->virtual_slots > NBDRIVER_TOUCH_VIRTUAL_MAX ||
	    !request->screen_width || !request->screen_height)
		return -EINVAL;
	if (nbdriver_touch_virtual.initialized) {
		if (nbdriver_touch_virtual.owner == owner &&
		    request->virtual_slots == nbdriver_touch_virtual.virtual_slots)
			return 0;
		return -EBUSY;
	}
	dev = nbdriver_touch_device;
	if (!dev)
		dev = nbdriver_touch_find_device();
	if (!dev || !dev->mt)
		return -ENODEV;
	if (!nbdriver_touch_device) {
		unsigned long irq_flags;
		spin_lock_irqsave(&nbdriver_touch_lock, irq_flags);
		if (!nbdriver_touch_device && !nbdriver_touch_bind_locked(dev)) {
			spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
			return -EINVAL;
		}
		dev = nbdriver_touch_device;
		spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
	}
	slots = dev->mt->num_slots;
	if (slots <= request->virtual_slots || slots > NBDRIVER_TOUCH_SLOTS)
		return -EINVAL;
	if (!dev->absinfo || !test_bit(ABS_MT_SLOT, dev->absbit))
		return -EINVAL;
	mt = dev->mt;
	nbdriver_touch_virtual.dev = dev;
	nbdriver_touch_virtual.original_slots = slots;
	nbdriver_touch_virtual.physical_slots = slots - request->virtual_slots;
	nbdriver_touch_virtual.virtual_slots = request->virtual_slots;
	nbdriver_touch_virtual.original_mt_flags = mt->flags;
	nbdriver_touch_virtual.original_slot_abs = dev->absinfo[ABS_MT_SLOT];
	nbdriver_touch_virtual.have_slot_abs = true;
	nbdriver_touch_virtual.had_btn_touch = test_bit(BTN_TOUCH, dev->keybit);
	nbdriver_touch_virtual.had_btn_finger = test_bit(BTN_TOOL_FINGER, dev->keybit);
	nbdriver_touch_virtual.had_btn_doubletap = test_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
	nbdriver_touch_virtual.owner = owner;
	nbdriver_touch_virtual.x_min = nbdriver_touch_x_min;
	nbdriver_touch_virtual.x_max = nbdriver_touch_x_max;
	nbdriver_touch_virtual.y_min = nbdriver_touch_y_min;
	nbdriver_touch_virtual.y_max = nbdriver_touch_y_max;
	nbdriver_touch_virtual.screen_width = request->screen_width;
	nbdriver_touch_virtual.screen_height = request->screen_height;
	nbdriver_touch_virtual.next_tracking_id = 40000;
	get_device(&dev->dev);
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.physical_slots);
	mt->flags &= ~INPUT_MT_DROP_UNUSED;
	mt->flags |= INPUT_MT_DIRECT;
	mt->flags &= ~INPUT_MT_POINTER;
	input_set_abs_params(dev, ABS_MT_SLOT, 0, (int)slots - 1, 0, 0);
	{
		unsigned int i;
		for (i = 0; i < NBDRIVER_TOUCH_VIRTUAL_MAX; ++i)
			nbdriver_touch_virtual.tracking_ids[i] = -1;
	}
	nbdriver_touch_virtual.initialized = true;
	nbd_info("stage=touch_virtual operation=init status=ready owner_pid=%d slots=%u physical_slots=%u range=%d..%d,%d..%d\n",
		 current->tgid, request->virtual_slots,
		 nbdriver_touch_virtual.physical_slots,
		 nbdriver_touch_virtual.x_min, nbdriver_touch_virtual.x_max,
		 nbdriver_touch_virtual.y_min, nbdriver_touch_virtual.y_max);
	request->x_min = nbdriver_touch_virtual.x_min;
	request->x_max = nbdriver_touch_virtual.x_max;
	request->y_min = nbdriver_touch_virtual.y_min;
	request->y_max = nbdriver_touch_virtual.y_max;
	request->active = 1;
	return 0;
}

static void nbdriver_touch_virtual_destroy_locked(void)
{
	struct input_dev *dev = nbdriver_touch_virtual.dev;
	unsigned int i;

	if (!nbdriver_touch_virtual.initialized)
		return;
	if (dev) {
		for (i = 0; i < nbdriver_touch_virtual.virtual_slots; ++i) {
			if (!nbdriver_touch_virtual.active[i])
				continue;
			nbdriver_touch_virtual.active[i] = false;
			(void)nbdriver_touch_virtual_report_locked(i, 0, 0, false);
		}
		if (dev->mt) {
			WRITE_ONCE(dev->mt->num_slots, nbdriver_touch_virtual.original_slots);
			dev->mt->flags = nbdriver_touch_virtual.original_mt_flags;
		}
		if (nbdriver_touch_virtual.have_slot_abs)
			dev->absinfo[ABS_MT_SLOT] = nbdriver_touch_virtual.original_slot_abs;
		put_device(&dev->dev);
	}
	nbd_info("stage=touch_virtual operation=destroy status=released owner_pid=%d\n",
		 current->tgid);
	nbdriver_touch_virtual_reset_locked();
}

static int nbdriver_touch_virtual_event_locked(
	struct file *owner, struct nbdriver_touch_virtual_request *request)
{
	unsigned int slot = request->slot;
	int x;
	int y;
	int ret;

	if (!nbdriver_touch_virtual.initialized)
		return -ENODEV;
	if (nbdriver_touch_virtual.owner != owner)
		return -EPERM;
	if (slot >= nbdriver_touch_virtual.virtual_slots)
		return -EINVAL;
	if (!request->screen_width || !request->screen_height ||
	    request->screen_width > 8192U || request->screen_height > 8192U)
		return -EINVAL;
	/* Userspace sends the current display size with every event. This keeps a
	 * rotation or display-size change effective without recreating the bridge. */
	nbdriver_touch_virtual.screen_width = request->screen_width;
	nbdriver_touch_virtual.screen_height = request->screen_height;
	if (request->operation != NBDRIVER_TOUCH_VIRTUAL_UP) {
		x = nbdriver_touch_virtual_coordinate(request->x,
			nbdriver_touch_virtual.screen_width,
			nbdriver_touch_virtual.x_min, nbdriver_touch_virtual.x_max);
		y = nbdriver_touch_virtual_coordinate(request->y,
			nbdriver_touch_virtual.screen_height,
			nbdriver_touch_virtual.y_min, nbdriver_touch_virtual.y_max);
		if (x < 0 || y < 0)
			return -EINVAL;
	}
	if (request->operation == NBDRIVER_TOUCH_VIRTUAL_DOWN) {
		if (nbdriver_touch_virtual.active[slot])
			return -EALREADY;
		nbdriver_touch_virtual.tracking_ids[slot] =
			nbdriver_touch_virtual.next_tracking_id++;
		nbdriver_touch_virtual.active[slot] = true;
		ret = nbdriver_touch_virtual_report_locked(slot, x, y, true);
		if (ret)
			nbdriver_touch_virtual.active[slot] = false;
		return ret;
	}
	if (request->operation == NBDRIVER_TOUCH_VIRTUAL_MOVE) {
		if (!nbdriver_touch_virtual.active[slot])
			return -ENOENT;
		return nbdriver_touch_virtual_report_locked(slot, x, y, true);
	}
	if (request->operation == NBDRIVER_TOUCH_VIRTUAL_UP) {
		if (!nbdriver_touch_virtual.active[slot])
			return -ENOENT;
		nbdriver_touch_virtual.active[slot] = false;
		ret = nbdriver_touch_virtual_report_locked(slot, 0, 0, false);
		return ret;
	}
	return -EINVAL;
}

static long nbdriver_touch_virtual_ioctl(struct file *owner,
	unsigned long argument)
{
	struct nbdriver_touch_virtual_request request;
	int ret;

	if (!argument || copy_from_user(&request, (void __user *)argument,
		sizeof(request)))
		return -EFAULT;
	if (request.version != NBDRIVER_TOUCH_VIRTUAL_ABI_VERSION ||
	    request.size != sizeof(request))
		return -EPROTO;
	mutex_lock(&nbdriver_touch_virtual_lock);
	if (request.operation == NBDRIVER_TOUCH_VIRTUAL_INIT)
		ret = nbdriver_touch_virtual_init_locked(owner, &request);
	else if (request.operation == NBDRIVER_TOUCH_VIRTUAL_DESTROY) {
		if (!nbdriver_touch_virtual.initialized)
			ret = 0;
		else if (nbdriver_touch_virtual.owner != owner)
			ret = -EPERM;
		else {
			nbdriver_touch_virtual_destroy_locked();
			ret = 0;
		}
	} else
		ret = nbdriver_touch_virtual_event_locked(owner, &request);
	request.error = ret;
	request.active = nbdriver_touch_virtual.initialized ? 1U : 0U;
	mutex_unlock(&nbdriver_touch_virtual_lock);
	return copy_to_user((void __user *)argument, &request, sizeof(request)) ?
		-EFAULT : (ret ? ret : 0);
}

long nbdriver_touch_ioctl(struct file *file, unsigned int command,
	unsigned long argument)
{
	struct nbdriver_touch_snapshot snapshot;
	unsigned long irq_flags;

	if (command == NBDRIVER_IOC_TOUCH_VIRTUAL)
		return nbdriver_touch_virtual_ioctl(file, argument);
	if (command != NBDRIVER_IOC_TOUCH_READ)
		return -ENOTTY;
	if (!argument)
		return -EFAULT;
	spin_lock_irqsave(&nbdriver_touch_lock, irq_flags);
	if (!nbdriver_touch_registered || !nbdriver_touch_device) {
		spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
		return -ENODEV;
	}
	nbdriver_touch_fill_locked(&snapshot);
	spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
	return copy_to_user((void __user *)argument, &snapshot, sizeof(snapshot)) ?
		-EFAULT : 0;
}

void nbdriver_touch_release(struct file *file)
{
	mutex_lock(&nbdriver_touch_virtual_lock);
	if (nbdriver_touch_virtual.initialized &&
	    nbdriver_touch_virtual.owner == file)
		nbdriver_touch_virtual_destroy_locked();
	mutex_unlock(&nbdriver_touch_virtual_lock);
}

u32 nbdriver_touch_features(void)
{
	return READ_ONCE(nbdriver_touch_registered) ?
		(NBDRIVER_FEATURE_TOUCH | NBDRIVER_FEATURE_TOUCH_VIRTUAL) : 0;
}

int nbdriver_touch_init(void)
{
	int ret_handle;
	int ret_event;

	nbdriver_touch_probe = (struct kprobe) {
		.symbol_name = "input_handle_event",
		.pre_handler = nbdriver_touch_pre,
	};
	ret_handle = register_kprobe(&nbdriver_touch_probe);
	if (!ret_handle) {
		nbdriver_touch_backend = "input_handle_event";
	} else {
		nbdriver_touch_probe = (struct kprobe) {
			.symbol_name = "input_event",
			.pre_handler = nbdriver_touch_pre,
		};
		ret_event = register_kprobe(&nbdriver_touch_probe);
		if (ret_event) {
			nbd_warn("stage=touch_init status=disabled input_handle_event_error=%d input_event_error=%d\n",
				 ret_handle, ret_event);
			return 0;
		}
		nbdriver_touch_backend = "input_event";
	}
	nbdriver_touch_probe_registered = true;
	WRITE_ONCE(nbdriver_touch_registered, true);
	nbd_info("stage=touch_init status=ready backend=%s snapshot=kernel injection=disabled\n",
		 nbdriver_touch_backend);
	return 0;
}

void nbdriver_touch_exit(void)
{
	mutex_lock(&nbdriver_touch_virtual_lock);
	nbdriver_touch_virtual_destroy_locked();
	mutex_unlock(&nbdriver_touch_virtual_lock);
	WRITE_ONCE(nbdriver_touch_registered, false);
	if (nbdriver_touch_probe_registered) {
		unregister_kprobe(&nbdriver_touch_probe);
		nbdriver_touch_probe_registered = false;
	}
	nbdriver_touch_backend = NULL;
	spin_lock_irq(&nbdriver_touch_lock);
	nbdriver_touch_device = NULL;
	nbdriver_touch_multi = false;
	nbdriver_touch_x_min = 0;
	nbdriver_touch_x_max = 0;
	nbdriver_touch_y_min = 0;
	nbdriver_touch_y_max = 0;
	nbdriver_touch_flags = 0;
	nbdriver_touch_sequence = 0;
	nbdriver_touch_timestamp_ns = 0;
	nbdriver_touch_reset_slots_locked();
	spin_unlock_irq(&nbdriver_touch_lock);
}
