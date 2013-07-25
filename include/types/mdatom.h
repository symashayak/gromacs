/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.
 * Copyright (c) 2012,2013, by the GROMACS development team, led by
 * David van der Spoel, Berk Hess, Erik Lindahl, and including many
 * others, as listed in the AUTHORS file in the top-level source
 * directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

#ifndef _mdatom_h
#define _mdatom_h

#include "simple.h"

#ifdef __cplusplus
extern "C" {
#endif


#define  NO_TF_TABLE 255
#define  DEFAULT_TF_TABLE 0

typedef struct {
    real                   tmassA, tmassB, tmass;
    int                    nr;
    int                    nalloc;
    int                    nenergrp;
    gmx_bool               bVCMgrps;
    int                    nPerturbed;
    int                    nMassPerturbed;
    int                    nChargePerturbed;
    gmx_bool               bOrires;
    real                  *massA, *massB, *massT, *invmass;
    real                  *chargeA, *chargeB;
    gmx_bool              *bPerturbed;
    int                   *typeA, *typeB;
    unsigned short        *ptype;
    unsigned short        *cTC, *cENER, *cACC, *cFREEZE, *cVCM;
    unsigned short        *cU1, *cU2, *cORF;
    /* for QMMM, atomnumber contains atomic number of the atoms */
    gmx_bool              *bQM;
    /* The range of home atoms */
    int                    start;
    int                    homenr;
    /* The lambda value used to create the contents of the struct */
    real                   lambda;
    /* The AdResS weighting function */
    real                  *wf;
    unsigned short        *tf_table_index; /* The tf table that will be applied, if thermodyn, force enabled*/

  /* additions to compute local pressue in slab in z direction */
  real *z_bin; /* z positions of bin */

  real *pkin_slab;    /* avg kinetic local pressure vector */
  real *pvir_slab;    /* avg virial local pressure vector */

  real *pkin_zz_slab; /* kinetic local Pzz vector */
  real *pkin_xx_slab; /* kinetic local Pxx vector */
  real *pkin_yy_slab; /* kinetic local Pyy vector */
  real *pkin_xz_slab; /* kinetic local Pxz vector */
  real *pkin_yz_slab; /* kinetic local Pyz vector */

  real *pvir_zz_slab; /* virial local Pzz vector */
  real *pvir_xx_slab; /* virial local Pxx vector */
  real *pvir_yy_slab; /* virial local Pyy vector */
  real *pvir_xz_slab; /* virial local Pxz vector */
  real *pvir_yz_slab; /* virial local Pyz vector */

  real z_lp; /* domain length in z */
  int n_lp_bins; /* number of bins */
  real dz_lp_bin; /* size of a bin */
  real w_gauss; /* size of gaussian kernel variance */

} t_mdatoms;

#ifdef __cplusplus
}
#endif


#endif
