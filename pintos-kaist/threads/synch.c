/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

   #include "threads/synch.h"
   #include <stdio.h>
   #include <string.h>
   #include "threads/interrupt.h"
   #include "threads/thread.h"   
   bool cmp_sema_priority(struct list_elem *a, struct list_elem *b, void *aux);
   
   /* 세마포어 SEMA를 VALUE 값으로 초기화한다.
          세마포어는 음수가 될 수 없는 정수 값과 다음 두 연산으로 구성된다:

          - down 또는 "P": 값이 양수가 될 때까지 기다린 뒤 1 감소한다.

          - up 또는 "V": 값을 증가시키고 대기 중인 스레드가 있으면 한 개를 깨운다. */
   void
   sema_init (struct semaphore *sema, unsigned value) {
	   ASSERT (sema != NULL);
   
	   sema->value = value;
	   list_init (&sema->waiters);
   }
   
   /* 세마포어를 감소시키는 down(P) 연산.
          값이 양수가 될 때까지 기다렸다가 원자적으로 1 줄인다.

          이 함수는 슬립할 수 있으므로 인터럽트 핸들러 안에서는 호출하면 안 된다.
          인터럽트를 끈 상태로 호출할 수는 있지만, 잠들었다 깨어나면
          다음 스케줄된 스레드가 인터럽트를 다시 켤 수도 있다. */
   void
   sema_down (struct semaphore *sema) {
	   enum intr_level old_level;
   
	   ASSERT (sema != NULL);
	   ASSERT (!intr_context ());
   
	   old_level = intr_disable ();
	   
	   /* sema의 값이 0일 때는 실행 중인 스레드를 우선순위대로 sema의 waiters 리스트에 삽입 */
	   while (sema->value == 0) {
		   list_insert_ordered(&sema->waiters, &thread_current ()->elem, cmp_priority, NULL);						
		   thread_block ();
	   }
	   sema->value--;
	   preempt_priority();
	   intr_set_level (old_level);	   
   }
   
   /* Down or "P" operation on a semaphore, but only if the
	  semaphore is not already 0.  Returns true if the semaphore is
	  decremented, false otherwise.
   
	  This function may be called from an interrupt handler. */
   bool
   sema_try_down (struct semaphore *sema) {
	   enum intr_level old_level;
	   bool success;
   
	   ASSERT (sema != NULL);
   
	   old_level = intr_disable ();
	   if (sema->value > 0)
	   {
		   sema->value--;
		   success = true;
	   }
	   else
		   success = false;
	   intr_set_level (old_level);
   
	   return success;
   }
   
   /* Up or "V" operation on a semaphore.  Increments SEMA's value
	  and wakes up one thread of those waiting for SEMA, if any.
   
	  This function may be called from an interrupt handler. */
   void
   sema_up (struct semaphore *sema) {
	   enum intr_level old_level;
   
	   ASSERT (sema != NULL);
   
	   old_level = intr_disable ();
	   if (!list_empty (&sema->waiters))
	   {
		    /* sema의 대기열에서 lock holder가 donation을 받아 우선순위가 바뀌는 경우를 상정하여 재정렬 */
			list_sort(&sema->waiters, cmp_priority, NULL);
			thread_unblock (list_entry (list_pop_front (&sema->waiters), struct thread, elem));
	   }
	   
	   sema->value++;
	   intr_set_level (old_level);
	   preempt_priority();
   }
   
   static void sema_test_helper (void *sema_);
   
   /* Self-test for semaphores that makes control "ping-pong"
	  between a pair of threads.  Insert calls to printf() to see
	  what's going on. */
   void
   sema_self_test (void) {
	   struct semaphore sema[2];
	   int i;
   
	   printf ("Testing semaphores...");
	   sema_init (&sema[0], 0);
	   sema_init (&sema[1], 0);
	   thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	   for (i = 0; i < 10; i++)
	   {
		   sema_up (&sema[0]);
		   sema_down (&sema[1]);
	   }
	   printf ("done.\n");
   }
   
   /* sema_self_test()에서 사용하는 스레드 함수. */
   static void
   sema_test_helper (void *sema_) {
	   struct semaphore *sema = sema_;
	   int i;
   
	   for (i = 0; i < 10; i++)
	   {
		   sema_down (&sema[0]);
		   sema_up (&sema[1]);
	   }
   }
   
   /* Initializes LOCK.  A lock can be held by at most a single
	  thread at any given time.  Our locks are not "recursive", that
	  is, it is an error for the thread currently holding a lock to
	  try to acquire that lock.
   
	  A lock is a specialization of a semaphore with an initial
	  value of 1.  The difference between a lock and such a
	  semaphore is twofold.  First, a semaphore can have a value
	  greater than 1, but a lock can only be owned by a single
	  thread at a time.  Second, a semaphore does not have an owner,
	  meaning that one thread can "down" the semaphore and then
	  another one "up" it, but with a lock the same thread must both
	  acquire and release it.  When these restrictions prove
	  onerous, it's a good sign that a semaphore should be used,
	  instead of a lock. */
   void
   lock_init (struct lock *lock) {
	   ASSERT (lock != NULL);
   
	   lock->holder = NULL;
	   sema_init (&lock->semaphore, 1);
   }
   
   /* Acquires LOCK, sleeping until it becomes available if
	  necessary.  The lock must not already be held by the current
	  thread.
   
	  This function may sleep, so it must not be called within an
	  interrupt handler.  This function may be called with
	  interrupts disabled, but interrupts will be turned back on if
	  we need to sleep. */
   void
   lock_acquire (struct lock *lock) {
	   ASSERT (lock != NULL);
	   ASSERT (!intr_context ());
	   thread_current()->wait_on_lock = lock;	
	   
	   /* holder가 없거나, donur 자신인 경우, holder의 우선순위가 더 높은 경우를 제외하고, donur의 우선순위 donation */
	   donate_priority(thread_current(), lock->holder); 
	   
	   /* sema의 value가 0이면 실행중인 thread를 block, 그렇지 않으면 value를 0으로 만들고 종료 */
	   sema_down (&lock->semaphore);
   
	   /* lock 획득에 따른 holder, wait_on_lock 갱신 */	
	   lock->holder = thread_current ();
	   thread_current()->wait_on_lock = NULL;
	   preempt_priority();
   }
   
   /* Tries to acquires LOCK and returns true if successful or false
	  on failure.  The lock must not already be held by the current
	  thread.
   
	  This function will not sleep, so it may be called within an
	  interrupt handler. */
   bool
   lock_try_acquire (struct lock *lock) {
	   bool success;
   
	   ASSERT (lock != NULL);
	   ASSERT (!lock_held_by_current_thread (lock));
   
	   success = sema_try_down (&lock->semaphore);
	   if (success)
		   lock->holder = thread_current ();
	   return success;
   }
   
   /* Releases LOCK, which must be owned by the current thread.
	  This is lock_release function.
   
	  An interrupt handler cannot acquire a lock, so it does not
	  make sense to try to release a lock within an interrupt
	  handler. */
   void
   lock_release (struct lock *lock) {
	   struct thread *curr = thread_current ();
	   ASSERT (lock != NULL);
	   ASSERT (lock_held_by_current_thread (lock));
   
	   /* donations 리스트를 순회하며 작업이 끝난 lock의 대기 thread를 리스트에서 제거 */
	   for (struct list_elem *e = list_begin (&curr->donations); e != list_end (&curr->donations); e = list_next(e)) 
	   {
		   struct thread *donor = list_entry (e, struct thread, d_elem);        
   
		   if (donor->wait_on_lock == lock) 
			   list_remove (e);             
	   }
   
	   /* 우선순위 복원 */
	   recal_priority (curr);
   
	   /* 해당 lock 업데이트 */
	   lock->holder = NULL;
	   sema_up (&lock->semaphore);
   
	   preempt_priority();
   }
   
   /* Returns true if the current thread holds LOCK, false
	  otherwise.  (Note that testing whether some other thread holds
	  a lock would be racy.) */
   bool
   lock_held_by_current_thread (const struct lock *lock) {
	   ASSERT (lock != NULL);
   
	   return lock->holder == thread_current ();
   }
   
   /* One semaphore in a list. */
   struct semaphore_elem {
	   struct list_elem elem;              /* List element. */
	   struct semaphore semaphore;         /* This semaphore. */
   };
   
   /* Initializes condition variable COND.  A condition variable
	  allows one piece of code to signal a condition and cooperating
	  code to receive the signal and act upon it. */
   void
   cond_init (struct condition *cond) {
	   ASSERT (cond != NULL);
   
	   list_init (&cond->waiters);
   }
   
   /* Atomically releases LOCK and waits for COND to be signaled by
	  some other piece of code.  After COND is signaled, LOCK is
	  reacquired before returning.  LOCK must be held before calling
	  this function.
   
	  The monitor implemented by this function is "Mesa" style, not
	  "Hoare" style, that is, sending and receiving a signal are not
	  an atomic operation.  Thus, typically the caller must recheck
	  the condition after the wait completes and, if necessary, wait
	  again.
   
	  A given condition variable is associated with only a single
	  lock, but one lock may be associated with any number of
	  condition variables.  That is, there is a one-to-many mapping
	  from locks to condition variables.
   
	  This function may sleep, so it must not be called within an
	  interrupt handler.  This function may be called with
	  interrupts disabled, but interrupts will be turned back on if
	  we need to sleep. */
   void
   cond_wait (struct condition *cond, struct lock *lock) {
	   	struct semaphore_elem waiter;
   
	   	ASSERT (cond != NULL);
	   	ASSERT (lock != NULL);
	   	ASSERT (!intr_context ());
	   	ASSERT (lock_held_by_current_thread (lock));
   
	   	sema_init (&waiter.semaphore, 0);
	   
	 	/* 각 semaphore의 대기열에 있는 스레드간 우선순위를 비교하여 리스트에 삽입 */  
	   	list_insert_ordered(&cond->waiters, &waiter.elem, cmp_sema_priority, NULL);

	   	lock_release (lock);
	   	sema_down (&waiter.semaphore);
	   	lock_acquire (lock);
   }
   
   /* If any threads are waiting on COND (protected by LOCK), then
	  this function signals one of them to wake up from its wait.
	  LOCK must be held before calling this function.
   
	  An interrupt handler cannot acquire a lock, so it does not
	  make sense to try to signal a condition variable within an
	  interrupt handler. */
   void
   cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	   ASSERT (cond != NULL);
	   ASSERT (lock != NULL);
	   ASSERT (!intr_context ());
	   ASSERT (lock_held_by_current_thread (lock));
   
	   if (!list_empty (&cond->waiters))
	   {
			/* cond의 대기열에서 lock holder가 donation을 받아 우선순위가 바뀌는 경우를 상정하여 재정렬 */
			list_sort(&cond->waiters, cmp_sema_priority, NULL);
			sema_up (&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);
	   }		   	  	
   }
   
   /* Wakes up all threads, if any, waiting on COND (protected by
	  LOCK).  LOCK must be held before calling this function.
   
	  An interrupt handler cannot acquire a lock, so it does not
	  make sense to try to signal a condition variable within an
	  interrupt handler. */
   void
   cond_broadcast (struct condition *cond, struct lock *lock) {
	   ASSERT (cond != NULL);
	   ASSERT (lock != NULL);
   
	   while (!list_empty (&cond->waiters))
		   cond_signal (cond, lock);
   }

   bool 
   cmp_sema_priority(struct list_elem *a, struct list_elem *b, void *aux UNUSED)
   {
		/* 각 semaphore에 접근 */
		struct semaphore_elem *sema_a = list_entry(a, struct semaphore_elem, elem);
    	struct semaphore_elem *sema_b = list_entry(b, struct semaphore_elem, elem);   

		/* 각 semaphore의 대기열 리스트에 접근 */		
		struct list *waiters_a = &(sema_a->semaphore.waiters);
		struct list *waiters_b = &(sema_b->semaphore.waiters);
   
	   	/* 각 대기열에 있는 스레드에 접근 */
		struct thread *root_a = list_entry(list_begin(waiters_a), struct thread, elem);
	   	struct thread *root_b = list_entry(list_begin(waiters_b), struct thread, elem);
   
	   	/* 두 스레드의 우선순위 비교 */
		return root_a->priority > root_b->priority;
   }

   