/* 
   Unix SMB/CIFS implementation.

   implement the DsAddEntry call

   Copyright (C) Stefan Metzmacher 2009
   Copyright (C) Andrew Tridgell   2009
   
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

#include "includes.h"
#include "rpc_server/dcerpc_server.h"
#include "dsdb/samdb/samdb.h"
#include "param/param.h"
#include "rpc_server/drsuapi/dcesrv_drsuapi.h"
#include "librpc/gen_ndr/ndr_drsuapi.h"


/*
  add special SPNs needed for DRS replication to machine accounts when
  an AddEntry is done to create a nTDSDSA object
 */
static WERROR drsuapi_add_SPNs(struct drsuapi_bind_state *b_state,
			       struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
			       const struct drsuapi_DsReplicaObjectListItem *first_object)
{
	int ret;
	const struct drsuapi_DsReplicaObjectListItem *obj;
	const char *attrs[] = { "serverReference", "objectGUID", NULL };

	for (obj = first_object; obj; obj=obj->next_object) {
		const char *dn_string = obj->object.identifier->dn;
		struct ldb_dn *dn = ldb_dn_new(mem_ctx, b_state->sam_ctx, dn_string);
		struct ldb_result *res;
		struct ldb_dn *ref_dn;
		struct GUID ntds_guid;
		struct ldb_message *msg;
		struct ldb_message_element *el;
		const char *ntds_guid_str;
		const char *dom_string;

		DEBUG(6,(__location__ ": Adding SPNs for %s\n", 
			 ldb_dn_get_linearized(dn)));
		 
		ret = ldb_search(b_state->sam_ctx, mem_ctx, &res,
				 dn, LDB_SCOPE_BASE, attrs,
				 "(objectClass=ntDSDSA)");
		if (ret != LDB_SUCCESS || res->count < 1) {
			DEBUG(0,(__location__ ": Failed to find dn '%s'\n", dn_string));
			return WERR_DS_DRA_INTERNAL_ERROR;
		}

		ref_dn = samdb_result_dn(b_state->sam_ctx, mem_ctx, res->msgs[0], "serverReference", NULL);
		if (ref_dn == NULL) {
			/* we only add SPNs for objects with a
			   serverReference */
			continue;
		}

		DEBUG(6,(__location__ ": serverReference %s\n", 
			 ldb_dn_get_linearized(ref_dn)));

		ntds_guid = samdb_result_guid(res->msgs[0], "objectGUID");

		ntds_guid_str = GUID_string(res, &ntds_guid);

		dom_string = lp_dnsdomain(dce_call->conn->dce_ctx->lp_ctx);

		/*
		 * construct a modify request to add the new SPNs to
		 * the machine account
		 */
		msg = ldb_msg_new(mem_ctx);
		if (msg == NULL) {
			return WERR_NOMEM;
		}

		msg->dn = ref_dn;
		ret = ldb_msg_add_empty(msg, "servicePrincipalName",
					LDB_FLAG_MOD_ADD, &el);
		if (ret != LDB_SUCCESS) {
			return WERR_NOMEM;
		}

		el->num_values = 2;
		el->values = talloc_array(msg->elements, struct ldb_val, 2);
		if (el->values == NULL) {
			return WERR_NOMEM;
		}
		/* the magic constant is the GUID of the DRSUAPI RPC
		   interface */
		el->values[0].data = (uint8_t *)talloc_asprintf(el->values, 
								"E3514235-4B06-11D1-AB04-00C04FC2DCD2/%s/%s", 
								ntds_guid_str, dom_string);
		el->values[0].length = strlen((char *)el->values[0].data);
		el->values[1].data = (uint8_t *)talloc_asprintf(el->values, "ldap/%s._msdcs.%s", 
								ntds_guid_str, dom_string);
		el->values[1].length = strlen((char *)el->values[1].data);

		ret = ldb_modify(b_state->sam_ctx, msg);
		if (ret != LDB_SUCCESS) {
			DEBUG(0,(__location__ ": Failed to add SPNs - %s\n",
				 ldb_errstring(b_state->sam_ctx)));
			return WERR_DS_DRA_INTERNAL_ERROR;
		}
	}
	
	return WERR_OK;
}




/* 
  drsuapi_DsAddEntry
*/
WERROR dcesrv_drsuapi_DsAddEntry(struct dcesrv_call_state *dce_call, TALLOC_CTX *mem_ctx,
				 struct drsuapi_DsAddEntry *r)
{
	WERROR status;
	struct drsuapi_bind_state *b_state;
	struct dcesrv_handle *h;
	uint32_t num = 0;
	struct drsuapi_DsReplicaObjectIdentifier2 *ids = NULL;
	int ret;
	const struct drsuapi_DsReplicaObjectListItem *first_object;

	if (DEBUGLVL(4)) {
		NDR_PRINT_FUNCTION_DEBUG(drsuapi_DsAddEntry, NDR_IN, r);
	}

	/* TODO: check which out level the client supports */

	ZERO_STRUCTP(r->out.ctr);
	*r->out.level_out = 3;
	r->out.ctr->ctr3.level = 1;
	r->out.ctr->ctr3.error = talloc_zero(mem_ctx, union drsuapi_DsAddEntryError);

	DCESRV_PULL_HANDLE_WERR(h, r->in.bind_handle, DRSUAPI_BIND_HANDLE);
	b_state = h->data;

	status = drs_security_level_check(dce_call, "DsAddEntry");
	if (!W_ERROR_IS_OK(status)) {
		return status;
	}

	switch (r->in.level) {
	case 2:
		ret = ldb_transaction_start(b_state->sam_ctx);
		if (ret != LDB_SUCCESS) {
			return WERR_DS_DRA_INTERNAL_ERROR;
		}


		first_object = &r->in.req->req2.first_object;

		status = dsdb_origin_objects_commit(b_state->sam_ctx,
						    mem_ctx,
						    first_object,
						    &num,
						    &ids);
		if (!W_ERROR_IS_OK(status)) {
			r->out.ctr->ctr3.error->info1.status = status;
			ldb_transaction_cancel(b_state->sam_ctx);
			DEBUG(0,(__location__ ": DsAddEntry failed - %s\n", win_errstr(status)));
			return status;
		}

		r->out.ctr->ctr3.count = num;
		r->out.ctr->ctr3.objects = ids;

		break;
	default:
		return WERR_FOOBAR;
	}

	/* if any of the added entries are nTDSDSA objects then we
	 * need to add the SPNs to the machine account
	 */
	status = drsuapi_add_SPNs(b_state, dce_call, mem_ctx, first_object);
	if (!W_ERROR_IS_OK(status)) {
		r->out.ctr->ctr3.error->info1.status = status;
		ldb_transaction_cancel(b_state->sam_ctx);
		DEBUG(0,(__location__ ": DsAddEntry add SPNs failed - %s\n", win_errstr(status)));
		return status;
	}

	ret = ldb_transaction_commit(b_state->sam_ctx);
	if (ret != LDB_SUCCESS) {
		DEBUG(0,(__location__ ": DsAddEntry commit failed\n"));
		return WERR_DS_DRA_INTERNAL_ERROR;
	}

	return WERR_OK;
}
