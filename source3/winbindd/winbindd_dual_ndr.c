/*
   Unix SMB/CIFS implementation.

   Provide parent->child communication based on NDR marshalling

   Copyright (C) Volker Lendecke 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * This file implements an RPC between winbind parent and child processes,
 * leveraging the autogenerated marshalling routines for MSRPC. This is not
 * MSRPC, as it does not go through the whole DCERPC fragmentation, we just
 * leverage much the same infrastructure we already have for it.
 */

#include "includes.h"
#include "winbindd/winbindd.h"
#include "winbindd/winbindd_proto.h"
#include "librpc/gen_ndr/srv_wbint.h"

struct wb_ndr_transport_priv {
	struct winbindd_domain *domain;
	struct winbindd_child *child;
};

struct wb_ndr_dispatch_state {
	struct wb_ndr_transport_priv *transport;
	uint32_t opnum;
	const struct ndr_interface_call *call;
	void *r;
	DATA_BLOB req_blob, resp_blob;
	struct winbindd_request request;
	struct winbindd_response *response;
};

static void wb_ndr_dispatch_done(struct tevent_req *subreq);

static struct tevent_req *wb_ndr_dispatch_send(TALLOC_CTX *mem_ctx,
					       struct tevent_context *ev,
					       struct rpc_pipe_client *cli,
					       const struct ndr_interface_table *table,
					       uint32_t opnum,
					       void *r)
{
	struct tevent_req *req, *subreq;
	struct wb_ndr_dispatch_state *state;
	struct wb_ndr_transport_priv *transport = talloc_get_type_abort(
		cli->transport->priv, struct wb_ndr_transport_priv);
	struct ndr_push *push;
	enum ndr_err_code ndr_err;

	req = tevent_req_create(mem_ctx, &state,
				struct wb_ndr_dispatch_state);
	if (req == NULL) {
		return NULL;
	}

	state->r = r;
	state->call = &table->calls[opnum];
	state->transport = transport;
	state->opnum = opnum;

	push = ndr_push_init_ctx(state, NULL);
	if (tevent_req_nomem(push, req)) {
		return tevent_req_post(req, ev);
	}

	ndr_err = state->call->ndr_push(push, NDR_IN, r);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		tevent_req_nterror(req, ndr_map_error2ntstatus(ndr_err));
		TALLOC_FREE(push);
		return tevent_req_post(req, ev);
	}

	state->req_blob = ndr_push_blob(push);

	if ((transport->domain != NULL)
	    && wcache_fetch_ndr(state, transport->domain, opnum,
				&state->req_blob, &state->resp_blob)) {
		tevent_req_done(req);
		return tevent_req_post(req, ev);
	}

	state->request.cmd = WINBINDD_DUAL_NDRCMD;
	state->request.data.ndrcmd = opnum;
	state->request.extra_data.data = (char *)state->req_blob.data;
	state->request.extra_len = state->req_blob.length;

	subreq = wb_child_request_send(state, ev, transport->child,
				       &state->request);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, wb_ndr_dispatch_done, req);
	return req;
}

static void wb_ndr_dispatch_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_ndr_dispatch_state *state = tevent_req_data(
		req, struct wb_ndr_dispatch_state);
	int ret, err;

	ret = wb_child_request_recv(subreq, state, &state->response, &err);
	TALLOC_FREE(subreq);
	if (ret == -1) {
		tevent_req_nterror(req, map_nt_error_from_unix(err));
		return;
	}

	state->resp_blob = data_blob_const(
		state->response->extra_data.data,
		state->response->length	- sizeof(struct winbindd_response));

	if (state->transport->domain != NULL) {
		wcache_store_ndr(state->transport->domain, state->opnum,
				 &state->req_blob, &state->resp_blob);
	}

	tevent_req_done(req);
}

static NTSTATUS wb_ndr_dispatch_recv(struct tevent_req *req,
				     TALLOC_CTX *mem_ctx)
{
	struct wb_ndr_dispatch_state *state = tevent_req_data(
		req, struct wb_ndr_dispatch_state);
	NTSTATUS status;
	struct ndr_pull *pull;
	enum ndr_err_code ndr_err;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}

	pull = ndr_pull_init_blob(&state->resp_blob, mem_ctx, NULL);
	if (pull == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/* have the ndr parser alloc memory for us */
	pull->flags |= LIBNDR_FLAG_REF_ALLOC;
	ndr_err = state->call->ndr_pull(pull, NDR_OUT, state->r);
	TALLOC_FREE(pull);

	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return ndr_map_error2ntstatus(ndr_err);
	}

	return NT_STATUS_OK;
}

static NTSTATUS wb_ndr_dispatch(struct rpc_pipe_client *cli,
				TALLOC_CTX *mem_ctx,
				const struct ndr_interface_table *table,
				uint32_t opnum, void *r)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct event_context *ev;
	struct tevent_req *req;
	NTSTATUS status = NT_STATUS_OK;

	ev = event_context_init(frame);
	if (ev == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	req = wb_ndr_dispatch_send(frame, ev, cli, table, opnum, r);
	if (req == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto fail;
	}

	if (!tevent_req_poll(req, ev)) {
		status = map_nt_error_from_unix(errno);
		goto fail;
	}

	status = wb_ndr_dispatch_recv(req, mem_ctx);
 fail:
	TALLOC_FREE(frame);
	return status;
}

struct rpc_pipe_client *wbint_rpccli_create(TALLOC_CTX *mem_ctx,
					    struct winbindd_domain *domain,
					    struct winbindd_child *child)
{
	struct rpc_pipe_client *result;
	struct wb_ndr_transport_priv *transp;

	result = talloc(mem_ctx, struct rpc_pipe_client);
	if (result == NULL) {
		return NULL;
	}
	result->abstract_syntax = ndr_table_wbint.syntax_id;
	result->transfer_syntax = ndr_transfer_syntax;
	result->dispatch = wb_ndr_dispatch;
	result->dispatch_send = wb_ndr_dispatch_send;
	result->dispatch_recv = wb_ndr_dispatch_recv;
	result->max_xmit_frag = RPC_MAX_PDU_FRAG_LEN;
	result->max_recv_frag = RPC_MAX_PDU_FRAG_LEN;
	result->desthost = NULL;
	result->srv_name_slash = NULL;

	/*
	 * Initialize a fake transport. Due to our own wb_ndr_dispatch
	 * function we don't use all the fragmentation engine in
	 * cli_pipe, which would use all the _read and _write
	 * functions in rpc_cli_transport. But we need a place to
	 * store the child struct in, and we're re-using
	 * result->transport->priv for that.
	 */

	result->transport = talloc_zero(result, struct rpc_cli_transport);
	if (result->transport == NULL) {
		TALLOC_FREE(result);
		return NULL;
	}
	transp = talloc(result->transport, struct wb_ndr_transport_priv);
	if (transp == NULL) {
		TALLOC_FREE(result);
		return NULL;
	}
	transp->domain = domain;
	transp->child = child;
	result->transport->priv = transp;
	return result;
}

enum winbindd_result winbindd_dual_ndrcmd(struct winbindd_domain *domain,
					  struct winbindd_cli_state *state)
{
	pipes_struct p;
	struct api_struct *fns;
	int num_fns;
	bool ret;

	wbint_get_pipe_fns(&fns, &num_fns);

	if (state->request->data.ndrcmd >= num_fns) {
		return WINBINDD_ERROR;
	}

	DEBUG(10, ("winbindd_dual_ndrcmd: Running command %s (%s)\n",
		   fns[state->request->data.ndrcmd].name,
		   domain ? domain->name : "no domain"));

	ZERO_STRUCT(p);
	p.mem_ctx = talloc_stackframe();
	p.in_data.data.buffer_size = state->request->extra_len;
	p.in_data.data.data_p = state->request->extra_data.data;
	prs_init(&p.out_data.rdata, 0, state->mem_ctx, false);

	ret = fns[state->request->data.ndrcmd].fn(&p);
	TALLOC_FREE(p.mem_ctx);
	if (!ret) {
		return WINBINDD_ERROR;
	}

	state->response->extra_data.data =
		talloc_memdup(state->mem_ctx, p.out_data.rdata.data_p,
			      p.out_data.rdata.data_offset);
	state->response->length += p.out_data.rdata.data_offset;
	prs_mem_free(&p.out_data.rdata);
	if (state->response->extra_data.data == NULL) {
		return WINBINDD_ERROR;
	}
	return WINBINDD_OK;
}

/*
 * Just a dummy to make srv_wbint.c happy
 */
NTSTATUS rpc_srv_register(int version, const char *clnt, const char *srv,
			  const struct ndr_interface_table *iface,
			  const struct api_struct *cmds, int size)
{
	return NT_STATUS_OK;
}
