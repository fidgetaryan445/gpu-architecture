#include "interface.h"
static void fill_num(int *ptr, int val, int count)
{
   for(int i=0; i<count; ++i)
      *(ptr+i) = val;
   return;   
}
int main()
{
   int failed = 0;
   int *v_s1, *v_s2, *v_d;            // Host pointers	
   acc_handle_t s1_dv, s2_dv, d_dv;   // Device memory handles	
   int count = 4096;
   int flag;

   int region = REGION_DMA;
   int fd = open("/dev/cs623accel",O_RDWR);
   if(fd < 0){
       perror("open");
       exit(-1);
   }

  accel_query(fd);

  //Alloc and init vectors
  v_s1 = malloc(sizeof(int)*count);
  assert(v_s1);
  fill_num(v_s1, 5, count);
  
  v_s2 = malloc(sizeof(int)*count);
  assert(v_s2);
  fill_num(v_s2, 10, count);

  v_d = malloc(sizeof(int)*count);
  assert(v_d);

  //Allocate device memory
  s1_dv = accel_malloc(fd, 4096*sizeof(int), region);
  assert(s1_dv != ERR_ACCEL_MEM);
  
  s2_dv = accel_malloc(fd, 4096*sizeof(int), region);
  assert(s2_dv != ERR_ACCEL_MEM);

  d_dv = accel_malloc(fd, 4096*sizeof(int), region);
  assert(d_dv != ERR_ACCEL_MEM);

  //Perform copy
  flag = region | (H2D << 8); 
  assert(accel_copy(fd, s1_dv, v_s1, 4096*sizeof(int), flag) == 0);
  assert(accel_copy(fd, s2_dv, v_s2, 4096*sizeof(int), flag) == 0);

  //Compute
  assert(accel_add(fd, s1_dv, s2_dv, d_dv, 4096, region) == 0);

  flag = region | (D2H << 8); 
  assert(accel_copy(fd, d_dv, v_d, 4096*sizeof(int), flag) == 0);

  for(int i=0; i<4096; ++i){
	 if(v_d[i] != 15)
		failed = 1;
  } 

  if(!failed)
	  printf("Acceldev test successful\n");

  close(fd);
  return 0;
}
