/**
 * @brief Implementation of bridge.
 *
 * These utilities allow us to manage Postgres metadata.
 *
 * Copyright(c) 2015, CMU
 */

#include "postgres.h"
#include "c.h"

#include "miscadmin.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "bridge/bridge.h"
#include "backend/bridge/ddl.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_database.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "common/fe_memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include <sys/types.h>
#include <unistd.h>

//===--------------------------------------------------------------------===//
// Postgres Utility Functions
//===--------------------------------------------------------------------===//

//===--------------------------------------------------------------------===//
// Getters
//===--------------------------------------------------------------------===//

/**
 * @brief Get the pg class tuple
 * @param tuple relevant tuple if it exists, NULL otherwise
 */
HeapTuple
GetPGClassTupleForRelationOid(Oid relation_id){
  Relation pg_class_rel;
  HeapTuple tuple = NULL;

  StartTransactionCommand();

  // Open pg_class table
  pg_class_rel = heap_open(RelationRelationId, AccessShareLock);

  // Search the pg_class table with given relation id
  tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relation_id));
  if (!HeapTupleIsValid(tuple)) {
    elog(DEBUG2, "cache lookup failed for relation %u", relation_id);
    // Don't break here, we need to close heap and commit.
  }

  heap_close(pg_class_rel, AccessShareLock);
  CommitTransactionCommand();

  return tuple;
}

/**
 * @brief Get the pg class tuple
 * @param tuple relevant tuple if it exists, NULL otherwise
 */
HeapTuple
GetPGClassTupleForRelationName(const char *relation_name){
  Relation pg_class_rel;
  HeapTuple tuple = NULL;
  HeapScanDesc scan;

  // Open pg_class table
  pg_class_rel = heap_open(RelationRelationId, AccessShareLock);

  // Search the pg_class table with given relation name
  scan = heap_beginscan_catalog(pg_class_rel, 0, NULL);

  while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection))) {
    Form_pg_class pgclass = (Form_pg_class) GETSTRUCT(tuple);

    if( pgclass->relnamespace==PG_PUBLIC_NAMESPACE){
      if(strcmp(NameStr(pgclass->relname), relation_name) == 0) {
        // We need to end scan and close heap
        break;
      }
    }
  }

  heap_endscan(scan);
  heap_close(pg_class_rel, AccessShareLock);

  return tuple;
}

//===--------------------------------------------------------------------===//
// Oid <--> Name
//===--------------------------------------------------------------------===//

/**
 * @brief Getting the relation name
 * @param relation_id relation id
 * @return Tuple if valid relation_id, otherwise null
 */
char*
GetRelationName(Oid relation_id){
  HeapTuple tuple;
  Form_pg_class pg_class;
  char* relation_name;

  tuple = GetPGClassTupleForRelationOid(relation_id);
  if (!HeapTupleIsValid(tuple)) {
    return NULL;
  }

  // Get relation name
  pg_class = (Form_pg_class) GETSTRUCT(tuple);
  relation_name = NameStr(pg_class->relname);

  return relation_name;
}

/*
 * given a table name, look up the OID
 * @param table_name table name
 * @return relation id, if relation is valid, 0 otherewise
 */
Oid
GetRelationOid(const char *relation_name){
  Oid relation_oid = InvalidOid;
  HeapTuple tuple;

  tuple = GetPGClassTupleForRelationName(relation_name);
  if (!HeapTupleIsValid(tuple)) {
    return InvalidOid;
  }

  // Get relation oid
  relation_oid = HeapTupleHeaderGetOid(tuple->t_data);

  return relation_oid;
}

//===--------------------------------------------------------------------===//
// Catalog information
//===--------------------------------------------------------------------===//

/**
 * @brief Getting the number of attributes.
 * @param relation_id relation id
 * @return num_atts if valid relation_id, otherwise -1
 */
int 
GetNumberOfAttributes(Oid relation_id) {
  HeapTuple tuple;
  Form_pg_class pg_class;
  int num_atts = -1;

  tuple = GetPGClassTupleForRelationOid(relation_id);
  if (!HeapTupleIsValid(tuple)) {
    return num_atts;
  }

  pg_class = (Form_pg_class) GETSTRUCT(tuple);

  // Get number of attributes
  num_atts = pg_class->relnatts;

  return num_atts;
}

/**
 * @brief Getting the number of tuples.
 * @param relation_id relation id
 * @return num_tuples if valid relation_id, otherwise -1
 */
float 
GetNumberOfTuples(Oid relation_id){
  HeapTuple tuple;
  Form_pg_class pg_class;
  float num_tuples;

  tuple = GetPGClassTupleForRelationOid(relation_id);
  if (!HeapTupleIsValid(tuple)) {
    return -1;
  }

  pg_class = (Form_pg_class) GETSTRUCT(tuple);

  // Get number of tuples
  num_tuples = pg_class->reltuples;

  return num_tuples;
}

/**
 * @brief Getting the current database Oid
 * @return MyDatabaseId
 */
Oid 
GetCurrentDatabaseOid(void){
  return MyDatabaseId;
}


/**
 * @Determine whether table exists in the *current* database or not
 * @param table_name table name
 * @return true or false depending on whether table exists or not.
 */
bool RelationExists(const char* relation_name) {
  HeapTuple tuple;

  tuple = GetPGClassTupleForRelationName(relation_name);
  if (!HeapTupleIsValid(tuple)) {
    return false;
  }

  return true;
}

//===--------------------------------------------------------------------===//
// Table lists
//===--------------------------------------------------------------------===//

/**
 * @brief Print all tables in *current* database using catalog table pg_class
 */
void GetTableList(bool catalog_only) {
  Relation pg_class_rel;
  HeapScanDesc scan;
  HeapTuple tuple;

  StartTransactionCommand();

  // Scan pg class table
  pg_class_rel = heap_open(RelationRelationId, AccessShareLock);
  scan = heap_beginscan_catalog(pg_class_rel, 0, NULL);

  while (HeapTupleIsValid(tuple = heap_getnext(scan, ForwardScanDirection))) {
    Form_pg_class pgclass = (Form_pg_class) GETSTRUCT(tuple);

    // Check if we only need catalog tables or not ?
    if(catalog_only == false) {
      elog(LOG, "pgclass->relname :: %s  \n", NameStr(pgclass->relname ) );
    }
    else if(pgclass->relnamespace==PG_PUBLIC_NAMESPACE) {
      elog(LOG, "pgclass->relname :: %s  \n", NameStr(pgclass->relname ) );
    }

  }

  heap_endscan(scan);
  heap_close(pg_class_rel, AccessShareLock);

  CommitTransactionCommand();
}

/**
 * @brief Print all databases using catalog table pg_database
 */
void GetDatabaseList(void) {
  Relation pg_database_rel;
  HeapScanDesc scan;
  HeapTuple tup;

  StartTransactionCommand();

  // Scan pg database table
  pg_database_rel = heap_open(DatabaseRelationId, AccessShareLock);
  scan = heap_beginscan_catalog(pg_database_rel, 0, NULL);

  while (HeapTupleIsValid(tup = heap_getnext(scan, ForwardScanDirection)))  {
    Form_pg_database pg_database = (Form_pg_database) GETSTRUCT(tup);
    elog(LOG, "pgdatabase->datname  :: %s\n", NameStr(pg_database->datname) );
  }

  heap_endscan(scan);
  heap_close(pg_database_rel, AccessShareLock);

  CommitTransactionCommand();
}

//===--------------------------------------------------------------------===//
// Setters
//===--------------------------------------------------------------------===//

/**
 * @brief Setting the number of tuples.
 * @param relation_id relation id
 * @param num_tuples number of tuples
 */
void
SetNumberOfTuples(Oid relation_id, float num_tuples) {
  Relation pg_class_rel;
  HeapTuple tuple;
  Form_pg_class pgclass;

  StartTransactionCommand();

  // Open pg_class table in exclusive mode
  pg_class_rel = heap_open(RelationRelationId,RowExclusiveLock);

  tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relation_id));
  if (!HeapTupleIsValid(tuple)) {
    elog(DEBUG2, "cache lookup failed for relation %u", relation_id);
    return;
  }

  pgclass = (Form_pg_class) GETSTRUCT(tuple);
  pgclass->reltuples = (float4) num_tuples;
  // update tuple
  simple_heap_update(pg_class_rel, &tuple->t_data->t_ctid, tuple);

  heap_close(pg_class_rel, RowExclusiveLock);

  CommitTransactionCommand();
}


/**
 * @brief This function constructs all the user-defined tables in all databases
 * @return true or false, depending on whether we could bootstrap.
 */
bool BootstrapPeloton(void)
{
  // Relations for catalog tables
  Relation pg_class_rel;
  Relation pg_attribute_rel;
  HeapScanDesc pg_class_scan;
  HeapTuple pg_class_tuple;

  int column_itr;
  bool status;

  elog(LOG, "Initializing Peloton");

  StartTransactionCommand();

  // Open the pg_class and pg_attribute catalog tables
  pg_class_rel = heap_open(RelationRelationId, AccessShareLock);
  pg_attribute_rel = heap_open(AttributeRelationId, AccessShareLock);

  pg_class_scan = heap_beginscan_catalog(pg_class_rel, 0, NULL);

  // Go over all tuples in pg_class
  // pg_class catalogs tables and most everything else that has columns or is otherwise similar to a table.
  // This includes indexes, sequences, views, composite types, and some kinds of special relation.
  // So, each tuple can correspond to a table, index, etc.
  while(1) {
    Form_pg_class pg_class;
    char *relation_name;
    char relation_kind;
    int attnum;

    // Get next tuple from pg_class
    pg_class_tuple = heap_getnext(pg_class_scan, ForwardScanDirection);

    if(!HeapTupleIsValid(pg_class_tuple))
      break;

    pg_class = (Form_pg_class) GETSTRUCT(pg_class_tuple);
    relation_name = NameStr(pg_class->relname);
    relation_kind = pg_class->relkind;

    // Handle only user-defined structures, not pg-catalog structures
    if( pg_class->relnamespace==PG_PUBLIC_NAMESPACE)
    {

      // TODO: Currently, we only handle relations and indexes
      if(pg_class->relkind != 'r' && pg_class->relkind != 'i') {
        continue;
      }

      attnum =  pg_class->relnatts;
      if( attnum > 0 )
      {
        HeapScanDesc pg_attribute_scan;
        HeapTuple pg_attribute_tuple;
        ColumnInfo ddl_schema[attnum];

        // This will be different from pg's attnum, as we skip system columns
        int our_attnum;

        // Get the tuple oid
        // This can be a relation oid or index oid etc.
        Oid tuple_oid = HeapTupleHeaderGetOid(pg_class_tuple->t_data);

        // Scan the pg_attribute table for the relation oid we are interested in.
        pg_attribute_scan = heap_beginscan_catalog(pg_attribute_rel, 0, NULL);

        column_itr = 0;

        //===--------------------------------------------------------------------===//
        // Build the schema
        //===--------------------------------------------------------------------===//

        // Go over all entries in pg_attribute
        while (1) {
          Form_pg_attribute pg_attribute;

          // Get next <relation, attribute> tuple from pg_attribute table
          pg_attribute_tuple = heap_getnext(pg_attribute_scan, ForwardScanDirection);
          if(!HeapTupleIsValid(pg_attribute_tuple))
            break;

          // Check the relation oid
          pg_attribute = (Form_pg_attribute) GETSTRUCT(pg_attribute_tuple);
          if( pg_attribute->attrelid == tuple_oid )
          {
            // Skip system columns in the attribute list
            if( strcmp( NameStr(pg_attribute->attname),"cmax" ) &&
                strcmp( NameStr(pg_attribute->attname),"cmin" ) &&
                strcmp( NameStr(pg_attribute->attname),"ctid" ) &&
                strcmp( NameStr(pg_attribute->attname),"xmax" ) &&
                strcmp( NameStr(pg_attribute->attname),"xmin" ) &&
                strcmp( NameStr(pg_attribute->attname),"tableoid" ) )
            {
              ddl_schema[column_itr].type = pg_attribute->atttypid;
              ddl_schema[column_itr].column_offset = column_itr;
              ddl_schema[column_itr].column_length = pg_attribute->attlen;
              strcpy(ddl_schema[column_itr].name, NameStr(pg_attribute->attname));
              ddl_schema[column_itr].allow_null = ! pg_attribute->attnotnull;
              // NOTE: We set it as true later for VARCHAR.
              ddl_schema[column_itr].is_inlined = false;

              column_itr++;
            }

          }
        }

        our_attnum = column_itr;
        heap_endscan(pg_attribute_scan);

        //===--------------------------------------------------------------------===//
        // Create Peloton Structures
        //===--------------------------------------------------------------------===//

        switch(relation_kind){

          case 'r':
          {
            // Create the Peloton table
            status = DDLCreateTable(relation_name, ddl_schema, our_attnum);
            if(status == true) {
              elog(LOG, "Create Table \"%s\" in Peloton\n", relation_name);
            }
            else {
              elog(ERROR, "Create Table \"%s\" in Peloton\n", relation_name);
            }
          }
          break;

          case 'i':
          {
            // Create the Peloton index
            Relation pg_index_rel;
            HeapScanDesc pg_index_scan;
            HeapTuple pg_index_tuple;

            pg_index_rel = heap_open(IndexRelationId, AccessShareLock);
            pg_index_scan = heap_beginscan_catalog(pg_index_rel, 0, NULL);

            // Go over the pg_index catalog table looking for indexes
            // that are associated with this table
            while (1) {
              Form_pg_index pg_index;

              pg_index_tuple = heap_getnext(pg_index_scan, ForwardScanDirection);
              if(!HeapTupleIsValid(pg_index_tuple))
                break;

              pg_index = (Form_pg_index) GETSTRUCT(pg_index_tuple);

              // Search for the tuple in pg_index corresponding to our index
              if( pg_index->indexrelid == tuple_oid)
              {

                DDLCreateIndex(relation_name, get_rel_name(pg_index->indrelid),
                               0, pg_index->indisunique,
                               ddl_schema, our_attnum);

                if(status == true) {
                  elog(LOG, "Create Index \"%s\" in Peloton\n", relation_name);
                }
                else {
                  elog(ERROR, "Create Index \"%s\" in Peloton\n", relation_name);
                }
                break;
              }
            }

            heap_endscan(pg_index_scan);
            heap_close(pg_index_rel, AccessShareLock);
          }
          break;

          default:
            elog(ERROR, "Invalid pg_class entry type : %c \n", relation_kind);
            break;
        }

      }
      // TODO: Table with no attributes. Do we need to handle this ?
      else
      {
        switch(relation_kind){

          case 'r':
            // Create the Peloton table
            status = DDLCreateTable(relation_name, NULL, 0);
            if(status == true) {
              elog(LOG, "Create Table \"%s\" in Peloton\n", relation_name);
            }
            else {
              elog(ERROR, "Create Table \"%s\" in Peloton\n", relation_name);
            }
            break;

          case 'i':
            elog(ERROR, "We don't support indexes for tables with no attributes \n");
            break;

          default:
            elog(ERROR, "Invalid pg_class entry type : %c \n", relation_kind);
            break;
        }
      }

    }
  }

  heap_endscan(pg_class_scan);
  heap_close(pg_attribute_rel, AccessShareLock);
  heap_close(pg_class_rel, AccessShareLock);

  CommitTransactionCommand();

  elog(LOG, "Finished initializing Peloton");

  return true;
}



