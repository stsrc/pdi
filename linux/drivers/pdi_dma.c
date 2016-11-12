//TODO dma syncro??
//TODO netdev, dev, pdev - think/read about it.
//TODO spinlocks, semaphores, race conditions etc.
//TODO more then one card!!!
//TODO DETECTION OF ALREADY USED DEVICE, ETC!


#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/printk.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/slab.h> 
#include <linux/device.h>
#include <linux/err.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <linux/spinlock.h>

#include <linux/dmapool.h>

#define DRIVER_NAME "pdi"
/* 
 * WARNING ABOUT MAC ADDRESS! Its least significant bit says about 
 * unicast or multicast type of transmisson! If there LSB is 1, then
 * there is a multicast transmisson and TCP will fail!
 */
#define DEVICE_MAC_BYTE 0xa0

#define __DEBUG__

#ifdef __DEBUG__
#define debug_print(x) pr_info(x)
#else
#define debug_print(x)
#endif

MODULE_LICENSE("GPL");

#define PCKT_SIZE 2048


/*
 * reg0 - packet's size in bytes. To push packet into MAC write
 * packet bytes count into it.
 * reg1 - packet's word. Write to this register data which you want to
 *        transmit.
 * reg2 - control register.
 *        1 LSB bit - reset bit.
 *                    Write '1' to reset xgbe part of design.
 *                    After write delay execution for 1 ms, to ensure that
 *		      reset ended.
 *        2 LSB bit - data reception enable bit. 
 *                    Write '1' to enable data reception.
 * reg3 - Counter of not read packets yet. Each read zeroes this reg.
 */

struct ring_info {
	struct sk_buff *skb;
	dma_addr_t map;
};

struct dma_desc {
	dma_addr_t cnt;
	dma_addr_t addr; 
} __attribute__((packed));

struct dma_ring {
	struct ring_info *buffer;

	struct dma_desc *desc;
	dma_addr_t desc_p;

	u32 desc_cur;
	u32 desc_cons;
	const u32 desc_max;
};

struct pdi {
	struct device *dev;
	struct net_device *netdev;
	struct napi_struct napi;
	struct resource *ports;
	struct resource *iomem;
	int irq;

	void __iomem *reg0;
	void __iomem *reg1;
	void __iomem *reg2;
	void __iomem *reg3;
	void __iomem *reg4;
	void __iomem *reg5;
	void __iomem *reg6;
	void __iomem *reg7;

	struct dma_ring tx_ring;
	struct dma_ring rx_ring;
};

/*
 * Function pushes packet into the FPGA.
 */
static netdev_tx_t pdi_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	uint32_t cnt = 0;
	struct dma_ring *tx_ring = NULL;
	struct pdi *pdi = (struct pdi *)netdev_priv(dev);

	tx_ring = &pdi->tx_ring;

	if ((tx_ring->desc_cur + 1) % tx_ring->desc_max == 
	    tx_ring->desc_cons) {
		debug_print("pdi: tx_ring full! Packet will be droped.\n");
		return NETDEV_TX_BUSY;	
	}

	cnt = tx_ring->desc_cur;

	tx_ring->buffer[cnt].skb = skb; 

	tx_ring->buffer[cnt].map = dma_map_single(pdi->dev, skb->data, skb->len, 
						  DMA_TO_DEVICE);

	if (dma_mapping_error(pdi->dev, tx_ring->buffer[cnt].map)) {
		debug_print("pdi: dma_map_single failed!\n");
		return NETDEV_TX_BUSY;
	}

	tx_ring->desc[cnt].addr = tx_ring->buffer[cnt].map;
	tx_ring->desc[cnt].cnt = skb->len;


	dma_sync_single_for_device(pdi->dev, tx_ring->buffer[cnt].map, 
				   skb->len, DMA_TO_DEVICE);
	
	tx_ring->desc_cur = (tx_ring->desc_cur + 1) % tx_ring->desc_max;

	wmb();
	//iowrite32(0xFFFFFFFF, pdi->reg7);	
	wmb();

	netdev_sent_queue(dev, skb->len);
	return NETDEV_TX_OK;
}

static int pdi_complete_xmit(struct pdi *pdi)
{
	struct sk_buff *skb = NULL;
	struct dma_ring *tx_ring = &pdi->tx_ring;
	u32 packets = 0, bytes = 0;
	u32 i = 0;
	u32 processed = 0;
	u32 cons = 0;

	cons = tx_ring->desc_cons;
	processed = ioread32(pdi->reg6);
	rmb();	
	if (processed == 0)
		return 0;

	processed /= sizeof(struct dma_desc);
	
	if (tx_ring->desc_cons < tx_ring->desc_cur) {
		if (processed > tx_ring->desc_cur - tx_ring->desc_cons) {
			debug_print("PDI_COMPLETE_XMIT FAILED REALLY HARD!\n");
		}
	} else {
		if (processed > tx_ring->desc_cur + tx_ring->desc_max - tx_ring->desc_cons) {
			debug_print("PDI_COMPLETE_XMIT FAILED REALLY HARD!\n");
		}
	}

	for (i = cons; i != (cons + processed) % tx_ring->desc_max; 
	     i = (i + 1) % tx_ring->desc_max) {
		skb = tx_ring->buffer[i].skb;
		dma_unmap_single(pdi->dev, tx_ring->buffer[i].map, skb->len,
			 	 DMA_TO_DEVICE);
		bytes += skb->len;
		packets++;
		dev_kfree_skb(skb);
	}

	netdev_completed_queue(pdi->netdev, packets, bytes);
	tx_ring->desc_cons = i;
	return 0;
}


static int pdi_open(struct net_device *dev)
{
	struct pdi *pdi = netdev_priv(dev);
	napi_enable(&pdi->napi);
	netif_start_queue(dev);
	netif_carrier_on(dev);
	return 0;
}

static int pdi_close(struct net_device *dev)
{
	struct pdi *pdi = netdev_priv(dev);
	netif_carrier_off(dev);
	netif_stop_queue(dev);
	napi_disable(&pdi->napi);
	return 0;
}

static const struct net_device_ops pdi_netdev_ops = {
	.ndo_open	= pdi_open,
	.ndo_stop	= pdi_close,
	.ndo_start_xmit = pdi_start_xmit,
};

static irqreturn_t pdi_int_handler(int irq, void *data)
{
	struct pdi *pdi = (struct pdi *)data;

	/* Turn off interrupts, allow RX and DMA */
	iowrite32(cpu_to_le32(1 << 1 | 1 << 3), pdi->reg2);
	wmb();
	napi_schedule(&pdi->napi);
	
	return IRQ_HANDLED;
}

static int pdi_init_irq(struct platform_device *pdev)
{
	int rt;
	struct pdi *pdi = pdev->dev.platform_data;

	pdi->irq = platform_get_irq(pdev, 0);
	if (pdi->irq <= 0) {
		debug_print("pdi: platform_get_irq failed.\n");
		return -ENXIO;
	}

	rt = request_irq(pdi->irq, pdi_int_handler, IRQF_SHARED, DRIVER_NAME, 
			 pdi);
	if (rt) {
		debug_print("pdi: request_irq failed.\n");
		pdi->irq = -1;	
		return -ENXIO;
	}

	return 0;
}

static void pdi_deinit_irq(struct platform_device *pdev)
{
	struct pdi *pdi = pdev->dev.platform_data;
	free_irq(pdi->irq, pdi);
}

static int pdi_rx(struct pdi *pdi)
{
	u32 packets_cnt = 0;
	u32 i = 0;
	unsigned int data_len = 0;
	struct sk_buff *skb = NULL;
	unsigned char *buf = NULL;
	struct dma_ring *rx_ring = &pdi->rx_ring;
	int ret = 0;
 	
	/*
	 * TODO: here I only flush the registers.
	 * I should deactivate not_read_packets_counter
	 * from xgbe when DMA RX is on.
	 */
	
	ioread32(pdi->reg3);
	packets_cnt = ioread32(pdi->reg4);
	
	if (!packets_cnt)
		return 0;		

	packets_cnt /= 8;

	pr_info("pdi_rx: packet_cnt = %d\n", packets_cnt);
	for (int i = 0; i < packets_cnt; i++) {
		pr_info("pdi_rx: cnt = %d\n", rx_ring->desc[rx_ring->desc_cons].cnt);
		rx_ring->desc_cons = (rx_ring->desc_cons + 1) % rx_ring->desc_max;				
	}
	return 0;

	for (i = rx_ring->desc_cons; i < (rx_ring->desc_cons + packets_cnt) %
		rx_ring->desc_max; i = (i + 1) % rx_ring->desc_max) {
		dma_sync_single_for_cpu(pdi->dev, rx_ring->buffer[i].map, 
				   PCKT_SIZE, DMA_FROM_DEVICE);

		data_len = rx_ring->desc[i].cnt;
		pr_info("pdi_rx: data_len = %d\n", data_len);
		skb = dev_alloc_skb(data_len + NET_IP_ALIGN);
		if (!skb) {
			debug_print("alloc_skb failed! Packet not received! "
				"FPGA IN ERROR STATE\n");
			pdi->netdev->stats.rx_errors++;
			pdi->netdev->stats.rx_dropped++;
			rx_ring->desc_cons = i; 
			return 0;
		}

		skb_reserve(skb, NET_IP_ALIGN);

		buf = skb_put(skb, data_len);
		skb->dev = pdi->netdev; 

		memcpy(buf, (char *)rx_ring->buffer[i].skb, data_len);

		skb->protocol = eth_type_trans(skb, pdi->netdev); 
		skb_checksum_none_assert(skb);	
		ret = netif_receive_skb(skb);
		pdi->netdev->stats.rx_bytes += (unsigned long)data_len;


	}
	rx_ring->desc_cons = i;
	pdi->netdev->stats.rx_packets += (unsigned long)packets_cnt;
	return packets_cnt;
}

static int pdi_poll(struct napi_struct *napi, int budget)
{
	int packets_cnt;
	struct pdi *pdi = container_of(napi, struct pdi, napi);
	packets_cnt = pdi_rx(pdi);
	pdi_complete_xmit(pdi);
	napi_complete(&pdi->napi);

	if (budget > packets_cnt) {
		/* Enable interrupt, data reception and dma. */
		iowrite32(cpu_to_le32((1 << 1) | (1 << 2) | (1 << 3)), pdi->reg2);
	}
	return packets_cnt;
}

static int pdi_init_dma_ring(struct pdi *pdi, struct dma_ring *ring)
{
	const size_t desc_size = sizeof(struct dma_desc);
	

	ring->desc = dma_zalloc_coherent(pdi->dev, ring->desc_max * 
				desc_size, &ring->desc_p, GFP_KERNEL | 
				GFP_DMA);

	if (!ring->desc) 
		return -ENOMEM;
	

	ring->buffer = kzalloc(sizeof(struct ring_info) * ring->desc_max,
				 GFP_KERNEL);

	if (!ring->buffer) {
		dma_free_coherent(pdi->dev, desc_size * ring->desc_max, 
			  ring->desc, ring->desc_p);
		return -ENOMEM;		
	} 

	return 0;
}


static void pdi_deinit_dma_ring(struct pdi *pdi, struct dma_ring *ring)
{
	const size_t desc_size = sizeof(struct dma_desc);

	kfree(ring->buffer);	

	dma_free_coherent(pdi->dev, desc_size * ring->desc_max, 
			  ring->desc, ring->desc_p);
}

static void pdi_set_dma(struct pdi *pdi)
{
	/* Write where is TX ring located in physical memory. */
	iowrite32(pdi->tx_ring.desc_p, pdi->reg4);
	/* Write where is RX ring located in physical memory. */
	iowrite32(pdi->rx_ring.desc_p, pdi->reg6);
	/* Write byte size of TX and RX ring. */
	iowrite32(pdi->tx_ring.desc_max * sizeof(struct dma_desc), pdi->reg5); 
	wmb();
}

static int pdi_alloc_rx_skb(struct pdi *pdi, int dest)
{
	struct dma_ring *ring = &pdi->rx_ring;
	struct ring_info *ri = &ring->buffer[dest];

	char *buf;
	dma_addr_t mapping;

	buf = kzalloc(PCKT_SIZE, GFP_KERNEL | GFP_DMA);
	if (!buf)
		return -ENOMEM;

	mapping = dma_map_single(pdi->dev, buf, PCKT_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(pdi->dev, mapping)) {
		debug_print("pdi: mapping error!\n");
		kfree(buf);
		return -ENOMEM;
	} 
	
	ri->skb = (struct sk_buff *)buf;
	ri->map = mapping;
	ring->desc[dest].addr = 0;	
	return 0;	
}

static void pdi_free_rx_skb(struct pdi *pdi, int dest)
{
	struct dma_ring *ring = &pdi->rx_ring;
	struct ring_info *ri = &ring->buffer[dest];

	dma_unmap_single(pdi->dev, ri->map, PCKT_SIZE, DMA_FROM_DEVICE); 
	kfree((char *)ri->skb);
	ri->skb = NULL;
	ring->desc[dest].addr = 0;
}

static int pdi_init_dma_rx_ring_info(struct pdi *pdi)
{
	struct dma_ring *ring = &pdi->rx_ring;
	int ret;
	
	for (int i = 0; i < ring->desc_max; i++) {
		ret = pdi_alloc_rx_skb(pdi, i);	 
		if (ret) {
			for (int j = 0; j < i; j++)
				pdi_free_rx_skb(pdi, j);
			return ret;
		}	
	}
	return 0;
}

static void pdi_deinit_dma_rx_ring_info(struct pdi *pdi)
{
	struct dma_ring *ring = &pdi->rx_ring;
	struct ring_info *ri;

	for (int i = 0; i < ring->desc_max; i++) {
		ri = &ring->buffer[i];
		if (ri->skb != NULL) {
			pdi_free_rx_skb(pdi, i);
		}
	}
}

static void pdi_deinit_dma_tx_ring_info(struct pdi *pdi)
{
	/* UNMAP MAPPED PACKETS! */
}

static int pdi_init_dma_rings(struct platform_device *pdev)
{
	struct pdi *pdi = pdev->dev.platform_data;
	int ret;

	ret = pdi_init_dma_ring(pdi, &pdi->tx_ring);
	if (ret)
		return ret;

	ret = pdi_init_dma_ring(pdi, &pdi->rx_ring);
	if (ret) {
		pdi_deinit_dma_ring(pdi, &pdi->tx_ring);
		return ret;
	}	

	ret = pdi_init_dma_rx_ring_info(pdi);
	if (ret) {
		/* 
		 * TODO: change pdi_(de)init_dma_ring functions parameters
		 * 	 into something smarter.
		 */
		pdi_deinit_dma_ring(pdi, &pdi->tx_ring);
		pdi_deinit_dma_ring(pdi, &pdi->rx_ring);
		return ret;
	}

	return 0;
}

static void pdi_deinit_dma_rings(struct platform_device *pdev)
{
	struct pdi *pdi = pdev->dev.platform_data;

	pdi_deinit_dma_rx_ring_info(pdi);
	pdi_deinit_dma_tx_ring_info(pdi);
	pdi_deinit_dma_ring(pdi, &pdi->rx_ring);
	pdi_deinit_dma_ring(pdi, &pdi->tx_ring);
}

/*
 * Function maps registers into memory.
 */
static int pdi_init_registers(struct platform_device *pdev) 
{
	int rt;
	struct pdi *pdi = pdev->dev.platform_data;

	if (!pdi)
		return -EINVAL;

	pdi->iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!pdi->iomem) {
		rt = -ENXIO;
		debug_print("pdi: platform_get_resource failed.\n");
		goto err0;
	}

	pdi->ports = request_mem_region(pdi->iomem->start, pdi->iomem->end -
					pdi->iomem->start, DRIVER_NAME);
	if (!pdi->ports) {
		rt = -ENOMEM;
		debug_print("pdi: request_mem_region failed.\n");
		goto err0;
	}

	pdi->reg0 = ioremap(pdi->iomem->start, 4);
	if (!pdi->reg0)
		goto err1;

	pdi->reg1 = ioremap(pdi->iomem->start + 4, 4);
	if (!pdi->reg1) 
		goto err2;

	pdi->reg2 = ioremap(pdi->iomem->start + 8, 4);
	if (!pdi->reg2)
		goto err3;

	pdi->reg3 = ioremap(pdi->iomem->start + 12, 4);
	if (!pdi->reg3)
		goto err4;

	pdi->reg4 = ioremap(pdi->iomem->start + 16, 4);
	if (!pdi->reg4)
		goto err5;

	pdi->reg5 = ioremap(pdi->iomem->start + 20, 4);
	if (!pdi->reg5) 
		goto err6;

	pdi->reg6 = ioremap(pdi->iomem->start + 24, 4);
	if (!pdi->reg6)
		goto err7;

	pdi->reg7 = ioremap(pdi->iomem->start + 28, 4);
	if (!pdi->reg7)
		goto err8;

	return 0;


err8 :
	iounmap(pdi->reg6);
	pdi->reg4 = NULL;
err7 :
	iounmap(pdi->reg5);
	pdi->reg5 = NULL;
err6 :
	iounmap(pdi->reg4);
	pdi->reg4 = NULL;
err5 :
	iounmap(pdi->reg3);
	pdi->reg3 = NULL;
err4 :
	iounmap(pdi->reg2);
	pdi->reg2 = NULL;
err3 :
	iounmap(pdi->reg1);
	pdi->reg1 = NULL;
err2 :
	iounmap(pdi->reg0);
	pdi->reg0 = NULL;
err1 :
	debug_print("pdi_init_registers failed.\n");
	release_mem_region(pdi->iomem->start, pdi->iomem->end - 
			   pdi->iomem->start);
	pdi->iomem = NULL;
	rt = -ENOMEM;
err0 :
	debug_print("pdi_init_registers failed.\n");
	return rt;
}

static void pdi_deinit_registers(struct pdi *pdi)
{	
	iounmap(pdi->reg7);
	iounmap(pdi->reg6);
	iounmap(pdi->reg5);
	iounmap(pdi->reg4);
	iounmap(pdi->reg3);
	iounmap(pdi->reg2);
	iounmap(pdi->reg1);
	iounmap(pdi->reg0);
	release_mem_region(pdi->iomem->start, pdi->iomem->end - 
			   pdi->iomem->start);
} 


/*
 * Function sets ethernet interface.
 */
static int pdi_init_ethernet(struct platform_device *pdev)
{	
	int rt;
	struct pdi *pdi = pdev->dev.platform_data;
	
	pdi->netdev->irq = pdi->irq;
	pdi->netdev->base_addr = pdi->iomem->start;
	memset(pdi->netdev->dev_addr, DEVICE_MAC_BYTE, ETH_ALEN);
	pdi->netdev->netdev_ops = &pdi_netdev_ops;

	netif_napi_add(pdi->netdev, &pdi->napi, pdi_poll, 64);

	rt = register_netdev(pdi->netdev);
	
	if (rt) {
		return -EINVAL;
	}
	
	return 0;
}

static void pdi_set_pdi(struct pdi *pdi, struct platform_device *pdev,
			struct net_device *netdev)
{
	memset(pdi, 0, sizeof(struct pdi));
	*(u32 *)&pdi->tx_ring.desc_max = 64;
	*(u32 *)&pdi->rx_ring.desc_max = 64;
	pdi->netdev = netdev;
	pdev->dev.platform_data = pdi;
	pdi->dev = &pdev->dev;
}

static void pdi_reset_xgbe(struct pdi *pdi)
{
	iowrite32(cpu_to_le32(1), pdi->reg2);
	wmb();
	mdelay(1);
}

static int pdi_probe(struct platform_device *pdev)
{	
	int rt;
	struct pdi *pdi;
	struct net_device *netdev; 

	if (pdev->dev.platform_data) {
		return -EINVAL;
	}

	netdev = alloc_etherdev(sizeof(struct pdi));
	if (!netdev) {
		debug_print("pdi: alloc_etherdev failed.\n");
		return -ENOMEM;
	}
	debug_print("pdi: init 1\n");
	pdi = netdev_priv(netdev);
	if (!pdi) {
		debug_print("pdi: netdev_priv failed.\n");
		rt = -ENOMEM;
		goto err0;
	}

	pdi_set_pdi(pdi, pdev, netdev);
	debug_print("pdi: init 2\n");

	rt = pdi_init_registers(pdev);
	if (rt) {
		debug_print("pdi: pdi_init_registers failed.\n");
		rt = -EINVAL;
		goto err0;
	}
	debug_print("pdi: init 3\n");

	rt = pdi_init_dma_rings(pdev);
	if (rt) {
		debug_print("pdi: pdi_init_dma_rings failed.\n");
		rt = -EINVAL;
		goto err1;
	}
	debug_print("pdi: init 3\n");

	pdi_reset_xgbe(pdi);
	pdi_set_dma(pdi);

	rt = pdi_init_irq(pdev);
	if (rt) {
		debug_print("pdi: pdi_init_irq failed.\n");
		rt = -EINVAL;
		goto err2;
	}
	debug_print("pdi: init 4\n");

	rt = pdi_init_ethernet(pdev);
	debug_print("pdi: init 5\n");
	if (rt) {
		debug_print("pdi: pdi_init_ethernet failed.\n");
		goto err3;
	}
	wmb();
	/* Enable data reception, interrupts and DMA. */
	iowrite32(1 << 1 | 1 << 2 | 1 << 3, pdi->reg2); 
	wmb();
	debug_print("pdi: init 6\n");
	return 0;
err3:
	pdi_deinit_irq(pdev);
err2:
	pdi_deinit_dma_rings(pdev);
err1:
	pdi_deinit_registers(pdi);
err0:
	free_netdev(netdev);
	return rt;
}

static int pdi_remove(struct platform_device *pdev)
{
	struct pdi *pdi = pdev->dev.platform_data;
	struct net_device *netdev;	
	struct napi_struct *napi;
	if (!pdi) {
		debug_print("pdi: pdi_removed can't get struct pdi *pdi!\n");
		return -EINVAL;
	}

	netdev = pdi->netdev;
	napi = &pdi->napi;

	/*Disabling data reception, DMA and interrupts on FPGA */
	iowrite32(0, pdi->reg2);
	wmb();
	debug_print("pdi: 1\n");

	free_irq(pdi->irq, pdi);
	debug_print("pdi: 2\n");

	pdi_deinit_dma_rings(pdev);
	debug_print("pdi: 3\n");

	pdi_deinit_registers(pdi);
	debug_print("pdi: 4\n");

	unregister_netdev(netdev);
	debug_print("pdi: 5\n");

	netif_napi_del(napi);
	debug_print("pdi: 6\n");

	free_netdev(netdev);
	debug_print("pdi: 7\n");

	pdev->dev.platform_data = NULL;

	return 0;
}

static const struct of_device_id pdi_of_match[] = {
	{ .compatible = "xlnx,xgbe-pcs-pma-1.0", },
	{}
};

static struct platform_driver pdi_platform_driver = {
	.probe = pdi_probe,
	.remove = pdi_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(pdi_of_match),
	},
};

MODULE_DEVICE_TABLE(of, pdi_of_match);
MODULE_ALIAS("platform:pdi");

static int __init pdi_init(void)
{
	int rt;
	
	rt = platform_driver_register(&pdi_platform_driver);
	if (rt) 
		debug_print("pdi: platform_driver_register failed.\n");

	return rt;
}

static void __exit pdi_exit(void)
{
	platform_driver_unregister(&pdi_platform_driver);
}

module_init(pdi_init);
module_exit(pdi_exit);
