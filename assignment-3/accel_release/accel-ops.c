#include<linux/wait.h>
#include<linux/dma-mapping.h>
#include<linux/pci.h>
#include<acceldev.h>
#include<interface.h>


static int do_query(struct accel_interface *aif, struct msg *m)
{
   char *modnames[3] = {"DMA-COPY", "DiM-COPY", "DiM-MAP"};
   bool info = true;	
   int retval = 0;	
   char *ptr = (char *)m;
   
   switch(aif->status){

    case ACI_STATUS_DOWN:	   
    case ACI_STATUS_BEING_RESET:	   
            retval = sprintf(ptr, "AccelDev is Down or going down!\n");
	    info = false;
            break;
    case ACI_STATUS_INERROR:
            retval = sprintf(ptr, "AccelDev: Error Occured, Check debug logs\n");
	    info = false;
            break;
    case ACI_STATUS_READY:
            retval += sprintf(ptr, "AccelDev: [Ready]\n");
            break;
    case ACI_STATUS_BUSY_DCOPY:
            retval += sprintf(ptr, "AccelDev: [Busy] -- Data Transfer --\n");
            break;
    case ACI_STATUS_BUSY_COMP:
            retval += sprintf(ptr, "AccelDev: [Busy] -- Computing --\n");
            break;       
   }
   if(info){
      retval += sprintf(ptr + retval, "Mode = %s RAM Size = %ld UsedRAM = %ld DiM Size = %ld DimUsed = %ld", 
		         modnames[aif->cur_devmem_mode], aif->devram->end - aif->devram->start, aif->devram->used,
			 aif->devmap->end - aif->devmap->start, aif->devmap->used);
   }
   return retval;   
}

static void set_dev_mode(struct acceldev *adev, int mode)
{
   if(mode > ACCEL_MODE_MMIO)
       mode = ACCEL_MODE_MMIO;

   _cwrite(mode, adev->bar0, BAR0_MEM_MODE); 
   return;     	
}

static int do_set_modes(struct accel_interface *aif, struct msg *m)
{
   int retval = 0;

   if(m->flags > AC_MODE_MMIO_UMAP)
	   return -EINVAL;

   spin_lock(&aif->lock);	
   
   if(unlikely(aif->status != ACI_STATUS_READY)){
        retval = -EINVAL;
	goto unlock_ret;
   }
   
   if(unlikely(m->flags == AC_MODE_DMA && !aif->dma_cap)){
        retval = -ENOTSUPP;
	goto unlock_ret;
   }  
   aif->cur_devmem_mode = m->flags;
   set_dev_mode(aif->pci_dev, m->flags);

unlock_ret:   
   spin_unlock(&aif->lock);
   return retval;
}

static int free_range(struct compute_context *cc){
  struct mem_range *node , *tmp ;
  list_for_each_entry_safe(node,tmp,&cc->mem_range_list ,list){
    list_del(&node->list);
    kfree(node);
  }
  cc->used = 0 ;
}

//Should be called by holding aif->lock
static int do_reset(struct accel_interface *aif, struct msg *m)
{
  free_range(aif->devmap);
  free_range(aif->devram);
  aif->cur_devmem_mode = AC_MODE_DMA ;
  if(aif->pcidev){
      set_dev_mode(aif->pci_dev,ACCEL_MODE_DMA);

  }
  aif->status= ACI_STATUS_READY ;

   return -ENOTSUPP;   
           
}

// A simple best-fit allocation 
static long allocate_mem_range(struct compute_context *cc, long size)
{
  
    struct mem_range *curr = NULL, *prev = NULL, *n = NULL;
    long = cc->start;
    long pend = 0;

    list_for_each_entry(curr, &cc->mem_range_list, list){
       if(cstart == cc->start && curr->start - cstart >= size){  //Gap in the begining
	       curr->start -= size;
	       return curr->start;  
       }
       if(pend && curr->start - pend >= size){
	       curr->start -= size;
	       return curr->start;  
       }
       pend = curr->end;    
       prev = curr;
    }

    if(pend && pend + size <= cc->end){
	    BUG_ON(!prev);
            BUG_ON(pend != prev->end);
	    prev->end += size; 
	    return pend;
    } 
    if(!pend && cstart == cc->start){ //First allocation
            n = kzalloc(sizeof(struct mem_range), GFP_ATOMIC);
	    BUG_ON(!n);
            n->start = cstart;
	    n->end = cstart + size;
            list_add_tail(&n->list, &cc->mem_range_list);
	    return n->start;
    }
    return -1;

}

//Returns encapsulating range or NULL
static struct mem_range* find_range(struct compute_context *cc, long start, long end)
{
    struct mem_range *curr = NULL;
    list_for_each_entry(curr, &cc->mem_range_list, list){
       if(curr->start <= start && end <= curr->end){
	       return curr;
       }
   }
   return NULL;   	
}

static int do_free(struct accel_interface *aif, struct msg *m)
{
   int start = m->d_addr ;
   int end = start + m->size ;
   u8 mode = m->flags & 0xFF ; 
   int retval = 0 ;
   acceldev adev = aif->device ;
   strcut mem_range *node, *new ; 

   if(mode > ACCEL_MODE_MMIO){
     return -EINT ;
   }
   struct compute_context cc ;
   cc = (mode==0)?AC_MODE_DMA : AC_MODE_MMIO ;
   
   spin_lock(&aif->lock) ;
   if(aif->status!=ACI_STATUS_READY){
     return -EBUSY  ;
   }
   node = find_range(cc, start, end );
   if(node.start > start || node.end < end){
     return -ENOMEM;
   }

   if(node.start ==start && node.end == end){
     list_del(&node);
     kfree(node);
   }else if(node.start > start && node.end == end){
     node.end = start ;
   }else if(node.start == start && node.end > end){
     node.start  = end ;
   }else{
     if(!kzalloc(sizeof(mem_range)), GFP_ATOMIC);
          return -ENOMEM ; 
     new.start = end ; 
     new.end = node.end ;
     node.end = start ; 
     list_add(&new, &node);

   }
    spin_unlock(&aif->lock) ;

   return -ENOTSUPP;   
}

static int do_alloc(struct accel_interface *aif, struct msg *m)
{
   long total;	
   int retval = 0;	
   struct compute_context *cc;
   long mr;
  
   if(aif->status < ACI_STATUS_READY){
	sprintf(m->status, "Acceldev: Device unavailable or in error");
       	return -ENOMEM;
   }

   if(m->flags > AC_MODE_MMIO){
	 sprintf(m->status, "Acceldev: Invalid mode for memory allocation");
	 return -EINVAL;
   }

   if(((m->size >> PAGE_SHIFT) << PAGE_SHIFT) != m->size){
	 sprintf(m->status, "Acceldev: Allocation size must be multiple of page size");
	 return -EINVAL;
   }
   
   if(m->flags == AC_MODE_DMA)
	  cc = aif->devram;
   else
	  cc = aif->devmap;

   spin_lock(&aif->lock);
   total = cc->end - cc->start;
   if(cc->used + m->size > total){
	  retval = -ENOMEM; 
	  goto unlock_ret;
   }
   mr = allocate_mem_range(cc, m->size);
   if(mr < 0){
	  retval = -EBUSY; 
	  goto unlock_ret;
   }
   m->d_handle = mr;
   cc->used += m->size;
   printk(KERN_INFO "Allocated memory at %lx used: %ld\n", mr, cc->used);
   
unlock_ret:   

   spin_unlock(&aif->lock);
   if(retval)
	 sprintf(m->status, "Acceldev: Insufficient memory on the device, can not allocate");
   return retval;
}

static int do_accel_dma(struct accel_interface *aif, int dir, long size, long daddr, void *uaddr)
{
   int err;  
   long pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;	
   struct accel_dma_entry *de = &aif->dma_handles[0]; 
   struct acceldev *adev = aif->pci_dev;
   long dma_cmd = 0x5 | (dir << 1);  //0x4 is set for raising interrupt
   //First clear the error status
    _cwrite(ACCEL_SUCCESS, adev->bar0, BAR0_ERR_STATUS);
    
   //Copy data from user-space to device (i) to DMA mapped KVAs (ii) initiate DMA
   if(dir == ACCEL_COPY_TO_DEV){
	printk(KERN_INFO "Going to perform DMA from %lx of size %ld cmd %lx\n", daddr, size, dma_cmd);
	 if(copy_from_user(de->kva, uaddr, size)){
	       return -EINVAL;
	 } 
         _cwriteq(de->dev_addr, adev->bar0, BAR0_DMA_SRC); 
         _cwriteq(daddr, adev->bar0, BAR0_DMA_DST); 
         _cwriteq(pages, adev->bar0, BAR0_DMA_PAGES); 
         _cwriteq(dma_cmd, adev->bar0, BAR0_DMA_CMD); //Trigger the DMA
	 wait_for_completion(&adev->dma_comp);
         err = _cread(adev->bar0, BAR0_ERR_STATUS);
	 if(err != ACCEL_SUCCESS){
	       return -EIO;
	 }
	printk(KERN_INFO "Data copied from %lx into device addr %lx of size %ld dmacmd %lx val = %d dmah = %llx\n", (unsigned long)uaddr, daddr, size, dma_cmd, *(int *)de->kva, de->dev_addr);
   }else{
	printk(KERN_INFO "Going to perform DMA from %lx of size %ld cmd %lx\n", daddr, size, dma_cmd);
         _cwriteq(de->dev_addr, adev->bar0, BAR0_DMA_DST); 
         _cwriteq(daddr, adev->bar0, BAR0_DMA_SRC); 
         _cwriteq(pages, adev->bar0, BAR0_DMA_PAGES); 
         _cwriteq(dma_cmd, adev->bar0, BAR0_DMA_CMD); //Trigger the DMA
	 wait_for_completion(&adev->dma_comp);
         err = _cread(adev->bar0, BAR0_ERR_STATUS);
	 if(err != ACCEL_SUCCESS){
	       printk(KERN_INFO "Error code from the device = %d\n", err);
	       return -EIO;
	 }
	 if(copy_to_user(uaddr, de->kva, size)){
	       return -EINVAL;
	 }
	printk(KERN_INFO "Data copied to %lx from device addr %lx of size %ld val = %d dmah = %llx\n", (unsigned long)uaddr, daddr, size, *(int *)de->kva, de->dev_addr);
   }
   return 0;
}
static int do_copy(struct accel_interface *aif, struct msg *m)
{
   struct compute_context *cc;
   struct mem_range *mr;
   long retval = 0;
   int dir = (m->flags >> 8) & 0x1;
   u8 mode = m->flags & 0xff;

   if(mode > AC_MODE_MMIO){
	 sprintf(m->status, "Acceldev: Invalid mode for memory copy");
	 return -EINVAL;
   }
   if(aif->status != ACI_STATUS_READY){
	sprintf(m->status, "Acceldev: Device unavailable or busy");
       	return -EBUSY;
   }	   
   spin_lock(&aif->lock);

   if(mode == AC_MODE_DMA){
	  cc = aif->devram;
   }else{
	  cc = aif->devmap;
   }
   aif->status = ACI_STATUS_BUSY_DCOPY;

   mr = find_range(cc, m->d_handle, m->d_handle + m->size);
   spin_unlock(&aif->lock);
   if(!mr){
	 sprintf(m->status, "Acceldev: Invalid device address");
	 return -EINVAL;
   }
   //Now it is a valid range
   if(mode == AC_MODE_MMIO){ //mmio map
          return -ENOTSUPP;   
   }else{
	   long opsize = m->size;
	   long daddr = m->d_handle;
	   void *uaddr = (void *)m->h_handle; 
	   do{
	        void *dh;
                long csize;		
	        if(opsize > MAX_DMA_SIZE) 	   
	            csize = MAX_DMA_SIZE;
                else
	            csize = opsize;		
	        dh = accel_dma_get(aif, 0, csize, true);
                if(!dh){
		  retval = -EIO;
	          sprintf(m->status, "Acceldev: Error DMA allocation");
		  goto ret;
		}
                if(do_accel_dma(aif, dir, csize, daddr, uaddr)){
		    retval = -EIO;
		    accel_dma_put(aif, 0);
	            sprintf(m->status, "Acceldev: Error DMA Operation");
		    goto ret;
		}	
		opsize -= csize;
		daddr += csize;
		uaddr += csize;
		accel_dma_put(aif, 0);
	   }while(opsize);
   }

ret:    
   spin_lock(&aif->lock);
   aif->status = ACI_STATUS_READY;
   spin_unlock(&aif->lock);
   return retval;
}

static int do_compute(struct accel_interface *aif, struct msg *m)
{
   struct acceldev *adev = aif->pci_dev;
   struct compute_context *cc ;
  u8 mode = m->flag & 0xFF ;
  if(m->count<0 || mode >ACCEL_MODE_MMIO){
    return -EINVAL;
  }
  u32 devcmd ; 
  int err ; 
  unsigned long long len ;
  len = sizeof(int)*count ;
  int retval = 0 ;
  cc = (mode==ACCEL_MODE_MMIO)?aif->devmap: aif->devram ;
  spin_lock(&aif->lock)
   if(aif->status!=ACI_STATUS_READY){
     spin_unlock(&aif->lock);
     return -EBUSY;
   }
  if(!find_range(cc,m->s1_addr,m->s1_addr+len)||!find_range(cc,m->s2_addr,m->s2_addr+len)||!find_range(cc,m->d_addr,m->d_addr+len)){
    return -ENOMEM ;
  }
  _cwrite(ACCEL_SUCCESS , adev->bar0 , BAR0_ERR_STATUS);
  reinit_completion(&adev->compute_comp);
  _cwrite(m->s1_addr,adev->bar1,BAR1_SRC_ONE) ;
  

  wait_for_completion(&adev->compute_comp) ;
  err  = _cread(adev->bar0 , BAR0_ERR_STATUS) ;
  spin_unlock(&aif->lock) ;

  return -retval;   
}

static struct acceldev_ops accelops = {
           .accel_dev_query = do_query,
           .accel_set_modes = do_set_modes, 
           .accel_alloc = do_alloc, 
	   .accel_free = do_free,
	   .accel_copy = do_copy,
	   .accel_compute = do_compute,
	   .accel_reset = do_reset,
};

void set_accelops(struct accel_interface *cif)
{
   cif->devops = &accelops;	
   return;	
}
