#include<linux/module.h>
#include<linux/kernel.h>
#include<linux/sched.h>
#include<linux/mm.h>
#include<linux/mm_types.h>
#include<linux/io.h>

/*Definitions taken from specs*/
#define APIC_ID_OFFSET 0x20
#define APIC_VERSION_OFFSET 0x30


/*XXX Hard-coded, double check with in proc iomem*/
static unsigned long apic_mmio_base = 0xfee00000;
static void __iomem* apic_map;

static pte_t* get_pte(struct mm_struct *mm, unsigned long address)
{
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *ptep;
       

        pgd = pgd_offset(mm, address);
        if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
                goto nul_ret;
        p4d = p4d_offset(pgd, address);
        if (p4d_none(*p4d))
                goto nul_ret;
        if (unlikely(p4d_bad(*p4d)))
                goto nul_ret;
        pud = pud_offset(p4d, address);
        if (pud_none(*pud))
                goto nul_ret;
        if (unlikely(pud_bad(*pud)))
                goto nul_ret;
        pmd = pmd_offset(pud, address);
        if (!pmd || pmd_none(*pmd))
                goto nul_ret;

        if (unlikely(pmd_large(*pmd))){
                printk(KERN_INFO "I am huge\n");
                return NULL;
        }

        ptep = pte_offset_map(pmd, address);
        if(!ptep){
                printk(KERN_INFO "pte_p is null\n\n");
                goto nul_ret;
        }
        return ptep;

        nul_ret:
               return NULL;

}

static ssize_t mmio_apic_id(struct kobject *kobj,
                                  struct kobj_attribute *attr, char *buf)
{
	unsigned value = ioread32(apic_map + APIC_ID_OFFSET);
        return sprintf(buf, "%x\n", value);
}
static struct kobj_attribute mmio_attribute_init = __ATTR(apic_id, 0444, mmio_apic_id, NULL);

static ssize_t mmio_apic_version(struct kobject *kobj,
                                  struct kobj_attribute *attr, char *buf)
{
	unsigned value = ioread32(apic_map + APIC_VERSION_OFFSET);
        return sprintf(buf, "%x\n", value);
}
static struct kobj_attribute mmio_attribute_current = __ATTR(apic_version, 0444, mmio_apic_version, NULL);

static struct attribute *apic_mmio_attrs[] = {
        &mmio_attribute_current.attr,
        &mmio_attribute_init.attr,
        NULL,
};

static struct attribute_group cs614_attr_group = {
        .attrs = apic_mmio_attrs,
        .name = "cs614",
};

int init_module(void)
{
	pte_t *ptep;
        int ret = sysfs_create_group (kernel_kobj, &cs614_attr_group);
        if(unlikely(ret)){
                printk(KERN_INFO "demo: can't create sysfs\n");
		return -1;
	}

	apic_map = ioremap(apic_mmio_base, PAGE_SIZE);
	if(!apic_map){
                printk(KERN_INFO "demo: can't iomap apic device\n");
                sysfs_remove_group (kernel_kobj, &cs614_attr_group);
                return -1;
	}
	printk(KERN_INFO "All set to play apic mapped to %lx __pa = %lx\n", (unsigned long)apic_map, __pa(apic_map));
        ptep = get_pte(current->mm, (unsigned long)apic_map);
	if(ptep)
	     printk(KERN_INFO "IO mapping *ptep = %lx\n", ptep->pte);
	return 0;
}

void cleanup_module(void)
{
        sysfs_remove_group (kernel_kobj, &cs614_attr_group);
	iounmap(apic_map);
	printk(KERN_INFO "Clenup done\n");

}

MODULE_AUTHOR("deba@cse.iitk.ac.in");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CS614 example modules");
