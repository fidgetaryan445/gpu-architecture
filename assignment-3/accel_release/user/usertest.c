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

  /* ================= TEST BLOCK START (currently VADD) =================
     To test VMUL / VXOR: copy everything from this '{' down to the matching
     '}' (marked TEST BLOCK END) and paste it right after it, then change the
     three lines marked  "CHANGE PER BLOCK"  in the pasted copy.
     Variables (failed, fd, v_s1 ... d_dv, flag, region) are declared ABOVE,
     so the blocks share them.  Don't copy open(), accel_query() or close().
     ================================================================== */
  {
  //Alloc and init vectors
  v_s1 = malloc(sizeof(int)*count);
  assert(v_s1);
  fill_num(v_s1, 5, count);
  
  v_s2 = malloc(sizeof(int)*count);
  assert(v_s2);
  fill_num(v_s2, 10, count);   /* CHANGE PER BLOCK (1/3): input value. Use 3 so add=8, mul=15, xor=6 are all different */

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
  assert(accel_add(fd, s1_dv, s2_dv, d_dv, 4096, region) == 0);   /* CHANGE PER BLOCK (2/3): accel_add / accel_mul / accel_xor */

  flag = region | (D2H << 8); 
  assert(accel_copy(fd, d_dv, v_d, 4096*sizeof(int), flag) == 0);

  for(int i=0; i<4096; ++i){
	 if(v_d[i] != 15)   /* CHANGE PER BLOCK (3/3): expected result (inputs 5 and 3: add 8, mul 15, xor 6) */
		failed = 1;
  } 

  }
  /* ================= TEST BLOCK END ================= */

  if(!failed)
	  printf("Acceldev test successful\n");

  close(fd);
  return 0;
}
