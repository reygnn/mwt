/* stallsource - a clipboard source that does not behave.
 *
 *   stallsource silent|flood|plain SECONDS
 *
 * Takes the selection through wlr-data-control and exits after SECONDS. silent:
 * a reader gets a pipe that stays open and on which nothing ever comes. flood:
 * the pipe never stops giving text. plain: 16 blocks of 65520 bytes in lines of
 * 80, from a loop that takes a failed write for the end, as a simple program
 * does. wl-copy can do none of these. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *mgr;
static int flood, plain, secs;

static void global(void *d, struct wl_registry *r, uint32_t name,
		   const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { global, global_remove };

static void src_send(void *d, struct zwlr_data_control_source_v1 *s, const char *mime, int fd)
{
	(void)d; (void)s;
	printf("send %s\n", mime);
	fflush(stdout);
	if (!flood && !plain)
		return; /* the fd stays open, and nothing comes */
	if (fork() == 0) {
		static char buf[65520];
		memset(buf, 'x', sizeof buf);
		for (size_t i = 79; i < sizeof buf; i += 80)
			buf[i] = '\n';
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		alarm(secs);
		for (int i = 0; flood || i < 16; i++) {
			size_t off = 0;
			while (off < sizeof buf) {
				ssize_t n = write(fd, buf + off, sizeof buf - off);
				if (n <= 0)
					_exit(1);
				off += n;
			}
		}
		_exit(0);
	}
	close(fd);
}
static void src_cancelled(void *d, struct zwlr_data_control_source_v1 *s) { (void)d; (void)s; _exit(0); }
static const struct zwlr_data_control_source_v1_listener src_l = { src_send, src_cancelled };

static void offer_mime(void *d, struct zwlr_data_control_offer_v1 *o, const char *m) { (void)d; (void)o; (void)m; }
static const struct zwlr_data_control_offer_v1_listener offer_l = { offer_mime };
static void dev_offer(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o)
{
	(void)d; (void)dev;
	zwlr_data_control_offer_v1_add_listener(o, &offer_l, NULL);
}
static void dev_selection(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o) { (void)d; (void)dev; (void)o; }
static void dev_finished(void *d, struct zwlr_data_control_device_v1 *dev) { (void)d; (void)dev; }
static void dev_primary(void *d, struct zwlr_data_control_device_v1 *dev, struct zwlr_data_control_offer_v1 *o) { (void)d; (void)dev; (void)o; }
static const struct zwlr_data_control_device_v1_listener dev_l = { dev_offer, dev_selection, dev_finished, dev_primary };

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: stallsource silent|flood|plain SECONDS\n");
		return 1;
	}
	flood = !strcmp(argv[1], "flood");
	plain = !strcmp(argv[1], "plain");
	secs = atoi(argv[2]);
	signal(SIGCHLD, SIG_IGN);
	alarm(secs);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !mgr) {
		fprintf(stderr, "stallsource: no seat or wlr-data-control\n");
		return 1;
	}
	struct zwlr_data_control_device_v1 *dev =
		zwlr_data_control_manager_v1_get_data_device(mgr, seat);
	zwlr_data_control_device_v1_add_listener(dev, &dev_l, NULL);
	struct zwlr_data_control_source_v1 *src = zwlr_data_control_manager_v1_create_data_source(mgr);
	zwlr_data_control_source_v1_add_listener(src, &src_l, NULL);
	zwlr_data_control_source_v1_offer(src, "text/plain;charset=utf-8");
	zwlr_data_control_device_v1_set_selection(dev, src);
	printf("source ready\n");
	fflush(stdout);
	while (wl_display_dispatch(dpy) >= 0)
		;
	return 0;
}
