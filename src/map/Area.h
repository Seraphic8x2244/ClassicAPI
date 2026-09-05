// This file is part of ClassicAPI.
//
// ClassicAPI is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// ClassicAPI is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// ClassicAPI. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstdint>

// `Map::Area` — shared `WorldMapArea.dbc` row resolution. A WorldMapArea
// row is the client's per-zone map descriptor (directory name + the
// world-coordinate rect used for world↔map transforms); every C_Map.*
// reader that keys off "which zone" resolves a row first. Both
// `Map::Overlays` (GetMapOverlays) and `Map::WorldSize` (GetMapWorldSize)
// go through here so the two ways of naming a zone — an AreaTable areaID
// argument, or the world map's current continent/zone view — live in one
// place.
namespace Map::Area {

// WorldMapArea row for an AreaTable `areaID` (the stable zone identity
// `C_Map.GetBestMapForUnit` returns). Linear walk of the ~175-row table.
// Returns -1 when no row carries that areaID.
int RowForAreaID(uint32_t areaID);

// WorldMapArea row for the world map's currently *viewed* zone — the same
// continent/zone selection the engine's map-name resolver (FUN_004a6cf0,
// behind GetMapInfo) performs: continent index → per-continent blob, zone
// index (-1 = whole continent) → that continent's zone-row array; no
// continent selected falls back to the default-row global. Returns -1 when
// the continent blob isn't resident.
int CurrentViewRow();

// Resolves world point (x, y) on continent `mapID` to a zone: its AreaTable
// areaID and a 0..100 zone-relative percent (`mapX` horizontal off world Y,
// `mapY` vertical off world X — the WoW/pfQuest convention).
//
// WorldMapArea rects are loose, overlapping bounding boxes, so a point near a
// zone edge falls inside several. Resolution therefore prefers the zone whose
// **drawn landmass** (WorldMapOverlay hit rect) the point actually sits on —
// the runtime analog of an ADT area-grid lookup (Un'Goro's crater vs Thousand
// Needles' empty overlap). Overlay hit rects are themselves rectangles that
// overlap at borders, so among landmass matches the winner is the zone the
// point is DEEPEST inside an overlay of (Nijel's Point is deep in Desolace's
// own overlay but only clips the edge of Stonetalon's Charred Vale box). If the
// point is on no zone's overlay, falls back to the most-interior containing
// rect. Returns false when no zone rect encloses the point (open sea,
// degenerate rect, an instance with no zone row). Shared by GetAreaTriggerInfo
// and the taxi-node zone resolve.
bool ZonePercent(int mapID, float x, float y, int *outAreaID, double *outMapX,
                 double *outMapY);

// Projects world (x, y) into the WorldMapArea rect of a SPECIFIC AreaTable
// zone (`areaID`), filling the 0..100 zone-relative percent (`mapX` horizontal
// off world Y, `mapY` vertical off world X). Returns false when that zone has
// no usable WorldMapArea row OR the point lies outside its rect. Used when the
// zone is already known by other means (a taxi node's name) and only the
// in-zone position is needed — bypassing the containment / landmass search of
// ZonePercent. The containment check lets a caller test candidate zones (a
// node's city vs its outdoor region) and take the first that actually holds
// the point.
bool PercentInZone(int areaID, float x, float y, double *outMapX, double *outMapY);

// Projects world (x, y) on `mapID` to 0..1 CONTINENT-map coordinates using
// the continent's WorldMapArea row (the areaID == 0 row with a non-degenerate
// rect — some maps carry a stray zero-rect continent row that is skipped).
// `outPx` is horizontal (off world Y), `outPy` vertical (off world X).
// Returns false when the map has no usable continent row. This is the
// projection retail's TaxiNodeInfo.position uses.
bool ContinentPercent(int mapID, float x, float y, double *outPx, double *outPy);

// --- uiMapID identity ------------------------------------------------------
//
// A ClassicAPI `uiMapID` names either a zone or a whole map:
//   positive  = an `AreaTable.dbc` area id (a zone) — what
//               `C_Map.GetBestMapForUnit` returns.
//   negative  = -(WorldMapArea row) — continent / world / instance maps,
//               which carry no AreaTable id (their rows have areaID 0).
// The negative half mirrors the engine's own two-namespaces-in-one-int idiom
// (a quest's `zoneOrSort`: positive = AreaTable zone, negative = -QuestSort
// row), so the two id spaces can never collide.

// WorldMapArea row for a `uiMapID` in either namespace. Returns -1 when the
// id resolves to no row.
int RowForUiMapID(int uiMapID);

// The `uiMapID` that names WorldMapArea row `row` — its areaID when it has
// one, else -(row). Returns 0 for an unreadable row.
int UiMapIDForRow(int row);

// The continent-level WorldMapArea row for a `Map.dbc` map: the areaID == 0
// row with a non-degenerate rect (the same selection `ContinentPercent`
// makes, so the stray zero-rect "World" row never wins). -1 when none.
int ContinentRowForMapID(int mapID);

// Reads WorldMapArea row `row`'s placement rect and its `Map.dbc` map id.
// Every out-parameter is optional (pass null to skip it). False when the row
// is unreadable.
bool RowRect(int row, double *outLeft, double *outRight, double *outTop,
             double *outBottom, int *outMapID);

// Projects world (x, y) into row `row`'s rect as 0..1 (`outPx` horizontal off
// world Y, `outPy` vertical off world X). Unlike `PercentInZone` there is NO
// containment gate — a point outside the rect yields a value outside 0..1, and
// the caller decides whether that is meaningful. False for a missing or
// degenerate rect.
bool PercentInRow(int row, float x, float y, double *outPx, double *outPy);

// Inverse of `PercentInRow`: a 0..1 position on row `row` back to
// continent-space world coordinates. False for a missing or degenerate rect.
bool WorldFromRow(int row, double px, double py, double *outX, double *outY);

} // namespace Map::Area
