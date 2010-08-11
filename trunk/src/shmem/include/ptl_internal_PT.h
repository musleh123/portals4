#ifndef PTL_INTERNAL_PT_H
#define PTL_INTERNAL_PT_H

#include <pthread.h>
#include <stdint.h>		       /* for uint32_t */

typedef struct {
    pthread_mutex_t lock;
    struct PTqueue {
	void *head, *tail;
    } priority,
            overflow;
    ptl_handle_eq_t EQ;
    uint32_t status;
    unsigned int options;
} ptl_table_entry_t;

void PtlInternalPTInit(
    ptl_table_entry_t * t);
int PtlInternalPTValidate(
    ptl_table_entry_t * t);

#endif
