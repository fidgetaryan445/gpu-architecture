#ifndef _ACCELDEV_H_
#define _ACCELDEV_H_
#include<linux/cdev.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/pci.h>

////////////////////////////////////////// BAR0 START //////////////////////////////////////
//
#define BAR0_ID 0x0
#define BAR0_CMD 0x4
#define BAR0_ERR_STATUS 0x8
#define BAR0_MEM_MODE 0xC
#define BAR0_ACCEL_RAM_START 0x10
#define BAR0_ACCEL_RAM_SIZE 0x18

#define BAR0_IRQSTATUS 0x20
#define BAR0_STATUS 0x24

#define BAR0_IRQRAISE 0x60
#define BAR0_IRQACK 0x64
#define BAR0_DMA_SRC 0x80
#define BAR0_DMA_DST 0x88
#define BAR0_DMA_PAGES 0x90
#define BAR0_DMA_CMD 0x98
#define BAR0_END 0x100

#define ACCEL_CMD_VADD   0x1
#define ACCEL_CMD_VMUL   0x2
#define ACCEL_CMD_VXOR   0x3

#define ACCEL_SUCCESS          0
#define ACCEL_ERROR_ALLOC      1
#define ACCEL_ERROR_FREE       2
#define ACCEL_ERROR_DMA        3
#define ACCEL_ERROR_COMPUTE    4    
#define ACCEL_ERROR_BUSY       5
#define ACCEL_ERROR_MODE       6
#define ACCEL_ERROR_INVOP      7
#define ACCEL_ERROR_MEM        8    
#define ACCEL_ERROR_COMMAND    10

#define ACCEL_MODE_DMA  0    
#define ACCEL_MODE_MMIO 1    


#define ACCEL_STATUS_COMPUTING    0x01
#define ACCEL_STATUS_DMAOP        0x10    
#define ACCEL_STATUS_IRQ          0x80

#define ACCEL_DMA_RUN            0x1
#define ACCEL_COPY_TO_DEV           0
#define ACCEL_COPY_FROM_DEV         1
#define ACCEL_DMA_IRQ             0x4

////////////////////////////////////////// BAR0 END //////////////////////////////////////

////////////////////////////////////////// BAR1 START //////////////////////////////////////
//
#define BAR1_SRC_ONE 0x0
#define BAR1_SRC_TWO 0x4
#define BAR1_DST     0x8
#define BAR1_COUNT   0xC
#define ACCEL_MAPMEM 0x10
////////////////////////////////////////// BAR1 END //////////////////////////////////////


#define _cread(base, cmd) readl(base + cmd)
#define _cwrite(val, base, cmd) writel(val, base + cmd)

#define _creadq(base, cmd) readq(base + cmd)
#define _cwriteq(val, base, cmd) writeq(val, base + cmd)

#define DEVNAME "cs623accel"

#define DEV_MMIO_LEN (1 << 20)
#define DMA_MAX_LEN (1 << 12)


//IRQ status flags
#define COMP_DONE_IRQ  0x1
#define COMP_ERROR_IRQ 0x10
#define DMA_IRQ        0x100



struct acceldev{
        struct pci_dev *dev;
        void __iomem *bar0;
        unsigned long address_bar0;
        unsigned long size_bar0;

        void __iomem *bar1;   //IOMEM for ops
        unsigned long address_bar1;
        unsigned long size_bar1;

        unsigned long irq;
	unsigned irq_status;

        char name[16];
        struct completion dma_comp;
        struct completion compute_comp;
        void *private;
};

#define AC_MODE_DMA        0x0 
#define AC_MODE_MMIO       0x1  
#define AC_MODE_MMIO_UMAP  0x2  

struct mem_range{
	     unsigned long start;
	     unsigned long end;
	     struct list_head list;
};

struct compute_context{
	  unsigned long start;
	  unsigned long end;
	  long used; 
	  struct list_head mem_range_list;
          	
	  void *kva; //NULL for DMA
          unsigned long src1;
          unsigned long src2;
	  unsigned long dst;
	  unsigned count;
};

#ifdef ACCEL_SG_DMA
   #define MAX_DMA_SIZE 4096  //One page in SG mode
   #define MAX_DMA_HANDLES 1024
#else
   #define MAX_DMA_SIZE (1 << 20)  //1MB DMA maximum at a time
   #define MAX_DMA_HANDLES 1
#endif

struct accel_dma_entry{
	dma_addr_t dev_addr;
	void *kva;
        atomic_t usage;
	long size;
};

#define ACI_STATUS_DOWN 0 
#define ACI_STATUS_INERROR 1
#define ACI_STATUS_BEING_RESET 2
#define ACI_STATUS_READY 3
#define ACI_STATUS_BUSY_DCOPY 4
#define ACI_STATUS_BUSY_COMP 5

struct accel_interface{
       spinlock_t lock;
       struct acceldev *pci_dev;
       struct acceldev_ops *devops;
       /*Other members are for the chardev details*/
       int major;
       dev_t dev;
       struct cdev acdev;
       struct class *cif_class;
       bool dma_cap; //Is the device DMA capable
       bool cur_devmem_mode; //What is the current mode: DMA/DevMAP/DevMAPu
     
       /*DMA 
       XXX  Useful when the device allows SG DMA 
       */

       struct accel_dma_entry dma_handles[MAX_DMA_HANDLES];

       /*sysfs*/
       struct kobject *acceldev_kobj;

       /*WaitQueue and Other details*/
       wait_queue_head_t wq;

       struct compute_context *devmap;
       struct compute_context *devram;

       int status;  
       int usage;
};

static inline void *accel_dma_get(struct accel_interface *aif, int index, long size, bool dalloc)
{
   struct acceldev *dev = aif->pci_dev;
   struct pci_dev *pdev = dev->dev;
   struct accel_dma_entry *de = &aif->dma_handles[index];	
   
   if(index >= MAX_DMA_HANDLES || size <=0 || size >= MAX_DMA_SIZE){
           dev_warn(&pdev->dev, "Invalid arguments\n");
	   return NULL;
   }

   if(dalloc && atomic_read(&de->usage)){
           dev_warn(&pdev->dev, "Trying to map a used location... Possible DMA memory leak\n");
	   return NULL;
   }
   de->kva = dma_alloc_coherent(&pdev->dev, size, &de->dev_addr, GFP_KERNEL);

   if(de->kva){
	   atomic_inc(&de->usage);
	   de->size = size;
   }

   return de->kva;
}

static inline void accel_dma_put(struct accel_interface *aif, int index)
{
   struct acceldev *dev = aif->pci_dev;
   struct pci_dev *pdev = dev->dev;
   struct accel_dma_entry *de = &aif->dma_handles[index];	

   if(atomic_read(&de->usage) <= 0){
          dev_warn(&pdev->dev, "Possible double-free DMA BUG\n");
	  return;
   }

   if(atomic_dec_and_test(&de->usage)){
       dma_free_coherent(&pdev->dev, de->size, de->kva, de->dev_addr);
       de->kva = NULL;      
   }
   return;
}

extern int init_chdev(struct accel_interface *);
extern void accel_prepare(struct accel_interface *, struct acceldev *, bool);
extern void exit_chdev(struct accel_interface *);
extern void set_accelops(struct accel_interface *);

#endif
