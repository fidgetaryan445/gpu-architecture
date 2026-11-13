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

static void free_range(struct compute_context *cc)
{
  struct mem_range *node, *tmp;
  list_for_each_entry_safe(node, tmp, &cc->mem_range_list, list){
    list_del(&node->list);
    kfree(node);
  }
  cc->used = 0;
}

//Should be called by holding aif->lock (also called from accel_release() with the lock held)
//m may be NULL
static int do_reset(struct accel_interface *aif, struct msg *m)
{
  struct acceldev *adev = aif->pci_dev;

  //Every allocated range of both regions goes back to the free pool
  free_range(aif->devmap);
  free_range(aif->devram);

  aif->cur_devmem_mode = AC_MODE_DMA;
  if(adev){
      set_dev_mode(adev, ACCEL_MODE_DMA);
      _cwrite(ACCEL_SUCCESS, adev->bar0, BAR0_ERR_STATUS);
      _cwrite(0, adev->bar1, BAR1_SRC_ONE);
      _cwrite(0, adev->bar1, BAR1_SRC_TWO);
      _cwrite(0, adev->bar1, BAR1_DST);
      _cwrite(0, adev->bar1, BAR1_COUNT);
  }
  aif->status = ACI_STATUS_READY;
  return 0;
}

// Best-fit allocation: look at every free gap (before the first range, between two
// ranges, after the last range) and use the smallest one that is large enough.
// The new block is carved from the end of the chosen gap next to the range that follows it,
// or from the start of the gap after the last range.
// Returns the start address of the block or -1.
static long allocate_mem_range(struct compute_context *cc, long size)
{
    struct mem_range *curr = NULL, *prev = NULL, *n = NULL;
    struct mem_range *best = NULL;   //range that follows the best gap
    unsigned long best_gap = ~0UL;
    unsigned long gap_start, gap;

    if(size <= 0)
        return -1;

    list_for_each_entry(curr, &cc->mem_range_list, list){
        gap_start = prev ? prev->end : cc->start;
        gap = curr->start - gap_start;
        if(gap >= (unsigned long)size && gap < best_gap){
            best_gap = gap;
            best = curr;
        }
        prev = curr;
    }

    if(best){                       //gap in front of "best": grow it downwards
        best->start -= size;
        return best->start;
    }

    //No interior gap fits: use the space after the last range (or the whole region)
    gap_start = prev ? prev->end : cc->start;
    if(gap_start + size <= cc->end){
        if(prev){
            prev->end += size;
            return gap_start;
        }
        n = kzalloc(sizeof(struct mem_range), GFP_ATOMIC);
        if(!n)
            return -1;
        n->start = gap_start;
        n->end = gap_start + size;
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
   unsigned long start = m->d_handle;
   unsigned long end = start + m->size;
   u8 mode = m->flags & 0xFF;
   struct compute_context *cc;
   struct mem_range *node, *new;
   int retval = 0;

   if(mode > AC_MODE_MMIO || m->size <= 0){
       sprintf(m->status, "Acceldev: Invalid free request");
       return -EINVAL;
   }
   cc = (mode == AC_MODE_DMA) ? aif->devram : aif->devmap;

   //Allocate before taking the lock: a split needs one extra node
   new = kzalloc(sizeof(struct mem_range), GFP_KERNEL);
   if(!new){
       sprintf(m->status, "Acceldev: Out of kernel memory");
       return -ENOMEM;
   }

   spin_lock(&aif->lock);
   if(aif->status != ACI_STATUS_READY){
       retval = -EBUSY;
       sprintf(m->status, "Acceldev: Device unavailable or busy");
       goto unlock_ret;
   }

   //The freed block has to lie completely inside one allocated range
   node = find_range(cc, start, end);
   if(!node){
       retval = -EINVAL;
       sprintf(m->status, "Acceldev: Invalid device address to free");
       goto unlock_ret;
   }

   if(node->start == start && node->end == end){          //whole range
       list_del(&node->list);
       kfree(node);
   }else if(node->start < start && node->end == end){    //tail of the range
       node->end = start;
   }else if(node->start == start && node->end > end){    //head of the range
       node->start = end;
   }else{                                                 //hole in the middle: split
       new->start = end;
       new->end = node->end;
       node->end = start;
       list_add(&new->list, &node->list);                 //keeps the list sorted
       new = NULL;
   }
   cc->used -= m->size;

unlock_ret:
   spin_unlock(&aif->lock);
   kfree(new);                 //NULL (or unused) after a split
   return retval;
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
   spin_lock(&aif->lock);
   if(aif->status != ACI_STATUS_READY){
	spin_unlock(&aif->lock);
	sprintf(m->status, "Acceldev: Device unavailable or busy");
       	return -EBUSY;
   }

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
	 retval = -EINVAL;
	 goto ret;
   }
   //Now it is a valid range
   if(mode == AC_MODE_MMIO){ //mmio map: bounce through a kernel page, device address = BAR1 offset
          long off = 0;
          void *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
          if(!buf){
               sprintf(m->status, "Acceldev: Out of kernel memory");
               retval = -ENOMEM;
               goto ret;
          }
          while(off < m->size){
               long n = min_t(long, PAGE_SIZE, m->size - off);
               void __iomem *dev = (void __iomem *)cc->kva + m->d_handle + off;
               void __user *usr = (void __user *)(m->h_handle + off);
               if(dir == ACCEL_COPY_TO_DEV){
                    if(copy_from_user(buf, usr, n)) break;
                    memcpy_toio(dev, buf, n);
               }else{
                    memcpy_fromio(buf, dev, n);
                    if(copy_to_user(usr, buf, n)) break;
               }
               off += n;
          }
          kfree(buf);
          if(off < m->size){
               sprintf(m->status, "Acceldev: Error MMIO copy");
               retval = -EFAULT;
          }
          goto ret;
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
   struct compute_context *cc;
   u8 mode = m->flags & 0xFF;
   u32 devcmd;
   unsigned long len;
   int err;
   int retval = 0;

   switch(m->cmd){
       case OP_VADD: devcmd = ACCEL_CMD_VADD; break;
       case OP_VMUL: devcmd = ACCEL_CMD_VMUL; break;
       case OP_VXOR: devcmd = ACCEL_CMD_VXOR; break;
       default:
           sprintf(m->status, "Acceldev: Invalid compute command");
           return -EINVAL;
   }

   if(m->count <= 0 || mode > AC_MODE_MMIO){
       sprintf(m->status, "Acceldev: Invalid compute arguments");
       return -EINVAL;
   }
   len = sizeof(int) * m->count;
   cc = (mode == AC_MODE_MMIO) ? aif->devmap : aif->devram;

   spin_lock(&aif->lock);
   if(aif->status != ACI_STATUS_READY){
       spin_unlock(&aif->lock);
       sprintf(m->status, "Acceldev: Device unavailable or busy");
       return -EBUSY;
   }
   //All three vectors must be allocated device memory of the selected region
   if(!find_range(cc, m->s1_addr, m->s1_addr + len) ||
      !find_range(cc, m->s2_addr, m->s2_addr + len) ||
      !find_range(cc, m->d_addr,  m->d_addr  + len)){
       spin_unlock(&aif->lock);
       sprintf(m->status, "Acceldev: Compute operands are not valid device memory");
       return -EINVAL;
   }
   aif->status = ACI_STATUS_BUSY_COMP;
   spin_unlock(&aif->lock);            //never sleep with the spinlock held

   //Program the device and start the operation
   set_dev_mode(adev, mode);
   _cwrite(ACCEL_SUCCESS, adev->bar0, BAR0_ERR_STATUS);
   reinit_completion(&adev->compute_comp);
   _cwrite(m->s1_addr, adev->bar1, BAR1_SRC_ONE);
   _cwrite(m->s2_addr, adev->bar1, BAR1_SRC_TWO);
   _cwrite(m->d_addr,  adev->bar1, BAR1_DST);
   _cwrite(m->count,   adev->bar1, BAR1_COUNT);
   _cwrite(devcmd,     adev->bar0, BAR0_CMD);   //Trigger the computation

   wait_for_completion(&adev->compute_comp);    //raised by the completion interrupt
   err = _cread(adev->bar0, BAR0_ERR_STATUS);
   if(err != ACCEL_SUCCESS || (adev->irq_status & COMP_ERROR_IRQ)){
       printk(KERN_INFO "Compute failed, device error code = %d\n", err);
       sprintf(m->status, "Acceldev: Compute error %d", err);
       retval = -EIO;
   }

   spin_lock(&aif->lock);
   aif->status = ACI_STATUS_READY;
   spin_unlock(&aif->lock);
   return retval;
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
