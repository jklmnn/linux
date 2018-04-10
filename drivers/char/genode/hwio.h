
#ifndef _HWIO_H_
#define _HWIO_H_

#define MMIO_SET_RANGE _IOW('g', 1, mmio_range_t*)
#define IRQ_SET _IOW('g', 2, int*)
#define DMA_SET _IOW('g', 3, size_t*)

#define T_UNCONFIGURED 0
#define T_MMIO 1
#define T_IRQ 2
#define T_DMA 3

typedef struct {
    unsigned long phys;
    size_t length;
} mmio_range_t;

#endif //_HWIO_H_
