// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2019 Pensando Systems, Inc */

#include <linux/printk.h>
#include <linux/dynamic_debug.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/cpumask.h>

#include "ionic.h"
#include "ionic_bus.h"
#include "ionic_lif.h"
#include "ionic_txrx.h"
#include "ionic_ethtool.h"
#include "ionic_debugfs.h"

/* queuetype support level */
static const u8 ionic_qtype_versions[IONIC_QTYPE_MAX] = {
	[IONIC_QTYPE_ADMINQ]  = 0,   /* 0 = Base version with CQ support */
	[IONIC_QTYPE_NOTIFYQ] = 0,   /* 0 = Base version */
	[IONIC_QTYPE_RXQ]     = 0,   /* 0 = Base version with CQ+SG support */
	[IONIC_QTYPE_TXQ]     = 1,   /* 0 = Base version with CQ+SG support
				      * 1 =   ... with Tx SG version 1
				      */
};

static void ionic_lif_rx_mode(struct ionic_lif *lif, unsigned int rx_mode);
static int ionic_lif_addr_add(struct ionic_lif *lif, const u8 *addr);
static int ionic_lif_addr_del(struct ionic_lif *lif, const u8 *addr);
static void ionic_link_status_check(struct ionic_lif *lif);
static void ionic_lif_handle_fw_down(struct ionic_lif *lif);
static void ionic_lif_handle_fw_up(struct ionic_lif *lif);
static void ionic_lif_set_netdev_info(struct ionic_lif *lif);

static int ionic_start_queues(struct ionic_lif *lif);
static void ionic_stop_queues(struct ionic_lif *lif);
static void ionic_lif_queue_identify(struct ionic_lif *lif);

static void ionic_lif_deferred_work(struct work_struct *work)
{
	struct ionic_lif *lif = container_of(work, struct ionic_lif, deferred.work);
	struct ionic_deferred *def = &lif->deferred;
	struct ionic_deferred_work *w = NULL;

	spin_lock_bh(&def->lock);
	if (!list_empty(&def->list)) {
		w = list_first_entry(&def->list,
				     struct ionic_deferred_work, list);
		list_del(&w->list);
	}
	spin_unlock_bh(&def->lock);

	if (w) {
		switch (w->type) {
		case IONIC_DW_TYPE_RX_MODE:
			ionic_lif_rx_mode(lif, w->rx_mode);
			break;
		case IONIC_DW_TYPE_RX_ADDR_ADD:
			ionic_lif_addr_add(lif, w->addr);
			break;
		case IONIC_DW_TYPE_RX_ADDR_DEL:
			ionic_lif_addr_del(lif, w->addr);
			break;
		case IONIC_DW_TYPE_LINK_STATUS:
			ionic_link_status_check(lif);
			break;
		case IONIC_DW_TYPE_LIF_RESET:
			if (w->fw_status)
				ionic_lif_handle_fw_up(lif);
			else
				ionic_lif_handle_fw_down(lif);
			break;
		default:
			break;
		}
		kfree(w);
		schedule_work(&def->work);
	}
}

void ionic_lif_deferred_enqueue(struct ionic_deferred *def,
				struct ionic_deferred_work *work)
{
	spin_lock_bh(&def->lock);
	list_add_tail(&work->list, &def->list);
	spin_unlock_bh(&def->lock);
	schedule_work(&def->work);
}

static void ionic_link_status_check(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	u16 link_status;
	bool link_up;

	if (!test_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state))
		return;

	link_status = le16_to_cpu(lif->info->status.link_status);
	link_up = link_status == IONIC_PORT_OPER_STATUS_UP;

	if (link_up) {
		if (!netif_carrier_ok(netdev)) {
			u32 link_speed;

			ionic_port_identify(lif->ionic);
			link_speed = le32_to_cpu(lif->info->status.link_speed);
			netdev_info(netdev, "Link up - %d Gbps\n",
				    link_speed / 1000);
			netif_carrier_on(netdev);
		}

		if (lif->netdev->flags & IFF_UP && netif_running(lif->netdev)) {
			mutex_lock(&lif->queue_lock);
			ionic_start_queues(lif);
			mutex_unlock(&lif->queue_lock);
		}
	} else {
		if (netif_carrier_ok(netdev)) {
			netdev_info(netdev, "Link down\n");
			netif_carrier_off(netdev);
		}

		if (lif->netdev->flags & IFF_UP && netif_running(lif->netdev)) {
			mutex_lock(&lif->queue_lock);
			ionic_stop_queues(lif);
			mutex_unlock(&lif->queue_lock);
		}
	}

	clear_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state);
}

void ionic_link_status_check_request(struct ionic_lif *lif)
{
	struct ionic_deferred_work *work;

	/* we only need one request outstanding at a time */
	if (test_and_set_bit(IONIC_LIF_F_LINK_CHECK_REQUESTED, lif->state))
		return;

	if (in_interrupt()) {
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (!work)
			return;

		work->type = IONIC_DW_TYPE_LINK_STATUS;
		ionic_lif_deferred_enqueue(&lif->deferred, work);
	} else {
		ionic_link_status_check(lif);
	}
}

static irqreturn_t ionic_isr(int irq, void *data)
{
	struct napi_struct *napi = data;

	napi_schedule_irqoff(napi);

	return IRQ_HANDLED;
}

static int ionic_request_irq(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct ionic_intr_info *intr = &qcq->intr;
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	const char *name;

	if (lif->registered)
		name = lif->netdev->name;
	else
		name = dev_name(dev);

	snprintf(intr->name, sizeof(intr->name),
		 "%s-%s-%s", IONIC_DRV_NAME, name, q->name);

	return devm_request_irq(dev, intr->vector, ionic_isr,
				0, intr->name, &qcq->napi);
}

static int ionic_intr_alloc(struct ionic_lif *lif, struct ionic_intr_info *intr)
{
	struct ionic *ionic = lif->ionic;
	int index;

	index = find_first_zero_bit(ionic->intrs, ionic->nintrs);
	if (index == ionic->nintrs) {
		netdev_warn(lif->netdev, "%s: no intr, index=%d nintrs=%d\n",
			    __func__, index, ionic->nintrs);
		return -ENOSPC;
	}

	set_bit(index, ionic->intrs);
	ionic_intr_init(&ionic->idev, intr, index);

	return 0;
}

static void ionic_intr_free(struct ionic *ionic, int index)
{
	if (index != IONIC_INTR_INDEX_NOT_ASSIGNED && index < ionic->nintrs)
		clear_bit(index, ionic->intrs);
}

static int ionic_qcq_enable(struct ionic_qcq *qcq)
{
	struct ionic_queue *q = &qcq->q;
	struct ionic_lif *lif = q->lif;
	struct ionic_dev *idev;
	struct device *dev;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_control = {
			.opcode = IONIC_CMD_Q_CONTROL,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.index = cpu_to_le32(q->index),
			.oper = IONIC_Q_ENABLE,
		},
	};

	idev = &lif->ionic->idev;
	dev = lif->ionic->dev;

	dev_dbg(dev, "q_enable.index %d q_enable.qtype %d\n",
		ctx.cmd.q_control.index, ctx.cmd.q_control.type);

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		irq_set_affinity_hint(qcq->intr.vector,
				      &qcq->intr.affinity_mask);
		napi_enable(&qcq->napi);
		ionic_intr_clean(idev->intr_ctrl, qcq->intr.index);
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_CLEAR);
	}

	return ionic_adminq_post_wait(lif, &ctx);
}

static int ionic_qcq_disable(struct ionic_qcq *qcq)
{
	struct ionic_queue *q = &qcq->q;
	struct ionic_lif *lif = q->lif;
	struct ionic_dev *idev;
	struct device *dev;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_control = {
			.opcode = IONIC_CMD_Q_CONTROL,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.index = cpu_to_le32(q->index),
			.oper = IONIC_Q_DISABLE,
		},
	};

	idev = &lif->ionic->idev;
	dev = lif->ionic->dev;

	dev_dbg(dev, "q_disable.index %d q_disable.qtype %d\n",
		ctx.cmd.q_control.index, ctx.cmd.q_control.type);

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_SET);
		synchronize_irq(qcq->intr.vector);
		irq_set_affinity_hint(qcq->intr.vector, NULL);
		napi_disable(&qcq->napi);
	}

	return ionic_adminq_post_wait(lif, &ctx);
}

static void ionic_lif_qcq_deinit(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct ionic_dev *idev = &lif->ionic->idev;

	if (!qcq)
		return;

	if (!(qcq->flags & IONIC_QCQ_F_INITED))
		return;

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_SET);
		netif_napi_del(&qcq->napi);
	}

	qcq->flags &= ~IONIC_QCQ_F_INITED;
}

static void ionic_qcq_free(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;

	if (!qcq)
		return;

	ionic_debugfs_del_qcq(qcq);

	dma_free_coherent(dev, qcq->total_size, qcq->base, qcq->base_pa);
	qcq->base = NULL;
	qcq->base_pa = 0;

	if (qcq->flags & IONIC_QCQ_F_INTR) {
		irq_set_affinity_hint(qcq->intr.vector, NULL);
		devm_free_irq(dev, qcq->intr.vector, &qcq->napi);
		qcq->intr.vector = 0;
		ionic_intr_free(lif->ionic, qcq->intr.index);
	}

	devm_kfree(dev, qcq->cq.info);
	qcq->cq.info = NULL;
	devm_kfree(dev, qcq->q.info);
	qcq->q.info = NULL;
	devm_kfree(dev, qcq);
}

static void ionic_qcqs_free(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	unsigned int i;

	if (lif->notifyqcq) {
		ionic_qcq_free(lif, lif->notifyqcq);
		lif->notifyqcq = NULL;
	}

	if (lif->adminqcq) {
		ionic_qcq_free(lif, lif->adminqcq);
		lif->adminqcq = NULL;
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs; i++)
			if (lif->rxqcqs[i].stats)
				devm_kfree(dev, lif->rxqcqs[i].stats);
		devm_kfree(dev, lif->rxqcqs);
		lif->rxqcqs = NULL;
	}

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs; i++)
			if (lif->txqcqs[i].stats)
				devm_kfree(dev, lif->txqcqs[i].stats);
		devm_kfree(dev, lif->txqcqs);
		lif->txqcqs = NULL;
	}
}

static void ionic_link_qcq_interrupts(struct ionic_qcq *src_qcq,
				      struct ionic_qcq *n_qcq)
{
	if (WARN_ON(n_qcq->flags & IONIC_QCQ_F_INTR)) {
		ionic_intr_free(n_qcq->cq.lif->ionic, n_qcq->intr.index);
		n_qcq->flags &= ~IONIC_QCQ_F_INTR;
	}

	n_qcq->intr.vector = src_qcq->intr.vector;
	n_qcq->intr.index = src_qcq->intr.index;
}

static int ionic_qcq_alloc(struct ionic_lif *lif, unsigned int type,
			   unsigned int index,
			   const char *name, unsigned int flags,
			   unsigned int num_descs, unsigned int desc_size,
			   unsigned int cq_desc_size,
			   unsigned int sg_desc_size,
			   unsigned int pid, struct ionic_qcq **qcq)
{
	struct ionic_dev *idev = &lif->ionic->idev;
	u32 q_size, cq_size, sg_size, total_size;
	struct device *dev = lif->ionic->dev;
	void *q_base, *cq_base, *sg_base;
	dma_addr_t cq_base_pa = 0;
	dma_addr_t sg_base_pa = 0;
	dma_addr_t q_base_pa = 0;
	struct ionic_qcq *new;
	int err;

	*qcq = NULL;

	q_size  = num_descs * desc_size;
	cq_size = num_descs * cq_desc_size;
	sg_size = num_descs * sg_desc_size;

	total_size = ALIGN(q_size, PAGE_SIZE) + ALIGN(cq_size, PAGE_SIZE);
	/* Note: aligning q_size/cq_size is not enough due to cq_base
	 * address aligning as q_base could be not aligned to the page.
	 * Adding PAGE_SIZE.
	 */
	total_size += PAGE_SIZE;
	if (flags & IONIC_QCQ_F_SG) {
		total_size += ALIGN(sg_size, PAGE_SIZE);
		total_size += PAGE_SIZE;
	}

	new = devm_kzalloc(dev, sizeof(*new), GFP_KERNEL);
	if (!new) {
		netdev_err(lif->netdev, "Cannot allocate queue structure\n");
		err = -ENOMEM;
		goto err_out;
	}

	new->flags = flags;

	new->q.info = devm_kzalloc(dev, sizeof(*new->q.info) * num_descs,
				   GFP_KERNEL);
	if (!new->q.info) {
		netdev_err(lif->netdev, "Cannot allocate queue info\n");
		err = -ENOMEM;
		goto err_out;
	}

	new->q.type = type;

	err = ionic_q_init(lif, idev, &new->q, index, name, num_descs,
			   desc_size, sg_desc_size, pid);
	if (err) {
		netdev_err(lif->netdev, "Cannot initialize queue\n");
		goto err_out;
	}

	if (flags & IONIC_QCQ_F_INTR) {
		err = ionic_intr_alloc(lif, &new->intr);
		if (err) {
			netdev_warn(lif->netdev, "no intr for %s: %d\n",
				    name, err);
			goto err_out;
		}

		err = ionic_bus_get_irq(lif->ionic, new->intr.index);
		if (err < 0) {
			netdev_warn(lif->netdev, "no vector for %s: %d\n",
				    name, err);
			goto err_out_free_intr;
		}
		new->intr.vector = err;
		ionic_intr_mask_assert(idev->intr_ctrl, new->intr.index,
				       IONIC_INTR_MASK_SET);

		err = ionic_request_irq(lif, new);
		if (err) {
			netdev_warn(lif->netdev, "irq request failed %d\n", err);
			goto err_out_free_intr;
		}

		new->intr.cpu = cpumask_local_spread(new->intr.index,
						     dev_to_node(dev));
		if (new->intr.cpu != -1)
			cpumask_set_cpu(new->intr.cpu,
					&new->intr.affinity_mask);
	} else {
		new->intr.index = IONIC_INTR_INDEX_NOT_ASSIGNED;
	}

	new->cq.info = devm_kzalloc(dev, sizeof(*new->cq.info) * num_descs,
				    GFP_KERNEL);
	if (!new->cq.info) {
		netdev_err(lif->netdev, "Cannot allocate completion queue info\n");
		err = -ENOMEM;
		goto err_out_free_irq;
	}

	err = ionic_cq_init(lif, &new->cq, &new->intr, num_descs, cq_desc_size);
	if (err) {
		netdev_err(lif->netdev, "Cannot initialize completion queue\n");
		goto err_out_free_irq;
	}

	new->base = dma_alloc_coherent(dev, total_size, &new->base_pa,
				       GFP_KERNEL);
	if (!new->base) {
		netdev_err(lif->netdev, "Cannot allocate queue DMA memory\n");
		err = -ENOMEM;
		goto err_out_free_irq;
	}

	new->total_size = total_size;

	q_base = new->base;
	q_base_pa = new->base_pa;

	cq_base = (void *)ALIGN((uintptr_t)q_base + q_size, PAGE_SIZE);
	cq_base_pa = ALIGN(q_base_pa + q_size, PAGE_SIZE);

	if (flags & IONIC_QCQ_F_SG) {
		sg_base = (void *)ALIGN((uintptr_t)cq_base + cq_size,
					PAGE_SIZE);
		sg_base_pa = ALIGN(cq_base_pa + cq_size, PAGE_SIZE);
		ionic_q_sg_map(&new->q, sg_base, sg_base_pa);
	}

	ionic_q_map(&new->q, q_base, q_base_pa);
	ionic_cq_map(&new->cq, cq_base, cq_base_pa);
	ionic_cq_bind(&new->cq, &new->q);

	*qcq = new;

	return 0;

err_out_free_irq:
	if (flags & IONIC_QCQ_F_INTR)
		devm_free_irq(dev, new->intr.vector, &new->napi);
err_out_free_intr:
	if (flags & IONIC_QCQ_F_INTR)
		ionic_intr_free(lif->ionic, new->intr.index);
err_out:
	dev_err(dev, "qcq alloc of %s%d failed %d\n", name, index, err);
	return err;
}

static int ionic_qcqs_alloc(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	unsigned int q_list_size;
	unsigned int flags;
	int err;
	int i;

	flags = IONIC_QCQ_F_INTR;
	err = ionic_qcq_alloc(lif, IONIC_QTYPE_ADMINQ, 0, "admin", flags,
			      IONIC_ADMINQ_LENGTH,
			      sizeof(struct ionic_admin_cmd),
			      sizeof(struct ionic_admin_comp),
			      0, lif->kern_pid, &lif->adminqcq);
	if (err)
		return err;
	ionic_debugfs_add_qcq(lif, lif->adminqcq);

	if (lif->ionic->nnqs_per_lif) {
		flags = IONIC_QCQ_F_NOTIFYQ;
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_NOTIFYQ, 0, "notifyq",
				      flags, IONIC_NOTIFYQ_LENGTH,
				      sizeof(struct ionic_notifyq_cmd),
				      sizeof(union ionic_notifyq_comp),
				      0, lif->kern_pid, &lif->notifyqcq);
		if (err)
			goto err_out_free_adminqcq;
		ionic_debugfs_add_qcq(lif, lif->notifyqcq);

		/* Let the notifyq ride on the adminq interrupt */
		ionic_link_qcq_interrupts(lif->adminqcq, lif->notifyqcq);
	}

	q_list_size = sizeof(*lif->txqcqs) * lif->nxqs;
	err = -ENOMEM;
	lif->txqcqs = devm_kzalloc(dev, q_list_size, GFP_KERNEL);
	if (!lif->txqcqs)
		goto err_out_free_notifyqcq;
	for (i = 0; i < lif->nxqs; i++) {
		lif->txqcqs[i].stats = devm_kzalloc(dev,
						    sizeof(struct ionic_q_stats),
						    GFP_KERNEL);
		if (!lif->txqcqs[i].stats)
			goto err_out_free_tx_stats;
	}

	lif->rxqcqs = devm_kzalloc(dev, q_list_size, GFP_KERNEL);
	if (!lif->rxqcqs)
		goto err_out_free_tx_stats;
	for (i = 0; i < lif->nxqs; i++) {
		lif->rxqcqs[i].stats = devm_kzalloc(dev,
						    sizeof(struct ionic_q_stats),
						    GFP_KERNEL);
		if (!lif->rxqcqs[i].stats)
			goto err_out_free_rx_stats;
	}

	return 0;

err_out_free_rx_stats:
	for (i = 0; i < lif->nxqs; i++)
		if (lif->rxqcqs[i].stats)
			devm_kfree(dev, lif->rxqcqs[i].stats);
	devm_kfree(dev, lif->rxqcqs);
	lif->rxqcqs = NULL;
err_out_free_tx_stats:
	for (i = 0; i < lif->nxqs; i++)
		if (lif->txqcqs[i].stats)
			devm_kfree(dev, lif->txqcqs[i].stats);
	devm_kfree(dev, lif->txqcqs);
	lif->txqcqs = NULL;
err_out_free_notifyqcq:
	if (lif->notifyqcq) {
		ionic_qcq_free(lif, lif->notifyqcq);
		lif->notifyqcq = NULL;
	}
err_out_free_adminqcq:
	ionic_qcq_free(lif, lif->adminqcq);
	lif->adminqcq = NULL;

	return err;
}

static int ionic_lif_txq_init(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	struct ionic_cq *cq = &qcq->cq;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_SG),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
			.cq_ring_base = cpu_to_le64(cq->base_pa),
			.sg_ring_base = cpu_to_le64(q->sg_base_pa),
		},
	};
	unsigned int intr_index;
	int err;

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		intr_index = qcq->intr.index;
	else
		intr_index = lif->rxqcqs[q->index].qcq->intr.index;
	ctx.cmd.q_init.intr_index = cpu_to_le16(intr_index);

	dev_dbg(dev, "txq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "txq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "txq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "txq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);
	dev_dbg(dev, "txq_init.flags 0x%x\n", ctx.cmd.q_init.flags);
	dev_dbg(dev, "txq_init.ver %d\n", ctx.cmd.q_init.ver);
	dev_dbg(dev, "txq_init.intr_index %d\n", ctx.cmd.q_init.intr_index);

	q->tail = q->info;
	q->head = q->tail;
	cq->tail = cq->info;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "txq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "txq->hw_index %d\n", q->hw_index);

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		netif_napi_add(lif->netdev, &qcq->napi, ionic_tx_napi,
			       NAPI_POLL_WEIGHT);

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_lif_rxq_init(struct ionic_lif *lif, struct ionic_qcq *qcq)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	struct ionic_cq *cq = &qcq->cq;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_SG),
			.intr_index = cpu_to_le16(cq->bound_intr->index),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
			.cq_ring_base = cpu_to_le64(cq->base_pa),
			.sg_ring_base = cpu_to_le64(q->sg_base_pa),
		},
	};
	int err;

	dev_dbg(dev, "rxq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "rxq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "rxq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "rxq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);
	dev_dbg(dev, "rxq_init.flags 0x%x\n", ctx.cmd.q_init.flags);
	dev_dbg(dev, "rxq_init.ver %d\n", ctx.cmd.q_init.ver);
	dev_dbg(dev, "rxq_init.intr_index %d\n", ctx.cmd.q_init.intr_index);

	q->tail = q->info;
	q->head = q->tail;
	cq->tail = cq->info;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "rxq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "rxq->hw_index %d\n", q->hw_index);

	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		netif_napi_add(lif->netdev, &qcq->napi, ionic_rx_napi,
			       NAPI_POLL_WEIGHT);
	else
		netif_napi_add(lif->netdev, &qcq->napi, ionic_txrx_napi,
			       NAPI_POLL_WEIGHT);

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static bool ionic_notifyq_service(struct ionic_cq *cq,
				  struct ionic_cq_info *cq_info)
{
	union ionic_notifyq_comp *comp = cq_info->cq_desc;
	struct ionic_deferred_work *work;
	struct net_device *netdev;
	struct ionic_queue *q;
	struct ionic_lif *lif;
	u64 eid;

	q = cq->bound_q;
	lif = q->info[0].cb_arg;
	netdev = lif->netdev;
	eid = le64_to_cpu(comp->event.eid);

	/* Have we run out of new completions to process? */
	if ((s64)(eid - lif->last_eid) <= 0)
		return false;

	lif->last_eid = eid;

	dev_dbg(lif->ionic->dev, "notifyq event:\n");
	dynamic_hex_dump("event ", DUMP_PREFIX_OFFSET, 16, 1,
			 comp, sizeof(*comp), true);

	switch (le16_to_cpu(comp->event.ecode)) {
	case IONIC_EVENT_LINK_CHANGE:
		ionic_link_status_check_request(lif);
		break;
	case IONIC_EVENT_RESET:
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (!work) {
			netdev_err(lif->netdev, "%s OOM\n", __func__);
		} else {
			work->type = IONIC_DW_TYPE_LIF_RESET;
			ionic_lif_deferred_enqueue(&lif->deferred, work);
		}
		break;
	default:
		netdev_warn(netdev, "Notifyq event ecode=%d eid=%lld\n",
			    comp->event.ecode, eid);
		break;
	}

	return true;
}

static int ionic_notifyq_clean(struct ionic_lif *lif, int budget)
{
	struct ionic_dev *idev = &lif->ionic->idev;
	struct ionic_cq *cq = &lif->notifyqcq->cq;
	u32 work_done;

	work_done = ionic_cq_service(cq, budget, ionic_notifyq_service,
				     NULL, NULL);
	if (work_done)
		ionic_intr_credits(idev->intr_ctrl, cq->bound_intr->index,
				   work_done, IONIC_INTR_CRED_RESET_COALESCE);

	return work_done;
}

static bool ionic_adminq_service(struct ionic_cq *cq,
				 struct ionic_cq_info *cq_info)
{
	struct ionic_admin_comp *comp = cq_info->cq_desc;

	if (!color_match(comp->color, cq->done_color))
		return false;

	ionic_q_service(cq->bound_q, cq_info, le16_to_cpu(comp->comp_index));

	return true;
}

static int ionic_adminq_napi(struct napi_struct *napi, int budget)
{
	struct ionic_lif *lif = napi_to_cq(napi)->lif;
	int n_work = 0;
	int a_work = 0;

	if (likely(lif->notifyqcq && lif->notifyqcq->flags & IONIC_QCQ_F_INITED))
		n_work = ionic_notifyq_clean(lif, budget);
	a_work = ionic_napi(napi, budget, ionic_adminq_service, NULL, NULL);

	return max(n_work, a_work);
}

void ionic_get_stats64(struct net_device *netdev,
		       struct rtnl_link_stats64 *ns)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_lif_stats *ls;

	memset(ns, 0, sizeof(*ns));
	ls = &lif->info->stats;

	ns->rx_packets = le64_to_cpu(ls->rx_ucast_packets) +
			 le64_to_cpu(ls->rx_mcast_packets) +
			 le64_to_cpu(ls->rx_bcast_packets);

	ns->tx_packets = le64_to_cpu(ls->tx_ucast_packets) +
			 le64_to_cpu(ls->tx_mcast_packets) +
			 le64_to_cpu(ls->tx_bcast_packets);

	ns->rx_bytes = le64_to_cpu(ls->rx_ucast_bytes) +
		       le64_to_cpu(ls->rx_mcast_bytes) +
		       le64_to_cpu(ls->rx_bcast_bytes);

	ns->tx_bytes = le64_to_cpu(ls->tx_ucast_bytes) +
		       le64_to_cpu(ls->tx_mcast_bytes) +
		       le64_to_cpu(ls->tx_bcast_bytes);

	ns->rx_dropped = le64_to_cpu(ls->rx_ucast_drop_packets) +
			 le64_to_cpu(ls->rx_mcast_drop_packets) +
			 le64_to_cpu(ls->rx_bcast_drop_packets);

	ns->tx_dropped = le64_to_cpu(ls->tx_ucast_drop_packets) +
			 le64_to_cpu(ls->tx_mcast_drop_packets) +
			 le64_to_cpu(ls->tx_bcast_drop_packets);

	ns->multicast = le64_to_cpu(ls->rx_mcast_packets);

	ns->rx_over_errors = le64_to_cpu(ls->rx_queue_empty);

	ns->rx_missed_errors = le64_to_cpu(ls->rx_dma_error) +
			       le64_to_cpu(ls->rx_queue_disabled) +
			       le64_to_cpu(ls->rx_desc_fetch_error) +
			       le64_to_cpu(ls->rx_desc_data_error);

	ns->tx_aborted_errors = le64_to_cpu(ls->tx_dma_error) +
				le64_to_cpu(ls->tx_queue_disabled) +
				le64_to_cpu(ls->tx_desc_fetch_error) +
				le64_to_cpu(ls->tx_desc_data_error);

	ns->rx_errors = ns->rx_over_errors +
			ns->rx_missed_errors;

	ns->tx_errors = ns->tx_aborted_errors;
}

static int ionic_lif_addr_add(struct ionic_lif *lif, const u8 *addr)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_add = {
			.opcode = IONIC_CMD_RX_FILTER_ADD,
			.lif_index = cpu_to_le16(lif->index),
			.match = cpu_to_le16(IONIC_RX_FILTER_MATCH_MAC),
		},
	};
	struct ionic_rx_filter *f;
	int err;

	/* don't bother if we already have it */
	spin_lock_bh(&lif->rx_filters.lock);
	f = ionic_rx_filter_by_addr(lif, addr);
	spin_unlock_bh(&lif->rx_filters.lock);
	if (f)
		return 0;

	netdev_dbg(lif->netdev, "rx_filter add ADDR %pM\n", addr);

	memcpy(ctx.cmd.rx_filter_add.mac.addr, addr, ETH_ALEN);
	err = ionic_adminq_post_wait(lif, &ctx);
	if (err && err != -EEXIST)
		return err;

	return ionic_rx_filter_save(lif, 0, IONIC_RXQ_INDEX_ANY, 0, &ctx);
}

static int ionic_lif_addr_del(struct ionic_lif *lif, const u8 *addr)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_del = {
			.opcode = IONIC_CMD_RX_FILTER_DEL,
			.lif_index = cpu_to_le16(lif->index),
		},
	};
	struct ionic_rx_filter *f;
	int err;

	spin_lock_bh(&lif->rx_filters.lock);
	f = ionic_rx_filter_by_addr(lif, addr);
	if (!f) {
		spin_unlock_bh(&lif->rx_filters.lock);
		return -ENOENT;
	}

	netdev_dbg(lif->netdev, "rx_filter del ADDR %pM (id %d)\n",
		   addr, f->filter_id);

	ctx.cmd.rx_filter_del.filter_id = cpu_to_le32(f->filter_id);
	ionic_rx_filter_free(lif, f);
	spin_unlock_bh(&lif->rx_filters.lock);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err && err != -EEXIST)
		return err;

	return 0;
}

static int ionic_lif_addr(struct ionic_lif *lif, const u8 *addr, bool add)
{
	struct ionic *ionic = lif->ionic;
	struct ionic_deferred_work *work;
	unsigned int nmfilters;
	unsigned int nufilters;

	if (add) {
		/* Do we have space for this filter?  We test the counters
		 * here before checking the need for deferral so that we
		 * can return an overflow error to the stack.
		 */
		nmfilters = le32_to_cpu(ionic->ident.lif.eth.max_mcast_filters);
		nufilters = le32_to_cpu(ionic->ident.lif.eth.max_ucast_filters);

		if ((is_multicast_ether_addr(addr) && lif->nmcast < nmfilters))
			lif->nmcast++;
		else if (!is_multicast_ether_addr(addr) &&
			 lif->nucast < nufilters)
			lif->nucast++;
		else
			return -ENOSPC;
	} else {
		if (is_multicast_ether_addr(addr) && lif->nmcast)
			lif->nmcast--;
		else if (!is_multicast_ether_addr(addr) && lif->nucast)
			lif->nucast--;
	}

	if (in_interrupt()) {
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (!work) {
			netdev_err(lif->netdev, "%s OOM\n", __func__);
			return -ENOMEM;
		}
		work->type = add ? IONIC_DW_TYPE_RX_ADDR_ADD :
				   IONIC_DW_TYPE_RX_ADDR_DEL;
		memcpy(work->addr, addr, ETH_ALEN);
		netdev_dbg(lif->netdev, "deferred: rx_filter %s %pM\n",
			   add ? "add" : "del", addr);
		ionic_lif_deferred_enqueue(&lif->deferred, work);
	} else {
		netdev_dbg(lif->netdev, "rx_filter %s %pM\n",
			   add ? "add" : "del", addr);
		if (add)
			return ionic_lif_addr_add(lif, addr);
		else
			return ionic_lif_addr_del(lif, addr);
	}

	return 0;
}

static int ionic_addr_add(struct net_device *netdev, const u8 *addr)
{
	return ionic_lif_addr(netdev_priv(netdev), addr, true);
}

static int ionic_addr_del(struct net_device *netdev, const u8 *addr)
{
	return ionic_lif_addr(netdev_priv(netdev), addr, false);
}

static void ionic_lif_rx_mode(struct ionic_lif *lif, unsigned int rx_mode)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_mode_set = {
			.opcode = IONIC_CMD_RX_MODE_SET,
			.lif_index = cpu_to_le16(lif->index),
			.rx_mode = cpu_to_le16(rx_mode),
		},
	};
	char buf[128];
	int err;
	int i;
#define REMAIN(__x) (sizeof(buf) - (__x))

	i = scnprintf(buf, sizeof(buf), "rx_mode 0x%04x -> 0x%04x:",
		      lif->rx_mode, rx_mode);
	if (rx_mode & IONIC_RX_MODE_F_UNICAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_UNICAST");
	if (rx_mode & IONIC_RX_MODE_F_MULTICAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_MULTICAST");
	if (rx_mode & IONIC_RX_MODE_F_BROADCAST)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_BROADCAST");
	if (rx_mode & IONIC_RX_MODE_F_PROMISC)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_PROMISC");
	if (rx_mode & IONIC_RX_MODE_F_ALLMULTI)
		i += scnprintf(&buf[i], REMAIN(i), " RX_MODE_F_ALLMULTI");
	netdev_dbg(lif->netdev, "lif%d %s\n", lif->index, buf);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		netdev_warn(lif->netdev, "set rx_mode 0x%04x failed: %d\n",
			    rx_mode, err);
	else
		lif->rx_mode = rx_mode;
}

static void _ionic_lif_rx_mode(struct ionic_lif *lif, unsigned int rx_mode)
{
	struct ionic_deferred_work *work;

	if (in_interrupt()) {
		work = kzalloc(sizeof(*work), GFP_ATOMIC);
		if (!work) {
			netdev_err(lif->netdev, "%s OOM\n", __func__);
			return;
		}
		work->type = IONIC_DW_TYPE_RX_MODE;
		work->rx_mode = rx_mode;
		netdev_dbg(lif->netdev, "deferred: rx_mode\n");
		ionic_lif_deferred_enqueue(&lif->deferred, work);
	} else {
		ionic_lif_rx_mode(lif, rx_mode);
	}
}

static void ionic_set_rx_mode(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_identity *ident;
	unsigned int nfilters;
	unsigned int rx_mode;

	ident = &lif->ionic->ident;

	rx_mode = IONIC_RX_MODE_F_UNICAST;
	rx_mode |= (netdev->flags & IFF_MULTICAST) ? IONIC_RX_MODE_F_MULTICAST : 0;
	rx_mode |= (netdev->flags & IFF_BROADCAST) ? IONIC_RX_MODE_F_BROADCAST : 0;
	rx_mode |= (netdev->flags & IFF_PROMISC) ? IONIC_RX_MODE_F_PROMISC : 0;
	rx_mode |= (netdev->flags & IFF_ALLMULTI) ? IONIC_RX_MODE_F_ALLMULTI : 0;

	/* sync unicast addresses
	 * next check to see if we're in an overflow state
	 *    if so, we track that we overflowed and enable NIC PROMISC
	 *    else if the overflow is set and not needed
	 *       we remove our overflow flag and check the netdev flags
	 *       to see if we can disable NIC PROMISC
	 */
	__dev_uc_sync(netdev, ionic_addr_add, ionic_addr_del);
	nfilters = le32_to_cpu(ident->lif.eth.max_ucast_filters);
	if (netdev_uc_count(netdev) + 1 > nfilters) {
		rx_mode |= IONIC_RX_MODE_F_PROMISC;
		lif->uc_overflow = true;
	} else if (lif->uc_overflow) {
		lif->uc_overflow = false;
		if (!(netdev->flags & IFF_PROMISC))
			rx_mode &= ~IONIC_RX_MODE_F_PROMISC;
	}

	/* same for multicast */
	__dev_mc_sync(netdev, ionic_addr_add, ionic_addr_del);
	nfilters = le32_to_cpu(ident->lif.eth.max_mcast_filters);
	if (netdev_mc_count(netdev) > nfilters) {
		rx_mode |= IONIC_RX_MODE_F_ALLMULTI;
		lif->mc_overflow = true;
	} else if (lif->mc_overflow) {
		lif->mc_overflow = false;
		if (!(netdev->flags & IFF_ALLMULTI))
			rx_mode &= ~IONIC_RX_MODE_F_ALLMULTI;
	}

	if (lif->rx_mode != rx_mode)
		_ionic_lif_rx_mode(lif, rx_mode);
}

static __le64 ionic_netdev_features_to_nic(netdev_features_t features)
{
	u64 wanted = 0;

	if (features & NETIF_F_HW_VLAN_CTAG_TX)
		wanted |= IONIC_ETH_HW_VLAN_TX_TAG;
	if (features & NETIF_F_HW_VLAN_CTAG_RX)
		wanted |= IONIC_ETH_HW_VLAN_RX_STRIP;
	if (features & NETIF_F_HW_VLAN_CTAG_FILTER)
		wanted |= IONIC_ETH_HW_VLAN_RX_FILTER;
	if (features & NETIF_F_RXHASH)
		wanted |= IONIC_ETH_HW_RX_HASH;
	if (features & NETIF_F_RXCSUM)
		wanted |= IONIC_ETH_HW_RX_CSUM;
	if (features & NETIF_F_SG)
		wanted |= IONIC_ETH_HW_TX_SG;
	if (features & NETIF_F_HW_CSUM)
		wanted |= IONIC_ETH_HW_TX_CSUM;
	if (features & NETIF_F_TSO)
		wanted |= IONIC_ETH_HW_TSO;
	if (features & NETIF_F_TSO6)
		wanted |= IONIC_ETH_HW_TSO_IPV6;
	if (features & NETIF_F_TSO_ECN)
		wanted |= IONIC_ETH_HW_TSO_ECN;
	if (features & NETIF_F_GSO_GRE)
		wanted |= IONIC_ETH_HW_TSO_GRE;
	if (features & NETIF_F_GSO_GRE_CSUM)
		wanted |= IONIC_ETH_HW_TSO_GRE_CSUM;
	if (features & NETIF_F_GSO_IPXIP4)
		wanted |= IONIC_ETH_HW_TSO_IPXIP4;
	if (features & NETIF_F_GSO_IPXIP6)
		wanted |= IONIC_ETH_HW_TSO_IPXIP6;
	if (features & NETIF_F_GSO_UDP_TUNNEL)
		wanted |= IONIC_ETH_HW_TSO_UDP;
	if (features & NETIF_F_GSO_UDP_TUNNEL_CSUM)
		wanted |= IONIC_ETH_HW_TSO_UDP_CSUM;

	return cpu_to_le64(wanted);
}

static int ionic_set_nic_features(struct ionic_lif *lif,
				  netdev_features_t features)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_FEATURES,
		},
	};
	u64 vlan_flags = IONIC_ETH_HW_VLAN_TX_TAG |
			 IONIC_ETH_HW_VLAN_RX_STRIP |
			 IONIC_ETH_HW_VLAN_RX_FILTER;
	u64 old_hw_features;
	int err;

	ctx.cmd.lif_setattr.features = ionic_netdev_features_to_nic(features);
	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	old_hw_features = lif->hw_features;
	lif->hw_features = le64_to_cpu(ctx.cmd.lif_setattr.features &
				       ctx.comp.lif_setattr.features);

	if ((old_hw_features ^ lif->hw_features) & IONIC_ETH_HW_RX_HASH)
		ionic_lif_rss_config(lif, lif->rss_types, NULL, NULL);

	if ((vlan_flags & features) &&
	    !(vlan_flags & le64_to_cpu(ctx.comp.lif_setattr.features)))
		dev_info_once(lif->ionic->dev, "NIC is not supporting vlan offload, likely in SmartNIC mode\n");

	if (lif->hw_features & IONIC_ETH_HW_VLAN_TX_TAG)
		dev_dbg(dev, "feature ETH_HW_VLAN_TX_TAG\n");
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_STRIP)
		dev_dbg(dev, "feature ETH_HW_VLAN_RX_STRIP\n");
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_FILTER)
		dev_dbg(dev, "feature ETH_HW_VLAN_RX_FILTER\n");
	if (lif->hw_features & IONIC_ETH_HW_RX_HASH)
		dev_dbg(dev, "feature ETH_HW_RX_HASH\n");
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)
		dev_dbg(dev, "feature ETH_HW_TX_SG\n");
	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)
		dev_dbg(dev, "feature ETH_HW_TX_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_RX_CSUM)
		dev_dbg(dev, "feature ETH_HW_RX_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO)
		dev_dbg(dev, "feature ETH_HW_TSO\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)
		dev_dbg(dev, "feature ETH_HW_TSO_IPV6\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_ECN)
		dev_dbg(dev, "feature ETH_HW_TSO_ECN\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE)
		dev_dbg(dev, "feature ETH_HW_TSO_GRE\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE_CSUM)
		dev_dbg(dev, "feature ETH_HW_TSO_GRE_CSUM\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP4)
		dev_dbg(dev, "feature ETH_HW_TSO_IPXIP4\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP6)
		dev_dbg(dev, "feature ETH_HW_TSO_IPXIP6\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP)
		dev_dbg(dev, "feature ETH_HW_TSO_UDP\n");
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP_CSUM)
		dev_dbg(dev, "feature ETH_HW_TSO_UDP_CSUM\n");

	return 0;
}

static int ionic_init_nic_features(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	netdev_features_t features;
	int err;

	/* set up what we expect to support by default */
	features = NETIF_F_HW_VLAN_CTAG_TX |
		   NETIF_F_HW_VLAN_CTAG_RX |
		   NETIF_F_HW_VLAN_CTAG_FILTER |
		   NETIF_F_RXHASH |
		   NETIF_F_SG |
		   NETIF_F_HW_CSUM |
		   NETIF_F_RXCSUM |
		   NETIF_F_TSO |
		   NETIF_F_TSO6 |
		   NETIF_F_TSO_ECN;

	err = ionic_set_nic_features(lif, features);
	if (err)
		return err;

	/* tell the netdev what we actually can support */
	netdev->features |= NETIF_F_HIGHDMA;

	if (lif->hw_features & IONIC_ETH_HW_VLAN_TX_TAG)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_STRIP)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;
	if (lif->hw_features & IONIC_ETH_HW_VLAN_RX_FILTER)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;
	if (lif->hw_features & IONIC_ETH_HW_RX_HASH)
		netdev->hw_features |= NETIF_F_RXHASH;
	if (lif->hw_features & IONIC_ETH_HW_TX_SG)
		netdev->hw_features |= NETIF_F_SG;

	if (lif->hw_features & IONIC_ETH_HW_TX_CSUM)
		netdev->hw_enc_features |= NETIF_F_HW_CSUM;
	if (lif->hw_features & IONIC_ETH_HW_RX_CSUM)
		netdev->hw_enc_features |= NETIF_F_RXCSUM;
	if (lif->hw_features & IONIC_ETH_HW_TSO)
		netdev->hw_enc_features |= NETIF_F_TSO;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPV6)
		netdev->hw_enc_features |= NETIF_F_TSO6;
	if (lif->hw_features & IONIC_ETH_HW_TSO_ECN)
		netdev->hw_enc_features |= NETIF_F_TSO_ECN;
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE)
		netdev->hw_enc_features |= NETIF_F_GSO_GRE;
	if (lif->hw_features & IONIC_ETH_HW_TSO_GRE_CSUM)
		netdev->hw_enc_features |= NETIF_F_GSO_GRE_CSUM;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP4)
		netdev->hw_enc_features |= NETIF_F_GSO_IPXIP4;
	if (lif->hw_features & IONIC_ETH_HW_TSO_IPXIP6)
		netdev->hw_enc_features |= NETIF_F_GSO_IPXIP6;
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP)
		netdev->hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL;
	if (lif->hw_features & IONIC_ETH_HW_TSO_UDP_CSUM)
		netdev->hw_enc_features |= NETIF_F_GSO_UDP_TUNNEL_CSUM;

	netdev->hw_features |= netdev->hw_enc_features;
	netdev->features |= netdev->hw_features;
	netdev->vlan_features |= netdev->features & ~NETIF_F_VLAN_FEATURES;

	netdev->priv_flags |= IFF_UNICAST_FLT |
			      IFF_LIVE_ADDR_CHANGE;

	return 0;
}

static int ionic_set_features(struct net_device *netdev,
			      netdev_features_t features)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	netdev_dbg(netdev, "%s: lif->features=0x%08llx new_features=0x%08llx\n",
		   __func__, (u64)lif->netdev->features, (u64)features);

	err = ionic_set_nic_features(lif, features);

	return err;
}

static int ionic_set_mac_address(struct net_device *netdev, void *sa)
{
	struct sockaddr *addr = sa;
	u8 *mac;
	int err;

	mac = (u8 *)addr->sa_data;
	if (ether_addr_equal(netdev->dev_addr, mac))
		return 0;

	err = eth_prepare_mac_addr_change(netdev, addr);
	if (err)
		return err;

	if (!is_zero_ether_addr(netdev->dev_addr)) {
		netdev_info(netdev, "deleting mac addr %pM\n",
			    netdev->dev_addr);
		ionic_addr_del(netdev, netdev->dev_addr);
	}

	eth_commit_mac_addr_change(netdev, addr);
	netdev_info(netdev, "updating mac addr %pM\n", mac);

	return ionic_addr_add(netdev, mac);
}

static int ionic_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MTU,
			.mtu = cpu_to_le32(new_mtu),
		},
	};
	int err;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	netdev->mtu = new_mtu;
	err = ionic_reset_queues(lif, NULL, NULL);

	return err;
}

static void ionic_tx_timeout_work(struct work_struct *ws)
{
	struct ionic_lif *lif = container_of(ws, struct ionic_lif, tx_timeout_work);

	netdev_info(lif->netdev, "Tx Timeout recovery\n");

	rtnl_lock();
	ionic_reset_queues(lif, NULL, NULL);
	rtnl_unlock();
}

static void ionic_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	struct ionic_lif *lif = netdev_priv(netdev);

	schedule_work(&lif->tx_timeout_work);
}

static int ionic_vlan_rx_add_vid(struct net_device *netdev, __be16 proto,
				 u16 vid)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_add = {
			.opcode = IONIC_CMD_RX_FILTER_ADD,
			.lif_index = cpu_to_le16(lif->index),
			.match = cpu_to_le16(IONIC_RX_FILTER_MATCH_VLAN),
			.vlan.vlan = cpu_to_le16(vid),
		},
	};
	int err;

	netdev_dbg(netdev, "rx_filter add VLAN %d\n", vid);
	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	return ionic_rx_filter_save(lif, 0, IONIC_RXQ_INDEX_ANY, 0, &ctx);
}

static int ionic_vlan_rx_kill_vid(struct net_device *netdev, __be16 proto,
				  u16 vid)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.rx_filter_del = {
			.opcode = IONIC_CMD_RX_FILTER_DEL,
			.lif_index = cpu_to_le16(lif->index),
		},
	};
	struct ionic_rx_filter *f;

	spin_lock_bh(&lif->rx_filters.lock);

	f = ionic_rx_filter_by_vlan(lif, vid);
	if (!f) {
		spin_unlock_bh(&lif->rx_filters.lock);
		return -ENOENT;
	}

	netdev_dbg(netdev, "rx_filter del VLAN %d (id %d)\n",
		   vid, f->filter_id);

	ctx.cmd.rx_filter_del.filter_id = cpu_to_le32(f->filter_id);
	ionic_rx_filter_free(lif, f);
	spin_unlock_bh(&lif->rx_filters.lock);

	return ionic_adminq_post_wait(lif, &ctx);
}

int ionic_lif_rss_config(struct ionic_lif *lif, const u16 types,
			 const u8 *key, const u32 *indir)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.attr = IONIC_LIF_ATTR_RSS,
			.rss.addr = cpu_to_le64(lif->rss_ind_tbl_pa),
		},
	};
	unsigned int i, tbl_sz;

	if (lif->hw_features & IONIC_ETH_HW_RX_HASH) {
		lif->rss_types = types;
		ctx.cmd.lif_setattr.rss.types = cpu_to_le16(types);
	}

	if (key)
		memcpy(lif->rss_hash_key, key, IONIC_RSS_HASH_KEY_SIZE);

	if (indir) {
		tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
		for (i = 0; i < tbl_sz; i++)
			lif->rss_ind_tbl[i] = indir[i];
	}

	memcpy(ctx.cmd.lif_setattr.rss.key, lif->rss_hash_key,
	       IONIC_RSS_HASH_KEY_SIZE);

	return ionic_adminq_post_wait(lif, &ctx);
}

static int ionic_lif_rss_init(struct ionic_lif *lif)
{
	unsigned int tbl_sz;
	unsigned int i;

	lif->rss_types = IONIC_RSS_TYPE_IPV4     |
			 IONIC_RSS_TYPE_IPV4_TCP |
			 IONIC_RSS_TYPE_IPV4_UDP |
			 IONIC_RSS_TYPE_IPV6     |
			 IONIC_RSS_TYPE_IPV6_TCP |
			 IONIC_RSS_TYPE_IPV6_UDP;

	/* Fill indirection table with 'default' values */
	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	for (i = 0; i < tbl_sz; i++)
		lif->rss_ind_tbl[i] = ethtool_rxfh_indir_default(i, lif->nxqs);

	return ionic_lif_rss_config(lif, lif->rss_types, NULL, NULL);
}

static void ionic_lif_rss_deinit(struct ionic_lif *lif)
{
	int tbl_sz;

	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	memset(lif->rss_ind_tbl, 0, tbl_sz);
	memset(lif->rss_hash_key, 0, IONIC_RSS_HASH_KEY_SIZE);

	ionic_lif_rss_config(lif, 0x0, NULL, NULL);
}

static void ionic_txrx_disable(struct ionic_lif *lif)
{
	unsigned int i;
	int err;

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			err = ionic_qcq_disable(lif->txqcqs[i].qcq);
			if (err == -ETIMEDOUT)
				break;
		}
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			err = ionic_qcq_disable(lif->rxqcqs[i].qcq);
			if (err == -ETIMEDOUT)
				break;
		}
	}
}

static void ionic_txrx_deinit(struct ionic_lif *lif)
{
	unsigned int i;

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			ionic_lif_qcq_deinit(lif, lif->txqcqs[i].qcq);
			ionic_tx_flush(&lif->txqcqs[i].qcq->cq);
			ionic_tx_empty(&lif->txqcqs[i].qcq->q);
		}
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			ionic_lif_qcq_deinit(lif, lif->rxqcqs[i].qcq);
			ionic_rx_flush(&lif->rxqcqs[i].qcq->cq);
			ionic_rx_empty(&lif->rxqcqs[i].qcq->q);
		}
	}
	lif->rx_mode = 0;
}

static void ionic_txrx_free(struct ionic_lif *lif)
{
	unsigned int i;

	if (lif->txqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			ionic_qcq_free(lif, lif->txqcqs[i].qcq);
			lif->txqcqs[i].qcq = NULL;
		}
	}

	if (lif->rxqcqs) {
		for (i = 0; i < lif->nxqs; i++) {
			ionic_qcq_free(lif, lif->rxqcqs[i].qcq);
			lif->rxqcqs[i].qcq = NULL;
		}
	}
}

static int ionic_txrx_alloc(struct ionic_lif *lif)
{
	unsigned int sg_desc_sz;
	unsigned int flags;
	unsigned int i;
	int err = 0;

	if (lif->qtype_info[IONIC_QTYPE_TXQ].version >= 1 &&
	    lif->qtype_info[IONIC_QTYPE_TXQ].sg_desc_sz ==
					  sizeof(struct ionic_txq_sg_desc_v1))
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc_v1);
	else
		sg_desc_sz = sizeof(struct ionic_txq_sg_desc);

	flags = IONIC_QCQ_F_TX_STATS | IONIC_QCQ_F_SG;
	if (test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
		flags |= IONIC_QCQ_F_INTR;
	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_TXQ, i, "tx", flags,
				      lif->ntxq_descs,
				      sizeof(struct ionic_txq_desc),
				      sizeof(struct ionic_txq_comp),
				      sg_desc_sz,
				      lif->kern_pid, &lif->txqcqs[i].qcq);
		if (err)
			goto err_out;

		if (flags & IONIC_QCQ_F_INTR)
			ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
					     lif->txqcqs[i].qcq->intr.index,
					     lif->tx_coalesce_hw);

		lif->txqcqs[i].qcq->stats = lif->txqcqs[i].stats;
		ionic_debugfs_add_qcq(lif, lif->txqcqs[i].qcq);
	}

	flags = IONIC_QCQ_F_RX_STATS | IONIC_QCQ_F_SG | IONIC_QCQ_F_INTR;
	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_qcq_alloc(lif, IONIC_QTYPE_RXQ, i, "rx", flags,
				      lif->nrxq_descs,
				      sizeof(struct ionic_rxq_desc),
				      sizeof(struct ionic_rxq_comp),
				      sizeof(struct ionic_rxq_sg_desc),
				      lif->kern_pid, &lif->rxqcqs[i].qcq);
		if (err)
			goto err_out;

		ionic_intr_coal_init(lif->ionic->idev.intr_ctrl,
				     lif->rxqcqs[i].qcq->intr.index,
				     lif->rx_coalesce_hw);

		if (!test_bit(IONIC_LIF_F_SPLIT_INTR, lif->state))
			ionic_link_qcq_interrupts(lif->rxqcqs[i].qcq,
						  lif->txqcqs[i].qcq);

		lif->rxqcqs[i].qcq->stats = lif->rxqcqs[i].stats;
		ionic_debugfs_add_qcq(lif, lif->rxqcqs[i].qcq);
	}

	return 0;

err_out:
	ionic_txrx_free(lif);

	return err;
}

static int ionic_txrx_init(struct ionic_lif *lif)
{
	unsigned int i;
	int err;

	for (i = 0; i < lif->nxqs; i++) {
		err = ionic_lif_txq_init(lif, lif->txqcqs[i].qcq);
		if (err)
			goto err_out;

		err = ionic_lif_rxq_init(lif, lif->rxqcqs[i].qcq);
		if (err) {
			ionic_lif_qcq_deinit(lif, lif->txqcqs[i].qcq);
			goto err_out;
		}
	}

	if (lif->netdev->features & NETIF_F_RXHASH)
		ionic_lif_rss_init(lif);

	ionic_set_rx_mode(lif->netdev);

	return 0;

err_out:
	while (i--) {
		ionic_lif_qcq_deinit(lif, lif->txqcqs[i].qcq);
		ionic_lif_qcq_deinit(lif, lif->rxqcqs[i].qcq);
	}

	return err;
}

static int ionic_txrx_enable(struct ionic_lif *lif)
{
	int i, err;

	for (i = 0; i < lif->nxqs; i++) {
		ionic_rx_fill(&lif->rxqcqs[i].qcq->q);
		err = ionic_qcq_enable(lif->rxqcqs[i].qcq);
		if (err)
			goto err_out;

		err = ionic_qcq_enable(lif->txqcqs[i].qcq);
		if (err) {
			if (err != -ETIMEDOUT)
				ionic_qcq_disable(lif->rxqcqs[i].qcq);
			goto err_out;
		}
	}

	return 0;

err_out:
	while (i--) {
		err = ionic_qcq_disable(lif->txqcqs[i].qcq);
		if (err == -ETIMEDOUT)
			break;
		err = ionic_qcq_disable(lif->rxqcqs[i].qcq);
		if (err == -ETIMEDOUT)
			break;
	}

	return err;
}

static int ionic_start_queues(struct ionic_lif *lif)
{
	int err;

	if (test_and_set_bit(IONIC_LIF_F_UP, lif->state))
		return 0;

	err = ionic_txrx_enable(lif);
	if (err) {
		clear_bit(IONIC_LIF_F_UP, lif->state);
		return err;
	}
	netif_tx_wake_all_queues(lif->netdev);

	return 0;
}

int ionic_open(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	int err;

	err = ionic_txrx_alloc(lif);
	if (err)
		return err;

	err = ionic_txrx_init(lif);
	if (err)
		goto err_out;

	err = netif_set_real_num_tx_queues(netdev, lif->nxqs);
	if (err)
		goto err_txrx_deinit;

	err = netif_set_real_num_rx_queues(netdev, lif->nxqs);
	if (err)
		goto err_txrx_deinit;

	/* don't start the queues until we have link */
	if (netif_carrier_ok(netdev)) {
		err = ionic_start_queues(lif);
		if (err)
			goto err_txrx_deinit;
	}

	return 0;

err_txrx_deinit:
	ionic_txrx_deinit(lif);
err_out:
	ionic_txrx_free(lif);
	return err;
}

static void ionic_stop_queues(struct ionic_lif *lif)
{
	if (!test_and_clear_bit(IONIC_LIF_F_UP, lif->state))
		return;

	netif_tx_disable(lif->netdev);
	ionic_txrx_disable(lif);
}

int ionic_stop(struct net_device *netdev)
{
	struct ionic_lif *lif = netdev_priv(netdev);

	if (test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return 0;

	ionic_stop_queues(lif);
	ionic_txrx_deinit(lif);
	ionic_txrx_free(lif);

	return 0;
}

static int ionic_get_vf_config(struct net_device *netdev,
			       int vf, struct ifla_vf_info *ivf)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret = 0;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_read(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ivf->vf           = vf;
		ivf->vlan         = ionic->vfs[vf].vlanid;
		ivf->qos	  = 0;
		ivf->spoofchk     = ionic->vfs[vf].spoofchk;
		ivf->linkstate    = ionic->vfs[vf].linkstate;
		ivf->max_tx_rate  = ionic->vfs[vf].maxrate;
		ivf->trusted      = ionic->vfs[vf].trusted;
		ether_addr_copy(ivf->mac, ionic->vfs[vf].macaddr);
	}

	up_read(&ionic->vf_op_lock);
	return ret;
}

static int ionic_get_vf_stats(struct net_device *netdev, int vf,
			      struct ifla_vf_stats *vf_stats)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	struct ionic_lif_stats *vs;
	int ret = 0;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_read(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		memset(vf_stats, 0, sizeof(*vf_stats));
		vs = &ionic->vfs[vf].stats;

		vf_stats->rx_packets = le64_to_cpu(vs->rx_ucast_packets);
		vf_stats->tx_packets = le64_to_cpu(vs->tx_ucast_packets);
		vf_stats->rx_bytes   = le64_to_cpu(vs->rx_ucast_bytes);
		vf_stats->tx_bytes   = le64_to_cpu(vs->tx_ucast_bytes);
		vf_stats->broadcast  = le64_to_cpu(vs->rx_bcast_packets);
		vf_stats->multicast  = le64_to_cpu(vs->rx_mcast_packets);
		vf_stats->rx_dropped = le64_to_cpu(vs->rx_ucast_drop_packets) +
				       le64_to_cpu(vs->rx_mcast_drop_packets) +
				       le64_to_cpu(vs->rx_bcast_drop_packets);
		vf_stats->tx_dropped = le64_to_cpu(vs->tx_ucast_drop_packets) +
				       le64_to_cpu(vs->tx_mcast_drop_packets) +
				       le64_to_cpu(vs->tx_bcast_drop_packets);
	}

	up_read(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_mac(struct net_device *netdev, int vf, u8 *mac)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	if (!(is_zero_ether_addr(mac) || is_valid_ether_addr(mac)))
		return -EINVAL;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf, IONIC_VF_ATTR_MAC, mac);
		if (!ret)
			ether_addr_copy(ionic->vfs[vf].macaddr, mac);
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_vlan(struct net_device *netdev, int vf, u16 vlan,
			     u8 qos, __be16 proto)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	/* until someday when we support qos */
	if (qos)
		return -EINVAL;

	if (vlan > 4095)
		return -EINVAL;

	if (proto != htons(ETH_P_8021Q))
		return -EPROTONOSUPPORT;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf,
					  IONIC_VF_ATTR_VLAN, (u8 *)&vlan);
		if (!ret)
			ionic->vfs[vf].vlanid = vlan;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_rate(struct net_device *netdev, int vf,
			     int tx_min, int tx_max)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	int ret;

	/* setting the min just seems silly */
	if (tx_min)
		return -EINVAL;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf,
					  IONIC_VF_ATTR_RATE, (u8 *)&tx_max);
		if (!ret)
			lif->ionic->vfs[vf].maxrate = tx_max;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_spoofchk(struct net_device *netdev, int vf, bool set)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	u8 data = set;  /* convert to u8 for config */
	int ret;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf,
					  IONIC_VF_ATTR_SPOOFCHK, &data);
		if (!ret)
			ionic->vfs[vf].spoofchk = data;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_trust(struct net_device *netdev, int vf, bool set)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	u8 data = set;  /* convert to u8 for config */
	int ret;

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf,
					  IONIC_VF_ATTR_TRUST, &data);
		if (!ret)
			ionic->vfs[vf].trusted = data;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static int ionic_set_vf_link_state(struct net_device *netdev, int vf, int set)
{
	struct ionic_lif *lif = netdev_priv(netdev);
	struct ionic *ionic = lif->ionic;
	u8 data;
	int ret;

	switch (set) {
	case IFLA_VF_LINK_STATE_ENABLE:
		data = IONIC_VF_LINK_STATUS_UP;
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		data = IONIC_VF_LINK_STATUS_DOWN;
		break;
	case IFLA_VF_LINK_STATE_AUTO:
		data = IONIC_VF_LINK_STATUS_AUTO;
		break;
	default:
		return -EINVAL;
	}

	if (!netif_device_present(netdev))
		return -EBUSY;

	down_write(&ionic->vf_op_lock);

	if (vf >= pci_num_vf(ionic->pdev) || !ionic->vfs) {
		ret = -EINVAL;
	} else {
		ret = ionic_set_vf_config(ionic, vf,
					  IONIC_VF_ATTR_LINKSTATE, &data);
		if (!ret)
			ionic->vfs[vf].linkstate = set;
	}

	up_write(&ionic->vf_op_lock);
	return ret;
}

static const struct net_device_ops ionic_netdev_ops = {
	.ndo_open               = ionic_open,
	.ndo_stop               = ionic_stop,
	.ndo_start_xmit		= ionic_start_xmit,
	.ndo_get_stats64	= ionic_get_stats64,
	.ndo_set_rx_mode	= ionic_set_rx_mode,
	.ndo_set_features	= ionic_set_features,
	.ndo_set_mac_address	= ionic_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout         = ionic_tx_timeout,
	.ndo_change_mtu         = ionic_change_mtu,
	.ndo_vlan_rx_add_vid    = ionic_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid   = ionic_vlan_rx_kill_vid,
	.ndo_set_vf_vlan	= ionic_set_vf_vlan,
	.ndo_set_vf_trust	= ionic_set_vf_trust,
	.ndo_set_vf_mac		= ionic_set_vf_mac,
	.ndo_set_vf_rate	= ionic_set_vf_rate,
	.ndo_set_vf_spoofchk	= ionic_set_vf_spoofchk,
	.ndo_get_vf_config	= ionic_get_vf_config,
	.ndo_set_vf_link_state	= ionic_set_vf_link_state,
	.ndo_get_vf_stats       = ionic_get_vf_stats,
};

int ionic_reset_queues(struct ionic_lif *lif, ionic_reset_cb cb, void *arg)
{
	bool running;
	int err = 0;

	mutex_lock(&lif->queue_lock);
	running = netif_running(lif->netdev);
	if (running) {
		netif_device_detach(lif->netdev);
		err = ionic_stop(lif->netdev);
		if (err)
			goto reset_out;
	}

	if (cb)
		cb(lif, arg);

	if (running) {
		err = ionic_open(lif->netdev);
		netif_device_attach(lif->netdev);
	}

reset_out:
	mutex_unlock(&lif->queue_lock);

	return err;
}

static struct ionic_lif *ionic_lif_alloc(struct ionic *ionic, unsigned int index)
{
	struct device *dev = ionic->dev;
	union ionic_lif_identity *lid;
	struct net_device *netdev;
	struct ionic_lif *lif;
	int tbl_sz;
	int err;

	lid = kzalloc(sizeof(*lid), GFP_KERNEL);
	if (!lid)
		return ERR_PTR(-ENOMEM);

	netdev = alloc_etherdev_mqs(sizeof(*lif),
				    ionic->ntxqs_per_lif, ionic->ntxqs_per_lif);
	if (!netdev) {
		dev_err(dev, "Cannot allocate netdev, aborting\n");
		err = -ENOMEM;
		goto err_out_free_lid;
	}

	SET_NETDEV_DEV(netdev, dev);

	lif = netdev_priv(netdev);
	lif->netdev = netdev;
	ionic->master_lif = lif;
	netdev->netdev_ops = &ionic_netdev_ops;
	ionic_ethtool_set_ops(netdev);

	netdev->watchdog_timeo = 2 * HZ;
	netif_carrier_off(netdev);

	lif->identity = lid;
	lif->lif_type = IONIC_LIF_TYPE_CLASSIC;
	ionic_lif_identify(ionic, lif->lif_type, lif->identity);
	lif->netdev->min_mtu = le32_to_cpu(lif->identity->eth.min_frame_size);
	lif->netdev->max_mtu =
		le32_to_cpu(lif->identity->eth.max_frame_size) - ETH_HLEN - VLAN_HLEN;

	lif->neqs = ionic->neqs_per_lif;
	lif->nxqs = ionic->ntxqs_per_lif;

	lif->ionic = ionic;
	lif->index = index;
	lif->ntxq_descs = IONIC_DEF_TXRX_DESC;
	lif->nrxq_descs = IONIC_DEF_TXRX_DESC;
	lif->tx_budget = IONIC_TX_BUDGET_DEFAULT;

	/* Convert the default coalesce value to actual hw resolution */
	lif->rx_coalesce_usecs = IONIC_ITR_COAL_USEC_DEFAULT;
	lif->rx_coalesce_hw = ionic_coal_usec_to_hw(lif->ionic,
						    lif->rx_coalesce_usecs);
	lif->tx_coalesce_usecs = lif->rx_coalesce_usecs;
	lif->tx_coalesce_hw = lif->rx_coalesce_hw;

	snprintf(lif->name, sizeof(lif->name), "lif%u", index);

	spin_lock_init(&lif->adminq_lock);

	spin_lock_init(&lif->deferred.lock);
	INIT_LIST_HEAD(&lif->deferred.list);
	INIT_WORK(&lif->deferred.work, ionic_lif_deferred_work);

	/* allocate lif info */
	lif->info_sz = ALIGN(sizeof(*lif->info), PAGE_SIZE);
	lif->info = dma_alloc_coherent(dev, lif->info_sz,
				       &lif->info_pa, GFP_KERNEL);
	if (!lif->info) {
		dev_err(dev, "Failed to allocate lif info, aborting\n");
		err = -ENOMEM;
		goto err_out_free_netdev;
	}

	ionic_debugfs_add_lif(lif);

	/* allocate queues */
	err = ionic_qcqs_alloc(lif);
	if (err)
		goto err_out_free_lif_info;

	/* allocate rss indirection table */
	tbl_sz = le16_to_cpu(lif->ionic->ident.lif.eth.rss_ind_tbl_sz);
	lif->rss_ind_tbl_sz = sizeof(*lif->rss_ind_tbl) * tbl_sz;
	lif->rss_ind_tbl = dma_alloc_coherent(dev, lif->rss_ind_tbl_sz,
					      &lif->rss_ind_tbl_pa,
					      GFP_KERNEL);

	if (!lif->rss_ind_tbl) {
		err = -ENOMEM;
		dev_err(dev, "Failed to allocate rss indirection table, aborting\n");
		goto err_out_free_qcqs;
	}
	netdev_rss_key_fill(lif->rss_hash_key, IONIC_RSS_HASH_KEY_SIZE);

	list_add_tail(&lif->list, &ionic->lifs);

	return lif;

err_out_free_qcqs:
	ionic_qcqs_free(lif);
err_out_free_lif_info:
	dma_free_coherent(dev, lif->info_sz, lif->info, lif->info_pa);
	lif->info = NULL;
	lif->info_pa = 0;
err_out_free_netdev:
	free_netdev(lif->netdev);
	lif = NULL;
err_out_free_lid:
	kfree(lid);

	return ERR_PTR(err);
}

int ionic_lifs_alloc(struct ionic *ionic)
{
	struct ionic_lif *lif;

	INIT_LIST_HEAD(&ionic->lifs);

	/* only build the first lif, others are for later features */
	set_bit(0, ionic->lifbits);

	lif = ionic_lif_alloc(ionic, 0);
	if (IS_ERR_OR_NULL(lif)) {
		clear_bit(0, ionic->lifbits);
		return -ENOMEM;
	}

	ionic_lif_queue_identify(lif);

	return 0;
}

static void ionic_lif_reset(struct ionic_lif *lif)
{
	struct ionic_dev *idev = &lif->ionic->idev;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_reset(idev, lif->index);
	ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
}

static void ionic_lif_handle_fw_down(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;

	if (test_and_set_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return;

	dev_info(ionic->dev, "FW Down: Stopping LIFs\n");

	netif_device_detach(lif->netdev);

	if (test_bit(IONIC_LIF_F_UP, lif->state)) {
		dev_info(ionic->dev, "Surprise FW stop, stopping queues\n");
		mutex_lock(&lif->queue_lock);
		ionic_stop_queues(lif);
		mutex_unlock(&lif->queue_lock);
	}

	if (netif_running(lif->netdev)) {
		ionic_txrx_deinit(lif);
		ionic_txrx_free(lif);
	}
	ionic_lifs_deinit(ionic);
	ionic_reset(ionic);
	ionic_qcqs_free(lif);

	dev_info(ionic->dev, "FW Down: LIFs stopped\n");
}

static void ionic_lif_handle_fw_up(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;
	int err;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		return;

	dev_info(ionic->dev, "FW Up: restarting LIFs\n");

	ionic_init_devinfo(ionic);
	ionic_port_init(ionic);
	err = ionic_qcqs_alloc(lif);
	if (err)
		goto err_out;

	err = ionic_lifs_init(ionic);
	if (err)
		goto err_qcqs_free;

	if (lif->registered)
		ionic_lif_set_netdev_info(lif);

	ionic_rx_filter_replay(lif);

	if (netif_running(lif->netdev)) {
		err = ionic_txrx_alloc(lif);
		if (err)
			goto err_lifs_deinit;

		err = ionic_txrx_init(lif);
		if (err)
			goto err_txrx_free;
	}

	clear_bit(IONIC_LIF_F_FW_RESET, lif->state);
	ionic_link_status_check_request(lif);
	netif_device_attach(lif->netdev);
	dev_info(ionic->dev, "FW Up: LIFs restarted\n");

	return;

err_txrx_free:
	ionic_txrx_free(lif);
err_lifs_deinit:
	ionic_lifs_deinit(ionic);
err_qcqs_free:
	ionic_qcqs_free(lif);
err_out:
	dev_err(ionic->dev, "FW Up: LIFs restart failed - err %d\n", err);
}

static void ionic_lif_free(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;

	/* free rss indirection table */
	dma_free_coherent(dev, lif->rss_ind_tbl_sz, lif->rss_ind_tbl,
			  lif->rss_ind_tbl_pa);
	lif->rss_ind_tbl = NULL;
	lif->rss_ind_tbl_pa = 0;

	/* free queues */
	ionic_qcqs_free(lif);
	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state))
		ionic_lif_reset(lif);

	/* free lif info */
	kfree(lif->identity);
	dma_free_coherent(dev, lif->info_sz, lif->info, lif->info_pa);
	lif->info = NULL;
	lif->info_pa = 0;

	/* unmap doorbell page */
	ionic_bus_unmap_dbpage(lif->ionic, lif->kern_dbpage);
	lif->kern_dbpage = NULL;
	kfree(lif->dbid_inuse);
	lif->dbid_inuse = NULL;

	/* free netdev & lif */
	ionic_debugfs_del_lif(lif);
	list_del(&lif->list);
	free_netdev(lif->netdev);
}

void ionic_lifs_free(struct ionic *ionic)
{
	struct list_head *cur, *tmp;
	struct ionic_lif *lif;

	list_for_each_safe(cur, tmp, &ionic->lifs) {
		lif = list_entry(cur, struct ionic_lif, list);

		ionic_lif_free(lif);
	}
}

static void ionic_lif_deinit(struct ionic_lif *lif)
{
	if (!test_and_clear_bit(IONIC_LIF_F_INITED, lif->state))
		return;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state)) {
		cancel_work_sync(&lif->deferred.work);
		cancel_work_sync(&lif->tx_timeout_work);
		ionic_rx_filters_deinit(lif);
		if (lif->netdev->features & NETIF_F_RXHASH)
			ionic_lif_rss_deinit(lif);
	}

	napi_disable(&lif->adminqcq->napi);
	ionic_lif_qcq_deinit(lif, lif->notifyqcq);
	ionic_lif_qcq_deinit(lif, lif->adminqcq);

	mutex_destroy(&lif->queue_lock);
	ionic_lif_reset(lif);
}

void ionic_lifs_deinit(struct ionic *ionic)
{
	struct list_head *cur, *tmp;
	struct ionic_lif *lif;

	list_for_each_safe(cur, tmp, &ionic->lifs) {
		lif = list_entry(cur, struct ionic_lif, list);
		ionic_lif_deinit(lif);
	}
}

static int ionic_lif_adminq_init(struct ionic_lif *lif)
{
	struct device *dev = lif->ionic->dev;
	struct ionic_q_init_comp comp;
	struct ionic_dev *idev;
	struct ionic_qcq *qcq;
	struct ionic_queue *q;
	int err;

	idev = &lif->ionic->idev;
	qcq = lif->adminqcq;
	q = &qcq->q;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_adminq_init(idev, qcq, lif->index, qcq->intr.index);
	err = ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	ionic_dev_cmd_comp(idev, (union ionic_dev_cmd_comp *)&comp);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
	if (err) {
		netdev_err(lif->netdev, "adminq init failed %d\n", err);
		return err;
	}

	q->hw_type = comp.hw_type;
	q->hw_index = le32_to_cpu(comp.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "adminq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "adminq->hw_index %d\n", q->hw_index);

	netif_napi_add(lif->netdev, &qcq->napi, ionic_adminq_napi,
		       NAPI_POLL_WEIGHT);

	napi_enable(&qcq->napi);

	if (qcq->flags & IONIC_QCQ_F_INTR)
		ionic_intr_mask(idev->intr_ctrl, qcq->intr.index,
				IONIC_INTR_MASK_CLEAR);

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_lif_notifyq_init(struct ionic_lif *lif)
{
	struct ionic_qcq *qcq = lif->notifyqcq;
	struct device *dev = lif->ionic->dev;
	struct ionic_queue *q = &qcq->q;
	int err;

	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.q_init = {
			.opcode = IONIC_CMD_Q_INIT,
			.lif_index = cpu_to_le16(lif->index),
			.type = q->type,
			.ver = lif->qtype_info[q->type].version,
			.index = cpu_to_le32(q->index),
			.flags = cpu_to_le16(IONIC_QINIT_F_IRQ |
					     IONIC_QINIT_F_ENA),
			.intr_index = cpu_to_le16(lif->adminqcq->intr.index),
			.pid = cpu_to_le16(q->pid),
			.ring_size = ilog2(q->num_descs),
			.ring_base = cpu_to_le64(q->base_pa),
		}
	};

	dev_dbg(dev, "notifyq_init.pid %d\n", ctx.cmd.q_init.pid);
	dev_dbg(dev, "notifyq_init.index %d\n", ctx.cmd.q_init.index);
	dev_dbg(dev, "notifyq_init.ring_base 0x%llx\n", ctx.cmd.q_init.ring_base);
	dev_dbg(dev, "notifyq_init.ring_size %d\n", ctx.cmd.q_init.ring_size);

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;

	lif->last_eid = 0;
	q->hw_type = ctx.comp.q_init.hw_type;
	q->hw_index = le32_to_cpu(ctx.comp.q_init.hw_index);
	q->dbval = IONIC_DBELL_QID(q->hw_index);

	dev_dbg(dev, "notifyq->hw_type %d\n", q->hw_type);
	dev_dbg(dev, "notifyq->hw_index %d\n", q->hw_index);

	/* preset the callback info */
	q->info[0].cb_arg = lif;

	qcq->flags |= IONIC_QCQ_F_INITED;

	return 0;
}

static int ionic_station_set(struct ionic_lif *lif)
{
	struct net_device *netdev = lif->netdev;
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_getattr = {
			.opcode = IONIC_CMD_LIF_GETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_MAC,
		},
	};
	struct sockaddr addr;
	int err;

	err = ionic_adminq_post_wait(lif, &ctx);
	if (err)
		return err;
	netdev_dbg(lif->netdev, "found initial MAC addr %pM\n",
		   ctx.comp.lif_getattr.mac);
	if (is_zero_ether_addr(ctx.comp.lif_getattr.mac))
		return 0;

	if (!is_zero_ether_addr(netdev->dev_addr)) {
		/* If the netdev mac is non-zero and doesn't match the default
		 * device address, it was set by something earlier and we're
		 * likely here again after a fw-upgrade reset.  We need to be
		 * sure the netdev mac is in our filter list.
		 */
		if (!ether_addr_equal(ctx.comp.lif_getattr.mac,
				      netdev->dev_addr))
			ionic_lif_addr(lif, netdev->dev_addr, true);
	} else {
		/* Update the netdev mac with the device's mac */
		memcpy(addr.sa_data, ctx.comp.lif_getattr.mac, netdev->addr_len);
		addr.sa_family = AF_INET;
		err = eth_prepare_mac_addr_change(netdev, &addr);
		if (err) {
			netdev_warn(lif->netdev, "ignoring bad MAC addr from NIC %pM - err %d\n",
				    addr.sa_data, err);
			return 0;
		}

		eth_commit_mac_addr_change(netdev, &addr);
	}

	netdev_dbg(lif->netdev, "adding station MAC addr %pM\n",
		   netdev->dev_addr);
	ionic_lif_addr(lif, netdev->dev_addr, true);

	return 0;
}

static int ionic_lif_init(struct ionic_lif *lif)
{
	struct ionic_dev *idev = &lif->ionic->idev;
	struct device *dev = lif->ionic->dev;
	struct ionic_lif_init_comp comp;
	int dbpage_num;
	int err;

	mutex_lock(&lif->ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_init(idev, lif->index, lif->info_pa);
	err = ionic_dev_cmd_wait(lif->ionic, DEVCMD_TIMEOUT);
	ionic_dev_cmd_comp(idev, (union ionic_dev_cmd_comp *)&comp);
	mutex_unlock(&lif->ionic->dev_cmd_lock);
	if (err)
		return err;

	lif->hw_index = le16_to_cpu(comp.hw_index);
	mutex_init(&lif->queue_lock);

	/* now that we have the hw_index we can figure out our doorbell page */
	lif->dbid_count = le32_to_cpu(lif->ionic->ident.dev.ndbpgs_per_lif);
	if (!lif->dbid_count) {
		dev_err(dev, "No doorbell pages, aborting\n");
		return -EINVAL;
	}

	lif->dbid_inuse = bitmap_alloc(lif->dbid_count, GFP_KERNEL);
	if (!lif->dbid_inuse) {
		dev_err(dev, "Failed alloc doorbell id bitmap, aborting\n");
		return -ENOMEM;
	}

	/* first doorbell id reserved for kernel (dbid aka pid == zero) */
	set_bit(0, lif->dbid_inuse);
	lif->kern_pid = 0;

	dbpage_num = ionic_db_page_num(lif, lif->kern_pid);
	lif->kern_dbpage = ionic_bus_map_dbpage(lif->ionic, dbpage_num);
	if (!lif->kern_dbpage) {
		dev_err(dev, "Cannot map dbpage, aborting\n");
		err = -ENOMEM;
		goto err_out_free_dbid;
	}

	err = ionic_lif_adminq_init(lif);
	if (err)
		goto err_out_adminq_deinit;

	if (lif->ionic->nnqs_per_lif) {
		err = ionic_lif_notifyq_init(lif);
		if (err)
			goto err_out_notifyq_deinit;
	}

	err = ionic_init_nic_features(lif);
	if (err)
		goto err_out_notifyq_deinit;

	if (!test_bit(IONIC_LIF_F_FW_RESET, lif->state)) {
		err = ionic_rx_filters_init(lif);
		if (err)
			goto err_out_notifyq_deinit;
	}

	err = ionic_station_set(lif);
	if (err)
		goto err_out_notifyq_deinit;

	lif->rx_copybreak = IONIC_RX_COPYBREAK_DEFAULT;

	set_bit(IONIC_LIF_F_INITED, lif->state);

	INIT_WORK(&lif->tx_timeout_work, ionic_tx_timeout_work);

	return 0;

err_out_notifyq_deinit:
	ionic_lif_qcq_deinit(lif, lif->notifyqcq);
err_out_adminq_deinit:
	ionic_lif_qcq_deinit(lif, lif->adminqcq);
	ionic_lif_reset(lif);
	ionic_bus_unmap_dbpage(lif->ionic, lif->kern_dbpage);
	lif->kern_dbpage = NULL;
err_out_free_dbid:
	kfree(lif->dbid_inuse);
	lif->dbid_inuse = NULL;

	return err;
}

int ionic_lifs_init(struct ionic *ionic)
{
	struct list_head *cur, *tmp;
	struct ionic_lif *lif;
	int err;

	list_for_each_safe(cur, tmp, &ionic->lifs) {
		lif = list_entry(cur, struct ionic_lif, list);
		err = ionic_lif_init(lif);
		if (err)
			return err;
	}

	return 0;
}

static void ionic_lif_notify_work(struct work_struct *ws)
{
}

static void ionic_lif_set_netdev_info(struct ionic_lif *lif)
{
	struct ionic_admin_ctx ctx = {
		.work = COMPLETION_INITIALIZER_ONSTACK(ctx.work),
		.cmd.lif_setattr = {
			.opcode = IONIC_CMD_LIF_SETATTR,
			.index = cpu_to_le16(lif->index),
			.attr = IONIC_LIF_ATTR_NAME,
		},
	};

	strlcpy(ctx.cmd.lif_setattr.name, lif->netdev->name,
		sizeof(ctx.cmd.lif_setattr.name));

	ionic_adminq_post_wait(lif, &ctx);
}

static struct ionic_lif *ionic_netdev_lif(struct net_device *netdev)
{
	if (!netdev || netdev->netdev_ops->ndo_start_xmit != ionic_start_xmit)
		return NULL;

	return netdev_priv(netdev);
}

static int ionic_lif_notify(struct notifier_block *nb,
			    unsigned long event, void *info)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(info);
	struct ionic *ionic = container_of(nb, struct ionic, nb);
	struct ionic_lif *lif = ionic_netdev_lif(ndev);

	if (!lif || lif->ionic != ionic)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_CHANGENAME:
		ionic_lif_set_netdev_info(lif);
		break;
	}

	return NOTIFY_DONE;
}

int ionic_lifs_register(struct ionic *ionic)
{
	int err;

	INIT_WORK(&ionic->nb_work, ionic_lif_notify_work);

	ionic->nb.notifier_call = ionic_lif_notify;

	err = register_netdevice_notifier(&ionic->nb);
	if (err)
		ionic->nb.notifier_call = NULL;

	/* only register LIF0 for now */
	err = register_netdev(ionic->master_lif->netdev);
	if (err) {
		dev_err(ionic->dev, "Cannot register net device, aborting\n");
		return err;
	}
	ionic->master_lif->registered = true;
	ionic_lif_set_netdev_info(ionic->master_lif);

	return 0;
}

void ionic_lifs_unregister(struct ionic *ionic)
{
	if (ionic->nb.notifier_call) {
		unregister_netdevice_notifier(&ionic->nb);
		cancel_work_sync(&ionic->nb_work);
		ionic->nb.notifier_call = NULL;
	}

	/* There is only one lif ever registered in the
	 * current model, so don't bother searching the
	 * ionic->lif for candidates to unregister
	 */
	if (ionic->master_lif &&
	    ionic->master_lif->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(ionic->master_lif->netdev);
}

static void ionic_lif_queue_identify(struct ionic_lif *lif)
{
	struct ionic *ionic = lif->ionic;
	union ionic_q_identity *q_ident;
	struct ionic_dev *idev;
	int qtype;
	int err;

	idev = &lif->ionic->idev;
	q_ident = (union ionic_q_identity *)&idev->dev_cmd_regs->data;

	for (qtype = 0; qtype < ARRAY_SIZE(ionic_qtype_versions); qtype++) {
		struct ionic_qtype_info *qti = &lif->qtype_info[qtype];

		/* filter out the ones we know about */
		switch (qtype) {
		case IONIC_QTYPE_ADMINQ:
		case IONIC_QTYPE_NOTIFYQ:
		case IONIC_QTYPE_RXQ:
		case IONIC_QTYPE_TXQ:
			break;
		default:
			continue;
		}

		memset(qti, 0, sizeof(*qti));

		mutex_lock(&ionic->dev_cmd_lock);
		ionic_dev_cmd_queue_identify(idev, lif->lif_type, qtype,
					     ionic_qtype_versions[qtype]);
		err = ionic_dev_cmd_wait(ionic, DEVCMD_TIMEOUT);
		if (!err) {
			qti->version   = q_ident->version;
			qti->supported = q_ident->supported;
			qti->features  = le64_to_cpu(q_ident->features);
			qti->desc_sz   = le16_to_cpu(q_ident->desc_sz);
			qti->comp_sz   = le16_to_cpu(q_ident->comp_sz);
			qti->sg_desc_sz   = le16_to_cpu(q_ident->sg_desc_sz);
			qti->max_sg_elems = le16_to_cpu(q_ident->max_sg_elems);
			qti->sg_desc_stride = le16_to_cpu(q_ident->sg_desc_stride);
		}
		mutex_unlock(&ionic->dev_cmd_lock);

		if (err == -EINVAL) {
			dev_err(ionic->dev, "qtype %d not supported\n", qtype);
			continue;
		} else if (err == -EIO) {
			dev_err(ionic->dev, "q_ident failed, not supported on older FW\n");
			return;
		} else if (err) {
			dev_err(ionic->dev, "q_ident failed, qtype %d: %d\n",
				qtype, err);
			return;
		}

		dev_dbg(ionic->dev, " qtype[%d].version = %d\n",
			qtype, qti->version);
		dev_dbg(ionic->dev, " qtype[%d].supported = 0x%02x\n",
			qtype, qti->supported);
		dev_dbg(ionic->dev, " qtype[%d].features = 0x%04llx\n",
			qtype, qti->features);
		dev_dbg(ionic->dev, " qtype[%d].desc_sz = %d\n",
			qtype, qti->desc_sz);
		dev_dbg(ionic->dev, " qtype[%d].comp_sz = %d\n",
			qtype, qti->comp_sz);
		dev_dbg(ionic->dev, " qtype[%d].sg_desc_sz = %d\n",
			qtype, qti->sg_desc_sz);
		dev_dbg(ionic->dev, " qtype[%d].max_sg_elems = %d\n",
			qtype, qti->max_sg_elems);
		dev_dbg(ionic->dev, " qtype[%d].sg_desc_stride = %d\n",
			qtype, qti->sg_desc_stride);
	}
}

int ionic_lif_identify(struct ionic *ionic, u8 lif_type,
		       union ionic_lif_identity *lid)
{
	struct ionic_dev *idev = &ionic->idev;
	size_t sz;
	int err;

	sz = min(sizeof(*lid), sizeof(idev->dev_cmd_regs->data));

	mutex_lock(&ionic->dev_cmd_lock);
	ionic_dev_cmd_lif_identify(idev, lif_type, IONIC_IDENTITY_VERSION_1);
	err = ionic_dev_cmd_wait(ionic, DEVCMD_TIMEOUT);
	memcpy_fromio(lid, &idev->dev_cmd_regs->data, sz);
	mutex_unlock(&ionic->dev_cmd_lock);
	if (err)
		return (err);

	dev_dbg(ionic->dev, "capabilities 0x%llx\n",
		le64_to_cpu(lid->capabilities));

	dev_dbg(ionic->dev, "eth.max_ucast_filters %d\n",
		le32_to_cpu(lid->eth.max_ucast_filters));
	dev_dbg(ionic->dev, "eth.max_mcast_filters %d\n",
		le32_to_cpu(lid->eth.max_mcast_filters));
	dev_dbg(ionic->dev, "eth.features 0x%llx\n",
		le64_to_cpu(lid->eth.config.features));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_ADMINQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_ADMINQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_NOTIFYQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_NOTIFYQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_RXQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_RXQ]));
	dev_dbg(ionic->dev, "eth.queue_count[IONIC_QTYPE_TXQ] %d\n",
		le32_to_cpu(lid->eth.config.queue_count[IONIC_QTYPE_TXQ]));
	dev_dbg(ionic->dev, "eth.config.name %s\n", lid->eth.config.name);
	dev_dbg(ionic->dev, "eth.config.mac %pM\n", lid->eth.config.mac);
	dev_dbg(ionic->dev, "eth.config.mtu %d\n",
		le32_to_cpu(lid->eth.config.mtu));

	return 0;
}

int ionic_lifs_size(struct ionic *ionic)
{
	struct ionic_identity *ident = &ionic->ident;
	unsigned int nintrs, dev_nintrs;
	union ionic_lif_config *lc;
	unsigned int ntxqs_per_lif;
	unsigned int nrxqs_per_lif;
	unsigned int neqs_per_lif;
	unsigned int nnqs_per_lif;
	unsigned int nxqs, neqs;
	unsigned int min_intrs;
	int err;

	lc = &ident->lif.eth.config;
	dev_nintrs = le32_to_cpu(ident->dev.nintrs);
	neqs_per_lif = le32_to_cpu(ident->lif.rdma.eq_qtype.qid_count);
	nnqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_NOTIFYQ]);
	ntxqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_TXQ]);
	nrxqs_per_lif = le32_to_cpu(lc->queue_count[IONIC_QTYPE_RXQ]);

	nxqs = min(ntxqs_per_lif, nrxqs_per_lif);
	nxqs = min(nxqs, num_online_cpus());
	neqs = min(neqs_per_lif, num_online_cpus());

try_again:
	/* interrupt usage:
	 *    1 for master lif adminq/notifyq
	 *    1 for each CPU for master lif TxRx queue pairs
	 *    whatever's left is for RDMA queues
	 */
	nintrs = 1 + nxqs + neqs;
	min_intrs = 2;  /* adminq + 1 TxRx queue pair */

	if (nintrs > dev_nintrs)
		goto try_fewer;

	err = ionic_bus_alloc_irq_vectors(ionic, nintrs);
	if (err < 0 && err != -ENOSPC) {
		dev_err(ionic->dev, "Can't get intrs from OS: %d\n", err);
		return err;
	}
	if (err == -ENOSPC)
		goto try_fewer;

	if (err != nintrs) {
		ionic_bus_free_irq_vectors(ionic);
		goto try_fewer;
	}

	ionic->nnqs_per_lif = nnqs_per_lif;
	ionic->neqs_per_lif = neqs;
	ionic->ntxqs_per_lif = nxqs;
	ionic->nrxqs_per_lif = nxqs;
	ionic->nintrs = nintrs;

	ionic_debugfs_add_sizes(ionic);

	return 0;

try_fewer:
	if (nnqs_per_lif > 1) {
		nnqs_per_lif >>= 1;
		goto try_again;
	}
	if (neqs > 1) {
		neqs >>= 1;
		goto try_again;
	}
	if (nxqs > 1) {
		nxqs >>= 1;
		goto try_again;
	}
	dev_err(ionic->dev, "Can't get minimum %d intrs from OS\n", min_intrs);
	return -ENOSPC;
}
