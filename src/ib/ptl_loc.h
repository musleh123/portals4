#ifndef PTL_LOC_H
#define PTL_LOC_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE		/* For Mac OS X */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <poll.h>
#include <sys/time.h>
#include <search.h>
#include <sys/ioctl.h>
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#endif

#include "tree.h"

#if WITH_TRANSPORT_IB
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#endif

#include "portals4.h"

/* use these for network byte order */
typedef uint16_t	__be16;
typedef uint32_t	__be32;
typedef uint64_t	__be64;
typedef uint16_t	__le16;
typedef uint32_t	__le32;
typedef uint64_t	__le64;

extern unsigned int pagesize;
extern unsigned int linesize;

#include "ptl_log.h"
#include "ptl_list.h"
#include "ptl_sync.h"
#include "ptl_ref.h"
#include "ptl_atomic.h"
#include "ptl_param.h"
#include "ptl_evloop.h"
#include "ptl_obj.h"
#include "ptl_iface.h"
#include "ptl_gbl.h"
#include "ptl_pt.h"
#include "ptl_queue.h"
#include "ptl_ni.h"
#include "ptl_conn.h"
#include "ptl_mr.h"
#include "ptl_md.h"
#include "ptl_le.h"
#include "ptl_me.h"
#include "ptl_ct.h"
#include "ptl_buf.h"
#include "ptl_eq.h"
#include "ptl_data.h"
#include "ptl_hdr.h"

static inline __be16 cpu_to_be16(uint16_t x) { return htons(x); }
static inline uint16_t be16_to_cpu(__be16 x) { return htons(x); }
static inline __be32 cpu_to_be32(uint32_t x) { return htonl(x); }
static inline uint32_t be32_to_cpu(__be32 x) { return htonl(x); }
static inline __be64 cpu_to_be64(uint64_t x) {
	uint64_t y = htonl((uint32_t)x);
	return (y << 32) | htonl((uint32_t)(x >> 32));
}
static inline uint64_t be64_to_cpu(__be64 x) { return cpu_to_be64(x); }

#if __BYTE_ORDER==__LITTLE_ENDIAN
static inline __le16 cpu_to_le16(uint16_t x) { return x; }
static inline uint16_t le16_to_cpu(__le16 x) { return x; }
static inline __le32 cpu_to_le32(uint32_t x) { return x; }
static inline uint32_t le32_to_cpu(__le32 x) { return x; }
static inline __le64 cpu_to_le64(uint64_t x) { return x; }
static inline uint64_t le64_to_cpu(__le64 x) { return x; }
#else
#error Not defined yet.
#endif

enum recv_state {
	STATE_RECV_SEND_COMP,
	STATE_RECV_RDMA_COMP,
	STATE_RECV_PACKET_RDMA,
	STATE_RECV_PACKET,
	STATE_RECV_DROP_BUF,
	STATE_RECV_REQ,
	STATE_RECV_INIT,
	STATE_RECV_REPOST,
	STATE_RECV_ERROR,
	STATE_RECV_DONE,
};

enum tgt_state {
	STATE_TGT_START,
	STATE_TGT_DROP,
	STATE_TGT_GET_MATCH,
	STATE_TGT_GET_LENGTH,
	STATE_TGT_WAIT_CONN,
	STATE_TGT_DATA,
	STATE_TGT_DATA_IN,
	STATE_TGT_RDMA,
	STATE_TGT_ATOMIC_DATA_IN,
	STATE_TGT_SWAP_DATA_IN,
	STATE_TGT_DATA_OUT,
	STATE_TGT_WAIT_RDMA_DESC,
	STATE_TGT_SHMEM_DESC,
	STATE_TGT_SEND_ACK,
	STATE_TGT_SEND_REPLY,
	STATE_TGT_COMM_EVENT,
	STATE_TGT_WAIT_APPEND,
	STATE_TGT_OVERFLOW_EVENT,
	STATE_TGT_CLEANUP,
	STATE_TGT_CLEANUP_2,
	STATE_TGT_ERROR,
	STATE_TGT_DONE,
};

enum init_state {
	STATE_INIT_START,
	STATE_INIT_PREP_REQ,
	STATE_INIT_WAIT_CONN,
	STATE_INIT_SEND_REQ,
	STATE_INIT_WAIT_COMP,
	STATE_INIT_SEND_ERROR,
	STATE_INIT_EARLY_SEND_EVENT,
	STATE_INIT_WAIT_RECV,
	STATE_INIT_DATA_IN,
	STATE_INIT_LATE_SEND_EVENT,
	STATE_INIT_ACK_EVENT,
	STATE_INIT_REPLY_EVENT,
	STATE_INIT_CLEANUP,
	STATE_INIT_ERROR,
	STATE_INIT_DONE,
	STATE_INIT_LAST,
};

/* In current implementation a NID is just an IPv4 address in host order. */
static inline in_addr_t nid_to_addr(ptl_nid_t nid)
{
	return htonl(nid);
}

static inline ptl_nid_t addr_to_nid(struct sockaddr_in *sin)
{
	return ntohl(sin->sin_addr.s_addr);
}

/* A PID is a port in host order. */
static inline __be16 pid_to_port(ptl_pid_t pid)
{
	if (pid == PTL_PID_ANY) {
		return 0;
	} else {
		return htons(pid);
	}
}

static inline ptl_pid_t port_to_pid(__be16 port)
{
	return ntohs(port);
}

int iov_copy_out(void *dst, ptl_iovec_t *iov, ptl_size_t num_iov,
		 ptl_size_t offset, ptl_size_t length);

int iov_copy_in(void *src, ptl_iovec_t *iov, ptl_size_t num_iov,
		ptl_size_t offset, ptl_size_t length);

int iov_atomic_in(atom_op_t op, int atom_size, void *src,
		  ptl_iovec_t *iov, ptl_size_t num_iov,
		  ptl_size_t offset, ptl_size_t length);

int iov_count_elem(ptl_iovec_t *iov, ptl_size_t num_iov,
				   ptl_size_t offset, ptl_size_t length,
				   ptl_size_t *index_p, ptl_size_t *base_p);

int swap_data_in(ptl_op_t atom_op, ptl_datatype_t atom_type,
		 void *dest, void *source, void *operand);

int process_rdma_desc(buf_t *buf);

void *progress_thread(void *arg);

int process_init(buf_t *buf);

int process_tgt(buf_t *buf);

int check_match(buf_t *buf, const me_t *me);

int check_perm(buf_t *buf, const le_t *le);

#ifdef WITH_TRANSPORT_IB
int PtlNIInit_IB(iface_t *iface, ni_t *ni);
void cleanup_ib(ni_t *ni);
int init_iface_ib(iface_t *iface);
void initiate_disconnect_all(ni_t *ni);
void disconnect_conn_locked(conn_t *conn);
#else
static inline int PtlNIInit_IB(iface_t *iface, ni_t *ni) { return PTL_OK; }
static inline void cleanup_ib(ni_t *ni) {  }
static inline int init_iface_ib(iface_t *iface) { return PTL_OK; }
static inline void initiate_disconnect_all(ni_t *ni) { }
#endif

#ifdef WITH_TRANSPORT_SHMEM
int knem_init(ni_t *ni);
void knem_fini(ni_t *ni);
uint64_t knem_register(ni_t *ni, void *data, ptl_size_t len, int prot);
void knem_unregister(ni_t *ni, uint64_t cookie);
size_t knem_copy_from(ni_t * ni, void *dst,
					  uint64_t cookie, uint64_t off, size_t len);
size_t knem_copy_to(ni_t * ni, uint64_t cookie,
					uint64_t off, void *src, size_t len);
size_t knem_copy(ni_t * ni,
				 uint64_t scookie, uint64_t soffset, 
				 uint64_t dcookie, uint64_t doffset,
				 size_t length);
extern int PtlNIInit_shmem(ni_t *ni);
void cleanup_shmem(ni_t *ni);
int setup_shmem(ni_t *ni);
void shmem_enqueue(ni_t *ni, buf_t *buf, ptl_pid_t dest);
buf_t *shmem_dequeue(ni_t *ni);
#else
static inline uint64_t knem_register(ni_t *ni, void *data, ptl_size_t len, int prot)
{
	return 1;
}
static inline void knem_unregister(ni_t *ni, uint64_t cookie) { }
static inline int PtlNIInit_shmem(ni_t *ni) { return PTL_OK; }
static inline void cleanup_shmem(ni_t *ni) { }
static inline int setup_shmem(ni_t *ni) { return PTL_OK; }
#endif

#if WITH_TRANSPORT_SHMEM
void PtlSetMap_mem(ni_t *ni, ptl_size_t map_size,
				   const ptl_process_t *mapping);
int do_mem_transfer(buf_t *buf);
#else
static inline void PtlSetMap_mem(ni_t *ni, ptl_size_t map_size,
								 const ptl_process_t *mapping) { }
#endif

extern int ptl_log_level;

int misc_init_once(void);
int _PtlInit(gbl_t *gbl);
int gbl_init(gbl_t *gbl);
void _PtlFini(gbl_t *gbl);

#endif /* PTL_LOC_H */
