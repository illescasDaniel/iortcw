/*
===========================================================================
Copyright (C) 2009-2011 Andrei Drexler, Richard Allen, James Canete

This file is part of Reaction source code.

Reaction source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Reaction source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Reaction source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifndef __TR_EXTRATYPES_H__
#define __TR_EXTRATYPES_H__

// tr_extratypes.h, for mods that want to extend tr_types.h without losing compatibility with original VMs

// extra refdef flags start at (1 << 7)
#define RDF_NOFOG		(1 << 7)		// don't apply fog to polys added using RE_AddPolyToScene
#define RDF_EXTRA		(1 << 8)		// Makro - refdefex_t to follow after refdef_t
#define RDF_SUNLIGHT    (1 << 9)      // SmileTheory - render sunlight and shadows
#ifndef RDF_NOSUNLIGHT
#define RDF_NOSUNLIGHT  (1 << 10)     // codmod: scripted cutscene — lite lighting mode
#endif

// r_cutsceneFx bits: features to KEEP during RDF_NOSUNLIGHT.
// Default 27 = sun+dlights+autoexp+rays. Bits: sun=1 dlight=2 ao=4 autoexp=8 rays=16 gamma=32
#define CUTSCENE_FX_SUN      1
#define CUTSCENE_FX_DLIGHT   2
#define CUTSCENE_FX_AO       4
#define CUTSCENE_FX_AUTOEXP  8
#define CUTSCENE_FX_RAYS     16
#define CUTSCENE_FX_GAMMA    32

typedef struct {
	float			blurFactor;
	float           sunDir[3];
	float           sunCol[3];
	float           sunAmbCol[3];
} refdefex_t;

#endif
