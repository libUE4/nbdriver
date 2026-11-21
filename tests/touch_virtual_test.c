// SPDX-License-Identifier: GPL-2.0-only
/* Device test for the existing-input-dev virtual MT-slot bridge. */
#include "nbdriver.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void sleep_ms(unsigned int milliseconds)
{
	struct timespec value = {
		.tv_sec = (time_t)(milliseconds / 1000U),
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};
	while (!stop_requested && nanosleep(&value, &value) < 0 && errno == EINTR)
		;
}

static void print_snapshot(int fd, const char *stage)
{
	struct nbdriver_touch_snapshot snapshot;
	if (nbdriver_touch_read(fd, &snapshot) < 0) {
		printf("touch stage=%s snapshot=fail errno=%d\n", stage, errno);
		return;
	}
	printf("touch stage=%s sequence=%llu count=%u flags=0x%x\n", stage,
		(unsigned long long)snapshot.sequence, snapshot.count, snapshot.flags);
}

int main(int argc, char **argv)
{
	unsigned int width = 1080;
	unsigned int height = 1920;
	unsigned int wait_ms = 3000;
	unsigned int hold_ms = 1200;
	unsigned int virtual_slots = 2;
	int fd;
	struct nbdriver_abi_info abi;
	struct nbdriver_touch_virtual_request init;
	int x;
	int y;
	int ret = 0;

	if (argc > 1) width = (unsigned int)strtoul(argv[1], NULL, 10);
	if (argc > 2) height = (unsigned int)strtoul(argv[2], NULL, 10);
	if (argc > 3) wait_ms = (unsigned int)strtoul(argv[3], NULL, 10);
	if (argc > 4) hold_ms = (unsigned int)strtoul(argv[4], NULL, 10);
	if (argc > 5) virtual_slots = (unsigned int)strtoul(argv[5], NULL, 10);
	if (!width || !height || width > 8192U || height > 8192U ||
	    virtual_slots < 2U || virtual_slots > 4U) {
		fprintf(stderr, "touch result=invalid_dimensions\n");
		return 2;
	}
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	fd = nbdriver_open();
	if (fd < 0) {
		printf("touch stage=open result=fail errno=%d\n", errno);
		return 3;
	}
	if (nbdriver_get_abi(fd, &abi) < 0) {
		printf("touch stage=abi result=fail errno=%d\n", errno);
		ret = 4;
		goto out_close;
	}
	printf("touch stage=abi version=%u size=%u features=0x%x\n",
		abi.version, abi.size, abi.features);
	if (!(abi.features & NBDRIVER_FEATURE_TOUCH_VIRTUAL)) {
		printf("touch result=unsupported\n");
		ret = 5;
		goto out_close;
	}
	if (nbdriver_touch_virtual_init(fd, virtual_slots, width, height, &init) < 0) {
		printf("touch stage=init result=fail errno=%d\n", errno);
		ret = 6;
		goto out_close;
	}
	printf("touch stage=init result=ready raw_x=%d..%d raw_y=%d..%d active=%u\n",
		init.x_min, init.x_max, init.y_min, init.y_max, init.active);
	print_snapshot(fd, "before");
	printf("touch stage=wait milliseconds=%u\n", wait_ms);
	sleep_ms(wait_ms);
	if (stop_requested)
		goto out_destroy;
	x = (int)(width / 3U);
	y = (int)(height / 2U);
	if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_DOWN,
		0, x, y, width, height) < 0) {
		printf("touch stage=down result=fail errno=%d\n", errno);
		ret = 7;
		goto out_destroy;
	}
	printf("touch stage=down result=sent x=%d y=%d\n", x, y);
	print_snapshot(fd, "down");
	if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_DOWN,
		1, (int)(width * 2U / 3U), y, width, height) < 0) {
		printf("touch stage=down2 result=fail errno=%d\n", errno);
		ret = 11;
		goto out_destroy;
	}
	printf("touch stage=down2 result=sent x=%u y=%d\n",
		width * 2U / 3U, y);
	print_snapshot(fd, "down2");
	sleep_ms(120);
	{
		const int center_x = x;
		const int center_y = y;
		const int radius = (int)((width < height ? width : height) / 8U);
		const unsigned int steps = 48U;
		for (unsigned int step = 1; step <= steps && !stop_requested; ++step) {
			const double angle = 2.0 * 3.14159265358979323846 *
				(double)step / (double)steps;
			const int point_x = center_x + (int)lround(cos(angle) * radius);
			const int point_y = center_y + (int)lround(sin(angle) * radius);
			if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_MOVE,
				0, point_x, point_y, width, height) < 0) {
				printf("touch stage=circle step=%u result=fail errno=%d\n",
					step, errno);
				ret = 8;
				goto out_destroy;
			}
			if (step == 1U || step == steps / 2U || step == steps)
				printf("touch stage=circle step=%u/%u x=%d y=%d\n",
					step, steps, point_x, point_y);
			if (step == 1U || step == steps / 2U || step == steps)
				print_snapshot(fd, "circle");
			sleep_ms(16);
		}
	}
	if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_MOVE,
		1, (int)(width * 2U / 3U), y + (int)(height / 16U), width, height) < 0) {
		printf("touch stage=move2 result=fail errno=%d\n", errno);
		ret = 12;
		goto out_destroy;
	}
	printf("touch stage=move2 result=sent\n");
	print_snapshot(fd, "move2");
	sleep_ms(hold_ms);
	if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_UP,
		1, 0, 0, width, height) < 0) {
		printf("touch stage=up2 result=fail errno=%d\n", errno);
		ret = 13;
		goto out_destroy;
	}
	printf("touch stage=up2 result=sent\n");
	print_snapshot(fd, "up2");
	if (nbdriver_touch_virtual_event(fd, NBDRIVER_TOUCH_VIRTUAL_UP,
		0, 0, 0, width, height) < 0) {
		printf("touch stage=up result=fail errno=%d\n", errno);
		ret = 9;
		goto out_destroy;
	}
	printf("touch stage=up result=sent\n");
	print_snapshot(fd, "up");

out_destroy:
	if (nbdriver_touch_virtual_destroy(fd) < 0 && !ret) {
		printf("touch stage=destroy result=fail errno=%d\n", errno);
		ret = 10;
	} else {
		printf("touch stage=destroy result=restored\n");
	}
out_close:
	nbdriver_close(fd);
	printf("touch result=%s code=%d\n", ret ? "fail" : "passed", ret);
	return ret;
}
