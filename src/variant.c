#include "postgres.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "access/htup_details.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "variant.h"

static Oid getIntOid();
static char * get_cstring(Oid intTypeOid, MemoryContext ctx, Datum dat);
typedef struct {
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	Oid		typeOid;			/* OID of original data type */
} VariantData;

typedef VariantData *Variant;

#define VHDRSZ				(sizeof(VariantData))
#define VDATAPTR(x)		( (Datum *) ( (Variant *)(x) + 1 ) )


/*
 * You can include more files here if needed.
 * To use some types, you must include the
 * correct file here based on:
 * http://www.postgresql.org/docs/current/static/xfunc-c.html#XFUNC-C-TYPE-TABLE
 */

PG_MODULE_MAGIC;

/*
 * variant_in: Parse text representation of a variant
 *
 * - Cast to _variant._variant
 * - Extract type name and validate
 * 
 * TODO: use type's input function to convert type info to internal format. For
 * now, just store the raw _variant._variant Datum.
 */
PG_FUNCTION_INFO_V1(variant_in);
Datum
variant_in(PG_FUNCTION_ARGS)
{
	char					 *input = PG_GETARG_CSTRING(0);
	Variant					variant;
	Datum						composite;
	HeapTupleHeader	composite_tuple;
	bool						isnull;
	Oid							intTypeOid = InvalidOid;
	int32						typmod = 0;
	Oid							typiofunc;
	Oid							typioparam;
	FmgrInfo	 			proc;
	Oid							orgTypeOid;
	char					 *orgTypeName;
	text					 *orgData;
	StringInfoData	cmd;
	bool						do_pop = false;
	int							ret;
	Size						len;
	bytea					 *result;
	char					 *tmp;
	Datum					  tmpDatum;
	Datum					  orgDatum;
	int16 					resultTypLen;
	bool 						resultTypByVal;

	if (!SPI_connect()) {
		do_pop = true;
		SPI_push();
	}

	intTypeOid = getIntOid();

	/* Cast input data to our internal composite type */
	getTypeInputInfo(intTypeOid, &typiofunc, &typioparam);
	fmgr_info_cxt(typiofunc, &proc, fcinfo->flinfo->fn_mcxt);
	composite=InputFunctionCall(&proc, input, typioparam, typmod);

	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, composite);
	elog(LOG, "composite: %s, size %u", tmp, VARSIZE(composite));

	/* Extract data from internal composite type */
	composite_tuple=DatumGetHeapTupleHeader(composite);

	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, composite);
	elog(LOG, "(composite_tuple) composite: %s, size %u", tmp, VARSIZE(composite));

	tmpDatum=PointerGetDatum(composite_tuple);
	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, tmpDatum);
	elog(LOG, "composite_tuple size %u, tmpDatum: %s, size %u", VARSIZE(composite_tuple), tmp, VARSIZE(tmp));

	orgTypeOid = (Oid) GetAttributeByNum( composite_tuple, 1, &isnull );

	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, composite);
	elog(LOG, "(orgTypeOid) composite: %s, size %u", tmp, VARSIZE(composite));

	if (isnull)
		elog(ERROR, "original_type of variant must not be NULL");

	orgTypeName = format_type_be(orgTypeOid);
	orgData = (text *) GetAttributeByNum( composite_tuple, 2, &isnull );
	elog(LOG, "orgData: %s, size %u", text_to_cstring(orgData), VARSIZE(orgData));

	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, composite);
	elog(LOG, "(orgData) composite: %s, size %u", tmp, VARSIZE(composite));


	/* Make sure we can legitimately cast our input to the passed in type */
	initStringInfo(&cmd);

	/*
	 * $1 is our raw input. Cast it to internal composite, extract data
	 * portion, and attempt cast.
	 */
	appendStringInfo(&cmd, "SELECT CAST( '$1' AS %s )", orgTypeName);

	if (!(ret = SPI_execute_with_args(cmd.data, 1, &orgTypeOid, (Datum *) &orgData, isnull ? "n" : " ", true, 1)))
		elog(ERROR, "SPI_execute_with_args returned %s", SPI_result_code_string(ret));

	/*
	 * We can't just hand back a raw HeapTupleHeader Datum; it will confuse other
	 * parts of the system. So treat it as a bytea instead.
	 */
	len = VARSIZE(composite);
	elog( LOG, "len %zu, VARHDRSZ %u, VARSIZE %u, HeapTupleHeaderGetDatumLength %u", len, VARHDRSZ, VARSIZE(composite), HeapTupleHeaderGetDatumLength(composite));
	tmpDatum=PointerGetDatum(composite_tuple);
	tmp=get_cstring(intTypeOid, fcinfo->flinfo->fn_mcxt, tmpDatum);
	elog(LOG, "composite_tuple size %u, tmpDatum: %s, size %u", VARSIZE(composite_tuple), tmp, VARSIZE(tmp));
	result = (bytea *) palloc0(len + VARHDRSZ);
	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), &composite, len);
	elog(LOG, "result size %u", VARSIZE(result));

	getTypeInputInfo(orgTypeOid, &typiofunc, &typioparam);
	fmgr_info_cxt(typiofunc, &proc, fcinfo->flinfo->fn_mcxt);
	orgDatum=InputFunctionCall(&proc, text_to_cstring(orgData), typioparam, typmod);
	get_typlenbyval(orgTypeOid, &resultTypLen, &resultTypByVal);
	len = datumGetSize(orgDatum, resultTypByVal, resultTypLen);
	if (resultTypLen == -1)
		tmpDatum = PointerGetDatum(PG_DETOAST_DATUM_COPY(orgDatum));
	else
		tmpDatum = datumCopy(orgDatum, resultTypByVal, resultTypLen);

	variant = (Variant) SPI_palloc(len + VHDRSZ);
	SET_VARSIZE(variant, len);
	variant->typeOid = orgTypeOid;
	memcpy((char *)VDATAPTR(variant), (char *)tmpDatum, len);

  if (do_pop)
		SPI_pop();
	else
		SPI_finish();

	PG_RETURN_POINTER(variant);
}

char *
get_cstring(Oid intTypeOid, MemoryContext ctx, Datum dat)
{
	char					 *output;
	Oid							typiofunc;
	bool						isvarlena;
	FmgrInfo	 			proc;

	getTypeOutputInfo(intTypeOid, &typiofunc, &isvarlena);
	fmgr_info_cxt(typiofunc, &proc, ctx);
	output = OutputFunctionCall(&proc, dat);

	return output;
}

PG_FUNCTION_INFO_V1(variant_out);
Datum
variant_out(PG_FUNCTION_ARGS)
{
	Datum						input_datum = PG_GETARG_DATUM(0);
	Variant					input = (Variant) PG_DETOAST_DATUM_COPY(input_datum);
	char					 *output;
	bool						do_pop = false;
	Oid							intTypeOid = InvalidOid;
	Oid							typiofunc;
	bool						isvarlena;
	FmgrInfo	 			proc;
	char	*tmp;
	char					 *orgTypeName = format_type_be(input->typeOid);
	text					 *orgData;
	StringInfoData	out;


	elog(LOG, "input size=%u", VARSIZE(input));

	/* Simply call record output function for our internal type */
	getTypeOutputInfo(input->typeOid, &typiofunc, &isvarlena);
	fmgr_info_cxt(typiofunc, &proc, fcinfo->flinfo->fn_mcxt);
	tmp = OutputFunctionCall(&proc, *VDATAPTR(input));

	initStringInfo(&out);

	appendStringInfo(&out, "(%s,%s)", orgTypeName, tmp);

	tmp=out.data;

	PG_RETURN_CSTRING(tmp);
}

Oid
getIntOid()
{
	int		ret;
	bool	isnull;

	/*
	 * Get OID of our internal data type. This is necessary because record_in and
	 * record_out need it.
	 */
	if (!(ret = SPI_execute("SELECT '_variant._variant'::regtype::oid", true, 1)))
		elog( ERROR, "SPI_execute returned %s", SPI_result_code_string(ret));

	return DatumGetObjectId( heap_getattr(SPI_tuptable->vals[0], 1, SPI_tuptable->tupdesc, &isnull) );
}
// vi: noexpandtab sw=2 ts=2
