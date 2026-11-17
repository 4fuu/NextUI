#include "system_grab.h"

#include <stddef.h>
#include <sys/ioctl.h>

/* EVIOCGRAB is _IOW('E', 0x90, int) on the Linux evdev ABI. Defining it here
 * keeps the host test buildable on non-Linux hosts, where this module only
 * runs with an injected ioctl. */
#ifndef EVIOCGRAB
#define EVIOCGRAB 0x40044590UL
#endif

#define SYSTEM_GRAB_MAX_FDS 32

typedef struct
{
	int fd;
	uint32_t bit;
} GrabbedFd;

static GrabbedFd grabbed[SYSTEM_GRAB_MAX_FDS];
static int grabbed_count;
static uint32_t grabbed_mask;

static int real_ioctl(int fd, unsigned long request, unsigned long argument)
{
	return ioctl(fd, request, argument);
}

static SystemGrabIoctl grab_ioctl = real_ioctl;

void SystemGrab_setIoctl(SystemGrabIoctl fn)
{
	SystemGrab_release();
	grab_ioctl = fn ? fn : real_ioctl;
}

int SystemGrab_acquire(const int* fds, uint32_t mask, int count)
{
	int i;

	SystemGrab_release();
	if (!fds || count <= 0) return 0;

	for (i = 0; i < count && grabbed_count < SYSTEM_GRAB_MAX_FDS; i++)
	{
		uint32_t bit = (uint32_t)1 << i;
		if (!(mask & bit) || fds[i] < 0) continue;
		if (grab_ioctl(fds[i], EVIOCGRAB, 1) != 0) continue;
		grabbed[grabbed_count].fd = fds[i];
		grabbed[grabbed_count].bit = bit;
		grabbed_count++;
		grabbed_mask |= bit;
	}
	return grabbed_count;
}

void SystemGrab_release(void)
{
	int i;

	for (i = 0; i < grabbed_count; i++)
		(void)grab_ioctl(grabbed[i].fd, EVIOCGRAB, 0);
	grabbed_count = 0;
	grabbed_mask = 0;
}

int SystemGrab_active(void)
{
	return grabbed_count;
}

uint32_t SystemGrab_mask(void)
{
	return grabbed_mask;
}
