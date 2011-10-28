#ifndef PTL_ME_H
#define PTL_ME_H

struct ct;

#define TYPE_ME			(1)

typedef struct me {
	obj_t			obj;
	PTL_LE_OBJ

	ptl_size_t		offset;
	ptl_size_t		min_free;
	uint64_t		match_bits;
	uint64_t		ignore_bits;
	ptl_process_t   id;
} me_t;

int me_init(void *arg, void *unused);
void me_cleanup(void *arg);

static inline int me_alloc(ni_t *ni, me_t **me_p)
{
	int err;
	obj_t *obj;

	err = obj_alloc(&ni->me_pool, &obj);
	if (err) {
		*me_p = NULL;
		return err;
	}

	*me_p = container_of(obj, me_t, obj);
	return PTL_OK;
}

static inline int to_me(ptl_handle_me_t handle, me_t **me_p)
{
	obj_t *obj;

	obj = to_obj(POOL_ME, (ptl_handle_any_t)handle);
	if (!obj) {
		*me_p = NULL;
		return PTL_ARG_INVALID;
	}

	*me_p = container_of(obj, me_t, obj);
	return PTL_OK;
}

static inline void me_get(me_t *me)
{
	obj_get(&me->obj);
}

static inline int me_put(me_t *me)
{
	return obj_put(&me->obj);
}

static inline ptl_handle_me_t me_to_handle(me_t *me)
{
        return (ptl_handle_me_t)me->obj.obj_handle;
}

#endif /* PTL_ME_H */
