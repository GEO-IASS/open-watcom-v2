/****************************************************************************
*
*                            Open Watcom Project
*
*    Portions Copyright (c) 1983-2002 Sybase, Inc. All Rights Reserved.
*
*  ========================================================================
*
*    This file contains Original Code and/or Modifications of Original
*    Code as defined in and that are subject to the Sybase Open Watcom
*    Public License version 1.0 (the 'License'). You may not use this file
*    except in compliance with the License. BY USING THIS FILE YOU AGREE TO
*    ALL TERMS AND CONDITIONS OF THE LICENSE. A copy of the License is
*    provided with the Original Code and Modifications, and is also
*    available at www.sybase.com/developer/opensource.
*
*    The Original Code and all software distributed under the License are
*    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
*    EXPRESS OR IMPLIED, AND SYBASE AND ALL CONTRIBUTORS HEREBY DISCLAIM
*    ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF
*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR
*    NON-INFRINGEMENT. Please see the License for the specific language
*    governing rights and limitations under the License.
*
*  ========================================================================
*
* Description:  Type handle support for DWARF DIP.
*
****************************************************************************/


#include "dfdip.h"
#include <limits.h>
#include "dfld.h"
#include "dfmod.h"
#include "dfmodinf.h"
#include "dfloc.h"
#include "dfclean.h"
#include "dftype.h"

#include "clibext.h"


typedef struct {
    drmem_hdl curr;
    int       skip;
} array_wlk_skip; /* and jump */

static bool ArrayIndexSkip( drmem_hdl index, int pos, void *_df )
{
    array_wlk_skip *df = _df;
    pos = pos;
    if( df->skip == 0 ){
        df->curr = index;
        return( false );
    }
    df->skip--;
    return( true );
}

static DRWLKBLK ArrayWlkNext[DR_WLKBLK_ARRSIB] = {
    ArrayIndexSkip,
    ArrayIndexSkip,
    NULL
};

static drmem_hdl GetArrayDim( drmem_hdl index, int skip  ){
// Get current or next dim handle
    array_wlk_skip df;

    df.skip = skip;
    if( !DRWalkArraySibs( index, ArrayWlkNext, &df ) ){
        index = df.curr;
    }else{
        index = DRMEM_HDL_NULL;
    }
    return( index );
}

static bool GetStrLen( imp_image_handle *ii,
                        drmem_hdl dr_sym,
                        location_context *lc,
                        dr_typeinfo  *ret ){
//  Find value of scalar
    addr_seg        seg;
    location_list   src;
    location_list   dst;
    imp_mod_handle  im;
    union {
        long  l;
        short s;
        char  c;
    } val;
    unsigned        idx_size;
    mod_info        *modinfo;

    im = DwarfMod( ii, dr_sym );
    if( im == IMH_NOMOD ){
        return( false );
    }
    modinfo = IMH2MODI( ii, im );
    if( modinfo->is_segment == false ){
        seg = SEG_DATA; // if flat hoke segment
    }else{
        EvalSeg( ii, dr_sym, &seg );
    }
    if( EvalLocation( ii, lc, dr_sym, seg, &src ) != DS_OK ){
        return( false );
    }
    idx_size = ret->size;
    if( idx_size == 0 ){
        idx_size = modinfo->addr_size;
    }
    LocationCreate( &dst, LT_INTERNAL, &val );
    if( DCAssignLocation( &dst, &src, idx_size ) != DS_OK ){
        return( false );
    }
    switch( idx_size ){
    case 1:
        ret->size = val.c;
        break;
    case 2:
        ret->size = val.s;
        break;
    case 4:
        ret->size = val.l;
        break;
    }
    return( true );
}

/***********************/
/* Walk array dims     */
/***********************/
typedef struct{
    int_32           low;
    uint_32          count;
    imp_image_handle *ii;
    imp_type_handle  *it;
    location_context *lc;
    uint_32          num_elts;
    int              dim;
    bool             cont;
}array_wlk_wlk;

static bool ArraySubRange( drmem_hdl tsub, int index, void *df );
static bool ArrayEnumType( drmem_hdl tenu, int index, void *df );

static DRWLKBLK ArrayWlk[DR_WLKBLK_ARRSIB] = {
    ArraySubRange,
    ArrayEnumType,
    NULL
};

static void GetArraySize( imp_image_handle *ii,
                          imp_type_handle  *it,
                          location_context *lc ){
//Calculate size of array starting at it->array.index;
    drmem_hdl     dim;
    array_wlk_wlk df;
    uint_32       base_stride;
    uint_32       n_el;

    df.ii = ii;
    df.it = it;
    df.lc = lc;
    df.count = 1;
    df.dim = 0;
    df.cont = false;
    DRWalkArraySibs( it->array.index, ArrayWlk, &df );
    it->array.num_elts = df.count;
    it->array.low = df.low;
    df.cont = true;
    dim = GetArrayDim( it->array.index, 1 );
    if( dim != DRMEM_HDL_NULL ) {
        DRWalkArraySibs( dim, ArrayWlk, &df );
    }
    it->array.dims = df.dim;
    it->typeinfo.size = df.count * it->array.base_stride;
    if( !it->array.column_major ){
        base_stride = it->typeinfo.size;
        n_el = it->array.num_elts;
        base_stride = n_el ? base_stride / n_el : 0;
        it->array.base_stride = base_stride;
    }
    it->array.is_set = true;
    it->array.is_based = false;
}

static void GetArraySubSize( imp_image_handle *ii,
                          imp_type_handle  *it,
                          location_context *lc ){
// Calc array size one in from previous dim
    array_wlk_wlk df;
    uint_32         new_size;
    uint_32         base_stride;
    uint_32         n_el;

    df.ii = ii;
    df.it = it;
    df.lc = lc;
    df.count = 1;
    df.dim = 0;
    df.cont = false;
    DRWalkArraySibs( it->array.index, ArrayWlk, &df );
    new_size = it->typeinfo.size;
    n_el = it->array.num_elts;
    new_size = n_el ? new_size / n_el : 0;
    if( it->array.column_major ){
        base_stride = it->array.base_stride;
        base_stride *= it->array.num_elts;
        it->array.base_stride = base_stride;
    }else{
        base_stride = it->typeinfo.size;
        base_stride = df.count ? base_stride / df.count : 0;
        it->array.base_stride = base_stride;
    }
    it->typeinfo.size = new_size;
    it->array.num_elts = df.count;
    it->array.low = df.low;
    --it->array.dims;
    it->array.is_set = true;
    it->array.is_based = false;
}

static void InitTypeHandle( imp_image_handle *ii,
                            imp_type_handle  *it,
                            location_context *lc  ){
/***********************************************************************/
//Set type handle to the base state
//If array poise at first index
    imp_type_handle sub;
    dr_array_info   info;
    drmem_hdl       btype;
    dr_array_stat   stat;
    uint_32         base_stride;
    uint_32         n_el;

    if( it->state == DF_NOT ) {
        DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
        DRGetTypeInfo( it->type, &it->typeinfo );
        it->state = DF_SET;
        it->sub_array = false;
        if( it->typeinfo.kind == DR_TYPEK_ARRAY ){
            if( it->typeinfo.size == 0 ){
                btype =  DRSkipTypeChain( it->type ); /* skip modifiers and typedefs */
                stat = DRGetArrayInfo( btype, &info );
                if( stat & DR_ARRAY_STRIDE_SIZE ){
                    base_stride = info.stride_size/8;
                }else{
                    btype = DRGetTypeAT( btype );    /* get base type */
                    sub.type = btype;
                    sub.im = it->im;
                    sub.state = DF_NOT;
                    InitTypeHandle( ii, &sub, lc );
                    base_stride = sub.typeinfo.size;
                }
                it->array.base_stride = base_stride;
                it->array.column_major = 0; /* 1 for fortran */
                if( stat & DR_ARRAY_ORDERING ){
                    if( info.ordering == DW_ORD_col_major ){
                        it->array.column_major = 1;
                    }
                }else if( IMH2MODI( ii, it->im )->lang == DR_LANG_FORTRAN ){
                    it->array.column_major = 1;
                }
                if( info.child == DRMEM_HDL_NULL ) { // set info now
                    it->array.dims = 1;
                    it->array.low = 0;
                    it->array.index = DRMEM_HDL_NULL;
                    if( stat & DR_ARRAY_COUNT ){
                        if( info.count == 0 ){ // ie  char (*x)[]
                            info.count = 1;
                        }
                        it->typeinfo.size = info.count * it->array.base_stride;
                        it->array.num_elts= info.count;
                    }else{
                        it->typeinfo.size =  it->array.base_stride;
                    }
                    if( !it->array.column_major ){
                        base_stride = it->typeinfo.size;
                        n_el = it->array.num_elts;
                        base_stride = n_el ? base_stride / n_el : 0;
                        it->array.base_stride = base_stride;
                    }
                    it->array.is_set = true;
                    it->array.is_based = false;
                    it->sub_array = false;
                }else{
                    it->sub_array = true;
                    it->array.is_set = false;
                    it->array.index = GetArrayDim( info.child, 0 );
                }
            }
        }else if( it->typeinfo.kind == DR_TYPEK_STRING ){
            if( DRStringLengthAT( it->type ) ) {
                if( !GetStrLen( ii, it->type, lc, &it->typeinfo ) ){
                    it->typeinfo.size = 1;
                }
            }
        }
    }
    if( it->typeinfo.kind == DR_TYPEK_ARRAY ){
        if( !it->array.is_set ){
            GetArraySize( ii, it, lc );
        }else if( it->array.is_based ){
            GetArraySubSize( ii, it, lc );
        }
    }
}

struct mod_type{
    imp_image_handle    *ii;
    imp_mod_handle      im;
    IMP_TYPE_WKR        *wk;
    imp_type_handle     *it;
    void                *d;
    walk_result         wr;
};

static bool AType( drmem_hdl type, void *_typ_wlk, dr_search_context *cont )
/**************************************************************************/
{
    struct mod_type *typ_wlk = _typ_wlk;
    bool            ret;
    imp_type_handle *it;
    dr_dbg_handle   saved;

    cont = cont;
    ret = true;
    it = typ_wlk->it;
    it->im = typ_wlk->im;
    it->state = DF_NOT;
    it->type = type;
    saved = DRGetDebug();
    typ_wlk->wr = typ_wlk->wk( typ_wlk->ii, it, typ_wlk->d );
    DRSetDebug( saved );
    if( typ_wlk->wr != WR_CONTINUE ){
        ret = false;
    }
    return( ret );
}

walk_result     DIGENTRY DIPImpWalkTypeList( imp_image_handle *ii,
                    imp_mod_handle im, IMP_TYPE_WKR *wk, imp_type_handle *it,
                    void *d )
{
    drmem_hdl       cu_tag;
    struct mod_type typ_wlk;

    DRSetDebug( ii->dwarf->handle ); /* must do at each interface */
    cu_tag = IMH2MODI( ii, im )->cu_tag;
    typ_wlk.ii = ii;
    typ_wlk.im = im;
    typ_wlk.wk = wk;
    typ_wlk.it  = it;
    typ_wlk.d   = d;
    DRWalkModTypes( cu_tag, AType, &typ_wlk );
    return( typ_wlk.wr );
}

imp_mod_handle  DIGENTRY DIPImpTypeMod( imp_image_handle *ii,
                                imp_type_handle *it )
{
    /*
        Return the module that the type handle comes from.
    */
    ii = ii;
    return( it->im );
}

extern void MapImpTypeInfo( dr_typeinfo *typeinfo, dip_type_info *ti )
{
    /*
        Map dwarf info to dip imp
    */
    type_kind   kind = TK_NONE;

    ti->modifier = TM_NONE;
    switch( typeinfo->kind ){
    case DR_TYPEK_NONE:
        kind = TK_NONE;
        break;
    case DR_TYPEK_DATA:
        kind = TK_DATA;
        break;
    case DR_TYPEK_CODE:
        kind = TK_CODE;
        break;
    case DR_TYPEK_ADDRESS:
        kind = TK_ADDRESS;
        break;
    case DR_TYPEK_VOID:
        kind = TK_VOID;
        break;
    case DR_TYPEK_BOOL:
        kind = TK_BOOL;
        break;
    case DR_TYPEK_ENUM:
        kind = TK_ENUM;
        break;
    case DR_TYPEK_CHAR:
        kind = TK_CHAR;
        break;
    case DR_TYPEK_INTEGER:
        kind = TK_INTEGER;
        break;
    case DR_TYPEK_REAL:
        kind = TK_REAL;
        break;
    case DR_TYPEK_COMPLEX:
        kind = TK_COMPLEX;
        break;
    case DR_TYPEK_STRING:
        kind = TK_STRING;
        break;
    case DR_TYPEK_POINTER:
        kind = TK_POINTER;
        break;
    case DR_TYPEK_REF:
        kind = TK_POINTER;
        break;
    case DR_TYPEK_STRUCT:
    case DR_TYPEK_UNION:
    case DR_TYPEK_CLASS:
        kind = TK_STRUCT;
        break;
    case DR_TYPEK_ARRAY:
        kind = TK_ARRAY;
        break;
    case DR_TYPEK_FUNCTION:
        kind = TK_FUNCTION;
        break;
    }
    ti->kind = kind;
    ti->size = typeinfo->size;
    ti->modifier = TM_NONE;
    switch( typeinfo->mclass ){
    case DR_MOD_BASE:
        if( (ti->kind == TK_INTEGER) || (ti->kind == TK_CHAR)) {
            if( typeinfo->modifier.sign ) {
                ti->modifier = TM_SIGNED;
            } else {
                ti->modifier = TM_UNSIGNED;
            }
        }
        break;
    case DR_MOD_ADDR:
        switch( typeinfo->modifier.ptr ){
        case DR_PTR_none:
            ti->modifier = TM_NEAR;
            break;
        case DR_PTR_near16:
        case DR_PTR_near32:
            ti->modifier = TM_NEAR;
            break;
        case DR_PTR_far16:
        case DR_PTR_far32:
            ti->modifier = TM_FAR;
            break;
        case DR_PTR_huge16:
            ti->modifier = TM_HUGE;
            break;
        }
        if( typeinfo->kind == DR_TYPEK_REF ){
            ti->modifier |= TM_FLAG_DEREF;
        }
        break;
    }
}

dip_status      DIGENTRY DIPImpTypeInfo( imp_image_handle *ii,
                imp_type_handle *it, location_context *lc, dip_type_info *ti )
{
    /*
        Fill in the type information for the type handle. The location
        context is being passed in because it might be needed to calculate
        the size of the type (variable dimensioned arrays and the like).
    */

    InitTypeHandle( ii, it, lc );
    MapImpTypeInfo( &it->typeinfo, ti );
    if( ti->kind == TK_INTEGER ){  // this can be removed when 10.5 gets updated
        char *name;

        name =  DRGetName( it->type );
        if( name != NULL ){
            if( strcmp( name, "char" ) == 0
             || strcmp( name, "unsigned char" ) == 0 ){
                ti->kind = TK_CHAR;
            }
            DCFree( name );
        }

    }
    return( DS_OK );
}

dip_status      DIGENTRY DIPImpTypeBase( imp_image_handle *ii,
                        imp_type_handle *it, imp_type_handle *base,
                        location_context *lc, location_list *ll )
{
    drmem_hdl  btype;

    lc = lc; ll = ll;
    /*
        Given an implementation type handle, fill in 'base' with the
        base type of the handle.
     */
    if( base != it ){
        *base = *it;
    }
    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    if( base->state == DF_SET && base->sub_array  ){
        base->array.index = GetArrayDim( base->array.index, 1 );
        if( base->array.is_based ){
            base->array.is_set = false;
        }
        base->array.is_based = true;
        if( base->array.index != DRMEM_HDL_NULL ) {
            return( DS_OK );
        }
    }
    btype =  DRSkipTypeChain( base->type ); /* skip modifiers and typedefs */
    base->type = DRGetTypeAT( btype );      /* get base type */
    if( base->type == DRMEM_HDL_NULL ) {
        base->type = DRMEM_HDL_VOID;        /* no type means 'void' */
    }
    base->state = DF_NOT;
    return( DS_OK );
}

typedef struct {
    int_32 low;
    int_32 high;
} enum_range;

static bool AEnum( drmem_hdl var, int index, void *_de )
{
    enum_range *de = _de;
    int_32  value;

    index = index;
    if( !DRConstValAT( var, (uint_32 *)&value ) ){
        return( false );
    }
    if( value < de->low ){
        de->low = value;
    }else if( value > de->high ){
        de->high = value;
    }
    return( true );
}

static bool ArrayEnumType( drmem_hdl tenu, int index, void *_df )
/****************************************************************/
// Find low, high bounds of enum
{
//TODO:unsigned range
    array_wlk_wlk  *df = _df;
    enum_range     de;
    int_32         count;

    index = index;
    de.low  = _I32_MIN;
    de.high = _I32_MAX;
    if( !DRWalkEnum( tenu, AEnum, &de )){
        return( false );
    }
    df->low = de.low;
    count = de.high-de.low+1;
    df->count *= count;

    df->dim++;
    return( df->cont );
}

static bool GetSymVal( imp_image_handle *ii,
                       drmem_hdl dr_sym, location_context *lc,
                       int_32 *ret ){
//  Find value of scalar
    drmem_hdl       dr_type;
    dr_typeinfo     typeinfo[1];
    imp_mod_handle  im;
    addr_seg        seg;
    location_list   src;
    location_list   dst;

    dr_type = DRGetTypeAT( dr_sym );
    if( dr_type == DRMEM_HDL_NULL ) {
        return( false );
    }
    DRGetTypeInfo( dr_type, typeinfo );
    if( typeinfo->size > sizeof( *ret ) ){
        return( false );
    }
    im = DwarfMod( ii, dr_sym );
    if( im == IMH_NOMOD ){
        return( false );
    }
    if( IMH2MODI( ii, im )->is_segment == false ){
        seg = SEG_DATA; // if flat hoke segment
    }else{
        EvalSeg( ii, dr_sym, &seg );
    }
    if( EvalLocation( ii, lc, dr_sym, seg, &src ) != DS_OK ){
        return( false );
    }
    LocationCreate( &dst, LT_INTERNAL, ret );
    if( DCAssignLocation( &dst, &src, typeinfo->size ) != DS_OK ){
        return( false );
    }
    return( true );
}

static bool GetDrVal( array_wlk_wlk *df, dr_val32 *val, int_32 *ret )
/*******************************************************************/
// extract the value from val
{
    switch( val->val_class ){
    case DR_VAL_INT:
        *ret = val->val.s;
        return( true );
    case DR_VAL_REF:
        if( DRConstValAT( val->val.ref, (uint_32*)ret ) ){
            return( true );
        } else if( GetSymVal( df->ii, val->val.ref, df->lc, ret ) ) {
            return( true );
        }
    }
    return( false );
}

static bool ArraySubRange( drmem_hdl tsub, int index, void *_df )
/****************************************************************/
{
    array_wlk_wlk *df = _df;
    dr_subinfo info;
    int_32     low;
    int_32     high;
    int_32     count;

    index = index;
    DRGetSubrangeInfo( tsub, &info );
    /* DWARF 2.0 specifies lower bound defaults for C/C++ (0) and FORTRAN (1) */
    if( info.low.val_class == DR_VAL_NOT ) {
        if( IMH2MODI( df->ii, df->it->im )->lang == DR_LANG_FORTRAN )
            low = 1;
        else
            low = 0;
    } else {
        GetDrVal( df, &info.low, &low );
    }
    if( info.count.val_class == DR_VAL_NOT ){
        if( info.high.val_class == DR_VAL_NOT ){
            return( false );
        }
        GetDrVal( df, &info.high, &high );
        count = high - low +1;
    }else{
        GetDrVal( df, &info.count, &count );
    }
    df->low = low;
    df->count *= count;
    df->dim++;
    return( df->cont );
}

dip_status      DIGENTRY DIPImpTypeArrayInfo( imp_image_handle *ii,
                        imp_type_handle *array, location_context *lc,
                        array_info *ai, imp_type_handle *index )
{
    /*
        Given an implemenation type handle that represents an array type,
        get information about the array shape and index type. The location
        context is for variable dimensioned arrays again. The 'index'
        parameter is filled in with the type of variable used to subscript
        the array. It may be NULL, in which case no information is returned.
    */

    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    if( array->state == DF_NOT ){
        InitTypeHandle( ii, array, lc );
    }
    if( array->state == DF_NOT ){
        DCStatus( DS_ERR | DS_BAD_PARM );
        return( DS_ERR | DS_BAD_PARM  );
    }
    if( array->typeinfo.kind != DR_TYPEK_ARRAY ){
        DCStatus( DS_ERR | DS_BAD_PARM );
        return( DS_ERR | DS_BAD_PARM  );
    }
    ai->low_bound = array->array.low;
    ai->num_elts = array->array.num_elts;
    ai->num_dims = array->array.dims;
    ai->column_major = array->array.column_major;
    ai->stride = array->array.base_stride;
    if( index != NULL ){
        index->im = array->im;
        if( array->array.index == DRMEM_HDL_NULL ) { //Fake a type up
            index->state = DF_SET;
            index->type  = DRMEM_HDL_NULL;
            index->typeinfo.size = 0;
            index->typeinfo.kind = DR_TYPEK_NONE;
            index->typeinfo.mclass = DR_MOD_NONE;
        }else{
            index->state = DF_NOT;
            index->type = array->array.index;
        }
    }
    return( DS_OK );
}

/*
    Given an implementation type handle that represents a procedure type,
    get information about the return and parameter types. If the 'n'
    parameter is zero, store the return type handle into the 'parm'
    variable. Otherwise store the handle for the n'th parameter in
    'parm'.
*/
typedef struct {
    int             count;
    int             last;
    drmem_hdl       var;
} parm_wlk;

static bool AParm( drmem_hdl var, int index, void *_df )
/******************************************************/
{
    parm_wlk *df = _df;

    index = index;
    ++df->count;
    if( df->count == df->last ){
        df->var = var;
        return( false );
    }else{
        return( true );
    }
}

drmem_hdl GetParmN( imp_image_handle *ii, drmem_hdl proc, int count )
/******************************************************/
// return handle of the n parm
{
    parm_wlk    df;
    drmem_hdl   ret;

    df.count = 0;
    df.last = count;
    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    if( DRWalkBlock( proc, DR_SRCH_parm, AParm, (void *)&df ) ) {
        ret = DRMEM_HDL_NULL;
    }else{
        ret = df.var;
    }
    return( ret );
}

extern int GetParmCount(  imp_image_handle *ii, drmem_hdl proc ){
/******************************************************/
// return handle of the n parm
    parm_wlk df;

    df.count = 0;
    df.last = 0;
    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    DRWalkBlock( proc, DR_SRCH_parm, AParm, (void *)&df );
    return( df.count );
}

dip_status      DIGENTRY DIPImpTypeProcInfo( imp_image_handle *ii,
                imp_type_handle *proc, imp_type_handle *parm, unsigned n )
{
    drmem_hdl       btype;
    drmem_hdl       parm_type = DRMEM_HDL_NULL;
    dip_status      ret;

    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    btype = DRSkipTypeChain( proc->type ); /* skip modifiers and typedefs */
    if( n > 0 ){
        btype = GetParmN( ii, btype, n );
    }// if n == 0 just fall through and get type of function
    if( btype != DRMEM_HDL_NULL ) {
        parm_type = DRGetTypeAT( btype );    /* get type */
    }
    if( parm_type != DRMEM_HDL_NULL ) {
        parm->state = DF_NOT;
        parm->type = parm_type;
        parm->im = proc->im;
        ret = DS_OK;
    }else{
        ret = DS_FAIL;
    }
    return( ret );
}

dip_status      DIGENTRY DIPImpTypePtrAddrSpace( imp_image_handle *ii,
                    imp_type_handle *it, location_context *lc, address *a )
{
    /*
        Given an implementation type handle that represents a pointer type,
        get information about any implied address space for that pointer
        (based pointer cruft). If there is an implied address space for
        the pointer, fill in *a with the information and return DS_OK.
        Otherwise return DS_FAIL.
    */
    dip_status ret;

    ret = EvalBasedPtr( ii, lc, it->type, a );
    return( ret );
}


int DIGENTRY DIPImpTypeCmp( imp_image_handle *ii, imp_type_handle *it1,
                                imp_type_handle *it2 )
{
    long diff;

    ii = ii;
    diff = it1->type - it2->type;
    return( diff );
}
/*****************************/
/* Structure Enum Walks      */
/*****************************/
typedef struct inh_vbase {
    struct inh_vbase *next;
    drmem_hdl        base;
} inh_vbase;

typedef struct {
    imp_mod_handle      im;
    imp_image_handle    *ii;
    sym_sclass          sclass;
    void                *d;
    drmem_hdl           root;
    drmem_hdl           inh;
    inh_vbase           *vbase;
    enum_einfo          einfo;
} type_wlk_com;


typedef struct {
    type_wlk_com     com;
    IMP_SYM_WKR      *wk;
    imp_sym_handle   *is;
    walk_result      wr;
}type_wlk_wlk;

typedef struct {
    type_wlk_com     com;
    int              (*comp)(const void *, const void *, size_t);
    lookup_item      *li;
    search_result     sr;
}type_wlk_lookup;

typedef union {
    type_wlk_com    com;
    type_wlk_wlk    sym;
    type_wlk_lookup lookup;
}type_wlk;


static bool AddBase( drmem_hdl base, inh_vbase **lnk )
/****************************************************/
//if base not in list add return false
{
    inh_vbase   *cur;

    while( (cur = *lnk) != NULL ){
        if( base <= cur->base ){
            if( base == cur->base ){
                return( false );
            }
            break;
        }
        lnk = &cur->next;
    }
    cur = DCAlloc( sizeof( *cur ) );
    cur->base = base;
    cur->next = *lnk;
    *lnk = cur;
    return( true );
}

static bool FreeBases( void *_lnk )
/*********************************/
//Free bases
{
    inh_vbase   **lnk = _lnk;
    inh_vbase   *cur;
    inh_vbase   *next;

    for( cur = *lnk; cur != NULL; cur = next ) {
        next = cur->next;
        DCFree( cur );
    }
    *lnk = NULL;
    return 0;
}

static bool AMem( drmem_hdl var, int index, void *d );
static bool AInherit( drmem_hdl inh, int index, void *d );

static DRWLKBLK StrucWlk[DR_WLKBLK_STRUCT] = {
    AMem,
    AInherit,
    AMem,
    AMem,
    NULL
};

static void SetSymHandle( type_wlk *d, imp_sym_handle  *is ){
// Set is with info from com
    is->sclass = d->com.sclass;
    is->im = d->com.im;
    is->state = DF_NOT;
    if( d->com.sclass == SYM_ENUM ){
        is->f.einfo = d->com.einfo;
    }else{
        is->f.minfo.root = d->com.root;
        is->f.minfo.inh = d->com.inh;
    }
}

static bool AMem( drmem_hdl var, int index, void *_d )
/****************************************************/
{
    type_wlk_wlk    *d = _d;
    bool            cont;
    imp_sym_handle  *is;
    dr_dbg_handle   saved;

    cont = true;
    is = d->is;
    SetSymHandle( (type_wlk *)d, is );
    is->sym = var;
    switch( index ){
    case 0:
        is->sclass = SYM_MEM;
        break;
    case 2:
        is->sclass = SYM_MEMVAR;      // static member
        break;
    case 3:
        if( DRGetVirtuality( var ) == DR_VIRTUALITY_VIRTUAL  ){
            is->sclass = SYM_VIRTF;   // virtual func
        }else if( !DRIsSymDefined( var ) ){
            is->sclass = SYM_MEMF;    // memfunc decl
        }else{
           is->sclass = SYM_MEMVAR;   // inlined defn treat like a var
        }
        break;
    }
    saved = DRGetDebug();
    d->wr = d->wk( d->com.ii, SWI_SYMBOL, is, d->com.d );
    DRSetDebug( saved );
    if( d->wr != WR_CONTINUE ){
        cont = false;
    }
    return( cont );
}

static bool AInherit( drmem_hdl inh, int index, void *_d )
/********************************************************/
{
//TODO: Need to track virtual base as not to visit same place twice
    type_wlk_wlk    *d = _d;
    bool            cont;
    drmem_hdl       btype;
    drmem_hdl       old_inh;
    imp_sym_handle  *is;
    dr_dbg_handle   saved;
    walk_result     wr;

    index = index;
    cont = true;

    btype = DRGetTypeAT( inh );
    btype =  DRSkipTypeChain( btype ); /* skip modifiers and typedefs */
    if( DRGetVirtuality( inh ) == DR_VIRTUALITY_VIRTUAL  ){
        if( !AddBase( btype, &d->com.vbase ) ){
            return( cont );
        }
    }
    is = d->is;
    SetSymHandle( (type_wlk *)d, is );
    is->sym = inh;
    is->sclass = SYM_MEM;     //  treat inherit like a var
    saved = DRGetDebug();
    wr = d->wk( d->com.ii, SWI_INHERIT_START, is, d->com.d );
    DRSetDebug( saved );
    if( wr == WR_CONTINUE ) {
        old_inh = d->com.inh;
        d->com.inh = inh;
        DRWalkStruct( btype, StrucWlk, d );
        d->com.inh = old_inh;
        saved = DRGetDebug();
        d->wk( d->com.ii, SWI_INHERIT_END, NULL, d->com.d );
        DRSetDebug( saved );
        if( d->wr != WR_CONTINUE ){
            cont = false;
        }
    }
    return( cont );
}


static bool AMemLookup( drmem_hdl var, int index, void *d );
static bool AInheritLookup( drmem_hdl inh, int index, void *d );

static DRWLKBLK StrucWlkLookup[DR_WLKBLK_STRUCT] = {
    AMemLookup,
    AInheritLookup,
    AMemLookup,
    AMemLookup,
    NULL
};

static bool AMemLookup( drmem_hdl var, int index, void *_d )
/**********************************************************/
{
    type_wlk_lookup *d = _d;
    imp_sym_handle  *is;
    char            *name;
    size_t          len;

    name =  DRGetName( var );
    if( name == NULL ){
        DCStatus( DS_FAIL );
        return( false );
    }
    len = strlen( name );
    if( len == d->li->name.len && d->comp( name, d->li->name.start, len ) == 0 ) {
        is = DCSymCreate( d->com.ii, d->com.d );
        SetSymHandle( (type_wlk *)d, is );
        is->sym = var;
        switch( index ){
        case 0:
            is->sclass = SYM_MEM;
            break;
        case 2:
            is->sclass = SYM_MEMVAR;     // static member
            break;
        case 3:
            if( DRGetVirtuality( var ) == DR_VIRTUALITY_VIRTUAL  ){
                is->sclass = SYM_VIRTF;   // virtual func
            }else if( !DRIsSymDefined( var ) ){
                is->sclass = SYM_MEMF;    // memfunc decl
            }else{
                is->sclass = SYM_MEMVAR;   // inlined defn treat like a var
            }
            break;
        }
        d->sr = SR_EXACT;
    }
    DCFree( name );
    return( true );
}

static bool AInheritLookup( drmem_hdl inh, int index, void *_d )
/**************************************************************/
//Push inherit handle and search
{
    type_wlk_lookup *d = _d;
    drmem_hdl btype;
    drmem_hdl old_inh;

    index = index;
    btype = DRGetTypeAT( inh );
    btype = DRSkipTypeChain( btype ); /* skip modifiers and typedefs */
    if( DRGetVirtuality( inh ) == DR_VIRTUALITY_VIRTUAL ){
        if( !AddBase( btype, &d->com.vbase ) ){
            return( true );
        }
    }
    old_inh = d->com.inh;
    d->com.inh = inh;
    DRWalkStruct( btype, StrucWlkLookup, d );
    d->com.inh = old_inh;
    return( true );
}

static bool AEnumMem( drmem_hdl var, int index, void *_d )
/********************************************************/
{
    type_wlk_wlk    *d = _d;
    bool            cont;
    imp_sym_handle  *is;
    dr_dbg_handle   saved;

    index = index;
    cont = true;
    is = d->is;
    SetSymHandle( (type_wlk *)d, is );
    is->sym = var;
    saved = DRGetDebug();
    d->wr = d->wk( d->com.ii, SWI_SYMBOL, is, d->com.d );
    DRSetDebug( saved );
    if( d->wr != WR_CONTINUE ){
        cont = false;
    }
    return( cont );
}

static bool AEnumMemLookup( drmem_hdl var, int index, void *_d )
/**************************************************************/
{
    type_wlk_lookup *d = _d;
    imp_sym_handle  *is;
    char            *name;
    size_t          len;

    index = index;
    name =  DRGetName( var );
    if( name == NULL ){
        DCStatus( DS_FAIL );
        return( false );
    }
    len = strlen( name );
    if( len == d->li->name.len && d->comp( name, d->li->name.start, len ) == 0 ) {
        is = DCSymCreate( d->com.ii, d->com.d );
        SetSymHandle( (type_wlk *)d, is );
        is->sym = var;
        d->sr = SR_EXACT;
    }
    DCFree( name );
    return( true );
}

extern walk_result WalkTypeSymList( imp_image_handle *ii, imp_type_handle *it,
                 IMP_SYM_WKR *wk, imp_sym_handle *is, void *d ){
    drmem_hdl       btype;
    type_wlk_wlk    df;
    df_cleaner      cleanup;

    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    if( it->state == DF_NOT ){
        if(  DRGetTypeInfo( it->type, &it->typeinfo ) ){
            it->state = DF_SET;
        }
    }
    if( it->state == DF_NOT ){
        return( WR_STOP );
    }
    df.com.im = it->im;
    df.com.ii = ii;
    df.com.d = d;
    df.com.root = it->type;
    df.com.inh = DRMEM_HDL_NULL;
    df.com.vbase = NULL;
    cleanup.rtn = FreeBases;   //push cleanup
    cleanup.d = &df.com.vbase;
    cleanup.prev = Cleaners;
    Cleaners = &cleanup;
    btype = DRSkipTypeChain( it->type );
    df.is = is;
    df.wk = wk;
    df.wr = WR_CONTINUE;
    switch( it->typeinfo.kind ){
    case DR_TYPEK_ENUM:
        df.com.sclass = SYM_ENUM;
        df.com.einfo.size = it->typeinfo.size;
        df.com.einfo.sign = it->typeinfo.modifier.sign;
        DRWalkEnum( btype, AEnumMem, &df );
        break;
    case DR_TYPEK_STRUCT:
    case DR_TYPEK_UNION:
    case DR_TYPEK_CLASS:
        df.com.sclass = SYM_MEM;
        DRWalkStruct( btype, StrucWlk, &df );
        break;
    default:
        DCStatus( DS_ERR | DS_BAD_PARM );
        df.wr = WR_STOP;
    }
    FreeBases( &df.com.vbase ); // free virtual base list
    Cleaners = cleanup.prev; // pop cleanup
    return( df.wr );
}

search_result SearchMbr( imp_image_handle *ii, imp_type_handle *it, lookup_item *li, void *d )
//Search for matching lookup item
{
    drmem_hdl       btype;
    type_wlk_lookup df;
    df_cleaner      cleanup;

    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    if( it->state == DF_NOT ){
        if( DRGetTypeInfo( it->type, &it->typeinfo ) ){
            it->state = DF_SET;
        }
    }
    if( it->state == DF_NOT ){
        return( SR_NONE );
    }
    df.com.im = it->im;
    df.com.ii = ii;
    df.com.d = d;
    df.com.root = it->type;
    df.com.inh = DRMEM_HDL_NULL;
    df.com.vbase = NULL;
    cleanup.rtn = FreeBases;   //push cleanup
    cleanup.d = &df.com.vbase;
    cleanup.prev = Cleaners;
    Cleaners = &cleanup;
    btype = DRSkipTypeChain( it->type );
    if( li->case_sensitive ) {
        df.comp = memcmp;
    } else {
        df.comp = memicmp;
    }
    df.li = li;
    df.sr = SR_NONE;
    switch( it->typeinfo.kind ){
    case DR_TYPEK_ENUM:
        df.com.sclass = SYM_ENUM;
        df.com.einfo.size = it->typeinfo.size;
        df.com.einfo.sign = it->typeinfo.modifier.sign;
        DRWalkEnum( btype, AEnumMemLookup, &df );
        break;
    case DR_TYPEK_STRUCT:
    case DR_TYPEK_UNION:
    case DR_TYPEK_CLASS:
        df.com.sclass = SYM_MEM;
        DRWalkStruct( btype, StrucWlkLookup, &df );
        break;
    default:
        DCStatus( DS_ERR | DS_BAD_PARM );
        df.sr = SR_FAIL;
    }
    FreeBases( &df.com.vbase ); // free virtual base list
    Cleaners = cleanup.prev; // pop cleanup
    return( df.sr );
}
/*********************************************/
/*  Search for a derived type then eval loc  */
/*********************************************/
typedef struct inh_path {
    struct inh_path *next;
    drmem_hdl        inh;
}inh_path;

typedef struct type_wlk_inherit {
    imp_image_handle *ii;
    drmem_hdl         dr_derived;
    location_context *lc;
    address          *addr;
    inh_path         *head;
    inh_path         **lnk;
    dip_status       wr;
    bool             cont;
}type_wlk_inherit;

static bool AInhFind( drmem_hdl inh, int index, void *df );

static DRWLKBLK InheritWlk[DR_WLKBLK_STRUCT] = {
    NULL,
    AInhFind,
    NULL,
    NULL,
    NULL,
};


static bool AInhFind( drmem_hdl inh, int index, void *_df )
/*************************************************/
//Push inherit handle and search
{
    type_wlk_inherit *df = _df;
    drmem_hdl       dr_derived;
    inh_path        head, *curr, **old_lnk;

    index = index;
    head.inh = inh;
    head.next = NULL;
    old_lnk = df->lnk;
    *df->lnk  = &head;
    df->lnk = &head.next;
    dr_derived = DRGetTypeAT( inh );        /* get base type */
    if( dr_derived == df->dr_derived ){
        for( curr = df->head; curr != NULL; curr = curr->next ) {
            df->wr = EvalLocAdj( df->ii, df->lc, curr->inh, df->addr );
            if( df->wr != DS_OK ) {
                break;
            }
        }
        df->cont = false;
    } else {
        dr_derived =  DRSkipTypeChain( dr_derived); /* skip modifiers and typedefs */
        DRWalkStruct( dr_derived, InheritWlk, df ); /* walk struct looking for inheritance */
    }
    df->lnk = old_lnk;
    return( df->cont );
}

extern dip_status  DFBaseAdjust( imp_image_handle *ii,
                                drmem_hdl base, drmem_hdl derived,
                                location_context *lc, address *addr ){
    type_wlk_inherit df;
    drmem_hdl        dr_base;

    df.ii = ii;
    df.dr_derived = derived;
    df.lc =  lc;
    df.addr = addr;
    df.head = NULL;
    df.lnk  = &df.head;
    df.wr = DS_FAIL;
    df.cont = true;
    dr_base =  DRSkipTypeChain( base );   /* skip modifiers and typedefs */
    DRWalkStruct( dr_base, InheritWlk, &df ); /* walk struct looking for inheritance */
    return( df.wr );
}

dip_status      DIGENTRY DIPImpTypeThunkAdjust( imp_image_handle *ii,
                        imp_type_handle *base, imp_type_handle *derived,
                        location_context *lc, address *addr )
{
    /*
        When you convert a pointer to a C++ class to a pointer at one
        of its derived classes you have to adjust the pointer so that
        it points at the start of the derived class. The 'derived' type
        may not actually be a derived type of 'base'. In that case, return
        DS_FAIL and nothing to 'addr'. If it is a derived type, let 'disp'
        be the displacement between the 'base' type and the 'derived' type.
        You need to do the following. "addr->mach.offset += disp;".
    */
    return( DFBaseAdjust( ii, base->type, derived->type, lc, addr ) );
}

size_t DIGENTRY DIPImpTypeName( imp_image_handle *ii, imp_type_handle *it,
                unsigned num, symbol_type *tag, char *buff, size_t buff_size )
{
    /*
        Given the imp_type_handle, copy the name of the type into 'buff'.
        Do not copy more than 'buff_size' - 1 characters into the buffer and
        append a trailing '\0' character. Return the real length
        of the type name (not including the trailing '\0' character) even
        if you had to truncate it to fit it into the buffer. If something
        went wrong and you can't get the type name, call DCStatus and
        return zero. NOTE: the client might pass in zero for 'buff_size'. In that
        case, just return the length of the module name and do not attempt
        to put anything into the buffer.

        Since there can be a "string" of typedef names associated with
        a type_handle, the 'num' parm indicates which one of the names
        the client wants returned. Zero is the first type name, one is
        the second, etc. Fill in '*tag' with ST_ENUM_TAG, ST_UNION_TAG,
        ST_STRUCT_TAG, ST_CLASS_TAG if the name is a enum, union, struct,
        or class tag name respectively. If not, set '*tag' to ST_NONE.

        If the type does not have a name, return zero.
    */
    char        *name = NULL;
    drmem_hdl   dr_type;
    dr_typeinfo typeinfo;
    size_t      len;

    DRSetDebug( ii->dwarf->handle ); /* must do at each call into dwarf */
    ++num;
    len = 0;
    for( dr_type = it->type; dr_type != DRMEM_HDL_NULL; dr_type = DRGetTypeAT( dr_type ) ) {
        name =  DRGetName( dr_type );
        if( name != NULL ){
            if( --num == 0 )
                break;
            DCFree( name );
        }
    }
    if( num == 0 ){
        DRGetTypeInfo( dr_type, &typeinfo );
        switch( typeinfo.kind ){
        case DR_TYPEK_ENUM:
            *tag = ST_ENUM_TAG;
            break;
        case DR_TYPEK_STRUCT:
            *tag = ST_STRUCT_TAG;
            break;
        case DR_TYPEK_UNION:
            *tag = ST_UNION_TAG;
            break;
        case DR_TYPEK_CLASS:
            *tag = ST_CLASS_TAG;
            break;
        default:
            *tag = ST_NONE;
            break;
        }
        len = NameCopy( buff, name, buff_size, 0 );
        DCFree( name );
    }
    return( len );
}

dip_status DIGENTRY DIPImpTypeAddRef( imp_image_handle *ii, imp_type_handle *it )
{
    ii=ii;
    it=it;
    return(DS_OK);
}

dip_status DIGENTRY DIPImpTypeRelease( imp_image_handle *ii, imp_type_handle *it )
{
    ii=ii;
    it=it;
    return(DS_OK);
}

dip_status DIGENTRY DIPImpTypeFreeAll( imp_image_handle *ii )
{
    ii=ii;
    return(DS_OK);
}
