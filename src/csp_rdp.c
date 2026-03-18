/*
 * This is a implementation of the seq/ack handling taken from the Reliable Datagram Protocol (RDP)
 * For more information read RFC 908/1151. The implementation has been extended to include support for
 * delayed acknowledgments, to improve performance over half-duplex links.
 */

#include "csp_rdp_queue.h"

#include <stdlib.h>
#include <string.h>
#include <endian.h>

#include <csp/csp.h>
#include <csp/csp_debug.h>
#include <csp/csp_error.h>
#include "csp_macro.h"
#include <csp/arch/csp_queue.h>
#include <csp/arch/csp_time.h>

#include "csp_port.h"
#include "csp_conn.h"
#include "csp_io.h"
#include "csp_semaphore.h"

#define RDP_SYN 0x08
#define RDP_ACK 0x04
#define RDP_EAK 0x02
#define RDP_RST 0x01

#ifndef CSP_USE_RDP_FAST_CLOSE
#define CSP_USE_RDP_FAST_CLOSE 1
#endif



static uint32_t csp_rdp_window_size = 4;
static uint32_t csp_rdp_conn_timeout = 10000;
static uint32_t csp_rdp_packet_timeout = 1000;
static uint32_t csp_rdp_delayed_acks = 1;
static uint32_t csp_rdp_ack_timeout = 1000 / 4;
static uint32_t csp_rdp_ack_delay_count = 4 / 2;

typedef struct __packed {
	uint8_t flags;
	uint16_t seq_nr;
	uint16_t ack_nr;
} rdp_header_t;

static int csp_rdp_close_internal(csp_conn_t * conn, uint8_t closed_by, bool send_rst);

static rdp_header_t * csp_rdp_header_add(csp_packet_t * packet) {
	rdp_header_t * header;
	if ((packet->length + sizeof(*header)) > sizeof(packet->data)) {
		return NULL;
	}
	header = (rdp_header_t *)&packet->data[packet->length];
	packet->length += sizeof(*header);
	memset(header, 0, sizeof(*header));
	return header;
}

static rdp_header_t * csp_rdp_header_remove(csp_packet_t * packet) {
	rdp_header_t * header = (rdp_header_t *)&packet->data[packet->length - sizeof(*header)];
	packet->length -= sizeof(*header);
	return header;
}

static rdp_header_t * csp_rdp_header_ref(csp_packet_t * packet) {
	rdp_header_t * header = (rdp_header_t *)&packet->data[packet->length - sizeof(*header)];
	return header;
}

static inline int csp_rdp_seq_between(uint16_t seq, uint16_t start, uint16_t end) {
	return (uint16_t)(end - start) >= (uint16_t)(seq - start);
}

static inline int csp_rdp_seq_before(uint16_t seq, uint16_t cmp) {
	return (int16_t)(seq - cmp) < 0;
}

static inline int csp_rdp_seq_after(uint16_t seq, uint16_t cmp) {
	return csp_rdp_seq_before(cmp, seq);
}

static inline int csp_rdp_time_before(uint32_t time, uint32_t cmp) {
	return (int32_t)(time - cmp) < 0;
}

static inline int csp_rdp_time_after(uint32_t time, uint32_t cmp) {
	return csp_rdp_time_before(cmp, time);
}

static int csp_rdp_send_cmp(csp_conn_t * conn, csp_packet_t * packet, int flags, int seq_nr, int ack_nr) {

	if (!packet) {
		packet = csp_buffer_get(0);
		if (!packet)
			return CSP_ERR_NOMEM;
		packet->length = 0;
	}

	if (flags & RDP_ACK) {
		conn->rdp.rcv_lsa = ack_nr;
	}

	conn->rdp.ack_timestamp = csp_get_ms();

	rdp_header_t * header = csp_rdp_header_add(packet);
	if (header == NULL) {
		csp_rdp_error("RDP %p: No space for RDP header (cmp)", (void *)conn);
		csp_buffer_free(packet);
		return CSP_ERR_NOMEM;
	}
	header->seq_nr = htobe16(seq_nr);
	header->ack_nr = htobe16(ack_nr);

	static uint8_t csp_rdp_incr = 0;
	header->flags |= csp_rdp_incr++ << 4 | flags;

	if (flags & RDP_SYN) {
		csp_packet_t * rdp_packet = csp_buffer_clone(packet);
		if (rdp_packet == NULL) return CSP_ERR_NOMEM;
		rdp_packet->timestamp_tx = csp_get_ms();
		csp_rdp_queue_tx_add(conn, rdp_packet);
	}

	csp_id_t idout = conn->idout;
	idout.pri = conn->idout.pri < CSP_PRIO_HIGH ? conn->idout.pri : CSP_PRIO_HIGH;

	csp_rdp_protocol("RDP %p: Send CMP S %u: syn %u, ack %u, eack %u, rst %u, seq_nr %5u, ack_nr %5u, packet_len %u (%u)\n",
					 (void *)conn, conn->rdp.state,
					 ((header->flags & RDP_SYN) != 0), ((header->flags & RDP_ACK) != 0),
					 ((header->flags & RDP_EAK) != 0), ((header->flags & RDP_RST) != 0),
					 be16toh(header->seq_nr), be16toh(header->ack_nr),
					 packet->length, (unsigned int)(packet->length - sizeof(rdp_header_t)));

	csp_send_direct(&idout, packet, NULL);

	return CSP_ERR_NONE;
}

static int csp_rdp_send_syn(csp_conn_t * conn) {

	csp_packet_t * packet = csp_buffer_get(0);
	if (packet == NULL) return CSP_ERR_NOMEM;

	packet->data32[0] = htobe32(csp_rdp_window_size);
	packet->data32[1] = htobe32(csp_rdp_conn_timeout);
	packet->data32[2] = htobe32(csp_rdp_packet_timeout);
	packet->data32[3] = htobe32(csp_rdp_delayed_acks);
	packet->data32[4] = htobe32(csp_rdp_ack_timeout);
	packet->data32[5] = htobe32(csp_rdp_ack_delay_count);
	packet->length = 6 * sizeof(uint32_t);

	return csp_rdp_send_cmp(conn, packet, RDP_SYN, conn->rdp.snd_iss, 0);
}

static inline int csp_rdp_receive_data(csp_conn_t * conn, csp_packet_t * packet) {

	csp_rdp_header_remove(packet);

	if (csp_conn_enqueue_packet(conn, packet) != CSP_ERR_NONE) {
		csp_dbg_conn_ovf++;
		csp_rdp_error("RDP %p: Conn RX buffer full\n", (void *)conn);
		return CSP_ERR_NOBUFS;
	}

	return CSP_ERR_NONE;
}

static inline void csp_rdp_rx_queue_flush(csp_conn_t * conn) {

	int i, count;
	csp_packet_t * packet;

front:
	count = csp_rdp_queue_rx_size();
	for (i = 0; i < count; i++) {

		if (csp_queue_free(conn->rx_queue) <= 2)
			return;

		packet = csp_rdp_queue_rx_get(conn);
		if (packet == NULL) {
			break;
		}

		rdp_header_t * header = csp_rdp_header_ref(packet);

		if (header->seq_nr == (uint16_t)(conn->rdp.rcv_cur + 1)) {
			csp_rdp_protocol("RDP %p: Deliver seq %u\n", (void *)conn, header->seq_nr);
			if (csp_rdp_receive_data(conn, packet) != CSP_ERR_NONE) {
				csp_rdp_error("RDP lost packet internally, stream corrupted!\n");
				csp_buffer_free(packet);
			}
			conn->rdp.rcv_cur++;
			goto front;
		} else {
			csp_rdp_queue_rx_add(conn, packet);
		}
	}
}

static inline bool csp_rdp_seq_in_rx_queue(csp_conn_t * conn, uint16_t seq_nr) {

	int i, count;
	csp_packet_t * packet;
	count = csp_rdp_queue_rx_size();
	for (i = 0; i < count; i++) {

		packet = csp_rdp_queue_rx_get(conn);
		if (packet == NULL) {
			break;
		}

		csp_rdp_queue_rx_add(conn, packet);

		rdp_header_t * header = csp_rdp_header_ref((csp_packet_t *)packet);
		if (header->seq_nr == seq_nr) {
			return true;
		}
	}

	return false;
}

static inline int csp_rdp_rx_queue_add(csp_conn_t * conn, csp_packet_t * packet, uint16_t seq_nr) {

	if (csp_rdp_seq_in_rx_queue(conn, seq_nr)) {
		csp_rdp_protocol("RDP %p: Already exists in RX queue %u\n", (void *)conn, seq_nr);
		return CSP_ERR_USED;
	}
	csp_rdp_protocol("RDP %p: Add to RX queue %u\n", (void *) conn, seq_nr);
	csp_rdp_queue_rx_add(conn, packet);
	return CSP_ERR_NONE;
}


static inline bool csp_rdp_should_ack(csp_conn_t * conn) {

	/*
	 * FIX gratuitous ACK storm:
	 *
	 * Do not send an ACK when rcv_cur == rcv_lsa — there is nothing new
	 * to acknowledge. Without this guard, the periodic csp_rdp_check_ack
	 * call from csp_rdp_check_timeouts fires every N ms even when the
	 * receiver has no new data, flooding the half-duplex RF channel.
	 *
	 * This guard works correctly with both delayed_acks=0 and =1:
	 *   - After an ACK is sent via csp_rdp_send_cmp, rcv_lsa is updated
	 *     to rcv_cur, so subsequent periodic calls return false until
	 *     new data arrives.
	 *   - For duplicates, the ACK is sent directly via csp_rdp_send_cmp
	 *     (bypassing this function), so duplicates always get a re-ACK.
	 */
	if (conn->rdp.rcv_cur == conn->rdp.rcv_lsa) {
		return false;
	}

	/* If delayed ACKs are not used, send immediately */
	if (!conn->rdp.delayed_acks) {
		return true;
	}

	/* Delayed ACK: wait ack_timeout ms since last data reception */
	uint32_t time_now = csp_get_ms();
	if (csp_rdp_time_after(time_now, conn->rdp.ack_timestamp + conn->rdp.ack_timeout))
		return true;

	/* ACK if unacknowledged count exceeds delay count */
	if (csp_rdp_seq_after(conn->rdp.rcv_cur, conn->rdp.rcv_lsa + conn->rdp.ack_delay_count))
		return true;

	return false;
}

int csp_rdp_check_ack(csp_conn_t * conn) {

	if (CSP_CONN_RXQUEUE_LEN - csp_queue_size(conn->rx_queue) <= 2 * (int32_t)conn->rdp.window_size) {
		return CSP_ERR_NONE;
	}

	if (csp_rdp_should_ack(conn)) {
		csp_rdp_send_cmp(conn, NULL, RDP_ACK, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
	}

	return CSP_ERR_NONE;
}

static inline bool csp_rdp_is_conn_ready_for_tx(csp_conn_t * conn) {
	if (csp_rdp_seq_after(conn->rdp.snd_nxt, conn->rdp.snd_una + conn->rdp.window_size - 1)) {
		return false;
	}
	return true;
}

void csp_rdp_check_timeouts(csp_conn_t * conn) {

	const uint32_t time_now = csp_get_ms();

	if (conn->dest_socket != NULL) {
		if (csp_rdp_time_after(time_now, conn->timestamp + conn->rdp.conn_timeout)) {
			csp_rdp_error("RDP %p: Found a lost connection (now: %" PRIu32 ", ts: %" PRIu32 ", to: %" PRIu32 "), closing\n",
						  (void *)conn, time_now, conn->timestamp, conn->rdp.conn_timeout);
			csp_conn_close(conn, CSP_RDP_CLOSED_BY_USERSPACE | CSP_RDP_CLOSED_BY_PROTOCOL | CSP_RDP_CLOSED_BY_TIMEOUT);
			return;
		}
	}

	if (conn->rdp.state == RDP_CLOSE_WAIT) {
		if (csp_rdp_time_after(time_now, conn->timestamp + conn->rdp.conn_timeout)) {
			csp_conn_close(conn, CSP_RDP_CLOSED_BY_PROTOCOL | CSP_RDP_CLOSED_BY_TIMEOUT);
			return;
		}
	}

	int count = csp_rdp_queue_tx_size();
	for (int i = 0; i < count; i++) {

		csp_packet_t * packet;
		packet = csp_rdp_queue_tx_get(conn);
		if (packet == NULL) {
			break;
		}

		rdp_header_t * header = csp_rdp_header_ref((csp_packet_t *)packet);

		if (csp_rdp_seq_before(be16toh(header->seq_nr), conn->rdp.snd_una)) {
			csp_rdp_protocol("RDP %p: TX Element Free, time %" PRIu32 ", seq %u, una %u\n",
							 (void *)conn, packet->timestamp_tx, be16toh(header->seq_nr), conn->rdp.snd_una);
			csp_buffer_free(packet);
			continue;
		}

		if (csp_rdp_time_after(time_now, packet->timestamp_tx + conn->rdp.packet_timeout)) {
			csp_packet_t * new_packet = csp_buffer_get(0);
			if (new_packet) {
				csp_rdp_protocol("RDP %p: TX Element timed out, retransmitting seq %u\n",
								 (void *)conn, be16toh(header->seq_nr));

				header->ack_nr = htobe16(conn->rdp.rcv_cur);
				conn->rdp.ack_timestamp = csp_get_ms();
				packet->timestamp_tx = csp_get_ms();
				csp_buffer_copy(packet, new_packet);
				csp_send_direct(&conn->idout, new_packet, NULL);
			} else {
				csp_rdp_error("RDP %p: Failed to allocate packet buffer\n", (void *)conn);
			}
		}

		csp_rdp_queue_tx_add(conn, packet);

	}

	if (conn->rdp.state == RDP_OPEN) {

		/*
		 * Periodic ACK check. csp_rdp_should_ack prevents sending when
		 * rcv_cur == rcv_lsa (nothing new to acknowledge).
		 * With delayed_acks=0 this fires whenever new data is pending.
		 * With delayed_acks=1 this fires after ack_timeout ms.
		 */
		csp_rdp_check_ack(conn);

		if (csp_rdp_is_conn_ready_for_tx(conn)) {
			csp_bin_sem_post(&conn->rdp.tx_wait);
		}
	}

	csp_rdp_rx_queue_flush(conn);

}

bool csp_rdp_new_packet(csp_conn_t * conn, csp_packet_t * packet) {

	bool close_connection = false;

	rdp_header_t * rx_header = csp_rdp_header_ref(packet);
	rx_header->ack_nr = be16toh(rx_header->ack_nr);
	rx_header->seq_nr = be16toh(rx_header->seq_nr);

	uint8_t closed_by = CSP_RDP_CLOSED_BY_PROTOCOL;

	csp_rdp_protocol(
		"RDP %p: Received in S %u: syn %u, ack %u, eack %u, "
		"rst %u, seq_nr %5u, ack_nr %5u, packet_len %u (%u)\n",
		(void *)conn, conn->rdp.state,
		((rx_header->flags & RDP_SYN) != 0), ((rx_header->flags & RDP_ACK) != 0),
		((rx_header->flags & RDP_EAK) != 0), ((rx_header->flags & RDP_RST) != 0),
		rx_header->seq_nr, rx_header->ack_nr,
		packet->length, (unsigned int)(packet->length - sizeof(rdp_header_t)));

	if (rx_header->flags & RDP_RST) {

		if (rx_header->flags & RDP_ACK) {
			conn->rdp.snd_una = rx_header->ack_nr + 1;
		}

		if (conn->rdp.state == RDP_CLOSED) {
			csp_rdp_protocol("RDP %p: RST received in CLOSED - ignored\n", (void *)conn);
			close_connection = (conn->dest_socket != NULL);
			goto discard_open;
		}

		if (conn->rdp.state == RDP_CLOSE_WAIT) {
			csp_rdp_protocol("RDP %p: RST received in CLOSE_WAIT, ack: %d - closing\n",
							 (void *)conn, (rx_header->flags & RDP_ACK));
			if ((rx_header->flags & RDP_ACK) && CSP_USE_RDP_FAST_CLOSE) {
				closed_by |= CSP_RDP_CLOSED_BY_TIMEOUT;
			}
			goto discard_close;
		}

		if (rx_header->seq_nr == (uint16_t)(conn->rdp.rcv_cur + 1)) {
			csp_rdp_protocol("RDP %p: Received RST in sequence, no more data incoming, reply with RST\n", (void *)conn);
			conn->rdp.state = RDP_CLOSE_WAIT;
			conn->timestamp = csp_get_ms();
			csp_rdp_send_cmp(conn, NULL, RDP_ACK | RDP_RST, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
			if (CSP_USE_RDP_FAST_CLOSE) {
				closed_by |= CSP_RDP_CLOSED_BY_TIMEOUT;
			}
			goto discard_close;
		}

		csp_rdp_protocol("RDP %p: RST out of sequence, keep connection open\n", (void *)conn);
		goto discard_open;
	}

	switch (conn->rdp.state) {

		case RDP_CLOSED: {

			uint8_t rx_header_flags = rx_header->flags & 0x0f;

			if (rx_header_flags != RDP_SYN) {
				csp_rdp_protocol("RDP %p: Not SYN received in CLOSED state. Discarding packet\n", (void *)conn);
				csp_rdp_send_cmp(conn, NULL, RDP_RST, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
				goto discard_close;
			}

			csp_rdp_protocol("RDP %p: SYN-Received\n", (void *)conn);

			unsigned int seed = csp_get_ms();
			conn->rdp.snd_iss = (uint16_t)rand_r(&seed);
			conn->rdp.snd_nxt = conn->rdp.snd_iss + 1;
			conn->rdp.snd_una = conn->rdp.snd_iss;

			conn->rdp.rcv_cur = rx_header->seq_nr;
			conn->rdp.rcv_irs = rx_header->seq_nr;
			conn->rdp.rcv_lsa = rx_header->seq_nr;

			conn->rdp.window_size = be32toh(packet->data32[0]);
			conn->rdp.conn_timeout = be32toh(packet->data32[1]);
			conn->rdp.packet_timeout = be32toh(packet->data32[2]);
			conn->rdp.delayed_acks = be32toh(packet->data32[3]);
			conn->rdp.ack_timeout = be32toh(packet->data32[4]);
			conn->rdp.ack_delay_count = be32toh(packet->data32[5]);
			csp_rdp_protocol("RDP %p: window size %" PRIu32 ", conn timeout %" PRIu32 ", packet timeout %" PRIu32 ", delayed acks: %" PRIu32 ", ack timeout %" PRIu32 ", ack each %" PRIu32 " packet\n",
							 (void *)conn, conn->rdp.window_size, conn->rdp.conn_timeout, conn->rdp.packet_timeout,
							 conn->rdp.delayed_acks, conn->rdp.ack_timeout, conn->rdp.ack_delay_count);

			conn->rdp.state = RDP_SYN_RCVD;

			csp_rdp_send_cmp(conn, NULL, RDP_ACK | RDP_SYN, conn->rdp.snd_iss, conn->rdp.rcv_irs);

			goto discard_open;

		} break;

		case RDP_SYN_SENT: {

			if ((rx_header->flags & RDP_SYN) && (rx_header->flags & RDP_ACK)) {

				conn->rdp.rcv_cur = rx_header->seq_nr;
				conn->rdp.rcv_irs = rx_header->seq_nr;
				conn->rdp.rcv_lsa = rx_header->seq_nr;  /* equal to rcv_cur: nothing pending ACK */
				conn->rdp.snd_una = rx_header->ack_nr + 1;
				conn->rdp.ack_timestamp = csp_get_ms();
				conn->rdp.state = RDP_OPEN;

				csp_rdp_protocol("RDP %p: NP: Connection OPEN\n", (void *)conn);

				csp_rdp_send_cmp(conn, NULL, RDP_ACK, conn->rdp.snd_nxt, conn->rdp.rcv_cur);

				csp_rdp_protocol("RDP %p: Wake Tx task (ack)\n", (void *)conn);
				csp_bin_sem_post(&conn->rdp.tx_wait);

				goto discard_open;
			}

			if ((rx_header->flags & RDP_ACK)) {
				csp_rdp_error("RDP %p: Half-open connection found, send RST and wake Tx task\n", (void *)conn);
				csp_rdp_send_cmp(conn, NULL, RDP_RST, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
				csp_bin_sem_post(&conn->rdp.tx_wait);

				goto discard_open;
			}

			csp_rdp_error("RDP %p: Invalid reply to SYN request\n", (void *)conn);
			goto discard_close;

		} break;

		case RDP_SYN_RCVD:
		case RDP_OPEN: {

			/* SYN or !ACK is invalid */
			if ((rx_header->flags & RDP_SYN) || !(rx_header->flags & RDP_ACK)) {
				if (rx_header->seq_nr != conn->rdp.rcv_irs) {
					if (conn->rdp.state == RDP_OPEN) {
						/*
						 * FIX Bug-8: SYN zombie from a previous failed connection.
						 * In OPEN state, drop silently — do not reset the active
						 * connection.
						 */
						csp_rdp_protocol("RDP %p: Stale SYN in OPEN state (seq=%u), dropping zombie\n",
										 (void *)conn, rx_header->seq_nr);
						goto discard_open;
					}
					csp_rdp_error("RDP %p: Invalid SYN or no ACK, resetting!\n", (void *)conn);
					goto discard_close;
				} else {
					csp_rdp_protocol("RDP %p: Ignoring duplicate SYN packet!\n", (void *)conn);
					goto discard_open;
				}
			}

			/* Check sequence number */
			if (!csp_rdp_seq_between(rx_header->seq_nr, conn->rdp.rcv_cur + 1, conn->rdp.rcv_cur + (conn->rdp.window_size * 2))) {
				csp_rdp_protocol("RDP %p: Invalid sequence number! %u not between %u and %" PRIu32"\n",
								 (void *)conn, rx_header->seq_nr, conn->rdp.rcv_cur + 1U, conn->rdp.rcv_cur + (conn->rdp.window_size * 2U));

				if (conn->rdp.state == RDP_SYN_RCVD) {
					csp_rdp_send_cmp(conn, NULL, RDP_ACK | RDP_SYN, conn->rdp.snd_iss, conn->rdp.rcv_irs);

				} else if (conn->rdp.state == RDP_OPEN &&
				           csp_rdp_seq_before(rx_header->seq_nr, conn->rdp.rcv_cur + 1)) {
					/*
					 * FIX Bug-A: duplicate data packet — already processed.
					 *
					 * Send ACK immediately via csp_rdp_send_cmp, bypassing
					 * csp_rdp_should_ack. The sender just finished transmitting
					 * this retransmission, so its TNC is now releasing PTT.
					 * Our immediate ACK is sent within a few ms and travels
					 * over RF while the sender's TNC completes PTT release.
					 * By the time our ACK frame ends (~40ms at 9600 baud),
					 * the sender's TNC is in RX mode and can receive it.
					 */
					csp_rdp_protocol("RDP %p: Duplicate data seq %u, re-ACKing (seq %u)\n",
									 (void *)conn, rx_header->seq_nr, conn->rdp.rcv_cur);
					csp_rdp_send_cmp(conn, NULL, RDP_ACK, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
				}
				goto discard_open;
			}

			/* Check ACK number */
			if (!csp_rdp_seq_between(rx_header->ack_nr, conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2), conn->rdp.snd_nxt - 1)) {
				csp_rdp_error("RDP %p: Invalid ACK number! %u not between %" PRIu32 " and %u\n",
							  (void *)conn, rx_header->ack_nr, conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2), conn->rdp.snd_nxt - 1);
				goto discard_open;
			}

			/* Check SYN_RCVD ACK */
			if (conn->rdp.state == RDP_SYN_RCVD) {
				if (rx_header->ack_nr != conn->rdp.snd_iss) {
					csp_rdp_error("RDP %p: SYN-RCVD: Wrong ACK number\n", (void *)conn);
					goto discard_close;
				}
				csp_rdp_protocol("RDP %p: NC: Connection OPEN\n", (void *)conn);
				conn->rdp.state = RDP_OPEN;

				if (conn->dest_socket != NULL) {

					if (csp_queue_enqueue(conn->dest_socket->rx_queue, &conn, 0) == CSP_QUEUE_ERROR) {
						csp_rdp_error("RDP %p: ERROR socket cannot accept more connections\n", (void *)conn);
						goto discard_close;
					}

					conn->dest_socket = NULL;
				}
			}

			if (conn->dest_socket == NULL) {
				conn->timestamp = csp_get_ms();
			}

			conn->rdp.snd_una = rx_header->ack_nr + 1;

			if ((rx_header->flags & RDP_EAK)) {
				csp_rdp_protocol("RDP %p: Got EACK\n", (void *)conn);
				goto discard_open;
			}

			if (packet->length <= sizeof(rdp_header_t))
				goto discard_open;

			if (rx_header->seq_nr != (uint16_t)(conn->rdp.rcv_cur + 1)) {
				if (csp_rdp_rx_queue_add(conn, packet, rx_header->seq_nr) != CSP_ERR_NONE) {
					csp_rdp_check_ack(conn);
					goto discard_open;
				}
				goto accepted_open;
			}

			uint16_t seq_nr = rx_header->seq_nr;

			if (csp_rdp_receive_data(conn, packet) != CSP_ERR_NONE)
				goto discard_open;

			conn->rdp.rcv_cur = seq_nr;

			/*
			 * Reset ack_timestamp to now so the delay timer (when
			 * delayed_acks=1) measures from this reception event.
			 * With delayed_acks=0, csp_rdp_should_ack will return true
			 * immediately (rcv_cur > rcv_lsa, no delay needed).
			 */
			conn->rdp.ack_timestamp = csp_get_ms();

			csp_rdp_check_ack(conn);

			csp_rdp_rx_queue_flush(conn);

			goto accepted_open;

		} break;

		case RDP_CLOSE_WAIT:

			if ((rx_header->flags & RDP_SYN) || !(rx_header->flags & RDP_ACK)) {
				csp_rdp_protocol("RDP %p: Invalid SYN or no ACK in CLOSE-WAIT\n", (void *)conn);
				goto discard_open;
			}

			if (!csp_rdp_seq_between(rx_header->ack_nr, conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2), conn->rdp.snd_nxt - 1)) {
				csp_rdp_error("RDP %p: Invalid ACK number! %u not between %" PRIu32 " and %u\n",
							  (void *)conn, rx_header->ack_nr, conn->rdp.snd_una - 1 - (conn->rdp.window_size * 2), conn->rdp.snd_nxt - 1);
				goto discard_open;
			}

			conn->rdp.snd_una = rx_header->ack_nr + 1;

			csp_rdp_send_cmp(conn, NULL, RDP_ACK | RDP_RST, conn->rdp.snd_nxt, conn->rdp.rcv_cur);

			goto discard_open;

		default:
			csp_rdp_error("RDP %p: ERROR default state!\n", (void *)conn);
			goto discard_close;
	}

discard_close:
	if (conn->dest_socket == NULL) {
		csp_conn_close(conn, closed_by);
		csp_conn_enqueue_packet(conn, NULL);
	} else {
		csp_conn_close(conn, closed_by | CSP_RDP_CLOSED_BY_USERSPACE);
	}

discard_open:
	csp_buffer_free(packet);
accepted_open:
	return close_connection;
}

int csp_rdp_connect(csp_conn_t * conn) {

	int retry = 1;

	conn->rdp.window_size = csp_rdp_window_size;
	conn->rdp.conn_timeout = csp_rdp_conn_timeout;
	conn->rdp.packet_timeout = csp_rdp_packet_timeout;
	conn->rdp.delayed_acks = csp_rdp_delayed_acks;
	conn->rdp.ack_timeout = csp_rdp_ack_timeout;
	conn->rdp.ack_delay_count = csp_rdp_ack_delay_count;
	conn->rdp.ack_timestamp = csp_get_ms();

retry:
	csp_rdp_protocol("RDP %p: Active connect, conn state %u\n", (void *)conn, conn->rdp.state);

	if (conn->rdp.state == RDP_OPEN) {
		csp_rdp_error("RDP %p: Connection already open\n", (void *)conn);
		return CSP_ERR_ALREADY;
	}

	unsigned int seed = csp_get_ms();
	conn->rdp.snd_iss = (uint16_t)rand_r(&seed);
	conn->rdp.snd_nxt = conn->rdp.snd_iss + 1;
	conn->rdp.snd_una = conn->rdp.snd_iss;

	csp_rdp_protocol("RDP %p: AC: Sending SYN\n", (void *)conn);

	csp_bin_sem_wait(&conn->rdp.tx_wait, 0);

	conn->rdp.state = RDP_SYN_SENT;
	if (csp_rdp_send_syn(conn) != CSP_ERR_NONE)
		goto error;

	csp_rdp_protocol("RDP %p: AC: Waiting for SYN/ACK reply...\n", (void *)conn);
	int result = csp_bin_sem_wait(&conn->rdp.tx_wait, conn->rdp.conn_timeout);

	if (result == CSP_SEMAPHORE_OK) {
		if (conn->rdp.state == RDP_OPEN) {
			csp_rdp_protocol("RDP %p: AC: Connection OPEN\n", (void *)conn);
			return CSP_ERR_NONE;
		}
		if (conn->rdp.state == RDP_SYN_SENT) {
			if (retry) {
				csp_rdp_error("RDP %p: Half-open connection detected, RST sent, now retrying\n", (void *)conn);
				csp_rdp_queue_flush(conn);
				retry = 0;
				goto retry;
			}
			csp_rdp_error("RDP %p: Connection stayed half-open, even after RST and retry!\n", (void *)conn);
			goto error;
		}
	}

error:
	csp_rdp_protocol("RDP %p: AC: Connection Failed\n", (void *)conn);
	csp_rdp_close_internal(conn, CSP_RDP_CLOSED_BY_PROTOCOL, false);
	return CSP_ERR_TIMEDOUT;
}

int csp_rdp_send(csp_conn_t * conn, csp_packet_t * packet) {

	if (conn->rdp.state != RDP_OPEN) {
		csp_rdp_error("RDP %p: ERROR cannot send, connection not open (%d)\n", (void *)conn, conn->rdp.state);
		return CSP_ERR_RESET;
	}

	while (1) {
		if (conn->rdp.state == RDP_CLOSE_WAIT || conn->rdp.state == RDP_CLOSED) {
			csp_rdp_error("RDP %p: ERROR cannot send, connection closed by peer or timeout\n", (void *)conn);
			return CSP_ERR_RESET;
		}
		if (csp_rdp_is_conn_ready_for_tx(conn) == true)
			break;
		csp_rdp_protocol("RDP %p: Waiting for window update before sending seq %u\n", (void *)conn, conn->rdp.snd_nxt);
		csp_bin_sem_wait(&conn->rdp.tx_wait, conn->rdp.conn_timeout);
	}

	rdp_header_t * tx_header = csp_rdp_header_add(packet);
	if (tx_header == NULL) {
		csp_rdp_error("RDP %p: No space for RDP header (send)\n", (void *)conn);
		return CSP_ERR_NOMEM;
	}
	tx_header->ack_nr = htobe16(conn->rdp.rcv_cur);
	tx_header->seq_nr = htobe16(conn->rdp.snd_nxt);
	tx_header->flags |= RDP_ACK;

	csp_packet_t * rdp_packet = csp_buffer_clone(packet);
	if (rdp_packet == NULL) {
		csp_rdp_error("RDP %p: Failed to allocate packet buffer\n", (void *)conn);
		return CSP_ERR_NOMEM;
	}

	rdp_packet->timestamp_tx = csp_get_ms();
	csp_rdp_queue_tx_add(conn, rdp_packet);

	csp_rdp_protocol(
		"RDP %p: Sending  in S %u: syn %u, ack %u, eack %u, "
		"rst %u, seq_nr %5u, ack_nr %5u, packet_len %u (%u)\n",
		(void *)conn, conn->rdp.state,
		(tx_header->flags & RDP_SYN), (tx_header->flags & RDP_ACK),
		(tx_header->flags & RDP_EAK), (tx_header->flags & RDP_RST),
		be16toh(tx_header->seq_nr), be16toh(tx_header->ack_nr),
		packet->length, (unsigned int)(packet->length - sizeof(rdp_header_t)));

	conn->rdp.snd_nxt++;
	conn->rdp.ack_timestamp = csp_get_ms();
	return CSP_ERR_NONE;
}

void csp_rdp_init(csp_conn_t * conn) {

	conn->rdp.state = RDP_CLOSED;
	conn->rdp.closed_by = 0;
	conn->rdp.conn_timeout = csp_rdp_conn_timeout;
	conn->rdp.packet_timeout = csp_rdp_packet_timeout;

	csp_bin_sem_init(&conn->rdp.tx_wait);

}

int csp_rdp_close(csp_conn_t * conn, uint8_t closed_by) {
	return csp_rdp_close_internal(conn, closed_by, true);
}

static int csp_rdp_close_internal(csp_conn_t * conn, uint8_t closed_by, bool send_rst) {

	if (conn->rdp.state == RDP_CLOSED) {
		return CSP_ERR_NONE;
	}

	conn->rdp.closed_by |= closed_by;

	if (conn->rdp.state != RDP_CLOSE_WAIT) {
		conn->rdp.state = RDP_CLOSE_WAIT;
		conn->timestamp = csp_get_ms();
		if (send_rst) {
			csp_rdp_send_cmp(conn, NULL, RDP_ACK | RDP_RST, conn->rdp.snd_nxt, conn->rdp.rcv_cur);
		}
		csp_rdp_protocol("RDP %p: csp_rdp_close(0x%x)%s -> CLOSE_WAIT\n", (void *)conn, closed_by, send_rst ? ", sent RST" : "");
		/* FIX zombie TX: flush TX queue immediately on CLOSE_WAIT transition.
		 * The TX queue is global — packets from this connection must be
		 * removed NOW so they don't get retransmitted by future connections
		 * while this connection waits in CLOSE_WAIT. */
		csp_rdp_queue_flush(conn);
		csp_bin_sem_post(&conn->rdp.tx_wait);
	}

	if (conn->rdp.closed_by != CSP_RDP_CLOSED_BY_ALL) {
		csp_rdp_protocol("RDP %p: csp_rdp_close(0x%x) != %x, waiting for:%s%s%s\n",
						 (void *)conn, closed_by, conn->rdp.closed_by,
						 (conn->rdp.closed_by & CSP_RDP_CLOSED_BY_USERSPACE) ? "" : " userspace",
						 (conn->rdp.closed_by & CSP_RDP_CLOSED_BY_PROTOCOL) ? "" : " protocol",
						 (conn->rdp.closed_by & CSP_RDP_CLOSED_BY_TIMEOUT) ? "" : " timeout");
		return CSP_ERR_AGAIN;
	}

	csp_rdp_protocol("RDP %p: csp_rdp_close(0x%x) -> CLOSED\n", (void *)conn, closed_by);
	conn->rdp.state = RDP_CLOSED;
	conn->rdp.closed_by = 0;
	/*
	 * FIX Bug-B: flush TX queue to prevent zombie retransmissions from this
	 * connection from appearing in future connections (TX queue is global).
	 */
	csp_rdp_queue_flush(conn);
	return CSP_ERR_NONE;
}

void csp_rdp_set_opt(unsigned int window_size, unsigned int conn_timeout_ms,
					 unsigned int packet_timeout_ms, unsigned int delayed_acks,
					 unsigned int ack_timeout, unsigned int ack_delay_count) {
	csp_rdp_window_size = window_size;
	csp_rdp_conn_timeout = conn_timeout_ms;
	csp_rdp_packet_timeout = packet_timeout_ms;
	csp_rdp_delayed_acks = delayed_acks;
	csp_rdp_ack_timeout = ack_timeout;
	csp_rdp_ack_delay_count = ack_delay_count;
}

void csp_rdp_get_opt(unsigned int * window_size, unsigned int * conn_timeout_ms,
					 unsigned int * packet_timeout_ms, unsigned int * delayed_acks,
					 unsigned int * ack_timeout, unsigned int * ack_delay_count) {

	if (window_size)
		*window_size = csp_rdp_window_size;
	if (conn_timeout_ms)
		*conn_timeout_ms = csp_rdp_conn_timeout;
	if (packet_timeout_ms)
		*packet_timeout_ms = csp_rdp_packet_timeout;
	if (delayed_acks)
		*delayed_acks = csp_rdp_delayed_acks;
	if (ack_timeout)
		*ack_timeout = csp_rdp_ack_timeout;
	if (ack_delay_count)
		*ack_delay_count = csp_rdp_ack_delay_count;
}

bool csp_rdp_conn_is_active(csp_conn_t *conn) {

	uint32_t time_now = csp_get_ms();
	bool active = true;

	if (csp_rdp_time_after(time_now, conn->timestamp + conn->rdp.conn_timeout)) {
		csp_rdp_error("RDP %p: Timeout no packets received last %u ms\n", (void *)conn, conn->rdp.conn_timeout);
		active = false;
	}

	if (conn->rdp.state == RDP_CLOSE_WAIT || conn->rdp.state == RDP_CLOSED) {
		active = false;
	}

	return active;

}
