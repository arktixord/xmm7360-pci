// vim: noet ts=8 sw=8

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/wait.h>

MODULE_LICENSE("Dual BSD/GPL");

static struct pci_device_id xmm7360_ids[] = {
	{ PCI_DEVICE(0x8086, 0x7360), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, xmm7360_ids);

static dev_t xmm_base;

/* Command ring, which is used to configure the queue pairs */
struct cmd_ring_entry {
	dma_addr_t ptr;
	u16 len;
	u8 parm;
	u8 cmd;
	u32 extra;
	u32 unk, flags;
};

#define CMD_RING_OPEN	1
#define CMD_RING_CLOSE	3
#define CMD_WAKEUP	4

#define CMD_FLAG_DONE	1
#define CMD_FLAG_READY	2

/* Transfer descriptors used on the Tx and Rx rings of each queue pair */
struct td_ring_entry {
	dma_addr_t addr;
	u16 length;
	u16 flags;
	u32 unk;
};

#define TD_FLAG_COMPLETE 0x200

/* Root configuration object. This contains pointers to all of the control
 * structures that the modem will interact with.
 */
struct control {
	dma_addr_t status;
	dma_addr_t s_wptr, s_rptr;
	dma_addr_t c_wptr, c_rptr;
	dma_addr_t c_ring;
	u16 c_ring_size;
	u16 unk;
};

struct status {
	u32 code;
	u32 mode;
	u32 asleep;
	u32 pad;
};

#define CMD_RING_SIZE 0x80

/* All of the control structures can be packed into one page of RAM. */
struct control_page {
	struct control ctl;
	// Status words - written by modem.
	volatile struct status status;
	// Slave ring write/read pointers.
	volatile u32 s_wptr[16], s_rptr[16];
	// Command ring write/read pointers.
	volatile u32 c_wptr, c_rptr;
	// Command ring entries.
	volatile struct cmd_ring_entry c_ring[CMD_RING_SIZE];
};

#define BAR0_MODE	0x0c
#define BAR0_DOORBELL	0x04
#define BAR0_WAKEUP	0x14

#define DOORBELL_TD	0
#define DOORBELL_CMD	1

#define BAR2_STATUS	0x00
#define BAR2_MODE	0x18
#define BAR2_CONTROL	0x19
#define BAR2_CONTROLH	0x1a

#define BAR2_BLANK0	0x1b
#define BAR2_BLANK1	0x1c
#define BAR2_BLANK2	0x1d
#define BAR2_BLANK3	0x1e

/* There are 16 TD rings: a Tx and Rx ring for each queue pair */
struct td_ring {
	u8 size;
	u8 last_handled;
	u16 page_size;

	struct td_ring_entry *tds;
	dma_addr_t tds_phys;

	// One page of page_size per td
	void **pages;
	dma_addr_t *pages_phys;
};

struct queue_pair {
	struct xmm_dev *xmm;
	struct cdev cdev;
	struct device dev;
	int num;
	int open;
	wait_queue_head_t wq;
	spinlock_t lock;
};

struct xmm_dev {
	struct device *dev;
	struct pci_dev *pci_dev;

	volatile uint32_t *bar0, *bar2;

	int irq[4];
	wait_queue_head_t wq;

	volatile struct control_page *cp;
	dma_addr_t cp_phys;

	struct td_ring td_ring[16];

	struct queue_pair qp[8];
};

static void xmm7360_dump(struct xmm_dev *xmm)
{
	pr_info("xmm %08x slp %d cmd %d:%d\n", xmm->cp->status.code, xmm->cp->status.asleep, xmm->cp->c_rptr, xmm->cp->c_wptr);
	pr_info("xmm r2 %d:%d r3 %d:%d\n", xmm->cp->s_rptr[2], xmm->cp->s_wptr[2], xmm->cp->s_rptr[3], xmm->cp->s_wptr[3]);
}

static void xmm7360_ding(struct xmm_dev *xmm, int bell)
{
	if (xmm->cp->status.asleep)
		xmm->bar0[BAR0_WAKEUP] = 1;
	xmm->bar0[BAR0_DOORBELL] = bell;
}

static int xmm7360_cmd_ring_wait(struct xmm_dev *xmm)
{
	// Wait for all commands to complete
	return wait_event_interruptible(xmm->wq, xmm->cp->c_rptr == xmm->cp->c_wptr);
}

static int xmm7360_cmd_ring_submit(struct xmm_dev *xmm, u8 cmd, u8 parm, u16 len, dma_addr_t ptr, u32 extra)
{
	u8 wptr = xmm->cp->c_wptr;
	u8 new_wptr = (wptr + 1) % CMD_RING_SIZE;
	if (new_wptr == xmm->cp->c_rptr)	// ring full
		return -EAGAIN;

	pr_info("xmm7360_cmd_ring_submit %x %02x %04x %llx\n", cmd, parm, len, ptr);

	xmm->cp->c_ring[wptr].ptr = ptr;
	xmm->cp->c_ring[wptr].cmd = cmd;
	xmm->cp->c_ring[wptr].parm = parm;
	xmm->cp->c_ring[wptr].len = len;
	xmm->cp->c_ring[wptr].extra = extra;
	xmm->cp->c_ring[wptr].unk = 0;
	xmm->cp->c_ring[wptr].flags = CMD_FLAG_READY;

	xmm->cp->c_wptr = new_wptr;

	return 0;
}

static int xmm7360_cmd_ring_init(struct xmm_dev *xmm) {
	int timeout;
	int ret;

	xmm->cp = dma_alloc_coherent(xmm->dev, sizeof(struct control_page), &xmm->cp_phys, GFP_KERNEL);

	xmm->cp->ctl.status = xmm->cp_phys + offsetof(struct control_page, status);
	xmm->cp->ctl.s_wptr = xmm->cp_phys + offsetof(struct control_page, s_wptr);
	xmm->cp->ctl.s_rptr = xmm->cp_phys + offsetof(struct control_page, s_rptr);
	xmm->cp->ctl.c_wptr = xmm->cp_phys + offsetof(struct control_page, c_wptr);
	xmm->cp->ctl.c_rptr = xmm->cp_phys + offsetof(struct control_page, c_rptr);
	xmm->cp->ctl.c_ring = xmm->cp_phys + offsetof(struct control_page, c_ring);
	xmm->cp->ctl.c_ring_size = CMD_RING_SIZE;

	xmm->bar2[BAR2_CONTROL] = xmm->cp_phys;
	xmm->bar2[BAR2_CONTROLH] = xmm->cp_phys >> 32;

	xmm->bar0[BAR0_MODE] = 1;


	timeout = 100;
	while (xmm->bar2[BAR2_MODE] == 0 && --timeout)
		msleep(10);

	if (!timeout)
		return -ETIMEDOUT;

	xmm->bar2[BAR2_BLANK0] = 0;
	xmm->bar2[BAR2_BLANK1] = 0;
	xmm->bar2[BAR2_BLANK2] = 0;
	xmm->bar2[BAR2_BLANK3] = 0;

	xmm->bar0[BAR0_MODE] = 2;	// enable intrs?

	timeout = 100;
	while (xmm->bar2[BAR2_MODE] != 2 && !--timeout)
		msleep(10);

	if (!timeout)
		return -ETIMEDOUT;

	ret = xmm7360_cmd_ring_submit(xmm, CMD_WAKEUP, 0, 1, 0, 0);
	if (ret)
		return ret;
	ret = xmm7360_cmd_ring_submit(xmm, 0xf0, 0x80, 0, 0, 0);
	if (ret)
		return ret;

	xmm7360_dump(xmm);

	xmm7360_ding(xmm, DOORBELL_CMD);
	xmm7360_dump(xmm);

	ret = xmm7360_cmd_ring_wait(xmm);
	if (ret)
		return ret;

	xmm7360_dump(xmm);
	return 0;
}

static void xmm7360_cmd_ring_free(struct xmm_dev *xmm) {
	if (xmm->bar0)
		xmm->bar0[BAR0_MODE] = 0;
	if (xmm->cp)
		dma_free_coherent(xmm->dev, sizeof(struct control_page), (void*)xmm->cp, xmm->cp_phys);
	xmm->cp = NULL;
	return;
}

static void xmm7360_td_ring_create(struct xmm_dev *xmm, u8 ring_id, u8 size)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	int i;

	BUG_ON(ring->size);
	BUG_ON(size & (size-1));

	memset(ring, 0, sizeof(struct td_ring));
	ring->size = size;
	ring->page_size = 0x1000;
	ring->tds = dma_alloc_coherent(xmm->dev, sizeof(struct td_ring_entry)*size, &ring->tds_phys, GFP_KERNEL);

	ring->pages = kzalloc(sizeof(void*)*size, GFP_KERNEL);
	ring->pages_phys = kzalloc(sizeof(dma_addr_t)*size, GFP_KERNEL);

	for (i=0; i<size; i++) {
		ring->pages[i] = dma_alloc_coherent(xmm->dev, ring->page_size, &ring->pages_phys[i], GFP_KERNEL);
		ring->tds[i].addr = ring->pages_phys[i];
	}

	xmm->cp->s_rptr[ring_id] = xmm->cp->s_wptr[ring_id] = 0;
	xmm7360_cmd_ring_submit(xmm, CMD_RING_OPEN, ring_id, size, ring->tds_phys, 0x60);
	xmm7360_ding(xmm, DOORBELL_CMD);
}

static void xmm7360_td_ring_destroy(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	int i, size=ring->size;

	if (!size) {
		WARN_ON(1);
		pr_err("Tried destroying empty ring!\n");
		return;
	}

	xmm7360_cmd_ring_submit(xmm, CMD_RING_CLOSE, ring_id, 0, 0, 0);
	xmm7360_ding(xmm, DOORBELL_CMD);

	for (i=0; i<size; i++) {
		dma_free_coherent(xmm->dev, ring->page_size, ring->pages[i], ring->pages_phys[i]);
	}

	kfree(ring->pages_phys);
	kfree(ring->pages);

	dma_free_coherent(xmm->dev, sizeof(struct td_ring_entry)*size, ring->tds, ring->tds_phys);

	ring->size = 0;
}

static void xmm7360_td_ring_write_user(struct xmm_dev *xmm, u8 ring_id, const void __user *buf, size_t len)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];

	BUG_ON(!ring->size);
	BUG_ON(len > ring->page_size);
	BUG_ON(ring_id & 1);

	copy_from_user(ring->pages[wptr], buf, len);
	ring->tds[wptr].length = len;
	ring->tds[wptr].flags = 0;
	ring->tds[wptr].unk = 0;

	print_hex_dump(KERN_INFO, "xmm write ", DUMP_PREFIX_OFFSET, 8, 16, ring->pages[wptr], len, 1);

	wptr = (wptr + 1) & (ring->size - 1);
	BUG_ON(wptr == xmm->cp->s_rptr[ring_id]);

	xmm->cp->s_wptr[ring_id] = wptr;
}

static void xmm7360_td_ring_write(struct xmm_dev *xmm, u8 ring_id, const void *buf, int len)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];

	BUG_ON(!ring->size);
	BUG_ON(len > ring->page_size);
	BUG_ON(ring_id & 1);

	memcpy(ring->pages[wptr], buf, len);
	ring->tds[wptr].length = len;
	ring->tds[wptr].flags = 0;
	ring->tds[wptr].unk = 0;

	wptr = (wptr + 1) & (ring->size - 1);
	BUG_ON(wptr == xmm->cp->s_rptr[ring_id]);

	xmm->cp->s_wptr[ring_id] = wptr;
}

static int xmm7360_td_ring_full(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];
	wptr = (wptr + 1) & (ring->size - 1);
	return wptr == xmm->cp->s_rptr[ring_id];
}

static void xmm7360_td_ring_read(struct xmm_dev *xmm, u8 ring_id)
{
	struct td_ring *ring = &xmm->td_ring[ring_id];
	u8 wptr = xmm->cp->s_wptr[ring_id];

	if (!ring->size) {
		pr_err("read on disabled ring\n");
		WARN_ON(1);
		return;
	}
	if (!(ring_id & 1)) {
		pr_err("read on write ring\n");
		WARN_ON(1);
		return;
	}

	ring->tds[wptr].length = ring->page_size;
	ring->tds[wptr].flags = 0;
	ring->tds[wptr].unk = 0;

	wptr = (wptr + 1) & (ring->size - 1);
	BUG_ON(wptr == xmm->cp->s_rptr[ring_id]);

	xmm->cp->s_wptr[ring_id] = wptr;
}

static int xmm7360_qp_start(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	int ret;

	spin_lock(&qp->lock);

	if (qp->open) {
		ret = -EBUSY;
	} else {
		ret = 0;
		qp->open = 1;

		pr_info("xmm: opening qp %d\n", qp->num);
		xmm7360_td_ring_create(xmm, qp->num*2, 8);
		xmm7360_td_ring_create(xmm, qp->num*2+1, 8);
		xmm7360_ding(xmm, DOORBELL_CMD);
		while (!xmm7360_td_ring_full(xmm, qp->num*2+1))
			xmm7360_td_ring_read(xmm, qp->num*2+1);
		xmm7360_ding(xmm, DOORBELL_TD);
	}

	spin_unlock(&qp->lock);

	return ret;
}

static int xmm7360_qp_stop(struct queue_pair *qp)
{
	struct xmm_dev *xmm = qp->xmm;
	int ret = 0;

	spin_lock(&qp->lock);
	if (!qp->open) {
		ret = -ENODEV;
	} else {
		ret = 0;
		qp->open = 0;

		xmm7360_td_ring_destroy(xmm, qp->num*2);
		xmm7360_td_ring_destroy(xmm, qp->num*2+1);
		pr_info("xmm: closing qp %d\n", qp->num);
	}
	spin_unlock(&qp->lock);
	return ret;
}

static size_t xmm7360_qp_write_user(struct queue_pair *qp, const char __user *buf, size_t size)
{
	struct xmm_dev *xmm = qp->xmm;
	if (xmm7360_td_ring_full(xmm, qp->num*2))
		return -ENOSPC;
	pr_info("xmm7360_write: %ld bytes to qp %d\n", size, qp->num);
	xmm7360_td_ring_write_user(xmm, qp->num*2, buf, size);
	xmm7360_ding(xmm, DOORBELL_TD);
	return 0;
}

static size_t xmm7360_qp_read_user(struct queue_pair *qp, char __user *buf, size_t size)
{
	struct xmm_dev *xmm = qp->xmm;
	struct td_ring *ring = &xmm->td_ring[qp->num*2+1];
	int idx, nread, ret;
	pr_err("xmm7360_qp_read_user: initial rptr %d, lh %d\n", xmm->cp->s_rptr[qp->num*2+1], ring->last_handled);
	ret = wait_event_interruptible(qp->wq, xmm->cp->s_rptr[qp->num*2+1] != ring->last_handled);
	if (ret)
		return ret;
	pr_err("xmm7360_qp_read_user: mid rptr %d, lh %d\n", xmm->cp->s_rptr[qp->num*2+1], ring->last_handled);

	idx = ring->last_handled;
	nread = ring->tds[idx].length;
	pr_err("Ring length: %x Requested length: %lx Page size: %x\n",  nread, size, ring->page_size);
	if (nread > size)
		nread = size;
	copy_to_user(buf, ring->pages[idx], nread);
	//print_hex_dump(KERN_INFO, "xmm read ", DUMP_PREFIX_OFFSET, 8, 16, ring->pages[idx], ring->trbs[idx].length, 1);

	xmm7360_td_ring_read(xmm, qp->num*2+1);
	xmm7360_ding(xmm, DOORBELL_TD);
	ring->last_handled = (idx + 1) & (ring->size - 1);
	return nread;
}

int xmm7360_open (struct inode *inode, struct file *file)
{
	struct queue_pair *qp = container_of(inode->i_cdev, struct queue_pair, cdev);
	file->private_data = qp;
	pr_info("xmm7360_open %d\n", qp->num);
	return xmm7360_qp_start(qp);
}

int xmm7360_release (struct inode *inode, struct file *file)
{
	struct queue_pair *qp = file->private_data;
	pr_info("xmm7360_release %d\n", qp->num);
	return xmm7360_qp_stop(qp);
}

ssize_t xmm7360_write (struct file *file, const char __user *buf, size_t size, loff_t *offset)
{
	struct queue_pair *qp = file->private_data;
	int ret;

	pr_info("xmm7360_write %d %ld\n", qp->num, size);
	ret = xmm7360_qp_write_user(qp, buf, size);
	if (ret)
		return ret;

	*offset += size;
	return size;
}

ssize_t xmm7360_read (struct file *file, char __user *buf, size_t size, loff_t *offset)
{	struct queue_pair *qp = file->private_data;

	size_t ret = xmm7360_qp_read_user(qp, buf, size);
	if (ret < 0)
		return ret;
	*offset += ret;
	return ret;
}

static struct file_operations xmm7360_fops = {
	.read		= xmm7360_read,
	.write		= xmm7360_write,
	.open		= xmm7360_open,
	.release	= xmm7360_release
};

static irqreturn_t xmm7360_irq0(int irq, void *dev_id) {
	struct xmm_dev *xmm = dev_id;
	int id;

	pr_info("xmm irq0\n");
	wake_up(&xmm->wq);
	xmm7360_dump(xmm);
	if (xmm->td_ring) {
		for (id=0; id<8; id++) {
			if (xmm->qp[id].open)
				wake_up(&xmm->qp[id].wq);
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t xmm7360_irq(int irq, void *dev) {
	pr_info("\n\n\nxmm irq!!! %d %p\n", irq, dev);
	return IRQ_HANDLED;
}

static irq_handler_t xmm7360_irq_handlers[] = {
	xmm7360_irq0,
	xmm7360_irq,
	xmm7360_irq,
	xmm7360_irq,
};

static void xmm7360_remove(struct pci_dev *dev)
{
	struct xmm_dev *xmm = pci_get_drvdata(dev);
	int i;

	for (i=0; i<8; i++) {	// XXX
		if (xmm->qp[i].xmm) {
			cdev_del(&xmm->qp[i].cdev);
			device_unregister(&xmm->qp[i].dev);
		}
	}
	xmm7360_cmd_ring_free(xmm);

	for (i=0; i<4; i++) {
		if (xmm->irq[i])
			free_irq(xmm->irq[i], xmm);
	}
	pci_free_irq_vectors(dev);
	pci_release_region(dev, 0);
	pci_release_region(dev, 2);
	pci_disable_device(dev);
	kfree(xmm);
}

static void xmm7360_cdev_dev_release(struct device *dev)
{
}

static int xmm7360_create_cdev(struct xmm_dev *xmm, int num)
{
	struct queue_pair *qp = &xmm->qp[num];
	int ret;

	qp->xmm = xmm;
	qp->num = num;
	qp->open = 0;

	spin_lock_init(&qp->lock);
	init_waitqueue_head(&qp->wq);
	cdev_init(&qp->cdev, &xmm7360_fops);
	qp->cdev.owner = THIS_MODULE;
	device_initialize(&qp->dev);
	qp->dev.devt = MKDEV(MAJOR(xmm_base), num); // XXX multiple cards
	qp->dev.parent = &xmm->pci_dev->dev;
	qp->dev.release = xmm7360_cdev_dev_release;
	dev_set_name(&qp->dev, "xmm%d", num);
	dev_set_drvdata(&qp->dev, qp);
	ret = cdev_device_add(&qp->cdev, &qp->dev);
	if (ret) {
		pr_err("cdev_device_add: %d\n", ret);
		return ret;
	}
	return 0;
}

static int xmm7360_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct xmm_dev *xmm = kzalloc(sizeof(struct xmm_dev), GFP_KERNEL);
	int i, ret;
	u32 status;
	struct device *device;

	xmm->pci_dev = dev;
	xmm->dev = &dev->dev;

	if (!xmm) {
		dev_err(&(dev->dev), "kzalloc\n");
		return -ENOMEM;
	}

	ret = pci_enable_device(dev);
	if (ret) {
		dev_err(&(dev->dev), "pci_enable_device\n");
		goto fail;
	}
	pci_set_master(dev);

	ret = pci_request_region(dev, 0, "xmm0");
	if (ret) {
		dev_err(&(dev->dev), "pci_request_region(0)\n");
		goto fail;
	}
	xmm->bar0 = pci_iomap(dev, 0, pci_resource_len(dev, 0));

	ret = pci_request_region(dev, 2, "xmm2");
	if (ret) {
		dev_err(&(dev->dev), "pci_request_region(2)\n");
		goto fail;
	}
	xmm->bar2 = pci_iomap(dev, 2, pci_resource_len(dev, 2));

	ret = pci_alloc_irq_vectors(dev, 4, 4, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&(dev->dev), "pci_alloc_irq_vectors\n");
		goto fail;
	}

	for (i=0; i<4; i++) {
		xmm->irq[i] = pci_irq_vector(dev, i);
		ret = request_irq(xmm->irq[i], xmm7360_irq_handlers[i], 0, "xmm7360", xmm);
		if (ret) {
			dev_err(&(dev->dev), "request_irq\n");
			goto fail;
		}
	}
	init_waitqueue_head(&xmm->wq);

	pci_set_drvdata(dev, xmm);

	// Wait for modem core to boot if it's still coming up.
	// Typically ~5 seconds
	for (i=0; i<100; i++) {
		status = xmm->bar2[0];
		if (status == 0x600df00d)
			break;
		if (status == 0xbadc0ded) {
			dev_err(xmm->dev, "Modem is in crash dump state, aborting probe\n");
			ret = -EINVAL;
			goto fail;
		}
		mdelay(200);
	}

	if (status != 0x600df00d) {
		dev_err(xmm->dev, "Unknown modem status: 0x%08x\n", status);
		ret = -EINVAL;
		goto fail;
	}

	pci_set_dma_mask(dev, 0xffffffffffffffff);
	if (ret) {
		dev_err(xmm->dev, "Cannot set DMA mask\n");
		goto fail;
	}
	dma_set_coherent_mask(xmm->dev, 0xffffffffffffffff);

	ret = xmm7360_cmd_ring_init(xmm);
	if (ret) {
		dev_err(xmm->dev, "Could not bring up command ring\n");
		goto fail;
	}

	for (i=0; i<8; i++) {
		ret = xmm7360_create_cdev(xmm, i);
		if (ret)
			goto fail;
	}

	return ret;

fail:
	xmm7360_remove(dev);
	return ret;
}

static struct pci_driver xmm7360_driver = {
	.name		= "xmm7360",
	.id_table	= xmm7360_ids,
	.probe		= xmm7360_probe,
	.remove		= xmm7360_remove,
};

static int xmm7360_init(void)
{
	int ret;
	ret = alloc_chrdev_region(&xmm_base, 0, 8, "xmm");
	if (ret)
		return ret;

	ret = pci_register_driver(&xmm7360_driver);
	if (ret)
		return ret;

	return 0;
}

static void xmm7360_exit(void)
{
	pr_err("xmm7360_exit\n");
	unregister_chrdev_region(xmm_base, 8);
	pci_unregister_driver(&xmm7360_driver);
}

module_init(xmm7360_init);
module_exit(xmm7360_exit);
