/**
 ******************************************************************************
 *
 * rwnx_cmds.c
 *
 * Handles queueing (push to IPC, ack/cfm from IPC) of commands issued to
 * LMAC FW
 *
 * Copyright (C) RivieraWaves 2014-2019
 *
 ******************************************************************************
 */

#include <linux/list.h>

#include "rwnx_cmds.h"
#include "rwnx_defs.h"
#include "rwnx_strs.h"
//#define CREATE_TRACE_POINTS
#include "rwnx_events.h"
#include "aicwf_txrxif.h"
#include "aicwf_pcie.h"
#ifdef AICWF_SDIO_SUPPORT
#include "aicwf_sdio.h"
#else
#include "aicwf_usb.h"
#endif
/**
 *
 */
extern int aicwf_sdio_writeb(struct aic_sdio_dev *sdiodev, uint regaddr, u8 val);

void rwnx_cmd_free(struct rwnx_cmd *cmd);

static void cmd_dump(const struct rwnx_cmd *cmd)
{
	AICWFDBG(LOGERROR, KERN_CRIT "tkn[%d]  flags:%04x  result:%3d  cmd:%4d-%-24s - reqcfm(%4d-%-s)\n",
		   cmd->tkn, cmd->flags, cmd->result, cmd->id, RWNX_ID2STR(cmd->id),
		   cmd->reqid, cmd->reqid != (lmac_msg_id_t)-1 ? RWNX_ID2STR(cmd->reqid) : "none");
}

/**
 *
 */
static void cmd_complete(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	//RWNX_DBG(RWNX_FN_ENTRY_STR);
	lockdep_assert_held(&cmd_mgr->lock);

	//list_del(&cmd->list);
	//cmd_mgr->queue_sz--;

	cmd->flags |= RWNX_CMD_FLAG_DONE;
	if (cmd->flags & RWNX_CMD_FLAG_NONBLOCK) {
		rwnx_cmd_free(cmd);//kfree(cmd);
	} else {
		if (RWNX_CMD_WAIT_COMPLETE(cmd->flags)) {
			cmd->result = 0;
			complete(&cmd->complete);
		}
	}
}

int cmd_mgr_queue_force_defer(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	bool defer_push = false;

	RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
	trace_msg_send(cmd->id);
#endif
	spin_lock_bh(&cmd_mgr->lock);

	if (cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED) {
		AICWFDBG(LOGERROR, KERN_CRIT"cmd queue crashed\n");
		cmd->result = -EPIPE;
		spin_unlock_bh(&cmd_mgr->lock);
		return -EPIPE;
	}

	#ifndef CONFIG_RWNX_FHOST
	if (!list_empty(&cmd_mgr->cmds)) {
		if (cmd_mgr->queue_sz == cmd_mgr->max_queue_sz) {
			AICWFDBG(LOGERROR, KERN_CRIT"Too many cmds (%d) already queued\n",
				   cmd_mgr->max_queue_sz);
			cmd->result = -ENOMEM;
			spin_unlock_bh(&cmd_mgr->lock);
			return -ENOMEM;
		}
	}
	#endif

	cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
	defer_push = true;

	if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
		cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;

	cmd->tkn    = cmd_mgr->next_tkn++;
	cmd->result = -EINTR;

	if (!(cmd->flags & RWNX_CMD_FLAG_NONBLOCK))
		init_completion(&cmd->complete);

	list_add_tail(&cmd->list, &cmd_mgr->cmds);
	cmd_mgr->queue_sz++;
	spin_unlock_bh(&cmd_mgr->lock);

	WAKE_CMD_WORK(cmd_mgr);
	return 0;
}

static int cmd_mgr_queue(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	int err = 0;
#ifdef AICWF_SDIO_SUPPORT
	int ret;
	struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = sdiodev->rwnx_hw;
#endif
#ifdef AICWF_USB_SUPPORT
	struct aic_usb_dev *usbdev = container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = usbdev->rwnx_hw;
#endif
#ifdef AICWF_PCIE_SUPPORT
	struct aic_pci_dev *pciedev = container_of(cmd_mgr, struct aic_pci_dev, cmd_mgr);
	//struct rwnx_hw *rwnx_hw = pciedev->rwnx_hw;
#endif

	bool defer_push = false;
	u8_l empty = 0;

	//RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
	trace_msg_send(cmd->id);
#endif
	if(cmd->e2a_msg != NULL) {
		do {
			if(cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED)
				break;
			spin_lock_bh(&cmd_mgr->lock);
			empty = list_empty(&cmd_mgr->cmds);
			if(!empty) {
				spin_unlock_bh(&cmd_mgr->lock);
				if(in_softirq()) {
					AICWFDBG(LOGERROR, "in_softirq:check cmdqueue empty\n");
					mdelay(10);
				} else {
					AICWFDBG(LOGERROR, "check cmdqueue empty\n");
					msleep(50);
				}
			}
		} while(!empty);//wait for cmd queue empty
	} else {
		spin_lock_bh(&cmd_mgr->lock);
	}

	if (cmd_mgr->state == RWNX_CMD_MGR_STATE_CRASHED) {
		AICWFDBG(LOGERROR, KERN_CRIT"cmd queue crashed\n");
		cmd->result = -EPIPE;
		spin_unlock_bh(&cmd_mgr->lock);
		return -EPIPE;
	}

	#ifndef CONFIG_RWNX_FHOST
	if (!list_empty(&cmd_mgr->cmds)) {
		struct rwnx_cmd *last;

		if (cmd_mgr->queue_sz == cmd_mgr->max_queue_sz) {
			AICWFDBG(LOGERROR, KERN_CRIT"Too many cmds (%d) already queued\n",
				   cmd_mgr->max_queue_sz);
			cmd->result = -ENOMEM;
			spin_unlock_bh(&cmd_mgr->lock);
			return -ENOMEM;
		}
		last = list_entry(cmd_mgr->cmds.prev, struct rwnx_cmd, list);
		if (last->flags & (RWNX_CMD_FLAG_WAIT_ACK | RWNX_CMD_FLAG_WAIT_PUSH | RWNX_CMD_FLAG_WAIT_CFM)) {
#if 0 // queue even NONBLOCK command.
			if (cmd->flags & RWNX_CMD_FLAG_NONBLOCK) {
				printk(KERN_CRIT"cmd queue busy\n");
				cmd->result = -EBUSY;
				spin_unlock_bh(&cmd_mgr->lock);
				return -EBUSY;
			}
#endif
			cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
			defer_push = true;
		}
	}
	#endif
#ifdef CONFIG_WS
	rwnx_pm_stay_awake_pc(rwnx_hw);
#endif
#if 0
	cmd->flags |= RWNX_CMD_FLAG_WAIT_ACK;
#endif
	if (cmd->flags & RWNX_CMD_FLAG_REQ_CFM)
		cmd->flags |= RWNX_CMD_FLAG_WAIT_CFM;

	cmd->tkn    = cmd_mgr->next_tkn++;
	cmd->result = -EINTR;

	if (!(cmd->flags & RWNX_CMD_FLAG_NONBLOCK))
		init_completion(&cmd->complete);

	list_add_tail(&cmd->list, &cmd_mgr->cmds);
	cmd_mgr->queue_sz++;

	if (cmd->a2e_msg->id == ME_TRAFFIC_IND_REQ
	#ifdef AICWF_ARP_OFFLOAD
		|| cmd->a2e_msg->id == MM_SET_ARPOFFLOAD_REQ
	#endif
	) {
		defer_push = true;
		cmd->flags |= RWNX_CMD_FLAG_WAIT_PUSH;
		//printk("defer push: tkn=%d\r\n", cmd->tkn);
	}

	spin_unlock_bh(&cmd_mgr->lock);
	if (!defer_push) {
		//printk("queue:id=%x, param_len=%u\n",cmd->a2e_msg->id, cmd->a2e_msg->param_len);
		#ifdef AICWF_SDIO_SUPPORT
		aicwf_set_cmd_tx((void *)(sdiodev), cmd->a2e_msg, sizeof(struct lmac_msg) + cmd->a2e_msg->param_len);
		#endif
		#ifdef AICWF_USB_SUPPORT
		aicwf_set_cmd_tx((void *)(usbdev), cmd->a2e_msg, sizeof(struct lmac_msg) + cmd->a2e_msg->param_len);
		#endif
		#ifdef AICWF_PCIE_SUPPORT
        aicwf_set_cmd_tx((void *)(pciedev), cmd->a2e_msg, sizeof(struct lmac_msg) + cmd->a2e_msg->param_len);
        #endif
		//rwnx_ipc_msg_push(rwnx_hw, cmd, RWNX_CMD_A2EMSG_LEN(cmd->a2e_msg));
		kfree(cmd->a2e_msg);
	} else {
        if(cmd_mgr->queue_sz <= 1)
			WAKE_CMD_WORK(cmd_mgr);
#ifdef CONFIG_WS
		rwnx_pm_relax_pc(rwnx_hw);
#endif
		return 0;
	}

	if (!(cmd->flags & RWNX_CMD_FLAG_NONBLOCK)) {
#ifdef CONFIG_RWNX_FHOST
		if (wait_for_completion_killable(&cmd->complete)) {
			cmd->result = -EINTR;
			spin_lock_bh(&cmd_mgr->lock);
			cmd_complete(cmd_mgr, cmd);
			spin_unlock_bh(&cmd_mgr->lock);
			/* TODO: kill the cmd at fw level */
		}
#else
		unsigned long tout = msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS * cmd_mgr->queue_sz);
		if (!wait_for_completion_timeout(&cmd->complete, tout)) {
			AICWFDBG(LOGERROR, KERN_CRIT"cmd timed-out\n");
		#ifdef AICWF_SDIO_SUPPORT
			ret = aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.wakeup_reg, 2);
			if (ret < 0) {
				sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.wakeup_reg);
			}
		#endif

			cmd_dump(cmd);
			spin_lock_bh(&cmd_mgr->lock);
			cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
			if (!(cmd->flags & RWNX_CMD_FLAG_DONE)) {
				cmd->result = -ETIMEDOUT;
				cmd_complete(cmd_mgr, cmd);
			}
			err = -ETIMEDOUT;
			spin_unlock_bh(&cmd_mgr->lock);
		} else {
			spin_lock_bh(&cmd_mgr->lock);
			list_del(&cmd->list);
			cmd_mgr->queue_sz--;
			spin_unlock_bh(&cmd_mgr->lock);
			rwnx_cmd_free(cmd);//kfree(cmd);
			if (!list_empty(&cmd_mgr->cmds))
				WAKE_CMD_WORK(cmd_mgr);
		}
#endif
	} else {
		cmd->result = 0;
	}
#ifdef CONFIG_WS
	rwnx_pm_relax_pc(rwnx_hw);
#endif
	return err;
}

/**
 *
 */
static int cmd_mgr_llind(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd *cmd)
{
	struct rwnx_cmd *cur, *acked = NULL, *next = NULL;

	RWNX_DBG(RWNX_FN_ENTRY_STR);

	spin_lock_bh(&cmd_mgr->lock);
	list_for_each_entry(cur, &cmd_mgr->cmds, list) {
		if (!acked) {
			if (cur->tkn == cmd->tkn) {
				if (WARN_ON_ONCE(cur != cmd)) {
					cmd_dump(cmd);
				}
				acked = cur;
				continue;
			}
		}
		if (cur->flags & RWNX_CMD_FLAG_WAIT_PUSH) {
				next = cur;
				break;
		}
	}
	if (!acked) {
		AICWFDBG(LOGERROR, KERN_CRIT "Error: acked cmd not found\n");
	} else {
		cmd->flags &= ~RWNX_CMD_FLAG_WAIT_ACK;
		if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
			cmd_complete(cmd_mgr, cmd);
	}

	if (next) {
	#if 0 //there is no ack
		struct rwnx_hw *rwnx_hw = container_of(cmd_mgr, struct rwnx_hw, cmd_mgr);
		next->flags &= ~RWNX_CMD_FLAG_WAIT_PUSH;
		rwnx_ipc_msg_push(rwnx_hw, next, RWNX_CMD_A2EMSG_LEN(next->a2e_msg));
		kfree(next->a2e_msg);
	#endif
	}
	spin_unlock(&cmd_mgr->lock);

	return 0;
}

void cmd_mgr_task_process(struct work_struct *work)
{
	struct rwnx_cmd_mgr *cmd_mgr = container_of(work, struct rwnx_cmd_mgr, cmdWork);
	struct rwnx_cmd *cur, *next = NULL;
	unsigned long tout;
#ifdef AICWF_SDIO_SUPPORT
	struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = sdiodev->rwnx_hw;
#endif
#ifdef AICWF_USB_SUPPORT
	struct aic_usb_dev *usbdev = container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = usbdev->rwnx_hw;
#endif
#ifdef AICWF_PCIE_SUPPORT
	struct aic_pci_dev *pcidev = container_of(cmd_mgr, struct aic_pci_dev, cmd_mgr);
	//struct rwnx_hw *rwnx_hw = pcidev->rwnx_hw;
#endif

	RWNX_DBG(RWNX_FN_ENTRY_STR);

	while (1) {
		next = NULL;
		spin_lock_bh(&cmd_mgr->lock);

		list_for_each_entry(cur, &cmd_mgr->cmds, list) {
			if (cur->flags & RWNX_CMD_FLAG_WAIT_PUSH) { //just judge the first
					next = cur;
			}
			break;
		}
		spin_unlock_bh(&cmd_mgr->lock);

		if (next == NULL)
			break;

		if (next) {
			next->flags &= ~RWNX_CMD_FLAG_WAIT_PUSH;

			//printk("cmd_process, cmd->id=%d, tkn=%d\r\n",next->reqid, next->tkn);
			//rwnx_ipc_msg_push(rwnx_hw, next, RWNX_CMD_A2EMSG_LEN(next->a2e_msg));
#ifdef AICWF_SDIO_SUPPORT
			aicwf_set_cmd_tx((void *)(sdiodev), next->a2e_msg, sizeof(struct lmac_msg) + next->a2e_msg->param_len);
#endif
#ifdef AICWF_USB_SUPPORT
			aicwf_set_cmd_tx((void *)(usbdev), next->a2e_msg, sizeof(struct lmac_msg) + next->a2e_msg->param_len);
#endif
#ifdef AICWF_PCIE_SUPPORT
			aicwf_set_cmd_tx((void *)(pcidev), next->a2e_msg, sizeof(struct lmac_msg) + next->a2e_msg->param_len);
#endif

			kfree(next->a2e_msg);

			tout = msecs_to_jiffies(RWNX_80211_CMD_TIMEOUT_MS * cmd_mgr->queue_sz);
#ifdef CONFIG_WS
			rwnx_pm_stay_awake_pc(rwnx_hw);
#endif
			if (!wait_for_completion_timeout(&next->complete, tout)) {
				AICWFDBG(LOGERROR, KERN_CRIT"cmd timed-out\n");
#ifdef AICWF_SDIO_SUPPORT
				if (aicwf_sdio_writeb(sdiodev, sdiodev->sdio_reg.wakeup_reg, 2) < 0) {
					sdio_err("reg:%d write failed!\n", sdiodev->sdio_reg.wakeup_reg);
				}
#endif
				cmd_dump(next);
				spin_lock_bh(&cmd_mgr->lock);
				cmd_mgr->state = RWNX_CMD_MGR_STATE_CRASHED;
				if (!(next->flags & RWNX_CMD_FLAG_DONE)) {
					next->result = -ETIMEDOUT;
					cmd_complete(cmd_mgr, next);
				}
				spin_unlock_bh(&cmd_mgr->lock);
#ifdef CONFIG_WS
				rwnx_pm_relax_pc(rwnx_hw);
#endif
			} else {
				spin_lock_bh(&cmd_mgr->lock);
				list_del(&next->list);
				cmd_mgr->queue_sz--;
				spin_unlock_bh(&cmd_mgr->lock);
				rwnx_cmd_free(next);//kfree(next);
#ifdef CONFIG_WS
				rwnx_pm_relax_pc(rwnx_hw);
#endif
			}
		}
	}

}


static int cmd_mgr_run_callback(struct rwnx_hw *rwnx_hw, struct rwnx_cmd *cmd,
								struct rwnx_cmd_e2amsg *msg, msg_cb_fct cb)
{
	int res;

	if (!cb) {
		return 0;
	}
	//RWNX_DBG(RWNX_FN_ENTRY_STR);
	//spin_lock_bh(&rwnx_hw->cb_lock);
	res = cb(rwnx_hw, cmd, msg);
	//spin_unlock_bh(&rwnx_hw->cb_lock);

	return res;
}

/**
 *

 */
static int cmd_mgr_msgind(struct rwnx_cmd_mgr *cmd_mgr, struct rwnx_cmd_e2amsg *msg,
						  msg_cb_fct cb)
{
#ifdef AICWF_SDIO_SUPPORT
	struct aic_sdio_dev *sdiodev = container_of(cmd_mgr, struct aic_sdio_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = sdiodev->rwnx_hw;
#endif
#ifdef AICWF_USB_SUPPORT
	struct aic_usb_dev *usbdev = container_of(cmd_mgr, struct aic_usb_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = usbdev->rwnx_hw;
#endif
#ifdef AICWF_PCIE_SUPPORT
	struct aic_pci_dev *pcidev = container_of(cmd_mgr, struct aic_pci_dev, cmd_mgr);
	struct rwnx_hw *rwnx_hw = pcidev->rwnx_hw;
#endif

	struct rwnx_cmd *cmd, *pos;
	bool found = false;

	// RWNX_DBG(RWNX_FN_ENTRY_STR);
#ifdef CREATE_TRACE_POINTS
	trace_msg_recv(msg->id);
#endif
	//printk("cmd->id=%x\n", msg->id);
	spin_lock_bh(&cmd_mgr->lock);
	list_for_each_entry_safe(cmd, pos, &cmd_mgr->cmds, list) {
		if (cmd->reqid == msg->id &&
			(cmd->flags & RWNX_CMD_FLAG_WAIT_CFM)) {

			if (!cmd_mgr_run_callback(rwnx_hw, cmd, msg, cb)) {
				found = true;
				cmd->flags &= ~RWNX_CMD_FLAG_WAIT_CFM;

				if (WARN((msg->param_len > RWNX_CMD_E2AMSG_LEN_MAX),
						 "Unexpect E2A msg len %d > %d\n", msg->param_len,
						 RWNX_CMD_E2AMSG_LEN_MAX)) {
					msg->param_len = RWNX_CMD_E2AMSG_LEN_MAX;
				}

				if (cmd->e2a_msg && msg->param_len)
					memcpy(cmd->e2a_msg, &msg->param, msg->param_len);

				if (RWNX_CMD_WAIT_COMPLETE(cmd->flags))
					cmd_complete(cmd_mgr, cmd);

				break;
			}
		}
	}
	spin_unlock_bh(&cmd_mgr->lock);

	if (!found)
		cmd_mgr_run_callback(rwnx_hw, NULL, msg, cb);

	return 0;
}

/**
 *
 */
static void cmd_mgr_print(struct rwnx_cmd_mgr *cmd_mgr)
{
	struct rwnx_cmd *cur;

	spin_lock_bh(&cmd_mgr->lock);
	RWNX_DBG("q_sz/max: %2d / %2d - next tkn: %d\n",
			 cmd_mgr->queue_sz, cmd_mgr->max_queue_sz,
			 cmd_mgr->next_tkn);
	list_for_each_entry(cur, &cmd_mgr->cmds, list) {
		cmd_dump(cur);
	}
	spin_unlock_bh(&cmd_mgr->lock);
}

static void cmd_mgr_drain(struct rwnx_cmd_mgr *cmd_mgr)
{
	struct rwnx_cmd *cur, *nxt;

	RWNX_DBG(RWNX_FN_ENTRY_STR);

	spin_lock_bh(&cmd_mgr->lock);
	list_for_each_entry_safe(cur, nxt, &cmd_mgr->cmds, list) {
		list_del(&cur->list);
		cmd_mgr->queue_sz--;
		if (!(cur->flags & RWNX_CMD_FLAG_NONBLOCK))
			complete(&cur->complete);
	}
	spin_unlock_bh(&cmd_mgr->lock);
}

void rwnx_cmd_mgr_init(struct rwnx_cmd_mgr *cmd_mgr)
{
	RWNX_DBG(RWNX_FN_ENTRY_STR);

	INIT_LIST_HEAD(&cmd_mgr->cmds);
	cmd_mgr->state = RWNX_CMD_MGR_STATE_INITED;
	spin_lock_init(&cmd_mgr->lock);
	cmd_mgr->max_queue_sz = RWNX_CMD_MAX_QUEUED;
	cmd_mgr->queue  = &cmd_mgr_queue;
	cmd_mgr->print  = &cmd_mgr_print;
	cmd_mgr->drain  = &cmd_mgr_drain;
	cmd_mgr->llind  = &cmd_mgr_llind;
	cmd_mgr->msgind = &cmd_mgr_msgind;

	INIT_WORK(&cmd_mgr->cmdWork, cmd_mgr_task_process);
	cmd_mgr->cmd_wq = create_singlethread_workqueue("cmd_wq");
	if (!cmd_mgr->cmd_wq) {
		txrx_err("insufficient memory to create cmd workqueue.\n");
		return;
	}
}

void rwnx_cmd_mgr_deinit(struct rwnx_cmd_mgr *cmd_mgr)
{
	cmd_mgr->print(cmd_mgr);
	cmd_mgr->drain(cmd_mgr);
	cmd_mgr->print(cmd_mgr);
	flush_workqueue(cmd_mgr->cmd_wq);
	destroy_workqueue(cmd_mgr->cmd_wq);
	memset(cmd_mgr, 0, sizeof(*cmd_mgr));
}

void aicwf_set_cmd_tx(void *dev, struct lmac_msg *msg, uint len)
{
	u8 *buffer = NULL;
	u16 index = 0;
	struct aicwf_bus *bus = NULL;
#ifdef AICWF_SDIO_SUPPORT
	struct aic_sdio_dev *sdiodev = (struct aic_sdio_dev *)dev;
	bus = sdiodev->bus_if;
#endif
#ifdef AICWF_USB_SUPPORT
	struct aic_usb_dev *usbdev = (struct aic_usb_dev *)dev;
	if (!usbdev->state) {
		AICWFDBG(LOGERROR, "down msg \n");
		return;
	}
	bus = usbdev->bus_if;
#endif
#ifdef AICWF_PCIE_SUPPORT
	struct aic_pci_dev *pciedev = (struct aic_pci_dev *)dev;
	bus = pciedev->bus_if;
#endif

	spin_lock_bh(&pciedev->txmsg_lock);
	buffer = bus->cmd_buf;

	memset(buffer, 0, CMD_BUF_MAX);
	buffer[0] = (len+4) & 0x00ff;
	buffer[1] = ((len+4) >> 8) &0x0f;
	buffer[2] = 0x11;
    /*if (sdiodev->chipid == PRODUCT_ID_AIC8801 || sdiodev->chipid == PRODUCT_ID_AIC8800DC ||
        sdiodev->chipid == PRODUCT_ID_AIC8800DW)
        buffer[3] = 0x0;
    else if (sdiodev->chipid == PRODUCT_ID_AIC8800D80)
	    buffer[3] = crc8_ponl_107(&buffer[0], 3); // crc8*/
#ifdef AICWF_SDIO_SUPPORT
    buffer[3] = crc8_ponl_107(&buffer[0], 3); // crc8
#endif
	index += 4;
	//there is a dummy word
	index += 4;

	//make sure little endian
	put_u16(&buffer[index], msg->id);
	index += 2;
	put_u16(&buffer[index], msg->dest_id);
	index += 2;
	put_u16(&buffer[index], msg->src_id);
	index += 2;
	put_u16(&buffer[index], msg->param_len);
	index += 2;
	memcpy(&buffer[index], (u8 *)msg->param, msg->param_len);

	aicwf_bus_txmsg(bus, buffer, len + 8);
	spin_unlock_bh(&pciedev->txmsg_lock);
}

