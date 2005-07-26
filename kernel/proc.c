/* This file contains essentially all of the process and message handling.
 * Together with "mpx.s" it forms the lowest layer of the MINIX kernel.
 * There is one entry point from the outside:
 *
 *   sys_call: 	      a system call, i.e., the kernel is trapped with an INT 
 *
 * As well as several entry points used from the interrupt and task level:
 *
 *   lock_notify:     notify a process of a system event
 *   lock_send:	      send a message to a process
 *   lock_ready:      put a process on one of the ready queues 
 *   lock_unready:    remove a process from the ready queues
 *   lock_sched:      a process has run too long; schedule another one
 *
 * Changes:
 *         , 2005     better protection in sys_call()  (Jorrit N. Herder)
 *   May 26, 2005     optimized message passing functions  (Jorrit N. Herder)
 *   May 24, 2005     new, queued NOTIFY system call  (Jorrit N. Herder)
 *   Oct 28, 2004     new, non-blocking SEND and RECEIVE  (Jorrit N. Herder)
 *   Oct 28, 2004     rewrite of sys_call() function  (Jorrit N. Herder)
 *   Aug 19, 2004     generalized multilevel scheduling  (Jorrit N. Herder)
 *
 * The code here is critical to make everything work and is important for the
 * overall performance of the system. A large fraction of the code deals with
 * list manipulation. To make this both easy to understand and fast to execute 
 * pointer pointers are used throughout the code. Pointer pointers prevent
 * exceptions for the head or tail of a linked list. 
 *
 *  node_t *queue, *new_node;	// assume these as global variables
 *  node_t **xpp = &queue; 	// get pointer pointer to head of queue 
 *  while (*xpp != NULL) 	// find last pointer of the linked list
 *      xpp = &(*xpp)->next;	// get pointer to next pointer 
 *  *xpp = new_node;		// now replace the end (the NULL pointer) 
 *  new_node->next = NULL;	// and mark the new end of the list
 * 
 * For example, when adding a new node to the end of the list, one normally 
 * makes an exception for an empty list and looks up the end of the list for 
 * nonempty lists. As shown above, this is not required with pointer pointers.
 */

#include <minix/com.h>
#include <minix/callnr.h>
#include "kernel.h"
#include "proc.h"


/* Scheduling and message passing functions. The functions are available to 
 * other parts of the kernel through lock_...(). The lock temporarily disables 
 * interrupts to prevent race conditions. 
 */
FORWARD _PROTOTYPE( int mini_send, (struct proc *caller_ptr, int dst,
		message *m_ptr, unsigned flags) );
FORWARD _PROTOTYPE( int mini_receive, (struct proc *caller_ptr, int src,
		message *m_ptr, unsigned flags) );
FORWARD _PROTOTYPE( int mini_alert, (struct proc *caller_ptr, int dst) );
FORWARD _PROTOTYPE( int mini_notify, (struct proc *caller_ptr, int dst,
		message *m_ptr ) );

FORWARD _PROTOTYPE( void ready, (struct proc *rp) );
FORWARD _PROTOTYPE( void unready, (struct proc *rp) );
FORWARD _PROTOTYPE( void sched, (struct proc *rp) );
FORWARD _PROTOTYPE( void pick_proc, (void) );


#if TEMP_CODE
#define BuildOldMess(m,n) \
	(m).NOTIFY_SOURCE = (n)->n_source, \
	(m).NOTIFY_TYPE = (n)->n_type, \
	(m).NOTIFY_FLAGS = (n)->n_flags, \
	(m).NOTIFY_ARG = (n)->n_arg;
#endif

#define BuildMess(m_ptr, src, dst_ptr) \
	(m_ptr)->m_source = (src); 					\
	(m_ptr)->m_type = NOTIFY_FROM(src);				\
	(m_ptr)->NOTIFY_TIMESTAMP = get_uptime();			\
	switch (src) {							\
	case HARDWARE:							\
		(m_ptr)->NOTIFY_ARG = priv(dst_ptr)->s_int_pending;	\
		priv(dst_ptr)->s_int_pending = 0;			\
		break;							\
	case SYSTEM:							\
		(m_ptr)->NOTIFY_ARG = priv(dst_ptr)->s_sig_pending;	\
		priv(dst_ptr)->s_sig_pending = 0;			\
		break;							\
	}

#if (CHIP == INTEL)
#define CopyMess(s,sp,sm,dp,dm) \
	cp_mess(s, (sp)->p_memmap[D].mem_phys, (vir_bytes)sm, (dp)->p_memmap[D].mem_phys, (vir_bytes)dm)
#endif /* (CHIP == INTEL) */

#if (CHIP == M68000)
/* M68000 does not have cp_mess() in assembly like INTEL. Declare prototype
 * for cp_mess() here and define the function below. Also define CopyMess. 
 */
#endif /* (CHIP == M68000) */



/*===========================================================================*
 *				sys_call				     * 
 *===========================================================================*/
PUBLIC int sys_call(call_nr, src_dst, m_ptr)
int call_nr;			/* system call number and flags */
int src_dst;			/* src to receive from or dst to send to */
message *m_ptr;			/* pointer to message in the caller's space */
{
/* System calls are done by trapping to the kernel with an INT instruction.
 * The trap is caught and sys_call() is called to send or receive a message
 * (or both). The caller is always given by 'proc_ptr'.
 */
  register struct proc *caller_ptr = proc_ptr;	/* get pointer to caller */
  int function = call_nr & SYSCALL_FUNC;	/* get system call function */
  unsigned flags = call_nr & SYSCALL_FLAGS;	/* get flags */
  int mask_entry;				/* bit to check in send mask */
  int result;					/* the system call's result */
  vir_bytes vb;			/* message buffer pointer as vir_bytes */
  vir_clicks vlo, vhi;		/* virtual clicks containing message to send */

  /* Check if the process has privileges for the requested call. Calls to the 
   * kernel may only be SENDREC, because tasks always reply and may not block 
   * if the caller doesn't do receive(). 
   */
  if (! (priv(caller_ptr)->s_call_mask & (1 << function)) || 
          (iskerneln(src_dst) && function != SENDREC))  
      return(ECALLDENIED);	
  
  /* Require a valid source and/ or destination process, unless echoing. */
  if (! (isokprocn(src_dst) || src_dst == ANY || function == ECHO))  
      return(EBADSRCDST);

  /* If the call involves a message buffer, i.e., for SEND, RECEIVE, SENDREC, 
   * or ECHO, check the message pointer. This check allows a message to be 
   * anywhere in data or stack or gap. It will have to be made more elaborate 
   * for machines which don't have the gap mapped. 
   */
  if (function & SENDREC) {	
      vb = (vir_bytes) m_ptr;				/* virtual clicks */
      vlo = vb >> CLICK_SHIFT;				/* bottom of message */
      vhi = (vb + MESS_SIZE - 1) >> CLICK_SHIFT;	/* top of message */
      if (vlo < caller_ptr->p_memmap[D].mem_vir || vlo > vhi ||
              vhi >= caller_ptr->p_memmap[S].mem_vir + 
              caller_ptr->p_memmap[S].mem_len)  return(EFAULT); 
  }

  /* If the call is to send to a process, i.e., for SEND, SENDREC or NOTIFY,
   * verify that the caller is allowed to send to the given destination and
   * that the destination is still alive. 
   */
  if (function & SEND) {	
      if (! get_sys_bit(priv(caller_ptr)->s_send_mask, nr_to_id(src_dst))) {
          kprintf("Warning, send_mask denied %d sending to %d\n",
          	proc_nr(caller_ptr), src_dst);
          return(ECALLDENIED);
      }

      if (isemptyn(src_dst)) return(EDEADDST); 	/* cannot send to the dead */
  }

  /* Now check if the call is known and try to perform the request. The only
   * system calls that exist in MINIX are sending and receiving messages.
   *   - SENDREC: combines SEND and RECEIVE in a single system call
   *   - SEND:    sender blocks until its message has been delivered
   *   - RECEIVE: receiver blocks until an acceptable message has arrived
   *   - NOTIFY:  nonblocking call; deliver notification or mark pending
   *   - ECHO:    nonblocking call; directly echo back the message 
   */
  switch(function) {
  case SENDREC:					/* has FRESH_ANSWER flag */		
      /* fall through */
  case SEND:			
      result = mini_send(caller_ptr, src_dst, m_ptr, flags);
      if (function == SEND || result != OK) {	
          break;				/* done, or SEND failed */
      }						/* fall through for SENDREC */
  case RECEIVE:			
      result = mini_receive(caller_ptr, src_dst, m_ptr, flags);
      break;
  case ALERT:
      result = mini_alert(caller_ptr, src_dst);
      break;
  case NOTIFY:
      result = mini_notify(caller_ptr, src_dst, m_ptr);
      break;
  case ECHO:
      CopyMess(caller_ptr->p_nr, caller_ptr, m_ptr, caller_ptr, m_ptr);
      result = OK;
      break;
  default:
      result = EBADCALL;			/* illegal system call */
  }

  /* Now, return the result of the system call to the caller. */
  return(result);
}


/*===========================================================================*
 *				mini_send				     * 
 *===========================================================================*/
PRIVATE int mini_send(caller_ptr, dst, m_ptr, flags)
register struct proc *caller_ptr;	/* who is trying to send a message? */
int dst;				/* to whom is message being sent? */
message *m_ptr;				/* pointer to message buffer */
unsigned flags;				/* system call flags */
{
/* Send a message from 'caller_ptr' to 'dst'. If 'dst' is blocked waiting
 * for this message, copy the message to it and unblock 'dst'. If 'dst' is
 * not waiting at all, or is waiting for another source, queue 'caller_ptr'.
 */
  register struct proc *dst_ptr = proc_addr(dst);
  register struct proc **xpp;
  register struct proc *xp;

  /* Check for deadlock by 'caller_ptr' and 'dst' sending to each other. */
  xp = dst_ptr;
  while (xp->p_rts_flags & SENDING) {		/* check while sending */
  	xp = proc_addr(xp->p_sendto);		/* get xp's destination */
  	if (xp == caller_ptr) return(ELOCKED);	/* deadlock if cyclic */
  }

  /* Check if 'dst' is blocked waiting for this message. The destination's 
   * SENDING flag may be set when its SENDREC call blocked while sending.  
   */
  if ( (dst_ptr->p_rts_flags & (RECEIVING | SENDING)) == RECEIVING &&
       (dst_ptr->p_getfrom == ANY || dst_ptr->p_getfrom == caller_ptr->p_nr)) {
	/* Destination is indeed waiting for this message. */
	CopyMess(caller_ptr->p_nr, caller_ptr, m_ptr, dst_ptr,
		 dst_ptr->p_messbuf);
	if ((dst_ptr->p_rts_flags &= ~RECEIVING) == 0) ready(dst_ptr);
  } else if ( ! (flags & NON_BLOCKING)) {
	/* Destination is not waiting.  Block and queue caller. */
	caller_ptr->p_messbuf = m_ptr;
	if (caller_ptr->p_rts_flags == 0) unready(caller_ptr);
	caller_ptr->p_rts_flags |= SENDING;
	caller_ptr->p_sendto = dst;

	/* Process is now blocked.  Put in on the destination's queue. */
	xpp = &dst_ptr->p_caller_q;		/* find end of list */
	while (*xpp != NIL_PROC) xpp = &(*xpp)->p_q_link;
	*xpp = caller_ptr;			/* add caller to end */
	caller_ptr->p_q_link = NIL_PROC;	/* mark new end of list */
  } else {
	return(ENOTREADY);
  }
  return(OK);
}

/*===========================================================================*
 *				mini_receive				     * 
 *===========================================================================*/
PRIVATE int mini_receive(caller_ptr, src, m_ptr, flags)
register struct proc *caller_ptr;	/* process trying to get message */
int src;				/* which message source is wanted */
message *m_ptr;				/* pointer to message buffer */
unsigned flags;				/* system call flags */
{
/* A process or task wants to get a message.  If a message is already queued,
 * acquire it and deblock the sender.  If no message from the desired source
 * is available block the caller, unless the flags don't allow blocking.  
 */
  register struct proc **xpp;
  register struct notification **ntf_q_pp;
  message m;
  int bit_nr;
  sys_map_t *map;
  bitchunk_t *chunk;
  int i, src_id, src_proc_nr;

  /* Check to see if a message from desired source is already available.
   * The caller's SENDING flag may be set if SENDREC couldn't send. If it is
   * set, the process should be blocked.
   */
  if (!(caller_ptr->p_rts_flags & SENDING)) {

    /* Check if there are pending notifications, except for SENDREC. */
    if (! (flags & FRESH_ANSWER)) {

        map = &priv(caller_ptr)->s_notify_pending;
        for (chunk=&map->chunk[0]; chunk<&map->chunk[NR_SYS_CHUNKS]; chunk++) {

            /* Find a pending notification from the requested source. */ 
            if (! *chunk) continue; 			/* no bits in chunk */
            for (i=0; ! (*chunk & (1<<i)); ++i) {} 	/* look up the bit */
            src_id = (chunk - &map->chunk[0]) * BITCHUNK_BITS + i;
            if (src_id >= NR_SYS_PROCS) break;		/* out of range */
            src_proc_nr = id_to_nr(src_id);		/* get source proc */
            if (src!=ANY && src!=src_proc_nr) continue;	/* source not ok */
            *chunk &= ~(1 << i);			/* no longer pending */

            /* Found a suitable source, deliver the notification message. */
	    BuildMess(&m, src_proc_nr, caller_ptr);	/* assemble message */
            CopyMess(src_proc_nr, proc_addr(HARDWARE), &m, caller_ptr, m_ptr);
            return(OK);					/* report success */
        }

#if TEMP_CODE
        ntf_q_pp = &caller_ptr->p_ntf_q;	/* get pointer pointer */
        while (*ntf_q_pp != NULL) {
            if (src == ANY || src == (*ntf_q_pp)->n_source) {
		/* Found notification. Assemble and copy message. */
		BuildOldMess(m, *ntf_q_pp);
     		if (m.m_source == HARDWARE) {
          		m.NOTIFY_ARG = caller_ptr->p_priv->s_int_pending;
          		caller_ptr->p_priv->s_int_pending = 0;
      		}
                CopyMess((*ntf_q_pp)->n_source, proc_addr(HARDWARE), &m, 
                	caller_ptr, m_ptr);
                /* Remove notification from queue and bit map. */
                bit_nr = (int) (*ntf_q_pp - &notify_buffer[0]);  
                *ntf_q_pp = (*ntf_q_pp)->n_next;/* remove from queue */
                free_bit(bit_nr, notify_bitmap, NR_NOTIFY_BUFS);
                return(OK);			/* report success */
	    }
	    ntf_q_pp = &(*ntf_q_pp)->n_next;	/* proceed to next */
        }
    }
#endif

    /* Check caller queue. Use pointer pointers to keep code simple. */
    xpp = &caller_ptr->p_caller_q;
    while (*xpp != NIL_PROC) {
	if (src == ANY || src == proc_nr(*xpp)) {
	    /* Found acceptable message. Copy it and update status. */
	    CopyMess((*xpp)->p_nr, *xpp, (*xpp)->p_messbuf, caller_ptr, m_ptr);
            if (((*xpp)->p_rts_flags &= ~SENDING) == 0) ready(*xpp);
            *xpp = (*xpp)->p_q_link;		/* remove from queue */
            return(OK);				/* report success */
	}
	xpp = &(*xpp)->p_q_link;		/* proceed to next */
    }

  }

  /* No suitable message is available or the caller couldn't send in SENDREC. 
   * Block the process trying to receive, unless the flags tell otherwise.
   */
  if ( ! (flags & NON_BLOCKING)) {
      caller_ptr->p_getfrom = src;		
      caller_ptr->p_messbuf = m_ptr;
      if (caller_ptr->p_rts_flags == 0) unready(caller_ptr);
      caller_ptr->p_rts_flags |= RECEIVING;		
      return(OK);
  } else {
      return(ENOTREADY);
  }
}


/*===========================================================================*
 *				mini_alert				     * 
 *===========================================================================*/
PRIVATE int mini_alert(caller_ptr, dst)
register struct proc *caller_ptr;	/* sender of the notification */
int dst;				/* which process to notify */
{
  register struct proc *dst_ptr = proc_addr(dst);
  int src_id;				/* source id for late delivery */
  message m;				/* the notification message */

  /* Check to see if target is blocked waiting for this message. A process 
   * can be both sending and receiving during a SENDREC system call.
   */
  if ((dst_ptr->p_rts_flags & (RECEIVING|SENDING)) == RECEIVING &&
      (dst_ptr->p_getfrom == ANY || dst_ptr->p_getfrom == caller_ptr->p_nr)) {

      /* Destination is indeed waiting for a message. Assemble a notification 
       * message and deliver it. Copy from pseudo-source HARDWARE, since the
       * message is in the kernel's address space.
       */ 
      BuildMess(&m, proc_nr(caller_ptr), dst_ptr);
      CopyMess(proc_nr(caller_ptr), proc_addr(HARDWARE), &m, 
          dst_ptr, dst_ptr->p_messbuf);
      dst_ptr->p_rts_flags &= ~RECEIVING;	/* deblock destination */
      if (dst_ptr->p_rts_flags == 0) ready(dst_ptr);
      return(OK);
  } 

  /* Destination is not ready to receive the notification. Add it to the 
   * bit map with pending notifications. Note the indirectness: the system id 
   * instead of the process number is used in the pending bit map.
   */ 
  src_id = priv(caller_ptr)->s_id;
  set_sys_bit(priv(dst_ptr)->s_notify_pending, src_id); 
  return(OK);
}


/*===========================================================================*
 *				mini_notify				     * 
 *===========================================================================*/
PRIVATE int mini_notify(caller_ptr, dst, m_ptr)
register struct proc *caller_ptr;	/* process trying to notify */
int dst;				/* which process to notify */
message *m_ptr;				/* pointer to message buffer */
{
  register struct proc *dst_ptr = proc_addr(dst);
  register struct notification *ntf_p ;
  register struct notification **ntf_q_pp;
  int ntf_index;
  message ntf_mess;

  /* Check to see if target is blocked waiting for this message. A process 
   * can be both sending and receiving during a SENDREC system call.
   */
  if ((dst_ptr->p_rts_flags & (RECEIVING|SENDING)) == RECEIVING &&
      (dst_ptr->p_getfrom == ANY || dst_ptr->p_getfrom == caller_ptr->p_nr)) {

      /* Destination is indeed waiting for this message. Check if the source
       * is HARDWARE; this is a special case that gets the map of pending
       * interrupts as an argument. Then deliver the notification message. 
       */
      if (proc_nr(caller_ptr) == HARDWARE) {
          m_ptr->NOTIFY_ARG = priv(dst_ptr)->s_int_pending;
          priv(dst_ptr)->s_int_pending = 0;
      }

      CopyMess(proc_nr(caller_ptr), caller_ptr, m_ptr, dst_ptr, dst_ptr->p_messbuf);
      dst_ptr->p_rts_flags &= ~RECEIVING;	/* deblock destination */
      if (dst_ptr->p_rts_flags == 0) ready(dst_ptr);
      return(OK);
  } 

  /* Destination is not ready. Add the notification to the pending queue. 
   * Get pointer to notification message. Don't copy if already in kernel. 
   */
  if (! iskernelp(caller_ptr)) {
      CopyMess(proc_nr(caller_ptr), caller_ptr, m_ptr, 
          proc_addr(HARDWARE), &ntf_mess);
      m_ptr = &ntf_mess;
  }

  /* Enqueue the message. Existing notifications with the same source
   * and type are overwritten with newer ones. New notifications that
   * are not yet on the list are added to the end.
   */
  ntf_q_pp = &dst_ptr->p_ntf_q;
  while (*ntf_q_pp != NULL) {
      /* Replace notifications with same source and type. */
      if ((*ntf_q_pp)->n_type == m_ptr->NOTIFY_TYPE && 
              (*ntf_q_pp)->n_source == proc_nr(caller_ptr)) {
          (*ntf_q_pp)->n_flags = m_ptr->NOTIFY_FLAGS;
          (*ntf_q_pp)->n_arg = m_ptr->NOTIFY_ARG;
          return(OK);
      }
      ntf_q_pp = &(*ntf_q_pp)->n_next;
  }

  /* Add to end of queue (found above). Get a free notification buffer. */
  if ((ntf_index = alloc_bit(notify_bitmap, NR_NOTIFY_BUFS)) < 0)  
      return(ENOSPC);
  ntf_p = &notify_buffer[ntf_index];	/* get pointer to buffer */
  ntf_p->n_source = proc_nr(caller_ptr);/* store notification data */
  ntf_p->n_type = m_ptr->NOTIFY_TYPE;
  ntf_p->n_flags = m_ptr->NOTIFY_FLAGS;
  ntf_p->n_arg = m_ptr->NOTIFY_ARG;
  *ntf_q_pp = ntf_p;			/* add to end of queue */
  ntf_p->n_next = NULL;			/* mark new end of queue */
  return(OK);
}

/*==========================================================================*
 *				lock_notify				    *
 *==========================================================================*/
PUBLIC int lock_alert(src, dst)
int src;			/* sender of the notification */
int dst;			/* who is to be notified */
{
/* Safe gateway to mini_notify() for tasks and interrupt handlers. The sender
 * is explicitely given to prevent confusion where the call comes from. MINIX 
 * kernel is not reentrant, which means to interrupts are disabled after 
 * the first kernel entry (hardware interrupt, trap, or exception). Locking
 * is done by temporarily disabling interrupts. 
 */
  int result;

  /* Exception or interrupt occurred, thus already locked. */
  if (k_reenter >= 0) {
      result = mini_alert(proc_addr(src), dst); 
  }

  /* Call from task level, locking is required. */
  else {
      lock(0, "alert");
      result = mini_alert(proc_addr(src), dst); 
      unlock(0);
  }
  return(result);
}


/*===========================================================================*
 *				ready					     * 
 *===========================================================================*/
PRIVATE void ready(rp)
register struct proc *rp;	/* this process is now runnable */
{
/* Add 'rp' to one of the queues of runnable processes.  */
  register int q = rp->p_priority;		/* scheduling queue to use */

#if DEBUG_SCHED_CHECK
  check_runqueues("ready");
  if(rp->p_ready) kprintf("ready() already ready process\n");
#endif

  /* Processes, in principle, are added to the end of the queue. However, 
   * user processes are added in front of the queue, because this is a bit 
   * fairer to I/O bound processes. 
   */
  if (rdy_head[q] == NIL_PROC) {		/* add to empty queue */
      rdy_head[q] = rdy_tail[q] = rp; 		/* create a new queue */
      rp->p_nextready = NIL_PROC;		/* mark new end */
  } 
  else if (priv(rp)->s_flags & RDY_Q_HEAD) {    /* add to head of queue */
      rp->p_nextready = rdy_head[q];		/* chain head of queue */
      rdy_head[q] = rp;				/* set new queue head */
  } 
  else {					/* add to tail of queue */
      rdy_tail[q]->p_nextready = rp;		/* chain tail of queue */	
      rdy_tail[q] = rp;				/* set new queue tail */
      rp->p_nextready = NIL_PROC;		/* mark new end */
  }
  pick_proc();					/* select next to run */

#if DEBUG_SCHED_CHECK
  rp->p_ready = 1;
  check_runqueues("ready");
#endif
}

/*===========================================================================*
 *				unready					     * 
 *===========================================================================*/
PRIVATE void unready(rp)
register struct proc *rp;	/* this process is no longer runnable */
{
/* A process has blocked. See ready for a description of the queues. */

  register int q = rp->p_priority;		/* queue to use */
  register struct proc **xpp;			/* iterate over queue */
  register struct proc *prev_xp;

  /* Side-effect for kernel: check if the task's stack still is ok? */
  if (iskernelp(rp)) { 				
	if (*priv(rp)->s_stack_guard != STACK_GUARD)
		panic("stack overrun by task", proc_nr(rp));
  }

#if DEBUG_SCHED_CHECK
  check_runqueues("unready");
  if (! rp->p_ready) kprintf("unready() already unready process\n");
#endif

  /* Now make sure that the process is not in its ready queue. Remove the 
   * process if it is found. A process can be made unready even if it is not 
   * running by being sent a signal that kills it.
   */
  prev_xp = NIL_PROC;				
  for (xpp = &rdy_head[q]; *xpp != NIL_PROC; xpp = &(*xpp)->p_nextready) {

      if (*xpp == rp) {				/* found process to remove */
          *xpp = (*xpp)->p_nextready;		/* replace with next chain */
          if (rp == rdy_tail[q])		/* queue tail removed */
              rdy_tail[q] = prev_xp;		/* set new tail */
          if (rp == proc_ptr || rp == next_ptr)	/* active process removed */
              pick_proc();			/* pick new process to run */
          break;
      }
      prev_xp = *xpp;				/* save previous in chain */
  }
  
  /* The caller blocked. Reset the scheduling priority and quantums allowed.
   * The process' priority may have been lowered if a process consumed too 
   * many full quantums in a row to prevent damage from infinite loops 
   */
  rp->p_priority = rp->p_max_priority;
  rp->p_full_quantums = QUANTUMS(rp->p_priority);

#if DEBUG_SCHED_CHECK
  rp->p_ready = 0;
  check_runqueues("unready");
#endif
}

/*===========================================================================*
 *				sched					     * 
 *===========================================================================*/
PRIVATE void sched(sched_ptr)
struct proc *sched_ptr;				/* quantum eating process */
{
  int q;

  /* Check if this process is preemptible, otherwise leave it as is. */
  if (! (priv(sched_ptr)->s_flags & PREEMPTIBLE))  return;

  /* Process exceeded the maximum number of full quantums it is allowed
   * to use in a row. Lower the process' priority, but make sure we don't 
   * end up in the IDLE queue. This helps to limit the damage caused by 
   * for example infinite loops in high-priority processes. 
   * This is a rare situation, so the overhead is acceptable.  
   */
  if (-- sched_ptr->p_full_quantums <= 0) {	/* exceeded threshold */ 
      if (sched_ptr->p_priority + 1 < IDLE_Q ) {
	  q = sched_ptr->p_priority + 1;	/* backup new priority */
          unready(sched_ptr);			/* remove from queues */
          sched_ptr->p_priority = q; 		/* lower priority */
          ready(sched_ptr);			/* add to new queue */
      }
      sched_ptr->p_full_quantums = QUANTUMS(sched_ptr->p_priority);
  }

  /* The current process has run too long. If another low priority (user)
   * process is runnable, put the current process on the tail of its queue,
   * possibly promoting another user to head of the queue. Don't do anything
   * if the queue is empty, or the process to be scheduled is not the head.
   */
  q = sched_ptr->p_priority;			/* convenient shorthand */
  if (rdy_head[q] == sched_ptr) {		  
      rdy_tail[q]->p_nextready = rdy_head[q];  	/* add expired to end */
      rdy_tail[q] = rdy_head[q];	   	/* set new queue tail */
      rdy_head[q] = rdy_head[q]->p_nextready;  	/* set new queue head */
      rdy_tail[q]->p_nextready = NIL_PROC;   	/* mark new queue end */
  }

  /* Give the expired process a new quantum and see who is next to run. */
  sched_ptr->p_sched_ticks = sched_ptr->p_quantum_size;
  pick_proc();					
}


/*===========================================================================*
 *				pick_proc				     * 
 *===========================================================================*/
PRIVATE void pick_proc()
{
/* Decide who to run now.  A new process is selected by setting 'next_ptr'.
 * When a billable process is selected, record it in 'bill_ptr', so that the 
 * clock task can tell who to bill for system time.
 */
  register struct proc *rp;			/* process to run */
  int q;					/* iterate over queues */

  /* Check each of the scheduling queues for ready processes. The number of
   * queues is defined in proc.h, and priorities are set in the task table.
   * The lowest queue contains IDLE, which is always ready.
   */
  for (q=0; q < NR_SCHED_QUEUES; q++) {	
      if ( (rp = rdy_head[q]) != NIL_PROC) {
          next_ptr = rp;			/* run process 'rp' next */
          if (priv(rp)->s_flags & BILLABLE)	 	
              bill_ptr = rp;			/* bill for system time */
          return;				 
      }
  }
}


/*==========================================================================*
 *				lock_send				    *
 *==========================================================================*/
PUBLIC int lock_send(dst, m_ptr)
int dst;			/* to whom is message being sent? */
message *m_ptr;			/* pointer to message buffer */
{
/* Safe gateway to mini_send() for tasks. */
  int result;
  lock(2, "send");
  result = mini_send(proc_ptr, dst, m_ptr, NON_BLOCKING);
  unlock(2);
  return(result);
}


/*==========================================================================*
 *				lock_ready				    *
 *==========================================================================*/
PUBLIC void lock_ready(rp)
struct proc *rp;		/* this process is now runnable */
{
/* Safe gateway to ready() for tasks. */
  lock(3, "ready");
  ready(rp);
  unlock(3);
}

/*==========================================================================*
 *				lock_unready				    *
 *==========================================================================*/
PUBLIC void lock_unready(rp)
struct proc *rp;		/* this process is no longer runnable */
{
/* Safe gateway to unready() for tasks. */
  lock(4, "unready");
  unready(rp);
  unlock(4);
}

/*==========================================================================*
 *				lock_sched				    *
 *==========================================================================*/
PUBLIC void lock_sched(sched_ptr)
struct proc *sched_ptr;
{
/* Safe gateway to sched() for tasks. */
  lock(5, "sched");
  sched(sched_ptr);
  unlock(5);
}

