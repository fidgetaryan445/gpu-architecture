#include <linux/kernel.h>
#include <linux/linkage.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/module.h>  
#include <linux/syscalls.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <acceldev.h>

static struct accel_interface *cif = NULL;

static irqreturn_t accel_irq_handler(int irq, void *arg)
{
        struct acceldev *accel = (struct acceldev *)arg;
        if(irq == accel->irq){
              u32 status = _cread(accel->bar0, BAR0_IRQSTATUS);
              _cwrite(status, accel->bar0, BAR0_IRQACK);
	      accel->irq_status = status;
	      if(status & DMA_IRQ)  
                 complete(&accel->dma_comp);
	      if(status & (COMP_DONE_IRQ | COMP_ERROR_IRQ))
                 complete(&accel->compute_comp);
        }
        return IRQ_HANDLED;
}

static int config_irq(struct pci_dev *dev, struct acceldev *accel)
{
        if (pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_LEGACY) < 0){
                dev_warn(&dev->dev, "No IRQ assigned to device: no support for interrupts?\n");
                goto failed;
        }
            
        if (pci_irq_vector(dev, 0)) {
                accel->irq = pci_irq_vector(dev, 0);
                if(request_irq(accel->irq, &accel_irq_handler, IRQF_SHARED, accel->name, accel)){
                      dev_warn(&dev->dev, "Register IRQ failed\n");
                      goto free_irq;
                }
        }else {
                dev_warn(&dev->dev, "No IRQ assigned to device: no support for interrupts?\n");
                goto free_irq;
        }
     dev_info(&dev->dev, "IRQ registration is successful\n");
     return 0;
free_irq:        
      pci_free_irq_vectors(dev);
failed:
      return -1;
   
}

static int acceldev_pci_probe(struct pci_dev *dev,
                                        const struct pci_device_id *id)
{       
        
        struct acceldev *accel;
        unsigned long val;

        accel = kzalloc(sizeof(struct acceldev), GFP_KERNEL);
        if (!accel)
                return -ENOMEM;

        accel->private = dev;

        if (pci_enable_device(dev))
                goto out_free;

        if (pci_request_regions(dev, "cs623-acceldev"))
                goto out_disable;
        
        strcpy(accel->name, "cs623-acceldev"); 
        accel->address_bar0 = pci_resource_start(dev, 0);
        if (!accel->address_bar0)
                goto out_release;
        
        accel->size_bar0 = (pci_resource_len(dev, 0) + PAGE_SIZE - 1) & PAGE_MASK;

        accel->bar0 = pci_ioremap_bar(dev, 0);
        if (!accel->bar0){
                dev_warn(&dev->dev, "PCI I/O mapping failed for BAR0\n");
                goto out_release;
        }
        
        val = _cread(accel->bar0, BAR0_ID);
        dev_info(&dev->dev, "Acceldev with ID=%lx is dicovered DevRAM start = %lx DevRAM size = %ld\n", 
	                   val, (unsigned long)_creadq(accel->bar0, BAR0_ACCEL_RAM_START), (unsigned long)_creadq(accel->bar0, BAR0_ACCEL_RAM_SIZE));
        
        accel->address_bar1 = pci_resource_start(dev, 1);
        if (!accel->address_bar1)
                goto out_unmap_zero;
        
	accel->size_bar1 = (pci_resource_len(dev, 0) + PAGE_SIZE - 1) & PAGE_MASK;
        
	accel->bar1 = pci_ioremap_bar(dev, 1);
        if (!accel->bar1){
                dev_warn(&dev->dev, "PCI I/O mapping failed for BAR1\n");
                goto out_unmap_zero;
	}
        
        dev_info(&dev->dev, "Found PCI BAR0 at %lx of size %ld. Mapped at %lx\n", accel->address_bar0, accel->size_bar0, (unsigned long)accel->bar0); 
	dev_info(&dev->dev, "Found PCI BAR1 at %lx of size %ld. Mapped at %lx\n", accel->address_bar1, accel->size_bar1, (unsigned long)accel->bar1); 

        if(config_irq(dev, accel) < 0){
            dev_info(&dev->dev, "Can not allocate pci IRQ vectors\n");
            goto out_unmap;
        }
        if(dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32))){
             dev_info(&dev->dev, "No usable DMA configuration\n");
             cif->dma_cap = false;
        }else{
             cif->dma_cap = true;
             dev_info(&dev->dev, "Device DMA capability is set\n");
        }
        pci_set_drvdata(dev, accel);
        accel->dev = dev;
        dev_info(&dev->dev, "acceldev successfully loaded and initialized\n");
        
	init_completion(&accel->dma_comp);
	init_completion(&accel->compute_comp);

	accel_prepare(cif, accel, true);
        return 0;

out_unmap:
        iounmap(accel->bar1);
out_unmap_zero:
        iounmap(accel->bar0);
out_release:
        pci_release_regions(dev);
out_disable:
        pci_disable_device(dev);
out_free:
        kfree(accel);
        dev_warn(&dev->dev, "Device registration failed\n");

        return -ENODEV;
}

static void acceldev_pci_remove(struct pci_dev *dev)
{
        struct acceldev *accel = pci_get_drvdata(dev);
        pci_set_drvdata(dev, NULL);

        if(accel->irq)
            free_irq(accel->irq, accel);
        pci_free_irq_vectors(dev);
        pci_release_regions(dev);
        iounmap(accel->bar0);
        iounmap(accel->bar1);
        pci_disable_device(dev);
        kfree(accel);
        cif->pci_dev = NULL;
}

static struct pci_device_id acceldev_pci_ids[] = {
        {
                .vendor =       0x1234,
                .device =       0x5555,
                .subvendor =    PCI_ANY_ID,
                .subdevice =    PCI_ANY_ID,
        },
        { 0, }
};

static struct pci_driver acceldev_pci_driver = {
        .name = "cs623-acceldev",
        .id_table = acceldev_pci_ids,
        .probe = acceldev_pci_probe,
        .remove = acceldev_pci_remove,
};


static int __init acceldev_pci_driver_init(void) 
{ 
        int err;
        cif = kzalloc(sizeof(struct accel_interface), GFP_KERNEL);
        BUG_ON(!cif);        
        err = pci_register_driver(&acceldev_pci_driver); 
        if(err){
            printk(KERN_INFO "unable to register PCI device\n");
            return -EINVAL;
        }
        init_waitqueue_head(&cif->wq);
        return init_chdev(cif); 
} 
module_init(acceldev_pci_driver_init); 

static void __exit acceldev_pci_driver_exit(void) 
{ 
        exit_chdev(cif);
        pci_unregister_driver(&acceldev_pci_driver);
        kfree(cif);
        cif = NULL; 
} 
module_exit(acceldev_pci_driver_exit); 

MODULE_DEVICE_TABLE(pci, acceldev_pci_ids);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("deba@cse.iitk.ac.in");
