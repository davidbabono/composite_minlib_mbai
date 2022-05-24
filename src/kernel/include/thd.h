/**
 * Copyright 2014 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#ifndef THD_H
#define THD_H

#include "component.h"
#include "cap_ops.h"
#include "fpu_regs.h"
#include "chal/cpuid.h"
#include "chal/call_convention.h"
#include "chal/chal_thd.h"
#include "pgtbl.h"
#include "isb.h"
#include "retype_tbl.h"
#include "tcap.h"
#include "list.h"


#define THD_INVSTK_MAXSZ 32


/*
 * Thread capability descriptor that is minimal and contains only
 * consistency checking information (cpuid to ensure we're accessing
 * the thread on the correct core), and a pointer to the thread itself
 * that needs no synchronization (it is core-local, and interrupts are
 * disabled in the kernel).
 */
struct cap_thd {
	struct cap_header h;
	struct thread *   t;
	cpuid_t           cpuid;
} __attribute__((packed));

static void
thd_upcall_setup(struct thread *thd, vaddr_t entry_addr, int option, int arg1, int arg2, int arg3)
{
	regs_upcall_setup(&thd->regs, entry_addr, option, thd->tid | (get_cpuid() << 16), arg1, arg2, arg3);
}

/*
 * FIXME: We need global thread name space as we use thd_id to access
 * simple stacks. When we have low-level per comp stack free-list, we
 * don't have to use global thread id name space.
 *
 * Update: this is only partially true.  We should really just get rid
 * of this id in the kernel and replace it with a
 * scheduler-configurable variable.  That variable can be the thread
 * id where appropriate, and some other (component-controlled)
 * principal id otherwise.  Given this, the allocator should be in the
 * scheduler, not here.
 */
extern u32_t free_thd_id;
static u32_t
thdid_alloc(void)
{
	/* FIXME: thd id address space management. */
	if (unlikely(free_thd_id >= MAX_NUM_THREADS)) assert(0);
	return cos_faa((int *)&free_thd_id, 1);
}
static void
thd_rcvcap_take(struct thread *t)
{
	t->rcvcap.refcnt++;
}

static void
thd_rcvcap_release(struct thread *t)
{
	t->rcvcap.refcnt--;
}

static inline int
thd_rcvcap_isreferenced(struct thread *t)
{
	return t->rcvcap.refcnt > 0;
}

static inline int
thd_bound2rcvcap(struct thread *t)
{
	return t->rcvcap.isbound;
}

static inline int
thd_rcvcap_is_sched(struct thread *t)
{
	return thd_rcvcap_isreferenced(t);
}

static inline struct tcap *
thd_rcvcap_tcap(struct thread *t)
{
	return t->rcvcap.rcvcap_tcap;
}

static int
thd_rcvcap_isroot(struct thread *t)
{
	return t == t->rcvcap.rcvcap_thd_notif;
}

static inline struct thread *
thd_rcvcap_sched(struct thread *t)
{
	if (thd_rcvcap_isreferenced(t)) return t;
	return t->rcvcap.rcvcap_thd_notif;
}

static void
thd_next_thdinfo_update(struct cos_cpu_local_info *cli, struct thread *thd, struct tcap *tc, tcap_prio_t prio,
                        tcap_res_t budget)
{
	struct next_thdinfo *nti = &cli->next_ti;

	nti->thd    = thd;
	nti->tc     = tc;
	nti->prio   = prio;
	nti->budget = budget;
}

static void
thd_rcvcap_init(struct thread *t)
{
	struct rcvcap_info *rc = &t->rcvcap;

	rc->isbound = rc->pending = rc->refcnt = 0;
	rc->is_all_pending                     = 0;
	rc->sched_count                        = 0;
	rc->rcvcap_thd_notif                   = NULL;
}

static inline void
thd_rcvcap_evt_enqueue(struct thread *head, struct thread *t)
{
	if (list_empty(&t->event_list) && head != t) list_enqueue(&head->event_head, &t->event_list);
}

static inline void
thd_list_rem(struct thread *head, struct thread *t)
{
	list_rem(&t->event_list);
}

static inline struct thread *
thd_rcvcap_evt_dequeue(struct thread *head)
{
	return list_dequeue(&head->event_head);
}

/*
 * If events are going to be delivered on this thread, then we should
 * be tracking its execution time.  Thus, we co-opt the event list as
 * a notification trigger for tracking the thread's execution time.
 */
static inline int
thd_track_exec(struct thread *t)
{
	return !list_empty(&t->event_list);
}

static void
thd_rcvcap_all_pending_set(struct thread *t, int val)
{
	t->rcvcap.is_all_pending = val;
}

static int
thd_rcvcap_all_pending_get(struct thread *t)
{
	return t->rcvcap.is_all_pending;
}

static int
thd_rcvcap_all_pending(struct thread *t)
{
	int pending = t->rcvcap.pending;

	/* receive all pending */
	t->rcvcap.pending = 0;
	thd_rcvcap_all_pending_set(t, 0);

	return ((pending << 1) | !list_isempty(&t->event_head));
}

static int
thd_rcvcap_pending(struct thread *t)
{
	if (t->rcvcap.pending) return t->rcvcap.pending;
	return !list_isempty(&t->event_head);
	;
}

static sched_tok_t
thd_rcvcap_get_counter(struct thread *t)
{
	return t->rcvcap.sched_count;
}

static void
thd_rcvcap_set_counter(struct thread *t, sched_tok_t cntr)
{
	t->rcvcap.sched_count = cntr;
}

static void
thd_rcvcap_pending_inc(struct thread *arcvt)
{
	arcvt->rcvcap.pending++;
}

static int
thd_rcvcap_pending_dec(struct thread *arcvt)
{
	int pending = arcvt->rcvcap.pending;

	if (pending == 0) return 0;
	arcvt->rcvcap.pending--;

	return pending;
}

static inline int
thd_state_evt_deliver(struct thread *t, unsigned long *thd_state, unsigned long *cycles, unsigned long *timeout)
{
	struct thread *e = thd_rcvcap_evt_dequeue(t);

	assert(thd_bound2rcvcap(t));
	if (!e) return 0;

	*thd_state = e->tid | (e->state & THD_STATE_RCVING ? (thd_rcvcap_pending(e) ? 0 : 1 << 31) : 0);
	*cycles    = e->exec;
	e->exec    = 0;
	*timeout   = e->timeout;
	e->timeout = 0;

	return 1;
}

static inline struct thread *
thd_current(struct cos_cpu_local_info *cos_info)
{
	return (struct thread *)(cos_info->curr_thd);
}

static inline void
thd_current_update(struct thread *next, struct thread *prev, struct cos_cpu_local_info *cos_info)
{
	/* commit the cached data */
	prev->invstk_top     = cos_info->invstk_top;
	cos_info->invstk_top = next->invstk_top;
	cos_info->curr_thd   = next;
}

static inline struct thread *
thd_scheduler(struct thread *thd)
{
	return thd->scheduler_thread;
}

static inline void
thd_scheduler_set(struct thread *thd, struct thread *sched)
{
	if (unlikely(thd->scheduler_thread != sched)) thd->scheduler_thread = sched;
}

static int
thd_activate(struct captbl *t, capid_t cap, capid_t capin, struct thread *thd, capid_t compcap, thdclosure_index_t init_data, struct cos_ulinvstk *ulinvstk)
{
	struct cos_cpu_local_info *cli = cos_cpu_local_info();
	struct cap_thd            *tc;
	struct cap_comp           *compc;
	struct cap_isb            *isbc;
	int                        ret;

	memset(thd, 0, sizeof(struct thread));
	compc = (struct cap_comp *)captbl_lkup(t, compcap);
	if (unlikely(!compc || compc->h.type != CAP_COMP)) return -EINVAL;

	tc = (struct cap_thd *)__cap_capactivate_pre(t, cap, capin, CAP_THD, &ret);
	if (!tc) return ret;

	/* initialize the thread */
	memcpy(&(thd->invstk[0].comp_info), &compc->info, sizeof(struct comp_info));
	thd->invstk[0].ip = thd->invstk[0].sp = 0;
	thd->tid                              = thdid_alloc();
	thd->refcnt                           = 1;
	thd->invstk_top                       = 0;
	thd->cpuid                            = get_cpuid();
	thd->ulinvstk                         = ulinvstk;
	thd->ulinvstk_ktop                    = 0;
	assert(thd->tid <= MAX_NUM_THREADS);
	thd_scheduler_set(thd, thd_current(cli));

	thd_rcvcap_init(thd);
	list_head_init(&thd->event_head);
	list_init(&thd->event_list, thd);

	thd_upcall_setup(thd, compc->entry_addr, COS_UPCALL_THD_CREATE, init_data, 0, 0);

	/* initialize the capability */
	tc->t     = thd;
	tc->cpuid = get_cpuid();
	__cap_capactivate_post(&tc->h, CAP_THD);

	return 0;
}

static int
thd_deactivate(struct captbl *ct, struct cap_captbl *dest_ct, unsigned long capin, livenessid_t lid, capid_t pgtbl_cap,
               capid_t cosframe_addr, const int root)
{
	struct cos_cpu_local_info *cli = cos_cpu_local_info();
	struct cap_header *        thd_header;
	struct thread *            thd;
	unsigned long              old_v = 0, *pte = NULL;
	int                        ret;

	thd_header = captbl_lkup(dest_ct->captbl, capin);
	if (!thd_header || thd_header->type != CAP_THD) cos_throw(err, -EINVAL);
	thd = ((struct cap_thd *)thd_header)->t;
	assert(thd->refcnt);

	if (thd->refcnt == 1) {
		if (!root) cos_throw(err, -EINVAL);
		/* last ref cannot be removed if bound to arcv cap */
		if (thd_bound2rcvcap(thd)) cos_throw(err, -EBUSY);
		/*
		 * Last reference. Require pgtbl and cos_frame cap to
		 * release the kmem page.
		 */
		ret = kmem_deact_pre(thd_header, ct, pgtbl_cap, cosframe_addr, &pte, &old_v);
		if (ret) cos_throw(err, ret);
	} else {
		/* more reference exists. */
		if (root) cos_throw(err, -EINVAL);
		assert(thd->refcnt > 1);
		if (pgtbl_cap || cosframe_addr) {
			/* we pass in the pgtbl cap and frame addr,
			 * but ref_cnt is > 1. We'll ignore the two
			 * parameters as we won't be able to release
			 * the memory. */
			printk("cos: deactivating thread but not able to release kmem page (%p) yet (ref_cnt %d).\n",
			       (void *)cosframe_addr, thd->refcnt);
		}
	}

	ret = cap_capdeactivate(dest_ct, capin, CAP_THD, lid);
	if (ret) cos_throw(err, ret);

	thd->refcnt--;
	/* deactivation success */
	if (thd->refcnt == 0) {
		if (cli->next_ti.thd == thd) thd_next_thdinfo_update(cli, 0, 0, 0, 0);

		/* move the kmem for the thread to a location
		 * in a pagetable as COSFRAME */
		ret = kmem_deact_post(pte, old_v);
		if (ret) cos_throw(err, ret);
	}

	return 0;
err:
	return ret;
}

static int
thd_tls_set(struct captbl *ct, capid_t thd_cap, vaddr_t tlsaddr, struct thread *current)
{
	struct cap_thd *tc;
	struct thread * thd;

	tc = (struct cap_thd *)captbl_lkup(ct, thd_cap);
	if (!tc || tc->h.type != CAP_THD || get_cpuid() != tc->cpuid) return -EINVAL;

	thd = tc->t;
	assert(thd);
	thd->tls = tlsaddr;

	if (current == thd) chal_tls_update(tlsaddr);

	return 0;
}

static void
thd_init(void)
{
	assert(sizeof(struct cap_thd) <= __captbl_cap2bytes(CAP_THD));
	// assert(offsetof(struct thread, regs) == 4); /* see THD_REGS in entry.S */
}

static inline struct comp_info *
thd_invstk_current(struct thread *curr_thd, unsigned long *ip, unsigned long *sp, struct cos_cpu_local_info *cos_info)
{
	return chal_invstk_current(curr_thd, ip, sp, cos_info);
}

static inline pgtbl_t
thd_current_pgtbl(struct thread *thd)
{
	return chal_current_pgtbl(thd);
}

static inline int
thd_invstk_push(struct thread *thd, struct comp_info *ci, unsigned long ip, unsigned long sp,
                struct cos_cpu_local_info *cos_info)
{
	return chal_invstk_push(thd, ci, ip, sp, cos_info);
}

static inline struct comp_info *
thd_invstk_pop(struct thread *thd, unsigned long *ip, unsigned long *sp, struct cos_cpu_local_info *cos_info)
{
	return chal_invstk_pop(thd, ip, sp, cos_info);
}

static inline void
thd_preemption_state_update(struct thread *curr, struct thread *next, struct pt_regs *regs)
{
	curr->state |= THD_STATE_PREEMPTED;
	next->interrupted_thread = curr;
	memcpy(&curr->regs, regs, sizeof(struct pt_regs));
}

static inline void
thd_rcvcap_pending_deliver(struct thread *thd, struct pt_regs *regs)
{
	unsigned long thd_state = 0, cycles = 0, timeout = 0, pending = 0;
	int           all_pending = thd_rcvcap_all_pending_get(thd);

	thd_state_evt_deliver(thd, &thd_state, &cycles, &timeout);
	if (all_pending) {
		pending = thd_rcvcap_all_pending(thd);
	} else {
		thd_rcvcap_pending_dec(thd);
		pending = thd_rcvcap_pending(thd);
	}
	__userregs_setretvals(regs, pending, thd_state, cycles, timeout);
}

static inline int
thd_switch_update(struct thread *thd, struct pt_regs *regs, int issame)
{
	int preempt = 0;

	/* TODO: check FPU */
	/* fpu_save(thd); */
	if (thd->state & THD_STATE_PREEMPTED) {
		assert(!(thd->state & THD_STATE_RCVING));
		thd->state &= ~THD_STATE_PREEMPTED;
		preempt = 1;
	} else if (thd->state & THD_STATE_RCVING) {
		assert(!(thd->state & THD_STATE_PREEMPTED));
		thd->state &= ~THD_STATE_RCVING;
		thd_rcvcap_pending_deliver(thd, regs);

		/*
		 * If a scheduler thread was running using child tcap and blocked on RCVING
		 * and budget expended logic decided to run the scheduler thread with it's
		 * tcap, then curr_thd == next_thd and state will be RCVING.
		 */
	}

	if (issame && preempt == 0) {
		__userregs_set(regs, 0, __userregs_getsp(regs), __userregs_getip(regs));
	}

	return preempt;
}

static inline int
thd_introspect(struct thread *t, unsigned long op, unsigned long *retval)
{
	switch (op) {
	case THD_GET_TID:
		*retval = t->tid;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#endif /* THD_H */
