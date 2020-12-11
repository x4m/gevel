#include "postgres.h"

#include "access/genam.h"
#include "access/gin.h"
#if PG_VERSION_NUM >= 90400
#include "utils/snapmgr.h"
#endif
#if PG_VERSION_NUM >= 90100
#include "access/gin_private.h"
#endif
#include "access/gist.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#if PG_VERSION_NUM >= 90200
#include "access/spgist_private.h"
#include "access/spgist.h"
#include "utils/datum.h"
#endif
#include "access/heapam.h"
#include "catalog/index.h"
#if PG_VERSION_NUM >= 90600
#include <catalog/pg_am.h>
#endif
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "catalog/namespace.h"
#if PG_VERSION_NUM >= 80300
#include <tsearch/ts_utils.h>
#endif
#if PG_VERSION_NUM >= 100000
#include <utils/regproc.h>
#include <utils/varlena.h>
#endif
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include <fmgr.h>
#include <funcapi.h>
#include <access/heapam.h>
#include <catalog/pg_type.h>
#include <access/relscan.h>
#if PG_VERSION_NUM >= 120000
#include <access/nbtree.h>
#include <access/brin.h>
#include <access/brin_revmap.h>
#include <access/brin_page.h>
#include <access/brin_tuple.h>
#endif


#define PAGESIZE	(BLCKSZ - MAXALIGN(sizeof(PageHeaderData) + sizeof(ItemIdData)))

#ifndef PG_NARGS
#define PG_NARGS() (fcinfo->nargs)
#endif

#if PG_VERSION_NUM >= 90600
#define	ISNULL		true
#define ISNOTNULL	false
#define heap_formtuple	heap_form_tuple
#else
#define ISNULL		'n'
#define ISNOTNULL	' '
#endif

static char
*t2c(text* in) {
	char *out=palloc(VARSIZE(in));

	memcpy(out, VARDATA(in), VARSIZE(in)-VARHDRSZ);
	out[ VARSIZE(in)-VARHDRSZ ] ='\0';
	return out;
}

typedef struct {
	int maxlevel;
	text	*txt;
	char	*ptr;
	int		len;
} IdxInfo;

static Relation checkOpenedRelation(Relation r, Oid PgAmOid);

#ifdef PG_MODULE_MAGIC
/* >= 8.2 */

PG_MODULE_MAGIC;

static Relation
gist_index_open(RangeVar *relvar) {
#if PG_VERSION_NUM < 90200
	Oid relOid = RangeVarGetRelid(relvar, false);
#else
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
#endif
	return checkOpenedRelation(
				index_open(relOid, AccessExclusiveLock), GIST_AM_OID);
}

#define	gist_index_close(r)	index_close((r), AccessExclusiveLock)

static Relation
gin_index_open(RangeVar *relvar) {
#if PG_VERSION_NUM < 90200
	Oid relOid = RangeVarGetRelid(relvar, false);
#else
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
#endif
	return checkOpenedRelation(
				index_open(relOid, AccessShareLock), GIN_AM_OID);
}

#define gin_index_close(r) index_close((r), AccessShareLock)

#if PG_VERSION_NUM >= 120000
static Relation
btree_index_open(RangeVar *relvar) {
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
	return checkOpenedRelation(
				index_open(relOid, AccessExclusiveLock), BTREE_AM_OID);
}

#define	btree_index_close(r)	index_close((r), AccessExclusiveLock)

static Relation
brin_index_open(RangeVar *relvar)
{
	Oid relOid = RangeVarGetRelid(relvar, NoLock, false);
	return checkOpenedRelation(
				index_open(relOid, AccessExclusiveLock), BRIN_AM_OID);
}

#define	brin_index_close(r)	index_close((r), AccessExclusiveLock)
#endif

#else /* <8.2 */

static Relation
gist_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessExclusiveLock);
	return checkOpenedRelation(rel, GIST_AM_OID);
}

static void
gist_index_close(Relation rel) {
	UnlockRelation(rel, AccessExclusiveLock);
	index_close(rel);
}

static Relation
gin_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessShareLock);
	return checkOpenedRelation(rel, GIN_AM_OID);
}

static void
gin_index_close(Relation rel) {
	UnlockRelation(rel, AccessShareLock);
	index_close(rel);
}

static Relation
btree_index_open(RangeVar *relvar) {
	Relation rel = index_openrv(relvar);

	LockRelation(rel, AccessExclusiveLock);
	return checkOpenedRelation(rel, BTREE_AM_OID);
}

static void
btree_index_close(Relation rel) {
	UnlockRelation(rel, AccessExclusiveLock);
	index_close(rel);
}

#endif

#if PG_VERSION_NUM >= 80300
#define stringToQualifiedNameList(x,y)	stringToQualifiedNameList(x)
#endif

#if PG_VERSION_NUM < 80300
#define SET_VARSIZE(p,l)	VARATT_SIZEP(p)=(l)
#endif

static Relation
checkOpenedRelation(Relation r, Oid PgAmOid) {
	if ( r->rd_index == NULL )
		elog(ERROR, "Relation %s.%s is not an index",
					get_namespace_name(RelationGetNamespace(r)),
					RelationGetRelationName(r)
			);

	if ( r->rd_rel->relam != PgAmOid )
		elog(ERROR, "Index %s.%s has wrong type",
					get_namespace_name(RelationGetNamespace(r)),
					RelationGetRelationName(r)
			);

	return r;
}

static void
gist_dumptree(Relation r, int level, BlockNumber blk, OffsetNumber coff, IdxInfo *info) {
	Buffer		buffer;
	Page		page;
	IndexTuple	which;
	ItemId		iid;
	OffsetNumber i,
				maxoff;
	BlockNumber cblk;
	char	   *pred;

	pred = (char *) palloc(sizeof(char) * level * 4 + 1);
	MemSet(pred, ' ', level*4);
	pred[level*4] = '\0';

	buffer = ReadBuffer(r, blk);
	page = (Page) BufferGetPage(buffer);

	maxoff = PageGetMaxOffsetNumber(page);


	while ( (info->ptr-((char*)info->txt)) + level*4 + 128 >= info->len ) {
		int dist=info->ptr-((char*)info->txt);
		info->len *= 2;
		info->txt=(text*)repalloc(info->txt, info->len);
		info->ptr = ((char*)info->txt)+dist;
	}

	sprintf(info->ptr, "%s%d(l:%d) blk: %u numTuple: %d free: %db(%.2f%%) rightlink:%u (%s)\n",
		pred,
		coff,
		level,
		blk,
		(int) maxoff,
		(int) PageGetFreeSpace(page),
		100.0*(((float)PAGESIZE)-(float)PageGetFreeSpace(page))/((float)PAGESIZE),
		GistPageGetOpaque(page)->rightlink,
		( GistPageGetOpaque(page)->rightlink == InvalidBlockNumber ) ? "InvalidBlockNumber" : "OK" );
	info->ptr=strchr(info->ptr,'\0');

	if (!GistPageIsLeaf(page) && ( info->maxlevel<0 || level<info->maxlevel ) )
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
			iid = PageGetItemId(page, i);
			which = (IndexTuple) PageGetItem(page, iid);
			cblk = ItemPointerGetBlockNumber(&(which->t_tid));
			gist_dumptree(r, level + 1, cblk, i, info);
		}
	ReleaseBuffer(buffer);
	pfree(pred);
}

PG_FUNCTION_INFO_V1(gist_tree);
Datum	gist_tree(PG_FUNCTION_ARGS);
Datum
gist_tree(PG_FUNCTION_ARGS) {
	text	*name=PG_GETARG_TEXT_P(0);
	char *relname=t2c(name);
	RangeVar   *relvar;
	Relation		index;
	List	   *relname_list;
	IdxInfo	info;

	relname_list = stringToQualifiedNameList(relname, "gist_tree");
	relvar = makeRangeVarFromNameList(relname_list);
	index = gist_index_open(relvar);
	PG_FREE_IF_COPY(name,0);

	info.maxlevel = ( PG_NARGS() > 1 ) ? PG_GETARG_INT32(1) : -1;
	info.len=1024;
	info.txt=(text*)palloc( info.len );
	info.ptr=((char*)info.txt)+VARHDRSZ;

	gist_dumptree(index, 0, GIST_ROOT_BLKNO, 0, &info);

	gist_index_close(index);
	pfree(relname);

	SET_VARSIZE(info.txt, info.ptr-((char*)info.txt));
	PG_RETURN_POINTER(info.txt);
}

typedef struct {
	int		level;
	int		numpages;
	int		numleafpages;
	int		numtuple;
	int		numinvalidtuple;
	int		numleaftuple;
	uint64	tuplesize;
	uint64	leaftuplesize;
	uint64	totalsize;
} IdxStat;

static void
gist_stattree(Relation r, int level, BlockNumber blk, OffsetNumber coff, IdxStat *info) {
	Buffer		buffer;
	Page		page;
	IndexTuple	which;
	ItemId		iid;
	OffsetNumber i,
				maxoff;
	BlockNumber cblk;
	char	   *pred;

	pred = (char *) palloc(sizeof(char) * level * 4 + 1);
	MemSet(pred, ' ', level*4);
	pred[level*4] = '\0';

	buffer = ReadBuffer(r, blk);
	page = (Page) BufferGetPage(buffer);

	maxoff = PageGetMaxOffsetNumber(page);

	info->numpages++;
	info->tuplesize+=PAGESIZE-PageGetFreeSpace(page);
	info->totalsize+=BLCKSZ;
	info->numtuple+=maxoff;
	if ( info->level < level )
		info->level = level;

	if (GistPageIsLeaf(page)) {
		info->numleafpages++;
		info->leaftuplesize+=PAGESIZE-PageGetFreeSpace(page);
		info->numleaftuple+=maxoff;
	} else {
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i)) {
			iid = PageGetItemId(page, i);
			which = (IndexTuple) PageGetItem(page, iid);
			if ( GistTupleIsInvalid(which) )
				info->numinvalidtuple++;
			cblk = ItemPointerGetBlockNumber(&(which->t_tid));
				gist_stattree(r, level + 1, cblk, i, info);
		}
	}

	ReleaseBuffer(buffer);
	pfree(pred);
}

PG_FUNCTION_INFO_V1(gist_stat);
Datum	gist_stat(PG_FUNCTION_ARGS);
Datum
gist_stat(PG_FUNCTION_ARGS) {
	text	*name=PG_GETARG_TEXT_P(0);
	char *relname=t2c(name);
	RangeVar   *relvar;
	Relation		index;
	List	   *relname_list;
	IdxStat	info;
	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;


	relname_list = stringToQualifiedNameList(relname, "gist_tree");
	relvar = makeRangeVarFromNameList(relname_list);
	index = gist_index_open(relvar);
	PG_FREE_IF_COPY(name,0);

	memset(&info, 0, sizeof(IdxStat));

	gist_stattree(index, 0, GIST_ROOT_BLKNO, 0, &info);

	gist_index_close(index);
	pfree(relname);

	sprintf(ptr,
		"Number of levels:		  %d\n"
		"Number of pages:		   %d\n"
		"Number of leaf pages:	  %d\n"
		"Number of tuples:		  %d\n"
		"Number of invalid tuples:  %d\n"
		"Number of leaf tuples:	 %d\n"
		"Total size of tuples:	  "INT64_FORMAT" bytes\n"
		"Total size of leaf tuples: "INT64_FORMAT" bytes\n"
		"Total size of index:	   "INT64_FORMAT" bytes\n",
		info.level+1,
		info.numpages,
		info.numleafpages,
		info.numtuple,
		info.numinvalidtuple,
		info.numleaftuple,
		info.tuplesize,
		info.leaftuplesize,
		info.totalsize);

	ptr=strchr(ptr,'\0');

	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

typedef struct GPItem {
	Buffer	buffer;
	Page	page;
	OffsetNumber	offset;
	int	level;
	struct GPItem *next;
} GPItem;

typedef struct {
	List	*relname_list;
	RangeVar   *relvar;
	Relation		index;
	Datum	*dvalues;
#if PG_VERSION_NUM >= 90600
	bool	*nulls;
#else
	char	*nulls;
#endif
	GPItem	*item;
} TypeStorage;

static GPItem*
openGPPage( FuncCallContext *funcctx, BlockNumber blk ) {
	GPItem	*nitem;
	MemoryContext	 oldcontext;
	Relation index = ( (TypeStorage*)(funcctx->user_fctx) )->index;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	nitem = (GPItem*)palloc( sizeof(GPItem) );
	memset(nitem,0,sizeof(GPItem));

	nitem->buffer = ReadBuffer(index, blk);
	nitem->page = (Page) BufferGetPage(nitem->buffer);
	nitem->offset=FirstOffsetNumber;
	nitem->next = ( (TypeStorage*)(funcctx->user_fctx) )->item;
	nitem->level = ( nitem->next ) ? nitem->next->level+1 : 1;
	( (TypeStorage*)(funcctx->user_fctx) )->item = nitem;

	MemoryContextSwitchTo(oldcontext);
	return nitem;
}

static GPItem*
closeGPPage( FuncCallContext *funcctx ) {
	GPItem  *oitem = ( (TypeStorage*)(funcctx->user_fctx) )->item;

	( (TypeStorage*)(funcctx->user_fctx) )->item = oitem->next;

	ReleaseBuffer(oitem->buffer);
	pfree( oitem );
	return ( (TypeStorage*)(funcctx->user_fctx) )->item;
}

#if PG_VERSION_NUM >= 110000
#define TS_GET_TYPEVAL(s, i, v)	(s)->index->rd_att->attrs[(i)].v
#else
#define TS_GET_TYPEVAL(s, i, v)	(s)->index->rd_att->attrs[(i)]->v
#endif

static void
setup_firstcall(FuncCallContext  *funcctx, text *name) {
	MemoryContext	 oldcontext;
	TypeStorage	 *st;
	char *relname=t2c(name);
	TupleDesc			tupdesc;
	char			attname[NAMEDATALEN];
	int i;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(TypeStorage*)palloc( sizeof(TypeStorage) );
	memset(st,0,sizeof(TypeStorage));
	st->relname_list = stringToQualifiedNameList(relname, "gist_tree");
	st->relvar = makeRangeVarFromNameList(st->relname_list);
	st->index = gist_index_open(st->relvar);
	funcctx->user_fctx = (void*)st;

#if PG_VERSION_NUM >= 120000
	tupdesc = CreateTemplateTupleDesc(st->index->rd_att->natts+2);
#else
	tupdesc = CreateTemplateTupleDesc(3 /* types */ + 1 /* level */ + 1 /* nlabel */ +  2 /* tids */ + 1, false);
#endif
	TupleDescInitEntry(tupdesc, 1, "level", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "valid", BOOLOID, -1, 0);
	for (i = 0; i < st->index->rd_att->natts; i++) {
		sprintf(attname, "z%d", i+2);
		TupleDescInitEntry(
			tupdesc,
			i+3,
			attname,
			TS_GET_TYPEVAL(st, i, atttypid),
			TS_GET_TYPEVAL(st, i, atttypmod),
			TS_GET_TYPEVAL(st, i, attndims)
		);
	}

	st->dvalues = (Datum *) palloc((tupdesc->natts+2) * sizeof(Datum));
	st->nulls = palloc((tupdesc->natts+2) * sizeof(*st->nulls));

#if PG_VERSION_NUM >= 120000
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#else
	funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

	MemoryContextSwitchTo(oldcontext);
	pfree(relname);

	st->item=openGPPage(funcctx, GIST_ROOT_BLKNO);
}

static void
close_call( FuncCallContext  *funcctx ) {
	TypeStorage *st = (TypeStorage*)(funcctx->user_fctx);

	while(st->item && closeGPPage(funcctx));

	pfree(st->dvalues);
	pfree(st->nulls);

	gist_index_close(st->index);
}

PG_FUNCTION_INFO_V1(gist_print);
Datum	gist_print(PG_FUNCTION_ARGS);
Datum
gist_print(PG_FUNCTION_ARGS) {
	FuncCallContext  *funcctx;
	TypeStorage	 *st;
	Datum result=(Datum)0;
	ItemId		  iid;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	int i;
	bool isnull;

	if (SRF_IS_FIRSTCALL()) {
		text	*name=PG_GETARG_TEXT_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		setup_firstcall(funcctx, name);
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (TypeStorage*)(funcctx->user_fctx);

	if ( !st->item ) {
		close_call(funcctx);
		SRF_RETURN_DONE(funcctx);
	}

	while( st->item->offset > PageGetMaxOffsetNumber( st->item->page ) ) {
		if ( ! closeGPPage(funcctx) ) {
			close_call(funcctx);
			SRF_RETURN_DONE(funcctx);
		}
	}

	iid = PageGetItemId( st->item->page, st->item->offset );
	ituple = (IndexTuple) PageGetItem(st->item->page, iid);

	st->dvalues[0] = Int32GetDatum( st->item->level );
	st->nulls[0] = ISNOTNULL;
	st->dvalues[1] = BoolGetDatum( (!GistPageIsLeaf(st->item->page) && GistTupleIsInvalid(ituple)) ? false : true );
	st->nulls[1] = ISNOTNULL;
	for(i=2; i<funcctx->attinmeta->tupdesc->natts; i++)
	{
		if ( !GistPageIsLeaf(st->item->page) && GistTupleIsInvalid(ituple) )
		{
			st->dvalues[i] = (Datum)0;
			st->nulls[i] = ISNULL;
		} else {
			st->dvalues[i] = index_getattr(ituple, i-1, st->index->rd_att, &isnull);
			st->nulls[i] = ( isnull ) ? ISNULL : ISNOTNULL;
		}
	}

	htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
#if PG_VERSION_NUM >= 120000
	result = HeapTupleGetDatum(htuple);
#else
	result = TupleGetDatum(funcctx->slot, htuple);
#endif
	st->item->offset = OffsetNumberNext(st->item->offset);
	if ( !GistPageIsLeaf(st->item->page) )
		openGPPage(funcctx, ItemPointerGetBlockNumber(&(ituple->t_tid)) );

	SRF_RETURN_NEXT(funcctx, result);
}

typedef struct GinStatState {
	Relation		index;
	GinState		ginstate;
	OffsetNumber	attnum;

	Buffer			buffer;
	OffsetNumber	offset;
	Datum			curval;
#if PG_VERSION_NUM >= 90100
	GinNullCategory	category;
#endif
	Datum			dvalues[2];
#if PG_VERSION_NUM >= 110000
	bool			nulls[2];
#else
	char			nulls[2];
#endif
} GinStatState;

static bool
moveRightIfItNeeded( GinStatState *st )
{
	Page page = BufferGetPage(st->buffer);

	if ( st->offset > PageGetMaxOffsetNumber(page) ) {
		/*
		* We scaned the whole page, so we should take right page
		*/
		BlockNumber blkno = GinPageGetOpaque(page)->rightlink;

		if ( GinPageRightMost(page) )
			return false;  /* no more page */

		LockBuffer(st->buffer, GIN_UNLOCK);
		st->buffer = ReleaseAndReadBuffer(st->buffer, st->index, blkno);
		LockBuffer(st->buffer, GIN_SHARE);
		st->offset = FirstOffsetNumber;
	}

	return true;
}

/*
 * Refinds a previois position, at returns it has correctly
 * set offset and buffer is locked
 */
static bool
refindPosition(GinStatState *st)
{
	Page	page;

	/* find left if needed (it causes only for first search) */
	for (;;) {
		IndexTuple  itup;
		BlockNumber blkno;

		LockBuffer(st->buffer, GIN_SHARE);

		page = BufferGetPage(st->buffer);
		if (GinPageIsLeaf(page))
			break;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = GinItemPointerGetBlockNumber(&(itup)->t_tid);

		LockBuffer(st->buffer,GIN_UNLOCK);
		st->buffer = ReleaseAndReadBuffer(st->buffer, st->index, blkno);
	}

	if (st->offset == InvalidOffsetNumber) {
		return (PageGetMaxOffsetNumber(page) >= FirstOffsetNumber ) ? true : false; /* first one */
	}

	for(;;) {
		int				cmp;
#if PG_VERSION_NUM >= 90100
		GinNullCategory	category;
#elif PG_VERSION_NUM < 80400
		bool			isnull = false;
#endif
		Datum			datum;
		IndexTuple		itup;

		if (moveRightIfItNeeded(st)==false)
			return false;

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, st->offset));
#if PG_VERSION_NUM >= 90100
		datum = gintuple_get_key(&st->ginstate, itup, &category);
		cmp = ginCompareAttEntries(&st->ginstate,
									st->attnum + 1, st->curval, st->category,
									gintuple_get_attrnum(&st->ginstate, itup), datum, category);
#elif PG_VERSION_NUM >= 80400
		datum = gin_index_getattr(&st->ginstate, itup);

		cmp = compareAttEntries(&st->ginstate,
									st->attnum + 1, st->curval,
									gintuple_get_attrnum(&st->ginstate, itup), datum);
#else
		datum = index_getattr(itup, FirstOffsetNumber, st->ginstate.tupdesc, &isnull);

		cmp = DatumGetInt32(
				FunctionCall2(
						&st->ginstate.compareFn,
						st->curval,
						datum
					));
#endif
		if ( cmp == 0 )
		{
			if ( st->curval && !TS_GET_TYPEVAL(st, st->attnum, attbyval) )
				pfree( (void*) st->curval );
			return true;
		}

		st->offset++;
	}

	return false;
}

static void
gin_setup_firstcall(FuncCallContext  *funcctx, text *name, int attnum) {
	MemoryContext	 oldcontext;
	GinStatState	 *st;
	char *relname=t2c(name);
	TupleDesc			tupdesc;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(GinStatState*)palloc( sizeof(GinStatState) );
	memset(st,0,sizeof(GinStatState));
	st->index = gin_index_open(
		 makeRangeVarFromNameList(stringToQualifiedNameList(relname, "gin_stat")));
	initGinState( &st->ginstate, st->index );

#if PG_VERSION_NUM >= 80400
	if (attnum < 0 || attnum >= st->index->rd_att->natts)
		elog(ERROR,"Wrong column's number");
	st->attnum = attnum;
#else
	st->attnum = 0;
#endif

	funcctx->user_fctx = (void*)st;

#if PG_VERSION_NUM >= 120000
	tupdesc = CreateTemplateTupleDesc(2);
#else
	tupdesc = CreateTemplateTupleDesc(2, false);
#endif
	TupleDescInitEntry(tupdesc, 1, "value",
			TS_GET_TYPEVAL(st, st->attnum, atttypid),
			TS_GET_TYPEVAL(st, st->attnum, atttypmod),
			TS_GET_TYPEVAL(st, st->attnum, attndims));
	TupleDescInitEntry(tupdesc, 2, "nrow", INT4OID, -1, 0);

	memset( st->nulls, ISNOTNULL, 2*sizeof(*st->nulls) );

#if PG_VERSION_NUM >= 120000
	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#else
	funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

	MemoryContextSwitchTo(oldcontext);
	pfree(relname);

	st->offset = InvalidOffsetNumber;
	st->buffer = ReadBuffer( st->index, GIN_ROOT_BLKNO );
}

static void
processTuple( FuncCallContext  *funcctx,  GinStatState *st, IndexTuple itup ) {
	MemoryContext		oldcontext;
#if PG_VERSION_NUM >= 90100
	Datum				key;
#elif PG_VERSION_NUM < 80400
	bool				isnull;
#endif

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

#if PG_VERSION_NUM >= 90100
	key = gintuple_get_key(&st->ginstate, itup, &st->category);

	if (st->category != GIN_CAT_NORM_KEY)
		st->curval = (Datum)0;
	else
#endif
	st->curval = datumCopy(
#if PG_VERSION_NUM >= 90100
					key,
#elif PG_VERSION_NUM >= 80400
					gin_index_getattr(&st->ginstate, itup),
#else
					index_getattr(itup, FirstOffsetNumber, st->ginstate.tupdesc, &isnull),
#endif
					TS_GET_TYPEVAL(st, st->attnum, attbyval),
					TS_GET_TYPEVAL(st, st->attnum, attlen));
	MemoryContextSwitchTo(oldcontext);

	st->dvalues[0] = st->curval;
#if PG_VERSION_NUM >= 90100
	/* do no distiguish various null category */
	st->nulls[0] = (st->category == GIN_CAT_NORM_KEY) ? ISNOTNULL : ISNULL;
#endif

	if ( GinIsPostingTree(itup) ) {
		BlockNumber	rootblkno = GinGetPostingTree(itup);
#if PG_VERSION_NUM >= 90400
		GinBtreeData	btree;
		GinBtreeStack	*stack;
		ItemPointerData	minItem;
		int				nlist;
		ItemPointer		list;
#else
		GinPostingTreeScan *gdi;
		Buffer			entrybuffer;
#endif
		Page		page;
		uint32		predictNumber;

		LockBuffer(st->buffer, GIN_UNLOCK);
#if PG_VERSION_NUM >= 90400
		stack = ginScanBeginPostingTree(&btree, st->index, rootblkno
#if PG_VERSION_NUM >= 90600
										, NULL
#endif
										);
		page = BufferGetPage(stack->buffer);
		ItemPointerSetMin(&minItem);
		list = GinDataLeafPageGetItems(page, &nlist, minItem);
		pfree(list);
		predictNumber = stack->predictNumber;
		st->dvalues[1] = Int32GetDatum( predictNumber * nlist );
#elif PG_VERSION_NUM >= 90100
		gdi = ginPrepareScanPostingTree(st->index, rootblkno, TRUE);
		entrybuffer = ginScanBeginPostingTree(gdi);
		page = BufferGetPage(entrybuffer);
		predictNumber = gdi->stack->predictNumber;
		st->dvalues[1] = Int32GetDatum( predictNumber * GinPageGetOpaque(page)->maxoff );
#else
		gdi = prepareScanPostingTree(st->index, rootblkno, TRUE);
		entrybuffer = scanBeginPostingTree(gdi);
		page = BufferGetPage(entrybuffer);
		predictNumber = gdi->stack->predictNumber;
		st->dvalues[1] = Int32GetDatum( predictNumber * GinPageGetOpaque(page)->maxoff );
#endif

#if PG_VERSION_NUM < 90400
		LockBuffer(entrybuffer, GIN_UNLOCK);
		freeGinBtreeStack(gdi->stack);
		pfree(gdi);
#else
		LockBuffer(stack->buffer, GIN_UNLOCK);
		freeGinBtreeStack(stack);
#endif
	} else {
		st->dvalues[1] = Int32GetDatum( GinGetNPosting(itup) );
		LockBuffer(st->buffer, GIN_UNLOCK);
	}
}

PG_FUNCTION_INFO_V1(gin_stat);
Datum	gin_stat(PG_FUNCTION_ARGS);
Datum
gin_stat(PG_FUNCTION_ARGS) {
	FuncCallContext  *funcctx;
	GinStatState	 *st;
	Datum result=(Datum)0;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	Page page;

	if (SRF_IS_FIRSTCALL()) {
		text	*name=PG_GETARG_TEXT_P(0);
		funcctx = SRF_FIRSTCALL_INIT();
		gin_setup_firstcall(funcctx, name, (PG_NARGS()==2) ? PG_GETARG_INT32(1) : 0 );
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (GinStatState*)(funcctx->user_fctx);

	if ( refindPosition(st) == false ) {
		UnlockReleaseBuffer( st->buffer );
		gin_index_close(st->index);

		SRF_RETURN_DONE(funcctx);
	}

	for(;;) {
		st->offset++;

		if (moveRightIfItNeeded(st)==false) {
			UnlockReleaseBuffer( st->buffer );
			gin_index_close(st->index);

			SRF_RETURN_DONE(funcctx);
		}

		page = BufferGetPage(st->buffer);
		ituple = (IndexTuple) PageGetItem(page, PageGetItemId(page, st->offset));

#if PG_VERSION_NUM >= 80400
		if (st->attnum + 1 == gintuple_get_attrnum(&st->ginstate, ituple))
#endif
			break;
	}

	processTuple( funcctx,  st, ituple );

	htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
#if PG_VERSION_NUM >= 120000
	result = HeapTupleGetDatum(htuple);
#else
	result = TupleGetDatum(funcctx->slot, htuple);
#endif
	
	SRF_RETURN_NEXT(funcctx, result);
}

PG_FUNCTION_INFO_V1(gin_count_estimate);
Datum gin_count_estimate(PG_FUNCTION_ARGS);
#if PG_VERSION_NUM >= 80300
Datum
gin_count_estimate(PG_FUNCTION_ARGS) {
	text			*name=PG_GETARG_TEXT_P(0);
	Relation		index;
	IndexScanDesc	scan;
	int64			count = 0;
	char			*relname=t2c(name);
	ScanKeyData		key;
#if PG_VERSION_NUM >= 80400
	TIDBitmap		*bitmap = tbm_create(work_mem * 1024L
#if PG_VERSION_NUM >= 100000
										 , NULL
#endif
										 );
#else
#define	MAXTIDS		1024
	ItemPointerData	tids[MAXTIDS];
	int32			returned_tids;
	bool			more;
#endif

	index = gin_index_open(
		 makeRangeVarFromNameList(stringToQualifiedNameList(relname, "gin_count_estimate")));

	if ( index->rd_opcintype[0] != TSVECTOROID ) {
		gin_index_close(index);
		elog(ERROR, "Column type is not a tsvector");
	}

	key.sk_flags = 0;
	key.sk_attno = 1;
	key.sk_strategy	= TSearchStrategyNumber;
	key.sk_subtype  = 0;
	key.sk_argument = PG_GETARG_DATUM(1);

	fmgr_info( F_TS_MATCH_VQ , &key.sk_func );

#if PG_VERSION_NUM >= 90100
#if PG_VERSION_NUM >= 90400
	scan = index_beginscan_bitmap(index, SnapshotSelf, 1);
#else
	scan = index_beginscan_bitmap(index, SnapshotNow, 1);
#endif
	index_rescan(scan, &key, 1, NULL, 0);

	count = index_getbitmap(scan, bitmap);
	tbm_free(bitmap);
#elif PG_VERSION_NUM >= 80400
	scan = index_beginscan_bitmap(index, SnapshotNow, 1, &key);

	count = index_getbitmap(scan, bitmap);
	tbm_free(bitmap);
#else
	scan = index_beginscan_multi(index, SnapshotNow, 1, &key);

	do {
		more = index_getmulti(scan, tids, MAXTIDS, &returned_tids);
		count += returned_tids;
	} while(more);
#endif

	index_endscan( scan );
	gin_index_close(index);

	PG_RETURN_INT64(count);
}
#else
Datum
gin_count_estimate(PG_FUNCTION_ARGS) {
	elog(NOTICE, "Function is not working under PgSQL < 8.3");

	PG_RETURN_INT64(0);
}
#endif

PG_FUNCTION_INFO_V1(spgist_stat);
Datum spgist_stat(PG_FUNCTION_ARGS);
Datum
spgist_stat(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90200
	elog(NOTICE, "Function is not working under PgSQL < 9.2");

	PG_RETURN_TEXT_P(CStringGetTextDatum("???"));
#else
	text	   *name = PG_GETARG_TEXT_P(0);
	RangeVar   *relvar;
	Relation	index;
	BlockNumber blkno;
	BlockNumber totalPages = 0,
				innerPages = 0,
				leafPages = 0,
				emptyPages = 0,
				deletedPages = 0;
	double	  usedSpace = 0.0,
			  usedLeafSpace = 0.0,
			  usedInnerSpace = 0.0;
	char		res[1024];
	int		 bufferSize = -1;
	int64	   innerTuples = 0,
				leafTuples = 0,
				nAllTheSame = 0,
				nLeafPlaceholder = 0,
				nInnerPlaceholder = 0,
				nLeafRedirect = 0,
				nInnerRedirect = 0;

#define IS_INDEX(r) ((r)->rd_rel->relkind == RELKIND_INDEX)
#define IS_SPGIST(r) ((r)->rd_rel->relam == SPGIST_AM_OID)

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
	index = relation_openrv(relvar, AccessExclusiveLock);

	if (!IS_INDEX(index) || !IS_SPGIST(index))
		elog(ERROR, "relation \"%s\" is not an SPGiST index",
			 RelationGetRelationName(index));

	totalPages = RelationGetNumberOfBlocks(index);

	for (blkno = SPGIST_ROOT_BLKNO; blkno < totalPages; blkno++)
	{
		Buffer	  buffer;
		Page		page;
		int		 pageFree;

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);

		if (PageIsNew(page) || SpGistPageIsDeleted(page))
		{
			deletedPages++;
			UnlockReleaseBuffer(buffer);
			continue;
		}

		if (SpGistPageIsLeaf(page))
		{
			leafPages++;
			leafTuples += PageGetMaxOffsetNumber(page);
			nLeafPlaceholder += SpGistPageGetOpaque(page)->nPlaceholder;
			nLeafRedirect += SpGistPageGetOpaque(page)->nRedirection;
		}
		else
		{
			int	 i,
					max;

			innerPages++;
			max = PageGetMaxOffsetNumber(page);
			innerTuples += max;
			nInnerPlaceholder += SpGistPageGetOpaque(page)->nPlaceholder;
			nInnerRedirect += SpGistPageGetOpaque(page)->nRedirection;
			for (i = FirstOffsetNumber; i <= max; i++)
			{
				SpGistInnerTuple it;

				it = (SpGistInnerTuple) PageGetItem(page,
													PageGetItemId(page, i));
				if (it->allTheSame)
					nAllTheSame++;
			}
		}

		if (bufferSize < 0)
			bufferSize = BufferGetPageSize(buffer)
				- MAXALIGN(sizeof(SpGistPageOpaqueData))
				- SizeOfPageHeaderData;

		pageFree = PageGetExactFreeSpace(page);

		usedSpace += bufferSize - pageFree;
		if (SpGistPageIsLeaf(page))
			usedLeafSpace += bufferSize - pageFree;
		else
			usedInnerSpace += bufferSize - pageFree;

		if (pageFree == bufferSize)
			emptyPages++;

		UnlockReleaseBuffer(buffer);
	}

	index_close(index, AccessExclusiveLock);

	totalPages--;			   /* discount metapage */

	snprintf(res, sizeof(res),
			 "totalPages:		%u\n"
			 "deletedPages:	  %u\n"
			 "innerPages:		%u\n"
			 "leafPages:		 %u\n"
			 "emptyPages:		%u\n"
			 "usedSpace:		 %.2f kbytes\n"
			 "usedInnerSpace:	%.2f kbytes\n"
			 "usedLeafSpace:	 %.2f kbytes\n"
			 "freeSpace:		 %.2f kbytes\n"
			 "fillRatio:		 %.2f%%\n"
			 "leafTuples:		" INT64_FORMAT "\n"
			 "innerTuples:	   " INT64_FORMAT "\n"
			 "innerAllTheSame:   " INT64_FORMAT "\n"
			 "leafPlaceholders:  " INT64_FORMAT "\n"
			 "innerPlaceholders: " INT64_FORMAT "\n"
			 "leafRedirects:	 " INT64_FORMAT "\n"
			 "innerRedirects:	" INT64_FORMAT,
			 totalPages, deletedPages, innerPages, leafPages, emptyPages,
			 usedSpace / 1024.0,
			 usedInnerSpace / 1024.0,
			 usedLeafSpace / 1024.0,
			 (((double) bufferSize) * ((double) totalPages) - usedSpace) / 1024,
			 100.0 * (usedSpace / (((double) bufferSize) * ((double) totalPages))),
			 leafTuples, innerTuples, nAllTheSame,
			 nLeafPlaceholder, nInnerPlaceholder,
			 nLeafRedirect, nInnerRedirect);

	PG_RETURN_TEXT_P(CStringGetTextDatum(res));
#endif
}

#if PG_VERSION_NUM >= 90200

typedef struct SPGistPrintStackElem {
	ItemPointerData		iptr;
	int16				nlabel;
	int					level;
} SPGistPrintStackElem;

typedef struct SPGistPrint {
	SpGistState	state;
	Relation	index;
	Datum		dvalues[8 /* see CreateTemplateTupleDesc call */];
#if PG_VERSION_NUM >= 110000
	bool			nulls[8];
#else
	char			nulls[8];
#endif
	List		*stack;
} SPGistPrint;

static void
pushSPGistPrint(FuncCallContext *funcctx, SPGistPrint *prst, ItemPointer ip, int level) {
	MemoryContext	oldcontext;
	SPGistPrintStackElem	*e;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	e = palloc(sizeof(*e));
	e->iptr = *ip;
	e->nlabel = 0;
	e->level = level;
	prst->stack = lcons(e, prst->stack);

	MemoryContextSwitchTo(oldcontext);
}

static void
close_spgist_print(SPGistPrint *prst) {
	index_close(prst->index, AccessExclusiveLock);
}
#endif

PG_FUNCTION_INFO_V1(spgist_print);
Datum spgist_print(PG_FUNCTION_ARGS);
Datum
spgist_print(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90200
	elog(NOTICE, "Function is not working under PgSQL < 9.2");

	PG_RETURN_TEXT_P(CStringGetTextDatum("???"));
#else
	FuncCallContext			*funcctx;
	SPGistPrint				*prst;
	SPGistPrintStackElem	*s;
	HeapTuple				htuple;
	Datum					result;
	MemoryContext			oldcontext;

	if (SRF_IS_FIRSTCALL()) {
		text			*name=PG_GETARG_TEXT_P(0);
		RangeVar		*relvar;
		Relation		index;
		ItemPointerData	ipd;
		TupleDesc		tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
		index = relation_openrv(relvar, AccessExclusiveLock);

		if (!IS_INDEX(index) || !IS_SPGIST(index))
			elog(ERROR, "relation \"%s\" is not an SPGiST index",
				 RelationGetRelationName(index));

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		prst = palloc(sizeof(*prst));

		prst->index = index;
		initSpGistState(&prst->state, index);

#if PG_VERSION_NUM >= 120000
		tupdesc = CreateTemplateTupleDesc(3 /* types */ + 1 /* level */ + 1 /* nlabel */ +  2 /* tids */ + 1);
#else
		tupdesc = CreateTemplateTupleDesc(3 /* types */ + 1 /* level */ + 1 /* nlabel */ +  2 /* tids */ + 1, false);
#endif
		TupleDescInitEntry(tupdesc, 1, "tid", TIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 2, "allthesame", BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, 3, "node", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 4, "level", INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, 5, "tid_pointer", TIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, 6, "prefix",
				(prst->state.attPrefixType.type == VOIDOID) ? INT4OID : prst->state.attPrefixType.type, -1, 0);
		TupleDescInitEntry(tupdesc, 7, "label",
				(prst->state.attLabelType.type == VOIDOID) ? INT4OID : prst->state.attLabelType.type, -1, 0);
		TupleDescInitEntry(tupdesc, 8, "leaf",
				(prst->state.attType.type == VOIDOID) ? INT4OID : prst->state.attType.type, -1, 0);

#if PG_VERSION_NUM >= 120000
		funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);
#else
		funcctx->slot = TupleDescGetSlot(tupdesc);
#endif

		funcctx->user_fctx = (void*)prst;

		MemoryContextSwitchTo(oldcontext);

		ItemPointerSet(&ipd, SPGIST_ROOT_BLKNO, FirstOffsetNumber);
		prst->stack = NIL;
		pushSPGistPrint(funcctx, prst, &ipd, 1);

		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	prst = (SPGistPrint*)(funcctx->user_fctx);

next:
	for(;;) {
		if ( prst->stack == NIL ) {
			close_spgist_print(prst);
			SRF_RETURN_DONE(funcctx);
		}

		CHECK_FOR_INTERRUPTS();

		s = (SPGistPrintStackElem*)linitial(prst->stack);
		prst->stack = list_delete_first(prst->stack);

		if (ItemPointerIsValid(&s->iptr))
			break;
		free(s);
	}

	do {
		Buffer				buffer;
		Page				page;
		SpGistDeadTuple		dtuple;
		ItemPointer			tid;

		buffer = ReadBuffer(prst->index, ItemPointerGetBlockNumber(&s->iptr));
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		if (ItemPointerGetOffsetNumber(&s->iptr) > PageGetMaxOffsetNumber(page)) {
			UnlockReleaseBuffer(buffer);
			pfree(s);
			goto next;
		}

		dtuple = (SpGistDeadTuple)PageGetItem(page, PageGetItemId(page, ItemPointerGetOffsetNumber(&s->iptr)));

		if (dtuple->tupstate != SPGIST_LIVE)  {
			UnlockReleaseBuffer(buffer);
			pfree(s);
			goto next;
		}

		if (SpGistPageIsLeaf(page)) {
				SpGistLeafTuple	leafTuple = (SpGistLeafTuple)dtuple;

				tid = palloc(sizeof(ItemPointerData));
				*tid = s->iptr;
				prst->dvalues[0] = PointerGetDatum(tid);
				prst->nulls[0] = ISNOTNULL;
				prst->nulls[1] = ISNULL;
				prst->nulls[2] = ISNULL;
				prst->dvalues[3]  = s->level;
				prst->nulls[3] = ISNOTNULL;
				prst->nulls[4] = ISNULL;
				prst->nulls[5] = ISNULL;
				prst->nulls[6] = ISNULL;
				prst->dvalues[7]  = datumCopy(SGLTDATUM(leafTuple, &prst->state),
											prst->state.attType.attbyval, prst->state.attType.attlen);
				prst->nulls[7] = ISNOTNULL;
		} else {
			SpGistInnerTuple	innerTuple = (SpGistInnerTuple)dtuple;
			int					i;
			SpGistNodeTuple		node;

			SGITITERATE(innerTuple, i, node) {
				if (ItemPointerIsValid(&node->t_tid)) {
					if (i >= s->nlabel)
						break;
				}
			}

			if (i >= innerTuple->nNodes) {
				UnlockReleaseBuffer(buffer);
				pfree(s);
				goto next;
			}

			tid = palloc(sizeof(ItemPointerData));
			*tid = s->iptr;
			prst->dvalues[0] = PointerGetDatum(tid);
			prst->nulls[0] = ISNOTNULL;
			prst->dvalues[1] = innerTuple->allTheSame;
			prst->nulls[1] = ISNOTNULL;
			prst->dvalues[2] = Int32GetDatum(s->nlabel);
			prst->nulls[2] = ISNOTNULL;
			prst->dvalues[3]  = s->level;
			prst->nulls[3] = ISNOTNULL;
			tid = palloc(sizeof(ItemPointerData));
			*tid = node->t_tid;
			prst->dvalues[4] = PointerGetDatum(tid);
			prst->nulls[5] = ISNOTNULL;
			if (innerTuple->prefixSize > 0) {
				prst->dvalues[5]  = datumCopy(SGITDATUM(innerTuple, &prst->state),
											prst->state.attPrefixType.attbyval, prst->state.attPrefixType.attlen);
				prst->nulls[5] = ISNOTNULL;
			} else
				prst->nulls[5] = ISNULL;
			if (!IndexTupleHasNulls(node)) {
				prst->dvalues[6]  = datumCopy(SGNTDATUM(node, &prst->state),
											prst->state.attLabelType.attbyval, prst->state.attLabelType.attlen);
				prst->nulls[6] = ISNOTNULL;
			} else
				prst->nulls[6] = ISNULL;
			prst->nulls[7] = ISNULL;

			pushSPGistPrint(funcctx, prst, &node->t_tid, s->level + 1);
			s->nlabel = i + 1;
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
			prst->stack = lcons(s, prst->stack);
			MemoryContextSwitchTo(oldcontext);
			s = NULL;
		}

		UnlockReleaseBuffer(buffer);
		if (s) pfree(s);
	} while(0);

	htuple = heap_formtuple(funcctx->attinmeta->tupdesc, prst->dvalues, prst->nulls);
#if PG_VERSION_NUM >= 120000
	result = HeapTupleGetDatum(htuple);
#else
	result = TupleGetDatum(funcctx->slot, htuple);
#endif

	SRF_RETURN_NEXT(funcctx, result);
#endif
}

PG_FUNCTION_INFO_V1(gin_statpage);
Datum gin_statpage(PG_FUNCTION_ARGS);
Datum
gin_statpage(PG_FUNCTION_ARGS)
{
#if PG_VERSION_NUM < 90400
	elog(NOTICE, "Function is not working under PgSQL < 9.4");

	PG_RETURN_TEXT_P(CStringGetTextDatum("???"));
#else
	text	   *name = PG_GETARG_TEXT_P(0);
	RangeVar   *relvar;
	Relation	index;
	BlockNumber blkno;
	char		res[1024];
	uint32		totalPages,
#if PG_VERSION_NUM >= 100000
				deletedPages = 0,
				emptyDataPages = 0,
#endif
				entryPages = 0,
				dataPages = 0,
				dataInnerPages = 0,
				dataLeafPages = 0,
				entryInnerPages = 0,
				entryLeafPages = 0
				;
	uint64		dataInnerFreeSpace = 0,
				dataLeafFreeSpace = 0,
				dataInnerTuplesCount = 0,
				dataLeafIptrsCount = 0,
				entryInnerFreeSpace = 0,
				entryLeafFreeSpace = 0,
				entryInnerTuplesCount = 0,
				entryLeafTuplesCount = 0,
				entryPostingSize = 0,
				entryPostingCount = 0,
				entryAttrSize = 0
				;

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(name));
	index = relation_openrv(relvar, AccessExclusiveLock);

	if (index->rd_rel->relkind != RELKIND_INDEX ||
			index->rd_rel->relam != GIN_AM_OID)
		elog(ERROR, "relation \"%s\" is not an SPGiST index",
			 RelationGetRelationName(index));

	totalPages = RelationGetNumberOfBlocks(index);

	for (blkno = GIN_ROOT_BLKNO; blkno < totalPages; blkno++)
	{
		Buffer		buffer;
		Page		page;
		PageHeader	header;

		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buffer);
		header = (PageHeader)page;

#if PG_VERSION_NUM >= 100000
		if (GinPageIsDeleted(page))
		{
			deletedPages++;
		}
		else
#endif
		if (GinPageIsData(page))
		{
			dataPages++;
			if (GinPageIsLeaf(page))
			{
				ItemPointerData minItem, *ptr;
				int nlist;


				dataLeafPages++;
				dataLeafFreeSpace += header->pd_upper - header->pd_lower;
				ItemPointerSetMin(&minItem);

				ptr = GinDataLeafPageGetItems(page, &nlist, minItem);

				if (ptr)
				{
					pfree(ptr);
					dataLeafIptrsCount += nlist;
				}
				else
					emptyDataPages++;
			}
			else
			{
				dataInnerPages++;
				dataInnerFreeSpace += header->pd_upper - header->pd_lower;
				dataInnerTuplesCount += GinPageGetOpaque(page)->maxoff;
			}
		}
		else
		{
			IndexTuple itup;
			OffsetNumber i, maxoff;

			maxoff = PageGetMaxOffsetNumber(page);

			entryPages++;
			if (GinPageIsLeaf(page))
			{
				entryLeafPages++;
				entryLeafFreeSpace += header->pd_upper - header->pd_lower;
				entryLeafTuplesCount += maxoff;
			}
			else
			{
				entryInnerPages++;
				entryInnerFreeSpace += header->pd_upper - header->pd_lower;
				entryInnerTuplesCount += maxoff;
			}

			for (i = 1; i <= maxoff; i++)
			{
				itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, i));

				if (GinPageIsLeaf(page))
				{
					GinPostingList *list = (GinPostingList *)GinGetPosting(itup);
					entryPostingCount += GinGetNPosting(itup);
					entryPostingSize += SizeOfGinPostingList(list);
					entryAttrSize += GinGetPostingOffset(itup) - IndexInfoFindDataOffset((itup)->t_info);
				}
				else
				{
					entryAttrSize += IndexTupleSize(itup) - IndexInfoFindDataOffset((itup)->t_info);
				}
			}
		}

		UnlockReleaseBuffer(buffer);
	}

	index_close(index, AccessExclusiveLock);
	totalPages--;

	snprintf(res, sizeof(res),
			 "totalPages:			%u\n"
#if PG_VERSION_NUM >= 100000
			 "deletedPages:		  %u\n"
			 "emptyDataPages:		%u\n"
#endif
			 "dataPages:			 %u\n"
			 "dataInnerPages:		%u\n"
			 "dataLeafPages:		 %u\n"
			 "dataInnerFreeSpace:	" INT64_FORMAT "\n"
			 "dataLeafFreeSpace:	 " INT64_FORMAT "\n"
			 "dataInnerTuplesCount:  " INT64_FORMAT "\n"
			 "dataLeafIptrsCount:	" INT64_FORMAT "\n"
			 "entryPages:			%u\n"
			 "entryInnerPages:	   %u\n"
			 "entryLeafPages:		%u\n"
			 "entryInnerFreeSpace:   " INT64_FORMAT "\n"
			 "entryLeafFreeSpace:	" INT64_FORMAT "\n"
			 "entryInnerTuplesCount: " INT64_FORMAT "\n"
			 "entryLeafTuplesCount:  " INT64_FORMAT "\n"
			 "entryPostingSize:	  " INT64_FORMAT "\n"
			 "entryPostingCount:	 " INT64_FORMAT "\n"
			 "entryAttrSize:		 " INT64_FORMAT "\n"
			 ,
			 totalPages,
#if PG_VERSION_NUM >= 100000
			 deletedPages,
			 emptyDataPages,
#endif
			 dataPages,
			 dataInnerPages,
			 dataLeafPages,
			 dataInnerFreeSpace,
			 dataLeafFreeSpace,
			 dataInnerTuplesCount,
			 dataLeafIptrsCount,
			 entryPages,
			 entryInnerPages,
			 entryLeafPages,
			 entryInnerFreeSpace,
			 entryLeafFreeSpace,
			 entryInnerTuplesCount,
			 entryLeafTuplesCount,
			 entryPostingSize,
			 entryPostingCount,
			 entryAttrSize
			 );

	PG_RETURN_TEXT_P(CStringGetTextDatum(res));
#endif
}

#if PG_VERSION_NUM >= 120000
typedef enum {stat, print} TreeCond;
typedef struct
{
	IdxInfo idxInfo;
	IdxStat idxStat;
}BtreeIdxInfo;

/*
 * Depth-first search for btree
 * using for statistic data collection
 * and printing index values by level
 */
static void
btree_deep_search(Relation rel, int level,
		BlockNumber blk,  BtreeIdxInfo *btreeIdxInfo, TreeCond cond)
{
	Page			page;
	IndexTuple		itup;
	ItemId			iid;
	OffsetNumber	i,
					maxoff;
	BlockNumber		cblk;
	BTPageOpaque	opaque;
	Buffer buffer =  _bt_getbuf(rel, blk, BT_READ);

	page = (Page) BufferGetPage(buffer);
	opaque = (BTPageOpaque) PageGetSpecialPointer(page);
	maxoff = PageGetMaxOffsetNumber(page);

	switch (cond)
	{
		case stat:
		{
			btreeIdxInfo->idxStat.numpages++;
			btreeIdxInfo->idxStat.tuplesize+=BTMaxItemSize(page)-PageGetFreeSpace(page);
			btreeIdxInfo->idxStat.totalsize+=BLCKSZ;
			btreeIdxInfo->idxStat.numtuple+=maxoff;

			if (level > btreeIdxInfo->idxStat.level)
				btreeIdxInfo->idxStat.level = level;

			if (P_ISLEAF(opaque))
			{
				btreeIdxInfo->idxStat.numleafpages++;
				btreeIdxInfo->idxStat.leaftuplesize+=BTMaxItemSize(page)-
						PageGetFreeSpace(page);
				btreeIdxInfo->idxStat.numleaftuple+=maxoff;
			}
			break;
		}
		case print:
		{
			while ( (btreeIdxInfo->idxInfo.ptr-((char*)btreeIdxInfo->idxInfo.txt))
					+ level*4 + 128 >= btreeIdxInfo->idxInfo.len )
			{
					int dist=btreeIdxInfo->idxInfo.ptr-((char*)btreeIdxInfo->idxInfo.txt);
					btreeIdxInfo->idxInfo.len *= 2;
					btreeIdxInfo->idxInfo.txt=(text*)repalloc(btreeIdxInfo->idxInfo.txt,
							btreeIdxInfo->idxInfo.len);
					btreeIdxInfo->idxInfo.ptr = ((char*)btreeIdxInfo->idxInfo.txt)+dist;
			}

			sprintf(btreeIdxInfo->idxInfo.ptr, "lvl: %d, blk: %d, numTuples: %d\n",
							level,
							blk,
							(int)maxoff);

			btreeIdxInfo->idxInfo.ptr=strchr(btreeIdxInfo->idxInfo.ptr,'\0');
			break;
		}
	}

	if (!P_ISLEAF(opaque) && ((level < btreeIdxInfo->idxInfo.maxlevel)
			||(btreeIdxInfo->idxInfo.maxlevel<0)))
	{
		for (i = P_FIRSTDATAKEY(opaque); i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);

			if (!ItemIdIsValid(iid))
				btreeIdxInfo->idxStat.numinvalidtuple++;

			itup = (IndexTuple) PageGetItem(page, iid);
			cblk = BTreeTupleGetDownLink(itup);

			btree_deep_search(rel, level + 1, cblk, btreeIdxInfo, cond);
		}
	}
	UnlockReleaseBuffer(buffer);
}

/*
 * Print some statistic about btree index
 * This function shows information for live pages only
 * and do not shows information about deleting pages
 *
 * SELECT btree_stat(INDEXNAME);
 */
PG_FUNCTION_INFO_V1(btree_stat);
Datum btree_stat(PG_FUNCTION_ARGS);
Datum
btree_stat(PG_FUNCTION_ARGS)
{
	text		*name=PG_GETARG_TEXT_PP(0);
	RangeVar	*relvar;
	Relation	index;
	List		*relname_list;
	BtreeIdxInfo btreeIdxInfo;

	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;
	BlockNumber rootBlk;

	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;
	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = btree_index_open(relvar);

	memset(&btreeIdxInfo.idxStat, 0, sizeof(IdxStat));

	/* Start dts from root */
	metabuf = _bt_getbuf(index, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	rootBlk = metad->btm_root;
	UnlockReleaseBuffer(metabuf);

	btree_deep_search(index, 0, rootBlk, &btreeIdxInfo,stat);

	btree_index_close(index);

	sprintf(ptr,
		"Number of levels:		  %d\n"
		"Number of pages:		   %d\n"
		"Number of leaf pages:	  %d\n"
		"Number of tuples:		  %d\n"
		"Number of invalid tuples:  %d\n"
		"Number of leaf tuples:	 %d\n"
		"Total size of tuples:	  "INT64_FORMAT" bytes\n"
		"Total size of leaf tuples: "INT64_FORMAT" bytes\n"
		"Total size of index:	   "INT64_FORMAT" bytes\n",
		btreeIdxInfo.idxStat.level+1,
		btreeIdxInfo.idxStat.numpages,
		btreeIdxInfo.idxStat.numleafpages,
		btreeIdxInfo.idxStat.numtuple,
		btreeIdxInfo.idxStat.numinvalidtuple,
		btreeIdxInfo.idxStat.numleaftuple,
		btreeIdxInfo.idxStat.tuplesize,
		btreeIdxInfo.idxStat.leaftuplesize,
		btreeIdxInfo.idxStat.totalsize);

	ptr=strchr(ptr,'\0');

	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

typedef struct BtPItem
{
	Buffer		   buffer;
	Page		   page;
	OffsetNumber   offset;
	int			   level;
	struct BtPItem *next;
} BtPItem;

typedef struct
{
	List	 *relname_list;
	RangeVar *relvar;
	Relation index;
	Datum	 *dvalues;
	bool	 *nulls;
	BtPItem	 *item;
} BtTypeStorage;

/*
 * Open page in btree
 * Returns nitem as a pointer to stack for btree levels stored.
 * We process tuples from buffer in the top of nitem.
 * After the complete processing of a top buffer
 * we return to buffer on one level upper and go to next btree leaf.
 */
static BtPItem*
openBtPPage( FuncCallContext *funcctx, BlockNumber blk )
{
	BtPItem		  *nitem;
	MemoryContext oldcontext;

	Relation index = ( (BtTypeStorage*)(funcctx->user_fctx) )->index;
	BTPageOpaque opaque;
	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	nitem = (BtPItem*)palloc( sizeof(BtPItem) );
	memset(nitem,0,sizeof(BtPItem));

	nitem->buffer = _bt_getbuf(index, blk, BT_READ);
	Assert(BufferIsValid(nitem->buffer));
	nitem->page = (Page) BufferGetPage(nitem->buffer);
	opaque = (BTPageOpaque)PageGetSpecialPointer(nitem->page);
	nitem->offset=P_FIRSTDATAKEY(opaque);
	nitem->next = ( (BtTypeStorage*)(funcctx->user_fctx) )->item;
	nitem->level = ( nitem->next ) ? nitem->next->level+1 : 1;
	( (BtTypeStorage*)(funcctx->user_fctx) )->item = nitem;

	MemoryContextSwitchTo(oldcontext);

	return nitem;
}

static BtPItem*
closeBtPPage( FuncCallContext *funcctx )
{
	BtPItem  *oitem = ( (BtTypeStorage*)(funcctx->user_fctx) )->item;

	( (BtTypeStorage*)(funcctx->user_fctx) )->item = oitem->next;

	UnlockReleaseBuffer(oitem->buffer);
	pfree( oitem );
	return ( (BtTypeStorage*)(funcctx->user_fctx) )->item;
}

static void
btree_close_call( FuncCallContext  *funcctx )
{
	BtTypeStorage *st = (BtTypeStorage*)(funcctx->user_fctx);

	while(st->item && closeBtPPage(funcctx));

	pfree(st->dvalues);
	pfree(st->nulls);

	btree_index_close(st->index);
}

/*
 * Settings for first call of btree_print
 * Sets the current memory context
 */
static void
btree_setup_firstcall(FuncCallContext  *funcctx, text *name)
{
	MemoryContext oldcontext;
	BtTypeStorage *st;
	TupleDesc	 tupdesc;
	char		  attname[NAMEDATALEN];
	int			  i;
	BlockNumber   blk;
	Buffer		  buffer;

	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

	st=(BtTypeStorage*)palloc( sizeof(BtTypeStorage) );
	memset(st,0,sizeof(BtTypeStorage));
	st->relname_list = textToQualifiedNameList(name);
	st->relvar = makeRangeVarFromNameList(st->relname_list);
	st->index = btree_index_open(st->relvar);
	st->item = NULL;
	funcctx->user_fctx = (void*)st;

	tupdesc = CreateTemplateTupleDesc(st->index->rd_att->natts+2);
	TupleDescInitEntry(tupdesc, 1, "level", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, 2, "valid", BOOLOID, -1, 0);
	for (i = 0; i < st->index->rd_att->natts; i++)
	{
		sprintf(attname, "z%d", i+2);
		TupleDescInitEntry(
			tupdesc,
			i+3,
			attname,
			TS_GET_TYPEVAL(st, i, atttypid),
			TS_GET_TYPEVAL(st, i, atttypmod),
			TS_GET_TYPEVAL(st, i, attndims) );
		BlessTupleDesc(tupdesc);
	}
	BlessTupleDesc(tupdesc);


	st->dvalues = (Datum *) palloc((tupdesc->natts+2) * sizeof(Datum));
	st->nulls = palloc((tupdesc->natts+2) * sizeof(*st->nulls));

	funcctx->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	buffer = _bt_gettrueroot(st->index);
	blk = BufferGetBlockNumber(buffer);
	UnlockReleaseBuffer(buffer);

	MemoryContextSwitchTo(oldcontext);

	st->item=openBtPPage(funcctx, blk);
}

/*
 * Show index elements for btree from root to MAXLEVEL
 * SELECT btree_tree(INDEXNAME[, MAXLEVEL]);
 */
PG_FUNCTION_INFO_V1(btree_tree);
Datum btree_tree(PG_FUNCTION_ARGS);
Datum
btree_tree(PG_FUNCTION_ARGS)
{
	text			*name=PG_GETARG_TEXT_PP(0);
	RangeVar		*relvar;
	Relation		index;
	List			*relname_list;
	BtreeIdxInfo btreeIdxInfo;
	Buffer			metabuf;
	Page			metapg;
	BTMetaPageData  *metad;
	BlockNumber		rootBlk;

	/*
	 *  If we use MAXLEVEL is not used in SELECT btree_tree(INDEXNAME),
	 *  info.maxlevel set to -1
	 *  If MAXLEVEL is used in btree_tree call, set info.maxlevel = MAXLEVEL
	 */
	btreeIdxInfo.idxInfo.maxlevel = ( PG_NARGS() > 1 ) ? PG_GETARG_INT32(1) : -1;
	btreeIdxInfo.idxInfo.len=1024;
	btreeIdxInfo.idxInfo.txt=(text*)palloc( btreeIdxInfo.idxInfo.len );
	btreeIdxInfo.idxInfo.ptr=((char*)btreeIdxInfo.idxInfo.txt)+VARHDRSZ;

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = btree_index_open(relvar);

	/* Start dts from root block */
	metabuf = _bt_getbuf(index, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	rootBlk = metad->btm_root;
	UnlockReleaseBuffer(metabuf);

	btree_deep_search(index, 0, rootBlk, &btreeIdxInfo, print);

	btree_index_close(index);

	btreeIdxInfo.idxInfo.ptr=strchr(btreeIdxInfo.idxInfo.ptr,'\0');

	SET_VARSIZE(btreeIdxInfo.idxInfo.txt,
			btreeIdxInfo.idxInfo.ptr-((char*)btreeIdxInfo.idxInfo.txt));
	PG_RETURN_POINTER(btreeIdxInfo.idxInfo.txt);
}

/*
 * Print objects stored in btree tuples
 * works only if objects in index have textual representation
 * select * from btree_print(INDEXNAME)
 *		as t(level int, valid bool, a box) where level =1;
 */
PG_FUNCTION_INFO_V1(btree_print);
Datum btree_print(PG_FUNCTION_ARGS);
Datum
btree_print(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	BtTypeStorage   *st;
	ItemId		  iid;
	IndexTuple	  ituple;
	HeapTuple	   htuple;
	int				i;
	bool			isnull;
	BTPageOpaque opaque;
	Datum result=(Datum)0;

	if (SRF_IS_FIRSTCALL())
	{
		text	*name=PG_GETARG_TEXT_PP(0);
		funcctx = SRF_FIRSTCALL_INIT();
		btree_setup_firstcall(funcctx, name);
		PG_FREE_IF_COPY(name,0);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (BtTypeStorage*)(funcctx->user_fctx);

	/*
	 * Looking through the stack of btree pages.
	 * If the item == NULL, then the search is over.
	 * For each btree page we look through all the tuples.
	 * If the offset is larger than the btree page size, the search is over.
	 */

	if ( !st->item )
	{
		btree_close_call(funcctx);
		SRF_RETURN_DONE(funcctx);
	}

	/* View all tuples on the page */
	while( st->item->offset > PageGetMaxOffsetNumber( st->item->page ) )
	{
		if ( ! closeBtPPage(funcctx) ) {
			btree_close_call(funcctx);
			SRF_RETURN_DONE(funcctx);
		}
	}

	iid = PageGetItemId( st->item->page, st->item->offset );
	ituple = (IndexTuple) PageGetItem(st->item->page, iid);

	st->dvalues[0] = Int32GetDatum( st->item->level );
	st->nulls[0] = ISNOTNULL;
	opaque = (BTPageOpaque)PageGetSpecialPointer(st->item->page);
	st->dvalues[1] = BoolGetDatum(P_ISLEAF(opaque) && !ItemPointerIsValid(&(ituple->t_tid)) ? false : true);
	st->nulls[1] = ISNOTNULL;
	for(i=2; i<funcctx->attinmeta->tupdesc->natts; i++)
	{
		if (!P_ISLEAF(opaque) && !ItemPointerIsValid(&(ituple->t_tid)))
		{
			st->dvalues[i] = (Datum)0;
			st->nulls[i] = ISNULL;
		}
		else
		{
			st->dvalues[i] = index_getattr(ituple, i-1, st->index->rd_att, &isnull);
			st->nulls[i] = ( isnull ) ? ISNULL : ISNOTNULL;
		}
	}

	htuple = heap_formtuple(funcctx->attinmeta->tupdesc, st->dvalues, st->nulls);
	result = TupleGetDatum(funcctx->slot, htuple);
	result = HeapTupleGetDatum(htuple);
	st->item->offset = OffsetNumberNext(st->item->offset);

	if (!P_ISLEAF(opaque))
	{
		BlockNumber blk = BTreeTupleGetDownLink(ituple);
		openBtPPage(funcctx, blk );
	}

	SRF_RETURN_NEXT(funcctx, result);
}

/*
 * Print some statistic about brin index
 * SELECT brin_stat(INDEXNAME);
 */
PG_FUNCTION_INFO_V1(brin_stat);
Datum brin_stat(PG_FUNCTION_ARGS);
Datum
brin_stat(PG_FUNCTION_ARGS)
{
	text		*name=PG_GETARG_TEXT_PP(0);
	RangeVar	*relvar;
	Relation	index;
	List		*relname_list;

	BlockNumber startBlk;
	BlockNumber heapNumBlocks;
	BlockNumber pagesPerRange;
	BrinRevmap *revmap;
	Relation heapRel;

	Buffer buf;

	uint32 numRevmapPages=0;
	uint32 numTuples = 0;
	uint32 numEmptyPages = 0;
	uint32 numRegularPages = 0;
	uint64 freeSpace = 0;
	uint64 usedSpace = 0;

	text *out=(text*)palloc(1024);
	char *ptr=((char*)out)+VARHDRSZ;

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = brin_index_open(relvar);

	revmap = brinRevmapInitialize(index, &pagesPerRange, NULL);
	heapRel = table_open(IndexGetRelation(RelationGetRelid(index), false),
								 AccessShareLock);
	/* Determine range of pages to process */
	heapNumBlocks = RelationGetNumberOfBlocks(heapRel); // total pages

	table_close(heapRel, AccessShareLock);

	/* The index is up to date, no update required */
	startBlk = 0;

	buf = InvalidBuffer;
	/* Scan the revmap */
	for (; startBlk < heapNumBlocks; startBlk += pagesPerRange)
	{
		BrinTuple  *tup;
		OffsetNumber off;
		Page	page;

		numRevmapPages++;
		/* Get on-disk tuple */
		tup = brinGetTupleForHeapBlock(revmap, startBlk, &buf, &off, NULL,
				   BUFFER_LOCK_SHARE, NULL);

		if (tup)
		{
			BrinDesc* bdesc;

			numTuples++;
			page = BufferGetPage(buf);

			if (BRIN_IS_REGULAR_PAGE(page))
				numRegularPages++;
			freeSpace += PageGetFreeSpace(page);
			bdesc = brin_build_desc(index);
			usedSpace += MAXALIGN(sizeof(BrinMemTuple) +
					sizeof(BrinValues) * bdesc->bd_tupdesc->natts);
		}
		else
			/* If the revmap page points to void */
			numEmptyPages++;

		UnlockReleaseBuffer(buf);
	}

	brinRevmapTerminate(revmap);

	sprintf(ptr,
		"Number of revmap pages: 	%d\n"
		"Number of empty revmap pages:	%d\n"
		"Number of regular pages:	%d\n"
		"Number of tuples: 		%d\n"
		"Used space 		"INT64_FORMAT" bytes\n"
		"Free space 		"INT64_FORMAT" bytes\n",
		numRevmapPages,
		numEmptyPages,
		numRegularPages,
		numTuples,
		usedSpace,
		freeSpace
		);

	ptr=strchr(ptr,'\0');

	brin_index_close(index);
	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}

/*
 * Print numbers of heap blocks from revmap
 * and numbers of end blocks from ranges
 */
PG_FUNCTION_INFO_V1(brin_print);
Datum brin_print(PG_FUNCTION_ARGS);
Datum
brin_print(PG_FUNCTION_ARGS)
{
	text	*name=PG_GETARG_TEXT_PP(0);
	RangeVar		*relvar;
	Relation		index;
	List			*relname_list;

	BlockNumber heapBlk;
	BlockNumber numBlocks;
	BlockNumber pagesPerRange;
	BrinRevmap *revmap;
	Relation heapRel;
	int len = 1024;

	text *out=(text*)palloc(len);
	char *ptr=((char*)out)+VARHDRSZ;

	relname_list = textToQualifiedNameList(name);
	relvar = makeRangeVarFromNameList(relname_list);
	index = brin_index_open(relvar);

	heapRel = table_open(IndexGetRelation(RelationGetRelid(index), false),
									 AccessShareLock);

	/* Determine range of pages to process */
	numBlocks = RelationGetNumberOfBlocks(heapRel);
	table_close(heapRel, AccessShareLock);
	revmap = brinRevmapInitialize(index, &pagesPerRange, NULL);

	for(heapBlk = 0; heapBlk < numBlocks; heapBlk += pagesPerRange)
	{
		BlockNumber rangeEndBlk;
		BlockNumber rangeBlk;

		while ( (ptr-((char*)out)) + heapBlk*4 + 128 >= len )
		{
			int dist=ptr-((char*)out);
			len *= 2;
			out=(text*)repalloc(out, len);
			ptr = ((char*)out)+dist;
			Assert(0);
		}

		/* Compute rage end */
		if (heapBlk + pagesPerRange > numBlocks)
			rangeEndBlk = Min(RelationGetNumberOfBlocks(heapRel) - heapBlk,
								  pagesPerRange);
		else
			/* Easy case: range is known to be complete */
			rangeEndBlk = pagesPerRange;
		sprintf(ptr, "Start block: %d; end block: %d \n", heapBlk, rangeEndBlk);

		for (rangeBlk = 0; rangeBlk < rangeEndBlk; rangeBlk++)
		{
				Page page;
				/* Read buffer for open table */
				Buffer buf = ReadBuffer(heapRel, rangeBlk);
				page = BufferGetPage(buf);
				sprintf(ptr, "Start block: %d; end block: %d; offset: %d, free: %d\n",
						heapBlk,
						rangeEndBlk,
						(int) PageGetMaxOffsetNumber(page),
						(int) PageGetFreeSpace(page));

				ReleaseBuffer(buf);
		}

		ptr=strchr(ptr,'\0');
	}

	brinRevmapTerminate(revmap);

	brin_index_close(index);
	ptr=strchr(ptr,'\0');
	SET_VARSIZE(out, ptr-((char*)out));
	PG_RETURN_POINTER(out);
}
#endif
