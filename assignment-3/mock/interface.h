#ifndef __INTERFACE_H_
#define __INTERFACE_H_

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

#define COPY_DIR_H2D 0
#define COPY_DIR_D2H 1
#define dev_cmd(C) ((C) - OP_DEVCOPY) 
struct msg{
             s16 cmd;
	     u16 flags;
	     int timeout;
	     //Address ops
             unsigned long d_handle;
             unsigned long h_handle;
	     long size;

	     //Compute ops
	     unsigned long s1_addr;      
	     unsigned long s2_addr;      
	     unsigned long d_addr;      
             long count;      
             
	     char status[64];
};
struct accel_interface;
struct acceldev_ops{
        int (*accel_dev_query)(struct accel_interface *, struct msg *m);
        int (*accel_set_modes)(struct accel_interface *, struct msg *m);
        int (*accel_alloc)(struct accel_interface *, struct msg *m);
        int (*accel_free)(struct accel_interface *, struct msg *m);
        int (*accel_copy)(struct accel_interface *, struct msg *m);
        int (*accel_poll)(struct accel_interface *, struct msg *m);
        int (*accel_compute)(struct accel_interface *, struct msg *m);
        int (*accel_reset)(struct accel_interface *, struct msg *m);
};
#endif
