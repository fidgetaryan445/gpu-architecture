#include "interface.h"

int accel_add(int fd, acc_handle_t s1, acc_handle_t s2, acc_handle_t d, int elements, int where)
{
  struct msg m;
  m.cmd = OP_VADD;
  m.s1_addr = s1;
  m.s2_addr = s2;
  m.d_address = d;
  m.count = elements;
  m.flags = where;
  if(read(fd, &m, sizeof(m)) < 0){  
       printf("Compute Error: %s\n", m.status);
       return -1;
   }
  return 0;
}	


acc_handle_t accel_malloc(int fd, int size, int where)
{
  struct msg m;
  m.cmd = OP_ALLOC;
  m.size = size;
  m.flags = where;
  if(read(fd, &m, sizeof(m)) < 0){  
       printf("Allocation Error: %s\n", m.status);
       return ERR_ACCEL_MEM;
  }
  return m.daddr;
}

int accel_free(int fd, acc_handle_t addr, int size, int where)
{
  struct msg m;
  m.cmd = OP_FREE;
  m.size = size;
  m.flags = where;
  m.daddr = addr;
  if(read(fd, &m, sizeof(m)) < 0){  
       printf("Free Error: %s\n", m.status);
       return -1;
  }
  return 0;

}

int accel_copy(int fd, acc_handle_t d, void *h, int bytes, int cflags)
{
  struct msg m;
  m.cmd = OP_DEVCOPY;
  m.uaddr = (unsigned long)h;
  m.daddr = d;
  m.size = bytes;
  m.flags = cflags;
  if(read(fd, &m, sizeof(m)) < 0){  
       printf("Copy Error: %s\n", m.status);
       return -1;
   }
  return 0;
}	

void accel_query(int fd)
{
   struct msg m;
   m.cmd = OP_DEVQ;
  
   if(read(fd, &m, sizeof(m)) < 0){  
       printf("DevQuerry Error: %s\n", (char *)&m);
       exit(-1);
   }

  printf("Query Result: %s\n", (char *)&m);

}

