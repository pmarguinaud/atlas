/*
 * (C) Copyright 1996-2014 ECMWF.
 * 
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0. 
 * In applying this licence, ECMWF does not waive the privileges and immunities 
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */



#ifndef FunctionSpace_hpp
#define FunctionSpace_hpp

#include <vector>
#include <map>
#include <string>
#include <iostream>

#include "atlas/mpl/HaloExchange.hpp"
#include "atlas/mpl/Gather.hpp"
#include "atlas/mesh/Metadata.hpp"

namespace atlas {
class Field;
template <typename T> class FieldT;

/// @todo
// Horizontal nodes are always the slowest moving index
// Then variables
// Then levels are fastest moving index
class FunctionSpace {

public: // methods

  FunctionSpace(const std::string& name, const std::string& shape_func, const std::vector<int>& extents);

  virtual ~FunctionSpace();

  const std::string& name() const { return name_; }

  int index() const { return idx_; }

  Field& field( size_t ) const;
  Field& field(const std::string& name) const;

  template< typename DATA_TYPE>
  FieldT<DATA_TYPE>& field(const std::string& name) const;

  bool has_field(const std::string& name) const { return index_.count(name); }

  template< typename DATA_TYPE >
  FieldT<DATA_TYPE>& create_field(const std::string& name, size_t nb_vars);

  void remove_field(const std::string& name);

  // This is a Fortran view of the extents (i.e. reverse order)
  const std::vector<int>& boundsf() const { return bounds_; }
  const std::vector<int>& extents() const { return extents_; }

  void resize( const std::vector<int>& extents );

  void parallelise(const int proc[], const int glb_idx[], const int master_glb_idx[]);
  void parallelise();

  template< typename DATA_TYPE >
  void halo_exchange( DATA_TYPE field_data[], int field_size )
  {
    int nb_vars = field_size/dof_;
    if( dof_*nb_vars != field_size )
    {
      std::cout << "ERROR in FunctionSpace::halo_exchange" << std::endl;
      std::cout << "field_size = " << field_size << std::endl;
      std::cout << "dof_ = " << dof_ << std::endl;
    }
    halo_exchange_.execute( field_data, nb_vars );
  }

  const HaloExchange& halo_exchange() { return halo_exchange_; }

  template< typename DATA_TYPE >
  void gather( DATA_TYPE field_data[], int field_size, DATA_TYPE glbfield_data[], int glbfield_size )
  {
    int nb_vars = field_size/dof_;
    if( dof_*nb_vars != field_size ) std::cout << "ERROR in FunctionSpace::gather" << std::endl;
    if( glb_dof_*nb_vars != glbfield_size ) std::cout << "ERROR in FunctionSpace::gather" << std::endl;

    gather_.execute( field_data, glbfield_data, nb_vars );
  }

  const Gather& gather() { return gather_; }

  void set_index(int idx) { idx_ = idx; }

  Metadata& metadata() { return metadata_; }

  template< typename ValueT >
  const ValueT& metadata(const std::string name) const
  {
    return metadata_.get<ValueT>(name);
  }

  int nb_fields() const { return fields_.size(); }

  int dof() const { return dof_; }

  int glb_dof() const { return glb_dof_; }

protected: // members

  int idx_;
  int dof_;
  int glb_dof_;

  std::string name_;
  std::vector<int> bounds_; // deprecated, use extents which is reverse order
  std::vector<int> extents_;
  std::map< std::string, size_t > index_;
  std::vector< Field* > fields_;

  HaloExchange halo_exchange_;
  Gather gather_;
  Metadata metadata_;

private: // copy not allowed

    FunctionSpace(const FunctionSpace&);
    FunctionSpace& operator=(const FunctionSpace&);

};


// ------------------------------------------------------------------
// C wrapper interfaces to C++ routines
extern "C" 
{
  FunctionSpace* atlas__FunctionSpace__new (char* name, char* shape_func, int bounds[], int bounds_size);
  void atlas__FunctionSpace__delete (FunctionSpace* This);
  int atlas__FunctionSpace__dof (FunctionSpace* This);
  int atlas__FunctionSpace__glb_dof (FunctionSpace* This);
  void atlas__FunctionSpace__create_field_int (FunctionSpace* This, char* name, int nb_vars);
  void atlas__FunctionSpace__create_field_float (FunctionSpace* This, char* name, int nb_vars);
  void atlas__FunctionSpace__create_field_double (FunctionSpace* This, char* name, int nb_vars);
  void atlas__FunctionSpace__remove_field (FunctionSpace* This, char* name);
  int atlas__FunctionSpace__has_field (FunctionSpace* This, char* name);
  const char* atlas__FunctionSpace__name (FunctionSpace* This);
  void atlas__FunctionSpace__boundsf (FunctionSpace* This, int* &bounds, int &rank);
  Field* atlas__FunctionSpace__field (FunctionSpace* This, char* name);
  void atlas__FunctionSpace__parallelise (FunctionSpace* This, int proc[], int glb_idx[], int master_glb_idx[]);
  void atlas__FunctionSpace__halo_exchange_int (FunctionSpace* This, int field_data[], int field_size); 
  void atlas__FunctionSpace__halo_exchange_float (FunctionSpace* This, float field_data[], int field_size); 
  void atlas__FunctionSpace__halo_exchange_double (FunctionSpace* This, double field_data[], int field_size); 
  void atlas__FunctionSpace__gather_int (FunctionSpace* This, int field_data[], int field_size, int glbfield_data[], int glbfield_size);
  void atlas__FunctionSpace__gather_float (FunctionSpace* This, float field_data[], int field_size, float glbfield_data[], int glbfield_size);
  void atlas__FunctionSpace__gather_double (FunctionSpace* This, double field_data[], int field_size, double glbfield_data[], int glbfield_size);
  HaloExchange const* atlas__FunctionSpace__halo_exchange (FunctionSpace* This);

}
// ------------------------------------------------------------------

} // namespace atlas

#endif // functionspace_hpp