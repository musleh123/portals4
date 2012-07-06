/**
 * file ptl_recv.c
 *
 * Completion queue processing.
 */
#include "ptl_loc.h"

/**
 * Receive state name for debug output.
 */
static char *recv_state_name[] = {
	[STATE_RECV_SEND_COMP]		= "send_comp",
	[STATE_RECV_RDMA_COMP]		= "rdma_comp",
	[STATE_RECV_PACKET_RDMA]	= "recv_packet_rdma",
	[STATE_RECV_PACKET]		= "recv_packet",
	[STATE_RECV_DROP_BUF]		= "recv_drop_buf",
	[STATE_RECV_REQ]		= "recv_req",
	[STATE_RECV_INIT]		= "recv_init",
	[STATE_RECV_REPOST]		= "recv_repost",
	[STATE_RECV_ERROR]		= "recv_error",
	[STATE_RECV_DONE]		= "recv_done",
};

#ifdef WITH_TRANSPORT_IB
/**
 * Poll the rdma completion queue.
 *
 * @param ni the ni that owns the cq.
 * @param num_wc the number of entries in wc_list and buf_list.
 * @param wc_list an array of work completion structs.
 * @param buf_list an array of buf pointers.
 *
 * @return the number of work completions found if no error.
 * @return a negative number if an error occured.
 */
static int comp_poll(ni_t *ni, int num_wc,
					 struct ibv_wc wc_list[], buf_t *buf_list[])
{
	int ret;
	int i;
	buf_t *buf;

	ret = ibv_poll_cq(ni->rdma.cq, num_wc, wc_list);
	if (ret <= 0)
		return 0;

	/* convert from wc to buf and set initial state */
	for (i = 0; i < ret; i++) {
		const struct ibv_wc *wc = &wc_list[i];

		buf_list[i] = buf = (buf_t *)(uintptr_t)wc->wr_id;

		if (unlikely(wc->status != IBV_WC_SUCCESS &&
					 wc->status != IBV_WC_WR_FLUSH_ERR))
			WARN();

		/* The work request id might be NULL. That can happen when an
		 * inline send completed in error and no completion was
		 * requested. */
		if (!buf)
			continue;

		buf->length = wc->byte_len;

		if (unlikely(wc->status)) {
			if (buf->type == BUF_SEND) {
				buf->ni_fail = PTL_NI_UNDELIVERABLE;
				buf->recv_state = STATE_RECV_SEND_COMP;
			} else if (buf->type == BUF_RDMA)
				buf->recv_state = STATE_RECV_ERROR;
			else
				buf->recv_state = STATE_RECV_DROP_BUF;
		} else {
			if (buf->type == BUF_SEND)
				buf->recv_state = STATE_RECV_SEND_COMP;
			else if (buf->type == BUF_RDMA)
				buf->recv_state = STATE_RECV_RDMA_COMP;
			else if (buf->type == BUF_RECV)
				buf->recv_state = STATE_RECV_PACKET_RDMA;
			else
				buf->recv_state = STATE_RECV_ERROR;
		}
	}

	return ret;
}

/**
 * Process a send completion.
 *
 * @param buf the buffer that finished.
 *
 * @return next state
 */
static int send_comp(buf_t *buf)
{
	/* If it's a completion that was not requested, then it's either
	 * coming from the send completion threshold mechanism (see
	 * conn->rdma.completion_threshold), or it was completed in
	 * error. We ignore the first type and let the second one pass
	 * through the state machine. */
	if (buf->event_mask & XX_SIGNALED ||
		buf->ni_fail == PTL_NI_UNDELIVERABLE) {
		/* Fox XI only, restart the initiator state machine. */
		struct hdr_common *hdr = (struct hdr_common *)buf->data;

		if (hdr->operation <= OP_SWAP) {
			buf->completed = 1;
			process_init(buf);
		}
		else if (hdr->operation == OP_RDMA_DISC) {
			conn_t * conn = buf->conn;

			pthread_mutex_lock(&conn->mutex);

			assert(conn->rdma.local_disc == 1);
			conn->rdma.local_disc = 2;

			/* If the remote side has already informed us of its
			 * intention to disconnect, then we can destroy that
			 * connection. */
			if (conn->rdma.remote_disc)
				disconnect_conn_locked(conn);

			pthread_mutex_unlock(&conn->mutex);
		}
	}

	buf_put(buf);

	return STATE_RECV_DONE;
}

/**
 * Process an rdma completion.
 *
 * @param rdma_buf the buffer that finished.
 *
 * @return the next state.
 */
static int rdma_comp(buf_t *rdma_buf)
{
	struct list_head temp_list;
	int err;
	buf_t *buf = rdma_buf->xxbuf;

	/* If it's a completion that was not requested, then it's coming
	 * from the send completion threshold mechanism (see
	 * conn->rdma.completion_threshold), and we ignore it. */
	if (!(rdma_buf->event_mask & XX_SIGNALED))
		return STATE_RECV_DONE;

	/* Take a ref on the XT since freeing all its rdma_buffers will also
	 * free it. */
	buf_get(buf);

	/* do not do this for indirect rdma sge lists */
	if (rdma_buf != buf) {
		atomic_dec(&buf->rdma.rdma_comp);

		PTL_FASTLOCK_LOCK(&buf->rdma_list_lock);
		list_cut_position(&temp_list, &buf->rdma_list, &rdma_buf->list);
		PTL_FASTLOCK_UNLOCK(&buf->rdma_list_lock);

		/* free the chain of rdma bufs */
		while(!list_empty(&temp_list)) {
			rdma_buf = list_first_entry(&temp_list, buf_t, list);
			list_del(&rdma_buf->list);
			buf_put(rdma_buf);
		}
	} else {
		buf->rdma_desc_ok = 1;
	}

	err = process_tgt(buf);
	buf_put(buf);

	if (err) {
		WARN();
		return STATE_RECV_ERROR;
	}

	return STATE_RECV_DONE;
}

/**
 * Process a received buffer. RDMA only.
 *
 * @param buf the receive buffer that finished.
 *
 * @return the next state.
 */
static int recv_packet_rdma(buf_t *buf)
{
	ni_t *ni = obj_to_ni(buf);

	/* keep track of the number of buffers posted to the srq */
	atomic_dec(&ni->rdma.num_posted_recv);

	/* remove buf from pending receive list */
	assert(!list_empty(&buf->list));

	PTL_FASTLOCK_LOCK(&ni->rdma.recv_list_lock);
	list_del(&buf->list);
	PTL_FASTLOCK_UNLOCK(&ni->rdma.recv_list_lock);

	return STATE_RECV_PACKET;
}
#endif	/* WITH_TRANSPORT_IB */

/**
 * Process a received buffer. Common for RDMA and SHMEM.
 *
 * @param buf the receive buffer that finished.
 *
 * @return the next state.
 */
static int recv_packet(buf_t *buf)
{
	struct hdr_common *hdr = (struct hdr_common *)buf->data;

	/* sanity check received buffer */
	if (hdr->version != PTL_HDR_VER_1) {
		WARN();
		return STATE_RECV_DROP_BUF;
	}

	/* compute next state */
	if (hdr->operation <= OP_SWAP) {
		if (buf->length < sizeof(req_hdr_t))
			return STATE_RECV_DROP_BUF;
		else
			return STATE_RECV_REQ;
	}
	else if (hdr->operation >= OP_REPLY) {
		return STATE_RECV_INIT;
	}
	else {
#ifdef WITH_TRANSPORT_IB
		/* Disconnect. */
		conn_t *conn;
		const req_hdr_t *hdr = (req_hdr_t *)buf->data;
		ptl_process_t initiator;

		/* get per conn info */
		initiator.phys.nid = le32_to_cpu(hdr->src_nid);
		initiator.phys.pid = le32_to_cpu(hdr->src_pid);

		conn = get_conn(buf->obj.obj_ni, initiator);

		pthread_mutex_lock(&conn->mutex);

		conn->rdma.remote_disc = 1;

		/* Remote side ready to disconnect, and if we are too, then
		 * disconnect. */
		if (conn->rdma.local_disc == 2)
			disconnect_conn_locked(conn);

		pthread_mutex_unlock(&conn->mutex);
#endif

		return STATE_RECV_DROP_BUF;
	}
}

/**
 * Process a new request to target.
 *
 * @param buf the received buffer.
 *
 * @return the next state.
 */
static int recv_req(buf_t *buf)
{
	int err;
	req_hdr_t *hdr = (req_hdr_t *)buf->data;

	/* compute the data segments in the message
	 * note req packet data direction is wrt init */
	if (hdr->h1.data_in)
		buf->data_out = (data_t *)(buf->data + sizeof(*hdr));
	else
		buf->data_out = NULL;

	if (hdr->h1.data_out)
		buf->data_in = (data_t *)(buf->data + sizeof(*hdr) +
					  data_size(buf->data_out));
	else
		buf->data_in = NULL;

	buf->tgt_state = STATE_TGT_START;
	buf->type = BUF_TGT;

	/* Send message to target state machine. process_tgt must drop the
	 * buffer, so buf will not be valid after the call. */
	err = process_tgt(buf);
	if (err)
		WARN();

	return STATE_RECV_REPOST;
}

/**
 * Process a response message to initiator.
 *
 * @param buf the message received.
 *
 * @return the next state.
 */
static int recv_init(buf_t *buf)
{
	int err;
	buf_t *init_buf;
	ack_hdr_t *hdr = (ack_hdr_t *)buf->data;

	/* lookup the buf handle to get original buf */
	err = to_buf(le32_to_cpu(hdr->h1.handle), &init_buf);
	if (err) {
		WARN();
		return STATE_RECV_DROP_BUF;
	}

	/* compute data segments in response message */
	if (hdr->h1.data_in)
		init_buf->data_out = (data_t *)(buf->data + sizeof(ack_hdr_t));
	else
		init_buf->data_out = NULL;

	if (hdr->h1.data_out)
		init_buf->data_in = (data_t *)(buf->data  + sizeof(ack_hdr_t) +
					  data_size(init_buf->data_out));
	else
		init_buf->data_in = NULL;

	init_buf->recv_buf = buf;

	/* Note: process_init must drop recv_buf, so buf will not be valid
	 * after the call. */
	err = process_init(init_buf);
	if (err)
		WARN();

	buf_put(init_buf);	/* from to_buf() */

	return STATE_RECV_REPOST;
}

#if WITH_TRANSPORT_IB
/**
 * Repost receive buffers to srq.
 *
 * @param buf the received buffer.
 *
 * @return the next state.
 */
static int recv_repost(ni_t *ni)
{
	int num_bufs;

	/* compute the available room in the srq */
	num_bufs = ni->iface->cap.max_srq_wr - atomic_read(&ni->rdma.num_posted_recv);

	/* if rooms exceeds threshold repost that many buffers
	 * this should reduce the number of receive queue doorbells
	 * which should improve performance */
	if (num_bufs > get_param(PTL_SRQ_REPOST_SIZE))
		ptl_post_recv(ni, get_param(PTL_SRQ_REPOST_SIZE));

	return STATE_RECV_DONE;
}
#endif

/**
 * Drop the received buffer.
 *
 * @param buf the completed buffer.
 *
 * @return the next state.
 */
static int recv_drop_buf(buf_t *buf)
{
	ni_t *ni = obj_to_ni(buf);

	buf_put(buf);
	ni->num_recv_drops++;

	return STATE_RECV_REPOST;
}

#ifdef WITH_TRANSPORT_IB
/**
 * Completion queue polling thread.
 *
 * @param arg opaque pointer to ni.
 */
static void process_recv_rdma(ni_t *ni, buf_t *buf)
{
	enum recv_state state = buf->recv_state;

	while(1) {
		ptl_info("tid:%lx buf:%p: state = %s\n",
				 pthread_self(), buf, recv_state_name[state]);
		switch (state) {
		case STATE_RECV_SEND_COMP:
			state = send_comp(buf);
			break;
		case STATE_RECV_RDMA_COMP:
			state = rdma_comp(buf);
			break;
		case STATE_RECV_PACKET_RDMA:
			state = recv_packet_rdma(buf);
			break;
		case STATE_RECV_PACKET:
			state = recv_packet(buf);
			break;
		case STATE_RECV_REQ:
			state = recv_req(buf);
			break;
		case STATE_RECV_INIT:
			state = recv_init(buf);
			break;
		case STATE_RECV_REPOST:
			state = recv_repost(ni);
			break;
		case STATE_RECV_DROP_BUF:
			state = recv_drop_buf(buf);
			break;
		case STATE_RECV_ERROR:
			if (buf) {
				buf_put(buf);
				ni->num_recv_errs++;
			}
			goto done;
			break;
		case STATE_RECV_DONE:
			goto done;
			break;
		default:
			abort();
		}
	}
 done:
	return;
}

void progress_thread_ib(ni_t *ni)
{
	const int num_wc = get_param(PTL_WC_COUNT);
	buf_t *buf_list[num_wc];
	int i;
	int num_buf;
	struct ibv_wc wc_list[num_wc];

	num_buf = comp_poll(ni, num_wc, wc_list, buf_list);

	for (i = 0; i < num_buf; i++) {
		if (buf_list[i])
			process_recv_rdma(ni, buf_list[i]);
	}
}
#endif

#if WITH_TRANSPORT_SHMEM || IS_PPE
/**
 * Process a received message in shared memory.
 *
 * @param ni the ni to poll.
 * @param buf the received buffer.
 */
void process_recv_mem(ni_t *ni, buf_t *buf)
{
	enum recv_state state = STATE_RECV_PACKET;

	while(1) {
		ptl_info("tid:%lx buf:%p: recv state local = %s\n",
				 (long unsigned int)pthread_self(), buf,
				   recv_state_name[state]);
		switch (state) {
		case STATE_RECV_PACKET:
			state = recv_packet(buf);
			break;
		case STATE_RECV_REQ:
			state = recv_req(buf);
			break;
		case STATE_RECV_INIT:
			state = recv_init(buf);
			break;
		case STATE_RECV_DROP_BUF:
			state = recv_drop_buf(buf);
			break;
		case STATE_RECV_ERROR:
			if (buf) {
				buf_put(buf);
				ni->num_recv_errs++;
			}
			goto exit;
		case STATE_RECV_REPOST:
		case STATE_RECV_DONE:
			goto exit;

		case STATE_RECV_PACKET_RDMA:
		case STATE_RECV_SEND_COMP:
		case STATE_RECV_RDMA_COMP:
			/* Not reachable. */
			abort();
		}
	}
exit:
	return;
}
#endif

#if !IS_PPE
/**
 * Progress thread. Waits for both ib and shared memory messages.
 *
 * @param arg opaque pointer to ni.
 */

void *progress_thread(void *arg)
{
	ni_t *ni = arg;

	while(!ni->catcher_stop
#ifdef WITH_TRANSPORT_SHMEM
		  || atomic_read(&ni->sbuf_pool.count)
#endif
		  ) {

		progress_thread_ib(ni);

#if WITH_TRANSPORT_SHMEM
		/* Shared memory. Physical NIs don't have a receive queue. */
		if (ni->shmem.queue) {
			int err;
			buf_t *shmem_buf;

			shmem_buf = shmem_dequeue(ni);

			if (shmem_buf) {
				switch(shmem_buf->type) {
				case BUF_SHMEM_SEND: {
					buf_t *buf;

					/* Mark it for return now. The target state machine might
					 * change its type to BUF_SHMEM_SEND. */
					shmem_buf->type = BUF_SHMEM_RETURN;

					err = buf_alloc(ni, &buf);
					if (err) {
						WARN();
					} else {
						buf->data = shmem_buf->internal_data;
						buf->length = shmem_buf->length;
						buf->mem_buf = shmem_buf;
						INIT_LIST_HEAD(&buf->list);
						process_recv_mem(ni, buf);
					}

#if WITH_TRANSPORT_SHMEM && !USE_KNEM
					/* Don't send back if it's on the noknem list. */
					if (!list_empty(&buf->list))
						break;
#endif

					if (shmem_buf->type == BUF_SHMEM_SEND ||
						shmem_buf->shmem.index_owner != ni->mem.index) {
						/* Requested to send the buffer back, or not the
						 * owner. Send the buffer back in both cases. */
						shmem_enqueue(ni, shmem_buf, shmem_buf->shmem.index_owner);
					} else {
						/* It was returned to us with a message from a remote
						 * rank. From send_message_shmem(). */
						buf_put(shmem_buf);
					}
				}
					break;

				case BUF_SHMEM_RETURN:
					/* Buffer returned to us by remote node. */
					assert(shmem_buf->shmem.index_owner == ni->mem.index);

					/* From send_message_shmem(). */
					buf_put(shmem_buf);
					break;

				default:
					/* Should not happen. */
					abort();
				}
			}
		}
#endif

#if WITH_TRANSPORT_SHMEM && !USE_KNEM
		struct list_head *l, *t;

		/* TODO: instead of having a lock, the initiator should send
		 * the buf to itself, and on receiving it, the progress thread
		 * will put it on the list. That way, only the progress thread
		 * has access to the list. */
		PTL_FASTLOCK_LOCK(&ni->noknem_lock);

		list_for_each_safe(l, t, &ni->noknem_list) {
			buf_t *buf = list_entry(l, buf_t, list);
			struct noknem *noknem = buf->transfer.noknem.noknem;

			if (buf->transfer.noknem.transfer_state_expected == noknem->state) {
				if (noknem->state == 0)
					process_init(buf);
				else if (noknem->state == 2) {
					if (noknem->init_done) {
						buf_t *shmem_buf = buf->mem_buf;

						/* The transfer is now done. Remove from
						 * noknem_list. */
						list_del(&buf->list);

						process_tgt(buf);

						if (shmem_buf->type == BUF_SHMEM_SEND ||
							shmem_buf->shmem.index_owner != ni->mem.index) {
							/* Requested to send the buffer back, or not the
							 * owner. Send the buffer back in both cases. */
							shmem_enqueue(ni, shmem_buf, shmem_buf->shmem.index_owner);
						} else {
							/* It was returned to us with a message from a remote
							 * rank. From send_message_shmem(). */
							buf_put(shmem_buf);
						}

					} else {
						process_tgt(buf);
					}
				}
			}
		}

		PTL_FASTLOCK_UNLOCK(&ni->noknem_lock);
#endif
	}

	return NULL;
}
#endif
