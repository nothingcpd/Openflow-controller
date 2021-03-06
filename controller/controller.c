#include <stdlib.h>
#include <stdio.h>

#include "trema.h"
#define TCPO_NOOP		1
#define TCPO_MPTCP		30
#define MPTCP_CAPABLE		0
#define MPTCP_JOIN		1

#define SWITCH_NUMBER		3

struct switch_t {
	int next_port;
	char host_mac[6];
};

struct switch_t sws[SWITCH_NUMBER];

/* Generate host mac address */
static inline void host_mac(int i, char *mac)
{
	int j;
	for (j = 0; j < 5; j++)
		mac[j] = 0;
	mac[5] = i;
}

/* Print mac address */
static inline void print_mac(char *mac)
{
	int i = 0;
	for (i = 0; i < 6; i++)
		printf("%x, ", (int)mac[i]);
	printf("\n");
}

static int find_mptcp_flag(packet_info pi, uint8_t flag)
{
	tcp_header_t *tcp_h;
	void *tcp_o, *tcp_d;
	uint8_t kind, length, subtype;

	tcp_h = pi.l4_header;
	tcp_o = (void *)tcp_h + sizeof(tcp_header_t);
	tcp_d = (void *)tcp_h + tcp_h->offset * 4;


	while (tcp_o < tcp_d) {
		kind = *(uint8_t *)tcp_o;
		if (kind == TCPO_MPTCP) {
			subtype = *(uint8_t *)(tcp_o + 2);
			subtype >>= 4;
			return subtype == flag;
		}

		/* Skip no operation option */
		if (kind == TCPO_NOOP) {
			tcp_o++;
			continue;
		}

		length = *(uint8_t *)(tcp_o + 1);
		tcp_o += length;
	}

	return 0;
}


/* Install a rule */
static void set_rule(int sw, char *src_mac, char *dst_mac, int port,
		int tcp_src_port, int tcp_dst_port)
{
	int res = 0;
	openflow_actions *actions;
	struct ofp_match match;
	buffer *buff;


	/* Install rule to forward flow on the next port */
	memset(&match, 0, sizeof(struct ofp_match));
	match.wildcards = OFPFW_ALL;

	if (dst_mac) {
		match.wildcards ^= OFPFW_DL_DST;
		memcpy(match.dl_dst, dst_mac, 6);
	}
	if (src_mac) {
		match.wildcards ^= OFPFW_DL_SRC;
		memcpy(match.dl_src, src_mac, 6);
	}
	if (tcp_src_port > 0) {
		match.wildcards ^= OFPFW_TP_SRC;
		match.tp_src = tcp_src_port;
	}
	if (tcp_dst_port > 0) {
		match.wildcards ^= OFPFW_TP_DST;
		match.tp_dst = tcp_dst_port;
	}
	match.wildcards ^= OFPFW_NW_PROTO;
	match.nw_proto = IPPROTO_TCP;
	match.wildcards ^= OFPFW_DL_TYPE;
	match.dl_type = 0x0800;

	actions = create_actions();
	append_action_output(actions, port, 0);

	buff = create_flow_mod(get_transaction_id(), match, get_cookie(),
			OFPFC_ADD, OFP_FLOW_PERMANENT, OFP_FLOW_PERMANENT,
			0xffff, -1, OFPP_NONE, OFPFF_SEND_FLOW_REM, actions);
	if (!(res = send_openflow_message(sw, buff)))
		printf("Error installing rule, res %d\n", res);

	delete_actions(actions);
	free_buffer(buff);
}


/* Handler for switch connected event */
static void switch_connected(uint64_t datapath_id, void *user_data)
{
	printf("A switch has connected switch %d\n", (int)datapath_id);
	if (datapath_id > SWITCH_NUMBER) {
		printf("Unsuported switch\n");
		exit(0);
	}

	/* Set switch specific information */
	sws[datapath_id - 1].next_port = 2;
	host_mac(datapath_id, sws[datapath_id - 1].host_mac);
}

/* Install rule to forward all the packets unrelated to the switch's
 * host */
static void packet_forward(int sw, char *src_mac, char *dst_mac, int in_port,
		int tcp_src_port, int tcp_dst_port)
{
	int res = 0, port;
	openflow_actions *actions;
	struct ofp_match match;
	buffer *buff;

	/* Find the number of the other port */
	switch (in_port) {
		case 2:
			port = 3;
			break;
		case 3:
			port = 2;
			break;
		default:
			printf("Inport is %d, this shouldn't be handled here\n", in_port);
	}

	printf("Must forward this to port %d\n", port);

	/* Install rule to forward flow on the next port */
	set_rule(sw, src_mac, dst_mac, port, tcp_src_port, tcp_dst_port);
}

/* Install rule to forward flow to the host */
static void packet_to_host(int sw, char *src_mac, char *dst_mac, int in_port,
		int tcp_src_port, int tcp_dst_port)
{
	int res = 0;
	openflow_actions *actions;
	struct ofp_match match;
	buffer *buff;

	printf("Must forward this to my host\n");

	/* Install rule to forward flow on the switch's host */
	set_rule(sw, src_mac, dst_mac, 1, tcp_src_port, tcp_dst_port);

	/* Install rule to forward packets back to the sender */
	printf("Also install rule to go back through %d\n", in_port);
	set_rule(sw, dst_mac, src_mac, in_port, tcp_dst_port, tcp_src_port);
}

/* Install rule to forward flow on the next port */
static void packet_from_host(int sw, char *src_mac, char *dst_mac,
		int tcp_src_port, int tcp_dst_port)
{
	int res = 0, next_port = sws[sw - 1].next_port;
	openflow_actions *actions;
	struct ofp_match match;
	buffer *buff;

	printf("Must alternate this to port %d\n", next_port);

	/* Set the future next port */
	if (sws[sw - 1].next_port == 2)
		sws[sw - 1].next_port = 3;
	else
		sws[sw - 1].next_port = 2;


	/* Install rule to forward flow on the next port */
	set_rule(sw, src_mac, dst_mac, next_port, tcp_src_port, tcp_dst_port);
	printf("GOTO %d, for tcp src port %d, and mac\n", next_port, tcp_src_port);
	print_mac(src_mac);

	/* Install rule to forward packets back to the host */
	printf("Also install rule to go back to host\n");
	memset(&match, 0, sizeof(struct ofp_match));
	set_rule(sw, dst_mac, src_mac, 1, tcp_dst_port, tcp_src_port);
}

/* Handler for packet in event */
void packet_in_h(uint64_t datapath_id,
		uint32_t transaction_id,
		uint32_t buffer_id,
		uint16_t total_len,
		uint16_t in_port,
		uint8_t reason,
		const buffer *data,
		void *user_data)
{
	char src_mac[6], dst_mac[6];
	int tcp_src_port, tcp_dst_port;
	int res = 0;

	packet_info packet_info = get_packet_info(data);

	/* Filter out non-TCP traffic */
	if (!(packet_info.format & TP_TCP))
		return;

	/* Get packet information */
	memcpy(src_mac, packet_info.eth_macsa, OFP_ETH_ALEN);
	memcpy(dst_mac, packet_info.eth_macda, OFP_ETH_ALEN);
	tcp_src_port = packet_info.tcp_src_port;
	tcp_dst_port = packet_info.tcp_dst_port;

	printf("\nReceived packet on s%d from port %d, TCP src %d, dst %d with info:\n",
			(int)datapath_id, in_port, tcp_src_port, tcp_dst_port);
	print_mac(src_mac);
	print_mac(dst_mac);

#if CONFIG_MPTCP
	int is_mptcp = find_mptcp_flag(packet_info, MPTCP_CAPABLE);
	int is_mptcp_join = find_mptcp_flag(packet_info, MPTCP_JOIN);
	printf("MPTCP capable %d, MPTCP join %d\n", is_mptcp, is_mptcp_join);
#endif

	/* Case 1:
	 * Switch receives a packet from its host
	 * Forward it alternatively on the two exit ports */
	if (!memcmp(src_mac, sws[datapath_id - 1].host_mac, 6)) {
		packet_from_host((int)datapath_id, src_mac, dst_mac,
				tcp_src_port, tcp_dst_port);
		return;
	}


	/* Case 2:
	 * Switch receives a packet for its host
	 * Set a rule to forward all packets to host */
	if (!memcmp(dst_mac, sws[datapath_id - 1].host_mac, 6)) {
		packet_to_host((int)datapath_id, src_mac, dst_mac, in_port,
				tcp_src_port, tcp_dst_port);
		return;
	}


	/* Case 3:
	 * Switch receives a packet which is not related to the host
	 * Forward it on the other free port */
	packet_forward((int)datapath_id, src_mac, dst_mac, in_port,
			tcp_src_port, tcp_dst_port);
}


int main(int argc, char **argv)
{
	init_trema(&argc, &argv);

	printf("Waiting for a switch to register\n");
	set_switch_ready_handler(switch_connected, NULL);
	set_packet_in_handler(packet_in_h, NULL);

	start_trema();

	return EXIT_SUCCESS;
}
