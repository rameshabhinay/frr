// SPDX-License-Identifier: GPL-2.0-or-later
/* BGP I/O.
 * Implements packet I/O in a pthread.
 * Copyright (C) 2017  Cumulus Networks
 * Quentin Young
 */

/* clang-format off */
#include <zebra.h>
#include <pthread.h>		// for pthread_mutex_unlock, pthread_mutex_lock
#include <sys/uio.h>		// for writev

#include "frr_pthread.h"
#include "linklist.h"		// for list_delete, list_delete_all_node, lis...
#include "log.h"		// for zlog_debug, safe_strerror, zlog_err
#include "memory.h"		// for MTYPE_TMP, XCALLOC, XFREE
#include "network.h"		// for ERRNO_IO_RETRY
#include "stream.h"		// for stream_get_endp, stream_getw_from, str...
#include "ringbuf.h"		// for ringbuf_remain, ringbuf_peek, ringbuf_...
#include "frrevent.h"		// for EVENT_OFF, EVENT_ARG, thread...

#include "bgpd/bgp_io.h"
#include "bgpd/bgp_debug.h"	// for bgp_debug_neighbor_events, bgp_type_str
#include "bgpd/bgp_errors.h"	// for expanded error reference information
#include "bgpd/bgp_fsm.h"	// for BGP_EVENT_ADD, bgp_event
#include "bgpd/bgp_packet.h"	// for bgp_notify_io_invalid...
#include "bgpd/bgp_trace.h"	// for frrtraces
#include "bgpd/bgpd.h"		// for peer, BGP_MARKER_SIZE, bgp_master, bm
/* clang-format on */

/* forward declarations */
static uint16_t bgp_write(struct peer *);
static uint16_t bgp_read(struct peer *peer, int *code_p);
static void bgp_process_writes(struct event *event);
static void bgp_process_reads(struct event *event);
static bool validate_header(struct peer *);

/* generic i/o status codes */
#define BGP_IO_TRANS_ERR (1 << 0) /* EAGAIN or similar occurred */
#define BGP_IO_FATAL_ERR (1 << 1) /* some kind of fatal TCP error */
#define BGP_IO_WORK_FULL_ERR (1 << 2) /* No room in work buffer */

/* Thread external API ----------------------------------------------------- */

void bgp_writes_on(struct peer *peer)
{
	struct frr_pthread *fpt = bgp_pth_io;
	assert(fpt->running);

	assert(peer->status != Deleted);
	assert(peer->obuf);
	assert(peer->ibuf);
	assert(peer->ibuf_work);
	assert(!peer->t_connect_check_r);
	assert(!peer->t_connect_check_w);
	assert(peer->fd);

	event_add_write(fpt->master, bgp_process_writes, peer, peer->fd,
			&peer->t_write);
	SET_FLAG(peer->thread_flags, PEER_THREAD_WRITES_ON);
}

void bgp_writes_off(struct peer *peer)
{
	struct frr_pthread *fpt = bgp_pth_io;
	assert(fpt->running);

	event_cancel_async(fpt->master, &peer->t_write, NULL);
	EVENT_OFF(peer->t_generate_updgrp_packets);

	UNSET_FLAG(peer->thread_flags, PEER_THREAD_WRITES_ON);
}

void bgp_reads_on(struct peer *peer)
{
	struct frr_pthread *fpt = bgp_pth_io;
	assert(fpt->running);

	assert(peer->status != Deleted);
	assert(peer->ibuf);
	assert(peer->fd);
	assert(peer->ibuf_work);
	assert(peer->obuf);
	assert(!peer->t_connect_check_r);
	assert(!peer->t_connect_check_w);
	assert(peer->fd);

	event_add_read(fpt->master, bgp_process_reads, peer, peer->fd,
		       &peer->t_read);

	SET_FLAG(peer->thread_flags, PEER_THREAD_READS_ON);
}

void bgp_reads_off(struct peer *peer)
{
	struct frr_pthread *fpt = bgp_pth_io;
	assert(fpt->running);

	event_cancel_async(fpt->master, &peer->t_read, NULL);
	EVENT_OFF(peer->t_process_packet);
	EVENT_OFF(peer->t_process_packet_error);

	UNSET_FLAG(peer->thread_flags, PEER_THREAD_READS_ON);
}

/* Thread internal functions ----------------------------------------------- */

/*
 * Called from I/O pthread when a file descriptor has become ready for writing.
 */
static void bgp_process_writes(struct event *thread)
{
	static struct peer *peer;
	peer = EVENT_ARG(thread);
	uint16_t status;
	bool reschedule;
	bool fatal = false;

	if (peer->fd < 0)
		return;

	struct frr_pthread *fpt = bgp_pth_io;

	frr_with_mutex (&peer->io_mtx) {
		status = bgp_write(peer);
		reschedule = (stream_fifo_head(peer->obuf) != NULL);
	}

	/* no problem */
	if (CHECK_FLAG(status, BGP_IO_TRANS_ERR)) {
	}

	/* problem */
	if (CHECK_FLAG(status, BGP_IO_FATAL_ERR)) {
		reschedule = false;
		fatal = true;
	}

	/* If suppress fib pending is enabled, route is advertised to peers when
	 * the status is received from the FIB. The delay is added
	 * to update group packet generate which will allow more routes to be
	 * sent in the update message
	 */
	if (reschedule) {
		event_add_write(fpt->master, bgp_process_writes, peer, peer->fd,
				&peer->t_write);
	} else if (!fatal) {
		BGP_UPDATE_GROUP_TIMER_ON(&peer->t_generate_updgrp_packets,
					  bgp_generate_updgrp_packets);
	}
}

static int read_ibuf_work(struct peer *peer)
{
	/* static buffer for transferring packets */
	/* shorter alias to peer's input buffer */
	struct ringbuf *ibw = peer->ibuf_work;
	/* packet size as given by header */
	uint16_t pktsize = 0;
	struct stream *pkt;

	/* ============================================== */
	frr_with_mutex (&peer->io_mtx) {
		if (peer->ibuf->count >= bm->inq_limit)
			return -ENOMEM;
	}

	/* check that we have enough data for a header */
	if (ringbuf_remain(ibw) < BGP_HEADER_SIZE)
		return 0;

	/* check that header is valid */
	if (!validate_header(peer))
		return -EBADMSG;

	/* header is valid; retrieve packet size */
	ringbuf_peek(ibw, BGP_MARKER_SIZE, &pktsize, sizeof(pktsize));

	pktsize = ntohs(pktsize);

	/* if this fails we are seriously screwed */
	assert(pktsize <= peer->max_packet_size);

	/*
	 * If we have that much data, chuck it into its own
	 * stream and append to input queue for processing.
	 *
	 * Otherwise, come back later.
	 */
	if (ringbuf_remain(ibw) < pktsize)
		return 0;

	pkt = stream_new(pktsize);
	assert(STREAM_WRITEABLE(pkt) == pktsize);
	assert(ringbuf_get(ibw, pkt->data, pktsize) == pktsize);
	stream_set_endp(pkt, pktsize);

	frrtrace(2, frr_bgp, packet_read, peer, pkt);
	frr_with_mutex (&peer->io_mtx) {
		stream_fifo_push(peer->ibuf, pkt);
	}

	return pktsize;
}

/*
 * Called from I/O pthread when a file descriptor has become ready for reading,
 * or has hung up.
 *
 * We read as much data as possible, process as many packets as we can and
 * place them on peer->ibuf for secondary processing by the main thread.
 */
static void bgp_process_reads(struct event *thread)
{
	/* clang-format off */
	static struct peer *peer;       /* peer to read from */
	uint16_t status;                /* bgp_read status code */
	bool fatal = false;             /* whether fatal error occurred */
	bool added_pkt = false;         /* whether we pushed onto ->ibuf */
	int code = 0;                   /* FSM code if error occurred */
	bool ibuf_full = false;         /* Is peer fifo IN Buffer full */
	static bool ibuf_full_logged;   /* Have we logged full already */
	int ret = 1;
	/* clang-format on */

	peer = EVENT_ARG(thread);

	if (bm->terminating || peer->fd < 0)
		return;

	struct frr_pthread *fpt = bgp_pth_io;

	frr_with_mutex (&peer->io_mtx) {
		status = bgp_read(peer, &code);
	}

	/* error checking phase */
	if (CHECK_FLAG(status, BGP_IO_TRANS_ERR)) {
		/* no problem; just don't process packets */
		goto done;
	}

	if (CHECK_FLAG(status, BGP_IO_FATAL_ERR)) {
		/* problem; tear down session */
		fatal = true;

		/* Handle the error in the main pthread, include the
		 * specific state change from 'bgp_read'.
		 */
		event_add_event(bm->master, bgp_packet_process_error, peer,
				code, &peer->t_process_packet_error);
		goto done;
	}

	while (true) {
		ret = read_ibuf_work(peer);
		if (ret <= 0)
			break;

		added_pkt = true;
	}

	switch (ret) {
	case -EBADMSG:
		fatal = true;
		break;
	case -ENOMEM:
		ibuf_full = true;
		if (!ibuf_full_logged) {
			if (bgp_debug_neighbor_events(peer))
				zlog_debug(
					"%s [Event] Peer Input-Queue is full: limit (%u)",
					peer->host, bm->inq_limit);

			ibuf_full_logged = true;
		}
		break;
	default:
		ibuf_full_logged = false;
		break;
	}

done:
	/* handle invalid header */
	if (fatal) {
		/* wipe buffer just in case someone screwed up */
		ringbuf_wipe(peer->ibuf_work);
		return;
	}

	/* ringbuf should be fully drained unless ibuf is full */
	if (!ibuf_full)
		assert(ringbuf_space(peer->ibuf_work) >= peer->max_packet_size);

	event_add_read(fpt->master, bgp_process_reads, peer, peer->fd,
		       &peer->t_read);
	if (added_pkt)
		event_add_event(bm->master, bgp_process_packet, peer, 0,
				&peer->t_process_packet);
}

/*
 * Flush peer output buffer.
 *
 * This function pops packets off of peer->obuf and writes them to peer->fd.
 * The amount of packets written is equal to the minimum of peer->wpkt_quanta
 * and the number of packets on the output buffer, unless an error occurs.
 *
 * If write() returns an error, the appropriate FSM event is generated.
 *
 * The return value is equal to the number of packets written
 * (which may be zero).
 */
static uint16_t bgp_write(struct peer *peer)
{
	uint8_t type;
	struct stream *s;
	int update_last_write = 0;
	unsigned int count;
	uint32_t uo = 0;
	uint16_t status = 0;
	uint32_t wpkt_quanta_old;

	int writenum = 0;
	int num;
	unsigned int iovsz;
	unsigned int strmsz;
	unsigned int total_written;
	time_t now;

	wpkt_quanta_old = atomic_load_explicit(&peer->bgp->wpkt_quanta,
					       memory_order_relaxed);
	struct stream *ostreams[wpkt_quanta_old];
	struct stream **streams = ostreams;
	struct iovec iov[wpkt_quanta_old];

	s = stream_fifo_head(peer->obuf);

	if (!s)
		goto done;

	count = iovsz = 0;
	while (count < wpkt_quanta_old && iovsz < array_size(iov) && s) {
		ostreams[iovsz] = s;
		iov[iovsz].iov_base = stream_pnt(s);
		iov[iovsz].iov_len = STREAM_READABLE(s);
		writenum += STREAM_READABLE(s);
		s = s->next;
		++iovsz;
		++count;
	}

	strmsz = iovsz;
	total_written = 0;

	do {
		num = writev(peer->fd, iov, iovsz);

		if (num < 0) {
			if (!ERRNO_IO_RETRY(errno)) {
				BGP_EVENT_ADD(peer, TCP_fatal_error);
				SET_FLAG(status, BGP_IO_FATAL_ERR);
			} else {
				SET_FLAG(status, BGP_IO_TRANS_ERR);
			}

			break;
		} else if (num != writenum) {
			unsigned int msg_written = 0;
			unsigned int ic = iovsz;

			for (unsigned int i = 0; i < ic; i++) {
				size_t ss = iov[i].iov_len;

				if (ss > (unsigned int) num)
					break;

				msg_written++;
				iovsz--;
				writenum -= ss;
				num -= ss;
			}

			total_written += msg_written;

			assert(total_written < count);

			memmove(&iov, &iov[msg_written],
				sizeof(iov[0]) * iovsz);
			streams = &streams[msg_written];
			stream_forward_getp(streams[0], num);
			iov[0].iov_base = stream_pnt(streams[0]);
			iov[0].iov_len = STREAM_READABLE(streams[0]);

			writenum -= num;
			num = 0;
			assert(writenum > 0);
		} else {
			total_written = strmsz;
		}

	} while (num != writenum);

	/* Handle statistics */
	for (unsigned int i = 0; i < total_written; i++) {
		s = stream_fifo_pop(peer->obuf);

		assert(s == ostreams[i]);

		/* Retrieve BGP packet type. */
		stream_set_getp(s, BGP_MARKER_SIZE + 2);
		type = stream_getc(s);

		switch (type) {
		case BGP_MSG_OPEN:
			atomic_fetch_add_explicit(&peer->open_out, 1,
						  memory_order_relaxed);
			break;
		case BGP_MSG_UPDATE:
			atomic_fetch_add_explicit(&peer->update_out, 1,
						  memory_order_relaxed);
			uo++;
			break;
		case BGP_MSG_NOTIFY:
			atomic_fetch_add_explicit(&peer->notify_out, 1,
						  memory_order_relaxed);
			/* Double start timer. */
			peer->v_start *= 2;

			/* Overflow check. */
			if (peer->v_start >= (60 * 2))
				peer->v_start = (60 * 2);

			/*
			 * Handle Graceful Restart case where the state changes
			 * to Connect instead of Idle.
			 */
			BGP_EVENT_ADD(peer, BGP_Stop);
			goto done;

		case BGP_MSG_KEEPALIVE:
			atomic_fetch_add_explicit(&peer->keepalive_out, 1,
						  memory_order_relaxed);
			break;
		case BGP_MSG_ROUTE_REFRESH_NEW:
		case BGP_MSG_ROUTE_REFRESH_OLD:
			atomic_fetch_add_explicit(&peer->refresh_out, 1,
						  memory_order_relaxed);
			break;
		case BGP_MSG_CAPABILITY:
			atomic_fetch_add_explicit(&peer->dynamic_cap_out, 1,
						  memory_order_relaxed);
			break;
		}

		stream_free(s);
		ostreams[i] = NULL;
		update_last_write = 1;
	}

done : {
	now = monotime(NULL);
	/*
	 * Update last_update if UPDATEs were written.
	 * Note: that these are only updated at end,
	 *       not per message (i.e., per loop)
	 */
	if (uo)
		atomic_store_explicit(&peer->last_update, now,
				      memory_order_relaxed);

	/* If we TXed any flavor of packet */
	if (update_last_write) {
		atomic_store_explicit(&peer->last_write, now,
				      memory_order_relaxed);
		peer->last_sendq_ok = now;
	}
}

	return status;
}

/*
 * Reads a chunk of data from peer->fd into peer->ibuf_work.
 *
 * code_p
 *    Pointer to location to store FSM event code in case of fatal error.
 *
 * @return status flag (see top-of-file)
 */
static uint16_t bgp_read(struct peer *peer, int *code_p)
{
	size_t readsize; /* how many bytes we want to read */
	ssize_t nbytes;  /* how many bytes we actually read */
	size_t ibuf_work_space; /* space we can read into the work buf */
	uint16_t status = 0;

	ibuf_work_space = ringbuf_space(peer->ibuf_work);

	if (ibuf_work_space == 0) {
		SET_FLAG(status, BGP_IO_WORK_FULL_ERR);
		return status;
	}

	readsize = MIN(ibuf_work_space, sizeof(peer->ibuf_scratch));

	nbytes = read(peer->fd, peer->ibuf_scratch, readsize);

	/* EAGAIN or EWOULDBLOCK; come back later */
	if (nbytes < 0 && ERRNO_IO_RETRY(errno)) {
		SET_FLAG(status, BGP_IO_TRANS_ERR);
	} else if (nbytes < 0) {
		/* Fatal error; tear down session */
		flog_err(EC_BGP_UPDATE_RCV,
			 "%s [Error] bgp_read_packet error: %s", peer->host,
			 safe_strerror(errno));

		/* Handle the error in the main pthread. */
		if (code_p)
			*code_p = TCP_fatal_error;

		SET_FLAG(status, BGP_IO_FATAL_ERR);

	} else if (nbytes == 0) {
		/* Received EOF / TCP session closed */
		if (bgp_debug_neighbor_events(peer))
			zlog_debug("%s [Event] BGP connection closed fd %d",
				   peer->host, peer->fd);

		/* Handle the error in the main pthread. */
		if (code_p)
			*code_p = TCP_connection_closed;

		SET_FLAG(status, BGP_IO_FATAL_ERR);
	} else {
		assert(ringbuf_put(peer->ibuf_work, peer->ibuf_scratch, nbytes)
		       == (size_t)nbytes);
	}

	return status;
}

/*
 * Called after we have read a BGP packet header. Validates marker, message
 * type and packet length. If any of these aren't correct, sends a notify.
 *
 * Assumes that there are at least BGP_HEADER_SIZE readable bytes in the input
 * buffer.
 */
static bool validate_header(struct peer *peer)
{
	uint16_t size;
	uint8_t type;
	struct ringbuf *pkt = peer->ibuf_work;

	static const uint8_t m_correct[BGP_MARKER_SIZE] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	uint8_t m_rx[BGP_MARKER_SIZE] = {0x00};

	if (ringbuf_peek(pkt, 0, m_rx, BGP_MARKER_SIZE) != BGP_MARKER_SIZE)
		return false;

	if (memcmp(m_correct, m_rx, BGP_MARKER_SIZE) != 0) {
		bgp_notify_io_invalid(peer, BGP_NOTIFY_HEADER_ERR,
				      BGP_NOTIFY_HEADER_NOT_SYNC, NULL, 0);
		return false;
	}

	/* Get size and type in network byte order. */
	ringbuf_peek(pkt, BGP_MARKER_SIZE, &size, sizeof(size));
	ringbuf_peek(pkt, BGP_MARKER_SIZE + 2, &type, sizeof(type));

	size = ntohs(size);

	/* BGP type check. */
	if (type != BGP_MSG_OPEN && type != BGP_MSG_UPDATE
	    && type != BGP_MSG_NOTIFY && type != BGP_MSG_KEEPALIVE
	    && type != BGP_MSG_ROUTE_REFRESH_NEW
	    && type != BGP_MSG_ROUTE_REFRESH_OLD
	    && type != BGP_MSG_CAPABILITY) {
		if (bgp_debug_neighbor_events(peer))
			zlog_debug("%s unknown message type 0x%02x", peer->host,
				   type);

		bgp_notify_io_invalid(peer, BGP_NOTIFY_HEADER_ERR,
				      BGP_NOTIFY_HEADER_BAD_MESTYPE, &type, 1);
		return false;
	}

	/* Minimum packet length check. */
	if ((size < BGP_HEADER_SIZE) || (size > peer->max_packet_size)
	    || (type == BGP_MSG_OPEN && size < BGP_MSG_OPEN_MIN_SIZE)
	    || (type == BGP_MSG_UPDATE && size < BGP_MSG_UPDATE_MIN_SIZE)
	    || (type == BGP_MSG_NOTIFY && size < BGP_MSG_NOTIFY_MIN_SIZE)
	    || (type == BGP_MSG_KEEPALIVE && size != BGP_MSG_KEEPALIVE_MIN_SIZE)
	    || (type == BGP_MSG_ROUTE_REFRESH_NEW
		&& size < BGP_MSG_ROUTE_REFRESH_MIN_SIZE)
	    || (type == BGP_MSG_ROUTE_REFRESH_OLD
		&& size < BGP_MSG_ROUTE_REFRESH_MIN_SIZE)
	    || (type == BGP_MSG_CAPABILITY
		&& size < BGP_MSG_CAPABILITY_MIN_SIZE)) {
		if (bgp_debug_neighbor_events(peer)) {
			zlog_debug("%s bad message length - %d for %s",
				   peer->host, size,
				   type == 128 ? "ROUTE-REFRESH"
					       : bgp_type_str[(int)type]);
		}

		uint16_t nsize = htons(size);

		bgp_notify_io_invalid(peer, BGP_NOTIFY_HEADER_ERR,
				      BGP_NOTIFY_HEADER_BAD_MESLEN,
				      (unsigned char *)&nsize, 2);
		return false;
	}

	return true;
}
