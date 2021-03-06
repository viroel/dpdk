/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright(c) 2018 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <rte_bus_pci.h>
#include <rte_bus_vdev.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_cryptodev.h>
#include <rte_cryptodev_pmd.h>
#include <rte_pci.h>
#include <rte_dev.h>
#include <rte_malloc.h>

#include "ccp_crypto.h"
#include "ccp_dev.h"
#include "ccp_pmd_private.h"

/**
 * Global static parameter used to find if CCP device is already initialized.
 */
static unsigned int ccp_pmd_init_done;
uint8_t ccp_cryptodev_driver_id;

static struct ccp_session *
get_ccp_session(struct ccp_qp *qp, struct rte_crypto_op *op)
{
	struct ccp_session *sess = NULL;

	if (op->sess_type == RTE_CRYPTO_OP_WITH_SESSION) {
		if (unlikely(op->sym->session == NULL))
			return NULL;

		sess = (struct ccp_session *)
			get_session_private_data(
				op->sym->session,
				ccp_cryptodev_driver_id);
	} else if (op->sess_type == RTE_CRYPTO_OP_SESSIONLESS) {
		void *_sess;
		void *_sess_private_data = NULL;

		if (rte_mempool_get(qp->sess_mp, &_sess))
			return NULL;
		if (rte_mempool_get(qp->sess_mp, (void **)&_sess_private_data))
			return NULL;

		sess = (struct ccp_session *)_sess_private_data;

		if (unlikely(ccp_set_session_parameters(sess,
							op->sym->xform) != 0)) {
			rte_mempool_put(qp->sess_mp, _sess);
			rte_mempool_put(qp->sess_mp, _sess_private_data);
			sess = NULL;
		}
		op->sym->session = (struct rte_cryptodev_sym_session *)_sess;
		set_session_private_data(op->sym->session,
					 ccp_cryptodev_driver_id,
					 _sess_private_data);
	}

	return sess;
}

static uint16_t
ccp_pmd_enqueue_burst(void *queue_pair, struct rte_crypto_op **ops,
		      uint16_t nb_ops)
{
	struct ccp_session *sess = NULL;
	struct ccp_qp *qp = queue_pair;
	struct ccp_queue *cmd_q;
	struct rte_cryptodev *dev = qp->dev;
	uint16_t i, enq_cnt = 0, slots_req = 0;

	if (nb_ops == 0)
		return 0;

	if (unlikely(rte_ring_full(qp->processed_pkts) != 0))
		return 0;

	for (i = 0; i < nb_ops; i++) {
		sess = get_ccp_session(qp, ops[i]);
		if (unlikely(sess == NULL) && (i == 0)) {
			qp->qp_stats.enqueue_err_count++;
			return 0;
		} else if (sess == NULL) {
			nb_ops = i;
			break;
		}
		slots_req += ccp_compute_slot_count(sess);
	}

	cmd_q = ccp_allot_queue(dev, slots_req);
	if (unlikely(cmd_q == NULL))
		return 0;

	enq_cnt = process_ops_to_enqueue(qp, ops, cmd_q, nb_ops, slots_req);
	qp->qp_stats.enqueued_count += enq_cnt;
	return enq_cnt;
}

static uint16_t
ccp_pmd_dequeue_burst(void *queue_pair, struct rte_crypto_op **ops,
		uint16_t nb_ops)
{
	struct ccp_qp *qp = queue_pair;
	uint16_t nb_dequeued = 0, i;

	nb_dequeued = process_ops_to_dequeue(qp, ops, nb_ops);

	/* Free session if a session-less crypto op */
	for (i = 0; i < nb_dequeued; i++)
		if (unlikely(ops[i]->sess_type ==
			     RTE_CRYPTO_OP_SESSIONLESS)) {
			rte_mempool_put(qp->sess_mp,
					ops[i]->sym->session);
			ops[i]->sym->session = NULL;
		}
	qp->qp_stats.dequeued_count += nb_dequeued;

	return nb_dequeued;
}

/*
 * The set of PCI devices this driver supports
 */
static struct rte_pci_id ccp_pci_id[] = {
	{
		RTE_PCI_DEVICE(0x1022, 0x1456), /* AMD CCP-5a */
	},
	{
		RTE_PCI_DEVICE(0x1022, 0x1468), /* AMD CCP-5b */
	},
	{.device_id = 0},
};

/** Remove ccp pmd */
static int
cryptodev_ccp_remove(struct rte_vdev_device *dev)
{
	const char *name;

	ccp_pmd_init_done = 0;
	name = rte_vdev_device_name(dev);
	if (name == NULL)
		return -EINVAL;

	RTE_LOG(INFO, PMD, "Closing ccp device %s on numa socket %u\n",
			name, rte_socket_id());

	return 0;
}

/** Create crypto device */
static int
cryptodev_ccp_create(const char *name,
		     struct rte_vdev_device *vdev,
		     struct rte_cryptodev_pmd_init_params *init_params)
{
	struct rte_cryptodev *dev;
	struct ccp_private *internals;
	uint8_t cryptodev_cnt = 0;

	if (init_params->name[0] == '\0')
		snprintf(init_params->name, sizeof(init_params->name),
				"%s", name);

	dev = rte_cryptodev_pmd_create(init_params->name,
				       &vdev->device,
				       init_params);
	if (dev == NULL) {
		CCP_LOG_ERR("failed to create cryptodev vdev");
		goto init_error;
	}

	cryptodev_cnt = ccp_probe_devices(ccp_pci_id);

	if (cryptodev_cnt == 0) {
		CCP_LOG_ERR("failed to detect CCP crypto device");
		goto init_error;
	}

	printf("CCP : Crypto device count = %d\n", cryptodev_cnt);
	dev->driver_id = ccp_cryptodev_driver_id;

	/* register rx/tx burst functions for data path */
	dev->dev_ops = ccp_pmd_ops;
	dev->enqueue_burst = ccp_pmd_enqueue_burst;
	dev->dequeue_burst = ccp_pmd_dequeue_burst;

	dev->feature_flags = RTE_CRYPTODEV_FF_SYMMETRIC_CRYPTO |
			RTE_CRYPTODEV_FF_HW_ACCELERATED |
			RTE_CRYPTODEV_FF_SYM_OPERATION_CHAINING;

	internals = dev->data->dev_private;

	internals->max_nb_qpairs = init_params->max_nb_queue_pairs;
	internals->max_nb_sessions = init_params->max_nb_sessions;
	internals->crypto_num_dev = cryptodev_cnt;

	return 0;

init_error:
	CCP_LOG_ERR("driver %s: %s() failed",
		    init_params->name, __func__);
	cryptodev_ccp_remove(vdev);

	return -EFAULT;
}

/** Probe ccp pmd */
static int
cryptodev_ccp_probe(struct rte_vdev_device *vdev)
{
	int rc = 0;
	const char *name;
	struct rte_cryptodev_pmd_init_params init_params = {
		"",
		sizeof(struct ccp_private),
		rte_socket_id(),
		CCP_PMD_MAX_QUEUE_PAIRS,
		RTE_CRYPTODEV_PMD_DEFAULT_MAX_NB_SESSIONS
	};
	const char *input_args;

	if (ccp_pmd_init_done) {
		RTE_LOG(INFO, PMD, "CCP PMD already initialized\n");
		return -EFAULT;
	}
	name = rte_vdev_device_name(vdev);
	if (name == NULL)
		return -EINVAL;

	input_args = rte_vdev_device_args(vdev);
	rte_cryptodev_pmd_parse_input_args(&init_params, input_args);
	init_params.max_nb_queue_pairs = CCP_PMD_MAX_QUEUE_PAIRS;

	RTE_LOG(INFO, PMD, "Initialising %s on NUMA node %d\n", name,
			init_params.socket_id);
	RTE_LOG(INFO, PMD, "Max number of queue pairs = %d\n",
			init_params.max_nb_queue_pairs);
	RTE_LOG(INFO, PMD, "Max number of sessions = %d\n",
			init_params.max_nb_sessions);

	rc = cryptodev_ccp_create(name, vdev, &init_params);
	if (rc)
		return rc;
	ccp_pmd_init_done = 1;
	return 0;
}

static struct rte_vdev_driver cryptodev_ccp_pmd_drv = {
	.probe = cryptodev_ccp_probe,
	.remove = cryptodev_ccp_remove
};

static struct cryptodev_driver ccp_crypto_drv;

RTE_PMD_REGISTER_VDEV(CRYPTODEV_NAME_CCP_PMD, cryptodev_ccp_pmd_drv);
RTE_PMD_REGISTER_PARAM_STRING(CRYPTODEV_NAME_CCP_PMD,
	"max_nb_queue_pairs=<int> max_nb_sessions=<int> socket_id=<int>");
RTE_PMD_REGISTER_CRYPTO_DRIVER(ccp_crypto_drv, cryptodev_ccp_pmd_drv.driver,
			       ccp_cryptodev_driver_id);
