/*
 * Copyright (c) 2014 Jim Nasby, Blue Treble Consulting http://BlueTreble.com
 */

extern Datum variant_in(PG_FUNCTION_ARGS);
extern Datum variant_out(PG_FUNCTION_ARGS);

typedef struct
{
	int32				vl_len_;		/* varlena header (do not touch directly!) */
	Oid					pOid;				/* Not a plain OID! */
} VariantData;
typedef VariantData *Variant;

/*
 * To be as efficient as possible, variants are represented to the rest of the
 * system with a "Packed Oid" whose high-order bits are flags. Of course, this
 * poses a problem if the OID of an input type is too large; it would
 * over-write our flag bits. If that happens, VAR_OVERFLOW is true and the top
 * 8 bits of the Oid appear in the last byte of the variant. Note that our
 * flags are *always* in the top bits of the Packed Oid, but don't necessarily
 * take the full top 8 bits.
 *
 * Currently we also have a flag to indicate whether the data we were handed is
 * NULL or not. *This is not the same as the variant being NULL!* We support
 * being handed "(int,)", which means we have an int that is NULL.
 *
 * Funally, VAR_VERSION is used as an internal version indicator. Currently we
 * only support version 0, but if we didn't reserve space for a version
 * identifier pg_upgrade would be in trouble if we ever needed to change our
 * storage format.
 *
 * TODO: Further improve efficiency by not storing the varlena size header if
 * typid is a varlena.
 */

#define VAR_OVERFLOW				0x80000000
#define VAR_ISNULL					0x40000000
#define	VAR_VERSION					0x20000000
#define VAR_FLAGMASK				0xE0000000
#define OID_MASK						0x1FFFFFFF
#define OID_TOO_LARGE(Oid) (Oid > OID_MASK)


#define VHDRSZ				(sizeof(VariantData))
#define VDATAPTR(x)		( (Pointer) ( (x) + 1 ) )

/* Easier to use internal representation. */
typedef struct {
	Datum					data;
	Oid						typid;
	bool					isnull;		/* This is the only flag we care about internally */
} VariantDataInt;
typedef VariantDataInt *VariantInt;

/*
 * fmgr macros for range type objects
 */
#define DatumGetVariantType(X)		((Variant) PG_DETOAST_DATUM(X))
#define DatumGetVariantTypeCopy(X)	((Variant) PG_DETOAST_DATUM_COPY(X))
#define VariantTypeGetDatum(X)		PointerGetDatum(X)
#define PG_GETARG_VARIANT(n)			DatumGetVariantType(PG_GETARG_DATUM(n))
#define PG_GETARG_VARIANT_COPY(n)		DatumGetVariantTypeCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_VARIANT(x)			return VariantTypeGetDatum(x)
