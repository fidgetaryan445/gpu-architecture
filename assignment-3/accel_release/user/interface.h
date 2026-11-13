#ifndef _INTERFACE_H_
#define _INTERFACE_H_

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/fcntl.h>
#include<signal.h>
#include<assert.h>
#include<sys/ioctl.h>
#include<sys/mman.h>
#include <sys/unistd.h>


#define REGION_DMA 0
#define REGION_MAP 1

#define H2D 0
#define D2H 1

#define ERR_ACCEL_MEM (-1UL)

typedef unsigned long acc_handle_t;

typedef enum{
         OP_DEVQ=1,
         OP_DEVFLAGS,
         OP_ALLOC,
         OP_FREE,
         OP_DEVCOPY,
         OP_VADD,
         OP_VMUL,
         OP_VXOR,
         OP_RESET,
         OP_MAX
     }commands;

struct msg{
             short cmd;
             unsigned short flags;
             int timeout;
             unsigned long daddr;
             unsigned long uaddr;
             long size;

             unsigned long s1_addr;
             unsigned long s2_addr;
             unsigned long d_address;
             long count;
             char status[64];
};

extern int accel_add(int fd, acc_handle_t s1, acc_handle_t s2, acc_handle_t d, int elements, int where);
extern acc_handle_t accel_malloc(int fd, int size, int where);
extern int accel_free(int fd, acc_handle_t addr, int size, int where);
extern  int accel_copy(int fd, acc_handle_t d, void *h, int bytes, int cflags);
extern void accel_query(int fd);

#endif
