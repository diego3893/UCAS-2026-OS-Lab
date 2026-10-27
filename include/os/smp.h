#ifndef SMP_H
#define SMP_H

#define NR_CPUS 2
#define FINE_GRAINED_LOCK 1 // 0：大内核锁；1：细粒度锁
extern void smp_init();
extern void wakeup_other_hart();
extern uint64_t get_current_cpu_id();
extern void lock_kernel();
extern void unlock_kernel();
extern void lock_screen(void);
extern void unlock_screen(void);

#endif /* SMP_H */
