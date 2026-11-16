//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Placeholder for world/displacement physics collision output.
//
// The original implementation (ivp.cpp / disp_ivp.cpp) is not part of this
// source tree. Until it is rewritten, maps compiled by this vbsp have an empty
// LUMP_PHYSCOLLIDE: geometry, visibility and lighting work, but there is no
// world collision in game.
//
//=============================================================================

#include "vbsp.h"

void EmitPhysCollision()
{
	Warning( "WARNING: physics collision is not implemented in this vbsp build; "
		"the map will have no world collision (LUMP_PHYSCOLLIDE is empty).\n" );
}

void EmitWaterVolumesForBSP( dmodel_t *pModel, node_t *headnode )
{
	// Water physics volumes are part of the same missing implementation.
	(void)pModel;
	(void)headnode;
}

void DumpCollideToGlView( CPhysCollide *pCollide, const char *pFilename )
{
	// Debug output for -dumpstaticprops; needs the missing implementation.
	(void)pCollide;
	Warning( "DumpCollideToGlView: not implemented in this vbsp build (%s)\n", pFilename );
}
