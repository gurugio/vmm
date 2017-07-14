/*
 * Ram backed block device driver.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <linux/blk-mq.h>
#include <linux/nodemask.h>
#include <asm/msr.h>

#ifdef pr_warn
#undef pr_warn
#endif
#define pr_warn(fmt, arg...) printk(KERN_WARNING "mybrd: "fmt, ##arg)

MODULE_LICENSE("GPL");

#define ASM_VMX_VMXON_RAX         ".byte 0xf3, 0x0f, 0xc7, 0x30"
#define ASM_VMX_VMXOFF            ".byte 0x0f, 0x01, 0xc4"


struct my_vmcs {
	u32 revision_id;
	u32 abort;
	char data[0];
};

/* */
static struct my_vmcs *vmxon_region;
static struct my_vmcs *guest_region;

/* page tables of MMU of guest */
static u32 *page_dir;
static u32 *page_table;
static unsigned long g_IDT_region;
static unsigned long g_GDT_region;
static unsigned long g_LDT_region;
static unsigned long g_TSS_region;
static unsigned long g_TOS_region;
static unsigned long h_MSR_region;

struct regs_ia32 {
	unsigned int	eip;
	unsigned int	eflags;
	unsigned int	eax;
	unsigned int	ecx;
	unsigned int	edx;
	unsigned int	ebx;
	unsigned int	esp;
	unsigned int	ebp;
	unsigned int	esi;
	unsigned int	edi;
	unsigned int	 es;
	unsigned int	 cs;
	unsigned int	 ss;
	unsigned int	 ds;
	unsigned int	 fs;
	unsigned int	 gs;
};

static struct regs_ia32 guest_regs = {
	.eflags = 0x00023000;
	.esp	  = 0x7FFA;
	.ss	  = 0x0000;

	// put recognizable values in the other registers 
	.eax	= 0xAAAAAAAA;
	.ebx	= 0xBBBBBBBB;
	.ecx	= 0xCCCCCCCC;
	.edx	= 0xDDDDDDDD;
	.ebp	= 0xBBBBBBBB;
	.esi	= 0xCCCCCCCC;
	.edi	= 0xDDDDDDDD;
	.ds	= 0xDDDD;
	.es	= 0xEEEE;
	.fs	= 0x8888;
	.gs	= 0x9999;
};



void enable_vmx(void *not_used)
{
	unsigned long val;
	unsigned long old;
	int cpu = smp_processor_id();
	
	asm volatile ("mov %%cr4, %0\n\t"
		      : "=r" (val), "=m" (__force_order));
	old = val;

	val |= (1UL << 13);
	asm volatile ("mov %0, %%cr4\n\t"
		      :
		      : "r" (val), "m" (__force_order));
	asm volatile ("mov %%cr4, %0\n\t"
		      : "=r" (val), "=m" (__force_order));
	pr_warn("cpu=%d cr4=%lx => %lx", cpu, old, val);
}

void disable_vmx(void *not_used)
{
	unsigned long val;
	unsigned long old;
	int cpu = smp_processor_id();

	asm volatile ("mov %%cr4, %0\n\t"
		      : "=r" (val), "=m" (__force_order));
	old = val;

	val &= ~(1UL << 13);
	asm volatile ("mov %0, %%cr4\n\t"
		      :
		      : "r" (val), "m" (__force_order));
	asm volatile ("mov %%cr4, %0\n\t"
		      : "=r" (val), "=m" (__force_order));
	pr_warn("cpu=%d cr4=%lx => %lx", cpu, old, val);
}

u32 read_revisionid(void)
{
	u32 low, high;

	rdmsr(MSR_IA32_VMX_BASIC, low, high);
	pr_warn("VMX_BASIC: vmx region size=%d\n", high & 0x1fff);
	pr_warn("VMX_BASIC: revision id=%d\n", low);
	return low;
}

int check_feature(void)
{
	u64 old;

	rdmsrl(MSR_IA32_FEATURE_CONTROL, old);
	pr_warn("MST_IA32_FEATURE_CONTROL=%llx\n", old);
	pr_warn("FEATURE_CONTROL_LOCKED=%d\n", !!(old & (1<<0)));
	pr_warn("FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX=%d\n", !!(old & (1<<2)));
	if (!(old & (1<<0)) || !(old & (1<<2)))
		pr_warn("FEATURE_CONTROL error=%lldx\n", old);
	return (old & (1<<0)) && (old & (1<<2));
}

void setup_guest_mmu(void)
{
	unsigned long page_regions;
	int order = 4;
	int i;

	page_dir = (unsigned long *)__get_free_page(GFP_KERNEL);
	page_table = (unsigned long *)__get_free_page(GFP_KERNEL);

	page_regions = __get_free_pages(GFP_KERNEL, order); // 16pages
	for (i = 0; i < (1 << order); i++) {
		page_dir[i] = __pa(page_region) /* base */
			+ (i << PAGE_SHIFT) /* page# */
			+ 0x7 /* flag */;
	}
	page_table[0] = __pa(page_dir) + 0x7;
}

void run_16bit_vm(void)
{
	struct page *p;
	u64 phy_vmcs;
	u32 revision_id;
	/*
	 * BUGBUG: what is 0x11??
	 */
	unsigned int	interrupt_number = 0x11;  // <--changed on 5/4/2007
	unsigned int	vector = *(unsigned int*)( interrupt_number << 2 );
	// plant the 'return' stack and code
	unsigned short	*tos = (unsigned short*)0x8000;
	unsigned int	*eoi = (unsigned int*)0x8000;

	/*
	 * vmxon
	 */
	revision_id = read_revisionid();

	p = alloc_pages(GFP_KERNEL, 0);
	vmxon_region = page_address(p);
	memset(vmxon_region, 0, PAGE_SIZE);
	/* revision id must be written before VMXON */
	vmxon_region->revision_id = revision_id;

	p = alloc_pages(GFP_KERNEL, 0);
	guest_region = page_address(p);
	memset(guest_region, 0, PAGE_SIZE);
	guest_region->revision_id = revision_id;

	/*
	 * page table of guest
	 */
	setup_guest_mmu();

	/*
	 * setup guest program
	 */
	pr_warn("interrupt-0x%02X: ", interrupt_number );
	pr_warn("vector = %08X \n", vector );

	// setup transition to our Virtual Machine Monitor
	eoi[ 0 ] = 0x90C1010F;	// 'vmcall' instruction

	tos[ -1 ] = 0x0000;	// image of FLAGS
	tos[ -2 ] = 0x0000;	// image of CS
	tos[ -3 ] = 0x8000;	// image of IP

	regs.eip = vector & 0xFFFF;
	regs.cs = (vector >> 16);


	/*
	 * 2017.07.14
	 */




	
	phy_vmcs = __pa(vmxon_region);
	pr_warn("phy-vmcs=%llx\n", phy_vmcs);
	asm volatile (ASM_VMX_VMXON_RAX
		      :
		      : "a" (&phy_vmcs), "m" (phy_vmcs)
		      : "memory", "cc");
	pr_warn("vmcs:id=%d\n", vmxon_region->revision_id);


}

static int __init mybrd_init(void)
{
	
	pr_warn("\n\n\nmybrd: module loaded\n\n\n\n");

	/*
	 * check msr
	 */
	if (!check_feature()) {
		return 0;
	}

	/*
	 * enable VMX in cr4
	 */
	enable_vmx(NULL); // mandatory, don't know why
	smp_call_function(enable_vmx, NULL, 1);

	run_16bit_vm();
	
	return 0;
}

static void __exit mybrd_exit(void)
{
	/*
	 * vmxoff
	 */
	//asm volatile (ASM_VMX_VMXOFF : : : "cc");

	/* free vmcs */
	/* pr_warn("vmx revision id=%d\n", vmxon_region->revision_id); */
	/* free_page((unsigned long)vmxon_region); */

	disable_vmx(NULL);
	smp_call_function(disable_vmx, NULL, 1);

	pr_warn("brd: module unloaded\n");
}

module_init(mybrd_init);
module_exit(mybrd_exit);

