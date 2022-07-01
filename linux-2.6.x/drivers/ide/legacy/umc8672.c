/*
 *  linux/drivers/ide/legacy/umc8672.c		Version 0.05	Jul 31, 1996
 *
 *  Copyright (C) 1995-1996  Linus Torvalds & author (see below)
 */

/*
 *  Principal Author/Maintainer:  PODIEN@hml2.atlas.de (Wolfram Podien)
 *
 *  This file provides support for the advanced features
 *  of the UMC 8672 IDE interface.
 *
 *  Version 0.01	Initial version, hacked out of ide.c,
 *			and #include'd rather than compiled separately.
 *			This will get cleaned up in a subsequent release.
 *
 *  Version 0.02	now configs/compiles separate from ide.c  -ml
 *  Version 0.03	enhanced auto-tune, fix display bug
 *  Version 0.05	replace sti() with restore_flags()  -ml
 *			add detection of possible race condition  -ml
 */

/*
 * VLB Controller Support from 
 * Wolfram Podien
 * Rohoefe 3
 * D28832 Achim
 * Germany
 *
 * To enable UMC8672 support there must a lilo line like
 * append="ide0=umc8672"...
 * To set the speed according to the abilities of the hardware there must be a
 * line like
 * #define UMC_DRIVE0 11
 * in the beginning of the driver, which sets the speed of drive 0 to 11 (there
 * are some lines present). 0 - 11 are allowed speed values. These values are
 * the results from the DOS speed test program supplied from UMC. 11 is the 
 * highest speed (about PIO mode 3)
 */
#define REALLY_SLOW_IO		/* some systems can safely undef this */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/ioport.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/init.h>

#include <asm/io.h>

/*
 * Default speeds.  These can be changed with "auto-tune" and/or hdparm.
 */
#define UMC_DRIVE0      1              /* DOS measured drive speeds */
#define UMC_DRIVE1      1              /* 0 to 11 allowed */
#define UMC_DRIVE2      1              /* 11 = Fastest Speed */
#define UMC_DRIVE3      1              /* In case of crash reduce speed */

static u8 current_speeds[4] = {UMC_DRIVE0, UMC_DRIVE1, UMC_DRIVE2, UMC_DRIVE3};
static const u8 pio_to_umc [5] = {0,3,7,10,11};	/* rough guesses */

/*       0    1    2    3    4    5    6    7    8    9    10   11      */
static const u8 speedtab [3][12] = {
	{0xf, 0xb, 0x2, 0x2, 0x2, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1 },
	{0x3, 0x2, 0x2, 0x2, 0x2, 0x2, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1 },
	{0xff,0xcb,0xc0,0x58,0x36,0x33,0x23,0x22,0x21,0x11,0x10,0x0}};

static void out_umc (char port,char wert)
{
	outb_p(port,0x108);
	outb_p(wert,0x109);
}

static inline u8 in_umc (char port)
{
	outb_p(port,0x108);
	return inb_p(0x109);
}

static void umc_set_speeds (u8 speeds[])
{
	int i, tmp;

	outb_p(0x5A,0x108); /* enable umc */

	out_umc (0xd7,(speedtab[0][speeds[2]] | (speedtab[0][speeds[3]]<<4)));
	out_umc (0xd6,(speedtab[0][speeds[0]] | (speedtab[0][speeds[1]]<<4)));
	tmp = 0;
	for (i = 3; i >= 0; i--) {
		tmp = (tmp << 2) | speedtab[1][speeds[i]];
	}
	out_umc (0xdc,tmp);
	for (i = 0;i < 4; i++) {
		out_umc (0xd0+i,speedtab[2][speeds[i]]);
		out_umc (0xd8+i,speedtab[2][speeds[i]]);
	}
	outb_p(0xa5,0x108); /* disable umc */

	printk ("umc8672: drive speeds [0 to 11]: %d %d %d %d\n",
		speeds[0], speeds[1], speeds[2], speeds[3]);
}

static void tune_umc (ide_drive_t *drive, u8 pio)
{
	unsigned long flags;
	ide_hwgroup_t *hwgroup = ide_hwifs[HWIF(drive)->index^1].hwgroup;

	pio = ide_get_best_pio_mode(drive, pio, 4, NULL);
	printk("%s: setting umc8672 to PIO mode%d (speed %d)\n",
		drive->name, pio, pio_to_umc[pio]);
	spin_lock_irqsave(&ide_lock, flags);
	if (hwgroup && hwgroup->handler != NULL) {
		printk(KERN_ERR "umc8672: other interface is busy: exiting tune_umc()\n");
	} else {
		current_speeds[drive->name[2] - 'a'] = pio_to_umc[pio];
		umc_set_speeds (current_speeds);
	}
	spin_unlock_irqrestore(&ide_lock, flags);
}

static int __init umc8672_probe(void)
{
	unsigned long flags;
	ide_hwif_t *hwif, *mate;

	if (!request_region(0x108, 2, "umc8672")) {
		printk(KERN_ERR "umc8672: ports 0x108-0x109 already in use.\n");
		return 1;
	}
	local_irq_save(flags);
	outb_p(0x5A,0x108); /* enable umc */
	if (in_umc (0xd5) != 0xa0) {
		local_irq_restore(flags);
		printk(KERN_ERR "umc8672: not found\n");
		release_region(0x108, 2);
		return 1;  
	}
	outb_p(0xa5,0x108); /* disable umc */

	umc_set_speeds (current_speeds);
	local_irq_restore(flags);

	hwif = &ide_hwifs[0];
	mate = &ide_hwifs[1];

	hwif->chipset = ide_umc8672;
	hwif->tuneproc = &tune_umc;
	hwif->mate = mate;

	mate->chipset = ide_umc8672;
	mate->tuneproc = &tune_umc;
	mate->mate = hwif;
	mate->channel = 1;

	probe_hwif_init(hwif);
	probe_hwif_init(mate);

	return 0;
}

/* Can be called directly from ide.c. */
int __init umc8672_init(void)
{
	if (umc8672_probe())
		return -ENODEV;
	return 0;
}

#ifdef MODULE
static void __exit umc8672_release_hwif(ide_hwif_t *hwif)
{
	if (hwif->chipset != ide_umc8672)
		return;

	hwif->chipset = ide_unknown;
	hwif->tuneproc = NULL;
	hwif->mate = NULL;
	hwif->channel = 0;
}

static void __exit umc8672_exit(void)
{
	unsigned long flags;

	umc8672_release_hwif(&ide_hwifs[0]);
	umc8672_release_hwif(&ide_hwifs[1]);

	local_irq_save(flags);
	outb_p(0xa5, 0x108);	/* disable umc */
	local_irq_restore(flags);

	release_region(0x108, 2);
}

module_init(umc8672_init);
module_exit(umc8672_exit);
#endif

MODULE_AUTHOR("Wolfram Podien");
MODULE_DESCRIPTION("Support for UMC 8672 IDE chipset");
MODULE_LICENSE("GPL");