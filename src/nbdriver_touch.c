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
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_touch.h"
#include "nbdriver_touch_route.h"

#define NBDRIVER_TOUCH_SLOTS NBDRIVER_TOUCH_MAX_CONTACTS
#define NBDRIVER_TOUCH_VIRTUAL_MAX 4U
#define NBDRIVER_TOUCH_WATCHDOG_MS 500U

/* Preserve the target KMI's exact prototype, including const qualifiers.
 * A handwritten near-match has a different KCFI type and panics at init. */
typedef __typeof__(&class_for_each_device) nbdriver_class_for_each_device_fn;
typedef void (*nbdriver_input_handle_event_fn)(struct input_dev *,
	unsigned int, unsigned int, int);
static nbdriver_input_handle_event_fn nbdriver_touch_handle_event;

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
	nbdriver_s32 x_min, x_max, y_min, y_max;
	nbdriver_u32 screen_width, screen_height;
	bool had_btn_touch;
	bool had_btn_finger;
	bool had_btn_doubletap;
	/* Preserve the contact shape/pressure contract used by the real
	 * touchscreen.  Without these fields Android can render the injected
	 * contact as an empty ring instead of a normal filled touch point. */
	bool has_touch_major;
	bool has_touch_minor;
	bool has_width_major;
	bool has_tool_type;
	bool has_pressure;
	struct file *owner;
	bool active[NBDRIVER_TOUCH_VIRTUAL_MAX];
	int tracking_ids[NBDRIVER_TOUCH_VIRTUAL_MAX];
	int next_tracking_id;
	unsigned long merged_keys;
	unsigned long frames;
	unsigned long last_event_jiffies;
	bool initialized;
};

static DEFINE_MUTEX(nbdriver_touch_virtual_lock);
static struct nbdriver_touch_virtual_state nbdriver_touch_virtual;
static struct delayed_work nbdriver_touch_watchdog_work;
static unsigned int nbdriver_touch_physical_active_count(
	struct input_dev *dev, unsigned int slots);

static void nbdriver_touch_virtual_watchdog(struct work_struct *work);

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

static void nbdriver_capture_complete(struct input_dev *dev);

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
	/* input_handle_event is called with dev->event_lock held. Physical
	 * drivers only count their own fingers when reporting BTN_TOUCH. An UP
	 * from that stream must not turn a still-live virtual finger into hover.
	 * Merge the actual MT slot states, without removing device capabilities
	 * or suppressing real fingers. The same rule applies to our own reports. */
	if (type == EV_KEY && READ_ONCE(nbdriver_touch_virtual.initialized) &&
	    dev == nbdriver_touch_virtual.dev && nbdriver_touch_handle_event) {
		unsigned int count = nbdriver_touch_physical_active_count(dev,
			nbdriver_touch_virtual.original_slots);
		int original = value;
		if (code == BTN_TOUCH)
			value = count != 0;
		else if (code == BTN_TOOL_FINGER)
			value = count == 1;
		else if (code == BTN_TOOL_DOUBLETAP)
			value = count == 2;
		else if (code == BTN_TOOL_TRIPLETAP)
			value = count == 3;
		else if (code == BTN_TOOL_QUADTAP)
			value = count == 4;
		else if (code == BTN_TOOL_QUINTTAP)
			value = count >= 5;
		if (value != original)
			++nbdriver_touch_virtual.merged_keys;
		regs->regs[3] = (unsigned long)value;
	}

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
					/* The read-only snapshot feeds the overlay UI.
					 * Never feed our output contacts back as real UI
					 * fingers: Android still receives them normally. */
					slot->down = value >= 0 &&
						!(READ_ONCE(nbdriver_touch_virtual.initialized) &&
						  nbdriver_touch_slot >=
						  nbdriver_touch_virtual.physical_slots);
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
	if (type == EV_SYN && code == SYN_REPORT)
		nbdriver_capture_complete(dev);
	return 0;
}

/* Separate from virtual injection: consume selected physical contacts only at
 * the completed-frame boundary. No input device or window is created. */
static struct kprobe nbdriver_capture_probe;
static bool nbdriver_capture_registered;
static bool nbdriver_capture_dispatching;
typedef void (*nbdriver_pass_values_fn)(struct input_dev *, struct input_value *, unsigned int);
static nbdriver_pass_values_fn nbdriver_capture_pass_values;
static struct input_dev *nbdriver_capture_device;
static struct file *nbdriver_capture_owner;
static unsigned long nbdriver_capture_deadline;
static struct nbdriver_touch_route nbdriver_capture_route;

static unsigned int nbdriver_capture_contacts(struct input_dev *dev,
	struct nbdriver_route_contact *contacts)
{
	unsigned int i, j, slots = dev->mt->num_slots;
	if (READ_ONCE(nbdriver_touch_virtual.initialized) &&
	    dev == nbdriver_touch_virtual.dev)
		slots = nbdriver_touch_virtual.original_slots;
	if (slots > NBDRIVER_TOUCH_MAX_CONTACTS)
		return 0;
	for (i = 0; i < slots; ++i)
		for (j = 0; j < NBDRIVER_ROUTE_AXES; ++j)
			contacts[i].value[j] = input_mt_get_value(&dev->mt->slots[i],
				nbdriver_route_axes[j]);
	return slots;
}

static void NBDRIVER_NO_CFI nbdriver_capture_complete(struct input_dev *dev)
{
	struct nbdriver_route_contact contacts[NBDRIVER_TOUCH_MAX_CONTACTS];
	unsigned int slots, physical, i, axes = 0, keys = 0, legacy = 0;
	static const unsigned int legacy_axes[] = {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TOOL_WIDTH};
	unsigned long flags;
	bool capture;
	struct input_value sync = {EV_SYN, SYN_REPORT, 0};
	unsigned int count;
	if (!nbdriver_capture_pass_values || dev != READ_ONCE(nbdriver_capture_device) || !dev->mt)
		return;
	spin_lock_irqsave(&nbdriver_touch_lock, flags);
	if (!nbdriver_capture_route.routing) {
		spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
		return;
	}
	slots = nbdriver_capture_contacts(dev, contacts);
	if (!slots) {
		spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
		return;
	}
	physical = READ_ONCE(nbdriver_touch_virtual.initialized) &&
		dev == nbdriver_touch_virtual.dev ? nbdriver_touch_virtual.physical_slots : slots;
	for (i = 0; i < NBDRIVER_ROUTE_AXES; ++i)
		if (test_bit(nbdriver_route_axes[i], dev->absbit))
			axes |= 1U << i;
	for (i = 0; i < ARRAY_SIZE(nbdriver_route_keys); ++i)
		if (test_bit(nbdriver_route_keys[i], dev->keybit))
			keys |= 1U << i;
	for (i = 0; i < ARRAY_SIZE(legacy_axes); ++i)
		if (test_bit(legacy_axes[i], dev->absbit))
			legacy |= 1U << i;
	capture = nbdriver_capture_owner && time_before(jiffies, nbdriver_capture_deadline);
	count = nbdriver_route_frame(&nbdriver_capture_route, &sync, 1, contacts,
		slots, physical, axes, keys, legacy, capture);
	spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
	if (count) {
		/* event_lock is already held by input_handle_event. Do not re-enter
		 * input_event, alter hardware slots, or send our output through the
		 * original-chunk gate again. This call is synchronous. */
		nbdriver_capture_dispatching = true;
		nbdriver_capture_pass_values(dev, nbdriver_capture_route.output, count);
		nbdriver_capture_dispatching = false;
	}
}

static int NBDRIVER_NO_CFI nbdriver_capture_pre(struct kprobe *probe,
	struct pt_regs *regs)
{
	struct input_dev *dev = (struct input_dev *)(unsigned long)regs->regs[0];
	struct input_value *values = (struct input_value *)(unsigned long)regs->regs[1];
	unsigned int count = (unsigned int)regs->regs[2];
	unsigned long flags;
	(void)probe;
	if (!dev || dev != READ_ONCE(nbdriver_capture_device) ||
		READ_ONCE(nbdriver_capture_dispatching) || !values || !count ||
		count > NBDRIVER_ROUTE_INPUT_MAX)
		return 0;
	spin_lock_irqsave(&nbdriver_touch_lock, flags);
	if (nbdriver_capture_route.routing) {
		count = nbdriver_route_original(&nbdriver_capture_route, values, count);
		regs->regs[1] = (unsigned long)nbdriver_capture_route.output;
		regs->regs[2] = count;
	}
	spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
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
	if (nbdriver_capture_device == nbdriver_touch_device && nbdriver_capture_route.routing)
		out->flags |= NBDRIVER_TOUCH_F_ROUTED;
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
		if (nbdriver_capture_device == nbdriver_touch_device &&
		    nbdriver_capture_owner && time_before(jiffies, nbdriver_capture_deadline) &&
		    nbdriver_capture_route.routing && nbdriver_capture_route.identity[i] == slot->tracking_id &&
		    nbdriver_capture_route.contact_epoch[i] == nbdriver_capture_route.epoch &&
		    nbdriver_capture_route.owner[i] == NBDRIVER_ROUTE_UI)
			out->contacts[out->count - 1].flags |= NBDRIVER_TOUCH_CONTACT_UI;
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
	/* class_for_each_device drops its iterator reference before returning. */
	get_device(&dev->dev);
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

static int nbdriver_touch_virtual_abs_value(struct input_dev *dev,
	unsigned int code, int preferred)
{
	const struct input_absinfo *abs;
	long value = preferred;

	abs = nbdriver_touch_abs(dev, code);
	/* Some panels advertise a zero-width pressure/width range but still
	 * emit nonzero raw values. Linux does not clamp EV_ABS to this range;
	 * do not clamp a live contact to zero here or mutate vendor absinfo. */
	if (!abs || abs->maximum <= abs->minimum)
		return preferred;
	if (value < abs->minimum)
		value = abs->minimum;
	if (value > abs->maximum)
		value = abs->maximum;
	return (int)value;
}

static int NBDRIVER_NO_CFI nbdriver_touch_virtual_report_locked(unsigned int slot,
	int x, int y, bool touching)
{
	struct input_dev *dev = nbdriver_touch_virtual.dev;
	struct input_mt *mt;
	unsigned int hardware_slot;
	unsigned int old_slot;
	unsigned int total;
	unsigned long flags;

	if (!dev || !dev->mt || !nbdriver_touch_handle_event ||
	    slot >= nbdriver_touch_virtual.virtual_slots)
		return -ENODEV;
	/* input_event locks one event at a time. A physical SLOT event between
	 * our tracking id and coordinates can corrupt either finger. Hold the
	 * input core lock for the complete frame and use its already-locked
	 * event handler; calling input_event under this lock would deadlock. */
	spin_lock_irqsave(&dev->event_lock, flags);
	mt = dev->mt;
	hardware_slot = nbdriver_touch_virtual.physical_slots + slot;
	old_slot = mt->slot;
	if (old_slot >= nbdriver_touch_virtual.physical_slots)
		old_slot = 0;
	/* The physical driver owns only the prefix. Temporarily expose the full
	 * original range for this one virtual transaction, then restore it before
	 * returning to the physical stream. */
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.original_slots);
	nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_SLOT, (int)hardware_slot);
	if (touching) {
		nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TRACKING_ID,
			nbdriver_touch_virtual.tracking_ids[slot]);
		nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
		nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);
		/* Match the reference virtual-input implementations: report a
		 * non-zero contact area and pressure on every live frame.  Values
		 * are clamped to the device's advertised range for vendor panels
		 * whose maximum is smaller than the reference value. */
		if (nbdriver_touch_virtual.has_touch_major)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR,
				nbdriver_touch_virtual_abs_value(dev,
					ABS_MT_TOUCH_MAJOR, 10));
		if (nbdriver_touch_virtual.has_touch_minor)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TOUCH_MINOR,
				nbdriver_touch_virtual_abs_value(dev,
					ABS_MT_TOUCH_MINOR, 10));
		if (nbdriver_touch_virtual.has_width_major)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR,
				nbdriver_touch_virtual_abs_value(dev,
					ABS_MT_WIDTH_MAJOR, 10));
		if (nbdriver_touch_virtual.has_pressure)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_PRESSURE,
				nbdriver_touch_virtual_abs_value(dev,
					ABS_MT_PRESSURE, 60));
		if (nbdriver_touch_virtual.has_tool_type)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
	} else {
		/* End the contact's shape before killing its tracking id.  Do not
		 * send position data after tracking_id=-1. */
		if (nbdriver_touch_virtual.has_touch_major)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 0);
		if (nbdriver_touch_virtual.has_touch_minor)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TOUCH_MINOR, 0);
		if (nbdriver_touch_virtual.has_width_major)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 0);
		if (nbdriver_touch_virtual.has_pressure)
			nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_PRESSURE, 0);
		nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
	}
	nbdriver_touch_handle_event(dev, EV_ABS, ABS_MT_SLOT, (int)old_slot);
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.physical_slots);
	total = nbdriver_touch_physical_active_count(dev,
		nbdriver_touch_virtual.original_slots);
	if (nbdriver_touch_virtual.had_btn_touch)
		nbdriver_touch_handle_event(dev, EV_KEY, BTN_TOUCH, total > 0);
	if (nbdriver_touch_virtual.had_btn_finger)
		nbdriver_touch_handle_event(dev, EV_KEY, BTN_TOOL_FINGER, total == 1);
	if (nbdriver_touch_virtual.had_btn_doubletap)
		nbdriver_touch_handle_event(dev, EV_KEY, BTN_TOOL_DOUBLETAP, total == 2);
	nbdriver_touch_handle_event(dev, EV_SYN, SYN_REPORT, 0);
	++nbdriver_touch_virtual.frames;
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return 0;
}

/* A blocked renderer or a failed userspace release must never leave a
 * kernel-owned contact down forever.  The watchdog runs in process context,
 * so it can use the same input frame path as the ioctl without doing input
 * work from a timer/IRQ handler. */
static void nbdriver_touch_virtual_watchdog(struct work_struct *work)
{
	unsigned int i;
	bool active = false;
	const unsigned long timeout = msecs_to_jiffies(
		NBDRIVER_TOUCH_WATCHDOG_MS);

	(void)work;
	mutex_lock(&nbdriver_touch_virtual_lock);
	if (nbdriver_touch_virtual.initialized) {
		for (i = 0; i < nbdriver_touch_virtual.virtual_slots; ++i) {
			if (nbdriver_touch_virtual.active[i]) {
				active = true;
				break;
			}
		}
		if (active && time_after_eq(jiffies,
				nbdriver_touch_virtual.last_event_jiffies + timeout)) {
			nbd_info("stage=touch_virtual operation=watchdog_release status=stale timeout_ms=%u owner_pid=%d\n",
				NBDRIVER_TOUCH_WATCHDOG_MS,
				 nbdriver_touch_virtual.owner ?
				 task_tgid_nr(current) : 0);
			for (i = 0; i < nbdriver_touch_virtual.virtual_slots; ++i) {
				if (!nbdriver_touch_virtual.active[i])
					continue;
				if (!nbdriver_touch_virtual_report_locked(i, 0, 0, false))
					nbdriver_touch_virtual.active[i] = false;
			}
			active = false;
		}
	}
	mutex_unlock(&nbdriver_touch_virtual_lock);
	if (active)
		schedule_delayed_work(&nbdriver_touch_watchdog_work,
			msecs_to_jiffies(NBDRIVER_TOUCH_WATCHDOG_MS));
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
	unsigned int i;
	unsigned long flags;
	int ret;

	if (!nbdriver_touch_handle_event)
		return -EOPNOTSUPP;
	if (!request->virtual_slots ||
	    request->virtual_slots > NBDRIVER_TOUCH_VIRTUAL_MAX ||
	    !request->screen_width || !request->screen_height ||
	    request->screen_width > 8192U || request->screen_height > 8192U)
		return -EINVAL;
	if (nbdriver_touch_virtual.initialized) {
		if (nbdriver_touch_virtual.owner == owner &&
		    request->virtual_slots == nbdriver_touch_virtual.virtual_slots)
			return 0;
		return -EBUSY;
	}
	dev = READ_ONCE(nbdriver_touch_device);
	if (dev)
		get_device(&dev->dev);
	else
		dev = nbdriver_touch_find_device();
	if (!dev)
		return -ENODEV;
	ret = -ENODEV;
	if (!dev->mt)
		goto put_device;
	{
		unsigned long irq_flags;
		spin_lock_irqsave(&nbdriver_touch_lock, irq_flags);
		if (!nbdriver_touch_device && !nbdriver_touch_bind_locked(dev)) {
			spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
			ret = -EINVAL;
			goto put_device;
		}
		/* A physical report may have selected another panel meanwhile. */
		if (nbdriver_touch_device != dev) {
			spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
			ret = -EAGAIN;
			goto put_device;
		}
		spin_unlock_irqrestore(&nbdriver_touch_lock, irq_flags);
	}
	slots = dev->mt->num_slots;
	ret = -EINVAL;
	if (slots <= request->virtual_slots || slots > NBDRIVER_TOUCH_SLOTS)
		goto put_device;
	if (!dev->absinfo || !test_bit(ABS_MT_SLOT, dev->absbit))
		goto put_device;
	mt = dev->mt;
	spin_lock_irqsave(&dev->event_lock, flags);
	/* Never steal a slot which already belongs to a physical finger. */
	for (i = slots - request->virtual_slots; i < slots; ++i) {
		if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1) {
			spin_unlock_irqrestore(&dev->event_lock, flags);
			ret = -EBUSY;
			goto put_device;
		}
	}
	nbdriver_touch_virtual.dev = dev;
	nbdriver_touch_virtual.original_slots = slots;
	nbdriver_touch_virtual.physical_slots = slots - request->virtual_slots;
	nbdriver_touch_virtual.virtual_slots = request->virtual_slots;
	nbdriver_touch_virtual.had_btn_touch = test_bit(BTN_TOUCH, dev->keybit);
	nbdriver_touch_virtual.had_btn_finger = test_bit(BTN_TOOL_FINGER, dev->keybit);
	nbdriver_touch_virtual.had_btn_doubletap = test_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
	nbdriver_touch_virtual.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit);
	nbdriver_touch_virtual.has_touch_minor = test_bit(ABS_MT_TOUCH_MINOR, dev->absbit);
	nbdriver_touch_virtual.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, dev->absbit);
	nbdriver_touch_virtual.has_tool_type = test_bit(ABS_MT_TOOL_TYPE, dev->absbit);
	nbdriver_touch_virtual.has_pressure = test_bit(ABS_MT_PRESSURE, dev->absbit);
	nbdriver_touch_virtual.owner = owner;
	nbdriver_touch_virtual.x_min = nbdriver_touch_x_min;
	nbdriver_touch_virtual.x_max = nbdriver_touch_x_max;
	nbdriver_touch_virtual.y_min = nbdriver_touch_y_min;
	nbdriver_touch_virtual.y_max = nbdriver_touch_y_max;
	nbdriver_touch_virtual.screen_width = request->screen_width;
	nbdriver_touch_virtual.screen_height = request->screen_height;
	nbdriver_touch_virtual.last_event_jiffies = jiffies;
	nbdriver_touch_virtual.next_tracking_id = 40000;
	/* Transfer the acquired device reference to virtual.dev until destroy. */
	WRITE_ONCE(mt->num_slots, nbdriver_touch_virtual.physical_slots);
	/* Keep vendor axes and MT frame flags intact. In particular,
	 * DROP_UNUSED must still clean real contacts; virtual slots are outside
	 * the physical prefix so physical sync_frame cannot drop them. */
	for (i = 0; i < NBDRIVER_TOUCH_VIRTUAL_MAX; ++i)
		nbdriver_touch_virtual.tracking_ids[i] = -1;
	WRITE_ONCE(nbdriver_touch_virtual.initialized, true);
	spin_unlock_irqrestore(&dev->event_lock, flags);
	nbd_info("stage=touch_virtual operation=init status=ready owner_pid=%d slots=%u physical_slots=%u props=major:%u,minor:%u,width:%u,tool:%u,pressure:%u range=%d..%d,%d..%d\n",
		 current->tgid, request->virtual_slots,
		 nbdriver_touch_virtual.physical_slots,
		 nbdriver_touch_virtual.has_touch_major,
		 nbdriver_touch_virtual.has_touch_minor,
		 nbdriver_touch_virtual.has_width_major,
		 nbdriver_touch_virtual.has_tool_type,
		 nbdriver_touch_virtual.has_pressure,
		 nbdriver_touch_virtual.x_min, nbdriver_touch_virtual.x_max,
		 nbdriver_touch_virtual.y_min, nbdriver_touch_virtual.y_max);
	request->x_min = nbdriver_touch_virtual.x_min;
	request->x_max = nbdriver_touch_virtual.x_max;
	request->y_min = nbdriver_touch_virtual.y_min;
	request->y_max = nbdriver_touch_virtual.y_max;
	request->active = 1;
	return 0;
put_device:
	put_device(&dev->dev);
	return ret;
}

static void nbdriver_touch_virtual_destroy_locked(void)
{
	struct input_dev *dev = nbdriver_touch_virtual.dev;
	unsigned int i;
	unsigned long flags;

	if (!nbdriver_touch_virtual.initialized)
		return;
	if (dev) {
		for (i = 0; i < nbdriver_touch_virtual.virtual_slots; ++i) {
			/* Clear every reserved slot, even when active[] is stale.  A
			 * failed UP or a userspace crash can leave the input core tracking
			 * id alive after the bookkeeping bit was cleared. */
			(void)nbdriver_touch_virtual_report_locked(i, 0, 0, false);
			nbdriver_touch_virtual.active[i] = false;
		}
		spin_lock_irqsave(&dev->event_lock, flags);
		if (dev->mt)
			WRITE_ONCE(dev->mt->num_slots, nbdriver_touch_virtual.original_slots);
		WRITE_ONCE(nbdriver_touch_virtual.initialized, false);
		spin_unlock_irqrestore(&dev->event_lock, flags);
		put_device(&dev->dev);
	}
	nbd_info("stage=touch_virtual operation=destroy status=released owner_pid=%d frames=%lu merged_keys=%lu\n",
		 current->tgid, nbdriver_touch_virtual.frames,
		 nbdriver_touch_virtual.merged_keys);
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
		else {
			nbdriver_touch_virtual.last_event_jiffies = jiffies;
			schedule_delayed_work(&nbdriver_touch_watchdog_work,
				msecs_to_jiffies(NBDRIVER_TOUCH_WATCHDOG_MS));
		}
		return ret;
	}
	if (request->operation == NBDRIVER_TOUCH_VIRTUAL_MOVE) {
		if (!nbdriver_touch_virtual.active[slot])
			return -ENOENT;
		ret = nbdriver_touch_virtual_report_locked(slot, x, y, true);
		if (!ret) {
			nbdriver_touch_virtual.last_event_jiffies = jiffies;
			schedule_delayed_work(&nbdriver_touch_watchdog_work,
				msecs_to_jiffies(NBDRIVER_TOUCH_WATCHDOG_MS));
		}
		return ret;
	}
	if (request->operation == NBDRIVER_TOUCH_VIRTUAL_UP) {
		if (!nbdriver_touch_virtual.active[slot])
			return -ENOENT;
		ret = nbdriver_touch_virtual_report_locked(slot, 0, 0, false);
		if (!ret)
			nbdriver_touch_virtual.active[slot] = false;
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

static long nbdriver_touch_capture_ioctl(struct file *file, unsigned long argument)
{
	struct nbdriver_touch_capture_request request;
	struct nbdriver_route_contact contacts[NBDRIVER_TOUCH_MAX_CONTACTS];
	struct input_dev *dev = NULL;
	unsigned long event_flags, flags;
	unsigned int i, slots, keys = 0;
	int ret = 0;
	bool acquire = false;
	static const unsigned int legacy_axes[] = {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TOOL_WIDTH};
	if (!argument || copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (request.version != NBDRIVER_TOUCH_CAPTURE_ABI_VERSION || request.size != sizeof(request))
		return -EPROTO;
	if (request.enabled > 1 || request.count > NBDRIVER_TOUCH_CAPTURE_RECTS)
		return -EINVAL;
	request.active = 0;
	for (i = 0; i < request.count; ++i) {
		const struct nbdriver_touch_capture_rect *r = &request.rects[i];
		/* Bound arithmetic before subtracting or adding signed coordinates. */
		if (r->left < -1000000 || r->top < -1000000 || r->right > 1000000 || r->bottom > 1000000 ||
		    r->right <= r->left || r->bottom <= r->top ||
		    r->radius_x < 0 || r->radius_y < 0 ||
		    r->radius_x > (r->right - r->left) / 2 ||
		    r->radius_y > (r->bottom - r->top) / 2)
			return -EINVAL;
	}
	mutex_lock(&nbdriver_touch_virtual_lock);
	if (!nbdriver_capture_registered) {
		ret = -EOPNOTSUPP;
		goto done;
	}
	dev = nbdriver_capture_device;
	if (!dev && request.enabled) {
		dev = nbdriver_touch_find_device();
		acquire = dev != NULL;
	}
	if (!dev) {
		ret = request.enabled ? -ENODEV : 0;
		goto done;
	}
	spin_lock_irqsave(&dev->event_lock, event_flags);
	spin_lock_irqsave(&nbdriver_touch_lock, flags);
	/* Refuse devices whose complete frame cannot fit the fixed IRQ buffer. */
	if (!dev->mt || dev->mt->num_slots < 1 || dev->max_vals > NBDRIVER_ROUTE_INPUT_MAX ||
	    dev->mt->num_slots > NBDRIVER_TOUCH_MAX_CONTACTS) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if (nbdriver_capture_owner && nbdriver_capture_owner != file &&
	    time_before(jiffies, nbdriver_capture_deadline)) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!request.enabled) {
		if (nbdriver_capture_owner == file) {
			nbdriver_capture_owner = NULL;
			nbdriver_capture_route.rect_count = 0;
		}
		goto unlock;
	}
	if (nbdriver_touch_device != dev && !nbdriver_touch_bind_locked(dev)) {
		ret = -ENODEV;
		goto unlock;
	}
	slots = nbdriver_capture_contacts(dev, contacts);
	if (!slots) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if (!nbdriver_capture_route.routing) {
		for (i = 0; i < ARRAY_SIZE(nbdriver_route_keys); ++i)
			if (test_bit(nbdriver_route_keys[i], dev->key))
				keys |= 1U << i;
		nbdriver_route_seed(&nbdriver_capture_route, contacts, slots,
			dev->absinfo[ABS_MT_SLOT].value, keys);
		for (i = 0; i < ARRAY_SIZE(legacy_axes); ++i)
			nbdriver_capture_route.sent_legacy[i] = dev->absinfo[legacy_axes[i]].value;
	}
	memcpy(nbdriver_capture_route.rects, request.rects,
		request.count * sizeof(request.rects[0]));
	nbdriver_capture_route.rect_count = request.count;
	if (nbdriver_capture_owner != file || !time_before(jiffies, nbdriver_capture_deadline))
		++nbdriver_capture_route.epoch;
	nbdriver_capture_deadline = jiffies + msecs_to_jiffies(2000);
	nbdriver_capture_owner = file;
	if (!nbdriver_capture_device) {
		nbdriver_capture_device = dev;
		acquire = false; /* Keep the enumeration reference until module exit. */
	}
unlock:
	request.active = !ret && nbdriver_capture_owner == file;
	spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
	spin_unlock_irqrestore(&dev->event_lock, event_flags);
done:
	if (acquire)
		put_device(&dev->dev);
	mutex_unlock(&nbdriver_touch_virtual_lock);
	request.error = ret;
	if (copy_to_user((void __user *)argument, &request, sizeof(request)))
		return -EFAULT;
	return ret;
}

long nbdriver_touch_ioctl(struct file *file, unsigned int command,
	unsigned long argument)
{
	struct nbdriver_touch_snapshot snapshot;
	unsigned long irq_flags;

	if (command == NBDRIVER_IOC_TOUCH_CAPTURE)
		return nbdriver_touch_capture_ioctl(file, argument);
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
	unsigned long flags;
	spin_lock_irqsave(&nbdriver_touch_lock, flags);
	if (nbdriver_capture_owner == file) {
		nbdriver_capture_owner = NULL;
		nbdriver_capture_route.rect_count = 0;
	}
	spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
	mutex_lock(&nbdriver_touch_virtual_lock);
	if (nbdriver_touch_virtual.initialized &&
	    nbdriver_touch_virtual.owner == file)
		nbdriver_touch_virtual_destroy_locked();
	mutex_unlock(&nbdriver_touch_virtual_lock);
}

u32 nbdriver_touch_features(void)
{
	return READ_ONCE(nbdriver_touch_registered) ?
		(NBDRIVER_FEATURE_TOUCH | (nbdriver_capture_registered ? NBDRIVER_FEATURE_TOUCH_CAPTURE : 0) |
		 (nbdriver_touch_handle_event ?
		 NBDRIVER_FEATURE_TOUCH_VIRTUAL : 0)) : 0;
}

int nbdriver_touch_init(void)
{
	int ret_handle;
	int ret_event;
	struct input_dev *dev;
	unsigned long flags;

	INIT_DELAYED_WORK(&nbdriver_touch_watchdog_work,
		nbdriver_touch_virtual_watchdog);

	nbdriver_touch_probe = (struct kprobe) {
		.symbol_name = "input_handle_event",
		.pre_handler = nbdriver_touch_pre,
	};
	ret_handle = register_kprobe(&nbdriver_touch_probe);
	if (!ret_handle) {
		nbdriver_touch_backend = "input_handle_event";
		/* kprobe.addr may be adjusted past a landing instruction. Resolve
		 * the canonical callable entry, not the probe's instruction site. */
		nbdriver_touch_handle_event =
			(nbdriver_input_handle_event_fn)
			nbdriver_ksym_lookup_optional("input_handle_event");
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
	nbdriver_capture_probe = (struct kprobe) {
		.symbol_name = "input_pass_values",
		.pre_handler = nbdriver_capture_pre,
	};
	ret_event = register_kprobe(&nbdriver_capture_probe);
    if (!ret_event) {
        nbdriver_capture_pass_values = (nbdriver_pass_values_fn)
            nbdriver_ksym_lookup_optional("input_pass_values");
        if (!nbdriver_capture_pass_values || !nbdriver_touch_handle_event) {
            unregister_kprobe(&nbdriver_capture_probe);
            nbdriver_capture_pass_values = NULL;
            ret_event = -EOPNOTSUPP;
        }
    }
	nbdriver_capture_registered = ret_event == 0;
	if (ret_event)
		nbd_warn("stage=touch_capture status=unsupported error=%d\n", ret_event);
	else
		nbd_info("stage=touch_capture status=ready backend=input_pass_values\n");
	/* Publish actual panel bounds before the first user tap. Otherwise the UI
	 * would arm its first rectangles using guessed display-sized raw ranges. */
	if (nbdriver_capture_registered) {
		dev = nbdriver_touch_find_device();
		if (dev) {
			spin_lock_irqsave(&nbdriver_touch_lock, flags);
			if (dev->mt && nbdriver_touch_bind_locked(dev)) {
				nbdriver_capture_device = dev;
				dev = NULL; /* Retain the enumeration reference. */
			}
			spin_unlock_irqrestore(&nbdriver_touch_lock, flags);
			if (dev)
				put_device(&dev->dev);
		}
	}
	nbd_info("stage=touch_init status=ready backend=%s snapshot=kernel injection=%s\n",
		 nbdriver_touch_backend,
		 nbdriver_touch_handle_event ? "atomic_mt_frame" : "disabled");
	return 0;
}

void nbdriver_touch_exit(void)
{
	cancel_delayed_work_sync(&nbdriver_touch_watchdog_work);
	mutex_lock(&nbdriver_touch_virtual_lock);
	nbdriver_touch_virtual_destroy_locked();
	mutex_unlock(&nbdriver_touch_virtual_lock);
	WRITE_ONCE(nbdriver_touch_registered, false);
	if (nbdriver_touch_probe_registered) {
		unregister_kprobe(&nbdriver_touch_probe);
		nbdriver_touch_probe_registered = false;
	}
	if (nbdriver_capture_registered) {
		unregister_kprobe(&nbdriver_capture_probe);
		nbdriver_capture_registered = false;
	}
	if (nbdriver_capture_device) {
		put_device(&nbdriver_capture_device->dev);
		nbdriver_capture_device = NULL;
	}
	nbdriver_capture_owner = NULL;
	nbdriver_capture_pass_values = NULL;
	memset(&nbdriver_capture_route, 0, sizeof(nbdriver_capture_route));
	nbdriver_touch_backend = NULL;
	nbdriver_touch_handle_event = NULL;
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
