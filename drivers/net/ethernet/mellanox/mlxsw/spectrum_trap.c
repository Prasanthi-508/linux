// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2019 Mellanox Technologies. All rights reserved */

#include <linux/kernel.h>
#include <net/devlink.h>
#include <uapi/linux/devlink.h>

#include "core.h"
#include "reg.h"
#include "spectrum.h"

/* All driver-specific traps must be documented in
 * Documentation/networking/devlink/mlxsw.rst
 */
enum {
	DEVLINK_MLXSW_TRAP_ID_BASE = DEVLINK_TRAP_GENERIC_ID_MAX,
	DEVLINK_MLXSW_TRAP_ID_IRIF_DISABLED,
	DEVLINK_MLXSW_TRAP_ID_ERIF_DISABLED,
};

#define DEVLINK_MLXSW_TRAP_NAME_IRIF_DISABLED \
	"irif_disabled"
#define DEVLINK_MLXSW_TRAP_NAME_ERIF_DISABLED \
	"erif_disabled"

#define MLXSW_SP_TRAP_METADATA DEVLINK_TRAP_METADATA_TYPE_F_IN_PORT

static void mlxsw_sp_rx_drop_listener(struct sk_buff *skb, u8 local_port,
				      void *priv);
static void mlxsw_sp_rx_exception_listener(struct sk_buff *skb, u8 local_port,
					   void *trap_ctx);

#define MLXSW_SP_TRAP_DROP(_id, _group_id)				      \
	DEVLINK_TRAP_GENERIC(DROP, DROP, _id,				      \
			     DEVLINK_TRAP_GROUP_GENERIC(_group_id),	      \
			     MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_TRAP_DRIVER_DROP(_id, _group_id)			      \
	DEVLINK_TRAP_DRIVER(DROP, DROP, DEVLINK_MLXSW_TRAP_ID_##_id,	      \
			    DEVLINK_MLXSW_TRAP_NAME_##_id,		      \
			    DEVLINK_TRAP_GROUP_GENERIC(_group_id),	      \
			    MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_TRAP_EXCEPTION(_id, _group_id)		      \
	DEVLINK_TRAP_GENERIC(EXCEPTION, TRAP, _id,			      \
			     DEVLINK_TRAP_GROUP_GENERIC(_group_id),	      \
			     MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_RXL_DISCARD(_id, _group_id)				      \
	MLXSW_RXL(mlxsw_sp_rx_drop_listener, DISCARD_##_id, SET_FW_DEFAULT,   \
		  false, SP_##_group_id, DISCARD)

#define MLXSW_SP_RXL_EXCEPTION(_id, _group_id, _action)			      \
	MLXSW_RXL(mlxsw_sp_rx_exception_listener, _id,			      \
		   _action, false, SP_##_group_id, DISCARD)

static struct devlink_trap mlxsw_sp_traps_arr[] = {
	MLXSW_SP_TRAP_DROP(SMAC_MC, L2_DROPS),
	MLXSW_SP_TRAP_DROP(VLAN_TAG_MISMATCH, L2_DROPS),
	MLXSW_SP_TRAP_DROP(INGRESS_VLAN_FILTER, L2_DROPS),
	MLXSW_SP_TRAP_DROP(INGRESS_STP_FILTER, L2_DROPS),
	MLXSW_SP_TRAP_DROP(EMPTY_TX_LIST, L2_DROPS),
	MLXSW_SP_TRAP_DROP(PORT_LOOPBACK_FILTER, L2_DROPS),
	MLXSW_SP_TRAP_DROP(BLACKHOLE_ROUTE, L3_DROPS),
	MLXSW_SP_TRAP_DROP(NON_IP_PACKET, L3_DROPS),
	MLXSW_SP_TRAP_DROP(UC_DIP_MC_DMAC, L3_DROPS),
	MLXSW_SP_TRAP_DROP(DIP_LB, L3_DROPS),
	MLXSW_SP_TRAP_DROP(SIP_MC, L3_DROPS),
	MLXSW_SP_TRAP_DROP(SIP_LB, L3_DROPS),
	MLXSW_SP_TRAP_DROP(CORRUPTED_IP_HDR, L3_DROPS),
	MLXSW_SP_TRAP_DROP(IPV4_SIP_BC, L3_DROPS),
	MLXSW_SP_TRAP_DROP(IPV6_MC_DIP_RESERVED_SCOPE, L3_DROPS),
	MLXSW_SP_TRAP_DROP(IPV6_MC_DIP_INTERFACE_LOCAL_SCOPE, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(MTU_ERROR, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(TTL_ERROR, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(RPF, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(REJECT_ROUTE, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(UNRESOLVED_NEIGH, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(IPV4_LPM_UNICAST_MISS, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(IPV6_LPM_UNICAST_MISS, L3_DROPS),
	MLXSW_SP_TRAP_DRIVER_DROP(IRIF_DISABLED, L3_DROPS),
	MLXSW_SP_TRAP_DRIVER_DROP(ERIF_DISABLED, L3_DROPS),
	MLXSW_SP_TRAP_DROP(NON_ROUTABLE, L3_DROPS),
	MLXSW_SP_TRAP_EXCEPTION(DECAP_ERROR, TUNNEL_DROPS),
	MLXSW_SP_TRAP_DROP(OVERLAY_SMAC_MC, TUNNEL_DROPS),
};

static struct mlxsw_listener mlxsw_sp_listeners_arr[] = {
	MLXSW_SP_RXL_DISCARD(ING_PACKET_SMAC_MC, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_SWITCH_VTAG_ALLOW, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_SWITCH_VLAN, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_SWITCH_STP, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_UC, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_MC_NULL, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_LB, L2_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ROUTER2, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_NON_IP_PACKET, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_UC_DIP_MC_DMAC, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_DIP_LB, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_SIP_MC, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_SIP_LB, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_CORRUPTED_IP_HDR, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ING_ROUTER_IPV4_SIP_BC, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(IPV6_MC_DIP_RESERVED_SCOPE, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(IPV6_MC_DIP_INTERFACE_LOCAL_SCOPE, L3_DISCARDS),
	MLXSW_SP_RXL_EXCEPTION(MTUERROR, ROUTER_EXP, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(TTLERROR, ROUTER_EXP, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(RPF, RPF, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(RTR_INGRESS1, REMOTE_ROUTE, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(HOST_MISS_IPV4, HOST_MISS, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(HOST_MISS_IPV6, HOST_MISS, TRAP_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER3, REMOTE_ROUTE,
			       TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER_LPM4, ROUTER_EXP,
			       TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER_LPM6, ROUTER_EXP,
			       TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_DISCARD(ROUTER_IRIF_EN, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(ROUTER_ERIF_EN, L3_DISCARDS),
	MLXSW_SP_RXL_DISCARD(NON_ROUTABLE, L3_DISCARDS),
	MLXSW_SP_RXL_EXCEPTION(DECAP_ECN0, ROUTER_EXP, TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(IPIP_DECAP_ERROR, ROUTER_EXP,
			       TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_EXCEPTION(DISCARD_DEC_PKT, TUNNEL_DISCARDS,
			       TRAP_EXCEPTION_TO_CPU),
	MLXSW_SP_RXL_DISCARD(OVERLAY_SMAC_MC, TUNNEL_DISCARDS),
};

/* Mapping between hardware trap and devlink trap. Multiple hardware traps can
 * be mapped to the same devlink trap. Order is according to
 * 'mlxsw_sp_listeners_arr'.
 */
static u16 mlxsw_sp_listener_devlink_map[] = {
	DEVLINK_TRAP_GENERIC_ID_SMAC_MC,
	DEVLINK_TRAP_GENERIC_ID_VLAN_TAG_MISMATCH,
	DEVLINK_TRAP_GENERIC_ID_INGRESS_VLAN_FILTER,
	DEVLINK_TRAP_GENERIC_ID_INGRESS_STP_FILTER,
	DEVLINK_TRAP_GENERIC_ID_EMPTY_TX_LIST,
	DEVLINK_TRAP_GENERIC_ID_EMPTY_TX_LIST,
	DEVLINK_TRAP_GENERIC_ID_PORT_LOOPBACK_FILTER,
	DEVLINK_TRAP_GENERIC_ID_BLACKHOLE_ROUTE,
	DEVLINK_TRAP_GENERIC_ID_NON_IP_PACKET,
	DEVLINK_TRAP_GENERIC_ID_UC_DIP_MC_DMAC,
	DEVLINK_TRAP_GENERIC_ID_DIP_LB,
	DEVLINK_TRAP_GENERIC_ID_SIP_MC,
	DEVLINK_TRAP_GENERIC_ID_SIP_LB,
	DEVLINK_TRAP_GENERIC_ID_CORRUPTED_IP_HDR,
	DEVLINK_TRAP_GENERIC_ID_IPV4_SIP_BC,
	DEVLINK_TRAP_GENERIC_ID_IPV6_MC_DIP_RESERVED_SCOPE,
	DEVLINK_TRAP_GENERIC_ID_IPV6_MC_DIP_INTERFACE_LOCAL_SCOPE,
	DEVLINK_TRAP_GENERIC_ID_MTU_ERROR,
	DEVLINK_TRAP_GENERIC_ID_TTL_ERROR,
	DEVLINK_TRAP_GENERIC_ID_RPF,
	DEVLINK_TRAP_GENERIC_ID_REJECT_ROUTE,
	DEVLINK_TRAP_GENERIC_ID_UNRESOLVED_NEIGH,
	DEVLINK_TRAP_GENERIC_ID_UNRESOLVED_NEIGH,
	DEVLINK_TRAP_GENERIC_ID_UNRESOLVED_NEIGH,
	DEVLINK_TRAP_GENERIC_ID_IPV4_LPM_UNICAST_MISS,
	DEVLINK_TRAP_GENERIC_ID_IPV6_LPM_UNICAST_MISS,
	DEVLINK_MLXSW_TRAP_ID_IRIF_DISABLED,
	DEVLINK_MLXSW_TRAP_ID_ERIF_DISABLED,
	DEVLINK_TRAP_GENERIC_ID_NON_ROUTABLE,
	DEVLINK_TRAP_GENERIC_ID_DECAP_ERROR,
	DEVLINK_TRAP_GENERIC_ID_DECAP_ERROR,
	DEVLINK_TRAP_GENERIC_ID_DECAP_ERROR,
	DEVLINK_TRAP_GENERIC_ID_OVERLAY_SMAC_MC,
};

static int mlxsw_sp_rx_listener(struct mlxsw_sp *mlxsw_sp, struct sk_buff *skb,
				u8 local_port,
				struct mlxsw_sp_port *mlxsw_sp_port)
{
	struct mlxsw_sp_port_pcpu_stats *pcpu_stats;

	if (unlikely(!mlxsw_sp_port)) {
		dev_warn_ratelimited(mlxsw_sp->bus_info->dev, "Port %d: skb received for non-existent port\n",
				     local_port);
		kfree_skb(skb);
		return -EINVAL;
	}

	skb->dev = mlxsw_sp_port->dev;

	pcpu_stats = this_cpu_ptr(mlxsw_sp_port->pcpu_stats);
	u64_stats_update_begin(&pcpu_stats->syncp);
	pcpu_stats->rx_packets++;
	pcpu_stats->rx_bytes += skb->len;
	u64_stats_update_end(&pcpu_stats->syncp);

	skb->protocol = eth_type_trans(skb, skb->dev);

	return 0;
}

static void mlxsw_sp_rx_drop_listener(struct sk_buff *skb, u8 local_port,
				      void *trap_ctx)
{
	struct devlink_port *in_devlink_port;
	struct mlxsw_sp_port *mlxsw_sp_port;
	struct mlxsw_sp *mlxsw_sp;
	struct devlink *devlink;

	mlxsw_sp = devlink_trap_ctx_priv(trap_ctx);
	mlxsw_sp_port = mlxsw_sp->ports[local_port];

	if (mlxsw_sp_rx_listener(mlxsw_sp, skb, local_port, mlxsw_sp_port))
		return;

	devlink = priv_to_devlink(mlxsw_sp->core);
	in_devlink_port = mlxsw_core_port_devlink_port_get(mlxsw_sp->core,
							   local_port);
	skb_push(skb, ETH_HLEN);
	devlink_trap_report(devlink, skb, trap_ctx, in_devlink_port);
	consume_skb(skb);
}

static void mlxsw_sp_rx_exception_listener(struct sk_buff *skb, u8 local_port,
					   void *trap_ctx)
{
	struct devlink_port *in_devlink_port;
	struct mlxsw_sp_port *mlxsw_sp_port;
	struct mlxsw_sp *mlxsw_sp;
	struct devlink *devlink;

	mlxsw_sp = devlink_trap_ctx_priv(trap_ctx);
	mlxsw_sp_port = mlxsw_sp->ports[local_port];

	if (mlxsw_sp_rx_listener(mlxsw_sp, skb, local_port, mlxsw_sp_port))
		return;

	devlink = priv_to_devlink(mlxsw_sp->core);
	in_devlink_port = mlxsw_core_port_devlink_port_get(mlxsw_sp->core,
							   local_port);
	skb_push(skb, ETH_HLEN);
	devlink_trap_report(devlink, skb, trap_ctx, in_devlink_port);
	skb_pull(skb, ETH_HLEN);
	skb->offload_fwd_mark = 1;
	netif_receive_skb(skb);
}

int mlxsw_sp_devlink_traps_init(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);

	if (WARN_ON(ARRAY_SIZE(mlxsw_sp_listener_devlink_map) !=
		    ARRAY_SIZE(mlxsw_sp_listeners_arr)))
		return -EINVAL;

	return devlink_traps_register(devlink, mlxsw_sp_traps_arr,
				      ARRAY_SIZE(mlxsw_sp_traps_arr),
				      mlxsw_sp);
}

void mlxsw_sp_devlink_traps_fini(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);

	devlink_traps_unregister(devlink, mlxsw_sp_traps_arr,
				 ARRAY_SIZE(mlxsw_sp_traps_arr));
}

int mlxsw_sp_trap_init(struct mlxsw_core *mlxsw_core,
		       const struct devlink_trap *trap, void *trap_ctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mlxsw_sp_listener_devlink_map); i++) {
		struct mlxsw_listener *listener;
		int err;

		if (mlxsw_sp_listener_devlink_map[i] != trap->id)
			continue;
		listener = &mlxsw_sp_listeners_arr[i];

		err = mlxsw_core_trap_register(mlxsw_core, listener, trap_ctx);
		if (err)
			return err;
	}

	return 0;
}

void mlxsw_sp_trap_fini(struct mlxsw_core *mlxsw_core,
			const struct devlink_trap *trap, void *trap_ctx)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mlxsw_sp_listener_devlink_map); i++) {
		struct mlxsw_listener *listener;

		if (mlxsw_sp_listener_devlink_map[i] != trap->id)
			continue;
		listener = &mlxsw_sp_listeners_arr[i];

		mlxsw_core_trap_unregister(mlxsw_core, listener, trap_ctx);
	}
}

int mlxsw_sp_trap_action_set(struct mlxsw_core *mlxsw_core,
			     const struct devlink_trap *trap,
			     enum devlink_trap_action action)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mlxsw_sp_listener_devlink_map); i++) {
		enum mlxsw_reg_hpkt_action hw_action;
		struct mlxsw_listener *listener;
		int err;

		if (mlxsw_sp_listener_devlink_map[i] != trap->id)
			continue;
		listener = &mlxsw_sp_listeners_arr[i];

		switch (action) {
		case DEVLINK_TRAP_ACTION_DROP:
			hw_action = MLXSW_REG_HPKT_ACTION_SET_FW_DEFAULT;
			break;
		case DEVLINK_TRAP_ACTION_TRAP:
			hw_action = MLXSW_REG_HPKT_ACTION_TRAP_EXCEPTION_TO_CPU;
			break;
		default:
			return -EINVAL;
		}

		err = mlxsw_core_trap_action_set(mlxsw_core, listener,
						 hw_action);
		if (err)
			return err;
	}

	return 0;
}

#define MLXSW_SP_DISCARD_POLICER_ID	(MLXSW_REG_HTGT_TRAP_GROUP_MAX + 1)

static int
mlxsw_sp_trap_group_policer_init(struct mlxsw_sp *mlxsw_sp,
				 const struct devlink_trap_group *group)
{
	enum mlxsw_reg_qpcr_ir_units ir_units;
	char qpcr_pl[MLXSW_REG_QPCR_LEN];
	u16 policer_id;
	u8 burst_size;
	bool is_bytes;
	u32 rate;

	switch (group->id) {
	case DEVLINK_TRAP_GROUP_GENERIC_ID_L2_DROPS: /* fall through */
	case DEVLINK_TRAP_GROUP_GENERIC_ID_L3_DROPS: /* fall through */
	case DEVLINK_TRAP_GROUP_GENERIC_ID_TUNNEL_DROPS:
		policer_id = MLXSW_SP_DISCARD_POLICER_ID;
		ir_units = MLXSW_REG_QPCR_IR_UNITS_M;
		is_bytes = false;
		rate = 10 * 1024; /* 10Kpps */
		burst_size = 7;
		break;
	default:
		return -EINVAL;
	}

	mlxsw_reg_qpcr_pack(qpcr_pl, policer_id, ir_units, is_bytes, rate,
			    burst_size);
	return mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(qpcr), qpcr_pl);
}

static int
__mlxsw_sp_trap_group_init(struct mlxsw_sp *mlxsw_sp,
			   const struct devlink_trap_group *group)
{
	char htgt_pl[MLXSW_REG_HTGT_LEN];
	u8 priority, tc, group_id;
	u16 policer_id;

	switch (group->id) {
	case DEVLINK_TRAP_GROUP_GENERIC_ID_L2_DROPS:
		group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_L2_DISCARDS;
		policer_id = MLXSW_SP_DISCARD_POLICER_ID;
		priority = 0;
		tc = 1;
		break;
	case DEVLINK_TRAP_GROUP_GENERIC_ID_L3_DROPS:
		group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_L3_DISCARDS;
		policer_id = MLXSW_SP_DISCARD_POLICER_ID;
		priority = 0;
		tc = 1;
		break;
	case DEVLINK_TRAP_GROUP_GENERIC_ID_TUNNEL_DROPS:
		group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_TUNNEL_DISCARDS;
		policer_id = MLXSW_SP_DISCARD_POLICER_ID;
		priority = 0;
		tc = 1;
		break;
	default:
		return -EINVAL;
	}

	mlxsw_reg_htgt_pack(htgt_pl, group_id, policer_id, priority, tc);
	return mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(htgt), htgt_pl);
}

int mlxsw_sp_trap_group_init(struct mlxsw_core *mlxsw_core,
			     const struct devlink_trap_group *group)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	int err;

	err = mlxsw_sp_trap_group_policer_init(mlxsw_sp, group);
	if (err)
		return err;

	err = __mlxsw_sp_trap_group_init(mlxsw_sp, group);
	if (err)
		return err;

	return 0;
}
