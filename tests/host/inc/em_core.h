#ifndef FAKE_CORE_H
#define FAKE_CORE_H
extern int core_depth, core_max_depth;
#define CORE_DECLARE_IRQ_STATE int _irq_st = 0
#define CORE_ENTER_ATOMIC() do { (void)_irq_st; core_depth++; if (core_depth > core_max_depth) core_max_depth = core_depth; } while (0)
#define CORE_EXIT_ATOMIC()  do { core_depth--; } while (0)
#endif
