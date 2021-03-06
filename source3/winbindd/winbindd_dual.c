/* 
   Unix SMB/CIFS implementation.

   Winbind child daemons

   Copyright (C) Andrew Tridgell 2002
   Copyright (C) Volker Lendecke 2004,2005

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
 * We fork a child per domain to be able to act non-blocking in the main
 * winbind daemon. A domain controller thousands of miles away being being
 * slow replying with a 10.000 user list should not hold up netlogon calls
 * that can be handled locally.
 */

#include "includes.h"
#include "winbindd.h"
#include "../../nsswitch/libwbclient/wbc_async.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_WINBIND

extern bool override_logfile;
extern struct winbindd_methods cache_methods;

/* Read some data from a client connection */

static NTSTATUS child_read_request(struct winbindd_cli_state *state)
{
	NTSTATUS status;

	/* Read data */

	status = read_data(state->sock, (char *)state->request,
			   sizeof(*state->request));

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(3, ("child_read_request: read_data failed: %s\n",
			  nt_errstr(status)));
		return status;
	}

	if (state->request->extra_len == 0) {
		state->request->extra_data.data = NULL;
		return NT_STATUS_OK;
	}

	DEBUG(10, ("Need to read %d extra bytes\n", (int)state->request->extra_len));

	state->request->extra_data.data =
		SMB_MALLOC_ARRAY(char, state->request->extra_len + 1);

	if (state->request->extra_data.data == NULL) {
		DEBUG(0, ("malloc failed\n"));
		return NT_STATUS_NO_MEMORY;
	}

	/* Ensure null termination */
	state->request->extra_data.data[state->request->extra_len] = '\0';

	status= read_data(state->sock, state->request->extra_data.data,
			  state->request->extra_len);

	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("Could not read extra data: %s\n",
			  nt_errstr(status)));
	}
	return status;
}

/*
 * Do winbind child async request. This is not simply wb_simple_trans. We have
 * to do the queueing ourselves because while a request is queued, the child
 * might have crashed, and we have to re-fork it in the _trigger function.
 */

struct wb_child_request_state {
	struct tevent_context *ev;
	struct winbindd_child *child;
	struct winbindd_request *request;
	struct winbindd_response *response;
};

static bool fork_domain_child(struct winbindd_child *child);

static void wb_child_request_trigger(struct tevent_req *req,
					    void *private_data);
static void wb_child_request_done(struct tevent_req *subreq);

struct tevent_req *wb_child_request_send(TALLOC_CTX *mem_ctx,
					 struct tevent_context *ev,
					 struct winbindd_child *child,
					 struct winbindd_request *request)
{
	struct tevent_req *req;
	struct wb_child_request_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct wb_child_request_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev = ev;
	state->child = child;
	state->request = request;

	if (!tevent_queue_add(child->queue, ev, req,
			      wb_child_request_trigger, NULL)) {
		tevent_req_nomem(NULL, req);
		return tevent_req_post(req, ev);
	}
	return req;
}

static void wb_child_request_trigger(struct tevent_req *req,
				     void *private_data)
{
	struct wb_child_request_state *state = tevent_req_data(
		req, struct wb_child_request_state);
	struct tevent_req *subreq;

	if ((state->child->pid == 0) && (!fork_domain_child(state->child))) {
		tevent_req_error(req, errno);
		return;
	}

	subreq = wb_simple_trans_send(state, winbind_event_context(), NULL,
				      state->child->sock, state->request);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, wb_child_request_done, req);

	if (!tevent_req_set_endtime(req, state->ev,
				    timeval_current_ofs(300, 0))) {
		tevent_req_nomem(NULL, req);
                return;
        }
}

static void wb_child_request_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_child_request_state *state = tevent_req_data(
		req, struct wb_child_request_state);
	int ret, err;

	ret = wb_simple_trans_recv(subreq, state, &state->response, &err);
	TALLOC_FREE(subreq);
	if (ret == -1) {
		tevent_req_error(req, err);
		return;
	}
	tevent_req_done(req);
}

int wb_child_request_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			  struct winbindd_response **presponse, int *err)
{
	struct wb_child_request_state *state = tevent_req_data(
		req, struct wb_child_request_state);

	if (tevent_req_is_unix_error(req, err)) {
		return -1;
	}
	*presponse = talloc_move(mem_ctx, &state->response);
	return 0;
}

struct wb_domain_request_state {
	struct tevent_context *ev;
	struct winbindd_domain *domain;
	struct winbindd_request *request;
	struct winbindd_request *init_req;
	struct winbindd_response *response;
};

static void wb_domain_request_gotdc(struct tevent_req *subreq);
static void wb_domain_request_initialized(struct tevent_req *subreq);
static void wb_domain_request_done(struct tevent_req *subreq);

struct tevent_req *wb_domain_request_send(TALLOC_CTX *mem_ctx,
					  struct tevent_context *ev,
					  struct winbindd_domain *domain,
					  struct winbindd_request *request)
{
	struct tevent_req *req, *subreq;
	struct wb_domain_request_state *state;

	req = tevent_req_create(mem_ctx, &state,
				struct wb_domain_request_state);
	if (req == NULL) {
		return NULL;
	}

	if (domain->initialized) {
		subreq = wb_child_request_send(state, ev, &domain->child,
					       request);
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, wb_domain_request_done, req);
		return req;
	}

	state->domain = domain;
	state->ev = ev;
	state->request = request;

	state->init_req = talloc_zero(state, struct winbindd_request);
	if (tevent_req_nomem(state->init_req, req)) {
		return tevent_req_post(req, ev);
	}

	if (IS_DC || domain->primary || domain->internal) {
		/* The primary domain has to find the DC name itself */
		state->init_req->cmd = WINBINDD_INIT_CONNECTION;
		fstrcpy(state->init_req->domain_name, domain->name);
		state->init_req->data.init_conn.is_primary =
			domain->primary ? true : false;
		fstrcpy(state->init_req->data.init_conn.dcname, "");

		subreq = wb_child_request_send(state, ev, &domain->child,
					       state->init_req);
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq, wb_domain_request_initialized,
					req);
		return req;
	}

	/*
	 * Ask our DC for a DC name
	 */
	domain = find_our_domain();

	/* This is *not* the primary domain, let's ask our DC about a DC
	 * name */

	state->init_req->cmd = WINBINDD_GETDCNAME;
	fstrcpy(state->init_req->domain_name, domain->name);

	subreq = wb_child_request_send(state, ev, &domain->child, request);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, wb_domain_request_gotdc, req);
	return req;
}

static void wb_domain_request_gotdc(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_domain_request_state *state = tevent_req_data(
		req, struct wb_domain_request_state);
	struct winbindd_response *response;
	int ret, err;

	ret = wb_child_request_recv(subreq, talloc_tos(), &response, &err);
	TALLOC_FREE(subreq);
	if (ret == -1) {
		tevent_req_error(req, err);
		return;
	}
	state->init_req->cmd = WINBINDD_INIT_CONNECTION;
	fstrcpy(state->init_req->domain_name, state->domain->name);
	state->init_req->data.init_conn.is_primary = False;
	fstrcpy(state->init_req->data.init_conn.dcname,
		response->data.dc_name);

	TALLOC_FREE(response);

	subreq = wb_child_request_send(state, state->ev, &state->domain->child,
				       state->init_req);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, wb_domain_request_initialized, req);
}

static void wb_domain_request_initialized(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_domain_request_state *state = tevent_req_data(
		req, struct wb_domain_request_state);
	struct winbindd_response *response;
	int ret, err;

	ret = wb_child_request_recv(subreq, talloc_tos(), &response, &err);
	TALLOC_FREE(subreq);
	if (ret == -1) {
		tevent_req_error(req, err);
		return;
	}

	if (!string_to_sid(&state->domain->sid,
			   response->data.domain_info.sid)) {
		DEBUG(1,("init_child_recv: Could not convert sid %s "
			"from string\n", response->data.domain_info.sid));
		tevent_req_error(req, EINVAL);
		return;
	}
	fstrcpy(state->domain->name, response->data.domain_info.name);
	fstrcpy(state->domain->alt_name, response->data.domain_info.alt_name);
	state->domain->native_mode = response->data.domain_info.native_mode;
	state->domain->active_directory =
		response->data.domain_info.active_directory;
	state->domain->initialized = true;

	TALLOC_FREE(response);

	subreq = wb_child_request_send(state, state->ev, &state->domain->child,
				       state->request);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, wb_domain_request_done, req);
}

static void wb_domain_request_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_domain_request_state *state = tevent_req_data(
		req, struct wb_domain_request_state);
	int ret, err;

	ret = wb_child_request_recv(subreq, talloc_tos(), &state->response,
				    &err);
	TALLOC_FREE(subreq);
	if (ret == -1) {
		tevent_req_error(req, err);
		return;
	}
	tevent_req_done(req);
}

int wb_domain_request_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			   struct winbindd_response **presponse, int *err)
{
	struct wb_domain_request_state *state = tevent_req_data(
		req, struct wb_domain_request_state);

	if (tevent_req_is_unix_error(req, err)) {
		return -1;
	}
	*presponse = talloc_move(mem_ctx, &state->response);
	return 0;
}

/*
 * Machinery for async requests sent to children. You set up a
 * winbindd_request, select a child to query, and issue a async_request
 * call. When the request is completed, the callback function you specified is
 * called back with the private pointer you gave to async_request.
 */

struct winbindd_async_request {
	struct winbindd_async_request *next, *prev;
	TALLOC_CTX *mem_ctx;
	struct winbindd_child *child;
	struct winbindd_response *response;
	void (*continuation)(void *private_data, bool success);
	struct timed_event *reply_timeout_event;
	pid_t child_pid; /* pid of the child we're waiting on. Used to detect
			    a restart of the child (child->pid != child_pid). */
	void *private_data;
};

static void async_request_done(struct tevent_req *req);

void async_request(TALLOC_CTX *mem_ctx, struct winbindd_child *child,
		   struct winbindd_request *request,
		   struct winbindd_response *response,
		   void (*continuation)(void *private_data, bool success),
		   void *private_data)
{
	struct winbindd_async_request *state;
	struct tevent_req *req;

	DEBUG(10, ("Sending request to child pid %d (domain=%s)\n",
		   (int)child->pid,
		   (child->domain != NULL) ? child->domain->name : "''"));

	state = talloc(mem_ctx, struct winbindd_async_request);
	if (state == NULL) {
		DEBUG(0, ("talloc failed\n"));
		continuation(private_data, False);
		return;
	}

	state->mem_ctx = mem_ctx;
	state->child = child;
	state->reply_timeout_event = NULL;
	state->response = response;
	state->continuation = continuation;
	state->private_data = private_data;

	request->pid = child->pid;

	req = wb_child_request_send(state, winbind_event_context(),
					   child, request);
	if (req == NULL) {
		DEBUG(0, ("wb_child_request_send failed\n"));
                continuation(private_data, false);
		return;
        }
	tevent_req_set_callback(req, async_request_done, state);
}

static void async_request_done(struct tevent_req *req)
{
	struct winbindd_async_request *state = tevent_req_callback_data(
		req, struct winbindd_async_request);
	struct winbindd_response *response;
	int ret, err;

	ret = wb_child_request_recv(req, state, &response, &err);
	TALLOC_FREE(req);
	if (ret == -1) {
		DEBUG(2, ("wb_child_request_recv failed: %s\n",
			  strerror(err)));
		state->continuation(state->private_data, false);
		return;
	}
	*state->response = *response;
	state->continuation(state->private_data, true);
}

struct domain_request_state {
	struct winbindd_domain *domain;
	struct winbindd_request *request;
	struct winbindd_response *response;
	void (*continuation)(void *private_data_data, bool success);
	void *private_data_data;
};

static void async_domain_request_done(struct tevent_req *req);

void async_domain_request(TALLOC_CTX *mem_ctx,
			  struct winbindd_domain *domain,
			  struct winbindd_request *request,
			  struct winbindd_response *response,
			  void (*continuation)(void *private_data_data, bool success),
			  void *private_data_data)
{
	struct tevent_req *subreq;
	struct domain_request_state *state;

	state = TALLOC_P(mem_ctx, struct domain_request_state);
	if (state == NULL) {
		DEBUG(0, ("talloc failed\n"));
		continuation(private_data_data, False);
		return;
	}

	state->domain = domain;
	state->request = request;
	state->response = response;
	state->continuation = continuation;
	state->private_data_data = private_data_data;

	subreq = wb_domain_request_send(state, winbind_event_context(),
					domain, request);
	if (subreq == NULL) {
		DEBUG(5, ("wb_domain_request_send failed\n"));
		continuation(private_data_data, false);
		return;
	}
	tevent_req_set_callback(subreq, async_domain_request_done, state);
}

static void async_domain_request_done(struct tevent_req *req)
{
	struct domain_request_state *state = tevent_req_callback_data(
		req, struct domain_request_state);
	struct winbindd_response *response;
	int ret, err;

	ret = wb_domain_request_recv(req, state, &response, &err);
	TALLOC_FREE(req);
	if (ret == -1) {
		DEBUG(5, ("wb_domain_request returned %s\n", strerror(err)));
		state->continuation(state->private_data_data, false);
		return;
	}
	*(state->response) = *response;
	state->continuation(state->private_data_data, true);
}

static void recvfrom_child(void *private_data_data, bool success)
{
	struct winbindd_cli_state *state =
		talloc_get_type_abort(private_data_data, struct winbindd_cli_state);
	enum winbindd_result result = state->response->result;

	/* This is an optimization: The child has written directly to the
	 * response buffer. The request itself is still in pending state,
	 * state that in the result code. */

	state->response->result = WINBINDD_PENDING;

	if ((!success) || (result != WINBINDD_OK)) {
		request_error(state);
		return;
	}

	request_ok(state);
}

void sendto_child(struct winbindd_cli_state *state,
		  struct winbindd_child *child)
{
	async_request(state->mem_ctx, child, state->request,
		      state->response, recvfrom_child, state);
}

void sendto_domain(struct winbindd_cli_state *state,
		   struct winbindd_domain *domain)
{
	async_domain_request(state->mem_ctx, domain,
			     state->request, state->response,
			     recvfrom_child, state);
}

static void child_process_request(struct winbindd_child *child,
				  struct winbindd_cli_state *state)
{
	struct winbindd_domain *domain = child->domain;
	const struct winbindd_child_dispatch_table *table = child->table;

	/* Free response data - we may be interrupted and receive another
	   command before being able to send this data off. */

	state->response->result = WINBINDD_ERROR;
	state->response->length = sizeof(struct winbindd_response);

	/* as all requests in the child are sync, we can use talloc_tos() */
	state->mem_ctx = talloc_tos();

	/* Process command */

	for (; table->name; table++) {
		if (state->request->cmd == table->struct_cmd) {
			DEBUG(10,("child_process_request: request fn %s\n",
				  table->name));
			state->response->result = table->struct_fn(domain, state);
			return;
		}
	}

	DEBUG(1 ,("child_process_request: unknown request fn number %d\n",
		  (int)state->request->cmd));
	state->response->result = WINBINDD_ERROR;
}

void setup_child(struct winbindd_domain *domain, struct winbindd_child *child,
		 const struct winbindd_child_dispatch_table *table,
		 const char *logprefix,
		 const char *logname)
{
	if (logprefix && logname) {
		if (asprintf(&child->logfilename, "%s/%s-%s",
			     get_dyn_LOGFILEBASE(), logprefix, logname) < 0) {
			smb_panic("Internal error: asprintf failed");
		}
	} else {
		smb_panic("Internal error: logprefix == NULL && "
			  "logname == NULL");
	}

	child->domain = NULL;
	child->table = table;
	child->queue = tevent_queue_create(NULL, "winbind_child");
	SMB_ASSERT(child->queue != NULL);
	child->rpccli = wbint_rpccli_create(NULL, domain, child);
	SMB_ASSERT(child->rpccli != NULL);
}

struct winbindd_child *children = NULL;

void winbind_child_died(pid_t pid)
{
	struct winbindd_child *child;

	for (child = children; child != NULL; child = child->next) {
		if (child->pid == pid) {
			break;
		}
	}

	if (child == NULL) {
		DEBUG(5, ("Already reaped child %u died\n", (unsigned int)pid));
		return;
	}

	/* This will be re-added in fork_domain_child() */

	DLIST_REMOVE(children, child);

	close(child->sock);
	child->sock = -1;
	child->pid = 0;
}

/* Ensure any negative cache entries with the netbios or realm names are removed. */

void winbindd_flush_negative_conn_cache(struct winbindd_domain *domain)
{
	flush_negative_conn_cache_for_domain(domain->name);
	if (*domain->alt_name) {
		flush_negative_conn_cache_for_domain(domain->alt_name);
	}
}

/* 
 * Parent winbindd process sets its own debug level first and then
 * sends a message to all the winbindd children to adjust their debug
 * level to that of parents.
 */

void winbind_msg_debug(struct messaging_context *msg_ctx,
 			 void *private_data,
			 uint32_t msg_type,
			 struct server_id server_id,
			 DATA_BLOB *data)
{
	struct winbindd_child *child;

	DEBUG(10,("winbind_msg_debug: got debug message.\n"));

	debug_message(msg_ctx, private_data, MSG_DEBUG, server_id, data);

	for (child = children; child != NULL; child = child->next) {

		DEBUG(10,("winbind_msg_debug: sending message to pid %u.\n",
			(unsigned int)child->pid));

		messaging_send_buf(msg_ctx, pid_to_procid(child->pid),
			   MSG_DEBUG,
			   data->data,
			   strlen((char *) data->data) + 1);
	}
}

/* Set our domains as offline and forward the offline message to our children. */

void winbind_msg_offline(struct messaging_context *msg_ctx,
			 void *private_data,
			 uint32_t msg_type,
			 struct server_id server_id,
			 DATA_BLOB *data)
{
	struct winbindd_child *child;
	struct winbindd_domain *domain;

	DEBUG(10,("winbind_msg_offline: got offline message.\n"));

	if (!lp_winbind_offline_logon()) {
		DEBUG(10,("winbind_msg_offline: rejecting offline message.\n"));
		return;
	}

	/* Set our global state as offline. */
	if (!set_global_winbindd_state_offline()) {
		DEBUG(10,("winbind_msg_offline: offline request failed.\n"));
		return;
	}

	/* Set all our domains as offline. */
	for (domain = domain_list(); domain; domain = domain->next) {
		if (domain->internal) {
			continue;
		}
		DEBUG(5,("winbind_msg_offline: marking %s offline.\n", domain->name));
		set_domain_offline(domain);
	}

	for (child = children; child != NULL; child = child->next) {
		/* Don't send message to internal childs.  We've already
		   done so above. */
		if (!child->domain || winbindd_internal_child(child)) {
			continue;
		}

		/* Or internal domains (this should not be possible....) */
		if (child->domain->internal) {
			continue;
		}

		/* Each winbindd child should only process requests for one domain - make sure
		   we only set it online / offline for that domain. */

		DEBUG(10,("winbind_msg_offline: sending message to pid %u for domain %s.\n",
			(unsigned int)child->pid, domain->name ));

		messaging_send_buf(msg_ctx, pid_to_procid(child->pid),
				   MSG_WINBIND_OFFLINE,
				   (uint8 *)child->domain->name,
				   strlen(child->domain->name)+1);
	}
}

/* Set our domains as online and forward the online message to our children. */

void winbind_msg_online(struct messaging_context *msg_ctx,
			void *private_data,
			uint32_t msg_type,
			struct server_id server_id,
			DATA_BLOB *data)
{
	struct winbindd_child *child;
	struct winbindd_domain *domain;

	DEBUG(10,("winbind_msg_online: got online message.\n"));

	if (!lp_winbind_offline_logon()) {
		DEBUG(10,("winbind_msg_online: rejecting online message.\n"));
		return;
	}

	/* Set our global state as online. */
	set_global_winbindd_state_online();

	smb_nscd_flush_user_cache();
	smb_nscd_flush_group_cache();

	/* Set all our domains as online. */
	for (domain = domain_list(); domain; domain = domain->next) {
		if (domain->internal) {
			continue;
		}
		DEBUG(5,("winbind_msg_online: requesting %s to go online.\n", domain->name));

		winbindd_flush_negative_conn_cache(domain);
		set_domain_online_request(domain);

		/* Send an online message to the idmap child when our
		   primary domain comes back online */

		if ( domain->primary ) {
			struct winbindd_child *idmap = idmap_child();

			if ( idmap->pid != 0 ) {
				messaging_send_buf(msg_ctx,
						   pid_to_procid(idmap->pid), 
						   MSG_WINBIND_ONLINE,
						   (uint8 *)domain->name,
						   strlen(domain->name)+1);
			}
		}
	}

	for (child = children; child != NULL; child = child->next) {
		/* Don't send message to internal childs. */
		if (!child->domain || winbindd_internal_child(child)) {
			continue;
		}

		/* Or internal domains (this should not be possible....) */
		if (child->domain->internal) {
			continue;
		}

		/* Each winbindd child should only process requests for one domain - make sure
		   we only set it online / offline for that domain. */

		DEBUG(10,("winbind_msg_online: sending message to pid %u for domain %s.\n",
			(unsigned int)child->pid, child->domain->name ));

		messaging_send_buf(msg_ctx, pid_to_procid(child->pid),
				   MSG_WINBIND_ONLINE,
				   (uint8 *)child->domain->name,
				   strlen(child->domain->name)+1);
	}
}

static const char *collect_onlinestatus(TALLOC_CTX *mem_ctx)
{
	struct winbindd_domain *domain;
	char *buf = NULL;

	if ((buf = talloc_asprintf(mem_ctx, "global:%s ", 
				   get_global_winbindd_state_offline() ? 
				   "Offline":"Online")) == NULL) {
		return NULL;
	}

	for (domain = domain_list(); domain; domain = domain->next) {
		if ((buf = talloc_asprintf_append_buffer(buf, "%s:%s ", 
						  domain->name, 
						  domain->online ?
						  "Online":"Offline")) == NULL) {
			return NULL;
		}
	}

	buf = talloc_asprintf_append_buffer(buf, "\n");

	DEBUG(5,("collect_onlinestatus: %s", buf));

	return buf;
}

void winbind_msg_onlinestatus(struct messaging_context *msg_ctx,
			      void *private_data,
			      uint32_t msg_type,
			      struct server_id server_id,
			      DATA_BLOB *data)
{
	TALLOC_CTX *mem_ctx;
	const char *message;
	struct server_id *sender;
	
	DEBUG(5,("winbind_msg_onlinestatus received.\n"));

	if (!data->data) {
		return;
	}

	sender = (struct server_id *)data->data;

	mem_ctx = talloc_init("winbind_msg_onlinestatus");
	if (mem_ctx == NULL) {
		return;
	}
	
	message = collect_onlinestatus(mem_ctx);
	if (message == NULL) {
		talloc_destroy(mem_ctx);
		return;
	}

	messaging_send_buf(msg_ctx, *sender, MSG_WINBIND_ONLINESTATUS, 
			   (uint8 *)message, strlen(message) + 1);

	talloc_destroy(mem_ctx);
}

void winbind_msg_dump_event_list(struct messaging_context *msg_ctx,
				 void *private_data,
				 uint32_t msg_type,
				 struct server_id server_id,
				 DATA_BLOB *data)
{
	struct winbindd_child *child;

	DEBUG(10,("winbind_msg_dump_event_list received\n"));

	dump_event_list(winbind_event_context());

	for (child = children; child != NULL; child = child->next) {

		DEBUG(10,("winbind_msg_dump_event_list: sending message to pid %u\n",
			(unsigned int)child->pid));

		messaging_send_buf(msg_ctx, pid_to_procid(child->pid),
				   MSG_DUMP_EVENT_LIST,
				   NULL, 0);
	}

}

void winbind_msg_dump_domain_list(struct messaging_context *msg_ctx,
				  void *private_data,
				  uint32_t msg_type,
				  struct server_id server_id,
				  DATA_BLOB *data)
{
	TALLOC_CTX *mem_ctx;
	const char *message = NULL;
	struct server_id *sender = NULL;
	const char *domain = NULL;
	char *s = NULL;
	NTSTATUS status;
	struct winbindd_domain *dom = NULL;

	DEBUG(5,("winbind_msg_dump_domain_list received.\n"));

	if (!data || !data->data) {
		return;
	}

	if (data->length < sizeof(struct server_id)) {
		return;
	}

	mem_ctx = talloc_init("winbind_msg_dump_domain_list");
	if (!mem_ctx) {
		return;
	}

	sender = (struct server_id *)data->data;
	if (data->length > sizeof(struct server_id)) {
		domain = (const char *)data->data+sizeof(struct server_id);
	}

	if (domain) {

		DEBUG(5,("winbind_msg_dump_domain_list for domain: %s\n",
			domain));

		message = NDR_PRINT_STRUCT_STRING(mem_ctx, winbindd_domain,
						  find_domain_from_name_noinit(domain));
		if (!message) {
			talloc_destroy(mem_ctx);
			return;
		}

		messaging_send_buf(msg_ctx, *sender,
				   MSG_WINBIND_DUMP_DOMAIN_LIST,
				   (uint8_t *)message, strlen(message) + 1);

		talloc_destroy(mem_ctx);

		return;
	}

	DEBUG(5,("winbind_msg_dump_domain_list all domains\n"));

	for (dom = domain_list(); dom; dom=dom->next) {
		message = NDR_PRINT_STRUCT_STRING(mem_ctx, winbindd_domain, dom);
		if (!message) {
			talloc_destroy(mem_ctx);
			return;
		}

		s = talloc_asprintf_append(s, "%s\n", message);
		if (!s) {
			talloc_destroy(mem_ctx);
			return;
		}
	}

	status = messaging_send_buf(msg_ctx, *sender,
				    MSG_WINBIND_DUMP_DOMAIN_LIST,
				    (uint8_t *)s, strlen(s) + 1);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("failed to send message: %s\n",
		nt_errstr(status)));
	}

	talloc_destroy(mem_ctx);
}

static void account_lockout_policy_handler(struct event_context *ctx,
					   struct timed_event *te,
					   struct timeval now,
					   void *private_data)
{
	struct winbindd_child *child =
		(struct winbindd_child *)private_data;
	TALLOC_CTX *mem_ctx = NULL;
	struct winbindd_methods *methods;
	struct samr_DomInfo12 lockout_policy;
	NTSTATUS result;

	DEBUG(10,("account_lockout_policy_handler called\n"));

	TALLOC_FREE(child->lockout_policy_event);

	if ( !winbindd_can_contact_domain( child->domain ) ) {
		DEBUG(10,("account_lockout_policy_handler: Removing myself since I "
			  "do not have an incoming trust to domain %s\n", 
			  child->domain->name));

		return;		
	}

	methods = child->domain->methods;

	mem_ctx = talloc_init("account_lockout_policy_handler ctx");
	if (!mem_ctx) {
		result = NT_STATUS_NO_MEMORY;
	} else {
		result = methods->lockout_policy(child->domain, mem_ctx, &lockout_policy);
	}
	TALLOC_FREE(mem_ctx);

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(10,("account_lockout_policy_handler: lockout_policy failed error %s\n",
			 nt_errstr(result)));
	}

	child->lockout_policy_event = event_add_timed(winbind_event_context(), NULL,
						      timeval_current_ofs(3600, 0),
						      account_lockout_policy_handler,
						      child);
}

static time_t get_machine_password_timeout(void)
{
	/* until we have gpo support use lp setting */
	return lp_machine_password_timeout();
}

static bool calculate_next_machine_pwd_change(const char *domain,
					      struct timeval *t)
{
	time_t pass_last_set_time;
	time_t timeout;
	time_t next_change;
	struct timeval tv;
	char *pw;

	pw = secrets_fetch_machine_password(domain,
					    &pass_last_set_time,
					    NULL);

	if (pw == NULL) {
		DEBUG(0,("cannot fetch own machine password ????"));
		return false;
	}

	SAFE_FREE(pw);

	timeout = get_machine_password_timeout();
	if (timeout == 0) {
		DEBUG(10,("machine password never expires\n"));
		return false;
	}

	tv.tv_sec = pass_last_set_time;
	DEBUG(10, ("password last changed %s\n",
		   timeval_string(talloc_tos(), &tv, false)));
	tv.tv_sec += timeout;
	DEBUGADD(10, ("password valid until %s\n",
		      timeval_string(talloc_tos(), &tv, false)));

	if (time(NULL) < (pass_last_set_time + timeout)) {
		next_change = pass_last_set_time + timeout;
		DEBUG(10,("machine password still valid until: %s\n",
			http_timestring(talloc_tos(), next_change)));
		*t = timeval_set(next_change, 0);

		if (lp_clustering()) {
			uint8_t randbuf;
			/*
			 * When having a cluster, we have several
			 * winbinds racing for the password change. In
			 * the machine_password_change_handler()
			 * function we check if someone else was
			 * faster when the event triggers. We add a
			 * 255-second random delay here, so that we
			 * don't run to change the password at the
			 * exact same moment.
			 */
			generate_random_buffer(&randbuf, sizeof(randbuf));
			DEBUG(10, ("adding %d seconds randomness\n",
				   (int)randbuf));
			t->tv_sec += randbuf;
		}
		return true;
	}

	DEBUG(10,("machine password expired, needs immediate change\n"));

	*t = timeval_zero();

	return true;
}

static void machine_password_change_handler(struct event_context *ctx,
					    struct timed_event *te,
					    struct timeval now,
					    void *private_data)
{
	struct winbindd_child *child =
		(struct winbindd_child *)private_data;
	struct rpc_pipe_client *netlogon_pipe = NULL;
	TALLOC_CTX *frame;
	NTSTATUS result;
	struct timeval next_change;

	DEBUG(10,("machine_password_change_handler called\n"));

	TALLOC_FREE(child->machine_password_change_event);

	if (!calculate_next_machine_pwd_change(child->domain->name,
					       &next_change)) {
		DEBUG(10, ("calculate_next_machine_pwd_change failed\n"));
		return;
	}

	DEBUG(10, ("calculate_next_machine_pwd_change returned %s\n",
		   timeval_string(talloc_tos(), &next_change, false)));

	if (!timeval_expired(&next_change)) {
		DEBUG(10, ("Someone else has already changed the pw\n"));
		goto done;
	}

	if (!winbindd_can_contact_domain(child->domain)) {
		DEBUG(10,("machine_password_change_handler: Removing myself since I "
			  "do not have an incoming trust to domain %s\n",
			  child->domain->name));
		return;
	}

	result = cm_connect_netlogon(child->domain, &netlogon_pipe);
	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(10,("machine_password_change_handler: "
			"failed to connect netlogon pipe: %s\n",
			 nt_errstr(result)));
		return;
	}

	frame = talloc_stackframe();

	result = trust_pw_find_change_and_store_it(netlogon_pipe,
						   frame,
						   child->domain->name);
	TALLOC_FREE(frame);

	DEBUG(10, ("machine_password_change_handler: "
		   "trust_pw_find_change_and_store_it returned %s\n",
		   nt_errstr(result)));

	if (NT_STATUS_EQUAL(result, NT_STATUS_ACCESS_DENIED) ) {
		DEBUG(3,("machine_password_change_handler: password set returned "
			 "ACCESS_DENIED.  Maybe the trust account "
			 "password was changed and we didn't know it. "
			 "Killing connections to domain %s\n",
			 child->domain->name));
		TALLOC_FREE(child->domain->conn.netlogon_pipe);
	}

	if (!calculate_next_machine_pwd_change(child->domain->name,
					       &next_change)) {
		DEBUG(10, ("calculate_next_machine_pwd_change failed\n"));
		return;
	}

	DEBUG(10, ("calculate_next_machine_pwd_change returned %s\n",
		   timeval_string(talloc_tos(), &next_change, false)));

	if (!NT_STATUS_IS_OK(result)) {
		struct timeval tmp;
		/*
		 * In case of failure, give the DC a minute to recover
		 */
		tmp = timeval_current_ofs(60, 0);
		next_change = timeval_max(&next_change, &tmp);
	}

done:
	child->machine_password_change_event = event_add_timed(winbind_event_context(), NULL,
							      next_change,
							      machine_password_change_handler,
							      child);
}

/* Deal with a request to go offline. */

static void child_msg_offline(struct messaging_context *msg,
			      void *private_data,
			      uint32_t msg_type,
			      struct server_id server_id,
			      DATA_BLOB *data)
{
	struct winbindd_domain *domain;
	struct winbindd_domain *primary_domain = NULL;
	const char *domainname = (const char *)data->data;

	if (data->data == NULL || data->length == 0) {
		return;
	}

	DEBUG(5,("child_msg_offline received for domain %s.\n", domainname));

	if (!lp_winbind_offline_logon()) {
		DEBUG(10,("child_msg_offline: rejecting offline message.\n"));
		return;
	}

	primary_domain = find_our_domain();

	/* Mark the requested domain offline. */

	for (domain = domain_list(); domain; domain = domain->next) {
		if (domain->internal) {
			continue;
		}
		if (strequal(domain->name, domainname)) {
			DEBUG(5,("child_msg_offline: marking %s offline.\n", domain->name));
			set_domain_offline(domain);
			/* we are in the trusted domain, set the primary domain 
			 * offline too */
			if (domain != primary_domain) {
				set_domain_offline(primary_domain);
			}
		}
	}
}

/* Deal with a request to go online. */

static void child_msg_online(struct messaging_context *msg,
			     void *private_data,
			     uint32_t msg_type,
			     struct server_id server_id,
			     DATA_BLOB *data)
{
	struct winbindd_domain *domain;
	struct winbindd_domain *primary_domain = NULL;
	const char *domainname = (const char *)data->data;

	if (data->data == NULL || data->length == 0) {
		return;
	}

	DEBUG(5,("child_msg_online received for domain %s.\n", domainname));

	if (!lp_winbind_offline_logon()) {
		DEBUG(10,("child_msg_online: rejecting online message.\n"));
		return;
	}

	primary_domain = find_our_domain();

	/* Set our global state as online. */
	set_global_winbindd_state_online();

	/* Try and mark everything online - delete any negative cache entries
	   to force a reconnect now. */

	for (domain = domain_list(); domain; domain = domain->next) {
		if (domain->internal) {
			continue;
		}
		if (strequal(domain->name, domainname)) {
			DEBUG(5,("child_msg_online: requesting %s to go online.\n", domain->name));
			winbindd_flush_negative_conn_cache(domain);
			set_domain_online_request(domain);

			/* we can be in trusted domain, which will contact primary domain
			 * we have to bring primary domain online in trusted domain process
			 * see, winbindd_dual_pam_auth() --> winbindd_dual_pam_auth_samlogon()
			 * --> contact_domain = find_our_domain()
			 * */
			if (domain != primary_domain) {
				winbindd_flush_negative_conn_cache(primary_domain);
				set_domain_online_request(primary_domain);
			}
		}
	}
}

static void child_msg_dump_event_list(struct messaging_context *msg,
				      void *private_data,
				      uint32_t msg_type,
				      struct server_id server_id,
				      DATA_BLOB *data)
{
	DEBUG(5,("child_msg_dump_event_list received\n"));

	dump_event_list(winbind_event_context());
}

bool winbindd_reinit_after_fork(const char *logfilename)
{
	struct winbindd_domain *domain;
	struct winbindd_child *cl;

	if (!NT_STATUS_IS_OK(reinit_after_fork(winbind_messaging_context(),
					       winbind_event_context(),
					       true))) {
		DEBUG(0,("reinit_after_fork() failed\n"));
		return false;
	}

	close_conns_after_fork();

	if (!override_logfile && logfilename) {
		lp_set_logfile(logfilename);
		reopen_logs();
	}

	if (!winbindd_setup_sig_term_handler(false))
		return false;
	if (!winbindd_setup_sig_hup_handler(override_logfile ? NULL :
					    logfilename))
		return false;

	/* Don't handle the same messages as our parent. */
	messaging_deregister(winbind_messaging_context(),
			     MSG_SMB_CONF_UPDATED, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_SHUTDOWN, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_WINBIND_OFFLINE, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_WINBIND_ONLINE, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_WINBIND_ONLINESTATUS, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_DUMP_EVENT_LIST, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_WINBIND_DUMP_DOMAIN_LIST, NULL);
	messaging_deregister(winbind_messaging_context(),
			     MSG_DEBUG, NULL);

	/* We have destroyed all events in the winbindd_event_context
	 * in reinit_after_fork(), so clean out all possible pending
	 * event pointers. */

	/* Deal with check_online_events. */

	for (domain = domain_list(); domain; domain = domain->next) {
		TALLOC_FREE(domain->check_online_event);
	}

	/* Ensure we're not handling a credential cache event inherited
	 * from our parent. */

	ccache_remove_all_after_fork();

	/* Destroy all possible events in child list. */
	for (cl = children; cl != NULL; cl = cl->next) {
		TALLOC_FREE(cl->lockout_policy_event);
		TALLOC_FREE(cl->machine_password_change_event);

		/* Children should never be able to send
		 * each other messages, all messages must
		 * go through the parent.
		 */
		cl->pid = (pid_t)0;
        }
	/*
	 * This is a little tricky, children must not
	 * send an MSG_WINBIND_ONLINE message to idmap_child().
	 * If we are in a child of our primary domain or
	 * in the process created by fork_child_dc_connect(),
	 * and the primary domain cannot go online,
	 * fork_child_dc_connection() sends MSG_WINBIND_ONLINE
	 * periodically to idmap_child().
	 *
	 * The sequence is, fork_child_dc_connect() ---> getdcs() --->
	 * get_dc_name_via_netlogon() ---> cm_connect_netlogon()
	 * ---> init_dc_connection() ---> cm_open_connection --->
	 * set_domain_online(), sends MSG_WINBIND_ONLINE to
	 * idmap_child(). Disallow children sending messages
	 * to each other, all messages must go through the parent.
	 */
	cl = idmap_child();
	cl->pid = (pid_t)0;

	return true;
}

/*
 * In a child there will be only one domain, reference that here.
 */
static struct winbindd_domain *child_domain;

struct winbindd_domain *wb_child_domain(void)
{
	return child_domain;
}

static bool fork_domain_child(struct winbindd_child *child)
{
	int fdpair[2];
	struct winbindd_cli_state state;
	struct winbindd_request request;
	struct winbindd_response response;
	struct winbindd_domain *primary_domain = NULL;

	if (child->domain) {
		DEBUG(10, ("fork_domain_child called for domain '%s'\n",
			   child->domain->name));
	} else {
		DEBUG(10, ("fork_domain_child called without domain.\n"));
	}
	child_domain = child->domain;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fdpair) != 0) {
		DEBUG(0, ("Could not open child pipe: %s\n",
			  strerror(errno)));
		return False;
	}

	ZERO_STRUCT(state);
	state.pid = sys_getpid();
	state.request = &request;
	state.response = &response;

	child->pid = sys_fork();

	if (child->pid == -1) {
		DEBUG(0, ("Could not fork: %s\n", strerror(errno)));
		return False;
	}

	if (child->pid != 0) {
		/* Parent */
		close(fdpair[0]);
		child->next = child->prev = NULL;
		DLIST_ADD(children, child);
		child->sock = fdpair[1];
		return True;
	}

	/* Child */

	DEBUG(10, ("Child process %d\n", (int)sys_getpid()));

	/* Stop zombies in children */
	CatchChild();

	state.sock = fdpair[0];
	close(fdpair[1]);

	if (!winbindd_reinit_after_fork(child->logfilename)) {
		_exit(0);
	}

	/* Handle online/offline messages. */
	messaging_register(winbind_messaging_context(), NULL,
			   MSG_WINBIND_OFFLINE, child_msg_offline);
	messaging_register(winbind_messaging_context(), NULL,
			   MSG_WINBIND_ONLINE, child_msg_online);
	messaging_register(winbind_messaging_context(), NULL,
			   MSG_DUMP_EVENT_LIST, child_msg_dump_event_list);
	messaging_register(winbind_messaging_context(), NULL,
			   MSG_DEBUG, debug_message);

	primary_domain = find_our_domain();

	if (primary_domain == NULL) {
		smb_panic("no primary domain found");
	}

	/* It doesn't matter if we allow cache login,
	 * try to bring domain online after fork. */
	if ( child->domain ) {
		child->domain->startup = True;
		child->domain->startup_time = time(NULL);
		/* we can be in primary domain or in trusted domain
		 * If we are in trusted domain, set the primary domain
		 * in start-up mode */
		if (!(child->domain->internal)) {
			set_domain_online_request(child->domain);
			if (!(child->domain->primary)) {
				primary_domain->startup = True;
				primary_domain->startup_time = time(NULL);
				set_domain_online_request(primary_domain);
			}
		}
	}

	/*
	 * We are in idmap child, make sure that we set the
	 * check_online_event to bring primary domain online.
	 */
	if (child == idmap_child()) {
		set_domain_online_request(primary_domain);
	}

	/* We might be in the idmap child...*/
	if (child->domain && !(child->domain->internal) &&
	    lp_winbind_offline_logon()) {

		set_domain_online_request(child->domain);

		if (primary_domain && (primary_domain != child->domain)) {
			/* We need to talk to the primary
			 * domain as well as the trusted
			 * domain inside a trusted domain
			 * child.
			 * See the code in :
			 * set_dc_type_and_flags_trustinfo()
			 * for details.
			 */
			set_domain_online_request(primary_domain);
		}

		child->lockout_policy_event = event_add_timed(
			winbind_event_context(), NULL, timeval_zero(),
			account_lockout_policy_handler,
			child);
	}

	if (child->domain && child->domain->primary &&
	    !USE_KERBEROS_KEYTAB &&
	    lp_server_role() == ROLE_DOMAIN_MEMBER) {

		struct timeval next_change;

		if (calculate_next_machine_pwd_change(child->domain->name,
						       &next_change)) {
			child->machine_password_change_event = event_add_timed(
				winbind_event_context(), NULL, next_change,
				machine_password_change_handler,
				child);
		}
	}

	while (1) {

		int ret;
		fd_set r_fds;
		fd_set w_fds;
		int maxfd;
		struct timeval t;
		struct timeval *tp;
		struct timeval now;
		TALLOC_CTX *frame = talloc_stackframe();
		struct iovec iov[2];
		int iov_count;
		NTSTATUS status;

		if (run_events(winbind_event_context(), 0, NULL, NULL)) {
			TALLOC_FREE(frame);
			continue;
		}

		GetTimeOfDay(&now);

		if (child->domain && child->domain->startup &&
				(now.tv_sec > child->domain->startup_time + 30)) {
			/* No longer in "startup" mode. */
			DEBUG(10,("fork_domain_child: domain %s no longer in 'startup' mode.\n",
				child->domain->name ));
			child->domain->startup = False;
		}

		FD_ZERO(&r_fds);
		FD_ZERO(&w_fds);
		FD_SET(state.sock, &r_fds);
		maxfd = state.sock;

		event_add_to_select_args(winbind_event_context(), &now,
					 &r_fds, &w_fds, &t, &maxfd);
		tp = get_timed_events_timeout(winbind_event_context(), &t);
		if (tp) {
			DEBUG(11,("select will use timeout of %u.%u seconds\n",
				(unsigned int)tp->tv_sec, (unsigned int)tp->tv_usec ));
		}

		ret = sys_select(maxfd + 1, &r_fds, &w_fds, NULL, tp);

		if (run_events(winbind_event_context(), ret, &r_fds, &w_fds)) {
			/* We got a signal - continue. */
			TALLOC_FREE(frame);
			continue;
		}

		if (ret == 0) {
			DEBUG(11,("nothing is ready yet, continue\n"));
			TALLOC_FREE(frame);
			continue;
		}

		if (ret == -1 && errno == EINTR) {
			/* We got a signal - continue. */
			TALLOC_FREE(frame);
			continue;
		}

		if (ret == -1 && errno != EINTR) {
			DEBUG(0,("select error occured\n"));
			TALLOC_FREE(frame);
			perror("select");
			_exit(1);
		}

		/* fetch a request from the main daemon */
		status = child_read_request(&state);

		if (!NT_STATUS_IS_OK(status)) {
			/* we lost contact with our parent */
			_exit(0);
		}

		DEBUG(4,("child daemon request %d\n", (int)state.request->cmd));

		ZERO_STRUCTP(state.response);
		state.request->null_term = '\0';
		state.mem_ctx = frame;
		child_process_request(child, &state);

		DEBUG(4, ("Finished processing child request %d\n",
			  (int)state.request->cmd));

		SAFE_FREE(state.request->extra_data.data);

		iov[0].iov_base = (void *)state.response;
		iov[0].iov_len = sizeof(struct winbindd_response);
		iov_count = 1;

		if (state.response->length > sizeof(struct winbindd_response)) {
			iov[1].iov_base =
				(void *)state.response->extra_data.data;
			iov[1].iov_len = state.response->length-iov[0].iov_len;
			iov_count = 2;
		}

		DEBUG(10, ("Writing %d bytes to parent\n",
			   (int)state.response->length));

		if (write_data_iov(state.sock, iov, iov_count) !=
		    state.response->length) {
			DEBUG(0, ("Could not write result\n"));
			exit(1);
		}
		TALLOC_FREE(frame);
	}
}
