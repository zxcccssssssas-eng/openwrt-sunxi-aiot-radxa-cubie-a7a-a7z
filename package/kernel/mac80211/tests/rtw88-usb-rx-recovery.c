/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace fault-injection shims for the actual prepared USB RX functions. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define RTW_USB_MAX_RECVBUF_SZ 32768
#define RTW_USB_RXCB_NUM 4
#define GFP_ATOMIC 1
#define GFP_KERNEL 2
#define READ_ONCE(x) (x)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define to_delayed_work(p) container_of(p, struct delayed_work, work)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define msecs_to_jiffies(x) (x)
#define rtw_err(dev, ...) ((void)(dev))
#define dev_err_ratelimited(dev, ...) ((void)(dev))
typedef int gfp_t;
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool pending; };
struct sk_buff { char *data; int len; };
struct sk_buff_head { struct sk_buff *items[16]; int count; };
struct urb { int status, actual_length; void *context; bool active; };
struct rtw_usb;
struct rtw_dev { struct rtw_usb *priv; void *dev; };
struct rx_usb_ctrl_block {
    struct rtw_dev *rtwdev;
    struct urb *rx_urb;
    struct delayed_work retry_work;
    struct sk_buff *rx_skb;
};
struct rtw_usb {
    struct rtw_dev *rtwdev;
    void *udev, *rxwq;
    int pipe_in, rx_lock;
    bool rx_stopped;
    struct rx_usb_ctrl_block rx_cb[RTW_USB_RXCB_NUM];
    struct sk_buff_head rx_queue;
    struct work_struct rx_work;
};
static struct rtw_usb usb;
static struct rtw_dev dev = { .priv = &usb };
static struct urb urbs[RTW_USB_RXCB_NUM];
static int allocation_failures, submit_error, submits, live_skbs, last_gfp, cancels;
static struct rtw_usb *rtw_get_usb_priv(struct rtw_dev *d) { return d->priv; }
static struct sk_buff *alloc_skb(int size, gfp_t gfp)
{
    last_gfp = gfp;
    if (allocation_failures) { allocation_failures--; return NULL; }
    struct sk_buff *s = calloc(1, sizeof(*s));
    assert(s);
    s->data = malloc(size);
    assert(s->data);
    live_skbs++;
    return s;
}
static void kfree_skb(struct sk_buff *s)
{
    if (!s) return;
    free(s->data); free(s); live_skbs--;
}
#define dev_kfree_skb_any kfree_skb
static void skb_put(struct sk_buff *s, int len) { s->len += len; }
static void skb_queue_tail(struct sk_buff_head *q, struct sk_buff *s)
{
    assert(q->count < 16); q->items[q->count++] = s;
}
static void queue_work(void *q, struct work_struct *w) { (void)q; (void)w; }
static void queue_delayed_work(void *q, struct delayed_work *w, int delay)
{
    (void)q; assert(delay >= 1); w->pending = true;
}
static int usb_rcvbulkpipe(void *u, int pipe) { (void)u; return pipe; }
static void usb_fill_bulk_urb(struct urb *u, void *device, int pipe,
                             void *data, int len, void (*complete)(struct urb *), void *ctx)
{
    (void)device; (void)pipe; (void)data; (void)len; (void)complete;
    u->context = ctx;
}
static int usb_submit_urb(struct urb *u, gfp_t gfp)
{
    submits++; last_gfp = gfp;
    assert(!u->active);
    if (!submit_error) u->active = true;
    return submit_error;
}
static void cancel_delayed_work_sync(struct delayed_work *w)
{
    assert(usb.rx_stopped); w->pending = false; cancels++;
}
static void rtw_usb_read_port_complete(struct urb *urb);
static void usb_kill_urb(struct urb *u)
{
    assert(cancels >= RTW_USB_RXCB_NUM);
    if (u->active) {
        u->active = false; u->status = -ENOENT;
        rtw_usb_read_port_complete(u);
    }
}
#include "rtw88-usb-rx-functions.h"
static void initialize(void)
{
    assert(live_skbs == 0);
    usb = (struct rtw_usb){ .rtwdev = &dev };
    submits = cancels = submit_error = allocation_failures = 0;
    for (int i = 0; i < RTW_USB_RXCB_NUM; i++) {
        urbs[i] = (struct urb){0};
        usb.rx_cb[i] = (struct rx_usb_ctrl_block){ .rtwdev = &dev, .rx_urb = &urbs[i] };
        rtw_usb_rx_resubmit(&usb, &usb.rx_cb[i], GFP_KERNEL);
        assert(urbs[i].active);
    }
}
static void complete(int i, int status, int length)
{
    assert(urbs[i].active);
    urbs[i].active = false; urbs[i].status = status; urbs[i].actual_length = length;
    rtw_usb_read_port_complete(&urbs[i]);
}
static void retry(int i)
{
    assert(usb.rx_cb[i].retry_work.pending);
    usb.rx_cb[i].retry_work.pending = false;
    rtw_usb_rx_retry_work(&usb.rx_cb[i].retry_work.work);
}
static void cleanup(void)
{
    rtw_usb_cancel_rx_bufs(&usb);
    for (int i = 0; i < usb.rx_queue.count; i++) kfree_skb(usb.rx_queue.items[i]);
    for (int i = 0; i < RTW_USB_RXCB_NUM; i++) {
        assert(!urbs[i].active); assert(!usb.rx_cb[i].retry_work.pending);
        assert(!usb.rx_cb[i].rx_skb);
    }
    assert(live_skbs == 0);
}
int main(void)
{
    int transient[] = { -EPROTO, -EILSEQ, -ETIME, -ETIMEDOUT, -ECOMM, -EOVERFLOW };
    int terminal[] = { -EINVAL, -EPIPE, -ENODEV, -ESHUTDOWN, -ENOENT, -ECONNRESET, -EINPROGRESS };
    for (size_t e = 0; e < sizeof(transient)/sizeof(*transient); e++) {
        initialize();
        /* Lose every receive request, then verify that all four recover. */
        for (int cycle = 0; cycle < 100; cycle++) {
            for (int i = 0; i < RTW_USB_RXCB_NUM; i++) complete(i, transient[e], 0);
            assert(live_skbs == 0);
            for (int i = 0; i < RTW_USB_RXCB_NUM; i++) {
                retry(i); assert(urbs[i].active); assert(last_gfp == GFP_KERNEL);
            }
        }
        cleanup();
    }
    for (size_t e = 0; e < sizeof(terminal)/sizeof(*terminal); e++) {
        initialize(); complete(0, terminal[e], 0);
        assert(!usb.rx_cb[0].retry_work.pending); assert(submits == 4);
        cleanup();
    }
    initialize(); complete(0, 0, 128);
    assert(usb.rx_queue.count == 1); assert(usb.rx_queue.items[0]->len == 128);
    assert(urbs[0].active); cleanup();
    initialize(); complete(0, 0, 8);
    assert(usb.rx_queue.count == 0); assert(urbs[0].active); cleanup();
    initialize(); complete(0, 0, RTW_USB_MAX_RECVBUF_SZ + 1);
    assert(usb.rx_queue.count == 0); assert(urbs[0].active); cleanup();
    initialize(); allocation_failures = 2;
    complete(0, 0, 8); assert(usb.rx_cb[0].retry_work.pending);
    retry(0); assert(usb.rx_cb[0].retry_work.pending);
    retry(0); assert(urbs[0].active); cleanup();
    int resource_errors[] = { -ENOMEM, -EAGAIN };
    for (size_t e = 0; e < sizeof(resource_errors)/sizeof(*resource_errors); e++) {
        initialize(); submit_error = resource_errors[e]; complete(0, 0, 8);
        assert(!usb.rx_cb[0].rx_skb); assert(live_skbs == 3);
        submit_error = 0; retry(0); assert(urbs[0].active); cleanup();
    }
    initialize(); submit_error = -ENODEV; complete(0, 0, 8);
    assert(!usb.rx_cb[0].retry_work.pending); submit_error = 0; cleanup();
    initialize(); complete(0, -EPROTO, 0);
    cleanup();
    int before = submits;
    /* A late callback/worker cannot requeue or submit after shutdown. */
    rtw_usb_rx_retry(&usb, &usb.rx_cb[0]);
    rtw_usb_rx_resubmit(&usb, &usb.rx_cb[0], GFP_KERNEL);
    assert(!usb.rx_cb[0].retry_work.pending); assert(submits == before);
    puts("PASS: RX recovery, allocation/submit failures, terminal errors, ownership, shutdown");
    return 0;
}
