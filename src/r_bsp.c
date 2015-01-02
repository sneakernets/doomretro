/*
========================================================================

                               DOOM RETRO
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright (C) 2013-2015 Brad Harding.

  DOOM RETRO is a fork of CHOCOLATE DOOM by Simon Howard.
  For a complete list of credits, see the accompanying AUTHORS file.

  This file is part of DOOM RETRO.

  DOOM RETRO is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM RETRO is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM RETRO. If not, see <http://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM RETRO is in no way affiliated with nor endorsed by
  id Software LLC.

========================================================================
*/

#include <string.h>

#include "doomstat.h"
#include "m_bbox.h"
#include "r_main.h"
#include "r_plane.h"
#include "r_things.h"

seg_t           *curline;
side_t          *sidedef;
line_t          *linedef;
sector_t        *frontsector;
sector_t        *backsector;

int             doorclosed;

drawseg_t       *drawsegs;
unsigned int    maxdrawsegs;
drawseg_t       *ds_p;

void R_StoreWallRange(int start, int stop);

//
// R_ClearDrawSegs
//
void R_ClearDrawSegs(void)
{
    ds_p = drawsegs;
}

//
// ClipWallSegment
// Clips the given range of columns
// and includes it in the new clip list.
//
typedef struct
{
    int         first;
    int         last;
} cliprange_t;

// 1/11/98: Lee Killough
//
// This fixes many strange venetian blinds crashes, which occurred when a scan
// line had too many "posts" of alternating non-transparent and transparent
// regions. Using a doubly-linked list to represent the posts is one way to
// do it, but it has increased overhead and poor spatial locality, which hurts
// cache performance on modern machines. Since the maximum number of posts
// theoretically possible is a function of screen width, a static limit is
// okay in this case. It used to be 32, which was way too small.
//
// This limit was frequently mistaken for the visplane limit in some Doom
// editing FAQs, where visplanes were said to "double" if a pillar or other
// object split the view's space into two pieces horizontally. That did not
// have anything to do with visplanes, but it had everything to do with these
// clip posts.

#define MAXSEGS (SCREENWIDTH / 2 + 1)

// newend is one past the last valid seg
static cliprange_t      *newend;
static cliprange_t      solidsegs[MAXSEGS];

//
// R_ClipSolidWallSegment
// Does handle solid walls,
//  e.g. single sided LineDefs (middle texture)
//  that entirely block the view.
//
static void R_ClipSolidWallSegment(int first, int last)
{
    cliprange_t *next;
    cliprange_t *start = solidsegs;

    // Find the first range that touches the range
    //  (adjacent pixels are touching).
    while (start->last < first - 1)
        ++start;

    if (first < start->first)
    {
        if (last < start->first - 1)
        {
            // Post is entirely visible (above start), so insert a new clippost.
            R_StoreWallRange(first, last);

            // 1/11/98 killough: performance tuning using fast memmove
            memmove(start + 1, start, (++newend - start) * sizeof(*start));
            start->first = first;
            start->last = last;
            return;
        }

        // There is a fragment above *start.
        R_StoreWallRange(first, start->first - 1);

        // Now adjust the clip size.
        start->first = first;
    }

    // Bottom contained in start?
    if (last <= start->last)
        return;

    next = start;
    while (last >= (next + 1)->first - 1)
    {
        // There is a fragment between two posts.
        R_StoreWallRange(next->last + 1, (next + 1)->first - 1);
        ++next;

        if (last <= next->last)
        {
            // Bottom is contained in next. Adjust the clip size.
            start->last = next->last;
            goto crunch;
        }
    }

    // There is a fragment after *next.
    R_StoreWallRange(next->last + 1, last);

    // Adjust the clip size.
    start->last = last;

    // Remove start + 1 to next from the clip list,
    // because start now covers their area.

crunch:

    if (next == start)
        return;                 // Post just extended past the bottom of one post.

    while (next++ != newend)
        *(++start) = *next;     // Remove a post.

    newend = start;
}

//
// R_ClipPassWallSegment
// Clips the given range of columns,
//  but does not includes it in the clip list.
// Does handle windows,
//  e.g. LineDefs with upper and lower texture.
//
static void R_ClipPassWallSegment(int first, int last)
{
    cliprange_t *start = solidsegs;

    // Find the first range that touches the range
    //  (adjacent pixels are touching).
    while (start->last < first - 1)
        ++start;

    if (first < start->first)
    {
        if (last < start->first - 1)
        {
            // Post is entirely visible (above start).
            R_StoreWallRange(first, last);
            return;
        }

        // There is a fragment above *start.
        R_StoreWallRange(first, start->first - 1);
    }

    // Bottom contained in start?
    if (last <= start->last)
        return;

    while (last >= (start + 1)->first - 1)
    {
        // There is a fragment between two posts.
        R_StoreWallRange(start->last + 1, (start + 1)->first - 1);
        ++start;

        if (last <= start->last)
            return;
    }

    // There is a fragment after *next.
    R_StoreWallRange(start->last + 1, last);
}

//
// R_ClearClipSegs
//
void R_ClearClipSegs(void)
{
    solidsegs[0].first = INT_MIN + 1;
    solidsegs[0].last = -1;
    solidsegs[1].first = viewwidth;
    solidsegs[1].last = INT_MAX - 1;
    newend = solidsegs + 2;
}

// killough 1/18/98 -- This function is used to fix the automap bug which
// showed lines behind closed doors simply because the door had a dropoff.
//
// It assumes that Doom has already ruled out a door being closed because
// of front-back closure (e.g. front floor is taller than back ceiling).
int R_DoorClosed(void)
{
    return
        // if door is closed because back is shut:
        (backsector->ceilingheight <= backsector->floorheight

        // preserve a kind of transparent door/lift special effect:
        && (backsector->ceilingheight >= frontsector->ceilingheight 
            || curline->sidedef->toptexture)
        && (backsector->floorheight <= frontsector->floorheight
            || curline->sidedef->bottomtexture)

        // properly render skies (consider door "open" if both ceilings are sky):
        && (backsector->ceilingpic != skyflatnum
            || frontsector->ceilingpic != skyflatnum));
}

//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//
static void R_AddLine(seg_t *line)
{
    int         x1;
    int         x2;
    angle_t     angle1;
    angle_t     angle2;
    angle_t     span;
    angle_t     tspan;

    curline = line;

    angle1 = R_PointToAngle(line->v1->x, line->v1->y);
    angle2 = R_PointToAngle(line->v2->x, line->v2->y);

    // Clip to view edges.
    span = angle1 - angle2;

    // Back side? I.e. backface culling?
    if (span >= ANG180)
        return;

    // Global angle needed by segcalc.
    rw_angle1 = angle1;
    angle1 -= viewangle;
    angle2 -= viewangle;

    tspan = angle1 + clipangle;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;

        // Totally off the left edge?
        if (tspan >= span)
            return;

        angle1 = clipangle;
    }

    tspan = clipangle - angle2;
    if (tspan > 2 * clipangle)
    {
        tspan -= 2 * clipangle;

        // Totally off the left edge?
        if (tspan >= span)
            return;
        angle2 = 0 - clipangle;
    }

    // The seg is in the view range,
    // but not necessarily visible.
    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;

    // killough 1/31/98: Here is where "slime trails" can SOMETIMES occur:
    x1 = viewangletox[angle1];
    x2 = viewangletox[angle2];

    // Does not cross a pixel?
    if (x1 >= x2)
        return;

    backsector = line->backsector;

    doorclosed = 0;

    // Single sided line?
    if (!backsector)
        goto clipsolid;

    // Closed door.
    if (backsector->ceilingheight <= frontsector->floorheight
        || backsector->floorheight >= frontsector->ceilingheight)
        goto clipsolid;

    if ((doorclosed = R_DoorClosed()))
        goto clipsolid;

    // Window.
    if (backsector->ceilingheight != frontsector->ceilingheight
        || backsector->floorheight != frontsector->floorheight)
        goto clippass;

    // Reject empty lines used for triggers
    //  and special events.
    // Identical floor and ceiling on both sides,
    // identical light levels on both sides,
    // and no middle texture.
    if (backsector->ceilingpic == frontsector->ceilingpic
        && backsector->floorpic == frontsector->floorpic
        && backsector->lightlevel == frontsector->lightlevel
        && !curline->sidedef->midtexture)
        return;

clippass:
    R_ClipPassWallSegment(x1, x2 - 1);
    return;

clipsolid:
    R_ClipSolidWallSegment(x1, x2 - 1);
}

//
// R_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
static const int checkcoord[12][4] =
{
    { 3, 0, 2, 1 },
    { 3, 0, 2, 0 },
    { 3, 1, 2, 0 },
    { 0 },
    { 2, 0, 2, 1 },
    { 0, 0, 0, 0 },
    { 3, 1, 3, 0 },
    { 0 },
    { 2, 0, 3, 1 },
    { 2, 1, 3, 1 },
    { 2, 1, 3, 0 }
};

static boolean R_CheckBBox(const fixed_t *bspcoord)
{
    int         boxpos;
    const int   *check;

    angle_t     angle1;
    angle_t     angle2;

    cliprange_t *start;

    int         sx1;
    int         sx2;

    // Find the corners of the box
    // that define the edges from current viewpoint.
    boxpos = (viewx <= bspcoord[BOXLEFT] ? 0 : viewx < bspcoord[BOXRIGHT ] ? 1 : 2) +
             (viewy >= bspcoord[BOXTOP ] ? 0 : viewy > bspcoord[BOXBOTTOM] ? 4 : 8);

    if (boxpos == 5)
        return true;

    check = checkcoord[boxpos];

    // check clip list for an open space
    angle1 = R_PointToAngle(bspcoord[check[0]], bspcoord[check[1]]) - viewangle;
    angle2 = R_PointToAngle(bspcoord[check[2]], bspcoord[check[3]]) - viewangle;

    // cph - replaced old code, which was unclear and badly commented
    // Much more efficient code now
    if ((signed int)angle1 < (signed int)angle2) 
    {
        // Either angle1 or angle2 is behind us, so it doesn't matter if we
        // change it to the corect sign
        if (angle1 >= ANG180 && angle1 < ANG270)
            angle1 = INT_MAX;           // which is ANG180 - 1
        else
            angle2 = INT_MIN;
    }

    if ((signed int)angle2 >= (signed int)clipangle)
        return false;                   // Both off left edge
    if ((signed int)angle1 <= -(signed int)clipangle)
        return false;                   // Both off right edge
    if ((signed int)angle1 >= (signed int)clipangle)
        angle1 = clipangle;             // Clip at left edge
    if ((signed int)angle2 <= -(signed int)clipangle)
        angle2 = 0 - clipangle;         // Clip at right edge

    // Find the first clippost
    //  that touches the source post
    //  (adjacent pixels are touching).
    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    sx1 = viewangletox[angle1];
    sx2 = viewangletox[angle2];

    // SoM: To account for the rounding error of the old BSP system, I needed to
    // make adjustments.
    // SoM: Moved this to before the "does not cross a pixel" check to fix 
    // another slime trail
    if (sx1 > 0)
        sx1--;
    if (sx2 < viewwidth - 1)
        sx2++;

    // SoM: Removed the "does not cross a pixel" test

    start = solidsegs;
    while (start->last < sx2)
        ++start;

    if (sx1 >= start->first && sx2 <= start->last)
        return false;                   // The clippost contains the new span.

    return true;
}

//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
static void R_Subsector(int num)
{
    subsector_t *sub = &subsectors[num];
    int         count = sub->numlines;
    seg_t       *line = &segs[sub->firstline];

    frontsector = sub->sector;

    if (frontsector->floorheight < viewz)
        floorplane = R_FindPlane(frontsector->floorheight, frontsector->floorpic,
            frontsector->lightlevel);
    else
        floorplane = NULL;

    if (frontsector->ceilingheight > viewz || frontsector->ceilingpic == skyflatnum)
        ceilingplane = R_FindPlane(frontsector->ceilingheight, frontsector->ceilingpic,
            frontsector->lightlevel);
    else
        ceilingplane = NULL;

    R_AddSprites(frontsector);

    while (count--)
        R_AddLine(line++);
}

//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
void R_RenderBSPNode(int bspnum)
{
    while (!(bspnum & NF_SUBSECTOR))    // Found a subsector?
    {
        const node_t    *bsp = &nodes[bspnum];

        // Decide which side the view point is on.
        int             side = R_PointOnSide(viewx, viewy, bsp);

        // Recursively divide front space.
        R_RenderBSPNode(bsp->children[side]);

        // Possibly divide back space.
        if (!R_CheckBBox(bsp->bbox[side ^= 1]))
            return;

        bspnum = bsp->children[side];
    }
    R_Subsector(bspnum == -1 ? 0 : (bspnum & ~NF_SUBSECTOR));
}
