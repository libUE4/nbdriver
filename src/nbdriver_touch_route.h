/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_TOUCH_ROUTE_H
#define NBDRIVER_TOUCH_ROUTE_H

/* Frame policy shared by the kernel bridge and the host regression. No locks,
 * allocation, input-core calls or user pointers are allowed in this layer. */
#define NBDRIVER_ROUTE_AXES 14U
#define NBDRIVER_ROUTE_VALUES 1024U
#define NBDRIVER_ROUTE_INPUT_MAX 768U
#define NBDRIVER_ROUTE_SYSTEM 1U
#define NBDRIVER_ROUTE_UI 2U

static const unsigned short nbdriver_route_axes[NBDRIVER_ROUTE_AXES] = {
	ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
	ABS_MT_PRESSURE, ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR,
	ABS_MT_WIDTH_MAJOR, ABS_MT_TOOL_TYPE, ABS_MT_WIDTH_MINOR,
	ABS_MT_ORIENTATION, ABS_MT_BLOB_ID, ABS_MT_DISTANCE,
	ABS_MT_TOOL_X, ABS_MT_TOOL_Y,
};
static const unsigned short nbdriver_route_keys[] = {
	BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP,
	BTN_TOOL_TRIPLETAP, BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
};

struct nbdriver_route_contact {
	int value[NBDRIVER_ROUTE_AXES];
};

struct nbdriver_touch_route {
	struct nbdriver_touch_capture_rect rects[NBDRIVER_TOUCH_CAPTURE_RECTS];
	unsigned int rect_count;
	unsigned int owner[NBDRIVER_TOUCH_MAX_CONTACTS];
	unsigned int contact_epoch[NBDRIVER_TOUCH_MAX_CONTACTS];
	unsigned int epoch;
	int identity[NBDRIVER_TOUCH_MAX_CONTACTS];
	struct nbdriver_route_contact sent[NBDRIVER_TOUCH_MAX_CONTACTS];
	int slot;
	unsigned int sent_keys;
	int sent_legacy[4];
	bool routing;
	struct input_value output[NBDRIVER_ROUTE_VALUES];
};

static bool nbdriver_route_inside(const struct nbdriver_touch_route *route,
	int x, int y)
{
	unsigned int i;
	for (i = 0; i < route->rect_count; ++i) {
		const struct nbdriver_touch_capture_rect *r = &route->rects[i];
		long long dx = 0, dy = 0;
		if (x < r->left || x >= r->right || y < r->top || y >= r->bottom)
			continue;
		if (!r->radius_x || !r->radius_y)
			return true;
		if (x < r->left + r->radius_x)
			dx = (long long)r->left + r->radius_x - x;
		else if (x > r->right - r->radius_x)
			dx = (long long)x - r->right + r->radius_x;
		if (y < r->top + r->radius_y)
			dy = (long long)r->top + r->radius_y - y;
		else if (y > r->bottom - r->radius_y)
			dy = (long long)y - r->bottom + r->radius_y;
		dx = dx * 1024 / r->radius_x;
		dy = dy * 1024 / r->radius_y;
		if (dx * dx + dy * dy <= 1024LL * 1024)
			return true;
	}
	return false;
}

static void nbdriver_route_seed(struct nbdriver_touch_route *route,
	const struct nbdriver_route_contact *contacts, unsigned int slots,
	int slot, unsigned int keys)
{
	unsigned int i;
	for (i = 0; i < slots; ++i) {
		route->identity[i] = contacts[i].value[0];
		route->owner[i] = NBDRIVER_ROUTE_SYSTEM;
		route->sent[i] = contacts[i];
	}
	route->slot = slot;
	route->sent_keys = keys;
	route->routing = true;
}

static void nbdriver_route_emit(struct nbdriver_touch_route *route,
	unsigned int *count, unsigned int type, unsigned int code, int value)
{
	route->output[(*count)++] = (struct input_value) {
		.type = type, .code = code, .value = value,
	};
}

static bool nbdriver_route_touch_value(const struct input_value *v)
{
	unsigned int j;
	if (v->type == EV_KEY)
		for (j = 0; j < sizeof(nbdriver_route_keys) / sizeof(nbdriver_route_keys[0]); ++j)
			if (v->code == nbdriver_route_keys[j])
				return true;
	return v->type == EV_ABS && (v->code == ABS_MT_SLOT ||
		(v->code >= ABS_MT_FIRST && v->code <= ABS_MT_LAST) ||
		v->code == ABS_X || v->code == ABS_Y ||
		v->code == ABS_PRESSURE || v->code == ABS_TOOL_WIDTH);
}

/* Original input-core chunks (including its synthetic buffer-full flush)
 * must not publish a partial touch. Non-touch events retain their order. */
static unsigned int nbdriver_route_original(struct nbdriver_touch_route *route,
	const struct input_value *input, unsigned int count)
{
	unsigned int i, n = 0;
	for (i = 0; i < count; ++i) {
		const struct input_value *v = &input[i];
		if (!nbdriver_route_touch_value(v) && !(v->type == EV_SYN && v->code == SYN_REPORT))
			route->output[n++] = *v;
	}
	if (n)
		nbdriver_route_emit(route, &n, EV_SYN, SYN_REPORT, 0);
	return n;
}

/* input_pass_values runs under the real device's event_lock. Its MT table is
	 * the completed frame, so TRACKING_ID arriving before X/Y cannot leak a DOWN.
 * Rebuild only the touch portion; leave hardware and virtual MT state intact.
 * Every emitted MT attribute gets an explicit slot when needed, independent
 * of input core's separate (unfiltered) slot cache. */
static unsigned int nbdriver_route_frame(struct nbdriver_touch_route *route,
	const struct input_value *input, unsigned int count,
	const struct nbdriver_route_contact *contacts, unsigned int slots,
	unsigned int physical_slots, unsigned int axes, unsigned int keys,
	unsigned int legacy,
	bool capture)
{
	unsigned int i, j, n = 0, live = 0;
	int primary = -1;
	/* Caller proves count <= INPUT_MAX and slots <= MAX_CONTACTS. Each slot
	 * adds at most SLOT + UP + fourteen axes; keys + legacy axes + SYN fit. */
	for (i = 0; i < count; ++i) {
		const struct input_value *v = &input[i];
		bool touch_key = false;
		for (j = 0; j < sizeof(nbdriver_route_keys) / sizeof(nbdriver_route_keys[0]); ++j)
			if (v->type == EV_KEY && v->code == nbdriver_route_keys[j])
				touch_key = true;
		if (touch_key || (v->type == EV_SYN && v->code == SYN_REPORT))
			continue;
		if (v->type == EV_ABS && (v->code == ABS_MT_SLOT ||
		    (v->code >= ABS_MT_FIRST && v->code <= ABS_MT_LAST) ||
		    v->code == ABS_X || v->code == ABS_Y ||
		    v->code == ABS_PRESSURE || v->code == ABS_TOOL_WIDTH))
			continue;
		route->output[n++] = *v;
	}
	for (i = 0; i < slots; ++i) {
		const struct nbdriver_route_contact *c = &contacts[i];
		int id = c->value[0];
		if (route->identity[i] != id) {
			route->identity[i] = id;
			route->contact_epoch[i] = route->epoch;
			route->owner[i] = id >= 0 && i < physical_slots && capture &&
				nbdriver_route_inside(route, c->value[1], c->value[2]) ?
				NBDRIVER_ROUTE_UI : NBDRIVER_ROUTE_SYSTEM;
		}
		if (id >= 0 && (i >= physical_slots || route->owner[i] == NBDRIVER_ROUTE_SYSTEM)) {
			++live;
			if (primary < 0)
				primary = (int)i;
		}
	}
	/* BTN_TOUCH precedes contact axes, as required by legacy input handlers. */
	for (j = 0; j < sizeof(nbdriver_route_keys) / sizeof(nbdriver_route_keys[0]); ++j) {
		bool down = j == 0 ? live > 0 : j == 5 ? live >= 5 : live == j;
		if ((keys & (1U << j)) && !!(route->sent_keys & (1U << j)) != down) {
			nbdriver_route_emit(route, &n, EV_KEY, nbdriver_route_keys[j], down);
			if (down)
				route->sent_keys |= 1U << j;
			else
				route->sent_keys &= ~(1U << j);
		}
	}
	for (i = 0; i < slots; ++i) {
		const struct nbdriver_route_contact *c = &contacts[i];
		struct nbdriver_route_contact *old = &route->sent[i];
		int id = c->value[0];
		bool visible = id >= 0 && (i >= physical_slots ||
			route->owner[i] == NBDRIVER_ROUTE_SYSTEM);
		if (old->value[0] >= 0 && (!visible || old->value[0] != id)) {
			if (route->slot != (int)i) {
				nbdriver_route_emit(route, &n, EV_ABS, ABS_MT_SLOT, i);
				route->slot = (int)i;
			}
			nbdriver_route_emit(route, &n, EV_ABS, ABS_MT_TRACKING_ID, -1);
			old->value[0] = -1;
		}
		if (!visible)
			continue;
		for (j = 0; j < NBDRIVER_ROUTE_AXES; ++j) {
			if (!(axes & (1U << j)) ||
			    (old->value[0] == id && old->value[j] == c->value[j]))
				continue;
			if (route->slot != (int)i) {
				nbdriver_route_emit(route, &n, EV_ABS, ABS_MT_SLOT, i);
				route->slot = (int)i;
			}
			nbdriver_route_emit(route, &n, EV_ABS, nbdriver_route_axes[j], c->value[j]);
		}
		*old = *c;
	}
	/* Preserve the legacy pointer emulation for the first visible finger. */
	if (primary >= 0) {
		static const unsigned short codes[] = {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TOOL_WIDTH};
		static const unsigned int attributes[] = {1, 2, 3, 6};
		for (j = 0; j < 4; ++j) {
			int value = contacts[primary].value[attributes[j]];
			if ((legacy & (1U << j)) && value != route->sent_legacy[j]) {
				nbdriver_route_emit(route, &n, EV_ABS, codes[j], value);
				route->sent_legacy[j] = value;
			}
		}
	}
	if (n)
		nbdriver_route_emit(route, &n, EV_SYN, SYN_REPORT, 0);
	return n;
}
#endif
