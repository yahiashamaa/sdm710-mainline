// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/net/sunrpc/svc.c
 *
 * High-level RPC service routines
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 *
 * Multiple threads pools and NUMAisation
 * Copyright (c) 2006 Silicon Graphics, Inc.
 * by Greg Banks <gnb@melbourne.sgi.com>
 */

#include <linux/linkage.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#include <linux/sunrpc/types.h>
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/stats.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/bc_xprt.h>

#include <trace/events/sunrpc.h>

#include "fail.h"

#define RPCDBG_FACILITY	RPCDBG_SVCDSP

static void svc_unregister(const struct svc_serv *serv, struct net *net);

#define SVC_POOL_DEFAULT	SVC_POOL_GLOBAL

/*
 * Mode for mapping cpus to pools.
 */
enum {
	SVC_POOL_AUTO = -1,	/* choose one of the others */
	SVC_POOL_GLOBAL,	/* no mapping, just a single global pool
				 * (legacy & UP mode) */
	SVC_POOL_PERCPU,	/* one pool per cpu */
	SVC_POOL_PERNODE	/* one pool per numa node */
};

/*
 * Structure for mapping cpus to pools and vice versa.
 * Setup once during sunrpc initialisation.
 */

struct svc_pool_map {
	int count;			/* How many svc_servs use us */
	int mode;			/* Note: int not enum to avoid
					 * warnings about "enumeration value
					 * not handled in switch" */
	unsigned int npools;
	unsigned int *pool_to;		/* maps pool id to cpu or node */
	unsigned int *to_pool;		/* maps cpu or node to pool id */
};

static struct svc_pool_map svc_pool_map = {
	.mode = SVC_POOL_DEFAULT
};

static DEFINE_MUTEX(svc_pool_map_mutex);/* protects svc_pool_map.count only */

static int
param_set_pool_mode(const char *val, const struct kernel_param *kp)
{
	int *ip = (int *)kp->arg;
	struct svc_pool_map *m = &svc_pool_map;
	int err;

	mutex_lock(&svc_pool_map_mutex);

	err = -EBUSY;
	if (m->count)
		goto out;

	err = 0;
	if (!strncmp(val, "auto", 4))
		*ip = SVC_POOL_AUTO;
	else if (!strncmp(val, "global", 6))
		*ip = SVC_POOL_GLOBAL;
	else if (!strncmp(val, "percpu", 6))
		*ip = SVC_POOL_PERCPU;
	else if (!strncmp(val, "pernode", 7))
		*ip = SVC_POOL_PERNODE;
	else
		err = -EINVAL;

out:
	mutex_unlock(&svc_pool_map_mutex);
	return err;
}

static int
param_get_pool_mode(char *buf, const struct kernel_param *kp)
{
	int *ip = (int *)kp->arg;

	switch (*ip)
	{
	case SVC_POOL_AUTO:
		return strlcpy(buf, "auto\n", 20);
	case SVC_POOL_GLOBAL:
		return strlcpy(buf, "global\n", 20);
	case SVC_POOL_PERCPU:
		return strlcpy(buf, "percpu\n", 20);
	case SVC_POOL_PERNODE:
		return strlcpy(buf, "pernode\n", 20);
	default:
		return sprintf(buf, "%d\n", *ip);
	}
}

module_param_call(pool_mode, param_set_pool_mode, param_get_pool_mode,
		 &svc_pool_map.mode, 0644);

/*
 * Detect best pool mapping mode heuristically,
 * according to the machine's topology.
 */
static int
svc_pool_map_choose_mode(void)
{
	unsigned int node;

	if (nr_online_nodes > 1) {
		/*
		 * Actually have multiple NUMA nodes,
		 * so split pools on NUMA node boundaries
		 */
		return SVC_POOL_PERNODE;
	}

	node = first_online_node;
	if (nr_cpus_node(node) > 2) {
		/*
		 * Non-trivial SMP, or CONFIG_NUMA on
		 * non-NUMA hardware, e.g. with a generic
		 * x86_64 kernel on Xeons.  In this case we
		 * want to divide the pools on cpu boundaries.
		 */
		return SVC_POOL_PERCPU;
	}

	/* default: one global pool */
	return SVC_POOL_GLOBAL;
}

/*
 * Allocate the to_pool[] and pool_to[] arrays.
 * Returns 0 on success or an errno.
 */
static int
svc_pool_map_alloc_arrays(struct svc_pool_map *m, unsigned int maxpools)
{
	m->to_pool = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->to_pool)
		goto fail;
	m->pool_to = kcalloc(maxpools, sizeof(unsigned int), GFP_KERNEL);
	if (!m->pool_to)
		goto fail_free;

	return 0;

fail_free:
	kfree(m->to_pool);
	m->to_pool = NULL;
fail:
	return -ENOMEM;
}

/*
 * Initialise the pool map for SVC_POOL_PERCPU mode.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_percpu(struct svc_pool_map *m)
{
	unsigned int maxpools = nr_cpu_ids;
	unsigned int pidx = 0;
	unsigned int cpu;
	int err;

	err = svc_pool_map_alloc_arrays(m, maxpools);
	if (err)
		return err;

	for_each_online_cpu(cpu) {
		BUG_ON(pidx >= maxpools);
		m->to_pool[cpu] = pidx;
		m->pool_to[pidx] = cpu;
		pidx++;
	}
	/* cpus brought online later all get mapped to pool0, sorry */

	return pidx;
};


/*
 * Initialise the pool map for SVC_POOL_PERNODE mode.
 * Returns number of pools or <0 on error.
 */
static int
svc_pool_map_init_pernode(struct svc_pool_map *m)
{
	unsigned int maxpools = nr_node_ids;
	unsigned int pidx = 0;
	unsigned int node;
	int err;

	err = svc_pool_map_alloc_arrays(m, maxpools);
	if (err)
		return err;

	for_each_node_with_cpus(node) {
		/* some architectures (e.g. SN2) have cpuless nodes */
		BUG_ON(pidx > maxpools);
		m->to_pool[node] = pidx;
		m->pool_to[pidx] = node;
		pidx++;
	}
	/* nodes brought online later all get mapped to pool0, sorry */

	return pidx;
}


/*
 * Add a reference to the global map of cpus to pools (and
 * vice versa) if pools are in use.
 * Initialise the map if we're the first user.
 * Returns the number of pools. If this is '1', no reference
 * was taken.
 */
static unsigned int
svc_pool_map_get(void)
{
	struct svc_pool_map *m = &svc_pool_map;
	int npools = -1;

	mutex_lock(&svc_pool_map_mutex);

	if (m->count++) {
		mutex_unlock(&svc_pool_map_mutex);
		WARN_ON_ONCE(m->npools <= 1);
		return m->npools;
	}

	if (m->mode == SVC_POOL_AUTO)
		m->mode = svc_pool_map_choose_mode();

	switch (m->mode) {
	case SVC_POOL_PERCPU:
		npools = svc_pool_map_init_percpu(m);
		break;
	case SVC_POOL_PERNODE:
		npools = svc_pool_map_init_pernode(m);
		break;
	}

	if (npools <= 0) {
		/* default, or memory allocation failure */
		npools = 1;
		m->mode = SVC_POOL_GLOBAL;
	}
	m->npools = npools;

	if (npools == 1)
		/* service is unpooled, so doesn't hold a reference */
		m->count--;

	mutex_unlock(&svc_pool_map_mutex);
	return npools;
}

/*
 * Drop a reference to the global map of cpus to pools, if
 * pools were in use, i.e. if npools > 1.
 * When the last reference is dropped, the map data is
 * freed; this allows the sysadmin to change the pool
 * mode using the pool_mode module option without
 * rebooting or re-loading sunrpc.ko.
 */
static void
svc_pool_map_put(int npools)
{
	struct svc_pool_map *m = &svc_pool_map;

	if (npools <= 1)
		return;
	mutex_lock(&svc_pool_map_mutex);

	if (!--m->count) {
		kfree(m->to_pool);
		m->to_pool = NULL;
		kfree(m->pool_to);
		m->pool_to = NULL;
		m->npools = 0;
	}

	mutex_unlock(&svc_pool_map_mutex);
}

static int svc_pool_map_get_node(unsigned int pidx)
{
	const struct svc_pool_map *m = &svc_pool_map;

	if (m->count) {
		if (m->mode == SVC_POOL_PERCPU)
			return cpu_to_node(m->pool_to[pidx]);
		if (m->mode == SVC_POOL_PERNODE)
			return m->pool_to[pidx];
	}
	return NUMA_NO_NODE;
}
/*
 * Set the given thread's cpus_allowed mask so that it
 * will only run on cpus in the given pool.
 */
static inline void
svc_pool_map_set_cpumask(struct task_struct *task, unsigned int pidx)
{
	struct svc_pool_map *m = &svc_pool_map;
	unsigned int node = m->pool_to[pidx];

	/*
	 * The caller checks for sv_nrpools > 1, which
	 * implies that we've been initialized.
	 */
	WARN_ON_ONCE(m->count == 0);
	if (m->count == 0)
		return;

	switch (m->mode) {
	case SVC_POOL_PERCPU:
	{
		set_cpus_allowed_ptr(task, cpumask_of(node));
		break;
	}
	case SVC_POOL_PERNODE:
	{
		set_cpus_allowed_ptr(task, cpumask_of_node(node));
		break;
	}
	}
}

/**
 * svc_pool_for_cpu - Select pool to run a thread on this cpu
 * @serv: An RPC service
 *
 * Use the active CPU and the svc_pool_map's mode setting to
 * select the svc thread pool to use. Once initialized, the
 * svc_pool_map does not change.
 *
 * Return value:
 *   A pointer to an svc_pool
 */
struct svc_pool *svc_pool_for_cpu(struct svc_serv *serv)
{
	struct svc_pool_map *m = &svc_pool_map;
	int cpu = raw_smp_processor_id();
	unsigned int pidx = 0;

	if (serv->sv_nrpools <= 1)
		return serv->sv_pools;

	switch (m->mode) {
	case SVC_POOL_PERCPU:
		pidx = m->to_pool[cpu];
		break;
	case SVC_POOL_PERNODE:
		pidx = m->to_pool[cpu_to_node(cpu)];
		break;
	}

	return &serv->sv_pools[pidx % serv->sv_nrpools];
}

int svc_rpcb_setup(struct svc_serv *serv, struct net *net)
{
	int err;

	err = rpcb_create_local(net);
	if (err)
		return err;

	/* Remove any stale portmap registrations */
	svc_unregister(serv, net);
	return 0;
}
EXPORT_SYMBOL_GPL(svc_rpcb_setup);

void svc_rpcb_cleanup(struct svc_serv *serv, struct net *net)
{
	svc_unregister(serv, net);
	rpcb_put_local(net);
}
EXPORT_SYMBOL_GPL(svc_rpcb_cleanup);

static int svc_uses_rpcbind(struct svc_serv *serv)
{
	struct svc_program	*progp;
	unsigned int		i;

	for (progp = serv->sv_program; progp; progp = progp->pg_next) {
		for (i = 0; i < progp->pg_nvers; i++) {
			if (progp->pg_vers[i] == NULL)
				continue;
			if (!progp->pg_vers[i]->vs_hidden)
				return 1;
		}
	}

	return 0;
}

int svc_bind(struct svc_serv *serv, struct net *net)
{
	if (!svc_uses_rpcbind(serv))
		return 0;
	return svc_rpcb_setup(serv, net);
}
EXPORT_SYMBOL_GPL(svc_bind);

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
static void
__svc_init_bc(struct svc_serv *serv)
{
	INIT_LIST_HEAD(&serv->sv_cb_list);
	spin_lock_init(&serv->sv_cb_lock);
	init_waitqueue_head(&serv->sv_cb_waitq);
}
#else
static void
__svc_init_bc(struct svc_serv *serv)
{
}
#endif

/*
 * Create an RPC service
 */
static struct svc_serv *
__svc_create(struct svc_program *prog, unsigned int bufsize, int npools,
	     int (*threadfn)(void *data))
{
	struct svc_serv	*serv;
	unsigned int vers;
	unsigned int xdrsize;
	unsigned int i;

	if (!(serv = kzalloc(sizeof(*serv), GFP_KERNEL)))
		return NULL;
	serv->sv_name      = prog->pg_name;
	serv->sv_program   = prog;
	kref_init(&serv->sv_refcnt);
	serv->sv_stats     = prog->pg_stats;
	if (bufsize > RPCSVC_MAXPAYLOAD)
		bufsize = RPCSVC_MAXPAYLOAD;
	serv->sv_max_payload = bufsize? bufsize : 4096;
	serv->sv_max_mesg  = roundup(serv->sv_max_payload + PAGE_SIZE, PAGE_SIZE);
	serv->sv_threadfn = threadfn;
	xdrsize = 0;
	while (prog) {
		prog->pg_lovers = prog->pg_nvers-1;
		for (vers=0; vers<prog->pg_nvers ; vers++)
			if (prog->pg_vers[vers]) {
				prog->pg_hivers = vers;
				if (prog->pg_lovers > vers)
					prog->pg_lovers = vers;
				if (prog->pg_vers[vers]->vs_xdrsize > xdrsize)
					xdrsize = prog->pg_vers[vers]->vs_xdrsize;
			}
		prog = prog->pg_next;
	}
	serv->sv_xdrsize   = xdrsize;
	INIT_LIST_HEAD(&serv->sv_tempsocks);
	INIT_LIST_HEAD(&serv->sv_permsocks);
	timer_setup(&serv->sv_temptimer, NULL, 0);
	spin_lock_init(&serv->sv_lock);

	__svc_init_bc(serv);

	serv->sv_nrpools = npools;
	serv->sv_pools =
		kcalloc(serv->sv_nrpools, sizeof(struct svc_pool),
			GFP_KERNEL);
	if (!serv->sv_pools) {
		kfree(serv);
		return NULL;
	}

	for (i = 0; i < serv->sv_nrpools; i++) {
		struct svc_pool *pool = &serv->sv_pools[i];

		dprintk("svc: initialising pool %u for %s\n",
				i, serv->sv_name);

		pool->sp_id = i;
		INIT_LIST_HEAD(&pool->sp_sockets);
		INIT_LIST_HEAD(&pool->sp_all_threads);
		spin_lock_init(&pool->sp_lock);
	}

	return serv;
}

/**
 * svc_create - Create an RPC service
 * @prog: the RPC program the new service will handle
 * @bufsize: maximum message size for @prog
 * @threadfn: a function to service RPC requests for @prog
 *
 * Returns an instantiated struct svc_serv object or NULL.
 */
struct svc_serv *svc_create(struct svc_program *prog, unsigned int bufsize,
			    int (*threadfn)(void *data))
{
	return __svc_create(prog, bufsize, 1, threadfn);
}
EXPORT_SYMBOL_GPL(svc_create);

/**
 * svc_create_pooled - Create an RPC service with pooled threads
 * @prog: the RPC program the new service will handle
 * @bufsize: maximum message size for @prog
 * @threadfn: a function to service RPC requests for @prog
 *
 * Returns an instantiated struct svc_serv object or NULL.
 */
struct svc_serv *svc_create_pooled(struct svc_program *prog,
				   unsigned int bufsize,
				   int (*threadfn)(void *data))
{
	struct svc_serv *serv;
	unsigned int npools = svc_pool_map_get();

	serv = __svc_create(prog, bufsize, npools, threadfn);
	if (!serv)
		goto out_err;
	return serv;
out_err:
	svc_pool_map_put(npools);
	return NULL;
}
EXPORT_SYMBOL_GPL(svc_create_pooled);

/*
 * Destroy an RPC service. Should be called with appropriate locking to
 * protect sv_permsocks and sv_tempsocks.
 */
void
svc_destroy(struct kref *ref)
{
	struct svc_serv *serv = container_of(ref, struct svc_serv, sv_refcnt);

	dprintk("svc: svc_destroy(%s)\n", serv->sv_program->pg_name);
	timer_shutdown_sync(&serv->sv_temptimer);

	/*
	 * The last user is gone and thus all sockets have to be destroyed to
	 * the point. Check this.
	 */
	BUG_ON(!list_empty(&serv->sv_permsocks));
	BUG_ON(!list_empty(&serv->sv_tempsocks));

	cache_clean_deferred(serv);

	svc_pool_map_put(serv->sv_nrpools);

	kfree(serv->sv_pools);
	kfree(serv);
}
EXPORT_SYMBOL_GPL(svc_destroy);

/*
 * Allocate an RPC server's buffer space.
 * We allocate pages and place them in rq_pages.
 */
static int
svc_init_buffer(struct svc_rqst *rqstp, unsigned int size, int node)
{
	unsigned int pages, arghi;

	/* bc_xprt uses fore channel allocated buffers */
	if (svc_is_backchannel(rqstp))
		return 1;

	pages = size / PAGE_SIZE + 1; /* extra page as we hold both request and reply.
				       * We assume one is at most one page
				       */
	arghi = 0;
	WARN_ON_ONCE(pages > RPCSVC_MAXPAGES);
	if (pages > RPCSVC_MAXPAGES)
		pages = RPCSVC_MAXPAGES;
	while (pages) {
		struct page *p = alloc_pages_node(node, GFP_KERNEL, 0);
		if (!p)
			break;
		rqstp->rq_pages[arghi++] = p;
		pages--;
	}
	return pages == 0;
}

/*
 * Release an RPC server buffer
 */
static void
svc_release_buffer(struct svc_rqst *rqstp)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rqstp->rq_pages); i++)
		if (rqstp->rq_pages[i])
			put_page(rqstp->rq_pages[i]);
}

struct svc_rqst *
svc_rqst_alloc(struct svc_serv *serv, struct svc_pool *pool, int node)
{
	struct svc_rqst	*rqstp;

	rqstp = kzalloc_node(sizeof(*rqstp), GFP_KERNEL, node);
	if (!rqstp)
		return rqstp;

	__set_bit(RQ_BUSY, &rqstp->rq_flags);
	rqstp->rq_server = serv;
	rqstp->rq_pool = pool;

	rqstp->rq_scratch_page = alloc_pages_node(node, GFP_KERNEL, 0);
	if (!rqstp->rq_scratch_page)
		goto out_enomem;

	rqstp->rq_argp = kmalloc_node(serv->sv_xdrsize, GFP_KERNEL, node);
	if (!rqstp->rq_argp)
		goto out_enomem;

	rqstp->rq_resp = kmalloc_node(serv->sv_xdrsize, GFP_KERNEL, node);
	if (!rqstp->rq_resp)
		goto out_enomem;

	if (!svc_init_buffer(rqstp, serv->sv_max_mesg, node))
		goto out_enomem;

	return rqstp;
out_enomem:
	svc_rqst_free(rqstp);
	return NULL;
}
EXPORT_SYMBOL_GPL(svc_rqst_alloc);

static struct svc_rqst *
svc_prepare_thread(struct svc_serv *serv, struct svc_pool *pool, int node)
{
	struct svc_rqst	*rqstp;

	rqstp = svc_rqst_alloc(serv, pool, node);
	if (!rqstp)
		return ERR_PTR(-ENOMEM);

	svc_get(serv);
	spin_lock_bh(&serv->sv_lock);
	serv->sv_nrthreads += 1;
	spin_unlock_bh(&serv->sv_lock);

	spin_lock_bh(&pool->sp_lock);
	pool->sp_nrthreads++;
	list_add_rcu(&rqstp->rq_all, &pool->sp_all_threads);
	spin_unlock_bh(&pool->sp_lock);
	return rqstp;
}

/*
 * Choose a pool in which to create a new thread, for svc_set_num_threads
 */
static inline struct svc_pool *
choose_pool(struct svc_serv *serv, struct svc_pool *pool, unsigned int *state)
{
	if (pool != NULL)
		return pool;

	return &serv->sv_pools[(*state)++ % serv->sv_nrpools];
}

/*
 * Choose a thread to kill, for svc_set_num_threads
 */
static inline struct task_struct *
choose_victim(struct svc_serv *serv, struct svc_pool *pool, unsigned int *state)
{
	unsigned int i;
	struct task_struct *task = NULL;

	if (pool != NULL) {
		spin_lock_bh(&pool->sp_lock);
	} else {
		/* choose a pool in round-robin fashion */
		for (i = 0; i < serv->sv_nrpools; i++) {
			pool = &serv->sv_pools[--(*state) % serv->sv_nrpools];
			spin_lock_bh(&pool->sp_lock);
			if (!list_empty(&pool->sp_all_threads))
				goto found_pool;
			spin_unlock_bh(&pool->sp_lock);
		}
		return NULL;
	}

found_pool:
	if (!list_empty(&pool->sp_all_threads)) {
		struct svc_rqst *rqstp;

		/*
		 * Remove from the pool->sp_all_threads list
		 * so we don't try to kill it again.
		 */
		rqstp = list_entry(pool->sp_all_threads.next, struct svc_rqst, rq_all);
		set_bit(RQ_VICTIM, &rqstp->rq_flags);
		list_del_rcu(&rqstp->rq_all);
		task = rqstp->rq_task;
	}
	spin_unlock_bh(&pool->sp_lock);

	return task;
}

/* create new threads */
static int
svc_start_kthreads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	struct svc_rqst	*rqstp;
	struct task_struct *task;
	struct svc_pool *chosen_pool;
	unsigned int state = serv->sv_nrthreads-1;
	int node;

	do {
		nrservs--;
		chosen_pool = choose_pool(serv, pool, &state);

		node = svc_pool_map_get_node(chosen_pool->sp_id);
		rqstp = svc_prepare_thread(serv, chosen_pool, node);
		if (IS_ERR(rqstp))
			return PTR_ERR(rqstp);

		task = kthread_create_on_node(serv->sv_threadfn, rqstp,
					      node, "%s", serv->sv_name);
		if (IS_ERR(task)) {
			svc_exit_thread(rqstp);
			return PTR_ERR(task);
		}

		rqstp->rq_task = task;
		if (serv->sv_nrpools > 1)
			svc_pool_map_set_cpumask(task, chosen_pool->sp_id);

		svc_sock_update_bufs(serv);
		wake_up_process(task);
	} while (nrservs > 0);

	return 0;
}

/*
 * Create or destroy enough new threads to make the number
 * of threads the given number.  If `pool' is non-NULL, applies
 * only to threads in that pool, otherwise round-robins between
 * all pools.  Caller must ensure that mutual exclusion between this and
 * server startup or shutdown.
 */

/* destroy old threads */
static int
svc_stop_kthreads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	struct task_struct *task;
	unsigned int state = serv->sv_nrthreads-1;

	/* destroy old threads */
	do {
		task = choose_victim(serv, pool, &state);
		if (task == NULL)
			break;
		kthread_stop(task);
		nrservs++;
	} while (nrservs < 0);
	return 0;
}

int
svc_set_num_threads(struct svc_serv *serv, struct svc_pool *pool, int nrservs)
{
	if (pool == NULL) {
		nrservs -= serv->sv_nrthreads;
	} else {
		spin_lock_bh(&pool->sp_lock);
		nrservs -= pool->sp_nrthreads;
		spin_unlock_bh(&pool->sp_lock);
	}

	if (nrservs > 0)
		return svc_start_kthreads(serv, pool, nrservs);
	if (nrservs < 0)
		return svc_stop_kthreads(serv, pool, nrservs);
	return 0;
}
EXPORT_SYMBOL_GPL(svc_set_num_threads);

/**
 * svc_rqst_replace_page - Replace one page in rq_pages[]
 * @rqstp: svc_rqst with pages to replace
 * @page: replacement page
 *
 * When replacing a page in rq_pages, batch the release of the
 * replaced pages to avoid hammering the page allocator.
 */
void svc_rqst_replace_page(struct svc_rqst *rqstp, struct page *page)
{
	if (*rqstp->rq_next_page) {
		if (!pagevec_space(&rqstp->rq_pvec))
			__pagevec_release(&rqstp->rq_pvec);
		pagevec_add(&rqstp->rq_pvec, *rqstp->rq_next_page);
	}

	get_page(page);
	*(rqstp->rq_next_page++) = page;
}
EXPORT_SYMBOL_GPL(svc_rqst_replace_page);

/*
 * Called from a server thread as it's exiting. Caller must hold the "service
 * mutex" for the service.
 */
void
svc_rqst_free(struct svc_rqst *rqstp)
{
	svc_release_buffer(rqstp);
	if (rqstp->rq_scratch_page)
		put_page(rqstp->rq_scratch_page);
	kfree(rqstp->rq_resp);
	kfree(rqstp->rq_argp);
	kfree(rqstp->rq_auth_data);
	kfree_rcu(rqstp, rq_rcu_head);
}
EXPORT_SYMBOL_GPL(svc_rqst_free);

void
svc_exit_thread(struct svc_rqst *rqstp)
{
	struct svc_serv	*serv = rqstp->rq_server;
	struct svc_pool	*pool = rqstp->rq_pool;

	spin_lock_bh(&pool->sp_lock);
	pool->sp_nrthreads--;
	if (!test_and_set_bit(RQ_VICTIM, &rqstp->rq_flags))
		list_del_rcu(&rqstp->rq_all);
	spin_unlock_bh(&pool->sp_lock);

	spin_lock_bh(&serv->sv_lock);
	serv->sv_nrthreads -= 1;
	spin_unlock_bh(&serv->sv_lock);
	svc_sock_update_bufs(serv);

	svc_rqst_free(rqstp);

	svc_put(serv);
}
EXPORT_SYMBOL_GPL(svc_exit_thread);

/*
 * Register an "inet" protocol family netid with the local
 * rpcbind daemon via an rpcbind v4 SET request.
 *
 * No netconfig infrastructure is available in the kernel, so
 * we map IP_ protocol numbers to netids by hand.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_rpcb_register4(struct net *net, const u32 program,
				const u32 version,
				const unsigned short protocol,
				const unsigned short port)
{
	const struct sockaddr_in sin = {
		.sin_family		= AF_INET,
		.sin_addr.s_addr	= htonl(INADDR_ANY),
		.sin_port		= htons(port),
	};
	const char *netid;
	int error;

	switch (protocol) {
	case IPPROTO_UDP:
		netid = RPCBIND_NETID_UDP;
		break;
	case IPPROTO_TCP:
		netid = RPCBIND_NETID_TCP;
		break;
	default:
		return -ENOPROTOOPT;
	}

	error = rpcb_v4_register(net, program, version,
					(const struct sockaddr *)&sin, netid);

	/*
	 * User space didn't support rpcbind v4, so retry this
	 * registration request with the legacy rpcbind v2 protocol.
	 */
	if (error == -EPROTONOSUPPORT)
		error = rpcb_register(net, program, version, protocol, port);

	return error;
}

#if IS_ENABLED(CONFIG_IPV6)
/*
 * Register an "inet6" protocol family netid with the local
 * rpcbind daemon via an rpcbind v4 SET request.
 *
 * No netconfig infrastructure is available in the kernel, so
 * we map IP_ protocol numbers to netids by hand.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_rpcb_register6(struct net *net, const u32 program,
				const u32 version,
				const unsigned short protocol,
				const unsigned short port)
{
	const struct sockaddr_in6 sin6 = {
		.sin6_family		= AF_INET6,
		.sin6_addr		= IN6ADDR_ANY_INIT,
		.sin6_port		= htons(port),
	};
	const char *netid;
	int error;

	switch (protocol) {
	case IPPROTO_UDP:
		netid = RPCBIND_NETID_UDP6;
		break;
	case IPPROTO_TCP:
		netid = RPCBIND_NETID_TCP6;
		break;
	default:
		return -ENOPROTOOPT;
	}

	error = rpcb_v4_register(net, program, version,
					(const struct sockaddr *)&sin6, netid);

	/*
	 * User space didn't support rpcbind version 4, so we won't
	 * use a PF_INET6 listener.
	 */
	if (error == -EPROTONOSUPPORT)
		error = -EAFNOSUPPORT;

	return error;
}
#endif	/* IS_ENABLED(CONFIG_IPV6) */

/*
 * Register a kernel RPC service via rpcbind version 4.
 *
 * Returns zero on success; a negative errno value is returned
 * if any error occurs.
 */
static int __svc_register(struct net *net, const char *progname,
			  const u32 program, const u32 version,
			  const int family,
			  const unsigned short protocol,
			  const unsigned short port)
{
	int error = -EAFNOSUPPORT;

	switch (family) {
	case PF_INET:
		error = __svc_rpcb_register4(net, program, version,
						protocol, port);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case PF_INET6:
		error = __svc_rpcb_register6(net, program, version,
						protocol, port);
#endif
	}

	trace_svc_register(progname, version, protocol, port, family, error);
	return error;
}

int svc_rpcbind_set_version(struct net *net,
			    const struct svc_program *progp,
			    u32 version, int family,
			    unsigned short proto,
			    unsigned short port)
{
	return __svc_register(net, progp->pg_name, progp->pg_prog,
				version, family, proto, port);

}
EXPORT_SYMBOL_GPL(svc_rpcbind_set_version);

int svc_generic_rpcbind_set(struct net *net,
			    const struct svc_program *progp,
			    u32 version, int family,
			    unsigned short proto,
			    unsigned short port)
{
	const struct svc_version *vers = progp->pg_vers[version];
	int error;

	if (vers == NULL)
		return 0;

	if (vers->vs_hidden) {
		trace_svc_noregister(progp->pg_name, version, proto,
				     port, family, 0);
		return 0;
	}

	/*
	 * Don't register a UDP port if we need congestion
	 * control.
	 */
	if (vers->vs_need_cong_ctrl && proto == IPPROTO_UDP)
		return 0;

	error = svc_rpcbind_set_version(net, progp, version,
					family, proto, port);

	return (vers->vs_rpcb_optnl) ? 0 : error;
}
EXPORT_SYMBOL_GPL(svc_generic_rpcbind_set);

/**
 * svc_register - register an RPC service with the local portmapper
 * @serv: svc_serv struct for the service to register
 * @net: net namespace for the service to register
 * @family: protocol family of service's listener socket
 * @proto: transport protocol number to advertise
 * @port: port to advertise
 *
 * Service is registered for any address in the passed-in protocol family
 */
int svc_register(const struct svc_serv *serv, struct net *net,
		 const int family, const unsigned short proto,
		 const unsigned short port)
{
	struct svc_program	*progp;
	unsigned int		i;
	int			error = 0;

	WARN_ON_ONCE(proto == 0 && port == 0);
	if (proto == 0 && port == 0)
		return -EINVAL;

	for (progp = serv->sv_program; progp; progp = progp->pg_next) {
		for (i = 0; i < progp->pg_nvers; i++) {

			error = progp->pg_rpcbind_set(net, progp, i,
					family, proto, port);
			if (error < 0) {
				printk(KERN_WARNING "svc: failed to register "
					"%sv%u RPC service (errno %d).\n",
					progp->pg_name, i, -error);
				break;
			}
		}
	}

	return error;
}

/*
 * If user space is running rpcbind, it should take the v4 UNSET
 * and clear everything for this [program, version].  If user space
 * is running portmap, it will reject the v4 UNSET, but won't have
 * any "inet6" entries anyway.  So a PMAP_UNSET should be sufficient
 * in this case to clear all existing entries for [program, version].
 */
static void __svc_unregister(struct net *net, const u32 program, const u32 version,
			     const char *progname)
{
	int error;

	error = rpcb_v4_register(net, program, version, NULL, "");

	/*
	 * User space didn't support rpcbind v4, so retry this
	 * request with the legacy rpcbind v2 protocol.
	 */
	if (error == -EPROTONOSUPPORT)
		error = rpcb_register(net, program, version, 0, 0);

	trace_svc_unregister(progname, version, error);
}

/*
 * All netids, bind addresses and ports registered for [program, version]
 * are removed from the local rpcbind database (if the service is not
 * hidden) to make way for a new instance of the service.
 *
 * The result of unregistration is reported via dprintk for those who want
 * verification of the result, but is otherwise not important.
 */
static void svc_unregister(const struct svc_serv *serv, struct net *net)
{
	struct svc_program *progp;
	unsigned long flags;
	unsigned int i;

	clear_thread_flag(TIF_SIGPENDING);

	for (progp = serv->sv_program; progp; progp = progp->pg_next) {
		for (i = 0; i < progp->pg_nvers; i++) {
			if (progp->pg_vers[i] == NULL)
				continue;
			if (progp->pg_vers[i]->vs_hidden)
				continue;
			__svc_unregister(net, progp->pg_prog, i, progp->pg_name);
		}
	}

	spin_lock_irqsave(&current->sighand->siglock, flags);
	recalc_sigpending();
	spin_unlock_irqrestore(&current->sighand->siglock, flags);
}

/*
 * dprintk the given error with the address of the client that caused it.
 */
#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
static __printf(2, 3)
void svc_printk(struct svc_rqst *rqstp, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	char 	buf[RPC_MAX_ADDRBUFLEN];

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	dprintk("svc: %s: %pV", svc_print_addr(rqstp, buf, sizeof(buf)), &vaf);

	va_end(args);
}
#else
static __printf(2,3) void svc_printk(struct svc_rqst *rqstp, const char *fmt, ...) {}
#endif

__be32
svc_generic_init_request(struct svc_rqst *rqstp,
		const struct svc_program *progp,
		struct svc_process_info *ret)
{
	const struct svc_version *versp = NULL;	/* compiler food */
	const struct svc_procedure *procp = NULL;

	if (rqstp->rq_vers >= progp->pg_nvers )
		goto err_bad_vers;
	versp = progp->pg_vers[rqstp->rq_vers];
	if (!versp)
		goto err_bad_vers;

	/*
	 * Some protocol versions (namely NFSv4) require some form of
	 * congestion control.  (See RFC 7530 section 3.1 paragraph 2)
	 * In other words, UDP is not allowed. We mark those when setting
	 * up the svc_xprt, and verify that here.
	 *
	 * The spec is not very clear about what error should be returned
	 * when someone tries to access a server that is listening on UDP
	 * for lower versions. RPC_PROG_MISMATCH seems to be the closest
	 * fit.
	 */
	if (versp->vs_need_cong_ctrl && rqstp->rq_xprt &&
	    !test_bit(XPT_CONG_CTRL, &rqstp->rq_xprt->xpt_flags))
		goto err_bad_vers;

	if (rqstp->rq_proc >= versp->vs_nproc)
		goto err_bad_proc;
	rqstp->rq_procinfo = procp = &versp->vs_proc[rqstp->rq_proc];
	if (!procp)
		goto err_bad_proc;

	/* Initialize storage for argp and resp */
	memset(rqstp->rq_argp, 0, procp->pc_argzero);
	memset(rqstp->rq_resp, 0, procp->pc_ressize);

	/* Bump per-procedure stats counter */
	versp->vs_count[rqstp->rq_proc]++;

	ret->dispatch = versp->vs_dispatch;
	return rpc_success;
err_bad_vers:
	ret->mismatch.lovers = progp->pg_lovers;
	ret->mismatch.hivers = progp->pg_hivers;
	return rpc_prog_mismatch;
err_bad_proc:
	return rpc_proc_unavail;
}
EXPORT_SYMBOL_GPL(svc_generic_init_request);

/*
 * Common routine for processing the RPC request.
 */
static int
svc_process_common(struct svc_rqst *rqstp)
{
	struct xdr_stream	*xdr = &rqstp->rq_res_stream;
	struct svc_program	*progp;
	const struct svc_procedure *procp = NULL;
	struct svc_serv		*serv = rqstp->rq_server;
	struct svc_process_info process;
	int			auth_res, rc;
	unsigned int		aoffset;
	__be32			*p;

	/* Will be turned off by GSS integrity and privacy services */
	set_bit(RQ_SPLICE_OK, &rqstp->rq_flags);
	/* Will be turned off only when NFSv4 Sessions are used */
	set_bit(RQ_USEDEFERRAL, &rqstp->rq_flags);
	clear_bit(RQ_DROPME, &rqstp->rq_flags);

	/* Construct the first words of the reply: */
	svcxdr_init_encode(rqstp);
	xdr_stream_encode_be32(xdr, rqstp->rq_xid);
	xdr_stream_encode_be32(xdr, rpc_reply);

	p = xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 4);
	if (unlikely(!p))
		goto err_short_len;
	if (*p++ != cpu_to_be32(RPC_VERSION))
		goto err_bad_rpc;

	xdr_stream_encode_be32(xdr, rpc_msg_accepted);

	rqstp->rq_prog = be32_to_cpup(p++);
	rqstp->rq_vers = be32_to_cpup(p++);
	rqstp->rq_proc = be32_to_cpup(p);

	for (progp = serv->sv_program; progp; progp = progp->pg_next)
		if (rqstp->rq_prog == progp->pg_prog)
			break;

	/*
	 * Decode auth data, and add verifier to reply buffer.
	 * We do this before anything else in order to get a decent
	 * auth verifier.
	 */
	auth_res = svc_authenticate(rqstp);
	/* Also give the program a chance to reject this call: */
	if (auth_res == SVC_OK && progp)
		auth_res = progp->pg_authenticate(rqstp);
	trace_svc_authenticate(rqstp, auth_res);
	switch (auth_res) {
	case SVC_OK:
		break;
	case SVC_GARBAGE:
		goto err_garbage_args;
	case SVC_SYSERR:
		goto err_system_err;
	case SVC_DENIED:
		goto err_bad_auth;
	case SVC_CLOSE:
		goto close;
	case SVC_DROP:
		goto dropit;
	case SVC_COMPLETE:
		goto sendit;
	}

	if (progp == NULL)
		goto err_bad_prog;

	switch (progp->pg_init_request(rqstp, progp, &process)) {
	case rpc_success:
		break;
	case rpc_prog_unavail:
		goto err_bad_prog;
	case rpc_prog_mismatch:
		goto err_bad_vers;
	case rpc_proc_unavail:
		goto err_bad_proc;
	}

	procp = rqstp->rq_procinfo;
	/* Should this check go into the dispatcher? */
	if (!procp || !procp->pc_func)
		goto err_bad_proc;

	/* Syntactic check complete */
	serv->sv_stats->rpccnt++;
	trace_svc_process(rqstp, progp->pg_name);

	aoffset = xdr_stream_pos(xdr);

	/* un-reserve some of the out-queue now that we have a
	 * better idea of reply size
	 */
	if (procp->pc_xdrressize)
		svc_reserve_auth(rqstp, procp->pc_xdrressize<<2);

	/* Call the function that processes the request. */
	rc = process.dispatch(rqstp);
	if (procp->pc_release)
		procp->pc_release(rqstp);
	if (!rc)
		goto dropit;
	if (rqstp->rq_auth_stat != rpc_auth_ok)
		goto err_bad_auth;

	if (*rqstp->rq_accept_statp != rpc_success)
		xdr_truncate_encode(xdr, aoffset);

	if (procp->pc_encode == NULL)
		goto dropit;

 sendit:
	if (svc_authorise(rqstp))
		goto close_xprt;
	return 1;		/* Caller can now send it */

 dropit:
	svc_authorise(rqstp);	/* doesn't hurt to call this twice */
	dprintk("svc: svc_process dropit\n");
	return 0;

 close:
	svc_authorise(rqstp);
close_xprt:
	if (rqstp->rq_xprt && test_bit(XPT_TEMP, &rqstp->rq_xprt->xpt_flags))
		svc_xprt_close(rqstp->rq_xprt);
	dprintk("svc: svc_process close\n");
	return 0;

err_short_len:
	svc_printk(rqstp, "short len %u, dropping request\n",
		   rqstp->rq_arg.len);
	goto close_xprt;

err_bad_rpc:
	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_MSG_DENIED);
	xdr_stream_encode_u32(xdr, RPC_MISMATCH);
	/* Only RPCv2 supported */
	xdr_stream_encode_u32(xdr, RPC_VERSION);
	xdr_stream_encode_u32(xdr, RPC_VERSION);
	goto sendit;

err_bad_auth:
	dprintk("svc: authentication failed (%d)\n",
		be32_to_cpu(rqstp->rq_auth_stat));
	serv->sv_stats->rpcbadauth++;
	/* Restore write pointer to location of reply status: */
	xdr_truncate_encode(xdr, XDR_UNIT * 2);
	xdr_stream_encode_u32(xdr, RPC_MSG_DENIED);
	xdr_stream_encode_u32(xdr, RPC_AUTH_ERROR);
	xdr_stream_encode_be32(xdr, rqstp->rq_auth_stat);
	goto sendit;

err_bad_prog:
	dprintk("svc: unknown program %d\n", rqstp->rq_prog);
	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_PROG_UNAVAIL);
	goto sendit;

err_bad_vers:
	svc_printk(rqstp, "unknown version (%d for prog %d, %s)\n",
		       rqstp->rq_vers, rqstp->rq_prog, progp->pg_name);

	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_PROG_MISMATCH);
	xdr_stream_encode_u32(xdr, process.mismatch.lovers);
	xdr_stream_encode_u32(xdr, process.mismatch.hivers);
	goto sendit;

err_bad_proc:
	svc_printk(rqstp, "unknown procedure (%d)\n", rqstp->rq_proc);

	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_PROC_UNAVAIL);
	goto sendit;

err_garbage_args:
	svc_printk(rqstp, "failed to decode RPC header\n");

	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_GARBAGE_ARGS);
	goto sendit;

err_system_err:
	serv->sv_stats->rpcbadfmt++;
	xdr_stream_encode_u32(xdr, RPC_SYSTEM_ERR);
	goto sendit;
}

/*
 * Process the RPC request.
 */
int
svc_process(struct svc_rqst *rqstp)
{
	struct kvec		*resv = &rqstp->rq_res.head[0];
	__be32 *p;

#if IS_ENABLED(CONFIG_FAIL_SUNRPC)
	if (!fail_sunrpc.ignore_server_disconnect &&
	    should_fail(&fail_sunrpc.attr, 1))
		svc_xprt_deferred_close(rqstp->rq_xprt);
#endif

	/*
	 * Setup response xdr_buf.
	 * Initially it has just one page
	 */
	rqstp->rq_next_page = &rqstp->rq_respages[1];
	resv->iov_base = page_address(rqstp->rq_respages[0]);
	resv->iov_len = 0;
	rqstp->rq_res.pages = rqstp->rq_next_page;
	rqstp->rq_res.len = 0;
	rqstp->rq_res.page_base = 0;
	rqstp->rq_res.page_len = 0;
	rqstp->rq_res.buflen = PAGE_SIZE;
	rqstp->rq_res.tail[0].iov_base = NULL;
	rqstp->rq_res.tail[0].iov_len = 0;

	svcxdr_init_decode(rqstp);
	p = xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 2);
	if (unlikely(!p))
		goto out_drop;
	rqstp->rq_xid = *p++;
	if (unlikely(*p != rpc_call))
		goto out_baddir;

	if (!svc_process_common(rqstp))
		goto out_drop;
	return svc_send(rqstp);

out_baddir:
	svc_printk(rqstp, "bad direction 0x%08x, dropping request\n",
		   be32_to_cpu(*p));
	rqstp->rq_server->sv_stats->rpcbadfmt++;
out_drop:
	svc_drop(rqstp);
	return 0;
}
EXPORT_SYMBOL_GPL(svc_process);

#if defined(CONFIG_SUNRPC_BACKCHANNEL)
/*
 * Process a backchannel RPC request that arrived over an existing
 * outbound connection
 */
int
bc_svc_process(struct svc_serv *serv, struct rpc_rqst *req,
	       struct svc_rqst *rqstp)
{
	struct rpc_task *task;
	int proc_error;
	int error;

	dprintk("svc: %s(%p)\n", __func__, req);

	/* Build the svc_rqst used by the common processing routine */
	rqstp->rq_xid = req->rq_xid;
	rqstp->rq_prot = req->rq_xprt->prot;
	rqstp->rq_server = serv;
	rqstp->rq_bc_net = req->rq_xprt->xprt_net;

	rqstp->rq_addrlen = sizeof(req->rq_xprt->addr);
	memcpy(&rqstp->rq_addr, &req->rq_xprt->addr, rqstp->rq_addrlen);
	memcpy(&rqstp->rq_arg, &req->rq_rcv_buf, sizeof(rqstp->rq_arg));
	memcpy(&rqstp->rq_res, &req->rq_snd_buf, sizeof(rqstp->rq_res));

	/* Adjust the argument buffer length */
	rqstp->rq_arg.len = req->rq_private_buf.len;
	if (rqstp->rq_arg.len <= rqstp->rq_arg.head[0].iov_len) {
		rqstp->rq_arg.head[0].iov_len = rqstp->rq_arg.len;
		rqstp->rq_arg.page_len = 0;
	} else if (rqstp->rq_arg.len <= rqstp->rq_arg.head[0].iov_len +
			rqstp->rq_arg.page_len)
		rqstp->rq_arg.page_len = rqstp->rq_arg.len -
			rqstp->rq_arg.head[0].iov_len;
	else
		rqstp->rq_arg.len = rqstp->rq_arg.head[0].iov_len +
			rqstp->rq_arg.page_len;

	/* Reset the response buffer */
	rqstp->rq_res.head[0].iov_len = 0;

	/*
	 * Skip the XID and calldir fields because they've already
	 * been processed by the caller.
	 */
	svcxdr_init_decode(rqstp);
	if (!xdr_inline_decode(&rqstp->rq_arg_stream, XDR_UNIT * 2)) {
		error = -EINVAL;
		goto out;
	}

	/* Parse and execute the bc call */
	proc_error = svc_process_common(rqstp);

	atomic_dec(&req->rq_xprt->bc_slot_count);
	if (!proc_error) {
		/* Processing error: drop the request */
		xprt_free_bc_request(req);
		error = -EINVAL;
		goto out;
	}
	/* Finally, send the reply synchronously */
	memcpy(&req->rq_snd_buf, &rqstp->rq_res, sizeof(req->rq_snd_buf));
	task = rpc_run_bc_task(req);
	if (IS_ERR(task)) {
		error = PTR_ERR(task);
		goto out;
	}

	WARN_ON_ONCE(atomic_read(&task->tk_count) != 1);
	error = task->tk_status;
	rpc_put_task(task);

out:
	dprintk("svc: %s(), error=%d\n", __func__, error);
	return error;
}
EXPORT_SYMBOL_GPL(bc_svc_process);
#endif /* CONFIG_SUNRPC_BACKCHANNEL */

/**
 * svc_max_payload - Return transport-specific limit on the RPC payload
 * @rqstp: RPC transaction context
 *
 * Returns the maximum number of payload bytes the current transport
 * allows.
 */
u32 svc_max_payload(const struct svc_rqst *rqstp)
{
	u32 max = rqstp->rq_xprt->xpt_class->xcl_max_payload;

	if (rqstp->rq_server->sv_max_payload < max)
		max = rqstp->rq_server->sv_max_payload;
	return max;
}
EXPORT_SYMBOL_GPL(svc_max_payload);

/**
 * svc_proc_name - Return RPC procedure name in string form
 * @rqstp: svc_rqst to operate on
 *
 * Return value:
 *   Pointer to a NUL-terminated string
 */
const char *svc_proc_name(const struct svc_rqst *rqstp)
{
	if (rqstp && rqstp->rq_procinfo)
		return rqstp->rq_procinfo->pc_name;
	return "unknown";
}


/**
 * svc_encode_result_payload - mark a range of bytes as a result payload
 * @rqstp: svc_rqst to operate on
 * @offset: payload's byte offset in rqstp->rq_res
 * @length: size of payload, in bytes
 *
 * Returns zero on success, or a negative errno if a permanent
 * error occurred.
 */
int svc_encode_result_payload(struct svc_rqst *rqstp, unsigned int offset,
			      unsigned int length)
{
	return rqstp->rq_xprt->xpt_ops->xpo_result_payload(rqstp, offset,
							   length);
}
EXPORT_SYMBOL_GPL(svc_encode_result_payload);

/**
 * svc_fill_write_vector - Construct data argument for VFS write call
 * @rqstp: svc_rqst to operate on
 * @payload: xdr_buf containing only the write data payload
 *
 * Fills in rqstp::rq_vec, and returns the number of elements.
 */
unsigned int svc_fill_write_vector(struct svc_rqst *rqstp,
				   struct xdr_buf *payload)
{
	struct page **pages = payload->pages;
	struct kvec *first = payload->head;
	struct kvec *vec = rqstp->rq_vec;
	size_t total = payload->len;
	unsigned int i;

	/* Some types of transport can present the write payload
	 * entirely in rq_arg.pages. In this case, @first is empty.
	 */
	i = 0;
	if (first->iov_len) {
		vec[i].iov_base = first->iov_base;
		vec[i].iov_len = min_t(size_t, total, first->iov_len);
		total -= vec[i].iov_len;
		++i;
	}

	while (total) {
		vec[i].iov_base = page_address(*pages);
		vec[i].iov_len = min_t(size_t, total, PAGE_SIZE);
		total -= vec[i].iov_len;
		++i;
		++pages;
	}

	WARN_ON_ONCE(i > ARRAY_SIZE(rqstp->rq_vec));
	return i;
}
EXPORT_SYMBOL_GPL(svc_fill_write_vector);

/**
 * svc_fill_symlink_pathname - Construct pathname argument for VFS symlink call
 * @rqstp: svc_rqst to operate on
 * @first: buffer containing first section of pathname
 * @p: buffer containing remaining section of pathname
 * @total: total length of the pathname argument
 *
 * The VFS symlink API demands a NUL-terminated pathname in mapped memory.
 * Returns pointer to a NUL-terminated string, or an ERR_PTR. Caller must free
 * the returned string.
 */
char *svc_fill_symlink_pathname(struct svc_rqst *rqstp, struct kvec *first,
				void *p, size_t total)
{
	size_t len, remaining;
	char *result, *dst;

	result = kmalloc(total + 1, GFP_KERNEL);
	if (!result)
		return ERR_PTR(-ESERVERFAULT);

	dst = result;
	remaining = total;

	len = min_t(size_t, total, first->iov_len);
	if (len) {
		memcpy(dst, first->iov_base, len);
		dst += len;
		remaining -= len;
	}

	if (remaining) {
		len = min_t(size_t, remaining, PAGE_SIZE);
		memcpy(dst, p, len);
		dst += len;
	}

	*dst = '\0';

	/* Sanity check: Linux doesn't allow the pathname argument to
	 * contain a NUL byte.
	 */
	if (strlen(result) != total) {
		kfree(result);
		return ERR_PTR(-EINVAL);
	}
	return result;
}
EXPORT_SYMBOL_GPL(svc_fill_symlink_pathname);
