/**
 * This file contains helper routines for MGCP Gateway handling.
 *
 * The first thing to remember is that each BSC has its own namespace/range
 * of endpoints. Whenever a BSSMAP ASSIGNMENT REQUEST is received this code
 * will be called to select an endpoint on the BSC. The mapping from original
 * multiplex/timeslot to BSC multiplex'/timeslot' will be stored.
 *
 * The second part is to take messages on the public MGCP GW interface
 * and forward them to the right BSC. This requires the MSC to first
 * assign the timeslot. This assumption has been true so far. We are using
 * the policy_cb of the MGCP protocol code to decide if the request should
 * be immediately answered or delayed. An extension "Z: noanswer" is used
 * to request the BSC to not respond. This is saving some bytes of bandwidth
 * and as we are using TCP to forward the message we know it will arrive.
 * The mgcp_do_read method reads these messages and hands them to the protocol
 * parsing code which will call the mentioned policy_cb. The bsc_mgcp_forward
 * method is used on the way back from the BSC to the network.
 *
 * The third part is to patch messages forwarded to the BSC. This includes
 * the endpoint number, the ports to be used inside the SDP file and maybe
 * some other bits.
 *
 */
/*
 * (C) 2010-2012 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2010-2012 by On-Waves
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <openbsc/bsc_nat.h>
#include <openbsc/bsc_nat_sccp.h>
#include <openbsc/gsm_data.h>
#include <openbsc/debug.h>
#include <openbsc/ipaccess.h>
#include <openbsc/mgcp.h>
#include <openbsc/mgcp_internal.h>

#include <osmocom/sccp/sccp.h>

#include <osmocom/core/talloc.h>
#include <osmocom/gsm/gsm0808.h>
#include <osmocom/gsm/protocol/gsm_08_08.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <unistd.h>

static void send_direct(struct bsc_nat *nat, struct msgb *output)
{
	if (osmo_wqueue_enqueue(&nat->mgcp_cfg->gw_fd, output) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to queue MGCP msg.\n");
		msgb_free(output);
	}
}

static void mgcp_queue_for_call_agent(struct bsc_nat *nat, struct msgb *output)
{
	if (nat->mgcp_ipa)
		bsc_nat_send_mgcp_to_msc(nat, output);
	else
		send_direct(nat, output);
}

int bsc_mgcp_nr_multiplexes(int max_endpoints)
{
	int div = max_endpoints / 32;

	if ((max_endpoints % 32) != 0)
		div += 1;

	return div;
}

static int bsc_init_endps_if_needed(struct bsc_connection *con)
{
	int multiplexes;

	/* we have done that */
	if (con->_endpoint_status)
		return 0;

	/* we have no config... */
	if (!con->cfg)
		return -1;

	multiplexes = bsc_mgcp_nr_multiplexes(con->cfg->max_endpoints);
	con->number_multiplexes = multiplexes;
	con->max_endpoints = con->cfg->max_endpoints;
	con->_endpoint_status = talloc_zero_array(con, char, 32 * multiplexes + 1);
	return con->_endpoint_status == NULL;
}

static int bsc_assign_endpoint(struct bsc_connection *bsc, struct sccp_connections *con)
{
	int multiplex;
	int timeslot;
	const int number_endpoints = bsc->max_endpoints;
	int i;

	mgcp_endpoint_to_timeslot(bsc->last_endpoint, &multiplex, &timeslot);
	timeslot += 1;

	for (i = 0; i < number_endpoints; ++i) {
		int endpoint;

		/* Wrap around timeslots */
		if (timeslot == 0)
			timeslot = 1;

		if (timeslot == 0x1f) {
			timeslot = 1;
			multiplex += 1;
		}

		/* Wrap around the multiplex */
		if (multiplex >= bsc->number_multiplexes)
			multiplex = 0;

		endpoint = mgcp_timeslot_to_endpoint(multiplex, timeslot);

		/* Now check if we are allowed to assign this one */
		if (endpoint >= bsc->max_endpoints) {
			multiplex = 0;
			timeslot = 1;
			endpoint = mgcp_timeslot_to_endpoint(multiplex, timeslot);
		}


		if (bsc->_endpoint_status[endpoint] == 0) {
			bsc->_endpoint_status[endpoint] = 1;
			con->bsc_endp = endpoint;
			bsc->last_endpoint = endpoint;
			return 0;
		}

		timeslot += 1;
	}

	return -1;
}

static uint16_t create_cic(int endpoint)
{
	int timeslot, multiplex;

	mgcp_endpoint_to_timeslot(endpoint, &multiplex, &timeslot);
	return (multiplex << 5) | (timeslot & 0x1f);
}

int bsc_mgcp_assign_patch(struct sccp_connections *con, struct msgb *msg)
{
	struct sccp_connections *mcon;
	struct tlv_parsed tp;
	uint16_t cic;
	uint8_t timeslot;
	uint8_t multiplex;
	unsigned int endp;

	if (!msg->l3h) {
		LOGP(DNAT, LOGL_ERROR, "Assignment message should have l3h pointer.\n");
		return -1;
	}

	if (msgb_l3len(msg) < 3) {
		LOGP(DNAT, LOGL_ERROR, "Assignment message has not enough space for GSM0808.\n");
		return -1;
	}

	tlv_parse(&tp, gsm0808_att_tlvdef(), msg->l3h + 3, msgb_l3len(msg) - 3, 0, 0);
	if (!TLVP_PRESENT(&tp, GSM0808_IE_CIRCUIT_IDENTITY_CODE)) {
		LOGP(DNAT, LOGL_ERROR, "Circuit identity code not found in assignment message.\n");
		return -1;
	}

	cic = ntohs(*(uint16_t *)TLVP_VAL(&tp, GSM0808_IE_CIRCUIT_IDENTITY_CODE));
	timeslot = cic & 0x1f;
	multiplex = (cic & ~0x1f) >> 5;


	endp = mgcp_timeslot_to_endpoint(multiplex, timeslot);

	if (endp >= con->bsc->nat->mgcp_cfg->trunk.number_endpoints) {
		LOGP(DNAT, LOGL_ERROR,
			"MSC attempted to assign bad endpoint 0x%x\n",
			endp);
		return -1;
	}

	/* find stale connections using that endpoint */
	llist_for_each_entry(mcon, &con->bsc->nat->sccp_connections, list_entry) {
		if (mcon->msc_endp == endp) {
			LOGP(DNAT, LOGL_ERROR,
			     "Endpoint %d was assigned to 0x%x and now 0x%x\n",
			     endp,
			     sccp_src_ref_to_int(&mcon->patched_ref),
			     sccp_src_ref_to_int(&con->patched_ref));
			bsc_mgcp_dlcx(mcon);
		}
	}

	con->msc_endp = endp;
	if (bsc_init_endps_if_needed(con->bsc) != 0)
		return -1;
	if (bsc_assign_endpoint(con->bsc, con) != 0)
		return -1;

	/*
	 * now patch the message for the new CIC...
	 * still assumed to be one multiplex only
	 */
	cic = htons(create_cic(con->bsc_endp));
	memcpy((uint8_t *) TLVP_VAL(&tp, GSM0808_IE_CIRCUIT_IDENTITY_CODE),
		&cic, sizeof(cic));

	return 0;
}

static void bsc_mgcp_free_endpoint(struct bsc_nat *nat, int i)
{
	if (nat->bsc_endpoints[i].transaction_id) {
		talloc_free(nat->bsc_endpoints[i].transaction_id);
		nat->bsc_endpoints[i].transaction_id = NULL;
	}

	nat->bsc_endpoints[i].transaction_state = 0;
	nat->bsc_endpoints[i].bsc = NULL;
}

void bsc_mgcp_free_endpoints(struct bsc_nat *nat)
{
	int i;

	for (i = 1; i < nat->mgcp_cfg->trunk.number_endpoints; ++i){
		bsc_mgcp_free_endpoint(nat, i);
		mgcp_free_endp(&nat->mgcp_cfg->trunk.endpoints[i]);
	}
}

/* send a MDCX where we do not want a response */
static void bsc_mgcp_send_mdcx(struct bsc_connection *bsc, int port, struct mgcp_endpoint *endp)
{
	char buf[2096];
	int len;

	len = snprintf(buf, sizeof(buf),
		       "MDCX 23 %x@mgw MGCP 1.0\r\n"
		       "Z: noanswer\r\n"
		       "\r\n"
		       "c=IN IP4 %s\r\n"
		       "m=audio %d RTP/AVP 255\r\n",
		       port,
		       bsc->nat->mgcp_cfg->source_addr,
		       endp->bts_end.local_port);
	if (len < 0) {
		LOGP(DMGCP, LOGL_ERROR, "snprintf for MDCX failed.\n");
		return;
	}

	bsc_write_mgcp(bsc, (uint8_t *) buf, len);
}

static void bsc_mgcp_send_dlcx(struct bsc_connection *bsc, int endpoint)
{
	char buf[2096];
	int len;

	len = snprintf(buf, sizeof(buf),
		       "DLCX 26 %x@mgw MGCP 1.0\r\n"
		       "Z: noanswer\r\n", endpoint);
	if (len < 0) {
		LOGP(DMGCP, LOGL_ERROR, "snprintf for DLCX failed.\n");
		return;
	}

	bsc_write_mgcp(bsc, (uint8_t *) buf, len);
}

void bsc_mgcp_init(struct sccp_connections *con)
{
	con->msc_endp = -1;
	con->bsc_endp = -1;
}

void bsc_mgcp_dlcx(struct sccp_connections *con)
{
	/* send a DLCX down the stream */
	if (con->bsc_endp != -1 && con->bsc->_endpoint_status) {
		if (con->bsc->_endpoint_status[con->bsc_endp] != 1)
			LOGP(DNAT, LOGL_ERROR, "Endpoint 0x%x was not in use\n", con->bsc_endp);
		con->bsc->_endpoint_status[con->bsc_endp] = 0;
		bsc_mgcp_send_dlcx(con->bsc, con->bsc_endp);
		bsc_mgcp_free_endpoint(con->bsc->nat, con->msc_endp);
	}

	bsc_mgcp_init(con);
}


struct sccp_connections *bsc_mgcp_find_con(struct bsc_nat *nat, int endpoint)
{
	struct sccp_connections *con = NULL;
	struct sccp_connections *sccp;

	llist_for_each_entry(sccp, &nat->sccp_connections, list_entry) {
		if (sccp->msc_endp == -1)
			continue;
		if (sccp->msc_endp != endpoint)
			continue;

		con = sccp;
	}

	if (con)
		return con;

	LOGP(DMGCP, LOGL_ERROR, "Failed to find the connection.\n");
	return NULL;
}

static int bsc_mgcp_policy_cb(struct mgcp_trunk_config *tcfg, int endpoint, int state, const char *transaction_id)
{
	struct bsc_nat *nat;
	struct bsc_endpoint *bsc_endp;
	struct sccp_connections *sccp;
	struct mgcp_endpoint *mgcp_endp;
	struct msgb *bsc_msg;

	nat = tcfg->cfg->data;
	bsc_endp = &nat->bsc_endpoints[endpoint];
	mgcp_endp = &nat->mgcp_cfg->trunk.endpoints[endpoint];

	if (bsc_endp->transaction_id) {
		LOGP(DMGCP, LOGL_ERROR, "Endpoint 0x%x had pending transaction: '%s'\n",
		     endpoint, bsc_endp->transaction_id);
		talloc_free(bsc_endp->transaction_id);
		bsc_endp->transaction_id = NULL;
		bsc_endp->transaction_state = 0;
	}
	bsc_endp->bsc = NULL;

	sccp = bsc_mgcp_find_con(nat, endpoint);

	if (!sccp) {
		LOGP(DMGCP, LOGL_ERROR, "Did not find BSC for change on endpoint: 0x%x state: %d\n", endpoint, state);

		switch (state) {
		case MGCP_ENDP_CRCX:
			return MGCP_POLICY_REJECT;
			break;
		case MGCP_ENDP_DLCX:
			return MGCP_POLICY_CONT;
			break;
		case MGCP_ENDP_MDCX:
			return MGCP_POLICY_CONT;
			break;
		default:
			LOGP(DMGCP, LOGL_FATAL, "Unhandled state: %d\n", state);
			return MGCP_POLICY_CONT;
			break;
		}
	}

	/* we need to generate a new and patched message */
	bsc_msg = bsc_mgcp_rewrite((char *) nat->mgcp_msg, nat->mgcp_length, sccp->bsc_endp,
				   nat->mgcp_cfg->source_addr, mgcp_endp->bts_end.local_port);
	if (!bsc_msg) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to patch the msg.\n");
		return MGCP_POLICY_CONT;
	}


	bsc_endp->transaction_id = talloc_strdup(nat, transaction_id);
	bsc_endp->transaction_state = state;
	bsc_endp->bsc = sccp->bsc;

	/* we need to update some bits */
	if (state == MGCP_ENDP_CRCX) {
		struct sockaddr_in sock;
		socklen_t len = sizeof(sock);
		if (getpeername(sccp->bsc->write_queue.bfd.fd, (struct sockaddr *) &sock, &len) != 0) {
			LOGP(DMGCP, LOGL_ERROR, "Can not get the peername...%d/%s\n",
			      errno, strerror(errno));
		} else {
			mgcp_endp->bts_end.addr = sock.sin_addr;
		}

		/* send the message and a fake MDCX to force sending of a dummy packet */
		bsc_write(sccp->bsc, bsc_msg, IPAC_PROTO_MGCP_OLD);
		bsc_mgcp_send_mdcx(sccp->bsc, sccp->bsc_endp, mgcp_endp);
		return MGCP_POLICY_DEFER;
	} else if (state == MGCP_ENDP_DLCX) {
		/* we will free the endpoint now and send a DLCX to the BSC */
		msgb_free(bsc_msg);
		bsc_mgcp_dlcx(sccp);
		return MGCP_POLICY_CONT;
	} else {
		bsc_write(sccp->bsc, bsc_msg, IPAC_PROTO_MGCP_OLD);
		return MGCP_POLICY_DEFER;
	}
}

/*
 * We do have a failure, free data downstream..
 */
static void free_chan_downstream(struct mgcp_endpoint *endp, struct bsc_endpoint *bsc_endp,
				 struct bsc_connection *bsc)
{
	LOGP(DMGCP, LOGL_ERROR, "No CI, freeing endpoint 0x%x in state %d\n",
		ENDPOINT_NUMBER(endp), bsc_endp->transaction_state);

	/* if a CRCX failed... send a DLCX down the stream */
	if (bsc_endp->transaction_state == MGCP_ENDP_CRCX) {
		struct sccp_connections *con;
		con = bsc_mgcp_find_con(bsc->nat, ENDPOINT_NUMBER(endp));
		if (!con) {
			LOGP(DMGCP, LOGL_ERROR,
				"No SCCP connection for endp 0x%x\n",
				ENDPOINT_NUMBER(endp));
		} else {
			if (con->bsc == bsc) {
				bsc_mgcp_send_dlcx(bsc, con->bsc_endp);
			} else {
				LOGP(DMGCP, LOGL_ERROR,
					"Endpoint belongs to a different BSC\n");
			}
		}
	}

	bsc_mgcp_free_endpoint(bsc->nat, ENDPOINT_NUMBER(endp));
	mgcp_free_endp(endp);
}

/*
 * We have received a msg from the BSC. We will see if we know
 * this transaction and if it belongs to the BSC. Then we will
 * need to patch the content to point to the local network and we
 * need to update the I: that was assigned by the BSS.
 */
void bsc_mgcp_forward(struct bsc_connection *bsc, struct msgb *msg)
{
	struct msgb *output;
	struct bsc_endpoint *bsc_endp = NULL;
	struct mgcp_endpoint *endp = NULL;
	int i, code;
	char transaction_id[60];

	/* Some assumption that our buffer is big enough.. and null terminate */
	if (msgb_l2len(msg) > 2000) {
		LOGP(DMGCP, LOGL_ERROR, "MGCP message too long.\n");
		return;
	}

	msg->l2h[msgb_l2len(msg)] = '\0';

	if (bsc_mgcp_parse_response((const char *) msg->l2h, &code, transaction_id) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to parse response code.\n");
		return;
	}

	for (i = 1; i < bsc->nat->mgcp_cfg->trunk.number_endpoints; ++i) {
		if (bsc->nat->bsc_endpoints[i].bsc != bsc)
			continue;
		/* no one listening? a bug? */
		if (!bsc->nat->bsc_endpoints[i].transaction_id)
			continue;
		if (strcmp(transaction_id, bsc->nat->bsc_endpoints[i].transaction_id) != 0)
			continue;

		endp = &bsc->nat->mgcp_cfg->trunk.endpoints[i];
		bsc_endp = &bsc->nat->bsc_endpoints[i];
		break;
	}

	if (!bsc_endp) {
		LOGP(DMGCP, LOGL_ERROR, "Could not find active endpoint: %s for msg: '%s'\n",
		     transaction_id, (const char *) msg->l2h);
		return;
	}

	endp->ci = bsc_mgcp_extract_ci((const char *) msg->l2h);
	if (endp->ci == CI_UNUSED) {
		free_chan_downstream(endp, bsc_endp, bsc);
		return;
	}

	/* free some stuff */
	talloc_free(bsc_endp->transaction_id);
	bsc_endp->transaction_id = NULL;
	bsc_endp->transaction_state = 0;

	/*
	 * rewrite the information. In case the endpoint was deleted
	 * there should be nothing for us to rewrite so putting endp->rtp_port
	 * with the value of 0 should be no problem.
	 */
	output = bsc_mgcp_rewrite((char * ) msg->l2h, msgb_l2len(msg), -1,
				  bsc->nat->mgcp_cfg->source_addr, endp->net_end.local_port);

	if (!output) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to rewrite MGCP msg.\n");
		return;
	}

	mgcp_queue_for_call_agent(bsc->nat, output);
}

int bsc_mgcp_parse_response(const char *str, int *code, char transaction[60])
{
	/* we want to parse two strings */
	return sscanf(str, "%3d %59s\n", code, transaction) != 2;
}

uint32_t bsc_mgcp_extract_ci(const char *str)
{
	unsigned int ci;
	char *res = strstr(str, "I: ");
	if (!res) {
		LOGP(DMGCP, LOGL_ERROR, "No CI in msg '%s'\n", str);
		return CI_UNUSED;
	}

	if (sscanf(res, "I: %u", &ci) != 1) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to parse CI in msg '%s'\n", str);
		return CI_UNUSED;
	}

	return ci;
}

/**
 * Create a new MGCPCommand based on the input and endpoint from a message
 */
static void patch_mgcp(struct msgb *output, const char *op, const char *tok,
		       int endp, int len, int cr)
{
	int slen;
	int ret;
	char buf[40];

	buf[0] = buf[39] = '\0';
	ret = sscanf(tok, "%*s %s", buf);
	if (ret != 1) {
		LOGP(DMGCP, LOGL_ERROR,
			"Failed to find Endpoint in: %s\n", tok);
		return;
	}

	slen = sprintf((char *) output->l3h, "%s %s %x@mgw MGCP 1.0%s",
			op, buf, endp, cr ? "\r\n" : "\n");
	output->l3h = msgb_put(output, slen);
}

/* we need to replace some strings... */
struct msgb *bsc_mgcp_rewrite(char *input, int length, int endpoint, const char *ip, int port)
{
	static const char crcx_str[] = "CRCX ";
	static const char dlcx_str[] = "DLCX ";
	static const char mdcx_str[] = "MDCX ";

	static const char ip_str[] = "c=IN IP4 ";
	static const char aud_str[] = "m=audio ";
	static const char fmt_str[] = "a=fmtp:";

	char buf[128];
	char *running, *token;
	struct msgb *output;

	/* keep state to add the a=fmtp line */
	int found_fmtp = 0;
	int payload = -1;
	int cr = 1;

	if (length > 4096 - 256) {
		LOGP(DMGCP, LOGL_ERROR, "Input is too long.\n");
		return NULL;
	}

	output = msgb_alloc_headroom(4096, 128, "MGCP rewritten");
	if (!output) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to allocate new MGCP msg.\n");
		return NULL;
	}

	running = input;
	output->l2h = output->data;
	output->l3h = output->l2h;
	for (token = strsep(&running, "\n"); running; token = strsep(&running, "\n")) {
		int len = strlen(token);
		cr = len > 0 && token[len - 1] == '\r';

		if (strncmp(crcx_str, token, (sizeof crcx_str) - 1) == 0) {
			patch_mgcp(output, "CRCX", token, endpoint, len, cr);
		} else if (strncmp(dlcx_str, token, (sizeof dlcx_str) - 1) == 0) {
			patch_mgcp(output, "DLCX", token, endpoint, len, cr);
		} else if (strncmp(mdcx_str, token, (sizeof mdcx_str) - 1) == 0) {
			patch_mgcp(output, "MDCX", token, endpoint, len, cr);
		} else if (strncmp(ip_str, token, (sizeof ip_str) - 1) == 0) {
			output->l3h = msgb_put(output, strlen(ip_str));
			memcpy(output->l3h, ip_str, strlen(ip_str));
			output->l3h = msgb_put(output, strlen(ip));
			memcpy(output->l3h, ip, strlen(ip));

			if (cr) {
				output->l3h = msgb_put(output, 2);
				output->l3h[0] = '\r';
				output->l3h[1] = '\n';
			} else {
				output->l3h = msgb_put(output, 1);
				output->l3h[0] = '\n';
			}
		} else if (strncmp(aud_str, token, (sizeof aud_str) - 1) == 0) {
			if (sscanf(token, "m=audio %*d RTP/AVP %d", &payload) != 1) {
				LOGP(DMGCP, LOGL_ERROR, "Could not parsed audio line.\n");
				msgb_free(output);
				return NULL;
			}

			snprintf(buf, sizeof(buf)-1, "m=audio %d RTP/AVP %d%s",
				 port, payload, cr ? "\r\n" : "\n");
			buf[sizeof(buf)-1] = '\0';

			output->l3h = msgb_put(output, strlen(buf));
			memcpy(output->l3h, buf, strlen(buf));
		} else if (strncmp(fmt_str, token, (sizeof fmt_str) - 1) == 0) {
			found_fmtp = 1;
			goto copy;
		} else {
copy:
			output->l3h = msgb_put(output, len + 1);
			memcpy(output->l3h, token, len);
			output->l3h[len] = '\n';
		}
	}

	/*
	 * the above code made sure that we have 128 bytes lefts. So we can
	 * safely append another line.
	 */
	if (!found_fmtp && payload != -1) {
		snprintf(buf, sizeof(buf) - 1, "a=fmtp:%d mode-set=2%s",
			payload, cr ? "\r\n" : "\n");
		buf[sizeof(buf) - 1] = '\0';
		output->l3h = msgb_put(output, strlen(buf));
		memcpy(output->l3h, buf, strlen(buf));
	}

	return output;
}

/*
 * This comes from the MSC and we will now parse it. The caller needs
 * to free the msgb.
 */
void bsc_nat_handle_mgcp(struct bsc_nat *nat, struct msgb *msg)
{
	struct msgb *resp;

	if (!nat->mgcp_ipa) {
		LOGP(DMGCP, LOGL_ERROR, "MGCP message not allowed on IPA.\n");
		return;
	}

	if (msgb_l2len(msg) > sizeof(nat->mgcp_msg) - 1) {
		LOGP(DMGCP, LOGL_ERROR, "MGCP msg too big for handling.\n");
		return;
	}

	memcpy(nat->mgcp_msg, msg->l2h, msgb_l2len(msg));
	nat->mgcp_length = msgb_l2len(msg);
	nat->mgcp_msg[nat->mgcp_length] = '\0';

	/* now handle the message */
	resp = mgcp_handle_message(nat->mgcp_cfg, msg);

	/* we do have a direct answer... e.g. AUEP */
	if (resp)
		mgcp_queue_for_call_agent(nat, resp);

	return;
}

static int mgcp_do_read(struct osmo_fd *fd)
{
	struct bsc_nat *nat;
	struct msgb *msg, *resp;
	int rc;

	nat = fd->data;

	rc = read(fd->fd, nat->mgcp_msg, sizeof(nat->mgcp_msg) - 1);
	if (rc <= 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to read errno: %d\n", errno);
		return -1;
	}

	nat->mgcp_msg[rc] = '\0';
	nat->mgcp_length = rc;

	msg = msgb_alloc(sizeof(nat->mgcp_msg), "MGCP GW Read");
	if (!msg) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to create buffer.\n");
		return -1;
	}

	msg->l2h = msgb_put(msg, rc);
	memcpy(msg->l2h, nat->mgcp_msg, msgb_l2len(msg));
	resp = mgcp_handle_message(nat->mgcp_cfg, msg);
	msgb_free(msg);

	/* we do have a direct answer... e.g. AUEP */
	if (resp)
		mgcp_queue_for_call_agent(nat, resp);

	return 0;
}

static int mgcp_do_write(struct osmo_fd *bfd, struct msgb *msg)
{
	int rc;

	rc = write(bfd->fd, msg->data, msg->len);

	if (rc != msg->len) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to write msg to MGCP CallAgent.\n");
		return -1;
	}

	return rc;
}

static int init_mgcp_socket(struct bsc_nat *nat, struct mgcp_config *cfg)
{
	struct sockaddr_in addr;
	int on;

	cfg->gw_fd.bfd.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (cfg->gw_fd.bfd.fd < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to create MGCP socket. errno: %d\n", errno);
		return -1;
	}

	on = 1;
	setsockopt(cfg->gw_fd.bfd.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(cfg->source_port);
	inet_aton(cfg->source_addr, &addr.sin_addr);

	if (bind(cfg->gw_fd.bfd.fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to bind on %s:%d errno: %d\n",
		     cfg->source_addr, cfg->source_port, errno);
		close(cfg->gw_fd.bfd.fd);
		cfg->gw_fd.bfd.fd = -1;
		return -1;
	}

	addr.sin_port = htons(2727);
	inet_aton(cfg->call_agent_addr, &addr.sin_addr);
	if (connect(cfg->gw_fd.bfd.fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to connect to: '%s'. errno: %d\n",
		     cfg->call_agent_addr, errno);
		close(cfg->gw_fd.bfd.fd);
		cfg->gw_fd.bfd.fd = -1;
		return -1;
	}

	osmo_wqueue_init(&cfg->gw_fd, 10);
	cfg->gw_fd.bfd.when = BSC_FD_READ;
	cfg->gw_fd.bfd.data = nat;
	cfg->gw_fd.read_cb = mgcp_do_read;
	cfg->gw_fd.write_cb = mgcp_do_write;

	if (osmo_fd_register(&cfg->gw_fd.bfd) != 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to register MGCP fd.\n");
		close(cfg->gw_fd.bfd.fd);
		cfg->gw_fd.bfd.fd = -1;
		return -1;
	}

	return 0;
}

int bsc_mgcp_nat_init(struct bsc_nat *nat)
{
	struct mgcp_config *cfg = nat->mgcp_cfg;

	if (!cfg->call_agent_addr) {
		LOGP(DMGCP, LOGL_ERROR, "The BSC nat requires the call agent ip to be set.\n");
		return -1;
	}

	if (cfg->bts_ip) {
		LOGP(DMGCP, LOGL_ERROR, "Do not set the BTS ip for the nat.\n");
		return -1;
	}

	/* initialize the MGCP socket */
	if (!nat->mgcp_ipa) {
		int rc =  init_mgcp_socket(nat, cfg);
		if (rc != 0)
			return rc;
	}


	/* some more MGCP config handling */
	cfg->data = nat;
	cfg->policy_cb = bsc_mgcp_policy_cb;
	cfg->trunk.force_realloc = 1;

	if (cfg->bts_ip)
		talloc_free(cfg->bts_ip);
	cfg->bts_ip = "";

	nat->bsc_endpoints = talloc_zero_array(nat,
					       struct bsc_endpoint,
					       cfg->trunk.number_endpoints + 1);
	if (!nat->bsc_endpoints) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to allocate nat endpoints\n");
		close(cfg->gw_fd.bfd.fd);
		cfg->gw_fd.bfd.fd = -1;
		return -1;
	}

	if (mgcp_reset_transcoder(cfg) < 0) {
		LOGP(DMGCP, LOGL_ERROR, "Failed to send packet to the transcoder.\n");
		talloc_free(nat->bsc_endpoints);
		nat->bsc_endpoints = NULL;
		close(cfg->gw_fd.bfd.fd);
		cfg->gw_fd.bfd.fd = -1;
		return -1;
	}

	return 0;
}

void bsc_mgcp_clear_endpoints_for(struct bsc_connection *bsc)
{
	struct rate_ctr *ctr = NULL;
	int i;

	if (bsc->cfg)
		ctr = &bsc->cfg->stats.ctrg->ctr[BCFG_CTR_DROPPED_CALLS];

	for (i = 1; i < bsc->nat->mgcp_cfg->trunk.number_endpoints; ++i) {
		struct bsc_endpoint *bsc_endp = &bsc->nat->bsc_endpoints[i];

		if (bsc_endp->bsc != bsc)
			continue;

		if (ctr)
			rate_ctr_inc(ctr);

		bsc_mgcp_free_endpoint(bsc->nat, i);
		mgcp_free_endp(&bsc->nat->mgcp_cfg->trunk.endpoints[i]);
	}
}
