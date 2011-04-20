/*
 * ptl_buf.c - IO buffer
 */

#include "ptl_loc.h"

/*
 * buf_release - release buffers reference to associated memory region.
 */ 
void buf_release(void *arg)
{
}

/*
 * buf_init - initialize a buffer object and create/reference memory region.
 */
int buf_init(void *arg, void *parm)
{
	buf_t *buf = arg;
	struct ibv_mr *mr = parm;

	INIT_LIST_HEAD(&buf->list);

	buf->size = sizeof(buf->data);
	buf->length = 0;

	buf->send_wr.next = NULL;
	buf->send_wr.wr_id = (uintptr_t)buf;
	buf->send_wr.sg_list = buf->sg_list;
	buf->send_wr.num_sge = 1;

	buf->sg_list[0].addr = (uintptr_t)buf->data;
	buf->sg_list[0].lkey = mr->lkey;

	return 0;
}

/*
 * buf_dump - debug function to print buffer attributes.
 */
void buf_dump(buf_t *buf)
{
	hdr_t *hdr = (hdr_t *)buf->data;

	printf("buf: %p\n", buf);
	printf("buf->size	= %d\n", buf->size);
	printf("buf->length	= %d\n", buf->length);
	printf("hdr->version	= %d\n", hdr->version);
	printf("hdr->operation	= %d\n", hdr->operation);
	printf("hdr->ni_type	= %d\n", hdr->ni_type);
	printf("hdr->pkt_fmt	= %d\n", hdr->pkt_fmt);
	printf("hdr->length	= %" PRId64 "\n", be64_to_cpu(hdr->length));
	printf("\n");
}

/*
 * post_recv - Allocate a receive buffer for specified NI and post to SRQ.
 */
int post_recv(ni_t *ni)
{
	int err;
	buf_t *buf;
	struct ibv_recv_wr *bad_wr;

	err = buf_alloc(ni, &buf);
	if (err) {
		WARN();
		err = PTL_FAIL;
		goto err1;
	}

	buf->sg_list[0].length = buf->size;
	buf->type = BUF_RECV;

	pthread_spin_lock(&ni->recv_list_lock);
	list_add_tail(&buf->list, &ni->recv_list);
	pthread_spin_unlock(&ni->recv_list_lock);

	err = ibv_post_srq_recv(ni->srq, &buf->recv_wr, &bad_wr);
	if (err) {
		WARN();
		pthread_spin_lock(&ni->recv_list_lock);
		list_del(&buf->list);
		pthread_spin_unlock(&ni->recv_list_lock);
		err = PTL_FAIL;
		goto err2;
	}

	return PTL_OK;

err2:
	pthread_spin_lock(&ni->recv_list_lock);
	list_del(&buf->list);
	pthread_spin_unlock(&ni->recv_list_lock);

	buf_put(buf);

err1:
	return err;
}