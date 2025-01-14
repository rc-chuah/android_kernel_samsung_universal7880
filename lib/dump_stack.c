/*
 * Provide a default dump_stack() function for architectures
 * which don't implement their own.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/atomic.h>

#ifdef CONFIG_KFAULT_AUTO_SUMMARY
static void __dump_stack(bool for_auto_summary)
#else
static void __dump_stack(void)
#endif
{
	dump_stack_print_info(KERN_DEFAULT);

#ifdef CONFIG_KFAULT_AUTO_SUMMARY
	if (for_auto_summary)
		show_stack_auto_summary(NULL, NULL);
	else
		show_stack(NULL, NULL);
#else
	show_stack(NULL, NULL);
#endif
}

/**
 * dump_stack - dump the current task information and its stack trace
 *
 * Architectures can override this implementation by implementing its own.
 */
#ifdef CONFIG_SMP
static atomic_t dump_lock = ATOMIC_INIT(-1);

asmlinkage __visible void _dump_stack(bool auto_summary)
{
	unsigned long flags;
	int was_locked;
	int old;
	int cpu;

	/*
	 * Permit this cpu to perform nested stack dumps while serialising
	 * against other CPUs
	 */
retry:
	local_irq_save(flags);
	cpu = smp_processor_id();
	old = atomic_cmpxchg(&dump_lock, -1, cpu);
	if (old == -1) {
		was_locked = 0;
	} else if (old == cpu) {
		was_locked = 1;
	} else {
		local_irq_restore(flags);
		/*
		 * Wait for the lock to release before jumping to
		 * atomic_cmpxchg() in order to mitigate the thundering herd
		 * problem.
		 */
		do { cpu_relax(); } while (atomic_read(&dump_lock) != -1);
		goto retry;
	}

#ifdef CONFIG_KFAULT_AUTO_SUMMARY
	__dump_stack(auto_summary);
#else
	__dump_stack();
#endif

	if (!was_locked)
		atomic_set(&dump_lock, -1);

	local_irq_restore(flags);
}

asmlinkage __visible void dump_stack(void)
{
	_dump_stack(false);
}

#else
asmlinkage __visible void dump_stack(void)
{
	__dump_stack();
}
#endif
EXPORT_SYMBOL(dump_stack);
