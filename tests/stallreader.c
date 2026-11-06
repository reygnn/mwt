/* stallreader - a clipboard reader that asks for the text and never reads it.
 *
 *   stallreader N SECONDS [primary]
 *
 * Requests the current selection N times through wlr-data-control, keeps the
 * read ends of the pipes open without reading, and exits after SECONDS. A
 * source that writes blocking hangs as soon as a pipe is full (64 KiB).
 * wl-paste cannot show that: it reads everything into a file of its own first.
 * Exits 2 when there is no selection, 1 on other errors. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_data_control_manager_v1 *mgr;
static uint32_t mgr_ver;
static struct zwlr_data_control_offer_v1 *sel, *prim;

static void global(void *d, struct wl_registry *r, uint32_t name,
		   const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwlr_data_control_manager_v1_interface.name)) {
		mgr_ver = ver < 2 ? ver : 2;
		mgr = wl_registry_bind(r, name, &zwlr_data_control_manager_v1_interface, mgr_ver);
	}
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_l = { global, global_remove };

static void offer_mime(void *d, struct zwlr_data_control_offer_v1 *o, const char *m)
{
	(void)d; (void)o; (void)m;
}
static const struct zwlr_data_control_offer_v1_listener offer_l = { offer_mime };

static void dev_offer(void *d, struct zwlr_data_control_device_v1 *dev,
		      struct zwlr_data_control_offer_v1 *o)
{
	(void)d; (void)dev;
	zwlr_data_control_offer_v1_add_listener(o, &offer_l, NULL);
}
static void dev_selection(void *d, struct zwlr_data_control_device_v1 *dev,
			  struct zwlr_data_control_offer_v1 *o)
{
	(void)d; (void)dev;
	sel = o;
}
static void dev_finished(void *d, struct zwlr_data_control_device_v1 *dev) { (void)d; (void)dev; }
static void dev_primary(void *d, struct zwlr_data_control_device_v1 *dev,
			struct zwlr_data_control_offer_v1 *o)
{
	(void)d; (void)dev;
	prim = o;
}
static const struct zwlr_data_control_device_v1_listener dev_l = {
	dev_offer, dev_selection, dev_finished, dev_primary,
};

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: stallreader N SECONDS [primary]\n");
		return 1;
	}
	int n = atoi(argv[1]), secs = atoi(argv[2]);
	int primary = argc > 3 && !strcmp(argv[3], "primary");
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy)
		return 1;
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !mgr || (primary && mgr_ver < 2)) {
		fprintf(stderr, "stallreader: no seat or wlr-data-control\n");
		return 1;
	}
	struct zwlr_data_control_device_v1 *dev =
		zwlr_data_control_manager_v1_get_data_device(mgr, seat);
	zwlr_data_control_device_v1_add_listener(dev, &dev_l, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	struct zwlr_data_control_offer_v1 *o = primary ? prim : sel;
	if (!o) {
		fprintf(stderr, "stallreader: no selection\n");
		return 2;
	}
	for (int i = 0; i < n; i++) {
		int fds[2];
		if (pipe(fds) < 0)
			return 1;
		zwlr_data_control_offer_v1_receive(o, "text/plain;charset=utf-8", fds[1]);
		close(fds[1]); /* fds[0] stays open and is never read */
	}
	wl_display_flush(dpy);
	printf("stalled %d\n", n);
	fflush(stdout);
	sleep(secs);
	return 0;
}
