//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Physics collision (LUMP_PHYSCOLLIDE) for brush models.
//
// The original ivp.cpp / disp_ivp.cpp are not part of this source tree; this
// is a reimplementation that writes the same lump layout the engine and game
// read (see CollisionBSPData_LoadPhysics and PhysCreateWorld_Shared):
//
//   world (model 0):
//     staticsolid 0   MASK_SOLID brushes           (always present)
//     staticsolid 1   CONTENTS_PLAYERCLIP brushes  (if any)
//     staticsolid 2   CONTENTS_MONSTERCLIP brushes (if any)
//     fluid N...      one per water surface (water/slime brushes grouped by
//                     contents and top plane)
//     virtualterrain  if the map has displacements: the engine builds their
//                     collision at load time from the displacement data
//     materialtable   maps convex game data -> surfaceprop name
//   brush entities (model 1..n):
//     solid 0 with mass from volume * surfaceprop density
//
// Each brush becomes one convex (from its planes). The convex's game data is
// its material table index, taken from the brush's dominant surfaceprop.
//
//=============================================================================

#include "vbsp.h"
#include "physdll.h"
#include "vphysics_interface.h"
#include "tier1/utlbuffer.h"
#include "tier1/utlvector.h"
#include "tier1/strtools.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IPhysicsSurfaceProps *physprops;
extern int g_SurfaceProperties[MAX_MAP_TEXDATA];

static IPhysicsCollision *s_pPhysCollision = NULL;

// Brushes that are merged within this distance are treated as one vertex.
#define CONVEX_MERGE_DISTANCE	0.01f

// Valve's density units are kg/m^3, volumes are in cubic inches.
#define MIN_BRUSH_MODEL_MASS	1.0f

//-----------------------------------------------------------------------------
// Material table: convex game data index -> surfaceprop.
// Index 0 is left as the identity mapping (surface 0); entries start at 1.
//-----------------------------------------------------------------------------
class CPhysMaterialTable
{
public:
	int GetIndex( int surfaceProp )
	{
		if ( surfaceProp < 0 )
			surfaceProp = physprops ? physprops->GetSurfaceIndex( "default" ) : 0;

		int i = m_SurfaceProps.Find( surfaceProp );
		if ( i == m_SurfaceProps.InvalidIndex() )
		{
			if ( m_SurfaceProps.Count() >= 127 )
			{
				static bool s_bWarned = false;
				if ( !s_bWarned )
				{
					Warning( "Physics material table is full (127 surfaceprops); extra materials use the last entry.\n" );
					s_bWarned = true;
				}
				return m_SurfaceProps.Count();
			}
			i = m_SurfaceProps.AddToTail( surfaceProp );
		}
		return i + 1;
	}

	void WriteKeyValues( CUtlBuffer &buf ) const
	{
		buf.Printf( "materialtable {\n" );
		for ( int i = 0; i < m_SurfaceProps.Count(); ++i )
		{
			const char *pName = physprops ? physprops->GetPropName( m_SurfaceProps[i] ) : NULL;
			buf.Printf( "\"%s\" \"%d\"\n", pName ? pName : "default", i + 1 );
		}
		buf.Printf( "}\n" );
	}

private:
	CUtlVector<int> m_SurfaceProps;
};

//-----------------------------------------------------------------------------
// Surfaceprop of a brush side (-1 if unknown).
//-----------------------------------------------------------------------------
static int SideSurfaceProp( const side_t *pSide )
{
	if ( pSide->texinfo < 0 || pSide->texinfo >= texinfo.Count() )
		return -1;
	int texdata = texinfo[pSide->texinfo].texdata;
	if ( texdata < 0 || texdata >= MAX_MAP_TEXDATA )
		return -1;
	return g_SurfaceProperties[texdata];
}

static bool IsToolSide( const side_t *pSide )
{
	if ( pSide->texinfo < 0 || pSide->texinfo >= texinfo.Count() )
		return true;
	return ( texinfo[pSide->texinfo].flags & ( SURF_NODRAW | SURF_SKIP | SURF_HINT ) ) != 0;
}

static float SideArea( const side_t *pSide )
{
	return pSide->winding ? WindingArea( pSide->winding ) : 0.0f;
}

//-----------------------------------------------------------------------------
// Area-weighted surfaceprop votes over brush sides. Visible sides win over
// tool sides (nodraw/skip/hint), which are often buried against other brushes.
//-----------------------------------------------------------------------------
class CSurfacePropVote
{
public:
	void AddBrush( const mapbrush_t *pBrush )
	{
		for ( int i = 0; i < pBrush->numsides; ++i )
		{
			const side_t *pSide = &pBrush->original_sides[i];
			if ( pSide->bevel )
				continue;
			int prop = SideSurfaceProp( pSide );
			if ( prop < 0 )
				continue;
			// Small floor so zero-area (or unwound) sides still count.
			float area = SideArea( pSide ) + 1.0f;
			Add( IsToolSide( pSide ) ? m_Tool : m_Visible, prop, area );
		}
	}

	int Winner() const
	{
		int prop = Best( m_Visible );
		if ( prop < 0 )
			prop = Best( m_Tool );
		return prop;
	}

private:
	struct Entry_t { int prop; float weight; };

	static void Add( CUtlVector<Entry_t> &list, int prop, float weight )
	{
		for ( int i = 0; i < list.Count(); ++i )
		{
			if ( list[i].prop == prop )
			{
				list[i].weight += weight;
				return;
			}
		}
		Entry_t e = { prop, weight };
		list.AddToTail( e );
	}

	static int Best( const CUtlVector<Entry_t> &list )
	{
		int best = -1;
		float bestWeight = -1.0f;
		for ( int i = 0; i < list.Count(); ++i )
		{
			if ( list[i].weight > bestWeight )
			{
				bestWeight = list[i].weight;
				best = list[i].prop;
			}
		}
		return best;
	}

	CUtlVector<Entry_t> m_Visible;
	CUtlVector<Entry_t> m_Tool;
};

//-----------------------------------------------------------------------------
// One convex per brush, from its (non-bevel) planes.
//-----------------------------------------------------------------------------
static CPhysConvex *ConvexFromBrush( const mapbrush_t *pBrush, unsigned int gameData )
{
	CUtlVector<float> planes;
	planes.EnsureCapacity( pBrush->numsides * 4 );
	for ( int i = 0; i < pBrush->numsides; ++i )
	{
		const side_t *pSide = &pBrush->original_sides[i];
		if ( pSide->bevel )
			continue;
		const plane_t &plane = g_MainMap->mapplanes[pSide->planenum];
		planes.AddToTail( plane.normal.x );
		planes.AddToTail( plane.normal.y );
		planes.AddToTail( plane.normal.z );
		planes.AddToTail( plane.dist );
	}

	if ( planes.Count() < 4 * 4 )
		return NULL;

	CPhysConvex *pConvex = s_pPhysCollision->ConvexFromPlanes( planes.Base(), planes.Count() / 4, CONVEX_MERGE_DISTANCE );
	if ( pConvex )
	{
		s_pPhysCollision->SetConvexGameData( pConvex, gameData );
	}
	else
	{
		Warning( "Brush %d (entity %d): couldn't build a physics convex, skipped.\n", pBrush->id, pBrush->entitynum );
	}
	return pConvex;
}

//-----------------------------------------------------------------------------
// A list of brushes that become one collide (solid) in the lump.
//-----------------------------------------------------------------------------
struct PhysSolid_t
{
	CUtlVector<const mapbrush_t *>	brushes;
	CPhysCollide					*pCollide;

	PhysSolid_t() : pCollide( NULL ) {}
};

static CPhysCollide *BuildCollide( const CUtlVector<const mapbrush_t *> &brushes, CPhysMaterialTable *pMaterials )
{
	CUtlVector<CPhysConvex *> convexes;
	for ( int i = 0; i < brushes.Count(); ++i )
	{
		unsigned int gameData = 0;
		if ( pMaterials )
		{
			CSurfacePropVote vote;
			vote.AddBrush( brushes[i] );
			gameData = pMaterials->GetIndex( vote.Winner() );
		}
		CPhysConvex *pConvex = ConvexFromBrush( brushes[i], gameData );
		if ( pConvex )
			convexes.AddToTail( pConvex );
	}

	if ( !convexes.Count() )
		return NULL;

	// Takes ownership of the convexes.
	return s_pPhysCollision->ConvertConvexToCollide( convexes.Base(), convexes.Count() );
}

//-----------------------------------------------------------------------------
// Serializes one model's collides + keydata as a dphysmodel_t record.
//-----------------------------------------------------------------------------
static void WritePhysModel( CUtlBuffer &out, int modelIndex, const CUtlVector<CPhysCollide *> &collides, const CUtlBuffer &keydata )
{
	int dataSize = 0;
	for ( int i = 0; i < collides.Count(); ++i )
	{
		dataSize += sizeof( int ) + s_pPhysCollision->CollideSize( collides[i] );
	}

	dphysmodel_t model;
	model.modelIndex = modelIndex;
	model.dataSize = dataSize;
	model.keydataSize = keydata.TellPut() + 1;	// include the terminating NUL
	model.solidCount = collides.Count();
	out.Put( &model, sizeof( model ) );

	for ( int i = 0; i < collides.Count(); ++i )
	{
		int size = s_pPhysCollision->CollideSize( collides[i] );
		out.PutInt( size );
		CUtlVector<char> data;
		data.SetCount( size );
		s_pPhysCollision->CollideWrite( data.Base(), collides[i] );
		out.Put( data.Base(), size );
	}

	out.Put( keydata.Base(), keydata.TellPut() );
	out.PutChar( 0 );
}

//-----------------------------------------------------------------------------
// Water: brushes grouped into one fluid per (contents, surface plane).
//-----------------------------------------------------------------------------
struct FluidVolume_t
{
	int								contents;
	Vector							surfaceNormal;
	float							surfaceDist;
	int								surfaceProp;
	CUtlVector<const mapbrush_t *>	brushes;
};

static const side_t *FindTopSide( const mapbrush_t *pBrush )
{
	const side_t *pTop = NULL;
	float bestZ = -2.0f;
	for ( int i = 0; i < pBrush->numsides; ++i )
	{
		const side_t *pSide = &pBrush->original_sides[i];
		if ( pSide->bevel )
			continue;
		float z = g_MainMap->mapplanes[pSide->planenum].normal.z;
		if ( z > bestZ )
		{
			bestZ = z;
			pTop = pSide;
		}
	}
	return pTop;
}

static void AddFluidBrush( CUtlVector<FluidVolume_t *> &fluids, const mapbrush_t *pBrush )
{
	const side_t *pTop = FindTopSide( pBrush );
	if ( !pTop )
		return;

	const plane_t &plane = g_MainMap->mapplanes[pTop->planenum];
	if ( plane.normal.z < 0.7f )
	{
		Warning( "Water brush %d has no upward-facing top surface; using its bounding box top.\n", pBrush->id );
	}
	Vector normal = ( plane.normal.z >= 0.7f ) ? plane.normal : Vector( 0, 0, 1 );
	float dist = ( plane.normal.z >= 0.7f ) ? plane.dist : pBrush->maxs.z;
	int contents = pBrush->contents & ~( CONTENTS_DETAIL );

	for ( int i = 0; i < fluids.Count(); ++i )
	{
		FluidVolume_t *pFluid = fluids[i];
		if ( pFluid->contents == contents &&
			 pFluid->surfaceNormal.DistToSqr( normal ) < 1e-4f &&
			 fabs( pFluid->surfaceDist - dist ) < 0.1f )
		{
			pFluid->brushes.AddToTail( pBrush );
			return;
		}
	}

	FluidVolume_t *pFluid = new FluidVolume_t;
	pFluid->contents = contents;
	pFluid->surfaceNormal = normal;
	pFluid->surfaceDist = dist;

	// The water material's surfaceprop, unless it's just the generic default.
	int prop = SideSurfaceProp( pTop );
	int defaultProp = physprops ? physprops->GetSurfaceIndex( "default" ) : -1;
	pFluid->surfaceProp = ( prop >= 0 && prop != defaultProp ) ? prop : -1;
	pFluid->brushes.AddToTail( pBrush );
	fluids.AddToTail( pFluid );
}

//-----------------------------------------------------------------------------
// World model
//-----------------------------------------------------------------------------
static void EmitWorldPhysics( CUtlBuffer &lump, const entity_t *pWorld )
{
	CPhysMaterialTable materials;

	static const int s_SolidMasks[] = { MASK_SOLID, CONTENTS_PLAYERCLIP, CONTENTS_MONSTERCLIP };
	PhysSolid_t solids[ARRAYSIZE( s_SolidMasks )];
	CUtlVector<FluidVolume_t *> fluids;

	for ( int i = 0; i < pWorld->numbrushes; ++i )
	{
		const mapbrush_t *pBrush = &g_MainMap->mapbrushes[pWorld->firstbrush + i];
		if ( pBrush->numsides <= 0 )
			continue;

		if ( pBrush->contents & ( CONTENTS_WATER | CONTENTS_SLIME ) )
		{
			AddFluidBrush( fluids, pBrush );
			continue;
		}

		for ( int s = 0; s < ARRAYSIZE( s_SolidMasks ); ++s )
		{
			if ( pBrush->contents & s_SolidMasks[s] )
				solids[s].brushes.AddToTail( pBrush );
		}
	}

	CUtlVector<CPhysCollide *> collides;
	CUtlBuffer keydata( 0, 0, CUtlBuffer::TEXT_BUFFER );

	for ( int s = 0; s < ARRAYSIZE( s_SolidMasks ); ++s )
	{
		solids[s].pCollide = BuildCollide( solids[s].brushes, &materials );
		if ( !solids[s].pCollide )
		{
			if ( s == 0 )
				Warning( "World has no solid brushes for physics collision!\n" );
			// Solid 0 must exist: the game uses solids[0] as the world object.
			if ( s != 0 )
				continue;

			// Fall back to a tiny box far outside the playable space.
			Vector mins( MIN_COORD_INTEGER, MIN_COORD_INTEGER, MIN_COORD_INTEGER );
			solids[s].pCollide = s_pPhysCollision->BBoxToCollide( mins, mins + Vector( 1, 1, 1 ) );
		}

		keydata.Printf( "staticsolid {\n\"index\" \"%d\"\n\"contents\" \"%d\"\n}\n", collides.Count(), s_SolidMasks[s] );
		collides.AddToTail( solids[s].pCollide );
	}

	for ( int f = 0; f < fluids.Count(); ++f )
	{
		FluidVolume_t *pFluid = fluids[f];
		CPhysCollide *pCollide = BuildCollide( pFluid->brushes, NULL );
		if ( pCollide )
		{
			const char *pSurfaceProp = ( pFluid->surfaceProp >= 0 && physprops ) ? physprops->GetPropName( pFluid->surfaceProp ) : "water";
			// surfaceplane is "nx ny nz dist" (plane: dot(n, p) = dist), as in retail maps.
			keydata.Printf( "fluid {\n\"index\" \"%d\"\n\"surfaceprop\" \"%s\"\n\"damping\" \"0.010000\"\n\"contents\" \"%d\"\n"
				"\"surfaceplane\" \"%f %f %f %f \"\n\"currentvelocity\" \"0.000000 0.000000 0.000000 \"\n}\n",
				collides.Count(), pSurfaceProp ? pSurfaceProp : "water", pFluid->contents,
				pFluid->surfaceNormal.x, pFluid->surfaceNormal.y, pFluid->surfaceNormal.z, pFluid->surfaceDist );
			collides.AddToTail( pCollide );
		}
		delete pFluid;
	}

	// Displacement collision is built by the engine at load time (virtual mesh).
	if ( g_dispinfo.Count() > 0 && !g_bNoVirtualMesh )
	{
		keydata.Printf( "virtualterrain {}\n" );
	}

	materials.WriteKeyValues( keydata );

	WritePhysModel( lump, 0, collides, keydata );

	for ( int i = 0; i < collides.Count(); ++i )
		s_pPhysCollision->DestroyCollide( collides[i] );
}

//-----------------------------------------------------------------------------
// Brush entity models
//-----------------------------------------------------------------------------
static void EmitBrushEntityPhysics( CUtlBuffer &lump, int modelIndex, const entity_t *pEntity )
{
	static const int s_UsefulContents = MASK_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP |
		CONTENTS_WATER | CONTENTS_SLIME | CONTENTS_LADDER;

	CUtlVector<const mapbrush_t *> brushes;
	CUtlVector<const mapbrush_t *> otherBrushes;
	CSurfacePropVote vote;
	for ( int i = 0; i < pEntity->numbrushes; ++i )
	{
		const mapbrush_t *pBrush = &g_MainMap->mapbrushes[pEntity->firstbrush + i];
		if ( pBrush->numsides <= 0 )
			continue;
		if ( pBrush->contents & s_UsefulContents )
			brushes.AddToTail( pBrush );
		else if ( pBrush->contents )
			otherBrushes.AddToTail( pBrush );
		vote.AddBrush( pBrush );
	}
	if ( !brushes.Count() )
		brushes.AddVectorToTail( otherBrushes );

	int surfaceProp = vote.Winner();
	if ( surfaceProp < 0 && physprops )
		surfaceProp = physprops->GetSurfaceIndex( "default" );

	// Brush entities use a single material for the whole model.
	CPhysCollide *pCollide = BuildCollide( brushes, NULL );
	if ( !pCollide )
		return;

	float volume = s_pPhysCollision->CollideVolume( pCollide );
	float density = 2000.0f, thickness, friction, elasticity;
	if ( physprops && surfaceProp >= 0 )
	{
		physprops->GetPhysicsProperties( surfaceProp, &density, &thickness, &friction, &elasticity );
	}
	float mass = volume * density * CUBIC_METERS_PER_CUBIC_INCH;
	mass = clamp( mass, MIN_BRUSH_MODEL_MASS, VPHYSICS_MAX_MASS );

	const char *pSurfaceProp = ( physprops && surfaceProp >= 0 ) ? physprops->GetPropName( surfaceProp ) : NULL;

	CUtlBuffer keydata( 0, 0, CUtlBuffer::TEXT_BUFFER );
	keydata.Printf( "solid {\n\"index\" \"0\"\n\"mass\" \"%f\"\n\"surfaceprop\" \"%s\"\n\"volume\" \"%f\"\n}\n",
		mass, pSurfaceProp ? pSurfaceProp : "default", volume );

	CUtlVector<CPhysCollide *> collides;
	collides.AddToTail( pCollide );
	WritePhysModel( lump, modelIndex, collides, keydata );
	s_pPhysCollision->DestroyCollide( pCollide );
}

//-----------------------------------------------------------------------------
// Entry point (called from EndBSPFile, after all models are processed)
//-----------------------------------------------------------------------------
void EmitPhysCollision()
{
	CreateInterfaceFn physicsFactory = GetPhysicsFactory();
	if ( physicsFactory )
	{
		s_pPhysCollision = (IPhysicsCollision *)physicsFactory( VPHYSICS_COLLISION_INTERFACE_VERSION, NULL );
	}
	if ( !s_pPhysCollision )
	{
		Warning( "!!! WARNING: Can't build collision data: vphysics not loaded!\n" );
		return;
	}

	CUtlBuffer lump;

	// Models are numbered in entity order, skipping entities without brushes;
	// this must match ProcessModels().
	int modelIndex = 0;
	for ( int e = 0; e < num_entities; ++e )
	{
		const entity_t *pEntity = &entities[e];
		if ( !pEntity->numbrushes )
			continue;

		if ( modelIndex == 0 )
			EmitWorldPhysics( lump, pEntity );
		else
			EmitBrushEntityPhysics( lump, modelIndex, pEntity );

		++modelIndex;
	}

	if ( modelIndex != nummodels )
	{
		Warning( "EmitPhysCollision: model count mismatch (%d vs %d); collision may be attached to the wrong models!\n", modelIndex, nummodels );
	}

	// Terminator
	dphysmodel_t end;
	end.modelIndex = -1;
	end.dataSize = -1;
	end.keydataSize = 0;
	end.solidCount = 0;
	lump.Put( &end, sizeof( end ) );

	if ( g_pPhysCollide )
	{
		free( g_pPhysCollide );
	}
	g_PhysCollideSize = lump.TellPut();
	g_pPhysCollide = (byte *)malloc( g_PhysCollideSize );
	memcpy( g_pPhysCollide, lump.Base(), g_PhysCollideSize );

	Msg( "Physics collision: %d models, %d bytes\n", modelIndex, g_PhysCollideSize );
}

//-----------------------------------------------------------------------------
// Water volumes are built from the water brushes in EmitPhysCollision; nothing
// needs to be collected from the BSP tree.
//-----------------------------------------------------------------------------
void EmitWaterVolumesForBSP( dmodel_t *pModel, node_t *headnode )
{
	(void)pModel;
	(void)headnode;
}

//-----------------------------------------------------------------------------
// Debug output for -dumpstaticprops (not implemented).
//-----------------------------------------------------------------------------
void DumpCollideToGlView( CPhysCollide *pCollide, const char *pFilename )
{
	(void)pCollide;
	Warning( "DumpCollideToGlView: not implemented in this vbsp build (%s)\n", pFilename );
}
