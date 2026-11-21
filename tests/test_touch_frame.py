#!/usr/bin/env python3
"""Compile the actual driver report/probe bodies against a small input-core model.

This is an interleaving/BTN_TOUCH regression test, NOT an Android device test.
Pass --source to evaluate a preserved preimage with the identical scenarios.
"""
import argparse
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent


def function(text, name):
    import re
    match = re.search(r'^static [^;{}]*\b' + re.escape(name) + r'\s*\([^;{}]*\)\s*\{', text, re.M)
    if not match:
        raise ValueError('missing actual source function: ' + name)
    end = match.end()
    depth = 1
    # These selected bodies contain no braces in strings/comments.
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/input-event-codes.h>
#define ABS_MT_FIRST ABS_MT_TOUCH_MAJOR
#define ABS_MT_LAST ABS_MT_TOOL_Y
#define NBDRIVER_TOUCH_MAX_CONTACTS 16
#define NBDRIVER_TOUCH_F_VALID 1
#define NBDRIVER_TOUCH_F_MT 2
#define NBDRIVER_TOUCH_F_SYN_DROPPED 4
#define NBDRIVER_NO_CFI
#define DEFINE_SPINLOCK(n) int n
#define DEFINE_MUTEX(n) int n
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define MT_TOOL_FINGER 0
#define spin_lock_irqsave(p,f) do { (f)=0; assert(*(p)==0); ++*(p); } while(0)
static void unlock(int *p);
#define spin_unlock_irqrestore(p,f) do { (void)(f); unlock(p); } while(0)
typedef int32_t nbdriver_s32;
typedef uint32_t nbdriver_u32;
typedef uint64_t nbdriver_u64;
struct class;
struct device;
int class_for_each_device(const struct class *, const struct device *, void *,
                          int (*)(struct device *, void *));
struct file;
struct kprobe { int unused; };
/* The production file owns a delayed-work watchdog, while this host model
 * extracts only the input-frame bodies.  Keep the real declarations visible
 * without pulling in the kernel workqueue implementation. */
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
#define INIT_DELAYED_WORK(w,f) ((void)(w), (void)(f))
#define schedule_delayed_work(w,d) (0)
#define cancel_delayed_work_sync(w) ((void)(w))
struct pt_regs { unsigned long regs[4]; };
struct input_absinfo { int value, minimum, maximum, fuzz, flat, resolution; };
struct input_mt_slot { int abs[ABS_MT_LAST - ABS_MT_FIRST + 1]; };
struct input_mt { int slot, num_slots; unsigned int flags; struct input_mt_slot slots[16]; };
struct input_dev {
    int event_lock;
    unsigned long evbit[1], keybit[16], absbit[1];
    struct input_absinfo *absinfo;
    struct input_mt *mt;
    int touch_key;
};
static int test_bit(unsigned int bit, const unsigned long *p) {
    return !!(p[bit/64] & (1UL << (bit%64)));
}
static int input_mt_get_value(const struct input_mt_slot *s, int code) {
    return s->abs[code - ABS_MT_FIRST];
}
static nbdriver_u64 ktime_get_ns(void) { static nbdriver_u64 n; return ++n; }
static void nbdriver_capture_complete(struct input_dev *d) { (void)d; }
static void input_event(struct input_dev *, unsigned int, unsigned int, int);
static void fake_handle_event(struct input_dev *, unsigned int, unsigned int, int);
'''

SUFFIX = r'''
static struct input_dev device;
static struct input_mt slots;
static struct input_absinfo axes[ABS_MAX + 1];
static int inject_interleaving, pending_interleaving, illegal_dead_attributes;
static int failures;
static void unlock(int *p) {
    assert(*p == 1); --*p;
    if (p == &device.event_lock && pending_interleaving) {
        pending_interleaving = 0;
        input_event(&device, EV_ABS, ABS_MT_SLOT, 0);
        input_event(&device, EV_ABS, ABS_MT_POSITION_X, 111);
        input_event(&device, EV_ABS, ABS_MT_POSITION_Y, 222);
        input_event(&device, EV_SYN, SYN_REPORT, 0);
    }
}
static void fake_handle_event(struct input_dev *d, unsigned int type,
                              unsigned int code, int value) {
    assert(d->event_lock == 1);
    struct pt_regs regs = { .regs = {(unsigned long)d, type, code, (unsigned long)value} };
    nbdriver_touch_pre(NULL, &regs);
    value = (int)regs.regs[3];
    if (type == EV_ABS) {
        if (code == ABS_MT_SLOT) {
            if (value >= 0 && value < d->mt->num_slots) d->mt->slot = value;
        } else if (code >= ABS_MT_FIRST && code <= ABS_MT_LAST) {
            struct input_mt_slot *s = &d->mt->slots[d->mt->slot];
            if (d->mt->slot == 3 && code != ABS_MT_TRACKING_ID &&
                input_mt_get_value(s, ABS_MT_TRACKING_ID) == -1)
                ++illegal_dead_attributes;
            s->abs[code - ABS_MT_FIRST] = value;
        }
    } else if (type == EV_KEY && code == BTN_TOUCH) d->touch_key = !!value;
    if (inject_interleaving && type == EV_ABS && code == ABS_MT_TRACKING_ID && value == 40000)
        pending_interleaving = 1;
}
static void input_event(struct input_dev *d, unsigned int type, unsigned int code, int value) {
    unsigned long flags;
    spin_lock_irqsave(&d->event_lock, flags);
    fake_handle_event(d, type, code, value);
    spin_unlock_irqrestore(&d->event_lock, flags);
}
static void reset(void) {
    memset(&device, 0, sizeof(device)); memset(&slots, 0, sizeof(slots));
    memset(axes, 0, sizeof(axes)); memset(&nbdriver_touch_virtual, 0, sizeof(nbdriver_touch_virtual));
    memset(nbdriver_touch_slots, 0, sizeof(nbdriver_touch_slots));
    nbdriver_touch_device = &device; nbdriver_touch_multi = true;
    nbdriver_touch_slot = 0; nbdriver_touch_lock = 0;
    device.mt = &slots; device.absinfo = axes; slots.num_slots = 3;
    for (int i=0; i<16; ++i) slots.slots[i].abs[ABS_MT_TRACKING_ID-ABS_MT_FIRST] = -1;
    slots.slots[0].abs[ABS_MT_TRACKING_ID-ABS_MT_FIRST] = 1;
    nbdriver_touch_virtual.dev = &device;
    nbdriver_touch_virtual.original_slots = 4;
    nbdriver_touch_virtual.physical_slots = 3;
    nbdriver_touch_virtual.virtual_slots = 1;
    nbdriver_touch_virtual.initialized = true;
    nbdriver_touch_virtual.had_btn_touch = true;
    nbdriver_touch_virtual.had_btn_finger = true;
    nbdriver_touch_virtual.had_btn_doubletap = true;
    nbdriver_touch_virtual.has_touch_major = true;
    nbdriver_touch_virtual.has_pressure = true;
    nbdriver_touch_virtual.active[0] = true;
    nbdriver_touch_virtual.tracking_ids[0] = 40000;
    axes[ABS_MT_TOUCH_MAJOR].maximum = 255;
    nbdriver_touch_handle_event = fake_handle_event;
    inject_interleaving = pending_interleaving = illegal_dead_attributes = 0;
}
static void check(const char *name, bool okay) {
    printf("%s %s\n", okay ? "PASS" : "FAIL", name); failures += !okay;
}
int main(void) {
    reset();
    check("zero_range_pressure_stays_nonzero_without_axis_mutation",
          nbdriver_touch_virtual_abs_value(&device, ABS_MT_PRESSURE, 60) == 60 &&
          axes[ABS_MT_PRESSURE].maximum == 0);
    reset();
    nbdriver_touch_virtual_report_locked(0, 999, 1000, true);
    check("virtual_output_is_not_echoed_into_overlay_input", !nbdriver_touch_slots[3].down);
    input_event(&device, EV_ABS, ABS_MT_SLOT, 0);
    input_event(&device, EV_ABS, ABS_MT_TRACKING_ID, -1);
    input_event(&device, EV_KEY, BTN_TOUCH, 0);
    input_event(&device, EV_SYN, SYN_REPORT, 0);
    check("physical_release_keeps_virtual_contact_touching_not_hovering", device.touch_key == 1);
    reset(); inject_interleaving = 1;
    nbdriver_touch_virtual_report_locked(0, 999, 1000, true);
    check("interleaved_physical_frame_cannot_corrupt_virtual_coordinates",
          input_mt_get_value(&slots.slots[3], ABS_MT_POSITION_X)==999 &&
          input_mt_get_value(&slots.slots[3], ABS_MT_POSITION_Y)==1000 &&
          input_mt_get_value(&slots.slots[0], ABS_MT_POSITION_X)==111 &&
          input_mt_get_value(&slots.slots[0], ABS_MT_POSITION_Y)==222);
    reset();
    nbdriver_touch_virtual_report_locked(0, 999, 1000, true);
    nbdriver_touch_virtual.active[0] = false;
    nbdriver_touch_virtual_report_locked(0, 0, 0, false);
    check("no_attributes_after_tracking_id_death", illegal_dead_attributes == 0);
    check("virtual_release_preserves_real_finger", device.touch_key == 1 &&
          input_mt_get_value(&slots.slots[0], ABS_MT_TRACKING_ID)==1);
    input_event(&device, EV_ABS, ABS_MT_SLOT, 0);
    input_event(&device, EV_ABS, ABS_MT_TRACKING_ID, -1);
    input_event(&device, EV_KEY, BTN_TOUCH, 0);
    check("last_finger_releases_global_key", device.touch_key == 0);
    check("frame_restores_physical_slot_range_and_unlocks", slots.num_slots == 3 &&
          device.event_lock == 0);
    return failures ? 1 : 0;
}
'''


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source', type=pathlib.Path, default=HERE.parent/'src/nbdriver_touch.c')
    args=p.parse_args()
    text=args.source.read_text()
    # Include exact production declarations and probe, not a reimplemented policy.
    declarations=text[text.index('typedef '):text.index('static bool nbdriver_touch_candidate')]
    if 'typedef void (*nbdriver_input_handle_event_fn)' not in declarations:
        declarations+='\ntypedef void (*nbdriver_input_handle_event_fn)(struct input_dev *, unsigned int, unsigned int, int);\nstatic nbdriver_input_handle_event_fn nbdriver_touch_handle_event;\n'
    declarations='\n#define NBDRIVER_TOUCH_SLOTS NBDRIVER_TOUCH_MAX_CONTACTS\n#define NBDRIVER_TOUCH_VIRTUAL_MAX 4U\n'+declarations
    bodies=[]
    for name in ['nbdriver_touch_candidate','nbdriver_touch_abs','nbdriver_touch_reset_slots_locked',
                 'nbdriver_touch_bind_locked','nbdriver_touch_physical_active_count',
                 'nbdriver_touch_pre','nbdriver_touch_virtual_abs_value','nbdriver_touch_virtual_report_locked']:
        if name=='nbdriver_touch_pre' and 'nbdriver_touch_virtual_active_count_locked' in text:
            bodies.append(function(text,'nbdriver_touch_virtual_active_count_locked'))
        bodies.append(function(text,name))
    with tempfile.TemporaryDirectory(prefix='touch-frame-host-') as tmp:
        source=pathlib.Path(tmp)/'test.c';exe=pathlib.Path(tmp)/'test'
        source.write_text(PREFIX+declarations+'\n'+'\n'.join(bodies)+SUFFIX)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-Wno-unused-variable','-Wno-unused-function',str(source),'-o',str(exe)],check=True)
        return subprocess.run([str(exe)]).returncode

if __name__=='__main__':
    raise SystemExit(main())
