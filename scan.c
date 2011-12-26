/*
 * scan.c
 *
 * Routines to scan column based data store with stream processing
 *
 * --
 * Copyright 2011-2012 (c) KaiGai Kohei <kaigai@kaigai.gr.jp>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the 'LICENSE' included within
 * this package.
 */
#include "postgres.h"
#include "access/relscan.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/varbit.h"
#include "pg_strom.h"

/*
 * Declarations
 */
int		pgstrom_max_async_chunks;
int		pgstrom_work_group_size;


RelationSet
pgstrom_open_relation_set(Relation base_rel,
						  LOCKMODE lockmode, bool with_index)
{
	RelationSet	relset;
	AttrNumber	i, nattrs;
	RangeVar   *range;
	char	   *base_schema;
	char		namebuf[NAMEDATALEN * 3 + 20];

	/*
	 * The base relation must be a foreign table being managed by
	 * pg_strom foreign data wrapper.
	 */
	if (RelationGetForm(base_rel)->relkind != RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a foreign table",
						RelationGetRelationName(base_rel))));
	else
	{
		ForeignTable	   *ft = GetForeignTable(RelationGetRelid(base_rel));
		ForeignServer	   *fs = GetForeignServer(ft->serverid);
		ForeignDataWrapper *fdw = GetForeignDataWrapper(fs->fdwid);

		if (GetFdwRoutine(fdw->fdwhandler) != &pgstromFdwHandlerData)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not managed by pg_strom",
							RelationGetRelationName(base_rel))));
	}

	/*
	 * Setting up RelationSet
	 */
	nattrs = RelationGetNumberOfAttributes(base_rel);
	relset = palloc0(sizeof(RelationSetData));
	relset->cs_rel = palloc0(sizeof(Relation) * nattrs);
	relset->cs_idx = palloc0(sizeof(Relation) * nattrs);
	relset->base_rel = base_rel;

	/*
	 * Open the underlying tables and corresponding indexes
	 */
	range = makeRangeVar(PGSTROM_SCHEMA_NAME, namebuf, -1);
	base_schema = get_namespace_name(RelationGetForm(base_rel)->relnamespace);

	snprintf(namebuf, sizeof(namebuf), "%s.%s.rowid",
			 base_schema, RelationGetRelationName(base_rel));
	relset->rowid_rel = relation_openrv(range, lockmode);
	if (RelationGetForm(relset->rowid_rel)->relkind != RELKIND_RELATION)
		ereport(ERROR,
                (errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a regular table",
						RelationGetRelationName(relset->rowid_rel))));
	if (with_index)
	{
		snprintf(namebuf, sizeof(namebuf), "%s.%s.idx",
				 base_schema, RelationGetRelationName(base_rel));
		relset->rowid_idx = relation_openrv(range, lockmode);
		if (RelationGetForm(relset->rowid_idx)->relkind != RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not an index",
							RelationGetRelationName(relset->rowid_idx))));
	}

	for (i = 0; i < nattrs; i++)
	{
		Form_pg_attribute attr = RelationGetDescr(base_rel)->attrs[i];

		if (attr->attisdropped)
			continue;

		snprintf(namebuf, sizeof(namebuf), "%s.%s.%s.cs",
				 base_schema,
                 RelationGetRelationName(base_rel),
                 NameStr(attr->attname));
		relset->cs_rel[i] = relation_openrv(range, lockmode);
		if (RelationGetForm(relset->cs_rel[i])->relkind != RELKIND_RELATION)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a regular table",
							RelationGetRelationName(relset->cs_rel[i]))));
		if (with_index)
		{
			snprintf(namebuf, sizeof(namebuf), "%s.%s.%s.idx",
					 base_schema,
					 RelationGetRelationName(base_rel),
					 NameStr(attr->attname));
			relset->cs_idx[i] = relation_openrv(range, lockmode);
			if (RelationGetForm(relset->cs_idx[i])->relkind != RELKIND_INDEX)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not an index",
								RelationGetRelationName(relset->cs_idx[i]))));
		}
	}

	/*
	 * Also, solve the sequence name
	 */
	snprintf(namebuf, sizeof(namebuf), "%s.%s.seq",
			 base_schema, RelationGetRelationName(base_rel));
	relset->rowid_seqid = RangeVarGetRelid(range, NoLock, false);

	return relset;
}

void
pgstrom_close_relation_set(RelationSet relset, LOCKMODE lockmode)
{
	AttrNumber	i, nattrs = RelationGetNumberOfAttributes(relset->base_rel);

	relation_close(relset->rowid_rel, lockmode);
	if (relset->rowid_idx)
		relation_close(relset->rowid_idx, lockmode);

	for (i=0; i < nattrs; i++)
	{
		if (relset->cs_rel[i])
			relation_close(relset->cs_rel[i], lockmode);
		if (relset->cs_idx[i])
			relation_close(relset->cs_idx[i], lockmode);
	}
	pfree(relset->cs_rel);
	pfree(relset->cs_idx);
	pfree(relset);
}


typedef struct {
	int			nattrs;
	int64		rowid;
	VarBit	   *rowmap;
	bits8	  **cs_nulls;
	void	  **cs_values;

	cl_mem		dgm_rowmap;			/* device global mem of rowmap */
	cl_mem	   *dgm_nulls;			/* device global mem of nulls */
	cl_mem	   *dgm_values;			/* device global mem of values */
	cl_event   *ev_copy_to_dev;		/* event to copy from host to device */
	cl_event	ev_kern_exec;		/* event to exec kernel function */
	cl_event	ev_copy_from_dev;	/* event to copy from device to host */
} PgStromChunkBuf;

typedef struct {
	RelationSet		relset;

	/* parameters come from planner */
	int				predictable;	/* is the result set predictable? */
	Bitmapset	   *required_cols;	/* columns being returned to executor */
	Bitmapset	   *clause_cols;	/* columns being copied to device */
	const char	   *device_kernel;	/* kernel part of device code */

	/* copy from EState */
	Snapshot		es_snapshot;
	MemoryContext	es_context;

	/* scan descriptors */
	HeapScanDesc	ri_scan;		/* scan on rowid map */
	IndexScanDesc  *cs_scan;		/* scan on column store */
	ArrayType	  **cs_cur_values;
	int64		   *cs_cur_rowid_min;
	int64		   *cs_cur_rowid_max;

	/* list of the chunk */
	List		   *chunk_exec_pending_list; /* chunk being pending to exec */
	List		   *chunk_exec_list;	/* chunks under kernel execution */
	List		   *chunk_ready_list;	/* chunks being ready to 2nd scan */
	ListCell	   *curr_chunk;
	int				curr_index;

	/* opencl related stuff */
	cl_program			device_program;
	cl_command_queue	device_command_queue[0];
} PgStromExecState;

static void
pgstrom_release_chunk_buffer(PgStromChunkBuf *chunk)
{
	int		i;

	pfree(chunk->rowmap);
	for (i=0; i < chunk->nattrs; i++)
	{
		if (chunk->cs_nulls[i])
			pfree(chunk->cs_nulls[i]);
		if (chunk->cs_values[i])
			pfree(chunk->cs_values[i]);
	}
	pfree(chunk->cs_nulls);
	pfree(chunk->cs_values);
	pfree(chunk);
}

static void
pgstrom_load_column_store(PgStromExecState *sestate,
						  PgStromChunkBuf *chunk, AttrNumber attnum)
{
	Form_pg_attribute	attr;
	IndexScanDesc	iscan;
	ScanKeyData		skeys[2];
	HeapTuple		tup;
	int				csidx = attnum - 1;
	bool			found = false;

	/*
	 * XXX - Because this column shall be copied to device to execute
	 * kernel function, variable length value should not be appeared
	 * in this stage.
	 */
	attr = RelationGetDescr(sestate->relset->base_rel)->attrs[csidx];
	Assert(attr->attlen > 0);

	chunk->cs_values[csidx]
		= MemoryContextAllocZero(sestate->es_context,
								 PGSTROM_CHUNK_SIZE * attr->attlen);
	/*
	 * Try to scan column store with cs_rowid betweem rowid and
	 * (rowid + PGSTROM_CHUNK_SIZE)
	 */
	ScanKeyInit(&skeys[0],
				(AttrNumber) 1,
				BTGreaterEqualStrategyNumber, F_INT8GE,
				Int64GetDatum(chunk->rowid));
	ScanKeyInit(&skeys[1],
				(AttrNumber) 1,
				BTLessStrategyNumber, F_INT8LT,
				Int64GetDatum(chunk->rowid + PGSTROM_CHUNK_SIZE));

	iscan = index_beginscan(sestate->relset->cs_rel[csidx],
							sestate->relset->cs_idx[csidx],
							sestate->es_snapshot, 2, 0);
	index_rescan(iscan, skeys, 2, NULL, 0);

	while (HeapTupleIsValid(tup = index_getnext(iscan, ForwardScanDirection)))
	{
		TupleDesc	tupdesc;
		Datum		values[2];
		bool		nulls[2];
		int64		cur_rowid;
		ArrayType  *cur_array;
		bits8	   *nullbitmap;
		int			offset;
		int			nitems;

		found = true;

		tupdesc = RelationGetDescr(sestate->relset->cs_rel[csidx]);
		heap_deform_tuple(tup, tupdesc, values, nulls);
		Assert(!nulls[0] && !nulls[1]);

		cur_rowid = Int64GetDatum(values[0]);
		cur_array = DatumGetArrayTypeP(values[1]);

		offset = cur_rowid - chunk->rowid;
		Assert(offset >= 0 && offset < PGSTROM_CHUNK_SIZE);
		Assert((offset & (BITS_PER_BYTE - 1)) == 0);
		Assert(ARR_NDIM(cur_array) == 1);
		Assert(ARR_LBOUND(cur_array)[0] == 0);
		Assert(ARR_ELEMTYPE(cur_array) == attr->atttypid);

		/*
		 * XXX - nullbitmap shall be acquired on demand. If NULL, it means
		 * this chunk has no null valus in this column-store.
		 */
		nullbitmap = ARR_NULLBITMAP(cur_array);
		if (nullbitmap && !chunk->cs_nulls[csidx])
			chunk->cs_nulls[csidx]
				= MemoryContextAllocZero(sestate->es_context,
										 PGSTROM_CHUNK_SIZE / BITS_PER_BYTE);

		nitems = ARR_DIMS(cur_array)[0];
		memcpy(((char *)chunk->cs_values[csidx]) + offset * attr->attlen,
			   ARR_DATA_PTR(cur_array),
			   nitems * attr->attlen);
		if (nullbitmap)
			memcpy(chunk->cs_nulls[csidx] + offset / BITS_PER_BYTE,
				   nullbitmap,
				   (nitems + BITS_PER_BYTE - 1) / BITS_PER_BYTE);
	}

	/*
	 * If we could not found any values between chunk->cs_rowid and
	 * (chunk->cs_rowid + PGSTROM_CHUNK_SIZE - 1), we initialize all
	 * the items as null.
	 */
	if (!found)
	{
		chunk->cs_nulls[csidx]
			= MemoryContextAlloc(sestate->es_context,
								 PGSTROM_CHUNK_SIZE / BITS_PER_BYTE);
		memset(chunk->cs_nulls[csidx], -1,
			   PGSTROM_CHUNK_SIZE / BITS_PER_BYTE);
	}
	index_endscan(iscan);
}

static int
pgstrom_load_chunk_buffer(PgStromExecState *sestate, int num_chunks)
{
	int		loaded_chunks;

	for (loaded_chunks = 0; loaded_chunks < num_chunks; loaded_chunks++)
	{
		TupleDesc	tupdesc;
		HeapTuple	tuple;
		Datum		values[2];
		bool		nulls[2];
		MemoryContext oldcxt;
		PgStromChunkBuf	*chunk;

		tuple = heap_getnext(sestate->ri_scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tuple))
			break;

		tupdesc = RelationGetDescr(sestate->relset->rowid_rel);
		heap_deform_tuple(tuple, tupdesc, values, nulls);
		Assert(!nulls[0] && !nulls[1]);

		oldcxt = MemoryContextSwitchTo(sestate->es_context);

		chunk = palloc0(sizeof(PgStromChunkBuf));
		chunk->nattrs = tupdesc->natts;
		chunk->rowid = DatumGetInt64(values[0]);
		chunk->rowmap = DatumGetVarBitPCopy(values[1]);
		chunk->cs_nulls = palloc0(sizeof(bool *) * chunk->nattrs);
		chunk->cs_values = palloc0(sizeof(void *) * chunk->nattrs);

		MemoryContextSwitchTo(oldcxt);

		if (!sestate->predictable)
		{
			Bitmapset  *temp;
			AttrNumber	attnum;

			/*
			 * Load necessary column store
			 */
			temp = bms_copy(sestate->clause_cols);
			while ((attnum = bms_first_member(temp)) > 0)
				pgstrom_load_column_store(sestate, chunk, attnum);
			bms_free(temp);

			/*
			 * Exec kernel on this chunk asynchronously
			 */
			// if bufalloc or others failed, exec_pending_list

			sestate->chunk_ready_list
				= lappend(sestate->chunk_ready_list, chunk);
		}
		else
		{
			/*
			 * In predictable query, chunks are ready to scan using rowid.
			 */
			sestate->chunk_ready_list
				= lappend(sestate->chunk_ready_list, chunk);
		}
	}
	return loaded_chunks;
}

static void
pgstrom_scan_column_store(PgStromExecState *sestate,
						  int csidx, int64 rowid,
						  TupleTableSlot *slot)
{
	ScanKeyData		skey;
	TupleDesc		tupdesc;
	HeapTuple		tuple;
	Datum			values[2];
	bool			nulls[2];
	int64			cur_rowid;
	ArrayType	   *cur_values;
	int				index;
	MemoryContext	oldcxt;
	Form_pg_attribute	attr;

	if (!sestate->cs_cur_values[csidx] ||
		rowid < sestate->cs_cur_rowid_min[csidx] ||
		rowid > sestate->cs_cur_rowid_max[csidx])
	{
		/*
		 * XXX - Just our heuristic, when the supplied rowid is located
		 * enought near range with the previous array, it will give us
		 * performance gain to just pick up next tuple according to the
		 * current index scan.
		 * Right now, we decide its threshold as a range between
		 * cs_cur_rowid_max and cs_cur_rowid_max + 2 * (cs_cur_rowid_max
		 * - cs_cur_rowid_min).
		 */
		if (sestate->cs_cur_values[csidx] &&
			rowid > sestate->cs_cur_rowid_max[csidx] &&
			rowid < (sestate->cs_cur_rowid_max[csidx] +
					 2 * (sestate->cs_cur_rowid_max[csidx] -
						  sestate->cs_cur_rowid_min[csidx])))
		{
			int		count = 2;

			while (count-- > 0)
			{
				tuple = index_getnext(sestate->cs_scan[csidx],
									  ForwardScanDirection);
				if (!HeapTupleIsValid(tuple))
					break;

				tupdesc = RelationGetDescr(sestate->relset->cs_rel[csidx]);
				heap_deform_tuple(tuple, tupdesc, values, nulls);
				Assert(!nulls[0] && !nulls[1]);

				oldcxt = MemoryContextSwitchTo(sestate->es_context);
				cur_rowid = Int64GetDatum(values[0]);
				cur_values = DatumGetArrayTypePCopy(values[1]);
				MemoryContextSwitchTo(oldcxt);

				/* Hit! */
				if (rowid >= cur_rowid &&
					rowid <= cur_rowid + ARR_DIMS(cur_values)[0])
				{
					pfree(sestate->cs_cur_values[csidx]);

					sestate->cs_cur_values[csidx] = cur_values;
					sestate->cs_cur_rowid_min[csidx] = cur_rowid;
					sestate->cs_cur_rowid_max[csidx]
						= cur_rowid + ARR_DIMS(cur_values)[0];

					goto out;
				}
#ifndef USE_FLOAT8_BYVAL
				pfree(cur_rowid);
#endif
				pfree(cur_values);
			}
		}

		/*
		 * Rewind the index scan again, the fetch tuple that contains
		 * the supplied rowid.
		 */
		if (sestate->cs_cur_values[csidx])
		{
			pfree(sestate->cs_cur_values[csidx]);
			sestate->cs_cur_values[csidx] = NULL;
			sestate->cs_cur_rowid_min[csidx] = -1;
			sestate->cs_cur_rowid_max[csidx] = -1;
		}

		ScanKeyInit(&skey,
					(AttrNumber) 1,
					BTLessEqualStrategyNumber, F_INT8LE,
					Int64GetDatum(rowid));
		index_rescan(sestate->cs_scan[csidx], &skey, 1, NULL, 0);

		tuple = index_getnext(sestate->cs_scan[csidx],
							  BackwardScanDirection);
		if (!HeapTupleIsValid(tuple))
		{
			slot->tts_isnull[csidx] = true;
			slot->tts_values[csidx] = (Datum) 0;
			return;
		}

		tupdesc = RelationGetDescr(sestate->relset->cs_rel[csidx]);
		heap_deform_tuple(tuple, tupdesc, values, nulls);
		Assert(!nulls[0] && !nulls[1]);

		oldcxt = MemoryContextSwitchTo(sestate->es_context);
		cur_rowid = Int64GetDatum(values[0]);
		cur_values = DatumGetArrayTypePCopy(values[1]);
		MemoryContextSwitchTo(oldcxt);

		sestate->cs_cur_values[csidx] = cur_values;
		sestate->cs_cur_rowid_min[csidx] = cur_rowid;
		sestate->cs_cur_rowid_max[csidx]
			= cur_rowid + ARR_DIMS(cur_values)[0];

		Assert(rowid >= sestate->cs_cur_rowid_min[csidx] &&
			   rowid <= sestate->cs_cur_rowid_max[csidx]);

		/*
		 * XXX - For the above heuristic shortcut, it resets direction
		 * and condition of index scan.
		 */
		ScanKeyInit(&skey,
					(AttrNumber) 1,
					BTGreaterStrategyNumber, F_INT8GT,
					Int64GetDatum(sestate->cs_cur_rowid_max[csidx]));
		index_rescan(sestate->cs_scan[csidx], &skey, 1, NULL, 0);
	}
out:
	attr  = slot->tts_tupleDescriptor->attrs[csidx];
	index = rowid - sestate->cs_cur_rowid_min[csidx];
	slot->tts_values[csidx] = array_ref(sestate->cs_cur_values[csidx],
										1,
										&index,
										-1,	/* varlena array */
										attr->attlen,
										attr->atttypid,
										attr->attalign,
										&slot->tts_isnull[csidx]);
}

static bool
pgstrom_scan_chunk_buffer(PgStromExecState *sestate, TupleTableSlot *slot)
{
	PgStromChunkBuf	*chunk = lfirst(sestate->curr_chunk);
	int		index;

	for (index = sestate->curr_index;
		 index < VARBITLEN(chunk->rowmap);
		 index++)
	{
		int		index_h = (index / BITS_PER_BYTE);
		int		index_l = (index & (BITS_PER_BYTE - 1));
		int		csidx;
		int64	rowid;

		if ((VARBITS(chunk->rowmap)[index_h] & (1 << index_l)) == 0)
			continue;

		rowid = chunk->rowid + index;
		for (csidx=0; csidx < chunk->nattrs; csidx++)
		{
			/*
			 * No need to back actual value of unreferenced column.
			 */
			if (!bms_is_member(csidx+1, sestate->required_cols))
			{
				slot->tts_isnull[csidx] = true;
				slot->tts_values[csidx] = (Datum) 0;
				continue;
			}

			/*
			 * No need to scan column-store again, if this column was
			 * already loaded on the previous stage. All we need to do
			 * is pick up an appropriate value from chunk buffer.
			 */
			if (chunk->cs_values[csidx])
			{
				if (!chunk->cs_nulls[csidx] ||
					(chunk->cs_nulls[csidx][index_h] & (1<<index_l)) == 0)
				{
					Form_pg_attribute	attr
						= slot->tts_tupleDescriptor->attrs[csidx];
					slot->tts_isnull[csidx] = false;
					slot->tts_values[csidx] =
						fetchatt(attr, ((char *)chunk->cs_values[csidx] +
										index * attr->attlen));
					}
				else
				{
					slot->tts_isnull[csidx] = true;
					slot->tts_values[csidx] = (Datum) 0;
				}
				continue;
			}

			/*
			 * Elsewhere, we scan the column-store with the current
			 * rowid.
			 */
			pgstrom_scan_column_store(sestate, csidx, rowid, slot);
		}
		ExecStoreVirtualTuple(slot);
		return true;
	}
	return false;	/* end of chunk, need next chunk! */
}

static PgStromExecState *
pgstrom_init_exec_state(ForeignScanState *fss)
{
	ForeignScan		   *fscan = (ForeignScan *) fss->ss.ps.plan;
	PgStromExecState   *sestate;
	ListCell		   *l;
	AttrNumber			nattrs;
	cl_int				i, ret;

	nattrs = RelationGetNumberOfAttributes(fss->ss.ss_currentRelation);
	sestate = palloc0(sizeof(PgStromExecState) +
					  sizeof(cl_command_queue) * pgstrom_num_devices);
	sestate->cs_scan = palloc0(sizeof(IndexScanDesc) * nattrs);
	sestate->cs_cur_values = palloc0(sizeof(ArrayType *) * nattrs);
	sestate->cs_cur_rowid_min = palloc0(sizeof(int64) * nattrs);
	sestate->cs_cur_rowid_max = palloc0(sizeof(int64) * nattrs);

	foreach (l, fscan->fdwplan->fdw_private)
	{
		DefElem	   *defel = (DefElem *)lfirst(l);

		if (strcmp(defel->defname, "predictable") == 0)
		{
			if (intVal(defel->arg) == TRUE)
				sestate->predictable = 1;	/* all the tuples are visible */
			else
				sestate->predictable = -1;	/* all the tuples are invisible */
		}
		else if (strcmp(defel->defname, "device_kernel") == 0)
		{
			sestate->device_kernel = strVal(defel->arg);
		}
		else if (strcmp(defel->defname, "clause_cols") == 0)
		{
			int		csidx = (intVal(defel->arg));

			Assert(csidx > 0);

			sestate->clause_cols
				= bms_add_member(sestate->clause_cols, csidx);
		}
		else if (strcmp(defel->defname, "required_cols") == 0)
		{
			int		csidx = (intVal(defel->arg));

			if (csidx < 1)
			{
				Assert(fscan->fsSystemCol);
				continue;
			}
			sestate->required_cols
				= bms_add_member(sestate->required_cols, csidx);
		}
		else
			elog(ERROR, "pg_strom: unexpected private plan information: %s",
				 defel->defname);
	}

	/*
	 * Skip stuff related to OpenCL, if the query is predictable
	 */
	if (sestate->predictable)
		goto skip_opencl;

	/*
	 * Build kernel function to binary representation
	 */
	Assert(pgstrom_device_context != NULL);
	sestate->device_program
		= clCreateProgramWithSource(pgstrom_device_context,
									1, &sestate->device_kernel,
									NULL, &ret);
	if (ret != CL_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("OpenCL failed to create program with source: %s",
						opencl_error_to_string(ret))));

	ret = clBuildProgram(sestate->device_program,
						 0, NULL,		/* for all the devices */
						 NULL,			/* no build options */
						 NULL, NULL);	/* no callback, so synchronous build */
	if (ret != CL_SUCCESS)
	{
		cl_build_status	status;
		char			logbuf[4096];

		for (i=0; i < pgstrom_num_devices; i++)
		{
			clGetProgramBuildInfo(sestate->device_program,
								  pgstrom_device_id[i],
								  CL_PROGRAM_BUILD_STATUS,
								  sizeof(status),
								  &status,
								  NULL);
			if (status != CL_BUILD_ERROR)
				continue;

			clGetProgramBuildInfo(sestate->device_program,
								  pgstrom_device_id[i],
								  CL_PROGRAM_BUILD_LOG,
								  sizeof(logbuf),
								  logbuf,
								  NULL);
			elog(NOTICE, "%s", logbuf);
		}
		clReleaseProgram(sestate->device_program);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("OpenCL failed to build program: %s",
						opencl_error_to_string(ret))));
	}

	/*
	 * Create command queues for each devices
	 */
	for (i=0; i < pgstrom_num_devices; i++)
	{
		sestate->device_command_queue[i]
			= clCreateCommandQueue(pgstrom_device_context,
								   pgstrom_device_id[i],
								   0,	/* no out-of-order, no profiling */
								   &ret);
		if (ret != CL_SUCCESS)
		{
			while (i > 0)
				clReleaseCommandQueue(sestate->device_command_queue[--i]);
			clReleaseProgram(sestate->device_program);

			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("OpenCL failed to create command queue: %s",
							opencl_error_to_string(ret))));
		}
	}

skip_opencl:
    sestate->es_snapshot = fss->ss.ps.state->es_snapshot;
    sestate->es_context = fss->ss.ps.state->es_query_cxt;

	sestate->chunk_exec_pending_list = NIL;
	sestate->chunk_exec_list = NIL;
	sestate->chunk_ready_list = NIL;
	sestate->curr_chunk = NULL;
	sestate->curr_index = 0;

	return sestate;
}

void
pgstrom_begin_foreign_scan(ForeignScanState *fss, int eflags)
{
	Relation			base_rel = fss->ss.ss_currentRelation;
	PgStromExecState   *sestate;
	Bitmapset		   *tempset;
	AttrNumber			attnum;

	/*
	 * Do nothing for EXPLAIN or ANALYZE cases
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	sestate = pgstrom_init_exec_state(fss);

	/*
	 * Begin the scan
	 */
	sestate->relset = pgstrom_open_relation_set(base_rel,
												AccessShareLock, true);
	sestate->ri_scan = heap_beginscan(sestate->relset->rowid_rel,
									  sestate->es_snapshot,
									  0, NULL);
	tempset = bms_copy(sestate->required_cols);
	while ((attnum = bms_first_member(tempset)) > 0)
	{
		/*
		 * Clause cols should be loaded prior to scan, so no need to
		 * scan it again using rowid.
		 */
		if (bms_is_member(attnum, sestate->clause_cols))
			continue;

		sestate->cs_scan[attnum - 1]
			= index_beginscan(sestate->relset->cs_rel[attnum - 1],
							  sestate->relset->cs_idx[attnum - 1],
							  sestate->es_snapshot, 2, 0);
	}
	bms_free(tempset);

	fss->fdw_state = sestate;
}

TupleTableSlot*
pgstrom_iterate_foreign_scan(ForeignScanState *fss)
{
	PgStromExecState   *sestate = (PgStromExecState *)fss->fdw_state;
	TupleTableSlot	   *slot = fss->ss.ss_ScanTupleSlot;
	int					num_chunks;

	ExecClearTuple(slot);
	if (sestate->predictable < 0)
		return slot;

	/* Is it the first call? */
	if (sestate->curr_chunk == NULL)
	{
		num_chunks = (pgstrom_max_async_chunks -
					  list_length(sestate->chunk_exec_list));
		if (pgstrom_load_chunk_buffer(sestate, num_chunks) == 0)
			return slot;
		/*
		 * XXX - pgstrom_sync_exec_kernel() here
		 */
		Assert(sestate->chunk_ready_list != NIL);
		sestate->curr_chunk = list_head(sestate->chunk_ready_list);
		sestate->curr_index = 0;
	}
retry:
	//chunk = lfirst(sestate->curr_chunk);
	if (!pgstrom_scan_chunk_buffer(sestate, slot))
	{
		/*
		 * XXX - check status of chunks in exec_list, and move them ready
		 */

		/*
		 * XXX - if exec_pending is here, try it again
		 */

		/*
		 * 
		 */
		num_chunks = (pgstrom_max_async_chunks -
					  list_length(sestate->chunk_exec_list));
		pgstrom_load_chunk_buffer(sestate, num_chunks);

		while (lnext(sestate->curr_chunk) == NULL)
		{
			/*
			 * No opportunity to read tuples any more
			 */
			if (sestate->chunk_exec_list == NIL &&
				sestate->chunk_exec_pending_list == NIL)
				return slot;

			/*
			 * XXX - pgstrom_sync_exec_kernel() here
			 */

			/*
			 * XXX - move exec chunk into ready
			 */


		}
		/*
		 * XXX - release older buffer
		 */
		sestate->curr_chunk = lnext(sestate->curr_chunk);
		sestate->curr_index = 0;
		goto retry;
	}
	return slot;
}

void
pgboost_rescan_foreign_scan(ForeignScanState *fss)
{
	PgStromExecState   *sestate = (PgStromExecState *)fss->fdw_state;

	/*
	 * XXX - To be implemented
	 */
}

void
pgboost_end_foreign_scan(ForeignScanState *fss)
{
	PgStromExecState   *sestate = (PgStromExecState *)fss->fdw_state;
	PgStromChunkBuf	   *chunk;
	ListCell		   *cell;
	int					nattrs, i;

	/* if sestate is NULL, we are in EXPLAIN; nothing to do */
	if (!sestate)
		return;

	/*
	 * End the rowid scan
	 */
	nattrs = RelationGetNumberOfAttributes(fss->ss.ss_currentRelation);
	for (i=0; i < nattrs; i++)
	{
		if (sestate->cs_scan[i])
			index_endscan(sestate->cs_scan[i]);
	}
	heap_endscan(sestate->ri_scan);

#if 0 // to be implemented
	foreach (cell, sestate->chunk_list)
	{
		chunk = (PgStromChunkBuf *) lfirst(cell);

		pgstrom_release_chunk_buffer(chunk);
	}
#endif
	pgstrom_close_relation_set(sestate->relset, AccessShareLock);

	/*
	 * Release device program and command queue
	 */
	if (sestate->device_program)
	{
		int		i;

		for (i=0; i < pgstrom_num_devices; i++)
			clReleaseCommandQueue(sestate->device_command_queue[i]);
		clReleaseProgram(sestate->device_program);
	}
}
