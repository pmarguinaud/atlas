/*
 * (C) Copyright 1996-2015 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */

#ifndef atlas_functionspace_SpectralFunctionSpace_h
#define atlas_functionspace_SpectralFunctionSpace_h

#include "atlas/atlas_defines.h"
#include "atlas/FunctionSpace.h"

namespace atlas { namespace trans { class Trans; } }
namespace atlas { class Field; }
namespace atlas { class FieldSet; }

namespace atlas {
namespace functionspace {

// -------------------------------------------------------------------

class SpectralFunctionSpace : public next::FunctionSpace
{
public:

  SpectralFunctionSpace(const std::string& name, const size_t truncation);

  SpectralFunctionSpace(const std::string& name, trans::Trans& );

  virtual ~SpectralFunctionSpace();

  /// @brief Create a spectral field
  Field* createField(const std::string& name) const;
  Field* createField(const std::string& name, size_t levels) const;

  /// @brief Create a global spectral field
  Field* createGlobalField(const std::string& name) const;
  Field* createGlobalField(const std::string& name, size_t levels) const;

  void gather( const FieldSet&, FieldSet& ) const;
  void gather( const Field&, Field& ) const;

  void scatter( const FieldSet&, FieldSet& ) const;
  void scatter( const Field&, Field& ) const;

  std::string checksum( const FieldSet& ) const;
  std::string checksum( const Field& ) const;

public: // methods

  size_t nb_spectral_coefficients() const;
  size_t nb_spectral_coefficients_global() const;

private: // data

  size_t truncation_;

  trans::Trans* trans_;

};

// -------------------------------------------------------------------
// C wrapper interfaces to C++ routines
#define Trans trans::Trans
extern "C"
{
  SpectralFunctionSpace* atlas__SpectralFunctionSpace__new__name_truncation (const char* name, int truncation);
  SpectralFunctionSpace* atlas__SpectralFunctionSpace__new__name_trans (const char* name, Trans* trans);
  void atlas__SpectralFunctionSpace__delete (SpectralFunctionSpace* This);

  Field* atlas__SpectralFunctionSpace__create_field (const SpectralFunctionSpace* This, const char* name);

  Field* atlas__SpectralFunctionSpace__create_field_lev (const SpectralFunctionSpace* This, const char* name, int levels);

  Field* atlas__SpectralFunctionSpace__create_global_field (const SpectralFunctionSpace* This, const char* name);

  Field* atlas__SpectralFunctionSpace__create_global_field_lev (const SpectralFunctionSpace* This, const char* name, int levels);

}
#undef Trans

} // namespace functionspace
} // namespace atlas

#endif // atlas_functionspace_SpectralFunctionSpace_h