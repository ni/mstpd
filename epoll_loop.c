/*****************************************************************************
  Copyright (c) 2006 EMC Corporation.
  Copyright (c) 2011 Factor-SPE

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the Free
  Software Foundation; either version 2 of the License, or (at your option)
  any later version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.

  The full GNU General Public License is included in this distribution in the
  file called LICENSE.

  Authors: Srinivas Aji <Aji_Srinivas@emc.com>
  Authors: Vitalii Demianets <dvitasgs@gmail.com>

******************************************************************************/

#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "log.h"
#include "epoll_loop.h"
#include "bridge_ctl.h"
#include "clock_gettime.h"

/* globals */
static int epoll_fd = -1;
static struct timespec nexttimeout;

int init_epoll(void)
{
    int r = epoll_create(128);
    if(r < 0)
    {
        ERROR("epoll_create failed: %m\n");
        return -1;
    }
    epoll_fd = r;
    return 0;
}

int add_epoll(struct epoll_event_handler *h)
{
    struct epoll_event ev =
    {
        .events = EPOLLIN,
        .data.ptr = h,
    };
    h->ref_ev = NULL;
    int r = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, h->fd, &ev);
    if(r < 0)
    {
        ERROR("epoll_ctl_add: %m\n");
        return -1;
    }
    return 0;
}

int remove_epoll(struct epoll_event_handler *h)
{
    int r = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, h->fd, NULL);
    if(r < 0)
    {
        ERROR("epoll_ctl_del: %m\n");
        return -1;
    }
    if(h->ref_ev && h->ref_ev->data.ptr == h)
    {
        h->ref_ev->data.ptr = NULL;
        h->ref_ev = NULL;
    }
    return 0;
}

void clear_epoll(void)
{
    if(epoll_fd >= 0)
        close(epoll_fd);
}

static inline int time_diff(struct timespec *second, struct timespec *first)
{
    return (second->tv_sec - first->tv_sec) * 1000
            + (second->tv_nsec - first->tv_nsec) / 1000000;
}

static inline void run_timeouts(void)
{
    bridge_one_second();
    ++(nexttimeout.tv_sec);
}

int epoll_main_loop(volatile bool *quit)
{
    clock_gettime(CLOCK_MONOTONIC, &nexttimeout);
    ++(nexttimeout.tv_sec);
#define EV_SIZE 8
    struct epoll_event ev[EV_SIZE];

    while(!*quit)
    {
        int r, i;
        int timeout;

        struct timespec tv;
        clock_gettime(CLOCK_MONOTONIC, &tv);
        timeout = time_diff(&nexttimeout, &tv);
        if(timeout < 0 || timeout > 1000)
        {
            run_timeouts();
            /*
             * Check if system time has changed.
             */
            if(timeout < -4000 || timeout > 1000)
            {
                /* Most probably, system time has changed */
                nexttimeout.tv_nsec = tv.tv_nsec;
                nexttimeout.tv_sec = tv.tv_sec + 1;
            }
            timeout = 0;
        }

        r = epoll_wait(epoll_fd, ev, EV_SIZE, timeout);
        if(r < 0 && errno != EINTR)
        {
            ERROR("epoll_wait: %m\n");
            return -1;
        }
        for(i = 0; i < r; ++i)
        {
            struct epoll_event_handler *p = ev[i].data.ptr;
            if(p != NULL)
                p->ref_ev = &ev[i];
        }
        for (i = 0; i < r; ++i)
        {
            struct epoll_event_handler *p = ev[i].data.ptr;
            if(p && p->handler)
                p->handler(ev[i].events, p);
        }
        for (i = 0; i < r; ++i)
        {
            struct epoll_event_handler *p = ev[i].data.ptr;
            if(p != NULL)
                p->ref_ev = NULL;
        }
    }

    return 0;
}

int epoll_timer_init(struct epoll_event_handler* timer) {
    timer->arg = NULL;
    timer->handler = NULL;
    timer->priv = 0;
    timer->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

    if(timer->fd < 0) {
        ERROR("timerfd_create: %m\n");
        return -1;
    }

    return add_epoll(timer);
};

void epoll_timer_close(struct epoll_event_handler* timer) {
    close(timer->fd);
    timer->fd = -1;
};

void epoll_timer_start(struct epoll_event_handler* timer, int seconds) {
    const struct itimerspec new_value = {
        .it_interval = {0},
        .it_value = {
            .tv_sec = seconds,
            .tv_nsec = 0},
    };
    // Starting clears the "expired" flag
    timer->priv = 0;
    timerfd_settime(timer->fd, 0, &new_value, NULL);
}

int epoll_timer_expired(struct epoll_event_handler* timer) {
    uint64_t expirations;
    ssize_t s;
    /* This check must be idempotent to support dry_run code. If we
     * already found the timer to be expired, keep returning true
     * without reading the timerfd.
    */
    if(timer->priv) {
            return 1;
    }
    /* If we did not find the timer to be expired already, check
     * it. Set flag if it expired.
     */
    s = read(timer->fd, &expirations, sizeof(expirations));
    if(s == sizeof(expirations)) {
            return timer->priv = 1;
    }
    // Log any errors
    if(s < 0) {
        if(errno != EAGAIN) {
            ERROR("timerfd read(): %m\n");
        }
    } else if(s != sizeof(expirations)) {
        ERROR("timerfd read() returned %d bytes", s);
    }
    /* If the timer isn't readable for any reason, assume it is still
     * running
     */
    return 0;
}

/* Treating the timer as a countdown, return which second of the
 * countdown we are in. Zero means the timer has expired. This logic
 * is required by the PRSM to reproduce the behavior of a prior
 * 'tick-based' check.
 */
int epoll_timer_which_second(struct epoll_event_handler* timer) {
    struct itimerspec current_value;
    timerfd_gettime(timer->fd, &current_value);
    /* assuming the timer starts from {seconds, 0} we can calculate
     * which second of the countdown we are in by "rounding up" the
     * current number of seconds when nsec is non-zero.
     */
    return current_value.it_value.tv_sec + (current_value.it_value.tv_nsec ? 1 : 0);
}
