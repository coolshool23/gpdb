/*-------------------------------------------------------------------------
 *
 * nodeDynamicSeqscan.c
 *	  Support routines for scanning one or more relations that are
 *	  determined at run time. The relations could be Heap, AppendOnly Row,
 *	  AppendOnly Columnar.
 *
 * DynamicSeqScan node scans each relation one after the other. For each
 * relation, it opens the table, scans the tuple, and returns relevant tuples.
 *
 * Portions Copyright (c) 2012 - present, EMC/Greenplum
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 *
 *
 * IDENTIFICATION
 *	    src/backend/executor/nodeDynamicSeqscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/execnodes.h"
#include "executor/execDynamicScan.h"
#include "executor/nodeDynamicSeqscan.h"
#include "executor/nodeSeqscan.h"
#include "executor/execUtils.h"
#include "utils/hsearch.h"
#include "parser/parsetree.h"
#include "utils/memutils.h"
#include "cdb/cdbpartition.h"
#include "cdb/cdbvars.h"
#include "cdb/partitionselection.h"
#include "cdb/cdbappendonlyam.h"
#include "cdb/cdbaocsam.h"

static void CleanupOnePartition(DynamicSeqScanState *node);

DynamicSeqScanState *
ExecInitDynamicSeqScan(DynamicSeqScan *node, EState *estate, int eflags)
{
	DynamicSeqScanState *state;
	Oid			reloid;

	Assert((eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) == 0);

	state = makeNode(DynamicSeqScanState);
	state->eflags = eflags;
	state->ss.ps.plan = (Plan *) node;
	state->ss.ps.state = estate;

	state->scan_state = SCAN_INIT;

	/* Initialize child expressions. This is needed to find subplans. */
	state->ss.ps.qual = (List *)
		ExecInitExpr((Expr *) node->seqscan.plan.qual,
					 (PlanState *) state);
	state->ss.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->seqscan.plan.targetlist,
					 (PlanState *) state);

	/* Initialize result tuple type. */
	ExecInitResultTupleSlot(estate, &state->ss.ps);
	ExecAssignResultTypeFromTL(&state->ss.ps);

	reloid = getrelid(node->seqscan.scanrelid, estate->es_range_table);
	Assert(OidIsValid(reloid));


	/* lastRelOid is used to remap varattno for heterogeneous partitions */
	state->lastRelOid = reloid;

	state->scanrelid = node->seqscan.scanrelid;

	/*
	 * This context will be reset per-partition to free up per-partition
	 * qual and targetlist allocations
	 */
	state->partitionMemoryContext = AllocSetContextCreate(CurrentMemoryContext,
									 "DynamicSeqScanPerPartition",
									 ALLOCSET_DEFAULT_MINSIZE,
									 ALLOCSET_DEFAULT_INITSIZE,
									 ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Hash table is to store each new partition's scanstate to avoid
	 * memory leak. We use it when ExecDynamicSeqScan and release all
	 * when EndPlan.
	 */
	state->ss_table = create_ss_cache_for_dynamic_scan("Dynamic SeqScan",
													   estate);
	state->cached_relids = NIL;

	return state;
}

/*
 * initNextTableToScan
 *   Find the next table to scan and initiate the scan if the previous table
 * is finished.
 *
 * If scanning on the current table is not finished, or a new table is found,
 * this function returns true.
 * If no more table is found, this function returns false.
 */
static bool
initNextTableToScan(DynamicSeqScanState *node)
{
	ScanState  *scanState = (ScanState *) node;
	DynamicSeqScan *plan = (DynamicSeqScan *) scanState->ps.plan;
	EState	   *estate = scanState->ps.state;
	Relation	lastScannedRel;
	TupleDesc	partTupDesc;
	TupleDesc	lastTupDesc;
	AttrNumber *attMap;
	Oid		   *pid;
	Relation	currentRelation;
	PlanState  *ssPlanState;
	bool        found;
	ScanOidEntry *soe;

	pid = hash_seq_search(&node->pidStatus);
	if (pid == NULL)
	{
		node->shouldCallHashSeqTerm = false;
		return false;
	}

	/* Collect number of partitions scanned in EXPLAIN ANALYZE */
	if (NULL != scanState->ps.instrument)
	{
		Instrumentation *instr = scanState->ps.instrument;
		instr->numPartScanned++;
	}

	soe = (ScanOidEntry *) hash_search(node->ss_table,
									   pid,
									   HASH_ENTER, &found);

	if (!found)
	{
		currentRelation = scanState->ss_currentRelation = heap_open(*pid, AccessShareLock);
	}
	else
	{
		Relation cr = ((SeqScanState *) (soe->ss))->ss.ss_currentRelation;
		currentRelation = scanState->ss_currentRelation = cr;
	}

	lastScannedRel = heap_open(node->lastRelOid, AccessShareLock);
	lastTupDesc = RelationGetDescr(lastScannedRel);
	partTupDesc = RelationGetDescr(scanState->ss_currentRelation);
	attMap = varattnos_map(lastTupDesc, partTupDesc);
	heap_close(lastScannedRel, AccessShareLock);

	/* If attribute remapping is not necessary, then do not change the varattno */
	if (attMap)
	{
		change_varattnos_of_a_varno((Node*)scanState->ps.plan->qual, attMap, node->scanrelid);
		change_varattnos_of_a_varno((Node*)scanState->ps.plan->targetlist, attMap, node->scanrelid);

		/*
		 * Now that the varattno mapping has been changed, change the relation that
		 * the new varnos correspond to
		 */
		node->lastRelOid = *pid;
		pfree(attMap);
	}

	DynamicScan_SetTableOid(&node->ss, *pid);

	if (!found)
	{
		node->seqScanState = ExecInitSeqScanForPartition(&plan->seqscan, estate, node->eflags,
														 currentRelation);
		soe->ss = (void *) (node->seqScanState);
		node->cached_relids = lappend_oid(node->cached_relids, currentRelation->rd_id);
	}
	else
	{
		node->seqScanState = (SeqScanState *) (soe->ss);
		/* We are to scan the opened scanstate, first rescan it. */
		ExecReScan((PlanState *) (node->seqScanState));
	}

	/*
	 * Setup gpmon counters for SeqScan. Rows count for sidecar partition scan should
	 * be consistent with a parent dynamic scan as they share the same plan_node_id.
	 * Otherwise partition sends zero row number while dynamic scan sends an actual
	 * value and this is confusing.
	 */
	ssPlanState = &node->seqScanState->ss.ps;
	InitPlanNodeGpmonPkt(ssPlanState->plan, &ssPlanState->gpmon_pkt, estate);
	ssPlanState->gpmon_pkt.u.qexec.rowsout = node->ss.ps.gpmon_pkt.u.qexec.rowsout;

	return true;
}

/*
 * setPidIndex
 *   Set the pid index for the given dynamic table.
 */
static void
setPidIndex(DynamicSeqScanState *node)
{
	Assert(node->pidIndex == NULL);

	ScanState *scanState = (ScanState *)node;
	EState *estate = scanState->ps.state;
	DynamicSeqScan *plan = (DynamicSeqScan *) scanState->ps.plan;
	Assert(estate->dynamicTableScanInfo != NULL);

	/*
	 * Ensure that the dynahash exists even if the partition selector
	 * didn't choose any partition for current scan node [MPP-24169].
	 */
	InsertPidIntoDynamicTableScanInfo(estate, plan->partIndex,
									  InvalidOid, InvalidPartitionSelectorId);

	Assert(NULL != estate->dynamicTableScanInfo->pidIndexes);
	Assert(estate->dynamicTableScanInfo->numScans >= plan->partIndex);
	node->pidIndex = estate->dynamicTableScanInfo->pidIndexes[plan->partIndex - 1];
	Assert(node->pidIndex != NULL);
}

TupleTableSlot *
ExecDynamicSeqScan(DynamicSeqScanState *node)
{
	TupleTableSlot *slot = NULL;

	/*
	 * If this is called the first time, find the pid index that contains all unique
	 * partition pids for this node to scan.
	 */
	if (node->pidIndex == NULL)
	{
		setPidIndex(node);
		Assert(node->pidIndex != NULL);

		hash_seq_init(&node->pidStatus, node->pidIndex);
		node->shouldCallHashSeqTerm = true;
	}

	/*
	 * Scan the table to find next tuple to return. If the current table
	 * is finished, close it and open the next table for scan.
	 */
	for (;;)
	{
		if (!node->seqScanState)
		{
			/* No partition open. Open the next one, if any. */
			if (!initNextTableToScan(node))
				break;
		}

		slot = ExecSeqScan(node->seqScanState);

		if (!TupIsNull(slot))
		{
			/*
			 * Increment sidecar's partition scan tuples count.
			 * It should be incremented consistently with a
			 * dynamic scan to avoid gpperfmon anomalies.
			 */
			if (&node->seqScanState->ss.ps.gpmon_pkt)
				Gpmon_Incr_Rows_Out(&node->seqScanState->ss.ps.gpmon_pkt);
			break;
		}

		/* No more tuples from this partition. Move to next one. */
		CleanupOnePartition(node);
	}

	return slot;
}

/*
 * CleanupOnePartition
 *		Cleans up a partition's relation and releases all locks.
 */
static void
CleanupOnePartition(DynamicSeqScanState *scanState)
{
	Assert(NULL != scanState);
	SeqScanState *sstate = scanState->seqScanState;

	if (sstate)
	{
		if (sstate->ss_currentScanDesc_heap)
			heap_afterscan(sstate->ss_currentScanDesc_heap);
		if (sstate->ss_currentScanDesc_ao)
			appendonly_afterscan(sstate->ss_currentScanDesc_ao);
		if (sstate->ss_currentScanDesc_aocs)
			aocs_afterscan(sstate->ss_currentScanDesc_aocs);

		/*
		 * Since we use the cache hack, we cannot end the subscan
		 * just set it to NULL and this will lead to the logic
		 * of initNextTableToScan work.
		 */
		scanState->seqScanState = NULL;
	}
	if ((scanState->scan_state & SCAN_SCAN) != 0)
	{
		Assert(scanState->ss.ss_currentRelation != NULL);
		ExecCloseScanRelation(scanState->ss.ss_currentRelation);
		scanState->ss.ss_currentRelation = NULL;
	}
}

/*
 * DynamicSeqScanEndCurrentScan
 *		Cleans up any ongoing scan.
 */
static void
DynamicSeqScanEndCurrentScan(DynamicSeqScanState *node)
{
	CleanupOnePartition(node);

	if (node->shouldCallHashSeqTerm)
	{
		hash_seq_term(&node->pidStatus);
		node->shouldCallHashSeqTerm = false;
	}
}

/*
 * ExecEndDynamicSeqScan
 *		Ends the scanning of this DynamicSeqScanNode and frees
 *		up all the resources.
 */
void
ExecEndDynamicSeqScan(DynamicSeqScanState *node)
{
	DynamicSeqScanEndCurrentScan(node);

	release_ss_cache_for_dynamic_scan(node->ss_table, node->cached_relids);

	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	EndPlanStateGpmonPkt(&node->ss.ps);
}

/*
 * ExecReScanDynamicSeqScan
 *		Prepares the internal states for a rescan.
 */
void
ExecReScanDynamicSeqScan(DynamicSeqScanState *node)
{
	DynamicSeqScanEndCurrentScan(node);

	/* Force reloading the partition hash table */
	node->pidIndex = NULL;
}
