#pragma once


#include "ioFieldDesc.h"
#include "atlas/grid.h"
#include "atlas/util/vector.h"
#include "atlas/parallel/mpi/mpi.h"

class GatherScatter
{
private:

  class locprc_t
  {
  public:
    atlas::idx_t loc = std::numeric_limits<atlas::idx_t>::min ();
    atlas::idx_t prc = std::numeric_limits<atlas::idx_t>::min ();
  };

  class offlen_t
  {
  public:
    atlas::gidx_t off = 0, len = 0;
  };

  using offlen_v = std::vector<offlen_t>;

  class fldprc_t
  {
  public:
    offlen_v prc;
    offlen_v fld;
  };

  using ioFieldDesc_v = std::vector<ioFieldDesc>;

  using byte_v = atlas::vector<byte>;

  atlas::idx_t max, nprc;

  std::vector<atlas::gidx_t> _prcloc2glo;
  std::vector<locprc_t> _glo2prcloc;

  const atlas::grid::Distribution & dist;

public:
  GatherScatter (const atlas::grid::Distribution & _dist);

  void gather (const ioFieldDesc_v & _floc, ioFieldDesc_v & fglo) const;
  void scatter (const ioFieldDesc_v & _fglo, ioFieldDesc_v & floc) const;

private:
  atlas::gidx_t prcloc2glo (atlas::idx_t iprc, atlas::idx_t jloc) const
  {
    return _prcloc2glo[iprc * max + jloc];
  }

  const locprc_t & glo2prcloc (atlas::gidx_t jglo) const
  {
    return _glo2prcloc[jglo];
  }
 
  void reOrderFields (ioFieldDesc_v & floc, ioFieldDesc_v & fglo) const;
  fldprc_t computeTLoc (const ioFieldDesc_v & floc) const;
  fldprc_t computeTGlo (const ioFieldDesc_v & fglo) const;

  template <typename A>
  void processLocBuffer (ioFieldDesc_v & floc, const fldprc_t & tloc,
                         byte_v & buf_loc, A a) const;
  template <typename A>
  void processGloBuffer (ioFieldDesc_v & fglo, const fldprc_t & tglo,
                         byte_v & buf_glo, A a) const;

  std::vector<eckit::mpi::Request> postRecv (byte_v & buf, const fldprc_t & t) const;
  std::vector<eckit::mpi::Request> postSend (const byte_v & buf, const fldprc_t & t) const;

};
