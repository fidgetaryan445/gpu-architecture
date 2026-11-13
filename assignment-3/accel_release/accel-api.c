#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>     
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include<linux/uaccess.h>
#include<acceldev.h>
#include<interface.h>

void accel_prepare(struct accel_interface *cif, struct acceldev *accel, bool dmacap)
{
   struct compute_context *c;
   
   cif->pci_dev = accel;
   cif->dma_cap = dmacap;

   //Lets query the device capability and fillup the structures
   c = kzalloc(sizeof(struct compute_context), GFP_KERNEL); 
   BUG_ON(!c);
   c->kva = accel->bar1;  
   //Quirk for first 16 bytes, we start from an offset of one page
   c->start = 0x1000;
   c->end = accel->size_bar1 - c->start;   
 
   //Initializing things for compute
   _cwrite(0, accel->bar1, BAR1_SRC_ONE);  
   _cwrite(0, accel->bar1, BAR1_SRC_TWO);  
   _cwrite(0, accel->bar1, BAR1_DST);  
   _cwrite(0, accel->bar1, BAR1_COUNT);
   _cwrite(ACCEL_STATUS_IRQ, accel->bar0, BAR0_STATUS); //enable compute interrupts

   INIT_LIST_HEAD(&c->mem_range_list); 
   cif->devmap = c;
   
   c = kzalloc(sizeof(struct compute_context), GFP_KERNEL); 
   BUG_ON(!c);
   c->kva = NULL;
   c->start = (unsigned long)_creadq(accel->bar0, BAR0_ACCEL_RAM_START); 
   c->end = c->start + (unsigned long)_creadq(accel->bar0, BAR0_ACCEL_RAM_SIZE);  
   INIT_LIST_HEAD(&c->mem_range_list); 
   cif->devram = c;

   set_accelops(cif); 
   //Finally make the device ready
   cif->status = ACI_STATUS_READY;
   accel->private = (void *)cif;  
   return;
}

static ssize_t accel_read(struct file *filp, char *buff, size_t len, loff_t * off)
{
       int err = len;
       struct msg msg;
       struct accel_interface *cif = filp->private_data;

       BUG_ON(!cif);

       if(cif->status == ACI_STATUS_DOWN || !cif->pci_dev){
             err = -ENODEV;
	     sprintf(msg.status, "Acceldev is not present in your system");
	     return err;
       }

       if(copy_from_user(&msg, buff, len)){
                err =  -EINVAL;
		return err;
       }

       switch(msg.cmd){
	       case OP_DEVQ:
		              if(cif->devops->accel_dev_query)
				      err = cif->devops->accel_dev_query(cif, &msg);
			      else
				      err = -EINVAL;
			      break;
	       case OP_DEVFLAGS: 
		              if(cif->devops->accel_set_modes)
				      err = cif->devops->accel_set_modes(cif, &msg);
			      else
				      err = -EINVAL;
			      break;
	       case OP_ALLOC:
		              if(cif->devops->accel_alloc)
				      err = cif->devops->accel_alloc(cif, &msg);
			      else
				      err = -EINVAL;
			      break;
	       case OP_FREE:
		              if(cif->devops->accel_free)
				      err = cif->devops->accel_free(cif, &msg);
			      else
				      err = -EINVAL;
			      break;
	       case OP_DEVCOPY:
		              if(cif->devops->accel_copy)
				      err = cif->devops->accel_copy(cif, &msg);
			      else
				      err = -EINVAL;
			      break;
	       case OP_VADD:
	       case OP_VMUL:
	       case OP_VXOR:
		              if(cif->devops->accel_compute)
				      err = cif->devops->accel_compute(cif, &msg);
			      else
				      err = -EINVAL;
			      break;

	       case OP_RESET:
		              if(cif->devops->accel_reset){
				      spin_lock(&cif->lock);
				      err = cif->devops->accel_reset(cif, &msg);
				      spin_unlock(&cif->lock);
			      }
			      else
				      err = -EINVAL;
			      break;
	       default:
			sprintf(msg.status, "Acceldev: not supported");
		        err = -ENOTSUPP;
	                break; 	       
       }
       if(copy_to_user(buff, &msg, len))  //Copy anyway
               err =  -EINVAL;
       return err;
}
static ssize_t accel_write(struct file *filp, const char *buff, size_t len, loff_t * off)
{
       printk(KERN_INFO "Not supported\n");
       return -EINVAL;
}
static int accel_open(struct inode *inode, struct file *filp)
{
        struct cdev *acdev = inode->i_cdev;
        struct accel_interface *cif= container_of(acdev, struct accel_interface, acdev);
        BUG_ON(!cif);
       if(cif->status == ACI_STATUS_DOWN || !cif->pci_dev){
	     printk(KERN_INFO "Acceldev is not present in your system");
	     return -ENODEV;
       }
	
	spin_lock(&cif->lock);
        filp->private_data = cif;
        cif->usage++;
	spin_unlock(&cif->lock);
        
	try_module_get(THIS_MODULE);
        printk(KERN_INFO "Device opened successfully\n");
        return 0;
}

static int accel_release(struct inode *inode, struct file *filp)
{
        struct accel_interface *cif = filp->private_data;
        BUG_ON(!cif);
	
	spin_lock(&cif->lock);
        cif->usage--;
	BUG_ON(cif->usage < 0);
	if(!cif->usage && cif->status > ACI_STATUS_DOWN){
	     BUG_ON(!cif->devops->accel_reset);	
	     cif->devops->accel_reset(cif, NULL);
	     cif->status = ACI_STATUS_READY;
	}
	spin_unlock(&cif->lock);
        
	module_put(THIS_MODULE);
        printk(KERN_INFO "Device closed successfully\n");
        return 0;
}
static struct file_operations fops = {
        .read = accel_read,
        .write = accel_write,
        .open = accel_open,
        .release = accel_release,
};

static char *cif_devnode(struct device *dev, umode_t *mode)
{
        struct accel_interface *cif = dev_get_drvdata(dev);
        if (cif && mode && dev->devt == MKDEV(cif->major, 0))
                *mode = 0666;
        return NULL;
}

int init_chdev(struct accel_interface *cif)
{
        struct device *dev;
        BUG_ON(!cif);
        
        if(alloc_chrdev_region(&cif->dev, 0, 1, DEVNAME)){
            pr_err("region create failed\n");
            goto out_ret;
        }

        cif->cif_class = class_create(THIS_MODULE, DEVNAME);
        if (IS_ERR(cif->cif_class)) {
                pr_err("class_create failed\n");
                goto out_unreg;
        }
        cif->cif_class->devnode = cif_devnode;
        cif->major = MAJOR(cif->dev);
        cdev_init(&cif->acdev, &fops);
        if (cdev_add(&cif->acdev, cif->dev, 1)) {
                pr_err("accel add failed\n");
                goto out_class;
        }


        dev = device_create(cif->cif_class, NULL,
                                   MKDEV(cif->major, 0),
                                   cif, DEVNAME);

        if (IS_ERR(dev)) {
                pr_err("failed to create device\n");
                goto out_accel;
        }
        spin_lock_init(&cif->lock);
        dev_set_drvdata(dev, cif);

        return 0;
out_accel:
        cdev_del(&cif->acdev);
out_class:
        class_destroy(cif->cif_class);
out_unreg:
        unregister_chrdev_region(cif->dev,1);
out_ret:
        return -EINVAL;
}
void exit_chdev(struct accel_interface *cif)
{
        device_destroy(cif->cif_class, MKDEV(cif->major, 0));
        cdev_del(&cif->acdev);
        class_destroy(cif->cif_class);
        unregister_chrdev_region(MKDEV(cif->major, 0), 1);
}
