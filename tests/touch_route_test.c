/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "nbdriver_abi.h"
/* Kernel-only input_value layout; policy under test is the production header. */
struct input_value { unsigned short type, code; int value; };
#define ABS_MT_FIRST ABS_MT_TOUCH_MAJOR
#define ABS_MT_LAST ABS_MT_TOOL_Y
#include "nbdriver_touch_route.h"

static struct nbdriver_touch_route route;
static struct nbdriver_route_contact contacts[NBDRIVER_TOUCH_MAX_CONTACTS];
static struct nbdriver_route_contact android[NBDRIVER_TOUCH_MAX_CONTACTS];
static int android_slot;
static unsigned int android_keys;
static unsigned int assertions;

static void check(const char *name, bool condition)
{
	printf("%s %s\n", condition ? "PASS" : "FAIL", name);
	assert(condition);
	++assertions;
}

static void reset(void)
{
	unsigned int i;
	memset(&route, 0, sizeof(route));
	memset(contacts, 0, sizeof(contacts));
	memset(android, 0, sizeof(android));
	android_slot = 0;
	android_keys = 0;
	for (i = 0; i < NBDRIVER_TOUCH_MAX_CONTACTS; ++i) {
		contacts[i].value[0] = -1;
		android[i].value[0] = -1;
	}
	route.rect_count = 1;
	route.rects[0] = (struct nbdriver_touch_capture_rect){100, 100, 300, 300, 0, 0};
	route.epoch = 1;
	nbdriver_route_seed(&route, contacts, NBDRIVER_TOUCH_MAX_CONTACTS, 0, 0);
}

static void finger(unsigned int slot, int id, int x, int y)
{
	contacts[slot].value[0] = id;
	contacts[slot].value[1] = x;
	contacts[slot].value[2] = y;
	contacts[slot].value[3] = 60;
	contacts[slot].value[4] = 10;
}

static unsigned int dispatch_values(const struct input_value *in, unsigned int count, bool capture)
{
	unsigned int i, j;
	unsigned int n = nbdriver_route_frame(&route, in, count, contacts,
		NBDRIVER_TOUCH_MAX_CONTACTS, NBDRIVER_TOUCH_MAX_CONTACTS-1,
		(1U << NBDRIVER_ROUTE_AXES)-1, 63, 15, capture);
	assert(n <= NBDRIVER_ROUTE_VALUES);
	for (i = 0; i < n; ++i) {
		const struct input_value *v = &route.output[i];
		if (v->type == EV_ABS && v->code == ABS_MT_SLOT) {
			assert(v->value >= 0 && v->value < (int)NBDRIVER_TOUCH_MAX_CONTACTS);
			android_slot = v->value;
		} else if (v->type == EV_ABS) {
			for (j = 0; j < NBDRIVER_ROUTE_AXES; ++j)
				if (v->code == nbdriver_route_axes[j]) {
					assert(j == 0 || android[android_slot].value[0] >= 0);
					android[android_slot].value[j] = v->value;
				}
		} else if (v->type == EV_KEY) {
			for (j = 0; j < 6; ++j)
				if (v->code == nbdriver_route_keys[j]) {
					if (v->value) android_keys |= 1U << j;
					else android_keys &= ~(1U << j);
				}
		}
	}
	return n;
}

static unsigned int dispatch(bool capture)
{
	const struct input_value sync = {EV_SYN, SYN_REPORT, 0};
	return dispatch_values(&sync, 1, capture);
}

int main(void)
{
	unsigned int i;
	struct input_value input[NBDRIVER_ROUTE_INPUT_MAX];
	reset();
	{
		const struct input_value partial[] = {
			{EV_ABS, ABS_MT_SLOT, 0}, {EV_ABS, ABS_MT_TRACKING_ID, 10},
			{EV_SYN, SYN_REPORT, 1},
		};
		finger(0, 10, 0, 0);
		check("buffer_full_partial_down_is_never_dispatched",
			nbdriver_route_original(&route, partial, 3) == 0 && route.identity[0] == -1);
		finger(0, 10, 150, 150);
		dispatch(true);
		check("real_syn_routes_using_complete_first_coordinates", route.owner[0] == NBDRIVER_ROUTE_UI && android[0].value[0] == -1);
	}
	reset();
	finger(0, 10, 150, 150);
	dispatch(true);
	check("menu_down_consumed_before_android", android[0].value[0] == -1 && android_keys == 0);
	check("menu_contact_available_to_private_reader", route.owner[0] == NBDRIVER_ROUTE_UI);
	finger(0, 10, 500, 500);
	dispatch(true);
	check("drag_outside_keeps_menu_ownership", android[0].value[0] == -1);
	finger(1, 11, 600, 600);
	dispatch(true);
	check("second_finger_outside_reaches_android_with_touch_key", android[1].value[0] == 11 && (android_keys & 1));
	finger(1, 11, 150, 150);
	dispatch(true);
	check("game_finger_entering_menu_is_not_stolen", android[1].value[1] == 150 && route.owner[1] == NBDRIVER_ROUTE_SYSTEM);
	finger(9, 40000, 160, 160);
	dispatch(true);
	check("aim_virtual_slot_passes_even_inside_menu", android[9].value[0] == 40000 && route.owner[9] == NBDRIVER_ROUTE_SYSTEM);
	finger(1, -1, 150, 150);
	dispatch(true);
	check("physical_release_keeps_virtual_touch_down", android[1].value[0] == -1 && (android_keys & 1));
	finger(9, -1, 160, 160);
	dispatch(true);
	check("last_visible_contact_releases_key_while_menu_finger_held", android_keys == 0);
	dispatch(false);
	check("close_or_expired_lease_does_not_leak_half_gesture", android[0].value[0] == -1);
	finger(0, -1, 500, 500);
	dispatch(false);
	finger(0, 12, 150, 150);
	dispatch(false);
	check("new_finger_after_close_returns_to_android", android[0].value[0] == 12);
	check("unchanged_frame_has_no_replayed_motion", dispatch(false) == 0);

	reset();
	finger(2, 13, 500, 500);
	nbdriver_route_seed(&route, contacts, NBDRIVER_TOUCH_MAX_CONTACTS, 2, 3);
	android[2] = contacts[2]; android_slot = 2; android_keys = 3;
	finger(2, 13, 150, 150); dispatch(true);
	check("arming_during_game_gesture_does_not_steal_contact", android[2].value[0] == 13 && route.owner[2] == NBDRIVER_ROUTE_SYSTEM);

	reset();
	route.rects[0].radius_x = route.rects[0].radius_y = 100;
	check("rounded_corner_remains_game_touch", !nbdriver_route_inside(&route, 101, 101));
	check("rounded_center_is_menu_touch", nbdriver_route_inside(&route, 200, 200));
	finger(0, 14, 200, 200);
	dispatch(true);
	route.rect_count = 0;
	dispatch(true);
	check("moving_or_removing_region_does_not_transfer_existing_owner", android[0].value[0] == -1);
	++route.epoch;
	check("new_ui_owner_cannot_adopt_old_ui_gesture", route.contact_epoch[0] != route.epoch);
	finger(0, -1, 200, 200); dispatch(true);
	finger(0, 15, 200, 200); dispatch(true);
	check("slot_reuse_is_routed_again", android[0].value[0] == 15);

	reset();
	for (i = 0; i < NBDRIVER_ROUTE_INPUT_MAX; ++i)
		input[i] = (struct input_value){EV_MSC, MSC_SCAN, (int)i};
	for (i = 0; i < NBDRIVER_TOUCH_MAX_CONTACTS; ++i)
		finger(i, 100+(int)i, 900, 800);
	check("maximum_frame_and_all_slots_fit_irq_buffer", dispatch_values(input, NBDRIVER_ROUTE_INPUT_MAX, true) < NBDRIVER_ROUTE_VALUES);
	check("unrelated_device_events_are_preserved", route.output[0].type == EV_MSC && route.output[0].value == 0 && route.output[NBDRIVER_ROUTE_INPUT_MAX-1].value == (int)NBDRIVER_ROUTE_INPUT_MAX-1);
	check("all_axes_preserved", android[0].value[3] == 60 && android[0].value[4] == 10);
	printf("PASS %u routing scenarios; synthetic input only\n", assertions);
	return 0;
}
