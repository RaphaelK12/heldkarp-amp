/* -----------------------------------------------------------------------------

	Copyright (c) 2012 Niels Fr�hling              niels@paradice-insight.us

	Permission is hereby granted, free of charge, to any person obtaining
	a copy of this software and associated documentation files (the
	"Software"), to	deal in the Software without restriction, including
	without limitation the rights to use, copy, modify, merge, publish,
	distribute, sublicense, and/or sell copies of the Software, and to
	permit persons to whom the Software is furnished to do so, subject to
	the following conditions:

	The above copyright notice and this permission notice shall be included
	in all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
	OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
	CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
	TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   -------------------------------------------------------------------------- */

#include <assert.h>
#include <ppl.h>
#include <amp.h>
#include <amp_graphics.h>
#include <amp_short_vectors.h>

#include "../helpers/amp/amp_short_arrays.h"
//nclude "../helpers/amp/amp_packed_arrays.h"

using namespace Concurrency;
using namespace Concurrency::graphics;

/* number of possible parallel branches, this is clamped
 * by the minimum-spanning tree connectivity and by the
 * number of cores
 */
#define PARALLEL_BRANCHES	32

#include "SSE2.h"
#include "AMP.h"
#include "AMP-CM-Opt.h"

/* ********************************************************************************************
 */
namespace tsp_amp_ccrcm_optcfx {

/* 4-way vectors
 * double access-alignment (for reduction)
 */
const int SIMDsize		= 4;	// in elements (for passed-in memory allocations)
const int AccessAlignment	= 8;	// in elements (for locally allocated memory only)
const int MemoryAlignment	= 16;	// in bytes

/* just a marker to know what's up */
#define amp_uav
#define amp_srv

/* execute a block of code only once,
 * use thread "0" for that
 */
#define singlethread(n, func)					\
  if ((tileheight == 1) || !th.local[0]) {			\
    if ((n) == th.local[1])	{				\
      const int j = (n);					\
      func							\
    }								\
  }

/* execute a block of code some chosen times
 */
#define limitedthread(i, n, func)				\
  if ((tileheight == 1) || !th.local[0]) {			\
    if (th.local[1] < (n)) {					\
      const int i = th.local[1];				\
      func							\
    }								\
  }

/* execute a block of code some chosen times
 */
#define wfrontthread(w, func)					\
  if ((tileheight == 1) || !th.local[0]) {			\
    if (th.local[1] < WAVEFRONT_SIZE) {				\
      const int w = th.local[1];				\
      func							\
    }								\
  }

/* execute a block of code over the stride of numnodes,
 * edgesimds is the valid memory region,
 * do this in parallel
 */
#define rangedthread(l, h, func)				\
  if ((tileheight == 1) || !th.local[0]) {			\
    if ((th.local[1] >= (l)) && (th.local[1] < (h))) {		\
      const int j = th.local[1];				\
      func							\
    }								\
  }

/* execute a block of code on all threads in parallel
 */
#define allthread(a, func)					\
  if (1) {							\
    const int a = th.local[1];					\
    func							\
  }

/* execute a block of code over the stride of numnodes,
 * edgesimds is the valid memory region,
 * do this in parallel
 */
#define clampstride(i, e, func)					\
  if (th.local[1] < (e)) {					\
    const int i = th.local[1];					\
    func							\
  }

/* execute a block of code over the size of a wave-front,
 * do this in parallel, any "wavefront-local" conditional
 * won't slow down the code as the entire wavefront sees
 * the same branch(es)
 * this runs in parallel per tileheight (no check)
 */
#define clampwfront(w, func)					\
  if (th.local[1] < WAVEFRONT_SIZE) {				\
    const int w = th.local[1];					\
    func							\
  }

/* execute a block of code over multiple strides of numnodes,
 * edgesimds is the valid memory region,
 * do this in parallel
 */
#define pfrontthread(w, s, e, numnodes, func)			\
  for (int pt = (s); pt < (e); pt += tileheight) {		\
    const int w = pt + th.local[0];				\
    if (w < (numnodes)) {					\
      func							\
    }								\
  }

/* perform a binary reduction, first execute the seeding
 * block of code, then successively apply the reducing
 * block of code until all data is consumed
 *
 * the incoming data doesn't need to be pow2
 */
#define reducethread(max, cond, seed, reduce, padd) {		\
  int length = max; {						\
    length = (length + 1) >> 1;					\
    								\
    if (((tileheight == 1) || (!th.local[0])) && (cond)) {	\
      if ((th.local[1] < length))				\
        seed							\
      if ((th.local[1] == length) && (length & 1))		\
        padd							\
    }								\
    								\
    /* ensure "half" has been calculated */			\
    yield(th, 1);						\
  }								\
  								\
  while (length > 1) {						\
    length = (length + 1) >> 1;					\
    								\
    if (((tileheight == 1) || (!th.local[0])) && (cond)) {	\
      if ((th.local[1] < length))				\
	reduce							\
      if ((th.local[1] == length) && (length & 1))		\
        padd							\
    }								\
  								\
    /* ensure "quarter" has been calculated */			\
    yield(th, 2);						\
  }								\
  								\
  {								\
    /* "broadcast" the result to all other threads		\
     * which is only possible if they stopped here		\
     * and didn't pass						\
     */								\
    yield(th, 0);						\
  }}

/* ********************************************************************************************
 */
#include "../helpers/SBM.h"

// float4 / double4, short4 / int4
template<typename sumvectype, typename idxvectype>
class PathMem {
protected:
public:
  typedef Concurrency::array<sumvectype, 2> wghtgrid;
  typedef Concurrency::array<sumvectype, 1> sumarray;
  typedef Concurrency::array<idxvectype, 1> idxarray;
  typedef Concurrency::graphics::texture<int4, 1> idxtrans;

  /* the accelerator to be used for each parallel branch */
//static accelerator_view *accs[PARALLEL_BRANCHES];
  static critical_section a;
  static vector<accelerator_view> accs;

  /* edge matrix with weights applied, the edge weights, and the minimum spanning tree */
  amp_uav static wghtgrid
    *wghtmatrix[PARALLEL_BRANCHES];		// [numnodes][edgestride]
  amp_uav static sumarray
    *weights[PARALLEL_BRANCHES],		//           [edgestride]
    *mstree[PARALLEL_BRANCHES];			//           [rdctstride]
  amp_uav static idxarray
    *connections[PARALLEL_BRANCHES],		//           [rdctstride]
    *parent[PARALLEL_BRANCHES];			//           [edgestride]
  amp_srv static idxtrans
    *parentx[PARALLEL_BRANCHES];		//           [edgestride]
  /* we don't count static_memory against peak_memory */

  /* per-thread memory requirements:
   *
   *  wghtmatrix[numnodes][edgestride][1] ->
   *  weights             [edgestride][1] ->
   *  mstree              [edgestride][1] ->
   *  connections         [rdctstride][1] ->
   *
   *  2000 * 2000 * sizeof(double) ->
   *         2000 * sizeof(double) ->
   *         2000 * sizeof(double) ->
   *         2000 * sizeof(short ) ->
   *
   *  32,000,000 bytes
   *      16,000 bytes
   *      16,000 bytes
   *       4,000 bytes
   */

  static int numnodes;		// number of nodes, in elements
  static int edgestride;	// number of one row of edges, in elements, aligned
  static int edgesimds;		// number of one row of edges, in simd-units
  static int edgelog2;		// number of one row of edges, in elements, next pow2
  static int rdctstride;	// number of one row of edges for reductions, in elements, aligned
  static int rdctsimds;		// number of one row of edges for reductions, in simd-units
  static int boolstride;	// number of one row of bools, in bits, aligned

#define	boolulongs()	(boolstride / (sizeof(ulong) * 8))
//efine	maxvec()	(sizeof(sumtype) == sizeof(double) ? DBL_MAX : FLT_MAX)
};

template<typename sumtype, typename idxtype, typename sumvectype, typename idxvectype>
class Path : protected SBMatrix< PathMem<sumvectype, idxvectype> > {
  /* why's this? */
  typedef PathMem::wghtgrid  wghtgrid;
  typedef PathMem::sumarray sumarray;
  typedef PathMem::idxarray idxarray;

  typedef Concurrency::array_view<sumvectype, 2> wgtview;
  typedef Concurrency::array_view<sumvectype, 1> sumview;

  typedef Concurrency::graphics::writeonly_texture_view<int4, 1>
    idxview;		// write-only
  typedef Concurrency::array_view<const ulong, 2>
    bolview;		// read-only

  typedef vector<sumtype, SSE2Allocator<sumtype> > sumvector;
  typedef vector<idxtype, SSE2Allocator<idxtype> > idxvector;
  typedef vector<ulong  , SSE2Allocator<ulong  > > bolvector;

public:
  /* Held-Karp solution */
  sumtype lowerBound;
  int lowestSubgraph;
  int lambdaSteps;

  /* tree of permutations */
  int depth;
  Path *seed;

  idxvector parent;				//           [edgestride]

private:
  /* per-path memory requirements:
   *
   *  parent            [edgestride] ->
   *  included[numnodes][boolstride] ->
   *
   *         2000 * sizeof(short) ->
   *  2000 * 2000 * sizeof(ulong) / 32 ->
   *
   *    4,000 bytes
   *  500,000 bytes
   */

  friend bool operator < (const Path &a, const Path  &b) {
    return (a.lowerBound >= b.lowerBound);
  }

  /* --------------------------------------------------------------------------------------
   * helpers (or optimized)
   */

  /* ensure the data for one full "tilestride" has been
   * calculated
   *
   * if it is also the task of another wave-front we yield
   * execution to them
   *
   * otherwise it was calculated by the same wave-front
   * this code is in already and we don't need to block
   */
  template<const int tileheight, const int tilestride>
  static void yieldall(tiled_index<tileheight, tilestride> th) restrict(amp,cpu) {
    if ((tilestride * tileheight) > WAVEFRONT_SIZE)
      th.barrier.wait_with_tile_static_memory_fence();
    else
      tile_static_memory_fence(th.barrier);
  }

  template<const int tileheight, const int tilestride>
  static void yield(tiled_index<tileheight, tilestride> th, int substride = 0) restrict(amp,cpu) {
    if ((tilestride >> substride) > WAVEFRONT_SIZE)
      th.barrier.wait_with_tile_static_memory_fence();
    else
      tile_static_memory_fence(th.barrier);
  }

  /* ensure the data just written is available to the other
   * threads in this wave-front (1:N), or if it goes over
   * memory, that it is available to all other wave-fronts
   *
   * the memory fence is stronger than the tile-fence, no
   * need to do both in that case
   */
  template<const int tileheight, const int tilestride>
  static void broadcast(tiled_index<tileheight, tilestride> th, bool istatic = false) restrict(amp,cpu) {
    if ((tilestride * 1) > WAVEFRONT_SIZE) {
      if (!istatic)
	all_memory_fence(th.barrier);
      else
	tile_static_memory_fence(th.barrier);
    }
  }

  /* -------------------------------------------------------------------------------------- */

  template<const int tileheight, const int tilestride>
  static void initweights(
    tiled_index<tileheight, tilestride> th,
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts
  ) restrict(amp,cpu) {
    /* mask out over-aligned weights */
    sumvectype l(
      (3 < (SIMDsize - (tv.numnodes % SIMDsize)) ? maxvec<sumtype>() : 0),	// sumvectype(m, m, m, m)
      (2 < (SIMDsize - (tv.numnodes % SIMDsize)) ? maxvec<sumtype>() : 0),	// sumvectype(0, m, m, m)
      (1 < (SIMDsize - (tv.numnodes % SIMDsize)) ? maxvec<sumtype>() : 0),	// sumvectype(0, 0, m, m)
      (0 < (SIMDsize - (tv.numnodes % SIMDsize)) ? maxvec<sumtype>() : 0)	// sumvectype(0, 0, 0, m)
    );

    /* "weights[]" is never subject to binary reduction */
    limitedthread(j, tv.edgesimds, {
      /* if "numnodes % SIMDsize" == 0, then "numnodes / SIMDsize" == edgesimds (< j) */
      ts.weights(tv, j) = (j == (tv.numnodes / SIMDsize) ? l : 0);
    });
  }

  template<const int tileheight, const int tilestride>
  static void syncweights(
    tiled_index<tileheight, tilestride> th,
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts
  ) restrict(amp,cpu) {
    /* "weights[]" is never subject to binary reduction */
    limitedthread(j, tv.edgesimds, {
      ts.weighti(tv, j);
    });
  }

  template<const int tileheight, const int tilestride>
  static void initmstree(
    tiled_index<tileheight, tilestride> th,
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts,
    int firstNeighbor
  ) restrict(amp,cpu) {
    /* ensure "firstNeighbor" of "wghtmatrix[]" has been calculated */
    yieldall(th);

    /* "mstree[]" is subject to binary reduction */
    limitedthread(j, tv.rdctsimds, {
      ts.mstree(tv, j) = (j >= tv.edgesimds ? maxvec<sumtype>() : ts.wghtmatrix(tv, firstNeighbor, j));
    });
  }

  template<const int tileheight, const int tilestride>
  static void initroutes(
    tiled_index<tileheight, tilestride> th,
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts,
    int firstNeighbor,
    idxvectype &parent_local
  ) restrict(amp,cpu) {
    /* mask out over-aligned connections */
    idxvectype l(
      (3 < (SIMDsize - (tv.numnodes % SIMDsize)) ? 2 : 0),			// idxvectype(2, 2, 2, 2)
      (2 < (SIMDsize - (tv.numnodes % SIMDsize)) ? 2 : 0),			// idxvectype(0, 2, 2, 2)
      (1 < (SIMDsize - (tv.numnodes % SIMDsize)) ? 2 : 0),			// idxvectype(0, 0, 2, 2)
      (0 < (SIMDsize - (tv.numnodes % SIMDsize)) ? 2 : 0)			// idxvectype(0, 0, 0, 2)
    );

    /* "connections[]" is subject to binary reduction */
    limitedthread(j, tv.rdctsimds, {
      /* if "numnodes % SIMDsize" == 0, then "numnodes / SIMDsize" == edgesimds (< j) */
      ts.connections(tv, j) = (j >= tv.edgesimds ? 2 : (j == (tv.numnodes / SIMDsize) ? l : 0));
      parent_local          = firstNeighbor;
    });
  }

  /* --------------------------------------------------------------------------------------
   */

  template<const int tileheight, const int tilestride>
  static sumtype connect0(
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts,
    int i, int j
  ) restrict(amp,cpu) {
    /* register new connections */
    ts.connections(tv, i / SIMDsize)[i % SIMDsize]++;
    ts.connections(tv, j / SIMDsize)[j % SIMDsize]++;

    /* indexed access to vector */
    return ts.wghtmtxrw0(tv, i, j / SIMDsize)[j % SIMDsize];
  }

  template<const int tileheight, const int tilestride>
  static sumtype connecti(
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts,
    int i, int j
  ) restrict(amp,cpu) {
    /* register new connections */
    ts.connections(tv, i / SIMDsize)[i % SIMDsize]++;
    ts.connections(tv, j / SIMDsize)[j % SIMDsize]++;

    /* indexed access to vector */
    return ts.wghtmatrix(tv, i, j / SIMDsize)[j % SIMDsize];
  }

  /* --------------------------------------------------------------------------------------
   * main function (50-60% of time spend here)
   */
  template<typename edgtype, typename edgvectype, typename edgresview,
           const int tileheight, const int tilestride>
  static void computeOneTree(
    tiled_index<tileheight, tilestride> th,
    tiled_views<idxvectype, wghtgrid, sumarray, idxarray> &tv,
    tiled_cache<sumtype, int, sumvectype, idxvectype, wghtgrid, sumarray, idxarray, tileheight, tilestride> &ts,
    edgresview &edgematrix,
    idxvectype &parent_local,
    bolview     includs,
    sumtype    &lowerBound,
    int        &lowestSubgraph
  ) restrict(amp,cpu) {
    /* globally valid thread-to-location mappings */
    const int r  = (th.local[1] << 0) + 0;
    const int r0 = (th.local[1] << 1) + 0;
    const int r1 = (th.local[1] << 1) + 1;

    const int pos  = (r  * SIMDsize);
    const int pos0 = (r0 * SIMDsize);
    const int pos1 = (r1 * SIMDsize);

    const int4 curindex  = int4(pos  + 0, pos  + 1, pos  + 2, pos  + 3);
    const int4 curindex0 = int4(pos0 + 0, pos0 + 1, pos0 + 2, pos0 + 3);
    const int4 curindex1 = int4(pos1 + 0, pos1 + 1, pos1 + 2, pos1 + 3);

    /* compute adjusted costs (all rows)
     *
     * this is the only block where the excess threads (> tilestride)
     * are used at all, after this block they'll basically run idle
     */
    pfrontthread(row, 0, tv.numnodes, tv.numnodes, {
      /* vertical */
      sumvectype vwght = ts.weights(tv, row / SIMDsize)[row % SIMDsize];

      /* burst reads for each tile-layout case:
       *
       *  1024x1 ... 32x32 (4-way double) -> 32768 bytes ->  16 bursts of (8 * 256 bytes) -> all channels used
       *  1024x1 ... 32x32 (4-way float ) -> 16384 bytes ->   8 bursts of (8 * 256 bytes)
       *  1024x1 ... 32x32 (4-way ulong ) -> 16384 bytes ->   8 bursts of (8 * 256 bytes)
       *  1024x1 ... 32x32 (4-way ushort) ->  8192 bytes ->   4 bursts of (8 * 256 bytes)
       *  1024x1 ... 32x32 (4-way uchar ) ->  4096 bytes ->   2 bursts of (8 * 256 bytes)
       *
       *  16x16            (4-way double) ->  8192 bytes ->  4/1 burst of (8 * 256 bytes)
       *  16x16            (4-way float ) ->  4096 bytes ->  2/1 burst of (8 * 256 bytes)
       *  16x16            (4-way ulong ) ->  4096 bytes ->  2/1 burst of (8 * 256 bytes)
       *  16x16            (4-way ushort) ->  2048 bytes ->  1/1 burst of (8 * 256 bytes)
       *  16x16            (4-way uchar ) ->  1024 bytes ->  1/2 burst of (8 * 256 bytes)
       *
       *  8x8              (4-way double) ->  2048 bytes ->  1/1 burst of (8 * 256 bytes)
       *  8x8              (4-way float ) ->  1024 bytes ->  1/2 burst of (8 * 256 bytes)
       *  8x8              (4-way ulong ) ->  1024 bytes ->  1/2 burst of (8 * 256 bytes)
       *  8x8              (4-way ushort) ->   512 bytes ->  1/4 burst of (8 * 256 bytes)
       *  8x8              (4-way uchar ) ->   256 bytes ->  1/8 burst of (8 * 256 bytes)
       *
       * texture (slow path): uchar/ushort/ulong
       * array   (fast path): float/double
       */
      clampstride(c, tv.edgesimds, {
      	/* rotate the access pattern in case multiple wavefront/tile_rows
      	 * are active to prevent memory channel bank conflicts
      	 */
      	int col = (c + row) % (tv.edgesimds);

	/* horizontal */
        sumvectype hwght = ts.weights(tv, col);
	sumvectype wght;

	int bpos = (col * SIMDsize);
	int basebit = bpos & 31;
	int bitfield = includs(row, bpos >> 5);

	// wghtmatrix[row * edgestride + col] = edgematrix[row * edgestride + col] + weights[row] + weights[col];
	wght = sumvectype(edgematrix(row, col)) + (vwght + hwght);

	/* GPUs have vectorized cmov */
	wght = amphelper::setenabled(wght, int4(
	  bitfield & (1 << (basebit + 0)),
	  bitfield & (1 << (basebit + 1)),
	  bitfield & (1 << (basebit + 2)),
	  bitfield & (1 << (basebit + 3))
	), sumvectype(maxvec<sumtype>()));

	ts.wghtmatrix(tv, row, col, wght);
      });
    });

    /* ensure "wghtmatrix[0]" has been calculated */
    yield(th);

    /* get the two cheapest edges from 0 --------------------------------------------------- */
    int firstNeighbor  = 0;
    int secondNeighbor = 0;

    {
      struct amphelper::ampminima_adaptive<
      	sumtype, int, sumvectype, idxvectype,
      	wghtgrid, sumarray, idxarray,
      	tileheight, tilestride
      > omi;

      /* divide into 4/32/64 buckets (wavefront size), only if we run out of LDS */
      if (ts.slowminima()) {
        /* horizontal reduction (X to 4/32/64, vectorized, single wave-front) */
        wfrontthread(w, {
	  omi.init(ts, w, maxvec<sumtype>());

	  /* start & end, clamped to edgesimds */
	  int l = min((w + 0) * omi.blcksize, tv.edgesimds);
	  int h = min((w + 1) * omi.blcksize, tv.edgesimds);
	  for (int col = l; col < h; col++) {
	    int cpos = (col * SIMDsize);
	    int4 cindx = int4(cpos + 0, cpos + 1, cpos + 2, cpos + 3);
	    sumvectype wght = ts.wghtmtxrw0(tv, 0, col);

	    /* assign to one of the 4/32/64 buckets */
	    omi.orderedmin(ts, w, wght, cindx);
	  }
        });

        /* vertical reduction (4/32/64 to 2, then 2 to 1, vectorized, single wave-front) */
        limitedthread(w, 2, { omi.slow_orderedred2(ts, tv.edgesimds, w); });
        singlethread (   0, { omi.slow_orderedred1(ts, tv.edgesimds   ); });

        /* ensure "omi" is visible to other threads/wavefronts (1:N) */
        broadcast(th, true /* == tile_static */);
      }
      else {
        reducethread(tv.edgesimds, true, {
	  sumvectype wght0 = ts.wghtmtxrw0(tv, 0, (r0 >= tv.edgesimds ? r0 : r0));
	  sumvectype wght1 = ts.wghtmtxrw0(tv, 0, (r1 >= tv.edgesimds ? r0 : r1));

	  /* put the smaller in "first", the larger in "second" */
	  omi.orderedset(ts, r, wght0, curindex0,
				wght1, curindex1);
        }, {
	  /* the r0's "second" may overlap r's "second" */
	  int4 indx0 = ts.secndN(r0);
	  int4 indx1 = ts.secndN(r1);
	  sumvectype wght0 = ts.secndV(r0);
	  sumvectype wght1 = ts.secndV(r1);

	  /* put the smaller in "first", the larger in "second" */
	  omi.orderedset(ts, r, ts.firstV(r0), ts.firstN(r0),
				ts.firstV(r1), ts.firstN(r1));

	  /* check how the remaining values fit in */
	  omi.orderedmin(ts, r, wght0, indx0);
	  omi.orderedmin(ts, r, wght1, indx1);
        }, {
	  /* crashes "link.exe" */
//	  omi.init(ts, r, maxvec<sumtype>());

	  ts.firstN(r) = 0;
	  ts.secndN(r) = 0;
	  ts.firstV(r) = maxvec<sumtype>();
	  ts.secndV(r) = maxvec<sumtype>();
        });
      }

      /* horizontal reduction (4-way vector to scalar, suck from thread-local into registers) */
      omi.orderedmallest<sumtype>(ts, firstNeighbor, secondNeighbor);
    }

    /* initialize connectivity */
    initroutes(th, tv, ts, firstNeighbor, parent_local);

    /* compute the length of minimum spanning tree on nodes [1, numnodes - 1] -------------- */
    tile_static sumtype mstLength;

    /* copy out initial costs from weight-matrix (waits for "wghtmatrix" being valid) */
    initmstree(th, tv, ts, firstNeighbor);

    /* select the thread responsible for parent[firstNeighbor / SIMDsize] */
    singlethread(firstNeighbor / SIMDsize, {
      /* add first edge */
      mstLength = connecti(tv, ts, 0, firstNeighbor);
      parent_local[firstNeighbor % SIMDsize] = 0;
    });

    /* ensure "connect[]" and "parent[]" is visible to other threads/wavefronts */
    broadcast(th, false /* == memory */);

    /* consume all unassigned edges */
    for (int k = 2, l = 0, h = tv.edgesimds; k < tv.numnodes /* * !th.local[1] */; k++) {
      /* search first unassigned with lowest cost  - - - - - - - - - - - - - - - - - - - - - */
      /* ensure "parent[]" and "mstree[]" has been calculated */
      yield(th);

      reducethread(h, true, {
	/* excess connections are neutral "2" */
	int4 con0 = ts.connections(tv, r0);
	int4 con1 = ts.connections(tv, r1);
	idxvectype cv_i_ = 0;
	sumvectype mv_i_ = sumvectype(maxvec<sumtype>());

	/* skip i/o if not necessary (all non-zero) */
	int2 chk = amphelper::hmul(con0, con1);
	if (!(chk.x * chk.y)) {
	  /* skip connections of "> 0" (assign MAX, voids the "smaller") */
	  sumvectype mstv0 = ts.mstree(tv, r0);
	  sumvectype mstv1 = ts.mstree(tv, r1);

	  /* GPUs have vectorized cmov */
	  mstv0 = amphelper::setdisabled(mstv0, con0, mv_i_);
	  mstv1 = amphelper::setdisabled(mstv1, con1, mv_i_);

	  /* maintain lower index in case of equality */
	  cv_i_ = amphelper::smaller(mstv1, mstv0, curindex1, curindex0);
	  mv_i_ = amphelper::smaller(mstv1, mstv0,     mstv1,     mstv0);
	}

	ts.cr(r) = cv_i_;
	ts.mr(r) = mv_i_;
      }, {
	ts.cr(r) = amphelper::smaller(ts.mr(r1), ts.mr(r0), ts.cr(r1), ts.cr(r0));
	ts.mr(r) = amphelper::smaller(ts.mr(r1), ts.mr(r0), ts.mr(r1), ts.mr(r0));
      }, {
	ts.mr(r) = sumvectype(maxvec<sumtype>());
      });

      /* add unassigned with lowest cost - - - - - - - - - - - - - - - - - - - - - - - - - - */
      int i = amphelper::usmallest(ts.cr(0), ts.mr(0));

      /* select the thread responsible for parent[i / SIMDsize] */
      singlethread(i / SIMDsize, {
	/* add next edge */
	int currentParent = parent_local[i % SIMDsize];
	mstLength += connecti(tv, ts, i, currentParent);
      });

      /* ensure "connect[]" and "parent[]" is visible to other threads/wavefronts */
      yield(th);

      /* reassign costs  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
      int4 iv = i;

      rangedthread(l, h, {
	/* excess connections are neutral "2" */
	int4 con = ts.connections(tv, j);

	/* skip i/o if not necessary (all non-zero) */
	if (!amphelper::hmul(con)) {
	  /* skip connections of "> 0" (assign MAX, voids the "greater") */
	  sumvectype wght = ts.wghtmatrix(tv, i, j);
	  sumvectype mstv = ts.mstree(tv, j);

	  /* GPUs have vectorized cmov */
	  wght = amphelper::setdisabled(wght, con, sumvectype(maxvec<sumtype>()));

	  /* maintain lower index in case of equality */
	  parent_local = amphelper::greater(mstv, wght, iv, parent_local);
	  ts.mstree(tv, j) = amphelper::greater(mstv, wght, wght, mstv);
	}
      });
    }

    /* select the thread responsible for parent[0 / SIMDsize] */
    singlethread(0, {
      /* add last edge */
      mstLength += connecti(tv, ts, 0, secondNeighbor);
      parent_local.x = secondNeighbor;
    });

    /* ensure "connections[]" and "parent[]" is visible to other threads/wavefronts */
    broadcast(th, false /* == memory */);

    /* built shortest non-cycle sub-graph -------------------------------------------------- */
    reducethread(tv.edgesimds, true, {
      /* excess connections are neutral "2" */
      /* -2 wraparound signflip, 16bit in 23 mantissa fixup */
      idxvectype con0 = (ts.connections(tv, r0) << 16) + 0x7FFD0000;
      idxvectype con1 = (ts.connections(tv, r1) << 16) + 0x7FFD0000;

      /*	fixed			  unfixed (doesn't fit into float)
       *
       * 0 ->	0x7FFD0000 -> 4EFFFA00    0x7FFFFFFD -> 4F000000
       * 1 ->	0x7FFE0000 -> 4EFFFC00    0x7FFFFFFE -> 4F000000
       * 2 -> 	0x7FFF0000 -> 4EFFFE00    0x7FFFFFFF -> 4F000000
       * 3 ->	0x80000000 -> CF000000    0x80000000 -> CF000000
       * 4 ->	0x80010000 -> CEFFFE00    0x80000001 -> CF000000
       * ...
       */

      /* maintain lower index in case of equality */
      ts.cr(r) = idxvectype(amphelper::smaller(con1, con0, curindex1, curindex0));
      ts.mr(r) = sumvectype(amphelper::smaller(con1, con0,      con1,      con0));
    }, {
      ts.cr(r) = idxvectype(amphelper::smaller(ts.mr(r1), ts.mr(r0), ts.cr(r1), ts.cr(r0)));
      ts.mr(r) = sumvectype(amphelper::smaller(ts.mr(r1), ts.mr(r0), ts.mr(r1), ts.mr(r0)));
    }, {
      ts.mr(r) = 0x7FFF0000;
    });

    /* round-to-nearest, round half towards minus infinity, prevent sum-of-squares problem */
    lowerBound = amphelper::round(mstLength);
    lowestSubgraph = amphelper::usmallestneg(ts.cr(0), ts.mr(0));
  }

  template<typename edgtype, typename edgvectype, typename edgresview, const int tileheight, const int tilestride>
  bool computeHeldKarp(int pb, edgresview &edgematrix, sumtype bestLowerBound) {
    /* choose the group-local resources */
    accelerator_view &acc = (PathMem::accs[pb]);
    amp_uav wghtgrid &wghtmatrix = *(PathMem::wghtmatrix[pb]);
    amp_uav sumarray &weights = *(PathMem::weights[pb]);
    amp_uav sumarray &mstree = *(PathMem::mstree[pb]);
    amp_uav idxarray &parent = *(PathMem::parent[pb]);
    amp_srv idxtrans &parentx = *(PathMem::parentx[pb]);
    amp_uav idxarray &connections = *(PathMem::connections[pb]);

    const int numnodes   = PathMem::numnodes;
    const int edgestride = PathMem::edgestride;
    const int edgesimds  = PathMem::edgesimds;
    const int rdctstride = PathMem::rdctstride;
    const int rdctsimds  = PathMem::rdctsimds;
    const int boolstride = PathMem::boolstride;

    /* fake extent, go for maximum threads */
    Concurrency::extent<2> ee(tileheight, tilestride);
    Concurrency::tiled_extent<tileheight, tilestride> te(ee);

    /* parents should be write-only, includes read-only */
    idxview parents(parentx);
    bolview includs(numnodes, boolulongs(), this->raw(pb));

    /* get the adjacency matrix onto this accelerator_view (does this provoke overhead/data cloning?) */
    edgresview edgetex(edgematrix, acc);

    /* unfortunately read-back is necessary */
    struct gpuio {
      sumtype currLambda;
      int currLambdaCount;
      sumtype currLowerBound;
      int currLowestSubgraph;
    } gpustate = {
      (sumtype)0.1,
      0,
      this->lowerBound,
      this->lowestSubgraph
    };

    Concurrency::array_view<struct gpuio, 1> persistant(1, &gpustate);

    /* lagrangian optimization */
    sumtype lambda = (sumtype)0.1;
    int     lambda_count = 0;
    sumtype lambda_reduction = (sumtype)::lambda_reduction;
    sumtype lambda_termination = (sumtype)::lambda_termination;

    while (lambda > lambda_termination) {
      /* run the algorithm in bursts of loops
       *
       * calculate the minimum number of iteration to get to the goal
       * without considering bestLowerBound breaks
       */
      int lambda_iterations = 0;
      sumtype lambda_test = lambda;
      while (lambda_test > lambda_termination) {
	lambda_test *= lambda_reduction;
	lambda_iterations++;
      }

      /* binary search */
      if (lambda_iterations > 32)
        lambda_iterations = (lambda_iterations + 1) >> 1;

      /* take the abortion rate of the seeding path into account
       * if it was very high we don't issue a large block but try
       * with its number of iteration as maximum first
       */
      if (this->seed)
	lambda_iterations = min(lambda_iterations, this->seed->lambdaSteps);
	lambda_iterations = max(lambda_iterations, 1);

      /* the loop can be outer or inner
       *
       * as the synchronize() is extreme slow (200x the parallel_for_each), we
       * just issue the loop without even looking at what's going on on the GPU
       */
//    lambda_iterations = 1;
//    for (int li = 0; li < lambda_iterations; li++)
      {
	Concurrency::parallel_for_each(acc, te, [=,
	  &edgetex /*&edgematrix*/,
	  &wghtmatrix,
	  &weights,
	  &mstree,
#if 0 // ndef NDEBUG
	  &parent,
#endif
	  &connections
	](tiled_index<tileheight, tilestride> th) restrict(amp) {
	  /* globally valid thread-to-location mappings */
	  const int r  = (th.local[1] << 0) + 0;
	  const int r0 = (th.local[1] << 1) + 0;
	  const int r1 = (th.local[1] << 1) + 1;

	  const int pos  = (r  * SIMDsize);
	  const int pos0 = (r0 * SIMDsize);
	  const int pos1 = (r1 * SIMDsize);

	  const int4 curindex  = int4(pos  + 0, pos  + 1, pos  + 2, pos  + 3);
	  const int4 curindex0 = int4(pos0 + 0, pos0 + 1, pos0 + 2, pos0 + 3);
	  const int4 curindex1 = int4(pos1 + 0, pos1 + 1, pos1 + 2, pos1 + 3);

	  /* shared memory views  - - - - - - - - - - - - - - - - - - - - - - */
	  tiled_views<
	    idxvectype,
	    wghtgrid, sumarray, idxarray
	  > tv(
	    numnodes,
	    edgesimds,
	    rdctsimds,
	    wghtmatrix,
	    weights,
	    mstree,
#if 0 // ndef NDEBUG
	    parent,
#endif
	    connections
	  );

	  /* shared reduction/cachable vars/arrays  - - - - - - - - - - - - - */
	  tile_static tiled_cache<
	    sumtype, int, sumvectype, idxvectype,
	    wghtgrid, sumarray, idxarray,
	    tileheight, tilestride
	  > ts;

	  /* thread local 1:1 vars (being uninitialized locks up, apparently
	   * passing references to uninitialized variables makes it crash)
	   *
	   * using tv's parent_local crashes "link.exe"
	   */
	  idxvectype parent_local = 0;

	  /* read persistant GPU-data into registers
	   * maintain across-call coherency, don't use constant buffers
	   */
	  sumtype currLambda         = persistant(0).currLambda;
	  int     currLambdaCount    = persistant(0).currLambdaCount;
	  sumtype prevLowerBound     = persistant(0).currLowerBound;
	  int     currLowestSubgraph = persistant(0).currLowestSubgraph;
//	  sumtype bestLowerBound     = persistant(0).bestLowerBound;
//	  sumtype currLowerBound     = persistant(0).currLowerBound;
//	  int     currLowestSubgraph = 0;
	  sumtype currLowerBound     = 0;

	  /* prepare weights (initially zero, overflow maxvec)  - - - - - - - */
	  if (!currLowestSubgraph)
	    initweights(th, tv, ts);
	  else
	    syncweights(th, tv, ts);

	  for (int i = 0; i < 1; i++)
	  {
            /* ensure "weights" has been calculated */
	    yieldall(th);

	    /* interpretes:
	     * - weights (all of it)
	     * - edgematrix (all of it)
	     *
	     * fills:
	     * - wghtmatrix (all of it)
	     * - parent (all of it)
	     * - connections (all of it)
	     */

	    computeOneTree<
	      edgtype, edgvectype, edgresview,
	      tileheight, tilestride
	    >(
	      th,
	      tv,
	      ts,
	      edgetex /*edgematrix*/,
	      parent_local,
	      includs,
	      currLowerBound,
	      currLowestSubgraph
	    );

	    /* CODEBUG: "notdone" apparently isn't calculated correctly */
#define notdone                     /* better than best, and not yet a valid path */	\
	     ((currLambda > lambda_termination) &&	/* regular loop condition */	\
	      (currLowestSubgraph > -1) &&		/* early loop termination */	\
	      (currLowerBound < bestLowerBound))	/* early loop termination */	\

	    /* cut early, binary parallel sum */
	    reducethread(tv.edgesimds, notdone, {
	      /* excess connections are neutral "2" */
	      /* connections(0)[0] is always "2" */
	      int4 d0 = (ts.connections(tv, r0) - 2);
	      int4 d1 = (ts.connections(tv, r1) - 2);

	      ts.cr(r) = d0 * d0 + d1 * d1;
	    }, {
	      ts.cr(r) =  ts.cr(r0) +  ts.cr(r1);
	    }, {
	      ts.cr(r) = 0;
	    });

	    if (notdone) {
	      if (!(currLowerBound < prevLowerBound)) {
		currLambda *= lambda_reduction;
	        currLambdaCount++;
	      }

	      /* if zero all are == 2 (notdone is false if denom 0) */
	      int denom = amphelper::hadd(ts.cr(0));

	      /* readjust weights */
	      sumvectype t = (currLambda * currLowerBound) / denom;
	      limitedthread(j, tv.edgesimds, {
		/* connections(0)[0] is always "2", thus "weight(0)[0]" is always 0 */
	        ts.weights(tv, j) = t * sumvectype(ts.connections(tv, j) - 2) + ts.weights(tv, j);
	      });

	      prevLowerBound = currLowerBound;
	    }

	    /* no use, it's worst or it's the best
	    if (!notdone)
	      break; */
	  }

#if 0 // ndef NDEBUG
	  /* CODEBUG: "bestdone" apparently isn't calculated correctly */
#define bestdone /* last iteration or a completely valid path, and better than best */	\
	   (((currLambda <= lambda_termination) ||	/* regular loop condition */	\
	     (currLowestSubgraph == -1)) &&		/* early loop termination */	\
	    (currLowerBound < bestLowerBound))		/* early loop termination */	\

	  limitedthread(j, tv.edgesimds, {
	    tv.connections(j) = ts.connections(tv, j);
	    tv.parent(j) = parent_local;

	    ts.weighto(tv, j);
	  });
#else
	  if (currLowerBound < bestLowerBound) {
	    limitedthread(j, tv.edgesimds, {
	      /* finished iterating */
	      if ((currLambda <= lambda_termination) ||
		  (currLowestSubgraph == -1))
		parents.set(index<1>(j), parent_local);

	      /* not finished iterating */
	      else
		ts.weighto(tv, j);
	    });
	  }
#endif

	  /* write out persistant GPU-data */
	  singlethread(1, {
	    persistant(0).currLambda         = currLambda;
	    persistant(0).currLambdaCount    = currLambdaCount;
	    persistant(0).currLowerBound     = currLowerBound;
	    persistant(0).currLowestSubgraph = currLowestSubgraph;
	  });

	  /* make sure persistant GPU-data propagates */
          all_memory_fence(th.barrier);
	});

//	wghtmatrix.accelerator_view.wait();
      }

      /* synchronize persistant GPU-data */
      persistant.synchronize();

      /* update path-data */
      this->lowerBound     = gpustate.currLowerBound;
      this->lowestSubgraph = gpustate.currLowestSubgraph;

#if 1
      // ndef NDEBUG
      {
	vector<int> conns(SIMDsize * rdctsimds);
	array_view<int4, 1> connv(rdctsimds, (int4 *)&conns[0]);

        Concurrency::extent<1> ec(edgesimds);
        Concurrency::parallel_for_each(acc, ec, [=,
	  &parent
        ](index<1> j) restrict(amp) {
	  parents.set(j, parent(j));
        });

        copy(parentx, &this->parent[0], (unsigned int)(this->parent.size() * sizeof(idxtype)));
        copy(connections, connv);
	connv.synchronize();

        fprintf(stdout, " l %f,", lambda);
        fprintf(stdout, " lc %5d,", lambda_count);
        fprintf(stdout, " lb %f,", this->lowerBound);
        fprintf(stdout, " ls %2d,", this->lowestSubgraph);
        for (int n = 0; n < numnodes; n++)
	  fprintf(stdout, " %3d (%1d)", this->parent[n], conns[n]);
        fprintf(stdout, "\n");
	fflush(stdout);
      }
#endif // !NDEBUG

      /* update loop variable */
      lambda       = gpustate.currLambda;
      lambda_count = gpustate.currLambdaCount;

      /* no use, it's worst or it's the best */
      if (!(this->lowerBound < bestLowerBound))
	break;
      if (!(this->lowestSubgraph > -1))
	break;
    }

    /* read parents back type-converted */
    if (this->lowerBound < bestLowerBound) {
      Concurrency::extent<1> ec(edgesimds);
      Concurrency::parallel_for_each(acc, ec, [=,
	&parent
      ](index<1> j) restrict(amp) {
	parents.set(j, parent(j));
      });

      /* there are PARALLEL_BRANCHES parentx buffers, we can do async */
      copy(parentx, &this->parent[0], (unsigned int)(this->parent.size() * sizeof(idxtype)));
    }

    /* is this a possibly shorter path? */
    this->lambdaSteps = lambda_count;
    return (this->lowerBound < bestLowerBound);
  }

  template<typename edgtype, typename edgvectype, typename edgresview>
  bool computeHeldKarp(int pb, edgresview &edgematrix, sumtype bestLowerBound) {
    /* choose the thread-local resources */
    if (!PathMem::wghtmatrix[pb]) {
      a.lock();

      {
	vector<accelerator> accelerators = accelerator::get_all();
	int na = 0, pa = 0;

	/* count native accelerators */
	for_each(accelerators.begin(), accelerators.end(), [&] (accelerator& accl) {
	  na += (!accl.is_emulated ? 1 : 0);
	});

	/* round-robin assignment of accelerators to branches */
	for_each(accelerators.begin(), accelerators.end(), [&] (accelerator& accl) {
	  if (!accl.is_emulated) {
	    if (pa == (pb % na)) {
	      /* expand on demand (prevent default constructor from being called/needed) */
	      accelerator_view acc = accl.create_view();
	      while (PathMem::accs.size() < (size_t)(pb + 1))
		PathMem::accs.push_back(acc);

	      /* create multiple views */
	      PathMem::accs[pb] = acc;
  //          PathMem::accs[pb] = accl.default_view;
  //	      break;
	    }

	    pa++;
	  }
	});
      }

      a.unlock();
      accelerator_view &acc = (PathMem::accs[pb]);

      /* first use initializes static memories */
      PathMem::wghtmatrix [pb] = new wghtgrid(numnodes, edgesimds, acc);
      PathMem::weights    [pb] = new sumarray(edgesimds, acc);
      PathMem::parent     [pb] = new idxarray(edgesimds, acc);
      PathMem::parentx    [pb] = new idxtrans(edgesimds, (unsigned int)(sizeof(idxtype) * 8U), acc);
      /* these are subject to binary reduction */
      PathMem::mstree     [pb] = new sumarray(rdctsimds, acc);
      PathMem::connections[pb] = new idxarray(rdctsimds, acc);

#if 0
      /* reduction-access can be one 4-way vector in excess over edgestride
       * as this is only read (reduction writes only to max >> 1) we can
       * set those values to the neutral value on allocation and they never
       * change
       */
      if ((rdctstride - edgestride) == SIMDsize) {
	amp_uav sumarray &mstree = *(PathMem::mstree[pb]);
	amp_uav idxarray &connections = *(PathMem::connections[pb]);

	/* serves as warm-up as well */
	Concurrency::extent<1> ee(rdctsimds);
        Concurrency::parallel_for_each(acc, ee, [=,
	  &mstree,
	  &connections
	](index<1> p) restrict(amp) {
	  mstree(p) = maxvec<sumtype>();
	  connections(p) = 2;
	});
      }
#endif

      /* can be anything
      tiled_cache<float , int, float4 , int4, wghtgrid, sumarray, idxarray, 1, 8>::stats();
      tiled_cache<double, int, double4, int4, wghtgrid, sumarray, idxarray, 1, 8>::stats();
       */
    }

    this->parent.resize(edgestride);
    this->lowerBound = minvec<sumtype>();
    this->lowestSubgraph = 0;

    /* choose a performant tile-distribution
     *
     * short-cut wave-fronts (the y-dimension != 0) run at about 50 cycles per y
     * striding wave-fronts (the x-dimension > 64) run at about 120 cycles per y
     * the first wave-front (the x-dimension <= 64) runs at about 1000 cycles per y
     *
     * GPU ShaderAnalyzer:
     *
     *  512x 2 -> 16755.10 max (~WF0), 1004.19 avg (~WFX),  6.75 min (~WFY)
     *  256x 4 -> 16805.60 max (~WF0), 1005.70 avg (~WFX),  6.70 min (~WFY)
     *  128x 8 -> 15912.20 max (~WF0),  935.68 avg (~WFX),  6.60 min (~WFY)
     *   64x16 -> 15542.50 max (~WF0),  918.63 avg (~WFX),  6.35 min (~WFY)
     *   32x32 -> 15542.50 max (~WF0),  918.63 avg (~WFX),  6.35 min (~WFY)
     *   16x16 ->  1726.45 max (~WF0),  280.63 avg (~WFX), 23.30 min (~WFY)
     *    8x 8 ->   426.90 max (~WF0),  106.34 avg (~WFX), 11.65 min (~WFY)
     */
    switch (edgelog2 / SIMDsize) {
      /*
      case 1024: // if (edgestride > (512 * SIMDsize))		// 4096 edges max, 16 wave-fronts
	return computeHeldKarp<edgtype, edgvectype, edgresview,  1, 1024>(pb, edgematrix, bestLowerBound);
      case  512: // if (edgestride > (256 * SIMDsize))		// 2048 edges max, 16 wave-fronts
	return computeHeldKarp<edgtype, edgvectype, edgresview,  2,  512>(pb, edgematrix, bestLowerBound);
      case  320: // if (edgestride > (256 * SIMDsize))		// 1280 edges max, 15 wave-fronts
  	return computeHeldKarp<edgtype, edgvectype, edgresview,  3,  320>(pb, edgematrix, bestLowerBound);
      case  256: // if (edgestride > (128 * SIMDsize))		// 1024 edges max, 16 wave-fronts
	return computeHeldKarp<edgtype, edgvectype, edgresview,  4,  256>(pb, edgematrix, bestLowerBound);
      case  192: // if (edgestride > (128 * SIMDsize))		//  768 edges max, 15 wave-fronts
  	return computeHeldKarp<edgtype, edgvectype, edgresview,  5,  192>(pb, edgematrix, bestLowerBound);
      case  160: // if (edgestride > (128 * SIMDsize))		//  640 edges max, 15 wave-fronts
  	return computeHeldKarp<edgtype, edgvectype, edgresview,  6,  160>(pb, edgematrix, bestLowerBound);
      case  128: // if (edgestride > ( 64 * SIMDsize))		//  512 edges max, 16 wave-fronts
	return computeHeldKarp<edgtype, edgvectype, edgresview,  8,  128>(pb, edgematrix, bestLowerBound);
      case   96: // if (edgestride > ( 64 * SIMDsize))		//  384 edges max, 15 wave-fronts
  	return computeHeldKarp<edgtype, edgvectype, edgresview, 10,   96>(pb, edgematrix, bestLowerBound);
      case   80: // if (edgestride > ( 64 * SIMDsize))		//  320 edges max, 15 wave-fronts
  	return computeHeldKarp<edgtype, edgvectype, edgresview, 12,   80>(pb, edgematrix, bestLowerBound);
      case   64: // if (edgestride > ( 32 * SIMDsize))		//  256 edges max, 16 wave-fronts
	return computeHeldKarp<edgtype, edgvectype, edgresview, 16,   64>(pb, edgematrix, bestLowerBound);
	*/
      case   32: // if (edgestride > ( 16 * SIMDsize))		//  128 edges max, square area, smallest tilestride at maximum threads
	return computeHeldKarp<edgtype, edgvectype, edgresview, 32,   32>(pb, edgematrix, bestLowerBound);
      case   24: // if (edgestride > ( 12 * SIMDsize))		//   96 edges max, square area
	return computeHeldKarp<edgtype, edgvectype, edgresview, 24,   24>(pb, edgematrix, bestLowerBound);
      case   16: // if (edgestride > (  8 * SIMDsize))		//   64 edges max, square area
        return computeHeldKarp<edgtype, edgvectype, edgresview, 16,   16>(pb, edgematrix, bestLowerBound);
      case    8: // if (edgestride > (  4 * SIMDsize))		//   32 edges max, square area, smallest thread-group size AMD
        return computeHeldKarp<edgtype, edgvectype, edgresview,  8,    8>(pb, edgematrix, bestLowerBound);
      case    4: // if (edgestride > (  2 * SIMDsize))		//   16 edges max, square area, smallest thread-group size REF
        return computeHeldKarp<edgtype, edgvectype, edgresview,  4,    4>(pb, edgematrix, bestLowerBound);
      default:
	abort(); return false;
    }
  }

public:
  Path() { mst.lock(); curr_memory += sizeof(Path) + sizeof(idxtype) * (edgestride); peak_memory = max(peak_memory, curr_memory); mst.unlock(); }
  virtual ~Path() { mst.lock(); curr_memory -= sizeof(Path) + sizeof(idxtype) * (edgestride); num_iterations += this->lambdaSteps; mst.unlock(); }

  static void exit(int pb) {
    for (int p = 0; p < pb; p++) {
      glob_memory += PathMem::wghtmatrix [p] ? sizeof(sumvectype) * (numnodes * edgesimds) : 0;
      glob_memory += PathMem::weights    [p] ? sizeof(sumvectype) * (           edgesimds) : 0;
      glob_memory += PathMem::parent     [p] ? sizeof(idxvectype) * (           edgesimds) : 0;
      glob_memory += PathMem::parentx    [p] ? sizeof(idxtype   ) * (           edgesimds) : 0;
      glob_memory += PathMem::mstree     [p] ? sizeof(sumvectype) * (           rdctsimds) : 0;
      glob_memory += PathMem::connections[p] ? sizeof(idxvectype) * (           rdctsimds) : 0;

      if (PathMem::wghtmatrix [p]) delete PathMem::wghtmatrix [p];
      if (PathMem::weights    [p]) delete PathMem::weights    [p];
      if (PathMem::parent     [p]) delete PathMem::parent     [p];
      if (PathMem::parentx    [p]) delete PathMem::parentx    [p];
      if (PathMem::mstree     [p]) delete PathMem::mstree     [p];
      if (PathMem::connections[p]) delete PathMem::connections[p];
    }

    SBMatrix::exit(pb);
  }

  template<typename edgtype, typename edgvectype, typename edgresview>
  static Path *setup(int pb, int numnodes, int edgestride, edgresview &edgematrix, sumtype bestLowerBound) {
    Path *root = new Path();

    /* round to full ulongs */
    PathMem::numnodes   =  numnodes;
    PathMem::edgestride = (edgestride +  3) & ( ~3);
    PathMem::edgesimds  =  edgestride / SIMDsize;
    PathMem::rdctstride = (edgestride +  7) & ( ~7);
    PathMem::rdctsimds  =  rdctstride / SIMDsize;
    PathMem::boolstride = (edgestride + 31) & (~31);

    /* get next 2^x which fits the number of possible edges */
    DWORD lg = 0;
    _BitScanReverse(&lg, ((edgestride << 1) - 1) | 1);
    PathMem::edgelog2 = squared_divisible_by(1 << lg, rdctsimds, 64 /*WAVEFRONT_SIZE*/, SIMDsize);

    /* clear out exclusion-matrix */
    root->SBMatrix::setup(pb, numnodes, edgestride);

    root->depth = 0;
    root->seed = NULL;
    root->computeHeldKarp<edgtype, edgvectype, edgresview>(0, edgematrix, bestLowerBound);

    return root;
  }

  template<typename edgtype, typename edgvectype, typename edgresview>
  Path *permute(int pb, int i, int j, edgresview &edgematrix, sumtype bestLowerBound) {
    /* memory allocation limit */
    if (curr_memory >= memory_limit)
      return NULL;
    /* loop-detection: depth exceeds the possible number of connections */
    if (this->depth >= numnodes)
      return NULL;

    Path *child = new Path();
    if (!child) {
      /* soft-abort, we can continue ignoring this
       * later maybe some memory is free again
       * won't find the optimal solution anymore of course
       */
      return child;
    }

    /* permute exclusion-matrix */
    child->SBMatrix::permute(pb, i, j, *this);

    child->depth = this->depth + 1;
    child->seed = this;
    if (!child->computeHeldKarp<edgtype, edgvectype, edgresview>(pb, edgematrix, bestLowerBound)) {
      delete child;
      child = NULL;
    }

    assert(child->lowerBound < bestLowerBound);
    assert(child->lowestSubgraph < numnodes);
    assert(child->lowestSubgraph > 0);

    return child;
  }
};

PathMem<float4, int4>::wghtgrid *PathMem<float4, int4>::wghtmatrix[PARALLEL_BRANCHES];
PathMem<float4, int4>::sumarray *PathMem<float4, int4>::weights[PARALLEL_BRANCHES];
PathMem<float4, int4>::sumarray *PathMem<float4, int4>::mstree[PARALLEL_BRANCHES];
PathMem<float4, int4>::idxarray *PathMem<float4, int4>::parent[PARALLEL_BRANCHES];
PathMem<float4, int4>::idxtrans *PathMem<float4, int4>::parentx[PARALLEL_BRANCHES];
PathMem<float4, int4>::idxarray *PathMem<float4, int4>::connections[PARALLEL_BRANCHES];
critical_section PathMem<float4 , int4>::a;
vector<accelerator_view> PathMem<float4, int4>::accs;

int PathMem<float4, int4>::numnodes;
int PathMem<float4, int4>::edgestride;
int PathMem<float4, int4>::edgesimds;
int PathMem<float4, int4>::edgelog2;
int PathMem<float4, int4>::rdctstride;
int PathMem<float4, int4>::rdctsimds;
int PathMem<float4, int4>::boolstride;

PathMem<double4, int4>::wghtgrid *PathMem<double4, int4>::wghtmatrix[PARALLEL_BRANCHES];
PathMem<double4, int4>::sumarray *PathMem<double4, int4>::weights[PARALLEL_BRANCHES];
PathMem<double4, int4>::sumarray *PathMem<double4, int4>::mstree[PARALLEL_BRANCHES];
PathMem<double4, int4>::idxarray *PathMem<double4, int4>::parent[PARALLEL_BRANCHES];
PathMem<double4, int4>::idxtrans *PathMem<double4, int4>::parentx[PARALLEL_BRANCHES];
PathMem<double4, int4>::idxarray *PathMem<double4, int4>::connections[PARALLEL_BRANCHES];
critical_section PathMem<double4, int4>::a;
vector<accelerator_view> PathMem<double4, int4>::accs;

int PathMem<double4, int4>::numnodes;
int PathMem<double4, int4>::edgestride;
int PathMem<double4, int4>::edgesimds;
int PathMem<double4, int4>::edgelog2;
int PathMem<double4, int4>::rdctstride;
int PathMem<double4, int4>::rdctsimds;
int PathMem<double4, int4>::boolstride;

#ifdef	CONSERVE_MEMORY
critical_section SBMatrixMem< PathMem<float4 , int4> >::r;
critical_section SBMatrixMem< PathMem<double4, int4> >::r;
vector<ulong, SSE2Allocator<ulong> > SBMatrixMem< PathMem<float4 , int4> >::continuous[PARALLEL_BRANCHES];
vector<ulong, SSE2Allocator<ulong> > SBMatrixMem< PathMem<double4, int4> >::continuous[PARALLEL_BRANCHES];
#endif

/* ********************************************************************************************
 */
#include "solver/ConcRT-Templated.cpp"

#undef	PARALLEL_BRANCHES

}
