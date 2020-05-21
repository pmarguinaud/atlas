#include "GatherScatter.h"

#include "atlas/array.h"
#include "atlas/field.h"
#include "atlas/grid.h"
#include "atlas/parallel/mpi/mpi.h"
#include "atlas/runtime/Trace.h"

namespace
{

template <typename T, typename I>
T reorder (const T & vec, const I & ord)
{
  T v;
  v.reserve (ord.size ());
  for (typename I::value_type i = 0; i < ord.size (); i++)
    v.push_back (vec[ord[i]]);
  return v;
}

template <typename T>
void integrate (T & v)
{
  v[0].off = 0;
  for (size_t i = 1; i < v.size (); i++)
    v[i].off = v[i-1].off + v[i-1].len;
}

template <typename I, typename F>
std::vector<atlas::idx_t> grep (I n, F f)
{
  std::vector<atlas::idx_t> r (n), g;
  std::iota (r.begin (), r.end (), 0);
  std::copy_if (r.begin (), r.end (), std::back_inserter (g), f);
  return g;
}

};

GatherScatter::GatherScatter (const atlas::StructuredGrid & _grid, const atlas::grid::Distribution & _dist)
 : grid (_grid), dist (_dist)
{
ATLAS_TRACE_SCOPE ("GatherScatter::GatherScatter")
{

  nprc = dist.nb_partitions ();

  std::vector<atlas::gidx_t> count (nprc);
  std::vector<atlas::idx_t> ind (grid.size ());

  for (atlas::gidx_t i = 0; i < grid.size (); i++)
    ind[i] = count[dist.partition (i)]++;

  max = *std::max_element (count.begin (), count.end ());

  _prcloc2glo.resize (max * nprc, std::numeric_limits<atlas::gidx_t>::min ());
  _glo2prcloc.resize (grid.size ());

  for (atlas::gidx_t i = 0; i < grid.size (); i++)
    {
      _prcloc2glo[max * dist.partition (i) + ind[i]] = i;
      _glo2prcloc[i].loc = ind[i];
      _glo2prcloc[i].prc = dist.partition (i);
    }

}
}

void GatherScatter::reOrderFields (ioFieldDesc_v & floc, ioFieldDesc_v & fglo) const
{
  ATLAS_ASSERT (floc.size () == fglo.size ());
  atlas::idx_t nfld = fglo.size ();  

  // Sort fields by owner
  
  std::vector<atlas::idx_t> isort (nfld);

  std::iota (std::begin (isort), std::end (isort), 0);
  std::sort (std::begin (isort), std::end (isort), 
             [&fglo] (atlas::idx_t a, atlas::idx_t b) 
             { return fglo[a].owner () < fglo[b].owner (); });

  floc  = reorder (floc,  isort);
  fglo  = reorder (fglo,  isort);

  for (int jfld = 0; jfld < nfld; jfld++)
    floc[jfld].owner () = fglo[jfld].owner ();

}

void GatherScatter::gather (ioFieldDesc_v & floc, ioFieldDesc_v & fglo) const
{
ATLAS_TRACE_SCOPE ("GatherScatter::gather")
{

  ATLAS_ASSERT (floc.size () == fglo.size ());
  atlas::idx_t nfld = floc.size ();

  auto & comm = eckit::mpi::comm ();
  atlas::idx_t nprc = comm.size ();
  atlas::idx_t lprc = comm.rank ();
  atlas::idx_t ldim = dist.nb_pts ()[lprc];

  reOrderFields (floc, fglo);

  // SEND

  fldprc_t loc;

  loc.fld.resize (nfld + 1);
  loc.prc.resize (nprc + 1);

  for (atlas::idx_t jfld = 0; jfld < nfld; jfld++)
    {
      atlas::idx_t owner = floc[jfld].owner ();
      loc.fld[jfld].len = floc[jfld].size ();
      loc.prc[owner].len += floc[jfld].size ();
    }

  integrate (loc.fld);
  integrate (loc.prc);

  // RECV

  fldprc_t glo;

  glo.prc.resize (nprc + 1);
  glo.fld.resize (nfld + 1);

  for (atlas::idx_t jfld = 0; jfld < nfld; jfld++)
    if (lprc == fglo[jfld].owner ())
      glo.fld[jfld].len = fglo[jfld].dlen ();

  integrate (glo.fld);

  for (atlas::idx_t iprc = 0; iprc < nprc; iprc++)
     glo.prc[iprc].len = dist.nb_pts ()[iprc] * glo.fld.back ().off;

  integrate (glo.prc);

  // Pack send buffer

  std::vector<byte> buf_loc (loc.fld.back ().off);

  ATLAS_TRACE_SCOPE ("Pack")
  {
#pragma omp parallel for
    for (atlas::idx_t jfld = 0; jfld < nfld; jfld++)
      {
        auto & f = floc[jfld];
        byte * buffer = &buf_loc[loc.fld[jfld].off];
        for (atlas::idx_t i = 0; i < f.ldim (); i++)
        for (int j = 0; j < f.dlen (); j++)
          buffer[i*f.dlen ()+j] = f (i, j);
      }
  }

  std::vector<byte> buf_glo (glo.prc.back ().off);

  ATLAS_TRACE_SCOPE ("SEND/RECV")
  {
    std::vector<eckit::mpi::Request> rqr;
   
    for (atlas::idx_t iprc = 0; iprc < nprc; iprc++)
      if (glo.prc[iprc].len > 0)
        rqr.push_back (comm.iReceive (&buf_glo[glo.prc[iprc].off], 
                                      glo.prc[iprc].len, iprc, 100));
   
    comm.barrier ();
   
    std::vector<eckit::mpi::Request> rqs;
   
    for (atlas::idx_t iprc = 0; iprc < nprc; iprc++)
      if (loc.prc[iprc].len > 0)
        rqs.push_back (comm.iSend (&buf_loc[loc.prc[iprc].off], 
                                   loc.prc[iprc].len, iprc, 100));
   
    for (auto & r : rqr)
      comm.wait (r);
   
    for (auto & r : rqs)
      comm.wait (r);
  }

  ATLAS_TRACE_SCOPE ("Unpack")
  {
    std::vector<atlas::idx_t> prcs = grep (nprc, 
       [&glo] (atlas::idx_t i) { return glo.prc[i].len > 0; });
    std::vector<atlas::idx_t> flds = grep (nfld, 
       [&glo] (atlas::idx_t i) { return glo.fld[i].len > 0; });

#ifdef UNDEF
    for (atlas::idx_t iprc = 0; iprc < nprc; iprc++)
      if (glo.prc[iprc].len > 0)
        {
          for (atlas::idx_t jfld = 0; jfld < nfld; jfld++)
            {
              if (glo.fld[jfld].len > 0)
#endif

    for (int ii = 0; ii < prcs.size (); ii++)
    for (int jj = 0; jj < flds.size (); jj++)
                {
                  const atlas::idx_t iprc = prcs[ii];
                  const atlas::idx_t jfld = flds[jj];
                  const atlas::idx_t ngptot = dist.nb_pts ()[iprc];
                  const size_t off = glo.prc[iprc].off + ngptot * glo.fld[jfld].off;
                  auto & f = fglo[jfld];
                  const size_t len = glo.fld[jfld].len;
#pragma omp parallel for 
                  for (atlas::idx_t jloc = 0; jloc < ngptot; jloc++)
                  for (int j = 0; j < len; j++)
                    {
                      size_t k = jloc * glo.fld[jfld].len + j;
                      f (prcloc2glo (iprc, jloc), j) = buf_glo[off+k];
                    }

                }

  }


}
}
