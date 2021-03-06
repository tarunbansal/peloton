//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// policy.h
//
// Identification: src/include/parser/commands/policy.h
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


/*-------------------------------------------------------------------------
 *
 * policy.h
 *	  prototypes for policy.c.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/policy.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLICY_H
#define POLICY_H

#include "parser/catalog/objectaddress.h"
#include "parser/nodes/parsenodes.h"
#include "parser/utils/relcache.h"

extern void RelationBuildRowSecurity(Relation relation);

extern void RemovePolicyById(Oid policy_id);

extern ObjectAddress CreatePolicy(CreatePolicyStmt *stmt);
extern ObjectAddress AlterPolicy(AlterPolicyStmt *stmt);

extern Oid get_relation_policy_oid(Oid relid, const char *policy_name,
						bool missing_ok);

extern ObjectAddress rename_policy(RenameStmt *stmt);


#endif   /* POLICY_H */
