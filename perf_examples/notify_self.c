/*
 * notify_self.c - example of how you can use overflow notifications
 *
 * Copyright (c) 2009 Google, Inc
 * Contributed by Stephane Eranian <eranian@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <err.h>
#include <sys/mman.h>

#include "perf_util.h"

#define SMPL_PERIOD	2400000000ULL

static volatile unsigned long notification_received;

static perf_event_desc_t *fds;
static int num_events;

static int buffer_pages = 1; /* size of buffer payload (must be power of 2)*/

static void
print_sample(int id)
{
	uint64_t nr, ip;
	uint64_t time_enabled, time_running;
	struct { uint64_t value, id; } grp;
	int e, ret;
	char *str;

	ret = perf_read_buffer_64(fds[id].buf, fds[id].pgmsk, &ip);
	if (ret)
		errx(1, "cannot read IP");


	ret = perf_read_buffer_64(fds[id].buf, fds[id].pgmsk, &nr);
	if (ret)
		errx(1, "cannot read sample");

	time_enabled = time_running = 1;

	ret = perf_read_buffer_64(fds[id].buf, fds[id].pgmsk, &time_enabled);
	if (ret)
		errx(1, "cannot read timing info");

	ret = perf_read_buffer_64(fds[id].buf, fds[id].pgmsk, &time_running);
	if (ret)
		errx(1, "cannot read timing info");

	printf("Notification %lu: ip=0x%"PRIx64" ena=%"PRIu64" run=%"PRIu64"\n", notification_received, ip, time_enabled, time_running);
	while(nr--) {
		ret = perf_read_buffer(fds[id].buf, fds[id].pgmsk, &grp, sizeof(grp));
		if (ret)
			errx(1, "cannot read grp");

		e = perf_id2event(fds, num_events, grp.id);
		if (e == -1)
			str = "unknown event";
		else
			str = fds[e].name;

		if (time_running)
			grp.value = grp.value * time_enabled / time_running;
		else
			grp.value = 0;

		printf("\t%"PRIu64" %s (%"PRIu64"%s)\n",
			grp.value, str,
			grp.id,
			time_running != time_enabled ? ", scaled":"");

	}
}

static void
sigio_handler(int n, struct siginfo *info, void *uc)
{
	struct perf_event_header ehdr;
	int ret, id;
	
	/*
	 * positive si_code indicate kernel generated signal
	 * which is normal for SIGIO
	 */
	if (info->si_code < 0)
		errx(1, "signal not generated by kernel");

	/*
	 * SIGPOLL = SIGIO
	 * expect POLL_HUP instead of POLL_IN because we are
	 * in one-shot mode (IOC_REFRESH)
	 */
	if (info->si_code != POLL_HUP)
		errx(1, "signal not generated by SIGIO");

	id = perf_fd2event(fds, num_events, info->si_fd);
	if (id == -1)
		errx(1, "no event associated with fd=%d", info->si_fd);

	ret = perf_read_buffer(fds[id].buf, fds[id].pgmsk, &ehdr, sizeof(ehdr));
	if (ret)
		errx(1, "cannot read event header");

	if (ehdr.type != PERF_RECORD_SAMPLE) {
		warnx("unexpected sample type=%d, skipping\n", ehdr.type);
		perf_skip_buffer(fds[id].buf, ehdr.size);
		goto skip;
	}
	print_sample(id);
	/*
	 * increment our notification counter
	 */
	notification_received++;
skip:
	/*
 	 * rearm the counter for one more shot
 	 */
	ret = ioctl(info->si_fd, PERF_EVENT_IOC_REFRESH, 1);
	if (ret == -1)
		err(1, "cannot refresh");

}

/*
 * infinite loop waiting for notification to get out
 */
void
busyloop(void)
{
	/*
	 * busy loop to burn CPU cycles
	 */
	for(;notification_received < 20;) ;
}

int
main(int argc, char **argv)
{
	struct sigaction act;
	uint64_t *val;
	size_t sz, pgsz;
	int ret, i;

	ret = pfm_initialize();
	if (ret != PFM_SUCCESS)
		errx(1, "Cannot initialize library: %s", pfm_strerror(ret));

	pgsz = getpagesize();

	/*
	 * Install the signal handler (SIGIO)
	 * need SA_SIGINFO because we need the fd
	 * in the signal handler
	 */
	memset(&act, 0, sizeof(act));
	act.sa_sigaction = sigio_handler;
	act.sa_flags = SA_SIGINFO;
	sigaction (SIGIO, &act, 0);

	/*
 	 * allocates fd for us
 	 */
	num_events = perf_setup_list_events("PERF_COUNT_HW_CPU_CYCLES,"
				       "PERF_COUNT_HW_INSTRUCTIONS",
					&fds);
	if (num_events < 1)
		exit(1);

	fds[0].fd = -1;
	for(i=0; i < num_events; i++) {

		/* want a notification for every each added to the buffer */
		fds[i].hw.disabled = !i;
		if (!i) {
			fds[i].hw.wakeup_events = 1;
			fds[i].hw.sample_type = PERF_SAMPLE_IP|PERF_SAMPLE_READ;
			fds[i].hw.sample_period = SMPL_PERIOD;

			/* read() returns event identification for signal handler */
			fds[i].hw.read_format = PERF_FORMAT_GROUP|PERF_FORMAT_ID|PERF_FORMAT_SCALE;
		} 

		fds[i].fd = perf_event_open(&fds[i].hw, 0, -1, fds[0].fd, 0);
		if (fds[i].fd == -1)
			err(1, "cannot attach event %s", fds[i].name);
	}
	
	sz = (3+2*num_events)*sizeof(uint64_t);
	val = malloc(sz);
	if (!val)
		err(1, "cannot allocated memory");
	/*
	 * On overflow, the non lead events are stored in the sample.
	 * However we need some key to figure the order in which they
	 * were laid out in the buffer. The file descriptor does not
	 * work for this. Instead, we extract a unique ID for each event.
	 * That id will be part of the sample for each event value.
	 * Therefore we will be able to match value to events
	 *
	 * PERF_FORMAT_ID: returns unique 64-bit identifier in addition
	 * to event value.
	 */
	ret = read(fds[0].fd, val, sz);
	if (ret == -1)
		err(1, "cannot read id %zu", sizeof(val));

	/*
	 * we are using PERF_FORMAT_GROUP, therefore the structure
	 * of val is as follows:
	 *
	 *      { u64           nr;
	 *        { u64         time_enabled; } && PERF_FORMAT_ENABLED
	 *        { u64         time_running; } && PERF_FORMAT_RUNNING
	 *        { u64         value;                  
	 *          { u64       id;           } && PERF_FORMAT_ID
	 *        }             cntr[nr];               
	 * We are skipping the first 3 values (nr, time_enabled, time_running)
	 * and then for each event we get a pair of values.
	 */ 
	for(i=0; i < num_events; i++) {
		fds[i].id = val[2*i+1+3];
		printf("%"PRIu64"  %s\n", fds[i].id, fds[i].name);
	}
	 
	fds[0].buf = mmap(NULL, (buffer_pages+1)*pgsz, PROT_READ|PROT_WRITE, MAP_SHARED, fds[0].fd, 0);
	if (fds[0].buf == MAP_FAILED)
		err(1, "cannot mmap buffer");
	
	fds[0].pgmsk = (buffer_pages * pgsz) - 1;

	/*
	 * setup asynchronous notification on the file descriptor
	 */
	ret = fcntl(fds[0].fd, F_SETFL, fcntl(fds[0].fd, F_GETFL, 0) | O_ASYNC);
	if (ret == -1)
		err(1, "cannot set ASYNC");

	/*
 	 * necessary if we want to get the file descriptor for
 	 * which the SIGIO is sent in siginfo->si_fd.
 	 * SA_SIGINFO in itself is not enough
 	 */
	ret = fcntl(fds[0].fd, F_SETSIG, SIGIO);
	if (ret == -1)
		err(1, "cannot setsig");

	/*
	 * get ownership of the descriptor
	 */
	ret = fcntl(fds[0].fd, F_SETOWN, getpid());
	if (ret == -1)
		err(1, "cannot setown");

	/*
	 * enable the group for one period
	 */
	ret = ioctl(fds[0].fd, PERF_EVENT_IOC_REFRESH , 1);
	if (ret == -1)
		err(1, "cannot refresh");

	busyloop();

	ret = ioctl(fds[0].fd, PERF_EVENT_IOC_DISABLE, 1);
	if (ret == -1)
		err(1, "cannot disable");

	/*
	 * destroy our session
	 */
	for(i=0; i < num_events; i++)
		close(fds[i].fd);

	free(fds);
	free(val);

	return 0;
}
