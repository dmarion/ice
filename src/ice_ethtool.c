// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2018-2019, Intel Corporation. */

/* ethtool support for ice */

#include "ice.h"
#include "ice_ethtool.h"
#include "ice_lib.h"
#include "ice_fltr.h"
#include "ice_dcb_lib.h"
#include "ice_dcb_nl.h"

static int ice_q_stats_len(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	int stats_size, total_slen = 0;

#ifdef ADQ_PERF_COUNTERS
	/* Tx stats */
	stats_size = sizeof(struct ice_q_stats) +
		     sizeof(struct ice_ch_q_poll_stats) +
		     sizeof(struct ice_ch_tx_q_stats);
	total_slen += np->vsi->alloc_txq * (stats_size / sizeof(u64));

	/* Rx stats */
	stats_size = sizeof(struct ice_q_stats) +
		     sizeof(struct ice_ch_q_poll_stats) +
		     sizeof(struct ice_ch_rx_q_stats);
	total_slen += np->vsi->alloc_rxq * (stats_size / sizeof(u64));

	stats_size = sizeof(struct ice_q_vector_ch_stats);
	total_slen += np->vsi->alloc_rxq * (stats_size / sizeof(u64));
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV

	stats_size = sizeof(struct ice_q_stats);
	total_slen += (ICE_MAX_MACVLANS * 2) * (stats_size / sizeof(u64));
	/* the napi_poll_cnt isn't included in the MACVLAN stats so reduce
	 * the count by that many so the stats get printed correctly
	 */
	total_slen -= ICE_MAX_MACVLANS * 2;
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */
#else
	stats_size = sizeof(struct ice_q_stats);

	total_slen += np->vsi->alloc_txq * (stats_size / sizeof(u64));
	total_slen += np->vsi->alloc_rxq * (stats_size / sizeof(u64));
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
	total_slen += (ICE_MAX_MACVLANS * 2) * (stats_size / sizeof(u64));
	/* the napi_poll_cnt isn't included in the MACVLAN stats so reduce
	 * the count by that many so the stats get printed correctly
	 */
	total_slen -= ICE_MAX_MACVLANS * 2;
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */
#endif /* ADQ_PERF_COUNTERS */

	return total_slen;
}

#define ICE_PF_STATS_LEN	ARRAY_SIZE(ice_gstrings_pf_stats)
#define ICE_VSI_STATS_LEN	ARRAY_SIZE(ice_gstrings_vsi_stats)

#define ICE_PFC_STATS_LEN ( \
		(sizeof_field(struct ice_pf, stats.priority_xoff_rx) + \
		 sizeof_field(struct ice_pf, stats.priority_xon_rx) + \
		 sizeof_field(struct ice_pf, stats.priority_xoff_tx) + \
		 sizeof_field(struct ice_pf, stats.priority_xon_tx)) \
		 / sizeof(u64))
#define ICE_ALL_STATS_LEN(n)	(ICE_PF_STATS_LEN + ICE_PFC_STATS_LEN + \
				 ICE_VSI_STATS_LEN + ice_q_stats_len(n))

static const struct ice_stats ice_gstrings_vsi_stats[] = {
	ICE_VSI_STAT(ICE_RX_UNICAST, eth_stats.rx_unicast),
	ICE_VSI_STAT(ICE_TX_UNICAST, eth_stats.tx_unicast),
	ICE_VSI_STAT(ICE_RX_MULTICAST, eth_stats.rx_multicast),
	ICE_VSI_STAT(ICE_TX_MULTICAST, eth_stats.tx_multicast),
	ICE_VSI_STAT(ICE_RX_BROADCAST, eth_stats.rx_broadcast),
	ICE_VSI_STAT(ICE_TX_BROADCAST, eth_stats.tx_broadcast),
	ICE_VSI_STAT(ICE_RX_BYTES, eth_stats.rx_bytes),
	ICE_VSI_STAT(ICE_TX_BYTES, eth_stats.tx_bytes),
	ICE_VSI_STAT(ICE_RX_DROPPED, eth_stats.rx_discards),
	ICE_VSI_STAT(ICE_RX_UNKNOWN_PROTO, eth_stats.rx_unknown_protocol),
	ICE_VSI_STAT(ICE_RX_ALLOC_FAIL, rx_buf_failed),
	ICE_VSI_STAT(ICE_RX_PAGE_ALLOC_FAIL, rx_page_failed),
	ICE_VSI_STAT(ICE_RX_GRO_DROPPED, rx_gro_dropped),
#ifdef ICE_ADD_PROBES
	ICE_VSI_STAT(ICE_RX_PAGE_REUSE, rx_page_reuse),
#endif /* ICE_ADD_PROBES */
	ICE_VSI_STAT(ICE_TX_ERRORS, eth_stats.tx_errors),
	ICE_VSI_STAT(ICE_TX_LINEARIZE, tx_linearize),
	ICE_VSI_STAT(ICE_TX_BUSY, tx_busy),
	ICE_VSI_STAT(ICE_TX_RESTART, tx_restart),
#ifdef ADQ_PERF_COUNTERS
	ICE_VSI_STAT("chnl_trans_inline_fd", cnt_inline_fd_transition),
	ICE_VSI_STAT("chnl_fd_table_flushed", cnt_table_flushed),
	ICE_VSI_STAT("chnl_fd_table_full", cnt_tbl_full),
#endif /* ADQ_PERF_COUNTERS */
};

enum ice_ethtool_test_id {
	ICE_ETH_TEST_REG = 0,
	ICE_ETH_TEST_EEPROM,
	ICE_ETH_TEST_INTR,
	ICE_ETH_TEST_LOOP,
	ICE_ETH_TEST_LINK,
};

static const char ice_gstrings_test[][ETH_GSTRING_LEN] = {
	"Register test  (offline)",
	"EEPROM test    (offline)",
	"Interrupt test (offline)",
	"Loopback test  (offline)",
	"Link test   (on/offline)",
};

#define ICE_TEST_LEN (sizeof(ice_gstrings_test) / ETH_GSTRING_LEN)

/* These PF_STATs might look like duplicates of some NETDEV_STATs,
 * but they aren't. This device is capable of supporting multiple
 * VSIs/netdevs on a single PF. The NETDEV_STATs are for individual
 * netdevs whereas the PF_STATs are for the physical function that's
 * hosting these netdevs.
 *
 * The PF_STATs are appended to the netdev stats only when ethtool -S
 * is queried on the base PF netdev.
 */
static const struct ice_stats ice_gstrings_pf_stats[] = {
	ICE_PF_STAT(ICE_PORT_RX_BYTES, stats.eth.rx_bytes),
	ICE_PF_STAT(ICE_PORT_TX_BYTES, stats.eth.tx_bytes),
	ICE_PF_STAT(ICE_PORT_RX_UNICAST, stats.eth.rx_unicast),
	ICE_PF_STAT(ICE_PORT_TX_UNICAST, stats.eth.tx_unicast),
	ICE_PF_STAT(ICE_PORT_RX_MULTICAST, stats.eth.rx_multicast),
	ICE_PF_STAT(ICE_PORT_TX_MULTICAST, stats.eth.tx_multicast),
	ICE_PF_STAT(ICE_PORT_RX_BROADCAST, stats.eth.rx_broadcast),
	ICE_PF_STAT(ICE_PORT_TX_BROADCAST, stats.eth.tx_broadcast),
	ICE_PF_STAT(ICE_PORT_TX_ERRORS, stats.eth.tx_errors),
	ICE_PF_STAT(ICE_PORT_TX_TIMEOUT, tx_timeout_count),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_64, stats.rx_size_64),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_64, stats.tx_size_64),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_127, stats.rx_size_127),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_127, stats.tx_size_127),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_255, stats.rx_size_255),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_255, stats.tx_size_255),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_511, stats.rx_size_511),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_511, stats.tx_size_511),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_1023, stats.rx_size_1023),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_1023, stats.tx_size_1023),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_1522, stats.rx_size_1522),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_1522, stats.tx_size_1522),
	ICE_PF_STAT(ICE_PORT_RX_SIZE_JUMBO, stats.rx_size_big),
	ICE_PF_STAT(ICE_PORT_TX_SIZE_JUMBO, stats.tx_size_big),
	ICE_PF_STAT(ICE_PORT_RX_LINK_XON, stats.link_xon_rx),
	ICE_PF_STAT(ICE_PORT_TX_LINK_XON, stats.link_xon_tx),
	ICE_PF_STAT(ICE_PORT_RX_LINK_XOFF, stats.link_xoff_rx),
	ICE_PF_STAT(ICE_PORT_TX_LINK_XOFF, stats.link_xoff_tx),
	ICE_PF_STAT(ICE_PORT_TX_DROP_LINK_DOWN, stats.tx_dropped_link_down),
	ICE_PF_STAT(ICE_PORT_RX_UNDERSIZE, stats.rx_undersize),
	ICE_PF_STAT(ICE_PORT_RX_FRAGMENTS, stats.rx_fragments),
	ICE_PF_STAT(ICE_PORT_RX_OVERSIZE, stats.rx_oversize),
	ICE_PF_STAT(ICE_PORT_RX_JABBER, stats.rx_jabber),
	ICE_PF_STAT(ICE_PORT_RX_CSUM_BAD, hw_csum_rx_error),
	ICE_PF_STAT(ICE_PORT_RX_LEN_ERRORS, stats.rx_len_errors),
	ICE_PF_STAT(ICE_PORT_RX_DROPPED, stats.eth.rx_discards),
	ICE_PF_STAT(ICE_PORT_RX_CRC_ERRORS, stats.crc_errors),
	ICE_PF_STAT(ICE_PORT_ILLEGAL_BYTES, stats.illegal_bytes),
	ICE_PF_STAT(ICE_PORT_MAC_LOCAL_FAULTS, stats.mac_local_faults),
	ICE_PF_STAT(ICE_PORT_MAC_REMOTE_FAULTS, stats.mac_remote_faults),
#ifdef ICE_ADD_PROBES
	ICE_PF_STAT(ICE_PORT_TX_TCP_SEGMENTS, tcp_segs),
	ICE_PF_STAT(ICE_PORT_TX_UDP_SEGMENTS, udp_segs),
	ICE_PF_STAT(ICE_PORT_RX_TCP_CSO, rx_tcp_cso),
	ICE_PF_STAT(ICE_PORT_TX_TCP_CSO, tx_tcp_cso),
	ICE_PF_STAT(ICE_PORT_RX_UDP_CSO, rx_udp_cso),
	ICE_PF_STAT(ICE_PORT_TX_UDP_CSO, tx_udp_cso),
	ICE_PF_STAT(ICE_PORT_RX_SCTP_CSO, rx_sctp_cso),
	ICE_PF_STAT(ICE_PORT_TX_SCTP_CSO, tx_sctp_cso),
	ICE_PF_STAT(ICE_PORT_RX_IP4_CSO, rx_ip4_cso),
	ICE_PF_STAT(ICE_PORT_TX_IP4_CSO, tx_ip4_cso),
	ICE_PF_STAT(ICE_PORT_RX_IP4_CSO_ERROR, rx_ip4_cso_err),
	ICE_PF_STAT(ICE_PORT_RX_IP4_CSO_ERROR, rx_tcp_cso_err),
	ICE_PF_STAT(ICE_PORT_RX_UDP_CSO_ERROR, rx_udp_cso_err),
	ICE_PF_STAT(ICE_PORT_RX_SCTP_CSO_ERROR, rx_sctp_cso_err),
	ICE_PF_STAT(ICE_PORT_TX_L3_CSO_ERROR, tx_l3_cso_err),
	ICE_PF_STAT(ICE_PORT_TX_L4_CSO_ERROR, tx_l4_cso_err),
	ICE_PF_STAT(ICE_PORT_RX_VLANO, rx_vlano),
	ICE_PF_STAT(ICE_PORT_TX_VLANO, tx_vlano),
#endif
	/* LPI stats */
	ICE_PF_STAT(ICE_PORT_RX_LPI_STATUS, stats.rx_lpi_status),
	ICE_PF_STAT(ICE_PORT_TX_LPI_STATUS, stats.tx_lpi_status),
	ICE_PF_STAT(ICE_PORT_RX_LPI_COUNT, stats.rx_lpi_count),
	ICE_PF_STAT(ICE_PORT_TX_LPI_COUNT, stats.tx_lpi_count),
	ICE_PF_STAT(ICE_PORT_FDIR_SB_MATCH, stats.fd_sb_match),
	ICE_PF_STAT(ICE_PORT_FDIR_SB_STATUS, stats.fd_sb_status),
	ICE_PF_STAT("chnl_inline_fd_match", stats.ch_atr_match),
#ifdef ICE_ADD_PROBES
	ICE_PF_STAT(ICE_PORT_ARFS_TCPV4_MATCH, stats.arfs_tcpv4_match),
	ICE_PF_STAT(ICE_PORT_ARFS_TCPV6_MATCH, stats.arfs_tcpv6_match),
	ICE_PF_STAT(ICE_PORT_ARFS_UDP4_MATCH, stats.arfs_udpv4_match),
	ICE_PF_STAT(ICE_PORT_ARFS_UDP6_MATCH, stats.arfs_udpv6_match),
#endif /* ICE_ADD_PROBES */
};

static const u32 ice_regs_dump_list[] = {
	PFGEN_STATE,
	PRTGEN_STATUS,
	QRX_CTRL(0),
	QINT_TQCTL(0),
	QINT_RQCTL(0),
	PFINT_OICR_ENA,
	QRX_ITR(0),
};

struct ice_priv_flag {
	char name[ETH_GSTRING_LEN];
	u32 bitno;			/* bit position in pf->flags */
};

#define ICE_PRIV_FLAG(_name, _bitno) { \
	.name = _name, \
	.bitno = _bitno, \
}

static const struct ice_priv_flag ice_gstrings_priv_flags[] = {
	ICE_PRIV_FLAG("link-down-on-close", ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA),
#ifndef ETHTOOL_GFECPARAM
	ICE_PRIV_FLAG("rs-fec", ICE_FLAG_RS_FEC),
	ICE_PRIV_FLAG("base-r-fec", ICE_FLAG_BASE_R_FEC),
#endif /* !ETHTOOL_GFECPARAM */
	ICE_PRIV_FLAG("fw-lldp-agent", ICE_FLAG_FW_LLDP_AGENT),
#ifdef NETIF_F_HW_TC
	ICE_PRIV_FLAG("channel-inline-flow-director",
		      ICE_FLAG_CHNL_INLINE_FD_ENA),
	ICE_PRIV_FLAG("channel-pkt-inspect-optimize",
		      ICE_FLAG_CHNL_PKT_INSPECT_OPT_ENA),
	ICE_PRIV_FLAG("channel-pkt-clean-bp-stop",
		      ICE_FLAG_CHNL_PKT_CLEAN_BP_STOP_ENA),
	ICE_PRIV_FLAG("channel-pkt-clean-bp-stop-cfg",
		      ICE_FLAG_CHNL_PKT_CLEAN_BP_STOP_CFG),
#endif /* NETIF_F_HW_TC */
	ICE_PRIV_FLAG("vf-true-promisc-support",
		      ICE_FLAG_VF_TRUE_PROMISC_ENA),
	ICE_PRIV_FLAG("mdd-auto-reset-vf", ICE_FLAG_MDD_AUTO_RESET_VF),
	ICE_PRIV_FLAG("legacy-rx", ICE_FLAG_LEGACY_RX),
};

#define ICE_PRIV_FLAG_ARRAY_SIZE	ARRAY_SIZE(ice_gstrings_priv_flags)

static void
ice_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *drvinfo)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = ice_get_netdev_priv_vsi(np);
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	struct ice_orom_info *orom;
	struct ice_nvm_info *nvm;

	nvm = &hw->flash.nvm;
	orom = &hw->flash.orom;

	strscpy(drvinfo->driver, KBUILD_MODNAME, sizeof(drvinfo->driver));
	strscpy(drvinfo->version, ice_drv_ver, sizeof(drvinfo->version));

	/* Display NVM version (from which the firmware version can be
	 * determined) which contains more pertinent information.
	 */
	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "%x.%02x 0x%x %d.%d.%d", nvm->major, nvm->minor,
		 nvm->eetrack, orom->major, orom->build, orom->patch);

	/* When called via 'ethtool -i|--driver <iface>', log the above with
	 * additional Netlist version information as a kernel message since it
	 * will not all fit in the 32-byte fixed-length buffer.
	 */
	if (!strncmp(current->comm, "ethtool", 7)) {
		struct ice_netlist_info *netlist = &hw->flash.netlist;

		/* The netlist versions are stored in packed BCD format */
		netdev_info(netdev, "NVM version details - %x.%02x, 0x%x, %x.%x.%x-%x.%x.%x.%08x, %d.%d.%d\n",
			    nvm->major, nvm->minor, nvm->eetrack,
			    netlist->major, netlist->minor,
			    netlist->type >> 16, netlist->type & 0xffff,
			    netlist->rev, netlist->cust_ver, netlist->hash,
			    orom->major, orom->build, orom->patch);
	}

	strscpy(drvinfo->bus_info, pci_name(pf->pdev),
		sizeof(drvinfo->bus_info));

	if (test_bit(ICE_RECOVERY_MODE, pf->state))
		return;

	drvinfo->n_priv_flags = ICE_PRIV_FLAG_ARRAY_SIZE;
}

static int ice_get_regs_len(struct net_device __always_unused *netdev)
{
	return sizeof(ice_regs_dump_list);
}

static void
ice_get_regs(struct net_device *netdev, struct ethtool_regs *regs, void *p)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;
	struct ice_hw *hw = &pf->hw;
	u32 *regs_buf = (u32 *)p;
	unsigned int i;

	regs->version = 1;

	for (i = 0; i < ARRAY_SIZE(ice_regs_dump_list); ++i)
		regs_buf[i] = rd32(hw, ice_regs_dump_list[i]);
}

static u32 ice_get_msglevel(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

#ifndef CONFIG_DYNAMIC_DEBUG
	if (pf->hw.debug_mask)
		netdev_info(netdev, "hw debug_mask: 0x%llX\n",
			    pf->hw.debug_mask);
#endif /* !CONFIG_DYNAMIC_DEBUG */

	return pf->msg_enable;
}

static void ice_set_msglevel(struct net_device *netdev, u32 data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

#ifndef CONFIG_DYNAMIC_DEBUG
	if (ICE_DBG_USER & data)
		pf->hw.debug_mask = data;
	else
		pf->msg_enable = data;
#else
	pf->msg_enable = data;
#endif /* !CONFIG_DYNAMIC_DEBUG */
}

static int ice_get_eeprom_len(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	/* Report the flash size, or at least 10MB */
	return max_t(int, pf->hw.flash.flash_size, 10 * 1024 * 1024);
}

static int
ice_get_eeprom(struct net_device *netdev, struct ethtool_eeprom *eeprom,
	       u8 *bytes)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	enum ice_status status;
	struct device *dev;
	int ret = 0;
	u32 magic;
	u8 *buf;

	dev = ice_pf_to_dev(pf);

	magic = hw->vendor_id | (hw->device_id << 16);
	if (eeprom->magic && eeprom->magic != magic) {
		struct ice_nvm_access_cmd *nvm;
		union ice_nvm_access_data *data;

		nvm = (struct ice_nvm_access_cmd *)eeprom;
		data = (union ice_nvm_access_data *)bytes;

		netdev_dbg(netdev, "GEEPROM config 0x%08x, offset 0x%08x, data_size 0x%08x\n",
			   nvm->config, nvm->offset, nvm->data_size);

		status = ice_handle_nvm_access(hw, nvm, data);

		ice_debug_array(hw, ICE_DBG_NVM, 16, 1, (u8 *)data,
				nvm->data_size);

		if (status) {
			int err = ice_status_to_errno(status);

			netdev_err(netdev, "NVM read offset 0x%x failed with status %s, error %d\n",
				   nvm->offset, ice_stat_str(status), err);

			return err;
		}

		return 0;
	}

	eeprom->magic = magic;
	netdev_dbg(netdev, "GEEPROM offset 0x%08x, len 0x%08x\n",
		   eeprom->offset, eeprom->len);

	buf = kzalloc(eeprom->len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	status = ice_acquire_nvm(hw, ICE_RES_READ);
	if (status) {
		dev_err(dev, "ice_acquire_nvm failed: %s %s\n",
			ice_stat_str(status),
			ice_aq_str(hw->adminq.sq_last_status));
		ret = -EIO;
		goto out;
	}

	status = ice_read_flat_nvm(hw, eeprom->offset, &eeprom->len, buf,
				   false);
	if (status) {
		dev_err(dev, "ice_read_flat_nvm failed: %s %s\n",
			ice_stat_str(status),
			ice_aq_str(hw->adminq.sq_last_status));
		ret = -EIO;
		goto release;
	}

	memcpy(bytes, buf, eeprom->len);
release:
	ice_release_nvm(hw);
out:
	kfree(buf);
	return ret;
}

static int
ice_set_eeprom(struct net_device *netdev, struct ethtool_eeprom *eeprom,
	       u8 *bytes)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_hw *hw = &np->vsi->back->hw;
	struct ice_pf *pf = np->vsi->back;
	union ice_nvm_access_data *data;
	struct ice_nvm_access_cmd *nvm;
	enum ice_status status = 0;
	int err = 0;
	u32 magic;

	/* normal ethtool set_eeprom is not supported */
	nvm = (struct ice_nvm_access_cmd *)eeprom;
	data = (union ice_nvm_access_data *)bytes;
	magic = hw->vendor_id | (hw->device_id << 16);

	netdev_dbg(netdev, "SEEPROM cmd 0x%08x, config 0x%08x, offset 0x%08x, data_size 0x%08x\n",
		   nvm->command, nvm->config, nvm->offset, nvm->data_size);
	ice_debug_array(hw, ICE_DBG_NVM, 16, 1, (u8 *)data, nvm->data_size);

	if (eeprom->magic == magic)
		err = -EOPNOTSUPP;
	/* check for NVM access method */
	else if (!eeprom->magic || (eeprom->magic >> 16) != hw->device_id)
		err = -EINVAL;
	else if (ice_is_reset_in_progress(pf->state))
		err = -EBUSY;
	else
		status = ice_handle_nvm_access(hw, nvm, data);

	if (status) {
		err = ice_status_to_errno(status);
		netdev_err(netdev, "NVM write offset 0x%x failed with status %s, error %d\n",
			   nvm->offset, ice_stat_str(status), err);
	}

	return err;
}

/**
 * ice_active_vfs - check if there are any active VFs
 * @pf: board private structure
 *
 * Returns true if an active VF is found, otherwise returns false
 */
static bool ice_active_vfs(struct ice_pf *pf)
{
	unsigned int i;

	ice_for_each_vf(pf, i) {
		struct ice_vf *vf = &pf->vf[i];

		if (test_bit(ICE_VF_STATE_ACTIVE, vf->vf_states))
			return true;
	}

	return false;
}

/**
 * ice_link_test - perform a link test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_link_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	enum ice_status status;
	bool link_up = false;

	netdev_info(netdev, "link test\n");
	status = ice_get_link_status(np->vsi->port_info, &link_up);
	if (status) {
		netdev_err(netdev, "link query error, status = %s\n",
			   ice_stat_str(status));
		return 1;
	}

	if (!link_up)
		return 2;

	return 0;
}

/**
 * ice_eeprom_test - perform an EEPROM test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_eeprom_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	netdev_info(netdev, "EEPROM test\n");
	return !!(ice_nvm_validate_checksum(&pf->hw));
}

/**
 * ice_reg_pattern_test
 * @hw: pointer to the HW struct
 * @reg: reg to be tested
 * @mask: bits to be touched
 */
static int ice_reg_pattern_test(struct ice_hw *hw, u32 reg, u32 mask)
{
	static const u32 patterns[] = {
		0x5A5A5A5A, 0xA5A5A5A5,
		0x00000000, 0xFFFFFFFF
	};
	struct ice_pf *pf = hw->back;
	struct device *dev;
	u32 val, orig_val;
	unsigned int i;

	dev = ice_pf_to_dev(pf);
	orig_val = rd32(hw, reg);
	for (i = 0; i < ARRAY_SIZE(patterns); ++i) {
		u32 pattern = patterns[i] & mask;

		wr32(hw, reg, pattern);
		val = rd32(hw, reg);
		if (val == pattern)
			continue;
		dev_err(dev, "%s: reg pattern test failed - reg 0x%08x pat 0x%08x val 0x%08x\n"
			, __func__, reg, pattern, val);
		return 1;
	}

	wr32(hw, reg, orig_val);
	val = rd32(hw, reg);
	if (val != orig_val) {
		dev_err(dev, "%s: reg restore test failed - reg 0x%08x orig 0x%08x val 0x%08x\n"
			, __func__, reg, orig_val, val);
		return 1;
	}

	return 0;
}

/**
 * ice_reg_test - perform a register test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_reg_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_hw *hw = np->vsi->port_info->hw;
	u32 int_elements = hw->func_caps.common_cap.num_msix_vectors ?
		hw->func_caps.common_cap.num_msix_vectors - 1 : 1;
	struct ice_diag_reg_test_info {
		u32 address;
		u32 mask;
		u32 elem_num;
		u32 elem_size;
	} ice_reg_list[] = {
		{GLINT_ITR(0, 0), 0x00000fff, int_elements,
			GLINT_ITR(0, 1) - GLINT_ITR(0, 0)},
		{GLINT_ITR(1, 0), 0x00000fff, int_elements,
			GLINT_ITR(1, 1) - GLINT_ITR(1, 0)},
		{GLINT_ITR(0, 0), 0x00000fff, int_elements,
			GLINT_ITR(2, 1) - GLINT_ITR(2, 0)},
		{GLINT_CTL, 0xffff0001, 1, 0}
	};
	unsigned int i;

	netdev_dbg(netdev, "Register test\n");
	for (i = 0; i < ARRAY_SIZE(ice_reg_list); ++i) {
		u32 j;

		for (j = 0; j < ice_reg_list[i].elem_num; ++j) {
			u32 mask = ice_reg_list[i].mask;
			u32 reg = ice_reg_list[i].address +
				(j * ice_reg_list[i].elem_size);

			/* bail on failure (non-zero return) */
			if (ice_reg_pattern_test(hw, reg, mask))
				return 1;
		}
	}

	return 0;
}

/**
 * ice_lbtest_prepare_rings - configure Tx/Rx test rings
 * @vsi: pointer to the VSI structure
 *
 * Function configures rings of a VSI for loopback test without
 * enabling interrupts or informing the kernel about new queues.
 *
 * Returns 0 on success, negative on failure.
 */
static int ice_lbtest_prepare_rings(struct ice_vsi *vsi)
{
	int status;

	status = ice_vsi_setup_tx_rings(vsi);
	if (status)
		goto err_setup_tx_ring;

	status = ice_vsi_setup_rx_rings(vsi);
	if (status)
		goto err_setup_rx_ring;

	status = ice_vsi_cfg(vsi);
	if (status)
		goto err_setup_rx_ring;

	status = ice_vsi_start_all_rx_rings(vsi);
	if (status)
		goto err_start_rx_ring;

	return status;

err_start_rx_ring:
	ice_vsi_free_rx_rings(vsi);
err_setup_rx_ring:
	ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, 0);
err_setup_tx_ring:
	ice_vsi_free_tx_rings(vsi);

	return status;
}

/**
 * ice_lbtest_disable_rings - disable Tx/Rx test rings after loopback test
 * @vsi: pointer to the VSI structure
 *
 * Function stops and frees VSI rings after a loopback test.
 * Returns 0 on success, negative on failure.
 */
static int ice_lbtest_disable_rings(struct ice_vsi *vsi)
{
	int status;

	status = ice_vsi_stop_lan_tx_rings(vsi, ICE_NO_RESET, 0);
	if (status)
		netdev_err(vsi->netdev, "Failed to stop Tx rings, VSI %d error %d\n",
			   vsi->vsi_num, status);

	status = ice_vsi_stop_all_rx_rings(vsi);
	if (status)
		netdev_err(vsi->netdev, "Failed to stop Rx rings, VSI %d error %d\n",
			   vsi->vsi_num, status);

	ice_vsi_free_tx_rings(vsi);
	ice_vsi_free_rx_rings(vsi);

	return status;
}

/**
 * ice_lbtest_create_frame - create test packet
 * @pf: pointer to the PF structure
 * @ret_data: allocated frame buffer
 * @size: size of the packet data
 *
 * Function allocates a frame with a test pattern on specific offsets.
 * Returns 0 on success, non-zero on failure.
 */
static int ice_lbtest_create_frame(struct ice_pf *pf, u8 **ret_data, u16 size)
{
	u8 *data;

	if (!pf)
		return -EINVAL;

	data = devm_kzalloc(ice_pf_to_dev(pf), size, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Since the ethernet test frame should always be at least
	 * 64 bytes long, fill some octets in the payload with test data.
	 */
	memset(data, 0xFF, size);
	data[32] = 0xDE;
	data[42] = 0xAD;
	data[44] = 0xBE;
	data[46] = 0xEF;

	*ret_data = data;

	return 0;
}

/**
 * ice_lbtest_check_frame - verify received loopback frame
 * @frame: pointer to the raw packet data
 *
 * Function verifies received test frame with a pattern.
 * Returns true if frame matches the pattern, false otherwise.
 */
static bool ice_lbtest_check_frame(u8 *frame)
{
	/* Validate bytes of a frame under offsets chosen earlier */
	if (frame[32] == 0xDE &&
	    frame[42] == 0xAD &&
	    frame[44] == 0xBE &&
	    frame[46] == 0xEF &&
	    frame[48] == 0xFF)
		return true;

	return false;
}

/**
 * ice_diag_send - send test frames to the test ring
 * @tx_ring: pointer to the transmit ring
 * @data: pointer to the raw packet data
 * @size: size of the packet to send
 *
 * Function sends loopback packets on a test Tx ring.
 */
static int ice_diag_send(struct ice_ring *tx_ring, u8 *data, u16 size)
{
	struct ice_tx_desc *tx_desc;
	struct ice_tx_buf *tx_buf;
	dma_addr_t dma;
	u64 td_cmd;

	tx_desc = ICE_TX_DESC(tx_ring, tx_ring->next_to_use);
	tx_buf = &tx_ring->tx_buf[tx_ring->next_to_use];

	dma = dma_map_single(tx_ring->dev, data, size, DMA_TO_DEVICE);
	if (dma_mapping_error(tx_ring->dev, dma))
		return -EINVAL;

	tx_desc->buf_addr = cpu_to_le64(dma);

	/* These flags are required for a descriptor to be pushed out */
	td_cmd = (u64)(ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS);
	tx_desc->cmd_type_offset_bsz =
		cpu_to_le64(ICE_TX_DESC_DTYPE_DATA |
			    (td_cmd << ICE_TXD_QW1_CMD_S) |
			    ((u64)0 << ICE_TXD_QW1_OFFSET_S) |
			    ((u64)size << ICE_TXD_QW1_TX_BUF_SZ_S) |
			    ((u64)0 << ICE_TXD_QW1_L2TAG1_S));

	tx_buf->next_to_watch = tx_desc;

	/* Force memory write to complete before letting h/w know
	 * there are new descriptors to fetch.
	 */
	wmb();

	tx_ring->next_to_use++;
	if (tx_ring->next_to_use >= tx_ring->count)
		tx_ring->next_to_use = 0;

	writel_relaxed(tx_ring->next_to_use, tx_ring->tail);

	/* Wait until the packets get transmitted to the receive queue. */
	usleep_range(1000, 2000);
	dma_unmap_single(tx_ring->dev, dma, size, DMA_TO_DEVICE);

	return 0;
}

#define ICE_LB_FRAME_SIZE 64
/**
 * ice_lbtest_receive_frames - receive and verify test frames
 * @rx_ring: pointer to the receive ring
 *
 * Function receives loopback packets and verify their correctness.
 * Returns number of received valid frames.
 */
static int ice_lbtest_receive_frames(struct ice_ring *rx_ring)
{
	struct ice_rx_buf *rx_buf;
	int valid_frames, i;
	u8 *received_buf;

	valid_frames = 0;

	for (i = 0; i < rx_ring->count; i++) {
		union ice_32b_rx_flex_desc *rx_desc;

		rx_desc = ICE_RX_DESC(rx_ring, i);

		if (!(rx_desc->wb.status_error0 &
		    cpu_to_le16(ICE_TX_DESC_CMD_EOP | ICE_TX_DESC_CMD_RS)))
			continue;

		rx_buf = &rx_ring->rx_buf[i];
		received_buf = page_address(rx_buf->page) + rx_buf->page_offset;

		if (ice_lbtest_check_frame(received_buf))
			valid_frames++;
	}

	return valid_frames;
}

/**
 * ice_loopback_test - perform a loopback test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_loopback_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *orig_vsi = np->vsi, *test_vsi;
	struct ice_pf *pf = orig_vsi->back;
	struct ice_ring *tx_ring, *rx_ring;
	u8 broadcast[ETH_ALEN], ret = 0;
	int num_frames, valid_frames;
	struct device *dev;
	u8 *tx_frame;
	int i;

	dev = ice_pf_to_dev(pf);
	netdev_info(netdev, "loopback test\n");

	test_vsi = ice_lb_vsi_setup(pf, pf->hw.port_info);
	if (!test_vsi) {
		netdev_err(netdev, "Failed to create a VSI for the loopback test\n");
		return 1;
	}

	test_vsi->netdev = netdev;
	tx_ring = test_vsi->tx_rings[0];
	rx_ring = test_vsi->rx_rings[0];

	if (ice_lbtest_prepare_rings(test_vsi)) {
		ret = 2;
		goto lbtest_vsi_close;
	}

	if (ice_alloc_rx_bufs(rx_ring, rx_ring->count)) {
		ret = 3;
		goto lbtest_rings_dis;
	}

	/* Enable MAC loopback in firmware */
	if (ice_aq_set_mac_loopback(&pf->hw, true, NULL)) {
		ret = 4;
		goto lbtest_mac_dis;
	}

	/* Test VSI needs to receive broadcast packets */
	eth_broadcast_addr(broadcast);
	if (ice_fltr_add_mac(test_vsi, broadcast, ICE_FWD_TO_VSI)) {
		ret = 5;
		goto lbtest_mac_dis;
	}

	if (ice_lbtest_create_frame(pf, &tx_frame, ICE_LB_FRAME_SIZE)) {
		ret = 7;
		goto remove_mac_filters;
	}

	num_frames = min_t(int, tx_ring->count, 32);
	for (i = 0; i < num_frames; i++) {
		if (ice_diag_send(tx_ring, tx_frame, ICE_LB_FRAME_SIZE)) {
			ret = 8;
			goto lbtest_free_frame;
		}
	}

	valid_frames = ice_lbtest_receive_frames(rx_ring);
	if (!valid_frames)
		ret = 9;
	else if (valid_frames != num_frames)
		ret = 10;

lbtest_free_frame:
	devm_kfree(dev, tx_frame);
remove_mac_filters:
	if (ice_fltr_remove_mac(test_vsi, broadcast, ICE_FWD_TO_VSI))
		netdev_err(netdev, "Could not remove MAC filter for the test VSI\n");
lbtest_mac_dis:
	/* Disable MAC loopback after the test is completed. */
	if (ice_aq_set_mac_loopback(&pf->hw, false, NULL))
		netdev_err(netdev, "Could not disable MAC loopback\n");
lbtest_rings_dis:
	if (ice_lbtest_disable_rings(test_vsi))
		netdev_err(netdev, "Could not disable test rings\n");
lbtest_vsi_close:
	test_vsi->netdev = NULL;
	if (ice_vsi_release(test_vsi))
		netdev_err(netdev, "Failed to remove the test VSI\n");

	return ret;
}

/**
 * ice_intr_test - perform an interrupt test on a given net_device
 * @netdev: network interface device structure
 *
 * This function performs one of the self-tests required by ethtool.
 * Returns 0 on success, non-zero on failure.
 */
static u64 ice_intr_test(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;
	u16 swic_old = pf->sw_int_count;

	netdev_info(netdev, "interrupt test\n");

	wr32(&pf->hw, GLINT_DYN_CTL(pf->oicr_idx),
	     GLINT_DYN_CTL_SW_ITR_INDX_M |
	     GLINT_DYN_CTL_INTENA_MSK_M |
	     GLINT_DYN_CTL_SWINT_TRIG_M);

	usleep_range(1000, 2000);
	return (swic_old == pf->sw_int_count);
}

/**
 * ice_self_test - handler function for performing a self-test by ethtool
 * @netdev: network interface device structure
 * @eth_test: ethtool_test structure
 * @data: required by ethtool.self_test
 *
 * This function is called after invoking 'ethtool -t devname' command where
 * devname is the name of the network device on which ethtool should operate.
 * It performs a set of self-tests to check if a device works properly.
 */
static void
ice_self_test(struct net_device *netdev, struct ethtool_test *eth_test,
	      u64 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	bool if_running = netif_running(netdev);
	struct ice_pf *pf = np->vsi->back;
	struct device *dev;

	dev = ice_pf_to_dev(pf);

	if (eth_test->flags == ETH_TEST_FL_OFFLINE) {
		netdev_info(netdev, "offline testing starting\n");

		set_bit(ICE_TESTING, pf->state);

		if (ice_active_vfs(pf) || ice_active_vmdqs(pf)) {
			dev_warn(dev, "Please take active VFs and Netqueues offline and restart the adapter before running NIC diagnostics\n");
			data[ICE_ETH_TEST_REG] = 1;
			data[ICE_ETH_TEST_EEPROM] = 1;
			data[ICE_ETH_TEST_INTR] = 1;
			data[ICE_ETH_TEST_LOOP] = 1;
			data[ICE_ETH_TEST_LINK] = 1;
			eth_test->flags |= ETH_TEST_FL_FAILED;
			clear_bit(ICE_TESTING, pf->state);
			goto skip_ol_tests;
		}
		/* If the device is online then take it offline */
		if (if_running)
			/* indicate we're in test mode */
			ice_stop(netdev);

		data[ICE_ETH_TEST_LINK] = ice_link_test(netdev);
		data[ICE_ETH_TEST_EEPROM] = ice_eeprom_test(netdev);
		data[ICE_ETH_TEST_INTR] = ice_intr_test(netdev);
		data[ICE_ETH_TEST_LOOP] = ice_loopback_test(netdev);
		data[ICE_ETH_TEST_REG] = ice_reg_test(netdev);

		if (data[ICE_ETH_TEST_LINK] ||
		    data[ICE_ETH_TEST_EEPROM] ||
		    data[ICE_ETH_TEST_LOOP] ||
		    data[ICE_ETH_TEST_INTR] ||
		    data[ICE_ETH_TEST_REG])
			eth_test->flags |= ETH_TEST_FL_FAILED;

		clear_bit(ICE_TESTING, pf->state);

		if (if_running) {
			int status = ice_open(netdev);

			if (status) {
				dev_err(dev, "Could not open device %s, err %d\n",
					pf->int_name, status);
			}
		}
	} else {
		/* Online tests */
		netdev_info(netdev, "online testing starting\n");

		data[ICE_ETH_TEST_LINK] = ice_link_test(netdev);
		if (data[ICE_ETH_TEST_LINK])
			eth_test->flags |= ETH_TEST_FL_FAILED;

		/* Offline only tests, not run in online; pass by default */
		data[ICE_ETH_TEST_REG] = 0;
		data[ICE_ETH_TEST_EEPROM] = 0;
		data[ICE_ETH_TEST_INTR] = 0;
		data[ICE_ETH_TEST_LOOP] = 0;
	}

skip_ol_tests:
	netdev_info(netdev, "testing finished\n");
}

#ifdef ADQ_PERF_COUNTERS
/**
 * ice_get_chnl_tx_strings
 * @vsi: ptr to VSI
 * @q:  queue index
 * @loc_in_buf: ptr to ptr to location in buffer (input and output param)
 *
 * This function returns Tx queue related strings for ADQ performance counters
 */
static void
ice_get_chnl_tx_strings(struct ice_vsi *vsi, unsigned int q, char **loc_in_buf)
{
	char *p;

	if (!loc_in_buf)
		return;
	if (q >= vsi->num_txq)
		return;

	p = *loc_in_buf;

	/* Tx queue specific extra counters */
	snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_BUSY_POLL, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_NOT_BUSY_POLL, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_ATR_SETUP, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_ATR_TEARDOWN, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_ATR_BAIL, q);
	p += ETH_GSTRING_LEN;

	/* copy back updated length */
	*loc_in_buf = p;
}

/**
 * ice_get_chnl_tx_stats - get stats for Tx rings if channel enabled
 * @vsi: ptr to VSI
 * @q:  queue index
 * @data: ptr to data
 * @idx: ptr to idx in data buffer (input/output param)
 * @set: copy counters if true otherwise copy zero
 *
 * This function is used to collect performance counters for specific Tx ring.
 */
static void
ice_get_chnl_tx_stats(struct ice_vsi *vsi, int q, u64 *data, int *idx, bool set)
{
	struct ice_ch_q_stats *ch_stats;
	struct ice_ring *tx_ring;
	int i;

	if (!idx)
		return;
	if (q >= vsi->num_txq)
		return;

	tx_ring = vsi->tx_rings[q];
	ch_stats = &tx_ring->ch_q_stats;
	set = set && ch_stats;

	i = *idx; /* start index in data buffer */

	/* Tx queue specific extra counters */
	data[i++] = set ? ch_stats->poll.bp_packets : 0;
	data[i++] = set ? ch_stats->poll.np_packets : 0;
	data[i++] = set ? ch_stats->tx.num_atr_setup : 0;
	data[i++] = set ? ch_stats->tx.num_atr_evict : 0;
	data[i++] = set ? ch_stats->tx.num_atr_bailouts : 0;

	/* copy back updated index */
	*idx = i;
}

/**
 * ice_get_chnl_rx_stats - get stats for Rx rings if channel enabled
 * @vsi: ptr to VSI
 * @q:  queue index
 * @data: ptr to data
 * @idx: ptr to idx in data buffer (input/output param)
 * @set: copy counters if true otherwise copy zero
 *
 * This function is used to collect performance counters for specific Rx ring
 * and related vector. All these counters are related to ADQ.
 */
static void
ice_get_chnl_rx_stats(struct ice_vsi *vsi, int q, u64 *data, int *idx, bool set)
{
	struct ice_q_vector_ch_stats *vector_ch_stats = NULL;
	struct ice_ch_q_stats *ch_stats;
	struct ice_ring *rx_ring;
	bool orig_set = set;
	int i;

	if (!idx)
		return;
	if (q >= vsi->num_rxq)
		return;

	rx_ring = vsi->rx_rings[q];
	ch_stats = &rx_ring->ch_q_stats;
	if (rx_ring->q_vector)
		vector_ch_stats = &rx_ring->q_vector->ch_stats;

	i = *idx; /* start index in data buffer */

	/* Rx queue specific extra counters */
	set = orig_set && ch_stats;
	/* busy_poll and not busy_poll packets */
	data[i++] = set ? ch_stats->poll.bp_packets : 0;
	data[i++] = set ? ch_stats->poll.np_packets : 0;
	/* Rx queue set/bailout from override */
	data[i++] = set ? ch_stats->rx.num_rx_queue_set : 0;
	data[i++] = set ? ch_stats->rx.num_rx_queue_bailouts : 0;
	/* ctrl pkts, only ctrl_pkts,, FIN/RST/SYN */
	data[i++] = set ? ch_stats->rx.num_tcp_ctrl_pkts : 0;
	data[i++] = set ? ch_stats->rx.num_only_ctrl_pkts : 0;
	data[i++] = set ? ch_stats->rx.num_tcp_flags_fin : 0;
	data[i++] = set ? ch_stats->rx.num_tcp_flags_rst : 0;
	data[i++] = set ? ch_stats->rx.num_tcp_flags_syn : 0;
	/* BP: no data packets cleaned */
	data[i++] = set ? ch_stats->rx.num_no_data_pkt_bp : 0;

	/* vector specific extra counters */
	set = orig_set && vector_ch_stats;
	/* state machine */
	data[i++] = set ? vector_ch_stats->in_bp : 0;
	data[i++] = set ? vector_ch_stats->real_int_to_bp : 0;
	data[i++] = set ? vector_ch_stats->real_bp_to_bp : 0;
	data[i++] = set ? vector_ch_stats->in_int : 0;
	data[i++] = set ? vector_ch_stats->real_bp_to_int : 0;
	data[i++] = set ? vector_ch_stats->real_int_to_int : 0;
	/* unlikely_cb_to_bp, once_in_bp */
	data[i++] = set ? vector_ch_stats->unlikely_cb_to_bp : 0;
	data[i++] = set ? vector_ch_stats->ucb_o_bp : 0;
	data[i++] = set ? vector_ch_stats->once_bp_false : 0;
	/* Busypoll stop due to either need_resched() or possible timeout */
	data[i++] = set ? vector_ch_stats->num_need_resched_bp_stop : 0;
	data[i++] = set ? vector_ch_stats->num_timeout_bp_stop : 0;
	/* Busypoll->Interrupt, last time "cleaned data packets" */
	data[i++] = set ? vector_ch_stats->cleaned_any_data_pkt : 0;
	/* need_resched() and !cleaned data packets */
	data[i++] = set ? vector_ch_stats->num_l_c_data_pkt : 0;
	/* possible timeout and !cleaned data packets */
	data[i++] = set ? vector_ch_stats->num_l_c_data_pkt1 : 0;
	/* software triggered omterrupt either from napi_poll based
	 * on channel specific heuristic or from service_task
	 */
	data[i++] = set ? vector_ch_stats->num_sw_intr_timeout : 0;
	data[i++] = set ? vector_ch_stats->num_sw_intr_serv_task : 0;
	/* times, SW triggered interrupt were not fired */
	data[i++] = set ? vector_ch_stats->num_no_sw_intr_opt_off : 0;
	/* number of times WB_ON_ITR is set */
	data[i++] = set ? vector_ch_stats->num_wb_on_itr_set : 0;

	/* number of Rx packets processed when busy_poll_stop is invoked */
	data[i++] = set ? vector_ch_stats->pkt_bp_stop_bp_budget : 0;

	/* number of Rx packets processed when napi_schedule is invoked because
	 * busy_poll_stop:napi_poll returned budget
	 */
	data[i++] = set ? vector_ch_stats->pkt_bp_stop_napi_budget : 0;

	/* num of times work_done == budget from busy_poll_stop code path */
	data[i++] = set ? vector_ch_stats->bp_wd_equals_budget8 : 0;

	/* num of times work_done == budget from napi_shedule which gets invoked
	 * if busy_poll_stop:napi_poll returned "budget"
	 */
	data[i++] = set ? vector_ch_stats->bp_wd_equals_budget64 : 0;

	/* how many times, kept internal state to be in BUSY_POLL
	 * when napi_poll is invoked due to busy_poll_stop
	 */
	data[i++] = set ? vector_ch_stats->keep_state_bp_budget8 : 0;

	/* how many times, kept internal state to be in BUSY_POLL
	 * when napi_poll is invoked due to napi_schedule.
	 */
	data[i++] = set ? vector_ch_stats->keep_state_bp_budget64 : 0;

	/* copy back updated index */
	*idx = i;
}

/**
 * ice_get_chnl_rx_strings
 * @vsi: ptr to VSI
 * @q:  queue index
 * @loc_in_buf: ptr to ptr to location in buffer (input and output param)
 *
 * This function returns Rx queue and vector related strings for
 * ADQ performance counters
 */
static void
ice_get_chnl_rx_strings(struct ice_vsi *vsi, unsigned int q, char **loc_in_buf)
{
	char *p;

	if (!loc_in_buf)
		return;
	if (q >= vsi->num_rxq)
		return;

	p = *loc_in_buf;

	/* Rx queue specific extra counters */

	/* busy and non-busy poll packets */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BUSY_POLL, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_NOT_BUSY_POLL, q);
	p += ETH_GSTRING_LEN;
	/* number of times Rx queue was set thru' Rx queue override logic */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_SET, q);
	p += ETH_GSTRING_LEN;
	/* number of times Rx queue was not set thru' Rx queue override logic */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BAIL, q);
	p += ETH_GSTRING_LEN;
	/* total TCP ctrl pkts */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_TCP_CTRL_PKTS, q);
	p += ETH_GSTRING_LEN;
	/* total "only ctrl pkts" */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_ONLY_CTRL_PKTS, q);
	p += ETH_GSTRING_LEN;
	/* number of FIN recv */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_TCP_FIN_RECV, q);
	p += ETH_GSTRING_LEN;
	/* number of RST recv */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_TCP_RST_RECV, q);
	p += ETH_GSTRING_LEN;
	/* number of SYN recv */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_TCP_SYN_RECV, q);
	p += ETH_GSTRING_LEN;
	/* BP, but didn't clean any data packets */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_NO_DATA_PKT, q);
	p += ETH_GSTRING_LEN;

	/* Vector specific extra counters */

	/* tracking BP, INT, BP->INT, INT->BP */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_IN_BP, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_INTR_TO_BP, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_TO_BP, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_IN_INTR, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_TO_INTR, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_INTR_TO_INTR, q);
	p += ETH_GSTRING_LEN;
	/* unlikely comeback to busy_poll */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_UNLIKELY_CB_TO_BP, q);
	p += ETH_GSTRING_LEN;
	/* unlikely comeback to busy_poll and once_in_bp is true */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_UCB_ONCE_IN_BP, q);
	p += ETH_GSTRING_LEN;
	/* once_in_bp is false */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_INTR_ONCE_IN_BP_FALSE, q);
	p += ETH_GSTRING_LEN;
	/* busy_poll stop due to need_resched() */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_STOP_NEED_RESCHED, q);
	p += ETH_GSTRING_LEN;
	/* busy_poll stop due to possible due to timeout */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_STOP_TIMEOUT, q);
	p += ETH_GSTRING_LEN;
	/* Transition: BP->INT: previously cleaned data packets */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_CLEANED_ANY_DATA_PKT, q);
	p += ETH_GSTRING_LEN;
	/* need_resched(), but didn't clean any data packets */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_NEED_RESCHED_NO_DATA, q);
	p += ETH_GSTRING_LEN;
	/* possible timeout(), but didn't clean any data packets */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_TIMEOUT_NO_DATA, q);
	p += ETH_GSTRING_LEN;
	/* number of SW triggered interrupt from napi_poll due to
	 * possible timeout detected
	 */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_SW_INTR_TIMEOUT, q);
	p += ETH_GSTRING_LEN;
	/* number of SW triggered interrupt from service_task */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_SW_INTR_SERV_TASK, q);
	p += ETH_GSTRING_LEN;
	/* number of times, SW triggered interrupt is not triggered from
	 * napi_poll even when unlikely_cb_to_bp is set, once_in_bp is set
	 * but ethtool private featute flag is off (for interrupt optimization
	 * strategy
	 */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_NO_SW_INTR_OPT_OFF, q);
	p += ETH_GSTRING_LEN;
	/* number of times WB_ON_ITR is set */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_WB_ON_ITR_SET, q);
	p += ETH_GSTRING_LEN;

	/* number of Rx packet processed due busy_poll_stop */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_PKTS_BP_STOP_BUDGET8, q);
	p += ETH_GSTRING_LEN;

	/* number of Rx packet processed due to napi_schedule which gets invoked
	 * if busy_poll_stop returned budget
	 */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_PKTS_BP_STOP_BUDGET64, q);
	p += ETH_GSTRING_LEN;

	/* num of times work_done == budget condition met from
	 * busy_poll_stop:napi_poll code path
	 */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_WD_EQUAL_BUDGET8, q);
	p += ETH_GSTRING_LEN;

	/* num of times work_done == budget condition met from
	 * napi_schedule:napi_poll code path (this happens if busy_poll_stop
	 * returned "budget")
	 */
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BP_WD_EQUAL_BUDGET64, q);
	p += ETH_GSTRING_LEN;

	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_KEEP_STATE_BP_BUDGET8, q);
	p += ETH_GSTRING_LEN;
	snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_KEEP_STATE_BP_BUDGET64, q);
	p += ETH_GSTRING_LEN;

	/* copy back updated length */
	*loc_in_buf = p;
}
#endif /* ADQ_PERF_COUNTERS */

#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
/**
 * ice_get_macvlan
 * @id: macvlan ID
 * @pf: pointer to the PF structure
 *
 * Returns the MACVLAN matching the provided ID
 */
static struct ice_macvlan *ice_get_macvlan(int id, struct ice_pf *pf)
{
	struct ice_macvlan *mv;

	/* If the ID is not marked as in use, no need to search */
	if (!(test_bit(id, pf->avail_macvlan)))
		return NULL;

	list_for_each_entry(mv, &pf->macvlan_list, list) {
		if (id == mv->id)
			return mv;
	}

	return NULL;
}

/**
 * ice_get_macvlan_tx_strings
 * @pf: pointer to the PF structure
 * @loc_in_buf: ptr to ptr to location in buffer (input and output param)
 *
 * This function returns Tx related strings for MACVLAN offload
 */
static void ice_get_macvlan_tx_strings(struct ice_pf *pf, char **loc_in_buf)
{
	char *p;
	int i;

	if (!loc_in_buf)
		return;
	p = *loc_in_buf;

	for (i = 0; i < ICE_MAX_MACVLANS; i++) {
		struct ice_macvlan *mv = ice_get_macvlan(i, pf);

		if (mv) {
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_TX_PKTS1,
				 mv->vdev->name);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_TX_BYTES1,
				 mv->vdev->name);
			p += ETH_GSTRING_LEN;
		} else {
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_TX_PKTS2, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_TX_BYTES2, i);
			p += ETH_GSTRING_LEN;
		}
	}

	/* copy back updated length */
	*loc_in_buf = p;
}

/**
 * ice_get_macvlan_tx_stats
 * @pf: pointer to the PF structure
 * @data: ptr to data
 * @idx: ptr to idx in data buffer (input/output param)
 *
 * This function is used to collect Tx statistics for MACVLAN offload
 */
static void ice_get_macvlan_tx_stats(struct ice_pf *pf, u64 *data, int *idx)
{
	int i, j;

	if (!idx)
		return;
	j = *idx;

	for (i = 0; i < ICE_MAX_MACVLANS; i++) {
		struct ice_macvlan *mv;

		mv = ice_get_macvlan(i, pf);
		if (mv) {
			data[j++] = mv->vsi->net_stats.tx_packets;
			data[j++] = mv->vsi->net_stats.tx_bytes;
		} else {
			data[j++] = 0;
			data[j++] = 0;
		}
	}

	/* copy back updated index */
	*idx = j;
}

/**
 * ice_get_macvlan_rx_strings
 * @pf: pointer to the PF structure
 * @loc_in_buf: ptr to ptr to location in buffer (input and output param)
 *
 * This function returns Rx related strings for MACVLAN offload
 */
static void ice_get_macvlan_rx_strings(struct ice_pf *pf, char **loc_in_buf)
{
	char *p;
	int i;

	if (!loc_in_buf)
		return;
	p = *loc_in_buf;

	for (i = 0; i < ICE_MAX_MACVLANS; i++) {
		struct ice_macvlan *mv = ice_get_macvlan(i, pf);

		if (mv) {
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_RX_PKTS1,
				 mv->vdev->name);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_RX_BYTES1,
				 mv->vdev->name);
			p += ETH_GSTRING_LEN;
		} else {
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_RX_PKTS2, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, L2_FWD_RX_BYTES2, i);
			p += ETH_GSTRING_LEN;
		}
	}

	/* copy back updated length */
	*loc_in_buf = p;
}

/**
 * ice_get_macvlan_rx_stats
 * @pf: private board structure
 * @data: ptr to data
 * @idx: ptr to idx in data buffer (input/output param)
 *
 * This function is used to collect statistics for MACVLAN offload
 */
static void
ice_get_macvlan_rx_stats(struct ice_pf *pf, u64 *data, int *idx)
{
	int i, j;

	if (!idx)
		return;
	j = *idx;

	for (i = 0; i < ICE_MAX_MACVLANS; i++) {
		struct ice_macvlan *mv;

		mv = ice_get_macvlan(i, pf);
		if (mv) {
			data[j++] = mv->vsi->net_stats.rx_packets;
			data[j++] = mv->vsi->net_stats.rx_bytes;
		} else {
			data[j++] = 0;
			data[j++] = 0;
		}
	}

	/* copy back updated index */
	*idx = j;
}
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */

static void ice_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = ice_get_netdev_priv_vsi(np);
	char *p = (char *)data;
	unsigned int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ICE_VSI_STATS_LEN; i++) {
			snprintf(p, ETH_GSTRING_LEN, "%s",
				 ice_gstrings_vsi_stats[i].stat_string);
			p += ETH_GSTRING_LEN;
		}

		ice_for_each_alloc_txq(vsi, i) {
			snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_PACKETS, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_BYTES, i);
			p += ETH_GSTRING_LEN;
#ifdef ICE_ADD_PROBES
			snprintf(p, ETH_GSTRING_LEN, ICE_TXQ_NAPI_POLL, i);
			p += ETH_GSTRING_LEN;
#endif /* ICE_ADD_PROBES */
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_tx_strings(vsi, i, &p);
#endif /* ADQ_PERF_COUNTERS */
		}
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
		ice_get_macvlan_tx_strings(vsi->back, &p);
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */

		ice_for_each_alloc_rxq(vsi, i) {
			snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_PACKETS, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_BYTES, i);
			p += ETH_GSTRING_LEN;
#ifdef ICE_ADD_PROBES
			snprintf(p, ETH_GSTRING_LEN, ICE_RXQ_NAPI_POLL, i);
			p += ETH_GSTRING_LEN;
#endif /* ICE_ADD_PROBES */
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_rx_strings(vsi, i, &p);
#endif /* ADQ_PERF_COUNTERS */
		}
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
		ice_get_macvlan_rx_strings(vsi->back, &p);
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */

		if (vsi->type != ICE_VSI_PF)
			return;

		for (i = 0; i < ICE_PF_STATS_LEN; i++) {
			snprintf(p, ETH_GSTRING_LEN, "%s",
				 ice_gstrings_pf_stats[i].stat_string);
			p += ETH_GSTRING_LEN;
		}

		for (i = 0; i < ICE_MAX_USER_PRIORITY; i++) {
			snprintf(p, ETH_GSTRING_LEN, PORT_TX_PRIO_XON, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, PORT_TX_PRIO_XOFF, i);
			p += ETH_GSTRING_LEN;
		}
		for (i = 0; i < ICE_MAX_USER_PRIORITY; i++) {
			snprintf(p, ETH_GSTRING_LEN, PORT_RX_PRIO_XON, i);
			p += ETH_GSTRING_LEN;
			snprintf(p, ETH_GSTRING_LEN, PORT_RX_PRIO_XOFF, i);
			p += ETH_GSTRING_LEN;
		}
		break;
	case ETH_SS_TEST:
		memcpy(data, ice_gstrings_test, ICE_TEST_LEN * ETH_GSTRING_LEN);
		break;
	case ETH_SS_PRIV_FLAGS:
		for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++) {
			snprintf(p, ETH_GSTRING_LEN, "%s",
				 ice_gstrings_priv_flags[i].name);
			p += ETH_GSTRING_LEN;
		}
		break;
	default:
		break;
	}
}

static int
ice_set_phys_id(struct net_device *netdev, enum ethtool_phys_id_state state)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	bool led_active;

	switch (state) {
	case ETHTOOL_ID_ACTIVE:
		led_active = true;
		break;
	case ETHTOOL_ID_INACTIVE:
		led_active = false;
		break;
	default:
		return -EINVAL;
	}

	if (ice_aq_set_port_id_led(np->vsi->port_info, !led_active, NULL))
		return -EIO;

	return 0;
}

/**
 * ice_set_fec_cfg - Set link FEC options
 * @netdev: network interface device structure
 * @req_fec: FEC mode to configure
 */
static int ice_set_fec_cfg(struct net_device *netdev, enum ice_fec_mode req_fec)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_set_phy_cfg_data config = { 0 };
	struct ice_vsi *vsi = np->vsi;
	struct ice_port_info *pi;

	pi = vsi->port_info;
	if (!pi)
		return -EOPNOTSUPP;

	/* Changing the FEC parameters is not supported if not the PF VSI */
	if (vsi->type != ICE_VSI_PF) {
		netdev_info(netdev, "Changing FEC parameters only supported for PF VSI\n");
		return -EOPNOTSUPP;
	}

	/* Proceed only if requesting different FEC mode */
	if (pi->phy.curr_user_fec_req == req_fec)
		return 0;

	/* Copy the current user PHY configuration. The current user PHY
	 * configuration is initialized during probe from PHY capabilities
	 * software mode, and updated on set PHY configuration.
	 */
	memcpy(&config, &pi->phy.curr_user_phy_cfg, sizeof(config));

	ice_cfg_phy_fec(pi, &config, req_fec);
	config.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	if (ice_aq_set_phy_cfg(pi->hw, pi, &config, NULL))
		return -EAGAIN;

	/* Save requested FEC config */
	pi->phy.curr_user_fec_req = req_fec;

	return 0;
}

#ifdef ETHTOOL_GFECPARAM
/**
 * ice_set_fecparam - Set FEC link options
 * @netdev: network interface device structure
 * @fecparam: Ethtool structure to retrieve FEC parameters
 */
static int
ice_set_fecparam(struct net_device *netdev, struct ethtool_fecparam *fecparam)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	enum ice_fec_mode fec;

	switch (fecparam->fec) {
	case ETHTOOL_FEC_AUTO:
		fec = ICE_FEC_AUTO;
		break;
	case ETHTOOL_FEC_RS:
		fec = ICE_FEC_RS;
		break;
	case ETHTOOL_FEC_BASER:
		fec = ICE_FEC_BASER;
		break;
	case ETHTOOL_FEC_OFF:
	case ETHTOOL_FEC_NONE:
		fec = ICE_FEC_NONE;
		break;
	default:
		dev_warn(ice_pf_to_dev(vsi->back), "Unsupported FEC mode: %d\n",
			 fecparam->fec);
		return -EINVAL;
	}

	return ice_set_fec_cfg(netdev, fec);
}

/**
 * ice_get_fecparam - Get link FEC options
 * @netdev: network interface device structure
 * @fecparam: Ethtool structure to retrieve FEC parameters
 */
static int
ice_get_fecparam(struct net_device *netdev, struct ethtool_fecparam *fecparam)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *caps;
	struct ice_link_status *link_info;
	struct ice_vsi *vsi = np->vsi;
	struct ice_port_info *pi;
	enum ice_status status;
	int err = 0;

	pi = vsi->port_info;

	if (!pi)
		return -EOPNOTSUPP;
	link_info = &pi->phy.link_info;

	/* Set FEC mode based on negotiated link info */
	switch (link_info->fec_info) {
	case ICE_AQ_LINK_25G_KR_FEC_EN:
		fecparam->active_fec = ETHTOOL_FEC_BASER;
		break;
	case ICE_AQ_LINK_25G_RS_528_FEC_EN:
	case ICE_AQ_LINK_25G_RS_544_FEC_EN:
		fecparam->active_fec = ETHTOOL_FEC_RS;
		break;
	default:
		fecparam->active_fec = ETHTOOL_FEC_OFF;
		break;
	}

	caps = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!caps)
		return -ENOMEM;

	status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_TOPO_CAP,
				     caps, NULL);
	if (status) {
		err = -EAGAIN;
		goto done;
	}

	/* Set supported/configured FEC modes based on PHY capability */
	if (caps->caps & ICE_AQC_PHY_EN_AUTO_FEC)
		fecparam->fec |= ETHTOOL_FEC_AUTO;
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_REQ)
		fecparam->fec |= ETHTOOL_FEC_BASER;
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_528_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_544_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN)
		fecparam->fec |= ETHTOOL_FEC_RS;
	if (caps->link_fec_options == 0)
		fecparam->fec |= ETHTOOL_FEC_OFF;

done:
	kfree(caps);
	return err;
}
#endif /* ETHTOOL_GFECPARAM */

/**
 * ice_nway_reset - restart autonegotiation
 * @netdev: network interface device structure
 */
static int ice_nway_reset(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	int err;

	/* If VSI state is up, then restart autoneg with link up */
	if (!test_bit(ICE_DOWN, vsi->back->state))
		err = ice_set_link(vsi, true);
	else
		err = ice_set_link(vsi, false);

	return err;
}

/**
 * ice_get_priv_flags - report device private flags
 * @netdev: network interface device structure
 *
 * The get string set count and the string set should be matched for each
 * flag returned. Add new strings for each flag to the ice_gstrings_priv_flags
 * array.
 *
 * Returns a u32 bitmap of flags.
 */
static u32 ice_get_priv_flags(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	u32 i, ret_flags = 0;

	for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++) {
		const struct ice_priv_flag *priv_flag;

		priv_flag = &ice_gstrings_priv_flags[i];

		if (test_bit(priv_flag->bitno, pf->flags))
			ret_flags |= BIT(i);
	}

	return ret_flags;
}

#ifdef NETIF_F_HW_TC
/**
 * ice_recfg_chnl_vsis - reconfig channel VSIs
 * @pf: ptr to PF
 * @vsi: ptr to main VSI
 *
 * This function adjust ADQ VSI's feature flags based on changes
 * in private flag setting - to avoid stale bits in per ADQ VSI's
 * feature flag.
 */
static void ice_recfg_chnl_vsis(struct ice_pf *pf, struct ice_vsi *vsi)
{
	struct ice_channel *ch;

	/* Nothing to be done if there is no active ADQ config */
	if (!ice_is_adq_active(pf))
		return;

	list_for_each_entry(ch, &vsi->ch_list, list) {
		struct ice_vsi *ch_vsi;

		ch_vsi = ch->ch_vsi;
		if (!ch_vsi)
			continue;
		/* set/clear VSI level feature flag for ADQ (aka channel) VSIs
		 * based on PF level private flags
		 */
		if (test_bit(ICE_FLAG_CHNL_PKT_INSPECT_OPT_ENA, pf->flags))
			set_bit(ICE_CHNL_FEATURE_PKT_INSPECT_OPT_ENA,
				ch_vsi->features);
		else
			clear_bit(ICE_CHNL_FEATURE_PKT_INSPECT_OPT_ENA,
				  ch_vsi->features);

		/* set/clear VSI level feature flag for ADQ (aka channel) VSIs
		 * based on PF level private flags: this flag meant to harvest
		 * clean of Rx queue upon busy_poll stop and after that clean
		 * once only.
		 */
		if (test_bit(ICE_FLAG_CHNL_PKT_CLEAN_BP_STOP_ENA, pf->flags))
			set_bit(ICE_CHNL_FEATURE_PKT_CLEAN_BP_STOP_ENA,
				ch_vsi->features);
		else
			clear_bit(ICE_CHNL_FEATURE_PKT_CLEAN_BP_STOP_ENA,
				  ch_vsi->features);

		/* set/clear inline flow-director bits for ADQ (aka channel)
		 * VSIs based on PF level private flags
		 */
		if (test_bit(ICE_FLAG_CHNL_INLINE_FD_ENA, pf->flags))
			set_bit(ICE_CHNL_FEATURE_INLINE_FD_ENA,
				ch_vsi->features);
		else
			clear_bit(ICE_CHNL_FEATURE_INLINE_FD_ENA,
				  ch_vsi->features);
	}
}

/**
 * ice_recfg_vsi - reconfig specified VSI
 * @pf: ptr to PF
 * @vsi: ptr to main VSI
 *
 * Set up per vector configurable param which allows cleanup of Tx and
 * Rx packets upto that many time if napi_schedule is invoked after
 * busy_poll_stop (where driver returned "budget") based on driver maintained
 * state for ADQ specific vector.
 */
static void ice_recfg_vsi(struct ice_pf *pf, struct ice_vsi *vsi)
{
	int q_vectors = vsi->num_q_vectors;
	int vector;

	if (!q_vectors)
		return;

	for (vector = 0; vector < q_vectors; vector++) {
		struct ice_q_vector *qv = vsi->q_vectors[vector];

		if (!qv)
			continue;
		if (test_bit(ICE_FLAG_CHNL_PKT_CLEAN_BP_STOP_CFG, pf->flags))
			qv->max_limit_process_rx_queues =
						ICE_MAX_LIMIT_PROCESS_RX_PKTS;
		else
			qv->max_limit_process_rx_queues =
					ICE_MAX_LIMIT_PROCESS_RX_PKTS_DFLT;
	}
}
#endif /* ADQ_SUPPORT */

/**
 * ice_set_priv_flags - set private flags
 * @netdev: network interface device structure
 * @flags: bit flags to be set
 */
static int ice_set_priv_flags(struct net_device *netdev, u32 flags)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	DECLARE_BITMAP(change_flags, ICE_PF_FLAGS_NBITS);
	DECLARE_BITMAP(orig_flags, ICE_PF_FLAGS_NBITS);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	int ret = 0;
	u32 i;

	if (flags > BIT(ICE_PRIV_FLAG_ARRAY_SIZE))
		return -EINVAL;

	dev = ice_pf_to_dev(pf);
	set_bit(ICE_FLAG_ETHTOOL_CTXT, pf->flags);

	bitmap_copy(orig_flags, pf->flags, ICE_PF_FLAGS_NBITS);
	for (i = 0; i < ICE_PRIV_FLAG_ARRAY_SIZE; i++) {
		const struct ice_priv_flag *priv_flag;

		priv_flag = &ice_gstrings_priv_flags[i];

		if (flags & BIT(i))
			set_bit(priv_flag->bitno, pf->flags);
		else
			clear_bit(priv_flag->bitno, pf->flags);
	}

#ifdef NETIF_F_HW_TC
	ice_recfg_chnl_vsis(pf, vsi);
	ice_recfg_vsi(pf, vsi);
#endif /* ADQ_SUPPORT */

	bitmap_xor(change_flags, pf->flags, orig_flags, ICE_PF_FLAGS_NBITS);

#ifndef ETHTOOL_GFECPARAM
	if (test_bit(ICE_FLAG_RS_FEC, change_flags) ||
	    test_bit(ICE_FLAG_BASE_R_FEC, change_flags)) {
		enum ice_fec_mode fec = ICE_FEC_NONE;
		int err;

		/* Check if FEC is supported */
		if (pf->hw.device_id != ICE_DEV_ID_E810C_BACKPLANE &&
		    pf->hw.device_id != ICE_DEV_ID_E810C_QSFP &&
		    pf->hw.device_id != ICE_DEV_ID_E810C_SFP) {
			dev_warn(dev, "Device does not support changing FEC configuration\n");
			ret = -EOPNOTSUPP;
			goto ethtool_exit;
		}

		/* Set FEC configuration */
		if (test_bit(ICE_FLAG_RS_FEC, pf->flags) &&
		    test_bit(ICE_FLAG_BASE_R_FEC, pf->flags))
			fec = ICE_FEC_AUTO;
		else if (test_bit(ICE_FLAG_RS_FEC, pf->flags))
			fec = ICE_FEC_RS;
		else if (test_bit(ICE_FLAG_BASE_R_FEC, pf->flags))
			fec = ICE_FEC_BASER;

		err = ice_set_fec_cfg(netdev, fec);

		/* If FEC configuration fails, restore original FEC flags */
		if (err) {
			if (test_bit(ICE_FLAG_BASE_R_FEC, orig_flags))
				set_bit(ICE_FLAG_BASE_R_FEC, pf->flags);
			else
				clear_bit(ICE_FLAG_BASE_R_FEC, pf->flags);

			if (test_bit(ICE_FLAG_RS_FEC, orig_flags))
				set_bit(ICE_FLAG_RS_FEC, pf->flags);
			else
				clear_bit(ICE_FLAG_RS_FEC, pf->flags);

			ret = err;
			goto ethtool_exit;
		}
	}
#endif /* !ETHTOOL_GFECPARAM */

	/* Do not allow change to link-down-on-close when Total Port Shutdown
	 * is enabled.
	 */
	if (test_bit(ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA, change_flags) &&
	    test_bit(ICE_FLAG_TOTAL_PORT_SHUTDOWN_ENA, pf->flags)) {
		dev_err(dev, "Setting link-down-on-close not supported on this port\n");
		set_bit(ICE_FLAG_LINK_DOWN_ON_CLOSE_ENA, pf->flags);
		ret = -EINVAL;
		goto ethtool_exit;
	}

	if (test_bit(ICE_FLAG_FW_LLDP_AGENT, change_flags)) {
		if (!test_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags)) {
			enum ice_status status;

			/* Disable FW LLDP engine */
			status = ice_cfg_lldp_mib_change(&pf->hw, false);

			/* If unregistering for LLDP events fails, this is
			 * not an error state, as there shouldn't be any
			 * events to respond to.
			 */
			if (status)
				dev_info(dev, "Failed to unreg for LLDP events\n");

			/* The AQ call to stop the FW LLDP agent will generate
			 * an error if the agent is already stopped.
			 */
			status = ice_aq_stop_lldp(&pf->hw, true, true, NULL);
			if (status)
				dev_warn(dev, "Fail to stop LLDP agent\n");
			/* Use case for having the FW LLDP agent stopped
			 * will likely not need DCB, so failure to init is
			 * not a concern of ethtool
			 */
			status = ice_init_pf_dcb(pf, true);
			if (status)
				dev_warn(dev, "Fail to init DCB\n");

			pf->dcbx_cap &= ~DCB_CAP_DCBX_LLD_MANAGED;
			pf->dcbx_cap |= DCB_CAP_DCBX_HOST;
		} else {
			enum ice_status status;
			bool dcbx_agent_status;

#ifdef NETIF_F_HW_TC
			if (ice_is_adq_active(pf)) {
				dev_err(dev, "Disable ADQ and try again ex:'tc qdisc del dev <eth0> root'\n");
				/* fw-lldp flag is set without checking if
				 * the operation is successful or not, so
				 * clear this flag when it fails
				 */
				clear_bit(ICE_FLAG_FW_LLDP_AGENT, pf->flags);
				ret = -EOPNOTSUPP;
				goto ethtool_exit;
			}
#endif /* NETIF_F_HW_TC */

			/* Remove rule to direct LLDP packets to default VSI.
			 * The FW LLDP engine will now be consuming them.
			 */
			ice_cfg_sw_lldp(vsi, false, false);

			/* AQ command to start FW LLDP agent will return an
			 * error if the agent is already started
			 */
			status = ice_aq_start_lldp(&pf->hw, true, NULL);
			if (status)
				dev_warn(dev, "Fail to start LLDP Agent\n");

			/* AQ command to start FW DCBX agent will fail if
			 * the agent is already started
			 */
			status = ice_aq_start_stop_dcbx(&pf->hw, true,
							&dcbx_agent_status,
							NULL);
			if (status)
				dev_dbg(dev, "Failed to start FW DCBX\n");

			dev_info(dev, "FW DCBX agent is %s\n",
				 dcbx_agent_status ? "ACTIVE" : "DISABLED");

			/* Failure to configure MIB change or init DCB is not
			 * relevant to ethtool.  Print notification that
			 * registration/init failed but do not return error
			 * state to ethtool
			 */
			status = ice_init_pf_dcb(pf, true);
			if (status)
				dev_dbg(dev, "Fail to init DCB\n");

			/* Register for MIB change events */
			status = ice_cfg_lldp_mib_change(&pf->hw, true);
			if (status)
				dev_dbg(dev, "Fail to enable MIB change events\n");

			pf->dcbx_cap &= ~DCB_CAP_DCBX_HOST;
			pf->dcbx_cap |= DCB_CAP_DCBX_LLD_MANAGED;

			ice_nway_reset(netdev);
		}
	}
	if (test_bit(ICE_FLAG_LEGACY_RX, change_flags)) {
		/* down and up VSI so that changes of Rx cfg are reflected. */
		ice_down(vsi);
		ice_up(vsi);
	}
	/* don't allow modification of this flag when a single VF is in
	 * promiscuous mode because it's not supported
	 */
	if (test_bit(ICE_FLAG_VF_TRUE_PROMISC_ENA, change_flags) &&
	    ice_is_any_vf_in_promisc(pf)) {
		dev_err(dev, "Changing vf-true-promisc-support flag while VF(s) are in promiscuous mode not supported\n");
		/* toggle bit back to previous state */
		change_bit(ICE_FLAG_VF_TRUE_PROMISC_ENA, pf->flags);
		ret = -EAGAIN;
	}
ethtool_exit:
	clear_bit(ICE_FLAG_ETHTOOL_CTXT, pf->flags);
	return ret;
}

static int ice_get_sset_count(struct net_device *netdev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		/* The number (and order) of strings reported *must* remain
		 * constant for a given netdevice. This function must not
		 * report a different number based on run time parameters
		 * (such as the number of queues in use, or the setting of
		 * a private ethtool flag). This is due to the nature of the
		 * ethtool stats API.
		 *
		 * Userspace programs such as ethtool must make 3 separate
		 * ioctl requests, one for size, one for the strings, and
		 * finally one for the stats. Since these cross into
		 * userspace, changes to the number or size could result in
		 * undefined memory access or incorrect string<->value
		 * correlations for statistics.
		 *
		 * Even if it appears to be safe, changes to the size or
		 * order of strings will suffer from race conditions and are
		 * not safe.
		 */
		return ICE_ALL_STATS_LEN(netdev);
	case ETH_SS_TEST:
		return ICE_TEST_LEN;
	case ETH_SS_PRIV_FLAGS:
		return ICE_PRIV_FLAG_ARRAY_SIZE;
	default:
		return -EOPNOTSUPP;
	}
}

static void
ice_get_ethtool_stats(struct net_device *netdev,
		      struct ethtool_stats __always_unused *stats, u64 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = ice_get_netdev_priv_vsi(np);
	struct ice_pf *pf = vsi->back;
	struct ice_ring *ring;
	unsigned int j;
	int i = 0;
	char *p;

	ice_update_pf_stats(pf);
	ice_update_vsi_stats(vsi);

	for (j = 0; j < ICE_VSI_STATS_LEN; j++) {
		p = (char *)vsi + ice_gstrings_vsi_stats[j].stat_offset;
		data[i++] = (ice_gstrings_vsi_stats[j].sizeof_stat ==
			     sizeof(u64)) ? *(u64 *)p : *(u32 *)p;
	}
	/* populate per queue stats */
	rcu_read_lock();

	ice_for_each_alloc_txq(vsi, j) {
		ring = READ_ONCE(vsi->tx_rings[j]);
		if (ring) {
			data[i++] = ring->stats.pkts;
			data[i++] = ring->stats.bytes;
#ifdef ICE_ADD_PROBES
			data[i++] = ring->stats.napi_poll_cnt;
#endif /* ICE_ADD_PROBES */
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_tx_stats(vsi, j, data, &i, true);
#endif /* ADQ_PERF_COUNTERS */
		} else {
			data[i++] = 0;
			data[i++] = 0;
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_tx_stats(vsi, j, data, &i, false);
#endif /* ADQ_PERF_COUNTERS */
		}
	}
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
	ice_get_macvlan_tx_stats(vsi->back, data, &i);
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */

	ice_for_each_alloc_rxq(vsi, j) {
		ring = READ_ONCE(vsi->rx_rings[j]);
		if (ring) {
			data[i++] = ring->stats.pkts;
			data[i++] = ring->stats.bytes;
#ifdef ICE_ADD_PROBES
			data[i++] = ring->stats.napi_poll_cnt;
#endif /* ICE_ADD_PROBES */
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_rx_stats(vsi, j, data, &i, true);
#endif /* ADQ_PERF_COUNTERS */
		} else {
			data[i++] = 0;
			data[i++] = 0;
#ifdef ADQ_PERF_COUNTERS
			ice_get_chnl_rx_stats(vsi, j, data, &i, false);
#endif /* ADQ_PERF_COUNTERS */
		}
	}
#ifdef ICE_ADD_PROBES
#ifdef HAVE_NETDEV_SB_DEV
	ice_get_macvlan_rx_stats(vsi->back, data, &i);
#endif /* HAVE_NETDEV_SB_DEV */
#endif /* ICE_ADD_PROBES */

	rcu_read_unlock();

	if (vsi->type != ICE_VSI_PF)
		return;

	for (j = 0; j < ICE_PF_STATS_LEN; j++) {
		p = (char *)pf + ice_gstrings_pf_stats[j].stat_offset;
		data[i++] = (ice_gstrings_pf_stats[j].sizeof_stat ==
			     sizeof(u64)) ? *(u64 *)p : *(u32 *)p;
	}

	for (j = 0; j < ICE_MAX_USER_PRIORITY; j++) {
		data[i++] = pf->stats.priority_xon_tx[j];
		data[i++] = pf->stats.priority_xoff_tx[j];
	}

	for (j = 0; j < ICE_MAX_USER_PRIORITY; j++) {
		data[i++] = pf->stats.priority_xon_rx[j];
		data[i++] = pf->stats.priority_xoff_rx[j];
	}
}

#define ICE_PHY_TYPE_LOW_MASK_MIN_1G	(ICE_PHY_TYPE_LOW_100BASE_TX | \
					 ICE_PHY_TYPE_LOW_100M_SGMII)

#define ICE_PHY_TYPE_LOW_MASK_MIN_25G	(ICE_PHY_TYPE_LOW_MASK_MIN_1G | \
					 ICE_PHY_TYPE_LOW_1000BASE_T | \
					 ICE_PHY_TYPE_LOW_1000BASE_SX | \
					 ICE_PHY_TYPE_LOW_1000BASE_LX | \
					 ICE_PHY_TYPE_LOW_1000BASE_KX | \
					 ICE_PHY_TYPE_LOW_1G_SGMII | \
					 ICE_PHY_TYPE_LOW_2500BASE_T | \
					 ICE_PHY_TYPE_LOW_2500BASE_X | \
					 ICE_PHY_TYPE_LOW_2500BASE_KX | \
					 ICE_PHY_TYPE_LOW_5GBASE_T | \
					 ICE_PHY_TYPE_LOW_5GBASE_KR | \
					 ICE_PHY_TYPE_LOW_10GBASE_T | \
					 ICE_PHY_TYPE_LOW_10G_SFI_DA | \
					 ICE_PHY_TYPE_LOW_10GBASE_SR | \
					 ICE_PHY_TYPE_LOW_10GBASE_LR | \
					 ICE_PHY_TYPE_LOW_10GBASE_KR_CR1 | \
					 ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_10G_SFI_C2C)

#define ICE_PHY_TYPE_LOW_MASK_100G	(ICE_PHY_TYPE_LOW_100GBASE_CR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_SR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_LR4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_KR4 | \
					 ICE_PHY_TYPE_LOW_100G_CAUI4_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_100G_CAUI4 | \
					 ICE_PHY_TYPE_LOW_100G_AUI4_AOC_ACC | \
					 ICE_PHY_TYPE_LOW_100G_AUI4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_CR_PAM4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4 | \
					 ICE_PHY_TYPE_LOW_100GBASE_CP2 | \
					 ICE_PHY_TYPE_LOW_100GBASE_SR2 | \
					 ICE_PHY_TYPE_LOW_100GBASE_DR)

#define ICE_PHY_TYPE_HIGH_MASK_100G	(ICE_PHY_TYPE_HIGH_100GBASE_KR2_PAM4 | \
					 ICE_PHY_TYPE_HIGH_100G_CAUI2_AOC_ACC |\
					 ICE_PHY_TYPE_HIGH_100G_CAUI2 | \
					 ICE_PHY_TYPE_HIGH_100G_AUI2_AOC_ACC | \
					 ICE_PHY_TYPE_HIGH_100G_AUI2)

#ifdef HAVE_ETHTOOL_100G_BITS
/**
 * ice_mask_min_supported_speeds
 * @phy_types_high: PHY type high
 * @phy_types_low: PHY type low to apply minimum supported speeds mask
 *
 * Apply minimum supported speeds mask to PHY type low. These are the speeds
 * for ethtool supported link mode.
 */
static
void ice_mask_min_supported_speeds(u64 phy_types_high, u64 *phy_types_low)
#else
static void ice_mask_min_supported_speeds(u64 *phy_types_low)
#endif  /* !HAVE_ETHTOOL_100G_BITS */
{
	/* if QSFP connection with 100G speed, minimum supported speed is 25G */
#ifdef HAVE_ETHTOOL_100G_BITS
	if (*phy_types_low & ICE_PHY_TYPE_LOW_MASK_100G ||
	    phy_types_high & ICE_PHY_TYPE_HIGH_MASK_100G)
#else /* HAVE_ETHTOOL_100G_BITS */
	if (*phy_types_low & ICE_PHY_TYPE_LOW_MASK_100G)
#endif /* !HAVE_ETHTOOL_100G_BITS */
		*phy_types_low &= ~ICE_PHY_TYPE_LOW_MASK_MIN_25G;
	else
		*phy_types_low &= ~ICE_PHY_TYPE_LOW_MASK_MIN_1G;
}

#ifdef HAVE_ETHTOOL_100G_BITS
#define ice_ethtool_advertise_link_mode(aq_link_speed, ethtool_link_mode)    \
	do {								     \
		if (req_speeds & (aq_link_speed) ||			     \
		    (!req_speeds &&					     \
		     (adv_phy_type_lo & phy_type_mask_lo ||		     \
		      adv_phy_type_hi & phy_type_mask_hi)))		     \
			ethtool_link_ksettings_add_link_mode(ks, advertising,\
							ethtool_link_mode);  \
	} while (0)
#else /* HAVE_ETHTOOL_100G_BITS */
#define ice_ethtool_advertise_link_mode(aq_link_speed, ethtool_link_mode)    \
	do {								     \
		if (req_speeds & (aq_link_speed) ||			     \
		    (req_speeds && adv_phy_type_lo & phy_type_mask_lo))	     \
			ethtool_link_ksettings_add_link_mode(ks, advertising,\
							ethtool_link_mode);  \
	} while (0)
#endif /* ! HAVE_ETHTOOL_100G_BITS */

/**
 * ice_phy_type_to_ethtool - convert the phy_types to ethtool link modes
 * @netdev: network interface device structure
 * @ks: ethtool link ksettings struct to fill out
 */
static void
ice_phy_type_to_ethtool(struct net_device *netdev,
			struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	u64 phy_type_mask_lo = 0;
#ifdef HAVE_ETHTOOL_100G_BITS
	u64 phy_type_mask_hi = 0;
#endif /* HAVE_ETHTOOL_100G_BITS */
	u64 adv_phy_type_lo = 0;
#ifdef HAVE_ETHTOOL_100G_BITS
	u64 adv_phy_type_hi = 0;
	u64 phy_types_high = 0;
#endif /* HAVE_ETHTOOL_100G_BITS */
	u64 phy_types_low = 0;
	u16 req_speeds;

	req_speeds = vsi->port_info->phy.link_info.req_speeds;

	/* Check if lenient mode is supported and enabled, or in strict mode.
	 *
	 * In lenient mode the Supported link modes are the PHY types without
	 * media. The Advertising link mode is either 1. the user requested
	 * speed, 2. the override PHY mask, or 3. the PHY types with media.
	 *
	 * In strict mode Supported link mode are the PHY type with media,
	 * and Advertising link modes are the media PHY type or the speed
	 * requested by user.
	 */
	if (test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags)) {
		struct ice_link_default_override_tlv *ldo;

		ldo = &pf->link_dflt_override;
		phy_types_low = le64_to_cpu(pf->nvm_phy_type_lo);
#ifdef HAVE_ETHTOOL_100G_BITS
		phy_types_high = le64_to_cpu(pf->nvm_phy_type_hi);

		ice_mask_min_supported_speeds(phy_types_high, &phy_types_low);
#else /* HAVE_ETHTOOL_100G_BITS */
		ice_mask_min_supported_speeds(&phy_types_low);
#endif /* !HAVE_ETHTOOL_100G_BITS */

		/* If override enabled and PHY mask set, then
		 * Advertising link mode is the intersection of the PHY
		 * types without media and the override PHY mask.
		 */
		if (ldo->options & ICE_LINK_OVERRIDE_EN &&
		    (ldo->phy_type_low || ldo->phy_type_high)) {
			adv_phy_type_lo =
				le64_to_cpu(pf->nvm_phy_type_lo) &
				ldo->phy_type_low;
#ifdef HAVE_ETHTOOL_100G_BITS
			adv_phy_type_hi =
				le64_to_cpu(pf->nvm_phy_type_hi) &
				ldo->phy_type_high;
#endif /* HAVE_ETHTOOL_100G_BITS */
		}
	} else {
		phy_types_low = vsi->port_info->phy.phy_type_low;
#ifdef HAVE_ETHTOOL_100G_BITS
		phy_types_high = vsi->port_info->phy.phy_type_high;
#endif /* HAVE_ETHTOOL_100G_BITS */
	}

	/* If Advertising link mode PHY type is not using override PHY type,
	 * then use PHY type with media.
	 */
#ifdef HAVE_ETHTOOL_100G_BITS
	if (!adv_phy_type_lo && !adv_phy_type_hi) {
		adv_phy_type_lo = vsi->port_info->phy.phy_type_low;
		adv_phy_type_hi = vsi->port_info->phy.phy_type_high;
	}
#else
	if (!adv_phy_type_lo)
		adv_phy_type_lo = vsi->port_info->phy.phy_type_low;
#endif /* !HAVE_ETHTOOL_100G_BITS */

	ethtool_link_ksettings_zero_link_mode(ks, supported);
	ethtool_link_ksettings_zero_link_mode(ks, advertising);

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_100BASE_TX |
			   ICE_PHY_TYPE_LOW_100M_SGMII;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     100baseT_Full);

		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_100MB,
						100baseT_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_1000BASE_T |
			   ICE_PHY_TYPE_LOW_1G_SGMII;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     1000baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_1000MB,
						1000baseT_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_1000BASE_KX;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     1000baseKX_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_1000MB,
						1000baseKX_Full);
	}
#ifdef HAVE_ETHTOOL_NEW_1G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_1000BASE_SX |
			   ICE_PHY_TYPE_LOW_1000BASE_LX;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     1000baseX_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_1000MB,
						1000baseX_Full);
	}
#else

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_1000BASE_SX |
			   ICE_PHY_TYPE_LOW_1000BASE_LX;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     1000baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_1000MB,
						1000baseT_Full);
	}
#endif /* HAVE_ETHTOOL_NEW_1G_BITS */
#ifdef HAVE_ETHTOOL_NEW_2500MB_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_2500BASE_T;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     2500baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_2500MB,
						2500baseT_Full);
	}
#else

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_2500BASE_T;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     2500baseX_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_2500MB,
						2500baseX_Full);
	}
#endif /* HAVE_ETHTOOL_NEW_2500MB_BITS */

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_2500BASE_X |
			   ICE_PHY_TYPE_LOW_2500BASE_KX;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     2500baseX_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_2500MB,
						2500baseX_Full);
	}
#ifdef HAVE_ETHTOOL_5G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_5GBASE_T |
			   ICE_PHY_TYPE_LOW_5GBASE_KR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     5000baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_5GB,
						5000baseT_Full);
	}
#endif /* HAVE_ETHTOOL_5G_BITS */

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_10GBASE_T |
			   ICE_PHY_TYPE_LOW_10G_SFI_DA |
			   ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC |
			   ICE_PHY_TYPE_LOW_10G_SFI_C2C;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     10000baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_10GB,
						10000baseT_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_10GBASE_KR_CR1;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     10000baseKR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_10GB,
						10000baseKR_Full);
	}
#ifdef HAVE_ETHTOOL_NEW_10G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_10GBASE_SR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     10000baseSR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_10GB,
						10000baseSR_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_10GBASE_LR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     10000baseLR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_10GB,
						10000baseLR_Full);
	}
#else

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_10GBASE_SR |
			   ICE_PHY_TYPE_LOW_10GBASE_LR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     10000baseT_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_10GB,
						10000baseT_Full);
	}
#endif /* HAVE_ETHTOOL_NEW_10G_BITS */
#ifdef HAVE_ETHTOOL_25G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_25GBASE_T |
			   ICE_PHY_TYPE_LOW_25GBASE_CR |
			   ICE_PHY_TYPE_LOW_25GBASE_CR_S |
			   ICE_PHY_TYPE_LOW_25GBASE_CR1 |
			   ICE_PHY_TYPE_LOW_25G_AUI_AOC_ACC |
			   ICE_PHY_TYPE_LOW_25G_AUI_C2C;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     25000baseCR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_25GB,
						25000baseCR_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_25GBASE_SR |
			   ICE_PHY_TYPE_LOW_25GBASE_LR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     25000baseSR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_25GB,
						25000baseSR_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_25GBASE_KR |
			   ICE_PHY_TYPE_LOW_25GBASE_KR_S |
			   ICE_PHY_TYPE_LOW_25GBASE_KR1;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     25000baseKR_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_25GB,
						25000baseKR_Full);
	}
#endif /* HAVE_ETHTOOL_25G_BITS */

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_40GBASE_KR4;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     40000baseKR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_40GB,
						40000baseKR4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_40GBASE_CR4 |
			   ICE_PHY_TYPE_LOW_40G_XLAUI_AOC_ACC |
			   ICE_PHY_TYPE_LOW_40G_XLAUI;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     40000baseCR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_40GB,
						40000baseCR4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_40GBASE_SR4;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     40000baseSR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_40GB,
						40000baseSR4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_40GBASE_LR4;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     40000baseLR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_40GB,
						40000baseLR4_Full);
	}
#ifdef HAVE_ETHTOOL_50G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_50GBASE_CR2 |
			   ICE_PHY_TYPE_LOW_50G_LAUI2_AOC_ACC |
			   ICE_PHY_TYPE_LOW_50G_LAUI2 |
			   ICE_PHY_TYPE_LOW_50G_AUI2_AOC_ACC |
			   ICE_PHY_TYPE_LOW_50G_AUI2 |
			   ICE_PHY_TYPE_LOW_50GBASE_CP |
			   ICE_PHY_TYPE_LOW_50GBASE_SR |
			   ICE_PHY_TYPE_LOW_50G_AUI1_AOC_ACC |
			   ICE_PHY_TYPE_LOW_50G_AUI1;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     50000baseCR2_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_50GB,
						50000baseCR2_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_50GBASE_KR2 |
			   ICE_PHY_TYPE_LOW_50GBASE_KR_PAM4;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     50000baseKR2_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_50GB,
						50000baseKR2_Full);
	}
#endif /* HAVE_ETHTOOL_50G_BITS */
#ifdef HAVE_ETHTOOL_NEW_50G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_50GBASE_SR2 |
			   ICE_PHY_TYPE_LOW_50GBASE_LR2 |
			   ICE_PHY_TYPE_LOW_50GBASE_FR |
			   ICE_PHY_TYPE_LOW_50GBASE_LR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     50000baseSR2_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_50GB,
						50000baseSR2_Full);
	}
#endif /* HAVE_ETHTOOL_NEW_50G_BITS */
#ifdef HAVE_ETHTOOL_100G_BITS

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_100GBASE_CR4 |
			   ICE_PHY_TYPE_LOW_100G_CAUI4_AOC_ACC |
			   ICE_PHY_TYPE_LOW_100G_CAUI4 |
			   ICE_PHY_TYPE_LOW_100G_AUI4_AOC_ACC |
			   ICE_PHY_TYPE_LOW_100G_AUI4 |
			   ICE_PHY_TYPE_LOW_100GBASE_CR_PAM4 |
			   ICE_PHY_TYPE_LOW_100GBASE_CP2;
	phy_type_mask_hi = ICE_PHY_TYPE_HIGH_100G_CAUI2_AOC_ACC |
			   ICE_PHY_TYPE_HIGH_100G_CAUI2 |
			   ICE_PHY_TYPE_HIGH_100G_AUI2_AOC_ACC |
			   ICE_PHY_TYPE_HIGH_100G_AUI2;
	if (phy_types_low & phy_type_mask_lo ||
	    phy_types_high & phy_type_mask_hi) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     100000baseCR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_100GB,
						100000baseCR4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_100GBASE_SR4 |
			   ICE_PHY_TYPE_LOW_100GBASE_SR2;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     100000baseSR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_100GB,
						100000baseSR4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_100GBASE_LR4 |
			   ICE_PHY_TYPE_LOW_100GBASE_DR;
	if (phy_types_low & phy_type_mask_lo) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     100000baseLR4_ER4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_100GB,
						100000baseLR4_ER4_Full);
	}

	phy_type_mask_lo = ICE_PHY_TYPE_LOW_100GBASE_KR4 |
			   ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4;
	phy_type_mask_hi = ICE_PHY_TYPE_HIGH_100GBASE_KR2_PAM4;
	if (phy_types_low & phy_type_mask_lo ||
	    phy_types_high & phy_type_mask_hi) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     100000baseKR4_Full);
		ice_ethtool_advertise_link_mode(ICE_AQ_LINK_SPEED_100GB,
						100000baseKR4_Full);
	}
#endif /* HAVE_ETHTOOL_100G_BITS */

	/* Autoneg PHY types */
	if (phy_types_low & ICE_PHY_TYPE_LOW_100BASE_TX ||
	    phy_types_low & ICE_PHY_TYPE_LOW_1000BASE_T ||
	    phy_types_low & ICE_PHY_TYPE_LOW_1000BASE_KX ||
	    phy_types_low & ICE_PHY_TYPE_LOW_2500BASE_T ||
	    phy_types_low & ICE_PHY_TYPE_LOW_2500BASE_KX ||
	    phy_types_low & ICE_PHY_TYPE_LOW_5GBASE_T ||
	    phy_types_low & ICE_PHY_TYPE_LOW_5GBASE_KR ||
	    phy_types_low & ICE_PHY_TYPE_LOW_10GBASE_T ||
	    phy_types_low & ICE_PHY_TYPE_LOW_10GBASE_KR_CR1 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_T ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_CR ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_CR_S ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_CR1 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_KR ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_KR_S ||
	    phy_types_low & ICE_PHY_TYPE_LOW_25GBASE_KR1 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_40GBASE_CR4 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_40GBASE_KR4) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Autoneg);
	}
	if (phy_types_low & ICE_PHY_TYPE_LOW_50GBASE_CR2 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_50GBASE_KR2 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_50GBASE_CP ||
	    phy_types_low & ICE_PHY_TYPE_LOW_50GBASE_KR_PAM4) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Autoneg);
	}
	if (phy_types_low & ICE_PHY_TYPE_LOW_100GBASE_CR4 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_100GBASE_KR4 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_100GBASE_KR_PAM4 ||
	    phy_types_low & ICE_PHY_TYPE_LOW_100GBASE_CP2) {
		ethtool_link_ksettings_add_link_mode(ks, supported,
						     Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Autoneg);
	}
}

#define TEST_SET_BITS_TIMEOUT	50
#define TEST_SET_BITS_SLEEP_MAX	2000
#define TEST_SET_BITS_SLEEP_MIN	1000

#ifdef ETHTOOL_GLINKSETTINGS
/**
 * ice_get_settings_link_up - Get Link settings for when link is up
 * @ks: ethtool ksettings to fill in
 * @netdev: network interface device structure
 */
static void
ice_get_settings_link_up(struct ethtool_link_ksettings *ks,
			 struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_info *pi = np->vsi->port_info;
	struct ice_link_status *link_info;
	struct ice_vsi *vsi = np->vsi;

	link_info = &vsi->port_info->phy.link_info;

	/* Get supported and advertised settings from PHY ability with media */
	ice_phy_type_to_ethtool(netdev, ks);

	switch (link_info->link_speed) {
#ifdef HAVE_ETHTOOL_100G_BITS
	case ICE_AQ_LINK_SPEED_100GB:
		ks->base.speed = SPEED_100000;
		break;
#endif /* HAVE_ETHTOOL_100G_BITS */
#if defined(HAVE_ETHTOOL_50G_BITS) || defined(HAVE_ETHTOOL_NEW_50G_BITS)
	case ICE_AQ_LINK_SPEED_50GB:
		ks->base.speed = SPEED_50000;
		break;
#endif /* HAVE_ETHTOOL_50G_BITS || HAVE_ETHTOOL_NEW_50G_BITS */
	case ICE_AQ_LINK_SPEED_40GB:
		ks->base.speed = SPEED_40000;
		break;
#ifdef HAVE_ETHTOOL_25G_BITS
	case ICE_AQ_LINK_SPEED_25GB:
		ks->base.speed = SPEED_25000;
		break;
#endif /* HAVE_ETHTOOL_25G_BITS */
	case ICE_AQ_LINK_SPEED_20GB:
		ks->base.speed = SPEED_20000;
		break;
	case ICE_AQ_LINK_SPEED_10GB:
		ks->base.speed = SPEED_10000;
		break;
#ifdef HAVE_ETHTOOL_5G_BITS
	case ICE_AQ_LINK_SPEED_5GB:
		ks->base.speed = SPEED_5000;
		break;
#endif /* HAVE_ETHTOOL_5G_BITS */
	case ICE_AQ_LINK_SPEED_2500MB:
		ks->base.speed = SPEED_2500;
		break;
	case ICE_AQ_LINK_SPEED_1000MB:
		ks->base.speed = SPEED_1000;
		break;
	case ICE_AQ_LINK_SPEED_100MB:
		ks->base.speed = SPEED_100;
		break;
	default:
		netdev_info(netdev, "WARNING: Unrecognized link_speed (0x%x).\n",
			    link_info->link_speed);
		break;
	}
	ks->base.duplex = DUPLEX_FULL;

	if (link_info->an_info & ICE_AQ_AN_COMPLETED)
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Autoneg);

	/* Set flow control negotiated Rx/Tx pause */
	switch (pi->fc.current_mode) {
	case ICE_FC_FULL:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising, Pause);
		break;
	case ICE_FC_TX_PAUSE:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	case ICE_FC_RX_PAUSE:
		ethtool_link_ksettings_add_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	case ICE_FC_PFC:
	default:
		ethtool_link_ksettings_del_link_mode(ks, lp_advertising, Pause);
		ethtool_link_ksettings_del_link_mode(ks, lp_advertising,
						     Asym_Pause);
		break;
	}
}

/**
 * ice_get_settings_link_down - Get the Link settings when link is down
 * @ks: ethtool ksettings to fill in
 * @netdev: network interface device structure
 *
 * Reports link settings that can be determined when link is down
 */
static void
ice_get_settings_link_down(struct ethtool_link_ksettings *ks,
			   struct net_device *netdev)
{
	/* link is down and the driver needs to fall back on
	 * supported PHY types to figure out what info to display
	 */
	ice_phy_type_to_ethtool(netdev, ks);

	/* With no link, speed and duplex are unknown */
	ks->base.speed = SPEED_UNKNOWN;
	ks->base.duplex = DUPLEX_UNKNOWN;
}

/**
 * ice_get_link_ksettings - Get Link Speed and Duplex settings
 * @netdev: network interface device structure
 * @ks: ethtool ksettings
 *
 * Reports speed/duplex settings based on media_type
 */
static int
ice_get_link_ksettings(struct net_device *netdev,
		       struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *caps;
	struct ice_link_status *hw_link_info;
	struct ice_vsi *vsi = np->vsi;
	enum ice_status status;
	int err = 0;

	ethtool_link_ksettings_zero_link_mode(ks, supported);
	ethtool_link_ksettings_zero_link_mode(ks, advertising);
	ethtool_link_ksettings_zero_link_mode(ks, lp_advertising);
	hw_link_info = &vsi->port_info->phy.link_info;


	/* set speed and duplex */
	if (hw_link_info->link_info & ICE_AQ_LINK_UP)
		ice_get_settings_link_up(ks, netdev);
	else
		ice_get_settings_link_down(ks, netdev);

	/* set autoneg settings */
	ks->base.autoneg = (hw_link_info->an_info & ICE_AQ_AN_COMPLETED) ?
		AUTONEG_ENABLE : AUTONEG_DISABLE;

	/* set media type settings */
	switch (vsi->port_info->phy.media_type) {
	case ICE_MEDIA_FIBER:
		ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
		ks->base.port = PORT_FIBRE;
		break;
	case ICE_MEDIA_BASET:
		ethtool_link_ksettings_add_link_mode(ks, supported, TP);
		ethtool_link_ksettings_add_link_mode(ks, advertising, TP);
		ks->base.port = PORT_TP;
		break;
	case ICE_MEDIA_BACKPLANE:
		ethtool_link_ksettings_add_link_mode(ks, supported, Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, supported, Backplane);
		ethtool_link_ksettings_add_link_mode(ks, advertising, Autoneg);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Backplane);
		ks->base.port = PORT_NONE;
		break;
	case ICE_MEDIA_DA:
		ethtool_link_ksettings_add_link_mode(ks, supported, FIBRE);
		ethtool_link_ksettings_add_link_mode(ks, advertising, FIBRE);
		ks->base.port = PORT_DA;
		break;
	default:
		ks->base.port = PORT_OTHER;
		break;
	}

	/* flow control is symmetric and always supported */
	ethtool_link_ksettings_add_link_mode(ks, supported, Pause);

	caps = kzalloc(sizeof(*caps), GFP_KERNEL);
	if (!caps)
		return -ENOMEM;

	status = ice_aq_get_phy_caps(vsi->port_info, false,
				     ICE_AQC_REPORT_SW_CFG, caps, NULL);
	if (status) {
		err = -EIO;
		goto done;
	}

	/* Set the advertised flow control based on the PHY capability */
	if ((caps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE) &&
	    (caps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)) {
		ethtool_link_ksettings_add_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else if (caps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE) {
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else if (caps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE) {
		ethtool_link_ksettings_add_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     Asym_Pause);
	} else {
		ethtool_link_ksettings_del_link_mode(ks, advertising, Pause);
		ethtool_link_ksettings_del_link_mode(ks, advertising,
						     Asym_Pause);
	}

#ifdef ETHTOOL_GFECPARAM
	/* Set advertised FEC modes based on PHY capability */
	ethtool_link_ksettings_add_link_mode(ks, advertising, FEC_NONE);

	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_REQ)
		ethtool_link_ksettings_add_link_mode(ks, advertising,
						     FEC_BASER);
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_528_REQ ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_544_REQ)
		ethtool_link_ksettings_add_link_mode(ks, advertising, FEC_RS);

	status = ice_aq_get_phy_caps(vsi->port_info, false,
				     ICE_AQC_REPORT_TOPO_CAP, caps, NULL);
	if (status) {
		err = -EIO;
		goto done;
	}

	/* Set supported FEC modes based on PHY capability */
	ethtool_link_ksettings_add_link_mode(ks, supported, FEC_NONE);

	if (caps->link_fec_options & ICE_AQC_PHY_FEC_10G_KR_40G_KR4_EN ||
	    caps->link_fec_options & ICE_AQC_PHY_FEC_25G_KR_CLAUSE74_EN)
		ethtool_link_ksettings_add_link_mode(ks, supported, FEC_BASER);
	if (caps->link_fec_options & ICE_AQC_PHY_FEC_25G_RS_CLAUSE91_EN)
		ethtool_link_ksettings_add_link_mode(ks, supported, FEC_RS);

#endif /* ETHTOOL_GFECPARAM */
done:
	kfree(caps);
	return err;
}

/**
 * ice_ksettings_find_adv_link_speed - Find advertising link speed
 * @ks: ethtool ksettings
 */
static u16
ice_ksettings_find_adv_link_speed(const struct ethtool_link_ksettings *ks)
{
	u16 adv_link_speed = 0;

	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  100baseT_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_100MB;
#ifdef HAVE_ETHTOOL_NEW_1G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseX_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_1000MB;
#endif /* HAVE_ETHTOOL_NEW_1G_BITS */
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseT_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  1000baseKX_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_1000MB;
#ifdef HAVE_ETHTOOL_NEW_2500MB_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  2500baseT_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_2500MB;
#endif /* HAVE_ETHTOOL_NEW_2500MB_BITS */
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  2500baseX_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_2500MB;
#ifdef HAVE_ETHTOOL_5G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  5000baseT_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_5GB;
#endif /* HAVE_ETHTOOL_5G_BITS */
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  10000baseT_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  10000baseKR_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_10GB;
#ifdef HAVE_ETHTOOL_NEW_10G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  10000baseSR_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  10000baseLR_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_10GB;
#endif /* HAVE_ETHTOOL_NEW_10G_BITS */
#ifdef HAVE_ETHTOOL_25G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  25000baseCR_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  25000baseSR_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  25000baseKR_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_25GB;
#endif /* HAVE_ETHTOOL_25G_BITS */
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  40000baseCR4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  40000baseSR4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  40000baseLR4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  40000baseKR4_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_40GB;
#ifdef HAVE_ETHTOOL_50G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  50000baseCR2_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  50000baseKR2_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_50GB;
#endif /* HAVE_ETHTOOL_50G_BITS */
#ifdef HAVE_ETHTOOL_NEW_50G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  50000baseSR2_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_50GB;
#endif /* HAVE_ETHTOOL_NEW_50G_BITS */
#ifdef HAVE_ETHTOOL_100G_BITS
	if (ethtool_link_ksettings_test_link_mode(ks, advertising,
						  100000baseCR4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  100000baseSR4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  100000baseLR4_ER4_Full) ||
	    ethtool_link_ksettings_test_link_mode(ks, advertising,
						  100000baseKR4_Full))
		adv_link_speed |= ICE_AQ_LINK_SPEED_100GB;
#endif /* HAVE_ETHTOOL_100G_BITS */

	return adv_link_speed;
}

/**
 * ice_setup_autoneg
 * @p: port info
 * @ks: ethtool_link_ksettings
 * @config: configuration that will be sent down to FW
 * @autoneg_enabled: autonegotiation is enabled or not
 * @autoneg_changed: will there a change in autonegotiation
 * @netdev: network interface device structure
 *
 * Setup PHY autonegotiation feature
 */
static int
ice_setup_autoneg(struct ice_port_info *p, struct ethtool_link_ksettings *ks,
		  struct ice_aqc_set_phy_cfg_data *config,
		  u8 autoneg_enabled, u8 *autoneg_changed,
		  struct net_device *netdev)
{
	int err = 0;

	*autoneg_changed = 0;

	/* Check autoneg */
	if (autoneg_enabled == AUTONEG_ENABLE) {
		/* If autoneg was not already enabled */
		if (!(p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED)) {
			/* If autoneg is not supported, return error */
			if (!ethtool_link_ksettings_test_link_mode(ks,
								   supported,
								   Autoneg)) {
				netdev_info(netdev, "Autoneg not supported on this phy.\n");
				err = -EINVAL;
			} else {
				/* Autoneg is allowed to change */
				config->caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
				*autoneg_changed = 1;
			}
		}
	} else {
		/* If autoneg is currently enabled */
		if (p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED) {
			/* If autoneg is supported 10GBASE_T is the only PHY
			 * that can disable it, so otherwise return error
			 */
			if (ethtool_link_ksettings_test_link_mode(ks,
								  supported,
								  Autoneg)) {
				netdev_info(netdev, "Autoneg cannot be disabled on this phy\n");
				err = -EINVAL;
			} else {
				/* Autoneg is allowed to change */
				config->caps &= ~ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
				*autoneg_changed = 1;
			}
		}
	}

	return err;
}

/**
 * ice_set_link_ksettings - Set Speed and Duplex
 * @netdev: network interface device structure
 * @ks: ethtool ksettings
 *
 * Set speed/duplex per media_types advertised/forced
 */
static int
ice_set_link_ksettings(struct net_device *netdev,
		       const struct ethtool_link_ksettings *ks)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ethtool_link_ksettings safe_ks, copy_ks;
	struct ice_aqc_get_phy_caps_data *abilities;
	u8 autoneg, timeout = TEST_SET_BITS_TIMEOUT;
	u16 adv_link_speed, curr_link_speed, idx;
	struct ice_aqc_set_phy_cfg_data config;
	struct ice_pf *pf = np->vsi->back;
	struct ice_port_info *p;
	u8 autoneg_changed = 0;
	enum ice_status status;
	u64 phy_type_high = 0;
	u64 phy_type_low = 0;
	int err = 0;
	bool linkup;

	p = np->vsi->port_info;

	if (!p)
		return -EOPNOTSUPP;

	/* Check if this is LAN VSI */
	ice_for_each_vsi(pf, idx)
		if (pf->vsi[idx]->type == ICE_VSI_PF) {
			if (np->vsi != pf->vsi[idx])
				return -EOPNOTSUPP;
			break;
		}

	if (p->phy.media_type != ICE_MEDIA_BASET &&
	    p->phy.media_type != ICE_MEDIA_FIBER &&
	    p->phy.media_type != ICE_MEDIA_BACKPLANE &&
	    p->phy.media_type != ICE_MEDIA_DA &&
	    p->phy.link_info.link_info & ICE_AQ_LINK_UP)
		return -EOPNOTSUPP;

	abilities = kzalloc(sizeof(*abilities), GFP_KERNEL);
	if (!abilities)
		return -ENOMEM;

	/* Get the PHY capabilities based on media */
	status = ice_aq_get_phy_caps(p, false, ICE_AQC_REPORT_TOPO_CAP,
				     abilities, NULL);
	if (status) {
		err = -EAGAIN;
		goto done;
	}

	/* copy the ksettings to copy_ks to avoid modifying the original */
	memcpy(&copy_ks, ks, sizeof(copy_ks));

	/* save autoneg out of ksettings */
	autoneg = copy_ks.base.autoneg;

	memset(&safe_ks, 0, sizeof(safe_ks));

	/* Get link modes supported by hardware.*/
	ice_phy_type_to_ethtool(netdev, &safe_ks);

	/* and check against modes requested by user.
	 * Return an error if unsupported mode was set.
	 */
	if (!bitmap_subset(copy_ks.link_modes.advertising,
			   safe_ks.link_modes.supported,
			   __ETHTOOL_LINK_MODE_MASK_NBITS)) {
		if (!test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags))
			netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
		err = -EINVAL;
		goto done;
	}

	/* get our own copy of the bits to check against */
	memset(&safe_ks, 0, sizeof(safe_ks));
	safe_ks.base.cmd = copy_ks.base.cmd;
	safe_ks.base.link_mode_masks_nwords =
		copy_ks.base.link_mode_masks_nwords;
	ice_get_link_ksettings(netdev, &safe_ks);

	/* set autoneg back to what it currently is */
	copy_ks.base.autoneg = safe_ks.base.autoneg;
	/* we don't compare the speed */
	copy_ks.base.speed = safe_ks.base.speed;

	/* If copy_ks.base and safe_ks.base are not the same now, then they are
	 * trying to set something that we do not support.
	 */
	if (memcmp(&copy_ks.base, &safe_ks.base, sizeof(copy_ks.base))) {
		err = -EOPNOTSUPP;
		goto done;
	}

	while (test_and_set_bit(ICE_CFG_BUSY, pf->state)) {
		timeout--;
		if (!timeout) {
			err = -EBUSY;
			goto done;
		}
		usleep_range(TEST_SET_BITS_SLEEP_MIN, TEST_SET_BITS_SLEEP_MAX);
	}

	/* Copy the current user PHY configuration. The current user PHY
	 * configuration is initialized during probe from PHY capabilities
	 * software mode, and updated on set PHY configuration.
	 */
	memcpy(&config, &p->phy.curr_user_phy_cfg, sizeof(config));

	config.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	/* Check autoneg */
	err = ice_setup_autoneg(p, &safe_ks, &config, autoneg, &autoneg_changed,
				netdev);

	if (err)
		goto done;

	/* Call to get the current link speed */
	p->phy.get_link_info = true;
	status = ice_get_link_status(p, &linkup);
	if (status) {
		err = -EAGAIN;
		goto done;
	}

	curr_link_speed = p->phy.link_info.link_speed;
	adv_link_speed = ice_ksettings_find_adv_link_speed(ks);

	/* If speed didn't get set, set it to what it currently is.
	 * This is needed because if advertise is 0 (as it is when autoneg
	 * is disabled) then speed won't get set.
	 */
	if (!adv_link_speed)
		adv_link_speed = curr_link_speed;

	/* Convert the advertise link speeds to their corresponded PHY_TYPE */
	ice_update_phy_type(&phy_type_low, &phy_type_high, adv_link_speed);

	if (!autoneg_changed && adv_link_speed == curr_link_speed) {
		netdev_info(netdev, "Nothing changed, exiting without setting anything.\n");
		goto done;
	}

	/* save the requested speeds */
	p->phy.link_info.req_speeds = adv_link_speed;

	/* set link and auto negotiation so changes take effect */
	config.caps |= ICE_AQ_PHY_ENA_LINK;

	/* check if there is a PHY type for the requested advertised speed */
	if (!(phy_type_low || phy_type_high)) {
		netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
		err = -EAGAIN;
		goto done;
	}

	/* intersect requested advertised speed PHY types with media PHY types
	 * for set PHY configuration
	 */
	config.phy_type_high = cpu_to_le64(phy_type_high) &
			abilities->phy_type_high;
	config.phy_type_low = cpu_to_le64(phy_type_low) &
			abilities->phy_type_low;

	if (!(config.phy_type_high || config.phy_type_low)) {
		/* If there is no intersection and lenient mode is enabled, then
		 * intersect the requested advertised speed with NVM media type
		 * PHY types.
		 */
		if (test_bit(ICE_FLAG_LINK_LENIENT_MODE_ENA, pf->flags)) {
			config.phy_type_high = cpu_to_le64(phy_type_high) &
					       pf->nvm_phy_type_hi;
			config.phy_type_low = cpu_to_le64(phy_type_low) &
					      pf->nvm_phy_type_lo;
		} else {
			netdev_info(netdev, "The selected speed is not supported by the current media. Please select a link speed that is supported by the current media.\n");
			err = -EAGAIN;
			goto done;
		}
	}

	/* If link is up put link down */
	if (p->phy.link_info.link_info & ICE_AQ_LINK_UP) {
		/* Tell the OS link is going down, the link will go
		 * back up when fw says it is ready asynchronously
		 */
		ice_print_link_msg(np->vsi, false);
		netif_carrier_off(netdev);
		netif_tx_stop_all_queues(netdev);
	}

	/* make the aq call */
	status = ice_aq_set_phy_cfg(&pf->hw, p, &config, NULL);
	if (status) {
		netdev_info(netdev, "Set phy config failed,\n");
		err = -EAGAIN;
		goto done;
	}

	/* Save speed request */
	p->phy.curr_user_speed_req = adv_link_speed;
done:
	kfree(abilities);
	clear_bit(ICE_CFG_BUSY, pf->state);

	return err;
}
#else /* ETHTOOL_GLINKSETTINGS */

/**
 * ice_get_legacy_settings_link_up - Get the Link settings for when link is up
 * @ecmd: ethtool command to fill in
 * @netdev: network interface device structure
 *
 * Reports link settings that can be determined when link is up
 */
static void
ice_get_legacy_settings_link_up(struct ethtool_cmd *ecmd,
				struct net_device *netdev)
{
	struct ethtool_link_ksettings ks, cap_ks;
	struct ice_link_status *hw_link_info;
	struct ice_netdev_priv *np;
	struct ice_vsi *vsi;
	u64 phy_types_low;

	np = netdev_priv(netdev);
	vsi = np->vsi;
	hw_link_info = &vsi->port_info->phy.link_info;
	phy_types_low = hw_link_info->phy_type_low;

	/* Initialize supported and advertised settings based on PHY settings */
	switch (phy_types_low) {
	case ICE_PHY_TYPE_LOW_100BASE_TX:
	case ICE_PHY_TYPE_LOW_100M_SGMII:
		ecmd->supported = SUPPORTED_100baseT_Full;
		if (phy_types_low == ICE_PHY_TYPE_LOW_100M_SGMII) {
			ecmd->supported |= SUPPORTED_Autoneg;
			ecmd->advertising = ADVERTISED_Autoneg |
					    ADVERTISED_100baseT_Full;
		}
		break;
	case ICE_PHY_TYPE_LOW_1000BASE_T:
	case ICE_PHY_TYPE_LOW_1000BASE_SX:
	case ICE_PHY_TYPE_LOW_1000BASE_LX:
	case ICE_PHY_TYPE_LOW_1G_SGMII:
		ecmd->supported = SUPPORTED_1000baseT_Full;
		if (phy_types_low == ICE_PHY_TYPE_LOW_1000BASE_T) {
			ecmd->supported |= SUPPORTED_Autoneg;
			ecmd->advertising = ADVERTISED_Autoneg |
				ADVERTISED_1000baseT_Full;
		}
		break;
	case ICE_PHY_TYPE_LOW_1000BASE_KX:
		ecmd->supported = SUPPORTED_Autoneg |
			SUPPORTED_1000baseKX_Full;
		ecmd->advertising = ADVERTISED_Autoneg |
			ADVERTISED_1000baseKX_Full;
		break;
	case ICE_PHY_TYPE_LOW_2500BASE_T:
	case ICE_PHY_TYPE_LOW_2500BASE_X:
	case ICE_PHY_TYPE_LOW_2500BASE_KX:
		ecmd->supported = SUPPORTED_2500baseX_Full;
		if (phy_types_low == ICE_PHY_TYPE_LOW_2500BASE_T ||
		    phy_types_low == ICE_PHY_TYPE_LOW_2500BASE_KX) {
			ecmd->supported |= SUPPORTED_Autoneg;
			ecmd->advertising = ADVERTISED_Autoneg |
				ADVERTISED_2500baseX_Full;
		}
		break;
	case ICE_PHY_TYPE_LOW_10GBASE_T:
	case ICE_PHY_TYPE_LOW_10G_SFI_DA:
	case ICE_PHY_TYPE_LOW_10G_SFI_AOC_ACC:
	case ICE_PHY_TYPE_LOW_10G_SFI_C2C:
	case ICE_PHY_TYPE_LOW_10GBASE_SR:
	case ICE_PHY_TYPE_LOW_10GBASE_LR:
		ecmd->supported = SUPPORTED_10000baseT_Full;
		if (phy_types_low == ICE_PHY_TYPE_LOW_10GBASE_T) {
			ecmd->supported |= SUPPORTED_Autoneg;
			ecmd->advertising = ADVERTISED_Autoneg |
				ADVERTISED_10000baseT_Full;
		}
		break;
	case ICE_PHY_TYPE_LOW_10GBASE_KR_CR1:
		ecmd->supported = SUPPORTED_Autoneg |
			SUPPORTED_10000baseKR_Full;
		ecmd->advertising = ADVERTISED_Autoneg |
			ADVERTISED_10000baseKR_Full;
		break;
	case ICE_PHY_TYPE_LOW_40GBASE_CR4:
	case ICE_PHY_TYPE_LOW_40G_XLAUI_AOC_ACC:
	case ICE_PHY_TYPE_LOW_40G_XLAUI:
		ecmd->supported = SUPPORTED_40000baseCR4_Full;
		if (phy_types_low == ICE_PHY_TYPE_LOW_40GBASE_CR4) {
			ecmd->supported |= SUPPORTED_Autoneg;
			ecmd->advertising = ADVERTISED_Autoneg |
				ADVERTISED_40000baseCR4_Full;
		}
		break;
	case ICE_PHY_TYPE_LOW_40GBASE_SR4:
		ecmd->supported = SUPPORTED_40000baseSR4_Full;
		break;
	case ICE_PHY_TYPE_LOW_40GBASE_LR4:
		ecmd->supported = SUPPORTED_40000baseLR4_Full;
		break;
	case ICE_PHY_TYPE_LOW_40GBASE_KR4:
		ecmd->supported = SUPPORTED_Autoneg |
			SUPPORTED_40000baseKR4_Full;
		ecmd->advertising = ADVERTISED_Autoneg |
			ADVERTISED_40000baseKR4_Full;
		break;
	default:
		/* if we got here and link is up something bad is afoot */
		netdev_info(netdev, "WARNING: Link up but PhyType isn't recognized.\n");
		netdev_info(netdev, "WARNING: Unrecognized PHY_Low (0x%llx).\n",
			    (u64)phy_types_low);
	}

	/* Now that we've worked out everything that could be supported by the
	 * current PHY type, get what is supported by the NVM and and them to
	 * get what is truly supported
	 *
	 * ice_phy_type_to_ethtool uses the new API ethtool_link_ksettings
	 * struct, so we need to convert ecmd to a ksettings to intersect them,
	 * then convert back to legacy ethtool_cmd.
	 */
	ks.link_modes.supported[0] = ecmd->supported;
	ks.link_modes.advertising[0] = ecmd->advertising;
	ice_phy_type_to_ethtool(netdev, &cap_ks);
	ethtool_intersect_link_masks(&ks, &cap_ks);
	ecmd->supported = (u32)ks.link_modes.supported[0];
	ecmd->advertising = (u32)ks.link_modes.advertising[0];

	/* Set speed and duplex */
	switch (hw_link_info->link_speed) {
	case ICE_AQ_LINK_SPEED_40GB:
		ethtool_cmd_speed_set(ecmd, SPEED_40000);
		break;
	case ICE_AQ_LINK_SPEED_10GB:
		ethtool_cmd_speed_set(ecmd, SPEED_10000);
		break;
	case ICE_AQ_LINK_SPEED_2500MB:
		ethtool_cmd_speed_set(ecmd, SPEED_2500);
		break;
	case ICE_AQ_LINK_SPEED_1000MB:
		ethtool_cmd_speed_set(ecmd, SPEED_1000);
		break;
	case ICE_AQ_LINK_SPEED_100MB:
		ethtool_cmd_speed_set(ecmd, SPEED_100);
		break;
	default:
		netdev_info(netdev, "WARNING: Unrecognized link_speed (0x%x).\n",
			    hw_link_info->link_speed);
		break;
	}
	ecmd->duplex = DUPLEX_FULL;
}

/**
 * ice_get_legacy_settings_link_down - Get the Link settings when link is down
 * @ecmd: ethtool command to fill in
 * @netdev: network interface device structure
 *
 * Reports link settings that can be determined when link is down
 */
static void
ice_get_legacy_settings_link_down(struct ethtool_cmd *ecmd,
				  struct net_device *netdev)
{
	struct ethtool_link_ksettings ks;

	/* link is down and the driver needs to fall back on
	 * supported PHY types to figure out what info to display
	 */
	ice_phy_type_to_ethtool(netdev, &ks);
	ecmd->supported = (u32)ks.link_modes.supported[0];
	ecmd->advertising = (u32)ks.link_modes.advertising[0];

	ethtool_cmd_speed_set(ecmd, SPEED_UNKNOWN);
	ecmd->duplex = DUPLEX_UNKNOWN;
}

/**
 * ice_get_settings - Get Link Speed and Duplex settings
 * @netdev: network interface device structure
 * @ecmd: ethtool command
 *
 * Reports speed/duplex settings based on media_type
 */
static int ice_get_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_link_status *hw_link_info;
	struct ice_vsi *vsi = np->vsi;
	bool link_up;

	hw_link_info = &vsi->port_info->phy.link_info;
	link_up = hw_link_info->link_info & ICE_AQ_LINK_UP;


	/* set speed and duplex */
	if (link_up)
		ice_get_legacy_settings_link_up(ecmd, netdev);
	else
		ice_get_legacy_settings_link_down(ecmd, netdev);

	/* set autoneg settings */
	ecmd->autoneg = (hw_link_info->an_info & ICE_AQ_AN_COMPLETED) ?
		AUTONEG_ENABLE : AUTONEG_DISABLE;

	/* Set media type settings */
	switch (vsi->port_info->phy.media_type) {
	case ICE_MEDIA_FIBER:
		ecmd->supported |= SUPPORTED_FIBRE;
		ecmd->port = PORT_FIBRE;
		break;
	case ICE_MEDIA_BASET:
		ecmd->supported |= SUPPORTED_TP;
		ecmd->advertising |= ADVERTISED_TP;
		ecmd->port = PORT_TP;
		break;
	case ICE_MEDIA_BACKPLANE:
		ecmd->supported |= SUPPORTED_Autoneg | SUPPORTED_Backplane;
		ecmd->advertising |= ADVERTISED_Autoneg | ADVERTISED_Backplane;
		ecmd->port = PORT_NONE;
		break;
	case ICE_MEDIA_DA:
		ecmd->supported |= SUPPORTED_FIBRE;
		ecmd->advertising |= ADVERTISED_FIBRE;
		ecmd->port = PORT_DA;
		break;
	default:
		ecmd->port = PORT_OTHER;
		break;
	}

	ecmd->transceiver = XCVR_EXTERNAL;

	/* flow control is symmetric and always supported */
	ecmd->supported |= SUPPORTED_Pause;

	switch (vsi->port_info->fc.req_mode) {
	case ICE_FC_RX_PAUSE:
		ecmd->advertising |= ADVERTISED_Pause | ADVERTISED_Asym_Pause;
		break;
	case ICE_FC_TX_PAUSE:
		ecmd->advertising |= ADVERTISED_Asym_Pause;
		break;
	case ICE_FC_FULL:
		ecmd->advertising |= ADVERTISED_Pause;
		break;
	case ICE_FC_PFC:
	default:
		ecmd->advertising &= ~(ADVERTISED_Pause |
				       ADVERTISED_Asym_Pause);
		break;
	}
	return 0;
}

/**
 * ice_legacy_find_adv_link_speed
 * @advertise_phy: advertisement PHY value
 *
 * Find advertising link speed
 */
static u16
ice_legacy_find_adv_link_speed(u32 advertise_phy)
{
	u16 adv_link_speed = 0;

	if (advertise_phy & ADVERTISED_100baseT_Full)
		adv_link_speed |= ICE_AQ_LINK_SPEED_100MB;
	if (advertise_phy & ADVERTISED_1000baseT_Full ||
	    advertise_phy & ADVERTISED_1000baseKX_Full)
		adv_link_speed |= ICE_AQ_LINK_SPEED_1000MB;
	if (advertise_phy & ADVERTISED_2500baseX_Full)
		adv_link_speed |= ICE_AQ_LINK_SPEED_2500MB;
	if (advertise_phy & ADVERTISED_10000baseT_Full ||
	    advertise_phy & ADVERTISED_10000baseKR_Full)
		adv_link_speed |= ICE_AQ_LINK_SPEED_10GB;
	if (advertise_phy & ADVERTISED_40000baseKR4_Full ||
	    advertise_phy & ADVERTISED_40000baseCR4_Full ||
	    advertise_phy & ADVERTISED_40000baseSR4_Full ||
	    advertise_phy & ADVERTISED_40000baseLR4_Full)
		adv_link_speed |= ICE_AQ_LINK_SPEED_40GB;

	return adv_link_speed;
}

/**
 * ice_setup_autoneg
 * @p: port info
 * @ecmd: ethtool command
 * @config: configuration that will be sent down to FW
 * @autoneg_enabled: autonegotiation is enabled or not
 * @autoneg_changed: will there a change in autonegotiation
 * @netdev: network interface device structure
 *
 * Setup PHY autonegotiation feature
 */
static int
ice_setup_autoneg(struct ice_port_info *p, struct ethtool_cmd *ecmd,
		  struct ice_aqc_set_phy_cfg_data *config,
		  u8 autoneg_enabled, u8 *autoneg_changed,
		  struct net_device *netdev)
{
	int err = 0;

	*autoneg_changed = 0;

	if (autoneg_enabled == AUTONEG_ENABLE) {
		/* If autoneg was not already enabled */
		if (!(p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED)) {
			/* If autoneg is not supported, return error */
			if (!(ecmd->supported & SUPPORTED_Autoneg)) {
				netdev_info(netdev, "Autoneg not supported on this phy.\n");
				return -EINVAL;
			}

			/* Autoneg is allowed to change */
			config->caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
			*autoneg_changed = 1;
		}
	} else {
		/* If autoneg is currently enabled */
		if (p->phy.link_info.an_info & ICE_AQ_AN_COMPLETED) {
			/* If autoneg is supported 10GBASE_T is the only PHY
			 * that can disable it, so otherwise return error
			 */
			if (ecmd->supported & SUPPORTED_Autoneg) {
				netdev_info(netdev, "Autoneg cannot be disabled.\n");
				return -EINVAL;
			}

			/* Autoneg is allowed to change */
			config->caps &= ~ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;
			*autoneg_changed = 1;
		}
	}

	return err;
}

/**
 * ice_set_settings - Set Speed and Duplex
 * @netdev: network interface device structure
 * @ecmd: ethtool command
 *
 * Set speed/duplex per media_types advertised/forced
 */
static int ice_set_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_aqc_get_phy_caps_data *abilities;
	u8 autoneg, timeout = TEST_SET_BITS_TIMEOUT;
	u16 adv_link_speed, curr_link_speed, idx;
	struct ice_aqc_set_phy_cfg_data config;
	struct ice_pf *pf = np->vsi->back;
	struct ethtool_cmd safe_ecmd;
	struct ice_port_info *p;
	u8 autoneg_changed = 0;
	enum ice_status status;
	u64 phy_type_high = 0;
	u64 phy_type_low = 0;
	u32 advertise;
	int err = 0;
	bool linkup;

	p = np->vsi->port_info;

	if (!p)
		return -EOPNOTSUPP;

	/* Check if this is LAN VSI */
	ice_for_each_vsi(pf, idx)
		if (pf->vsi[idx]->type == ICE_VSI_PF) {
			if (np->vsi != pf->vsi[idx])
				return -EOPNOTSUPP;
			break;
		}

	if (p->phy.media_type != ICE_MEDIA_BASET &&
	    p->phy.media_type != ICE_MEDIA_FIBER &&
	    p->phy.media_type != ICE_MEDIA_BACKPLANE &&
	    p->phy.media_type != ICE_MEDIA_DA &&
	    p->phy.link_info.link_info & ICE_AQ_LINK_UP)
		return -EOPNOTSUPP;

	/* get our own copy of the bits to check against */
	memset(&safe_ecmd, 0, sizeof(safe_ecmd));
	ice_get_settings(netdev, &safe_ecmd);

	/* save autoneg and speed out of ecmd */
	autoneg = ecmd->autoneg;
	advertise = ecmd->advertising;

	/* set autoneg and speed back to what they currently are */
	ecmd->autoneg = safe_ecmd.autoneg;
	ecmd->speed = safe_ecmd.speed;
	ecmd->advertising = safe_ecmd.advertising;
	ecmd->cmd = safe_ecmd.cmd;

	/* If ecmd and safe_ecmd are not the same now, then they are
	 * trying to set something that we do not support
	 */
	if (memcmp(ecmd, &safe_ecmd, sizeof(*ecmd)))
		return -EOPNOTSUPP;

	while (test_and_set_bit(ICE_CFG_BUSY, pf->state)) {
		timeout--;
		if (!timeout)
			return -EBUSY;
		usleep_range(TEST_SET_BITS_SLEEP_MIN, TEST_SET_BITS_SLEEP_MAX);
	}

	abilities = kzalloc(sizeof(*abilities), GFP_KERNEL);
	if (!abilities)
		return -ENOMEM;

	/* Get the current PHY config */
	status = ice_aq_get_phy_caps(p, false, ICE_AQC_REPORT_TOPO_CAP,
				     abilities, NULL);
	if (status) {
		err = -EAGAIN;
		goto done;
	}

	/* Copy the current user PHY configuration. The current user PHY
	 * configuration is initialized during probe from PHY capabilities
	 * software mode, and updated on set PHY configuration.
	 */
	memcpy(&config, &p->phy.curr_user_phy_cfg, sizeof(config));

	config.caps |= ICE_AQ_PHY_ENA_AUTO_LINK_UPDT;

	/* Check autoneg */
	err = ice_setup_autoneg(p, &safe_ecmd, &config, autoneg,
				&autoneg_changed, netdev);
	if (err)
		goto done;

	if (advertise & ~safe_ecmd.supported) {
		err = -EINVAL;
		goto done;
	}

	/* Call to get the current link speed */
	p->phy.get_link_info = true;
	status = ice_get_link_status(p, &linkup);
	if (status) {
		err = -EAGAIN;
		goto done;
	}

	curr_link_speed = p->phy.link_info.link_speed;
	adv_link_speed = ice_legacy_find_adv_link_speed(advertise);

	/* If speed didn't get set, set it to what it currently is.
	 * This is needed because if advertise is 0 (as it is when autoneg
	 * is disabled) then speed won't get set.
	 */
	if (!adv_link_speed)
		adv_link_speed = curr_link_speed;

	/* Convert the advertise link speeds to their corresponded PHY_TYPE */
	ice_update_phy_type(&phy_type_low, &phy_type_high, adv_link_speed);

	if (!autoneg_changed && adv_link_speed == curr_link_speed) {
		netdev_info(netdev, "Nothing changed, exiting without setting anything.\n");
		goto done;
	}

	/* save the requested speeds */
	p->phy.link_info.req_speeds = adv_link_speed;

	/* set link and auto negotiation so changes take effect */
	config.caps |= ICE_AQ_PHY_ENA_LINK;

	if (phy_type_low || phy_type_high) {
		config.phy_type_high = cpu_to_le64(phy_type_high) &
			abilities->phy_type_high;
		config.phy_type_low = cpu_to_le64(phy_type_low) &
			abilities->phy_type_low;
	} else {
		netdev_info(netdev, "Nothing changed. No PHY_TYPE is corresponded to advertised link speed.\n");
		goto done;
	}

	/* If link is up, put link down */
	if (p->phy.link_info.link_info & ICE_AQ_LINK_UP) {
		/* Tell the OS link is going down, the link will go
		 * back up when FW says it is ready asynchronously
		 */
		ice_print_link_msg(np->vsi, false);
		netif_carrier_off(netdev);
		netif_tx_stop_all_queues(netdev);
	}

	/* make the AQ call */
	status = ice_aq_set_phy_cfg(&pf->hw, p, &config, NULL);
	if (status) {
		netdev_info(netdev, "Set phy config failed,\n");
		err = -EAGAIN;
		goto done;
	}

	/* Save speed request */
	p->phy.curr_user_speed_req = adv_link_speed;
done:
	kfree(abilities);
	clear_bit(ICE_CFG_BUSY, pf->state);

	return err;
}
#endif /* ETHTOOL_GLINKSETTINGS */

/**
 * ice_parse_hdrs - parses headers from RSS hash input
 * @nfc: ethtool rxnfc command
 *
 * This function parses the rxnfc command and returns intended
 * header types for RSS configuration
 */
static u32 ice_parse_hdrs(struct ethtool_rxnfc *nfc)
{
	u32 hdrs = ICE_FLOW_SEG_HDR_NONE;

	switch (nfc->flow_type) {
	case TCP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_TCP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case UDP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_UDP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case SCTP_V4_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_SCTP | ICE_FLOW_SEG_HDR_IPV4;
		break;
	case TCP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_TCP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case UDP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_UDP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	case SCTP_V6_FLOW:
		hdrs |= ICE_FLOW_SEG_HDR_SCTP | ICE_FLOW_SEG_HDR_IPV6;
		break;
	default:
		break;
	}
	return hdrs;
}

#define ICE_FLOW_HASH_FLD_IPV4_SA	BIT_ULL(ICE_FLOW_FIELD_IDX_IPV4_SA)
#define ICE_FLOW_HASH_FLD_IPV6_SA	BIT_ULL(ICE_FLOW_FIELD_IDX_IPV6_SA)
#define ICE_FLOW_HASH_FLD_IPV4_DA	BIT_ULL(ICE_FLOW_FIELD_IDX_IPV4_DA)
#define ICE_FLOW_HASH_FLD_IPV6_DA	BIT_ULL(ICE_FLOW_FIELD_IDX_IPV6_DA)
#define ICE_FLOW_HASH_FLD_TCP_SRC_PORT	BIT_ULL(ICE_FLOW_FIELD_IDX_TCP_SRC_PORT)
#define ICE_FLOW_HASH_FLD_TCP_DST_PORT	BIT_ULL(ICE_FLOW_FIELD_IDX_TCP_DST_PORT)
#define ICE_FLOW_HASH_FLD_UDP_SRC_PORT	BIT_ULL(ICE_FLOW_FIELD_IDX_UDP_SRC_PORT)
#define ICE_FLOW_HASH_FLD_UDP_DST_PORT	BIT_ULL(ICE_FLOW_FIELD_IDX_UDP_DST_PORT)
#define ICE_FLOW_HASH_FLD_SCTP_SRC_PORT	\
	BIT_ULL(ICE_FLOW_FIELD_IDX_SCTP_SRC_PORT)
#define ICE_FLOW_HASH_FLD_SCTP_DST_PORT	\
	BIT_ULL(ICE_FLOW_FIELD_IDX_SCTP_DST_PORT)

/**
 * ice_parse_hash_flds - parses hash fields from RSS hash input
 * @nfc: ethtool rxnfc command
 *
 * This function parses the rxnfc command and returns intended
 * hash fields for RSS configuration
 */
static u64 ice_parse_hash_flds(struct ethtool_rxnfc *nfc)
{
	u64 hfld = ICE_HASH_INVALID;

	if (nfc->data & RXH_IP_SRC || nfc->data & RXH_IP_DST) {
		switch (nfc->flow_type) {
		case TCP_V4_FLOW:
		case UDP_V4_FLOW:
		case SCTP_V4_FLOW:
			if (nfc->data & RXH_IP_SRC)
				hfld |= ICE_FLOW_HASH_FLD_IPV4_SA;
			if (nfc->data & RXH_IP_DST)
				hfld |= ICE_FLOW_HASH_FLD_IPV4_DA;
			break;
		case TCP_V6_FLOW:
		case UDP_V6_FLOW:
		case SCTP_V6_FLOW:
			if (nfc->data & RXH_IP_SRC)
				hfld |= ICE_FLOW_HASH_FLD_IPV6_SA;
			if (nfc->data & RXH_IP_DST)
				hfld |= ICE_FLOW_HASH_FLD_IPV6_DA;
			break;
		default:
			break;
		}
	}

	if (nfc->data & RXH_L4_B_0_1 || nfc->data & RXH_L4_B_2_3) {
		switch (nfc->flow_type) {
		case TCP_V4_FLOW:
		case TCP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_TCP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_TCP_DST_PORT;
			break;
		case UDP_V4_FLOW:
		case UDP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_UDP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_UDP_DST_PORT;
			break;
		case SCTP_V4_FLOW:
		case SCTP_V6_FLOW:
			if (nfc->data & RXH_L4_B_0_1)
				hfld |= ICE_FLOW_HASH_FLD_SCTP_SRC_PORT;
			if (nfc->data & RXH_L4_B_2_3)
				hfld |= ICE_FLOW_HASH_FLD_SCTP_DST_PORT;
			break;
		default:
			break;
		}
	}

	return hfld;
}

/**
 * ice_set_rss_hash_opt - Enable/Disable flow types for RSS hash
 * @vsi: the VSI being configured
 * @nfc: ethtool rxnfc command
 *
 * Returns Success if the flow input set is supported.
 */
static int
ice_set_rss_hash_opt(struct ice_vsi *vsi, struct ethtool_rxnfc *nfc)
{
	struct ice_pf *pf = vsi->back;
	struct ice_rss_hash_cfg cfg;
	enum ice_status status;
	struct device *dev;
	u64 hashed_flds;
	u32 hdrs;

	dev = ice_pf_to_dev(pf);
	if (ice_is_safe_mode(pf)) {
		dev_dbg(dev, "Advanced RSS disabled. Package download failed, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	hashed_flds = ice_parse_hash_flds(nfc);
	if (hashed_flds == ICE_HASH_INVALID) {
		dev_dbg(dev, "Invalid hash fields, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	hdrs = ice_parse_hdrs(nfc);
	if (hdrs == ICE_FLOW_SEG_HDR_NONE) {
		dev_dbg(dev, "Header type is not valid, vsi num = %d\n",
			vsi->vsi_num);
		return -EINVAL;
	}

	cfg.hash_flds = hashed_flds;
	cfg.addl_hdrs = hdrs;
	cfg.hdr_type = ICE_RSS_ANY_HEADERS;
	cfg.symm = false;
	status = ice_add_rss_cfg(&pf->hw, vsi->idx, &cfg);
	if (status) {
		dev_dbg(dev, "ice_add_rss_cfg failed, vsi num = %d, error = %s\n",
			vsi->vsi_num, ice_stat_str(status));
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_get_rss_hash_opt - Retrieve hash fields for a given flow-type
 * @vsi: the VSI being configured
 * @nfc: ethtool rxnfc command
 */
static void
ice_get_rss_hash_opt(struct ice_vsi *vsi, struct ethtool_rxnfc *nfc)
{
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	u64 hash_flds;
	u32 hdrs;

	dev = ice_pf_to_dev(pf);

	nfc->data = 0;
	if (ice_is_safe_mode(pf)) {
		dev_dbg(dev, "Advanced RSS disabled. Package download failed, vsi num = %d\n",
			vsi->vsi_num);
		return;
	}

	hdrs = ice_parse_hdrs(nfc);
	if (hdrs == ICE_FLOW_SEG_HDR_NONE) {
		dev_dbg(dev, "Header type is not valid, vsi num = %d\n",
			vsi->vsi_num);
		return;
	}

	hash_flds = ice_get_rss_cfg(&pf->hw, vsi->idx, hdrs);
	if (hash_flds == ICE_HASH_INVALID) {
		dev_dbg(dev, "No hash fields found for the given header type, vsi num = %d\n",
			vsi->vsi_num);
		return;
	}

	if (hash_flds & ICE_FLOW_HASH_FLD_IPV4_SA ||
	    hash_flds & ICE_FLOW_HASH_FLD_IPV6_SA)
		nfc->data |= (u64)RXH_IP_SRC;

	if (hash_flds & ICE_FLOW_HASH_FLD_IPV4_DA ||
	    hash_flds & ICE_FLOW_HASH_FLD_IPV6_DA)
		nfc->data |= (u64)RXH_IP_DST;

	if (hash_flds & ICE_FLOW_HASH_FLD_TCP_SRC_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_UDP_SRC_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_SCTP_SRC_PORT)
		nfc->data |= (u64)RXH_L4_B_0_1;

	if (hash_flds & ICE_FLOW_HASH_FLD_TCP_DST_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_UDP_DST_PORT ||
	    hash_flds & ICE_FLOW_HASH_FLD_SCTP_DST_PORT)
		nfc->data |= (u64)RXH_L4_B_2_3;
}

/**
 * ice_set_rxnfc - command to set Rx flow rules.
 * @netdev: network interface device structure
 * @cmd: ethtool rxnfc command
 *
 * Returns 0 for success and negative values for errors
 */
static int ice_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		return ice_add_ntuple_ethtool(vsi, cmd);
	case ETHTOOL_SRXCLSRLDEL:
		return ice_del_ntuple_ethtool(vsi, cmd);
	case ETHTOOL_SRXFH:
		return ice_set_rss_hash_opt(vsi, cmd);
	default:
		break;
	}
	return -EOPNOTSUPP;
}

/**
 * ice_get_rxnfc - command to get Rx flow classification rules
 * @netdev: network interface device structure
 * @cmd: ethtool rxnfc command
 * @rule_locs: buffer to rturn Rx flow classification rules
 *
 * Returns Success if the command is supported.
 */
static int
ice_get_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd,
	      u32 __always_unused *rule_locs)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	int ret = -EOPNOTSUPP;
	struct ice_hw *hw;

	hw = &vsi->back->hw;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = vsi->rss_size;
		ret = 0;
		break;
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = hw->fdir_active_fltr;
		/* report max rule count */
		cmd->data = ice_ntuple_get_max_fltr_cnt(hw);
		ret = 0;
		break;
	case ETHTOOL_GRXCLSRULE:
		ret = ice_get_ethtool_fdir_entry(hw, cmd);
		break;
	case ETHTOOL_GRXCLSRLALL:
		ret = ice_get_fdir_fltr_ids(hw, cmd, (u32 *)rule_locs);
		break;
	case ETHTOOL_GRXFH:
		ice_get_rss_hash_opt(vsi, cmd);
		ret = 0;
		break;
	default:
		break;
	}

	return ret;
}

static void
ice_get_ringparam(struct net_device *netdev, struct ethtool_ringparam *ring)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	ring->rx_max_pending = ICE_MAX_NUM_DESC;
	ring->tx_max_pending = ICE_MAX_NUM_DESC;
	ring->rx_pending = vsi->rx_rings[0]->count;
	ring->tx_pending = vsi->tx_rings[0]->count;

	/* Rx mini and jumbo rings are not supported */
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int
ice_set_ringparam(struct net_device *netdev, struct ethtool_ringparam *ring)
{
	struct ice_ring *tx_rings = NULL, *rx_rings = NULL;
	struct ice_netdev_priv *np = netdev_priv(netdev);
#ifdef HAVE_XDP_SUPPORT
	struct ice_ring *xdp_rings = NULL;
#endif /* HAVE_XDP_SUPPORT */
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	int i, timeout = 50, err = 0;
	u16 new_rx_cnt, new_tx_cnt;

	if (ring->tx_pending > ICE_MAX_NUM_DESC ||
	    ring->tx_pending < ICE_MIN_NUM_DESC ||
	    ring->rx_pending > ICE_MAX_NUM_DESC ||
	    ring->rx_pending < ICE_MIN_NUM_DESC) {
		netdev_err(netdev, "Descriptors requested (Tx: %d / Rx: %d) out of range [%d-%d] (increment %d)\n",
			   ring->tx_pending, ring->rx_pending,
			   ICE_MIN_NUM_DESC, ICE_MAX_NUM_DESC,
			   ICE_REQ_DESC_MULTIPLE);
		return -EINVAL;
	}

	new_tx_cnt = ALIGN(ring->tx_pending, ICE_REQ_DESC_MULTIPLE);
	if (new_tx_cnt != ring->tx_pending)
		netdev_info(netdev, "Requested Tx descriptor count rounded up to %d\n",
			    new_tx_cnt);
	new_rx_cnt = ALIGN(ring->rx_pending, ICE_REQ_DESC_MULTIPLE);
	if (new_rx_cnt != ring->rx_pending)
		netdev_info(netdev, "Requested Rx descriptor count rounded up to %d\n",
			    new_rx_cnt);

	/* if nothing to do return success */
	if (new_tx_cnt == vsi->tx_rings[0]->count &&
	    new_rx_cnt == vsi->rx_rings[0]->count) {
		netdev_dbg(netdev, "Nothing to change, descriptor count is same as requested\n");
		return 0;
	}

#ifdef HAVE_AF_XDP_ZC_SUPPORT
	/* If there is a AF_XDP UMEM attached to any of Rx rings,
	 * disallow changing the number of descriptors -- regardless
	 * if the netdev is running or not.
	 */
	if (ice_xsk_any_rx_ring_ena(vsi))
		return -EBUSY;

#endif /* HAVE_AF_XDP_ZC_SUPPORT */
	while (test_and_set_bit(ICE_CFG_BUSY, pf->state)) {
		timeout--;
		if (!timeout)
			return -EBUSY;
		usleep_range(1000, 2000);
	}

	/* set for the next time the netdev is started */
	if (!netif_running(vsi->netdev)) {
		for (i = 0; i < vsi->alloc_txq; i++)
			vsi->tx_rings[i]->count = new_tx_cnt;
		for (i = 0; i < vsi->alloc_rxq; i++)
			vsi->rx_rings[i]->count = new_rx_cnt;
#ifdef HAVE_XDP_SUPPORT
		if (ice_is_xdp_ena_vsi(vsi))
			for (i = 0; i < vsi->num_xdp_txq; i++)
				vsi->xdp_rings[i]->count = new_tx_cnt;
#endif /* HAVE_XDP_SUPPORT */
		vsi->num_tx_desc = (u16)new_tx_cnt;
		vsi->num_rx_desc = (u16)new_rx_cnt;
		netdev_dbg(netdev, "Link is down, descriptor count change happens when link is brought up\n");
		goto done;
	}

	if (new_tx_cnt == vsi->tx_rings[0]->count)
		goto process_rx;

	/* alloc updated Tx resources */
	netdev_info(netdev, "Changing Tx descriptor count from %d to %d\n",
		    vsi->tx_rings[0]->count, new_tx_cnt);

	tx_rings = kcalloc(vsi->num_txq, sizeof(*tx_rings), GFP_KERNEL);
	if (!tx_rings) {
		err = -ENOMEM;
		goto done;
	}

	ice_for_each_txq(vsi, i) {
		/* clone ring and setup updated count */
		tx_rings[i] = *vsi->tx_rings[i];
		tx_rings[i].count = new_tx_cnt;
		tx_rings[i].desc = NULL;
		tx_rings[i].tx_buf = NULL;
		err = ice_setup_tx_ring(&tx_rings[i]);
		if (err) {
			while (i--)
				ice_clean_tx_ring(&tx_rings[i]);
			kfree(tx_rings);
			goto done;
		}
	}

#ifdef HAVE_XDP_SUPPORT
	if (!ice_is_xdp_ena_vsi(vsi))
		goto process_rx;

	/* alloc updated XDP resources */
	netdev_info(netdev, "Changing XDP descriptor count from %d to %d\n",
		    vsi->xdp_rings[0]->count, new_tx_cnt);

	xdp_rings = kcalloc(vsi->num_xdp_txq, sizeof(*xdp_rings), GFP_KERNEL);
	if (!xdp_rings) {
		err = -ENOMEM;
		goto free_tx;
	}

	for (i = 0; i < vsi->num_xdp_txq; i++) {
		/* clone ring and setup updated count */
		xdp_rings[i] = *vsi->xdp_rings[i];
		xdp_rings[i].count = new_tx_cnt;
		xdp_rings[i].desc = NULL;
		xdp_rings[i].tx_buf = NULL;
		err = ice_setup_tx_ring(&xdp_rings[i]);
		if (err) {
			while (i--)
				ice_clean_tx_ring(&xdp_rings[i]);
			kfree(xdp_rings);
			goto free_tx;
		}
		ice_set_ring_xdp(&xdp_rings[i]);
	}
#endif /* HAVE_XDP_SUPPORT */

process_rx:
	if (new_rx_cnt == vsi->rx_rings[0]->count)
		goto process_link;

	/* alloc updated Rx resources */
	netdev_info(netdev, "Changing Rx descriptor count from %d to %d\n",
		    vsi->rx_rings[0]->count, new_rx_cnt);

	rx_rings = kcalloc(vsi->num_rxq, sizeof(*rx_rings), GFP_KERNEL);
	if (!rx_rings) {
		err = -ENOMEM;
		goto done;
	}

	ice_for_each_rxq(vsi, i) {
		/* clone ring and setup updated count */
		rx_rings[i] = *vsi->rx_rings[i];
		rx_rings[i].count = new_rx_cnt;
		rx_rings[i].desc = NULL;
		rx_rings[i].rx_buf = NULL;
		/* this is to allow wr32 to have something to write to
		 * during early allocation of Rx buffers
		 */
		rx_rings[i].tail = vsi->back->hw.hw_addr + PRTGEN_STATUS;

		err = ice_setup_rx_ring(&rx_rings[i]);
		if (err)
			goto rx_unwind;

		/* allocate Rx buffers */
		err = ice_alloc_rx_bufs(&rx_rings[i],
					ICE_DESC_UNUSED(&rx_rings[i]));
rx_unwind:
		if (err) {
			while (i) {
				i--;
				ice_free_rx_ring(&rx_rings[i]);
			}
			kfree(rx_rings);
			err = -ENOMEM;
			goto free_tx;
		}
	}

process_link:
	/* Bring interface down, copy in the new ring info, then restore the
	 * interface. if VSI is up, bring it down and then back up
	 */
	if (!test_and_set_bit(ICE_VSI_DOWN, vsi->state)) {
		ice_down(vsi);

		if (tx_rings) {
			ice_for_each_txq(vsi, i) {
				ice_free_tx_ring(vsi->tx_rings[i]);
				*vsi->tx_rings[i] = tx_rings[i];
			}
			kfree(tx_rings);
		}

		if (rx_rings) {
			ice_for_each_rxq(vsi, i) {
				ice_free_rx_ring(vsi->rx_rings[i]);
				/* copy the real tail offset */
				rx_rings[i].tail = vsi->rx_rings[i]->tail;
				/* this is to fake out the allocation routine
				 * into thinking it has to realloc everything
				 * but the recycling logic will let us re-use
				 * the buffers allocated above
				 */
				rx_rings[i].next_to_use = 0;
				rx_rings[i].next_to_clean = 0;
				rx_rings[i].next_to_alloc = 0;
				*vsi->rx_rings[i] = rx_rings[i];
			}
			kfree(rx_rings);
		}

#ifdef HAVE_XDP_SUPPORT
		if (xdp_rings) {
			for (i = 0; i < vsi->num_xdp_txq; i++) {
				ice_free_tx_ring(vsi->xdp_rings[i]);
				*vsi->xdp_rings[i] = xdp_rings[i];
			}
			kfree(xdp_rings);
		}
#endif /* HAVE_XDP_SUPPORT */

		vsi->num_tx_desc = new_tx_cnt;
		vsi->num_rx_desc = new_rx_cnt;
		ice_up(vsi);
	}
	goto done;

free_tx:
	/* error cleanup if the Rx allocations failed after getting Tx */
	if (tx_rings) {
		ice_for_each_txq(vsi, i)
			ice_free_tx_ring(&tx_rings[i]);
		kfree(tx_rings);
	}

done:
	clear_bit(ICE_CFG_BUSY, pf->state);
	return err;
}

#ifdef ETHTOOL_GLINKSETTINGS
/**
 * ice_get_pauseparam - Get Flow Control status
 * @netdev: network interface device structure
 * @pause: ethernet pause (flow control) parameters
 *
 * Get requested flow control status from PHY capability.
 * If autoneg is true, then ethtool will send the ETHTOOL_GSET ioctl which
 * is handled by ice_get_link_ksettings. ice_get_link_ksettings will report
 * the negotiated Rx/Tx pause via lp_advertising.
 */
static void
ice_get_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *pause)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_info *pi = np->vsi->port_info;
	struct ice_aqc_get_phy_caps_data *pcaps;
	struct ice_dcbx_cfg *dcbx_cfg;
	enum ice_status status;

	/* Initialize pause params */
	pause->rx_pause = 0;
	pause->tx_pause = 0;

	dcbx_cfg = &pi->qos_cfg.local_dcbx_cfg;

	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return;

	/* Get current PHY config */
	status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_SW_CFG, pcaps,
				     NULL);
	if (status)
		goto out;

	pause->autoneg = ice_is_phy_caps_an_enabled(pcaps) ? AUTONEG_ENABLE :
							     AUTONEG_DISABLE;

	if (dcbx_cfg->pfc.pfcena)
		/* PFC enabled so report LFC as off */
		goto out;

	if (pcaps->caps & ICE_AQC_PHY_EN_TX_LINK_PAUSE)
		pause->tx_pause = 1;
	if (pcaps->caps & ICE_AQC_PHY_EN_RX_LINK_PAUSE)
		pause->rx_pause = 1;

out:
	kfree(pcaps);
}
#else /* ETHTOOL_GLINKSETTINGS */

/**
 * ice_get_pauseparam - Get Flow Control status
 * @netdev: network interface device structure
 * @pause: ethernet pause (flow control) parameters
 *
 * Get autonegotiated flow control status from link status.
 */
static void
ice_get_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *pause)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_port_info *pi = np->vsi->port_info;
	struct ice_link_status *hw_link_info;
	struct ice_dcbx_cfg *dcbx_cfg;

	hw_link_info = &pi->phy.link_info;

	/* Initialize pause params */
	pause->rx_pause = 0;
	pause->tx_pause = 0;

	pause->autoneg = ((hw_link_info->an_info & ICE_AQ_AN_COMPLETED) ?
			  AUTONEG_ENABLE : AUTONEG_DISABLE);

	dcbx_cfg = &pi->qos_cfg.local_dcbx_cfg;

	if (dcbx_cfg->pfc.pfcena)
		/* PFC enabled so report LFC as off */
		return;

	/* Get flow control status based on autonegotiation */
	switch (pi->fc.current_mode) {
	case ICE_FC_TX_PAUSE:
		pause->tx_pause = 1;
		break;
	case ICE_FC_RX_PAUSE:
		pause->rx_pause = 1;
		break;
	case ICE_FC_FULL:
		pause->tx_pause = 1;
		pause->rx_pause = 1;
		break;
	default:
		break;
	}
}
#endif /* ETHTOOL_GLINKSETTINGS */

/**
 * ice_set_pauseparam - Set Flow Control parameter
 * @netdev: network interface device structure
 * @pause: return Tx/Rx flow control status
 */
static int
ice_set_pauseparam(struct net_device *netdev, struct ethtool_pauseparam *pause)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
#ifdef ETHTOOL_GLINKSETTINGS
	struct ice_aqc_get_phy_caps_data *pcaps;
#endif
	struct ice_link_status *hw_link_info;
	struct ice_pf *pf = np->vsi->back;
	struct ice_dcbx_cfg *dcbx_cfg;
	struct ice_vsi *vsi = np->vsi;
	struct ice_hw *hw = &pf->hw;
	struct ice_port_info *pi;
	enum ice_status status;
	u8 aq_failures;
	bool link_up;
	int err = 0;
	u32 is_an;

	pi = vsi->port_info;
	hw_link_info = &pi->phy.link_info;
	dcbx_cfg = &pi->qos_cfg.local_dcbx_cfg;
	link_up = hw_link_info->link_info & ICE_AQ_LINK_UP;

	/* Changing the port's flow control is not supported if this isn't the
	 * PF VSI
	 */
	if (vsi->type != ICE_VSI_PF) {
		netdev_info(netdev, "Changing flow control parameters only supported for PF VSI\n");
		return -EOPNOTSUPP;
	}

#ifdef ETHTOOL_GLINKSETTINGS
	/* Get pause param reports configured and negotiated flow control pause
	 * when ETHTOOL_GLINKSETTINGS is defined. Since ETHTOOL_GLINKSETTINGS is
	 * defined get pause param pause->autoneg reports SW configured setting,
	 * so compare pause->autoneg with SW configured to prevent the user from
	 * using set pause param to chance autoneg.
	 */
	pcaps = kzalloc(sizeof(*pcaps), GFP_KERNEL);
	if (!pcaps)
		return -ENOMEM;

	/* Get current PHY config */
	status = ice_aq_get_phy_caps(pi, false, ICE_AQC_REPORT_SW_CFG, pcaps,
				     NULL);
	if (status) {
		kfree(pcaps);
		return -EIO;
	}

	is_an = ice_is_phy_caps_an_enabled(pcaps) ? AUTONEG_ENABLE :
						    AUTONEG_DISABLE;
#else /* ETHTOOL_GLINKSETTINGS */
	is_an = ((hw_link_info->an_info & ICE_AQ_AN_COMPLETED) ?
			AUTONEG_ENABLE : AUTONEG_DISABLE);
#endif /* ETHTOOL_GLINKSETTINGS */

#ifdef ETHTOOL_GLINKSETTINGS
	kfree(pcaps);
#endif

	if (pause->autoneg != is_an) {
		netdev_info(netdev, "To change autoneg please use: ethtool -s <dev> autoneg <on|off>\n");
		return -EOPNOTSUPP;
	}

	/* If we have link and don't have autoneg */
	if (!test_bit(ICE_DOWN, pf->state) &&
	    !(hw_link_info->an_info & ICE_AQ_AN_COMPLETED)) {
		/* Send message that it might not necessarily work*/
		netdev_info(netdev, "Autoneg did not complete so changing settings may not result in an actual change.\n");
	}

	if (dcbx_cfg->pfc.pfcena) {
		netdev_info(netdev, "Priority flow control enabled. Cannot set link flow control.\n");
		return -EOPNOTSUPP;
	}
	if (pause->rx_pause && pause->tx_pause)
		pi->fc.req_mode = ICE_FC_FULL;
	else if (pause->rx_pause && !pause->tx_pause)
		pi->fc.req_mode = ICE_FC_RX_PAUSE;
	else if (!pause->rx_pause && pause->tx_pause)
		pi->fc.req_mode = ICE_FC_TX_PAUSE;
	else if (!pause->rx_pause && !pause->tx_pause)
		pi->fc.req_mode = ICE_FC_NONE;
	else
		return -EINVAL;

	/* Set the FC mode and only restart AN if link is up */
	status = ice_set_fc(pi, &aq_failures, link_up);

	if (aq_failures & ICE_SET_FC_AQ_FAIL_GET) {
		netdev_info(netdev, "Set fc failed on the get_phy_capabilities call with err %s aq_err %s\n",
			    ice_stat_str(status),
			    ice_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	} else if (aq_failures & ICE_SET_FC_AQ_FAIL_SET) {
		netdev_info(netdev, "Set fc failed on the set_phy_config call with err %s aq_err %s\n",
			    ice_stat_str(status),
			    ice_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	} else if (aq_failures & ICE_SET_FC_AQ_FAIL_UPDATE) {
		netdev_info(netdev, "Set fc failed on the get_link_info call with err %s aq_err %s\n",
			    ice_stat_str(status),
			    ice_aq_str(hw->adminq.sq_last_status));
		err = -EAGAIN;
	}

	return err;
}

#if defined(ETHTOOL_GRSSH) && defined(ETHTOOL_SRSSH)
/**
 * ice_get_rxfh_key_size - get the RSS hash key size
 * @netdev: network interface device structure
 *
 * Returns the table size.
 */
static u32 ice_get_rxfh_key_size(struct net_device __always_unused *netdev)
{
	return ICE_VSIQF_HKEY_ARRAY_SIZE;
}

/**
 * ice_get_rxfh_indir_size - get the Rx flow hash indirection table size
 * @netdev: network interface device structure
 *
 * Returns the table size.
 */
static u32 ice_get_rxfh_indir_size(struct net_device *netdev)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);

	return np->vsi->rss_table_size;
}

#ifdef HAVE_RXFH_HASHFUNC
/**
 * ice_get_rxfh - get the Rx flow hash indirection table
 * @netdev: network interface device structure
 * @indir: indirection table
 * @key: hash key
 * @hfunc: hash function
 *
 * Reads the indirection table directly from the hardware.
 */
static int
ice_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key, u8 *hfunc)
#else
static int ice_get_rxfh(struct net_device *netdev, u32 *indir, u8 *key)
#endif
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	int err, i;
	u8 *lut;

#ifdef HAVE_RXFH_HASHFUNC
	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;
#endif

	if (!indir)
		return 0;

	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
		/* RSS not supported return error here */
		netdev_warn(netdev, "RSS is not configured on this VSI!\n");
		return -EIO;
	}

	lut = kzalloc(vsi->rss_table_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	err = ice_get_rss_key(vsi, key);
	if (err)
		goto out;

	err = ice_get_rss_lut(vsi, lut, vsi->rss_table_size);
	if (err)
		goto out;

	for (i = 0; i < vsi->rss_table_size; i++)
		indir[i] = (u32)(lut[i]);

out:
	kfree(lut);
	return err;
}

#ifdef HAVE_RXFH_HASHFUNC
/**
 * ice_set_rxfh - set the Rx flow hash indirection table
 * @netdev: network interface device structure
 * @indir: indirection table
 * @key: hash key
 * @hfunc: hash function
 *
 * Returns -EINVAL if the table specifies an invalid queue ID, otherwise
 * returns 0 after programming the table.
 */
static int
ice_set_rxfh(struct net_device *netdev, const u32 *indir, const u8 *key,
	     const u8 hfunc)
#elif defined(HAVE_RXFH_NONCONST)
static int ice_set_rxfh(struct net_device *netdev, u32 *indir, u8 *key)
#else
static int
ice_set_rxfh(struct net_device *netdev, const u32 *indir, const u8 *key)
#endif /* HAVE_RXFH_HASHFUNC */
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	int err;

	dev = ice_pf_to_dev(pf);
#ifdef HAVE_RXFH_HASHFUNC
	if (hfunc != ETH_RSS_HASH_NO_CHANGE && hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;
#endif

	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags)) {
		/* RSS not supported return error here */
		netdev_warn(netdev, "RSS is not configured on this VSI!\n");
		return -EIO;
	}

	/* Verify user input. */
	if (indir) {
		int i;

		for (i = 0; i < vsi->rss_table_size; i++)
			if (indir[i] >= vsi->rss_size)
				return -EINVAL;
	}

#ifdef NETIF_F_HW_TC
	if (ice_is_adq_active(pf)) {
		netdev_err(netdev,
			   "Cannot change RSS params with ADQ configured.\n");
		return -EOPNOTSUPP;
	}

#endif /* NETIF_F_HW_TC */
	if (key) {
		if (!vsi->rss_hkey_user) {
			vsi->rss_hkey_user =
				devm_kzalloc(dev, ICE_VSIQF_HKEY_ARRAY_SIZE,
					     GFP_KERNEL);
			if (!vsi->rss_hkey_user)
				return -ENOMEM;
		}
		memcpy(vsi->rss_hkey_user, key, ICE_VSIQF_HKEY_ARRAY_SIZE);

		err = ice_set_rss_key(vsi, vsi->rss_hkey_user);
		if (err)
			return err;
	}

	if (!vsi->rss_lut_user) {
		vsi->rss_lut_user = devm_kzalloc(dev, vsi->rss_table_size,
						 GFP_KERNEL);
		if (!vsi->rss_lut_user)
			return -ENOMEM;
	}

	/* Each 32 bits pointed by 'indir' is stored with a lut entry */
	if (indir) {
		int i;

		for (i = 0; i < vsi->rss_table_size; i++)
			vsi->rss_lut_user[i] = (u8)(indir[i]);
	} else {
		ice_fill_rss_lut(vsi->rss_lut_user, vsi->rss_table_size,
				 vsi->rss_size);
	}

	err = ice_set_rss_lut(vsi, vsi->rss_lut_user, vsi->rss_table_size);
	if (err)
		return err;

	return 0;
}
#endif /* ETHTOOL_GRSSH && ETHTOOL_SRSSH */

static int
ice_get_ts_info(struct net_device *dev, struct ethtool_ts_info *info)
{
	struct ice_pf *pf = ice_netdev_to_pf(dev);

	/* only report timestamping if PTP is enabled */
	if (!test_bit(ICE_FLAG_PTP, pf->flags))
		return ethtool_op_get_ts_info(dev, info);

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE |
				SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;

	info->phc_index = ice_get_ptp_clock_index(pf);

	info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) | BIT(HWTSTAMP_FILTER_ALL);

	return 0;
}

/**
 * ice_get_max_txq - return the maximum number of Tx queues for in a PF
 * @pf: PF structure
 */
static int ice_get_max_txq(struct ice_pf *pf)
{
	return min3(pf->num_lan_msix, (u16)num_online_cpus(),
		    (u16)pf->hw.func_caps.common_cap.num_txq);
}

/**
 * ice_get_max_rxq - return the maximum number of Rx queues for in a PF
 * @pf: PF structure
 */
static int ice_get_max_rxq(struct ice_pf *pf)
{
	return min3(pf->num_lan_msix, (u16)num_online_cpus(),
		    (u16)pf->hw.func_caps.common_cap.num_rxq);
}

/**
 * ice_get_combined_cnt - return the current number of combined channels
 * @vsi: PF VSI pointer
 *
 * Go through all queue vectors and count ones that have both Rx and Tx ring
 * attached
 */
static u32 ice_get_combined_cnt(struct ice_vsi *vsi)
{
	u32 combined = 0;
	int q_idx;

	ice_for_each_q_vector(vsi, q_idx) {
		struct ice_q_vector *q_vector = vsi->q_vectors[q_idx];

		if (q_vector->rx.ring && q_vector->tx.ring)
			combined++;
	}

	return combined;
}

/**
 * ice_get_channels - get the current and max supported channels
 * @dev: network interface device structure
 * @ch: ethtool channel data structure
 */
static void
ice_get_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct ice_netdev_priv *np = netdev_priv(dev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;

	/* report maximum channels */
	ch->max_rx = ice_get_max_rxq(pf);
	ch->max_tx = ice_get_max_txq(pf);
	ch->max_combined = min_t(int, ch->max_rx, ch->max_tx);

	/* report current channels */
	ch->combined_count = ice_get_combined_cnt(vsi);
	ch->rx_count = vsi->num_rxq - ch->combined_count;
	ch->tx_count = vsi->num_txq - ch->combined_count;
#ifdef HAVE_NETDEV_SB_DEV

	if (test_bit(ICE_FLAG_MACVLAN_ENA, pf->flags)) {
		/* L2 forwarding devices are single queue so we infer one
		 * device is one channel
		 */
		ch->max_combined += pf->max_num_macvlan;
		ch->combined_count += pf->num_macvlan;
	}
#endif /* HAVE_NETDEV_SB_DEV */

	/* report other queues */
	ch->other_count = test_bit(ICE_FLAG_FD_ENA, pf->flags) ? 1 : 0;
	ch->max_other = ch->other_count;
}

/**
 * ice_get_valid_rss_size - return valid number of RSS queues
 * @hw: pointer to the HW structure
 * @new_size: requested RSS queues
 */
static int ice_get_valid_rss_size(struct ice_hw *hw, int new_size)
{
	struct ice_hw_common_caps *caps = &hw->func_caps.common_cap;

	return min_t(int, new_size, BIT(caps->rss_table_entry_width));
}

/**
 * ice_vsi_set_dflt_rss_lut - set default RSS LUT with requested RSS size
 * @vsi: VSI to reconfigure RSS LUT on
 * @req_rss_size: requested range of queue numbers for hashing
 *
 * Set the VSI's RSS parameters, configure the RSS LUT based on these.
 */
static int ice_vsi_set_dflt_rss_lut(struct ice_vsi *vsi, int req_rss_size)
{
	struct ice_pf *pf = vsi->back;
	struct device *dev;
	struct ice_hw *hw;
	int err;
	u8 *lut;

	dev = ice_pf_to_dev(pf);
	hw = &pf->hw;

	if (!req_rss_size)
		return -EINVAL;

	lut = kzalloc(vsi->rss_table_size, GFP_KERNEL);
	if (!lut)
		return -ENOMEM;

	/* set RSS LUT parameters */
	if (!test_bit(ICE_FLAG_RSS_ENA, pf->flags))
		vsi->rss_size = 1;
	else
		vsi->rss_size = ice_get_valid_rss_size(hw, req_rss_size);

	/* create/set RSS LUT */
	ice_fill_rss_lut(lut, vsi->rss_table_size, vsi->rss_size);
	err = ice_set_rss_lut(vsi, lut, vsi->rss_table_size);
	if (err)
		dev_err(dev, "Cannot set RSS lut, err %d aq_err %s\n", err,
			ice_aq_str(hw->adminq.sq_last_status));

	kfree(lut);
	return err;
}

/**
 * ice_set_channels - set the number channels
 * @dev: network interface device structure
 * @ch: ethtool channel data structure
 */
static int ice_set_channels(struct net_device *dev, struct ethtool_channels *ch)
{
	struct ice_netdev_priv *np = netdev_priv(dev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	int new_rx = 0, new_tx = 0;
	u32 curr_combined;

	/* do not support changing channels in Safe Mode */
	if (ice_is_safe_mode(pf)) {
		netdev_err(dev, "Changing channel in Safe Mode is not supported\n");
		return -EOPNOTSUPP;
	}
	/* do not support changing other_count */
	if (ch->other_count != (test_bit(ICE_FLAG_FD_ENA, pf->flags) ? 1U : 0U))
		return -EINVAL;

#ifdef NETIF_F_HW_TC
	if (ice_is_adq_active(pf)) {
		netdev_err(dev, "Cannot set channels with ADQ configured.\n");
		return -EOPNOTSUPP;
	}
#endif /* NETIF_F_HW_TC */
#ifdef HAVE_NETDEV_SB_DEV
	if (test_bit(ICE_FLAG_MACVLAN_ENA, pf->flags)) {
		netdev_err(dev, "Cannot set channels when L2 forwarding enabled\n");
		return -EOPNOTSUPP;
	}
#endif /* HAVE_NETDEV_SB_DEV */

	if (test_bit(ICE_FLAG_FD_ENA, pf->flags) && pf->hw.fdir_active_fltr) {
		netdev_err(dev, "Cannot set channels when Flow Director filters are active\n");
		return -EOPNOTSUPP;
	}

	curr_combined = ice_get_combined_cnt(vsi);

	/* these checks are for cases where user didn't specify a particular
	 * value on cmd line but we get non-zero value anyway via
	 * get_channels(); look at ethtool.c in ethtool repository (the user
	 * space part), particularly, do_schannels() routine
	 */
	if (ch->rx_count == vsi->num_rxq - curr_combined)
		ch->rx_count = 0;
	if (ch->tx_count == vsi->num_txq - curr_combined)
		ch->tx_count = 0;
	if (ch->combined_count == curr_combined)
		ch->combined_count = 0;

	if (!(ch->combined_count || (ch->rx_count && ch->tx_count))) {
		netdev_err(dev, "Please specify at least 1 Rx and 1 Tx channel\n");
		return -EINVAL;
	}

	new_rx = ch->combined_count + ch->rx_count;
	new_tx = ch->combined_count + ch->tx_count;

	if (new_rx > ice_get_max_rxq(pf)) {
		netdev_err(dev, "Maximum allowed Rx channels is %d\n",
			   ice_get_max_rxq(pf));
		return -EINVAL;
	}
	if (new_tx > ice_get_max_txq(pf)) {
		netdev_err(dev, "Maximum allowed Tx channels is %d\n",
			   ice_get_max_txq(pf));
		return -EINVAL;
	}

	ice_vsi_recfg_qs(vsi, new_rx, new_tx);

	if (new_rx) {
#ifdef IFF_RXFH_CONFIGURED
		if (!netif_is_rxfh_configured(dev))
			return ice_vsi_set_dflt_rss_lut(vsi, new_rx);

		/* Update rss_size due to change in Rx queues */
		vsi->rss_size = ice_get_valid_rss_size(&pf->hw, new_rx);
#else
		/* Clear the previous vsi->rss_lut_user because it is assumed to
		 * be invalid at this point.
		 */
		if (vsi->rss_lut_user) {
			netdev_info(vsi->netdev, "Rx queue count changed, clearing user modified RSS LUT, re-run ethtool [-x|-X] to [check|set] settings if needed\n");
			devm_kfree(ice_pf_to_dev(pf), vsi->rss_lut_user);
			vsi->rss_lut_user = NULL;
		}

		return ice_vsi_set_dflt_rss_lut(vsi, new_rx);
#endif /* IFF_RXFH_CONFIGURED */
	}

	return 0;
}

/**
 * ice_get_wol - get current Wake on LAN configuration
 * @netdev: network interface device structure
 * @wol: Ethtool structure to retrieve WoL settings
 */
static void ice_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_pf *pf = np->vsi->back;

	if (np->vsi->type != ICE_VSI_PF)
		netdev_warn(netdev, "Wake on LAN is not supported on this interface!\n");

	/* Get WoL settings based on the HW capability */
	if (ice_is_wol_supported(pf)) {
		wol->supported = WAKE_MAGIC;
		wol->wolopts = pf->wol_ena ? WAKE_MAGIC : 0;
	} else {
		wol->supported = 0;
		wol->wolopts = 0;
	}
}

/**
 * ice_set_wol - set Wake on LAN on supported device
 * @netdev: network interface device structure
 * @wol: Ethtool structure to set WoL
 */
static int ice_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;

	if (vsi->type != ICE_VSI_PF || !ice_is_wol_supported(pf))
		return -EOPNOTSUPP;

	/* only magic packet is supported */
	if (wol->wolopts && wol->wolopts != WAKE_MAGIC)
		return -EOPNOTSUPP;

	/* Set WoL only if there is a new value */
	if (pf->wol_ena != !!wol->wolopts) {
		pf->wol_ena = !!wol->wolopts;
		device_set_wakeup_enable(ice_pf_to_dev(pf), pf->wol_ena);
		netdev_dbg(netdev, "WoL magic packet %sabled\n",
			   pf->wol_ena ? "en" : "dis");
	}

	return 0;
}

enum ice_container_type {
	ICE_RX_CONTAINER,
	ICE_TX_CONTAINER,
};

/**
 * ice_get_rc_coalesce - get ITR values for specific ring container
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @c_type: container type, Rx or Tx
 * @rc: ring container that the ITR values will come from
 *
 * Query the device for ice_ring_container specific ITR values. This is
 * done per ice_ring_container because each q_vector can have 1 or more rings
 * and all of said ring(s) will have the same ITR values.
 *
 * Returns 0 on success, negative otherwise.
 */
static int
ice_get_rc_coalesce(struct ethtool_coalesce *ec, enum ice_container_type c_type,
		    struct ice_ring_container *rc)
{
	struct ice_pf *pf;

	if (!rc->ring)
		return -EINVAL;

	pf = rc->ring->vsi->back;

	switch (c_type) {
	case ICE_RX_CONTAINER:
		ec->use_adaptive_rx_coalesce = ITR_IS_DYNAMIC(rc->itr_setting);
		ec->rx_coalesce_usecs = rc->itr_setting & ~ICE_ITR_DYNAMIC;
		ec->rx_coalesce_usecs_high = rc->ring->q_vector->intrl;
		break;
	case ICE_TX_CONTAINER:
		ec->use_adaptive_tx_coalesce = ITR_IS_DYNAMIC(rc->itr_setting);
		ec->tx_coalesce_usecs = rc->itr_setting & ~ICE_ITR_DYNAMIC;
		break;
	default:
		dev_dbg(ice_pf_to_dev(pf), "Invalid c_type %d\n", c_type);
		return -EINVAL;
	}

	return 0;
}

/**
 * ice_get_q_coalesce - get a queue's ITR/INTRL (coalesce) settings
 * @vsi: VSI associated to the queue for getting ITR/INTRL (coalesce) settings
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 *
 * Return 0 on success, and negative under the following conditions:
 * 1. Getting Tx or Rx ITR/INTRL (coalesce) settings failed.
 * 2. The q_num passed in is not a valid number/index for Tx and Rx rings.
 */
static int
ice_get_q_coalesce(struct ice_vsi *vsi, struct ethtool_coalesce *ec, int q_num)
{
	if (q_num < vsi->num_rxq && q_num < vsi->num_txq) {
		if (ice_get_rc_coalesce(ec, ICE_RX_CONTAINER,
					&vsi->rx_rings[q_num]->q_vector->rx))
			return -EINVAL;
		if (ice_get_rc_coalesce(ec, ICE_TX_CONTAINER,
					&vsi->tx_rings[q_num]->q_vector->tx))
			return -EINVAL;
	} else if (q_num < vsi->num_rxq) {
		if (ice_get_rc_coalesce(ec, ICE_RX_CONTAINER,
					&vsi->rx_rings[q_num]->q_vector->rx))
			return -EINVAL;
	} else if (q_num < vsi->num_txq) {
		if (ice_get_rc_coalesce(ec, ICE_TX_CONTAINER,
					&vsi->tx_rings[q_num]->q_vector->tx))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * __ice_get_coalesce - get ITR/INTRL values for the device
 * @netdev: pointer to the netdev associated with this query
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @q_num: queue number to get the coalesce settings for
 *
 * If the caller passes in a negative q_num then we return coalesce settings
 * based on queue number 0, else use the actual q_num passed in.
 */
static int
__ice_get_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec,
		   int q_num)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

	if (q_num < 0)
		q_num = 0;

	if (ice_get_q_coalesce(vsi, ec, q_num))
		return -EINVAL;

	return 0;
}

static int
ice_get_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec)
{
	return __ice_get_coalesce(netdev, ec, -1);
}

#ifdef ETHTOOL_PERQUEUE
static int
ice_get_per_q_coalesce(struct net_device *netdev, u32 q_num,
		       struct ethtool_coalesce *ec)
{
	return __ice_get_coalesce(netdev, ec, q_num);
}
#endif /* ETHTOOL_PERQUEUE */

/**
 * ice_set_rc_coalesce - set ITR values for specific ring container
 * @c_type: container type, Rx or Tx
 * @ec: ethtool structure from user to update ITR settings
 * @rc: ring container that the ITR values will come from
 * @vsi: VSI associated to the ring container
 *
 * Set specific ITR values. This is done per ice_ring_container because each
 * q_vector can have 1 or more rings and all of said ring(s) will have the same
 * ITR values.
 *
 * Returns 0 on success, negative otherwise.
 */
static int
ice_set_rc_coalesce(enum ice_container_type c_type, struct ethtool_coalesce *ec,
		    struct ice_ring_container *rc, struct ice_vsi *vsi)
{
	const char *c_type_str = (c_type == ICE_RX_CONTAINER) ? "rx" : "tx";
	u32 use_adaptive_coalesce, coalesce_usecs;
	struct ice_pf *pf = vsi->back;
	u16 itr_setting;

	if (!rc->ring)
		return -EINVAL;

	switch (c_type) {
	case ICE_RX_CONTAINER:
		if (ec->rx_coalesce_usecs_high > ICE_MAX_INTRL ||
		    (ec->rx_coalesce_usecs_high &&
		     ec->rx_coalesce_usecs_high < pf->hw.intrl_gran)) {
			netdev_info(vsi->netdev, "Invalid value, %s-usecs-high valid values are 0 (disabled), %d-%d\n",
				    c_type_str, pf->hw.intrl_gran,
				    ICE_MAX_INTRL);
			return -EINVAL;
		}
		if (ec->rx_coalesce_usecs_high != rc->ring->q_vector->intrl) {
			rc->ring->q_vector->intrl = ec->rx_coalesce_usecs_high;
			wr32(&pf->hw, GLINT_RATE(rc->ring->q_vector->reg_idx),
			     ice_intrl_usec_to_reg(ec->rx_coalesce_usecs_high,
						   pf->hw.intrl_gran));
		}

		use_adaptive_coalesce = ec->use_adaptive_rx_coalesce;
		coalesce_usecs = ec->rx_coalesce_usecs;

		break;
	case ICE_TX_CONTAINER:
#ifndef ETHTOOL_COALESCE_USECS
		if (ec->tx_coalesce_usecs_high)
			return -EOPNOTSUPP;

#endif /* !ETHTOOL_COALESCE_USECS */
		use_adaptive_coalesce = ec->use_adaptive_tx_coalesce;
		coalesce_usecs = ec->tx_coalesce_usecs;

		break;
	default:
		dev_dbg(ice_pf_to_dev(pf), "Invalid container type %d\n",
			c_type);
		return -EINVAL;
	}

	itr_setting = rc->itr_setting & ~ICE_ITR_DYNAMIC;
	if (coalesce_usecs != itr_setting && use_adaptive_coalesce) {
		netdev_info(vsi->netdev, "%s interrupt throttling cannot be changed if adaptive-%s is enabled\n",
			    c_type_str, c_type_str);
		return -EINVAL;
	}

	if (coalesce_usecs > ICE_ITR_MAX) {
		netdev_info(vsi->netdev, "Invalid value, %s-usecs range is 0-%d\n",
			    c_type_str, ICE_ITR_MAX);
		return -EINVAL;
	}

	if (use_adaptive_coalesce) {
		rc->itr_setting |= ICE_ITR_DYNAMIC;
	} else {
		/* save the user set usecs */
		rc->itr_setting = coalesce_usecs;
		/* device ITR granularity is in 2 usec increments */
		rc->target_itr = ITR_REG_ALIGN(rc->itr_setting);
	}

	return 0;
}

/**
 * ice_inline_itr_update - sets the ITR value instead of deferring
 * @vsi: VSI associated to the ring container
 * @q_vector: ptr to vector
 * @ec: ptr to ethtool coalesce
 *
 * Set specific ITR values. This is done per vector by writing to
 * GLINT_ITR register twice (once for Tx and Rx)
 */
static void
ice_inline_itr_update(struct ice_vsi *vsi, struct ice_q_vector *q_vector,
		      struct ethtool_coalesce *ec)
{
	if (!ec->use_adaptive_tx_coalesce) {
		struct ice_ring_container *tx_rc = &q_vector->tx;

		wr32(&vsi->back->hw,
		     GLINT_ITR(tx_rc->itr_idx, q_vector->reg_idx),
		     tx_rc->target_itr >> ICE_ITR_GRAN_S);
		tx_rc->current_itr = tx_rc->target_itr;
	}
	if (!ec->use_adaptive_rx_coalesce) {
		struct ice_ring_container *rx_rc = &q_vector->rx;

		wr32(&vsi->back->hw,
		     GLINT_ITR(rx_rc->itr_idx, q_vector->reg_idx),
		     rx_rc->target_itr >> ICE_ITR_GRAN_S);
		rx_rc->current_itr = rx_rc->target_itr;
	}
}

/**
 * ice_set_q_coalesce - set a queue's ITR/INTRL (coalesce) settings
 * @vsi: VSI associated to the queue that need updating
 * @ec: coalesce settings to program the device with
 * @q_num: update ITR/INTRL (coalesce) settings for this queue number/index
 *
 * Return 0 on success, and negative under the following conditions:
 * 1. Setting Tx or Rx ITR/INTRL (coalesce) settings failed.
 * 2. The q_num passed in is not a valid number/index for Tx and Rx rings.
 */
static int
ice_set_q_coalesce(struct ice_vsi *vsi, struct ethtool_coalesce *ec, int q_num)
{
	if (q_num < vsi->num_rxq && q_num < vsi->num_txq) {
		if (ice_set_rc_coalesce(ICE_RX_CONTAINER, ec,
					&vsi->rx_rings[q_num]->q_vector->rx,
					vsi))
			return -EINVAL;

		if (ice_set_rc_coalesce(ICE_TX_CONTAINER, ec,
					&vsi->tx_rings[q_num]->q_vector->tx,
					vsi))
			return -EINVAL;
	} else if (q_num < vsi->num_rxq) {
		if (ice_set_rc_coalesce(ICE_RX_CONTAINER, ec,
					&vsi->rx_rings[q_num]->q_vector->rx,
					vsi))
			return -EINVAL;
	} else if (q_num < vsi->num_txq) {
		if (ice_set_rc_coalesce(ICE_TX_CONTAINER, ec,
					&vsi->tx_rings[q_num]->q_vector->tx,
					vsi))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	return 0;
}

#ifndef ETHTOOL_COALESCE_USECS
/**
 * ice_is_coalesce_param_invalid - check for unsupported coalesce parameters
 * @ec: ethtool structure to fill with driver's coalesce settings
 */
static bool ice_is_coalesce_param_invalid(struct ethtool_coalesce *ec)
{
	if (ec->stats_block_coalesce_usecs || ec->rate_sample_interval ||
	    ec->pkt_rate_low || ec->pkt_rate_high ||
	    ec->rx_max_coalesced_frames || ec->rx_coalesce_usecs_irq ||
	    ec->rx_max_coalesced_frames_irq || ec->tx_max_coalesced_frames ||
	    ec->tx_coalesce_usecs_irq || ec->tx_max_coalesced_frames_irq ||
	    ec->rx_coalesce_usecs_low || ec->rx_max_coalesced_frames_low ||
	    ec->tx_coalesce_usecs_low || ec->tx_max_coalesced_frames_low ||
	    ec->rx_max_coalesced_frames_high ||
	    ec->tx_max_coalesced_frames_high)
		return true;

	return false;
}
#endif /* !ETHTOOL_COALESCE_USECS */

/**
 * ice_print_if_odd_usecs - print message if user tries to set odd [tx|rx]-usecs
 * @netdev: netdev used for print
 * @itr_setting: previous user setting
 * @use_adaptive_coalesce: if adaptive coalesce is enabled or being enabled
 * @coalesce_usecs: requested value of [tx|rx]-usecs
 * @c_type_str: either "rx" or "tx" to match user set field of [tx|rx]-usecs
 */
static void
ice_print_if_odd_usecs(struct net_device *netdev, u16 itr_setting,
		       u32 use_adaptive_coalesce, u32 coalesce_usecs,
		       const char *c_type_str)
{
	if (use_adaptive_coalesce)
		return;

	itr_setting = ITR_TO_REG(itr_setting);

	if (itr_setting != coalesce_usecs && (coalesce_usecs % 2))
		netdev_info(netdev, "User set %s-usecs to %d, device only supports even values. Rounding down and attempting to set %s-usecs to %d\n",
			    c_type_str, coalesce_usecs, c_type_str,
			    ITR_REG_ALIGN(coalesce_usecs));
}

/**
 * __ice_set_coalesce - set ITR/INTRL values for the device
 * @netdev: pointer to the netdev associated with this query
 * @ec: ethtool structure to fill with driver's coalesce settings
 * @q_num: queue number to get the coalesce settings for
 *
 * If the caller passes in a negative q_num then we set the coalesce settings
 * for all Tx/Rx queues, else use the actual q_num passed in.
 */
static int
__ice_set_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec,
		   int q_num)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;

#ifndef ETHTOOL_COALESCE_USECS
	if (ice_is_coalesce_param_invalid(ec))
		return -EOPNOTSUPP;
#endif /* !ETHTOOL_COALESCE_USECS */

	if (q_num < 0) {
		struct ice_q_vector *q_vector = vsi->q_vectors[0];
		int v_idx;

		if (q_vector) {
			ice_print_if_odd_usecs(netdev, q_vector->rx.itr_setting,
					       ec->use_adaptive_rx_coalesce,
					       ec->rx_coalesce_usecs, "rx");

			ice_print_if_odd_usecs(netdev, q_vector->tx.itr_setting,
					       ec->use_adaptive_tx_coalesce,
					       ec->tx_coalesce_usecs, "tx");
		}

		ice_for_each_q_vector(vsi, v_idx) {
			/* In some cases if DCB is configured the num_[rx|tx]q
			 * can be less than vsi->num_q_vectors. This check
			 * accounts for that so we don't report a false failure
			 */
			if (v_idx >= vsi->num_rxq && v_idx >= vsi->num_txq)
				goto set_complete;

			if (ice_set_q_coalesce(vsi, ec, v_idx))
				return -EINVAL;
			/* perform in-context ITR update only if ADQ is
			 * supported and "AIM" is off (for either Tx or Rx
			 * ITR setting)
			 */
			if (!(ec->use_adaptive_rx_coalesce &&
			      ec->use_adaptive_tx_coalesce))
				ice_inline_itr_update(vsi,
						      vsi->q_vectors[v_idx],
						      ec);
		}
		goto set_complete;
	}

	if (ice_set_q_coalesce(vsi, ec, q_num))
		return -EINVAL;

set_complete:
	/* make sure to flush writes to GLINT_ITR, only if ADQ SUPPORT is ON
	 * and "aim" setting for Tx or Rx ITR is off
	 */
	if (q_num < 0 &&
	    (!(ec->use_adaptive_rx_coalesce && ec->use_adaptive_tx_coalesce)))
		ice_flush(&vsi->back->hw);

	return 0;
}

static int
ice_set_coalesce(struct net_device *netdev, struct ethtool_coalesce *ec)
{
	return __ice_set_coalesce(netdev, ec, -1);
}

#ifdef ETHTOOL_PERQUEUE
static int
ice_set_per_q_coalesce(struct net_device *netdev, u32 q_num,
		       struct ethtool_coalesce *ec)
{
	return __ice_set_coalesce(netdev, ec, q_num);
}
#endif /* ETHTOOL_PERQUEUE */

#ifdef ETHTOOL_GMODULEINFO
#define ICE_I2C_EEPROM_DEV_ADDR		0xA0
#define ICE_I2C_EEPROM_DEV_ADDR2	0xA2
#define ICE_MODULE_TYPE_SFP		0x03
#define ICE_MODULE_TYPE_QSFP_PLUS	0x0D
#define ICE_MODULE_TYPE_QSFP28		0x11
#define ICE_MODULE_SFF_ADDR_MODE	0x04
#define ICE_MODULE_SFF_DIAG_CAPAB	0x40
#define ICE_MODULE_REVISION_ADDR	0x01
#define ICE_MODULE_SFF_8472_COMP	0x5E
#define ICE_MODULE_SFF_8472_SWAP	0x5C
#define ICE_MODULE_QSFP_MAX_LEN		640

/**
 * ice_get_module_info - get SFF module type and revision information
 * @netdev: network interface device structure
 * @modinfo: module EEPROM size and layout information structure
 */
static int
ice_get_module_info(struct net_device *netdev,
		    struct ethtool_modinfo *modinfo)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	enum ice_status status;
	u8 sff8472_comp = 0;
	u8 sff8472_swap = 0;
	u8 sff8636_rev = 0;
	u8 value = 0;

	status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR, 0x00, 0x00,
				   0, &value, 1, 0, NULL);
	if (status)
		return -EIO;

	switch (value) {
	case ICE_MODULE_TYPE_SFP:
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_SFF_8472_COMP, 0x00, 0,
					   &sff8472_comp, 1, 0, NULL);
		if (status)
			return -EIO;
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_SFF_8472_SWAP, 0x00, 0,
					   &sff8472_swap, 1, 0, NULL);
		if (status)
			return -EIO;

		if (sff8472_swap & ICE_MODULE_SFF_ADDR_MODE) {
			modinfo->type = ETH_MODULE_SFF_8079;
			modinfo->eeprom_len = ETH_MODULE_SFF_8079_LEN;
		} else if (sff8472_comp &&
			   (sff8472_swap & ICE_MODULE_SFF_DIAG_CAPAB)) {
			modinfo->type = ETH_MODULE_SFF_8472;
			modinfo->eeprom_len = ETH_MODULE_SFF_8472_LEN;
		} else {
			modinfo->type = ETH_MODULE_SFF_8079;
			modinfo->eeprom_len = ETH_MODULE_SFF_8079_LEN;
		}
		break;
	case ICE_MODULE_TYPE_QSFP_PLUS:
	case ICE_MODULE_TYPE_QSFP28:
		status = ice_aq_sff_eeprom(hw, 0, ICE_I2C_EEPROM_DEV_ADDR,
					   ICE_MODULE_REVISION_ADDR, 0x00, 0,
					   &sff8636_rev, 1, 0, NULL);
		if (status)
			return -EIO;
		/* Check revision compliance */
		if (sff8636_rev > 0x02) {
			/* Module is SFF-8636 compliant */
			modinfo->type = ETH_MODULE_SFF_8636;
			modinfo->eeprom_len = ICE_MODULE_QSFP_MAX_LEN;
		} else {
			modinfo->type = ETH_MODULE_SFF_8436;
			modinfo->eeprom_len = ICE_MODULE_QSFP_MAX_LEN;
		}
		break;
	default:
		netdev_warn(netdev, "SFF Module Type not recognized.\n");
		return -EINVAL;
	}
	return 0;
}

/**
 * ice_get_module_eeprom - fill buffer with SFF EEPROM contents
 * @netdev: network interface device structure
 * @ee: EEPROM dump request structure
 * @data: buffer to be filled with EEPROM contents
 */
static int
ice_get_module_eeprom(struct net_device *netdev,
		      struct ethtool_eeprom *ee, u8 *data)
{
	struct ice_netdev_priv *np = netdev_priv(netdev);
	u8 addr = ICE_I2C_EEPROM_DEV_ADDR;
	struct ice_vsi *vsi = np->vsi;
	struct ice_pf *pf = vsi->back;
	struct ice_hw *hw = &pf->hw;
	enum ice_status status;
	bool is_sfp = false;
	unsigned int i;
	u16 offset = 0;
	u8 value = 0;
	u8 page = 0;

	if (!ee || !ee->len || !data)
		return -EINVAL;

	status = ice_aq_sff_eeprom(hw, 0, addr, offset, page, 0, &value, 1, 0,
				   NULL);
	if (status)
		return -EIO;

	if (value == ICE_MODULE_TYPE_SFP)
		is_sfp = true;

	for (i = 0; i < ee->len; i++) {
		offset = i + ee->offset;

		/* Check if we need to access the other memory page */
		if (is_sfp) {
			if (offset >= ETH_MODULE_SFF_8079_LEN) {
				offset -= ETH_MODULE_SFF_8079_LEN;
				addr = ICE_I2C_EEPROM_DEV_ADDR2;
			}
		} else {
			while (offset >= ETH_MODULE_SFF_8436_LEN) {
				/* Compute memory page number and offset. */
				offset -= ETH_MODULE_SFF_8436_LEN / 2;
				page++;
			}
		}

		status = ice_aq_sff_eeprom(hw, 0, addr, offset, page, !is_sfp,
					   &value, 1, 0, NULL);
		if (status)
			value = 0;
		data[i] = value;
	}
	return 0;
}
#endif /* ETHTOOL_GMODULEINFO */

static const struct ethtool_ops ice_ethtool_ops = {
#ifdef ETHTOOL_COALESCE_USECS
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_USE_ADAPTIVE |
				     ETHTOOL_COALESCE_RX_USECS_HIGH,
#endif /* ETHTOOL_COALESCE_USECS */
#ifdef ETHTOOL_GLINKSETTINGS
	.get_link_ksettings	= ice_get_link_ksettings,
	.set_link_ksettings	= ice_set_link_ksettings,
#else
	.get_settings		= ice_get_settings,
	.set_settings		= ice_set_settings,
#endif /* ETHTOOL_GLINKSETTINGS */
	.get_drvinfo		= ice_get_drvinfo,
	.get_regs_len		= ice_get_regs_len,
	.get_regs		= ice_get_regs,
	.get_wol		= ice_get_wol,
	.set_wol		= ice_set_wol,
	.get_msglevel		= ice_get_msglevel,
	.set_msglevel		= ice_set_msglevel,
	.self_test		= ice_self_test,
	.get_link		= ethtool_op_get_link,
	.get_eeprom_len		= ice_get_eeprom_len,
	.get_eeprom		= ice_get_eeprom,
	.set_eeprom		= ice_set_eeprom,
	.get_coalesce		= ice_get_coalesce,
	.set_coalesce		= ice_set_coalesce,
	.get_strings		= ice_get_strings,
	.set_phys_id		= ice_set_phys_id,
	.get_ethtool_stats      = ice_get_ethtool_stats,
	.get_priv_flags		= ice_get_priv_flags,
	.set_priv_flags		= ice_set_priv_flags,
	.get_sset_count		= ice_get_sset_count,
	.get_rxnfc		= ice_get_rxnfc,
	.set_rxnfc		= ice_set_rxnfc,
	.get_ringparam		= ice_get_ringparam,
	.set_ringparam		= ice_set_ringparam,
	.nway_reset		= ice_nway_reset,
	.get_pauseparam		= ice_get_pauseparam,
	.set_pauseparam		= ice_set_pauseparam,
#if defined(ETHTOOL_GRSSH) && defined(ETHTOOL_SRSSH)
	.get_rxfh_key_size	= ice_get_rxfh_key_size,
	.get_rxfh_indir_size	= ice_get_rxfh_indir_size,
	.get_rxfh		= ice_get_rxfh,
	.set_rxfh		= ice_set_rxfh,
#endif /* ETHTOOL_GRSSH && ETHTOOL_SRSSH */
	.get_channels		= ice_get_channels,
	.set_channels		= ice_set_channels,
	.get_ts_info		= ice_get_ts_info,
#ifdef ETHTOOL_PERQUEUE
	.get_per_queue_coalesce	= ice_get_per_q_coalesce,
	.set_per_queue_coalesce	= ice_set_per_q_coalesce,
#endif /* ETHTOOL_PERQUEUE */
#ifdef ETHTOOL_GFECPARAM
	.get_fecparam		= ice_get_fecparam,
	.set_fecparam		= ice_set_fecparam,
#endif /* ETHTOOL_GFECPARAM */
#ifdef ETHTOOL_GMODULEINFO
	.get_module_info	= ice_get_module_info,
	.get_module_eeprom	= ice_get_module_eeprom,
#endif /* ETHTOOL_GMODULEINFO */
};

static const struct ethtool_ops ice_ethtool_recovery_ops = {
	.get_drvinfo		= ice_get_drvinfo,
	.get_eeprom_len		= ice_get_eeprom_len,
	.get_eeprom		= ice_get_eeprom,
	.set_eeprom		= ice_set_eeprom,
};

static const struct ethtool_ops ice_ethtool_safe_mode_ops = {
#ifdef ETHTOOL_GLINKSETTINGS
	.get_link_ksettings	= ice_get_link_ksettings,
	.set_link_ksettings	= ice_set_link_ksettings,
#else
	.get_settings		= ice_get_settings,
	.set_settings		= ice_set_settings,
#endif /* ETHTOOL_GLINKSETTINGS */
	.get_drvinfo		= ice_get_drvinfo,
	.get_regs_len		= ice_get_regs_len,
	.get_regs		= ice_get_regs,
	.get_wol		= ice_get_wol,
	.set_wol		= ice_set_wol,
	.get_msglevel		= ice_get_msglevel,
	.set_msglevel		= ice_set_msglevel,
	.get_link		= ethtool_op_get_link,
	.get_eeprom_len		= ice_get_eeprom_len,
	.get_eeprom		= ice_get_eeprom,
	.set_eeprom		= ice_set_eeprom,
	.get_strings		= ice_get_strings,
	.get_ethtool_stats      = ice_get_ethtool_stats,
	.get_sset_count		= ice_get_sset_count,
	.get_ringparam		= ice_get_ringparam,
	.set_ringparam		= ice_set_ringparam,
	.nway_reset		= ice_nway_reset,
	.get_channels		= ice_get_channels,
};

/**
 * ice_set_ethtool_safe_mode_ops - setup safe mode ethtool ops
 * @netdev: network interface device structure
 */
void ice_set_ethtool_safe_mode_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_safe_mode_ops;
}


/**
 * ice_set_ethtool_recovery_ops - setup FW recovery ethtool ops
 * @netdev: network interface device structure
 */
void ice_set_ethtool_recovery_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_recovery_ops;
}

/**
 * ice_set_ethtool_ops - setup netdev ethtool ops
 * @netdev: network interface device structure
 *
 * setup netdev ethtool ops with ice specific ops
 */
void ice_set_ethtool_ops(struct net_device *netdev)
{
	netdev->ethtool_ops = &ice_ethtool_ops;
}
