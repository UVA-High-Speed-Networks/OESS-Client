#include "config.h"

#include <stdio.h>
#include "gb2def.h"
#include "proto_gemlib.h"

static char wmocurrtable[LLMXLN];

/*
 * The following is declared here because it isn't declared elsewhere.
 */
extern void     ctb_g2rdvar(char *tbname, G2vars_t *vartbl, int *iret);

void     gb2_gtwmovartbl(char *wmovartbl, int iver, G2vars_t **g2vartbl,
                       int *iret)
/************************************************************************
 * gb2_gtwmovartbl							*
 *									*
 * This function reads the WMO GRIB2 parameter table from               *
 * specified file and returns a structure containing the table          *
 * entries.                                                             *
 *                                                                      *
 * If wmovartbl is NULL, the default table is read.                     *
 *									*
 * gb2_gtwmovartbl ( wmovartbl, iver, g2vartbl, iret )                  *
 *									*
 * Input parameters:							*
 *      *wmovartbl      char            WMO GRIB2 Parameter table       *
 *                                             filename                 *
 *      iver            int             WMO Table version number        *
 *									*
 * Output parameters:							*
 *	*g2vartbl	G2vars_t        structure for the table entries *
 *	*iret		int		Return code			*
 *                                        -31 = Error reading table     *
 **									*
 * Log:									*
 * S. Gilbert/NCEP		 08/2005				*
 ***********************************************************************/
{

    char tmpname[LLMXLN];
    int  ier;
    static G2vars_t currvartbl={0,0};

/*---------------------------------------------------------------------*/
    *iret = 0;

    /*
     *  Check if user supplied table.  If not, use default.
     */
    if ( strlen(wmovartbl) == (size_t)0 ) {
        sprintf( tmpname,"g2varswmo%d.tbl", iver );
    }
    else {
        strcpy( tmpname, wmovartbl );
    }

    /*
     *  Check if table has already been read in. 
     *  If different table, read new one in.
     */
    if ( strcmp( tmpname, wmocurrtable ) != 0 ) {
        if ( currvartbl.info != 0 ) {
            free(currvartbl.info);
            currvartbl.info=0;
            currvartbl.nlines=0;
        }
        ctb_g2rdvar( tmpname, &currvartbl, &ier );
        if ( ier != 0 ) {
            char        ctemp[256];

            currvartbl.nlines=0;
            *iret=-31;
            (void)sprintf(ctemp, "Couldn't open WMO GRIB2 parameter table: "
                    "\"%s\"", tmpname);
            ER_WMSG("GB",iret,ctemp,&ier,2,strlen(tmpname));
            *g2vartbl = &currvartbl;
            return;
        }
    }
    strcpy( wmocurrtable, tmpname );
    *g2vartbl = &currvartbl;

    /*
     *  Search through table for id.
    gb2_skvar( disc, cat, id, pdtn, &vartbl, g2var, &ier);
    if ( ier == -1 )*iret=-32;
     */

}

const char*
gb2_getwmocurrtable(void)
{
    return wmocurrtable;
}
