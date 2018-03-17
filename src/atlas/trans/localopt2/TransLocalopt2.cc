/*
 * (C) Copyright 2013 ECMWF.
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 * In applying this licence, ECMWF does not waive the privileges and immunities
 * granted to it by virtue of its status as an intergovernmental organisation
 * nor does it submit to any jurisdiction.
 */

#include "atlas/trans/localopt2/TransLocalopt2.h"
#include <math.h>
#include "atlas/array.h"
#include "atlas/option.h"
#include "atlas/parallel/mpi/mpi.h"
#include "atlas/runtime/ErrorHandling.h"
#include "atlas/runtime/Log.h"
#include "atlas/trans/VorDivToUV.h"
#include "atlas/trans/local/LegendrePolynomials.h"
#include "atlas/trans/localopt2/FourierTransformsopt2.h"
#include "atlas/trans/localopt2/LegendrePolynomialsopt2.h"
#include "atlas/trans/localopt2/LegendreTransformsopt2.h"
#include "atlas/util/Constants.h"
#include "eckit/linalg/LinearAlgebra.h"
#include "eckit/linalg/Matrix.h"
#ifdef ATLAS_HAVE_MKL
#include "mkl.h"
#endif

namespace atlas {
namespace trans {

namespace {
static TransBuilderGrid<TransLocalopt2> builder( "localopt2" );
}

// --------------------------------------------------------------------------------------------------------------------
// Helper functions
// --------------------------------------------------------------------------------------------------------------------
namespace {  // anonymous

size_t legendre_size( const size_t truncation ) {
    return ( truncation + 2 ) * ( truncation + 1 ) / 2;
}

int nlats_northernHemisphere( const int nlats ) {
    return ceil( nlats / 2. );
    // using ceil here should make it possible to have odd number of latitudes (with the centre latitude being the equator)
}

int num_n( const int truncation, const int m, const bool symmetric ) {
    int len = 0;
    if ( symmetric ) { len = ( truncation - m + 2 ) / 2; }
    else {
        len = ( truncation - m + 1 ) / 2;
    }
    return len;
}

void alloc_aligned( double*& ptr, size_t n ) {
#ifdef ATLAS_HAVE_MKL
    int al = 64;
    ptr    = mkl_malloc( sizeof( double ) * n, al );
#else
    posix_memalign( (void**)&ptr, sizeof( double ) * 64, sizeof( double ) * n );
    //ptr = (double*)malloc( sizeof( double ) * n );
    //ptr = new double[n];
#endif
}

void free_aligned( double*& ptr ) {
#ifdef ATLAS_HAVE_MKL
    mkl_free( ptr );
#else
    free( ptr );
#endif
}

int add_padding( int n ) {
    return std::ceil( n / 8. ) * 8;
}
}  // namespace

// --------------------------------------------------------------------------------------------------------------------
// Class TransLocalopt2
// --------------------------------------------------------------------------------------------------------------------

TransLocalopt2::TransLocalopt2( const Cache& cache, const Grid& grid, const long truncation,
                                const eckit::Configuration& config ) :
    grid_( grid ),
    truncation_( truncation ),
    precompute_( config.getBool( "precompute", true ) ) {
    ATLAS_TRACE( "Precompute legendre opt2" );
    eckit::linalg::LinearAlgebra::backend( "generic" );  // might want to choose backend with this command
    int nlats   = 0;
    int nlons   = 0;
    int nlatsNH = nlats_northernHemisphere( nlats );
    if ( grid::StructuredGrid( grid_ ) && not grid_.projection() ) {
        grid::StructuredGrid g( grid_ );
        nlats   = g.ny();
        nlons   = g.nxmax();
        nlatsNH = nlats_northernHemisphere( nlats );
    }
    else {
        nlats   = grid_.size();
        nlons   = grid_.size();
        nlatsNH = nlats;
    }
    std::vector<double> lats( nlatsNH );
    std::vector<double> lons( nlons );
    if ( grid::StructuredGrid( grid_ ) && not grid_.projection() ) {
        grid::StructuredGrid g( grid_ );
        // TODO: remove legendre_begin and legendre_data (only legendre_ should be needed)
        for ( size_t j = 0; j < nlatsNH; ++j ) {
            lats[j] = g.y( j ) * util::Constants::degreesToRadians();
        }
        for ( size_t j = 0; j < nlons; ++j ) {
            lons[j] = g.x( j, 0 ) * util::Constants::degreesToRadians();
        }
    }
    else {
        int j( 0 );
        for ( PointXY p : grid_.xy() ) {
            lats[j++] = p.y() * util::Constants::degreesToRadians();
            lons[j++] = p.x() * util::Constants::degreesToRadians();
        }
    }
    // precomputations for Legendre polynomials:
    {
        ATLAS_TRACE( "opt2 precomp Legendre" );
        int size_sym  = 0;
        int size_asym = 0;
        legendre_sym_begin_.resize( truncation_ + 3 );
        legendre_asym_begin_.resize( truncation_ + 3 );
        legendre_sym_begin_[0]  = 0;
        legendre_asym_begin_[0] = 0;
        for ( int jm = 0; jm <= truncation_ + 1; jm++ ) {
            size_sym += add_padding( num_n( truncation_ + 1, jm, true ) * nlatsNH );
            size_asym += add_padding( num_n( truncation_ + 1, jm, false ) * nlatsNH );
            legendre_sym_begin_[jm + 1]  = size_sym;
            legendre_asym_begin_[jm + 1] = size_asym;
        }
        alloc_aligned( legendre_sym_, size_sym );
        alloc_aligned( legendre_asym_, size_asym );
        compute_legendre_polynomialsopt2( truncation_ + 1, nlatsNH, lats.data(), legendre_sym_, legendre_asym_,
                                          legendre_sym_begin_.data(), legendre_asym_begin_.data() );
    }

    // precomputations for Fourier transformations:
    {
        ATLAS_TRACE( "opt2 precomp Fourier" );
        alloc_aligned( fourier_, 2 * ( truncation_ + 1 ) * nlons );
        int idx = 0;
        for ( int jlon = 0; jlon < nlons; jlon++ ) {
            for ( int jm = 0; jm < truncation_ + 1; jm++ ) {
                fourier_[idx++] = +std::cos( jm * lons[jlon] );  // real part
                fourier_[idx++] = -std::sin( jm * lons[jlon] );  // imaginary part
            }
        }
    }
    {
        ATLAS_TRACE( "opt2 precomp Fourier tp" );
        alloc_aligned( fouriertp_, 2 * ( truncation_ + 1 ) * nlons );
        int idx = 0;
        for ( int jm = 0; jm < truncation_ + 1; jm++ ) {
            for ( int jlon = 0; jlon < nlons; jlon++ ) {
                fouriertp_[idx++] = +std::cos( jm * lons[jlon] );  // real part
            }
            for ( int jlon = 0; jlon < nlons; jlon++ ) {
                fouriertp_[idx++] = -std::sin( jm * lons[jlon] );  // imaginary part
            }
        }
    }
#if ATLAS_HAVE_FFTW
    {
        ATLAS_TRACE( "opt2 precomp FFTW" );
        int num_complex = ( nlons / 2 ) + 1;
        fft_in_         = fftw_alloc_complex( nlats * num_complex );
        fft_out_        = fftw_alloc_real( nlats * nlons );
        plan_ = fftw_plan_many_dft_c2r( 1, &nlons, nlats, fft_in_, NULL, 1, num_complex, fft_out_, NULL, 1, nlons,
                                        FFTW_ESTIMATE );
    }
#endif
}  // namespace atlas

// --------------------------------------------------------------------------------------------------------------------

TransLocalopt2::TransLocalopt2( const Grid& grid, const long truncation, const eckit::Configuration& config ) :
    TransLocalopt2( Cache(), grid, truncation, config ) {}

// --------------------------------------------------------------------------------------------------------------------

TransLocalopt2::~TransLocalopt2() {
    free_aligned( legendre_sym_ );
    free_aligned( legendre_asym_ );
    free_aligned( fourier_ );
    free_aligned( fouriertp_ );
#if ATLAS_HAVE_FFTW
    fftw_destroy_plan( plan_ );
    fftw_free( fft_in_ );
    fftw_free( fft_out_ );
#endif
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans( const Field& spfield, Field& gpfield, const eckit::Configuration& config ) const {
    NOTIMP;
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans( const FieldSet& spfields, FieldSet& gpfields,
                               const eckit::Configuration& config ) const {
    NOTIMP;
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans_grad( const Field& spfield, Field& gradfield, const eckit::Configuration& config ) const {
    NOTIMP;
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans_grad( const FieldSet& spfields, FieldSet& gradfields,
                                    const eckit::Configuration& config ) const {
    NOTIMP;
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans_vordiv2wind( const Field& spvor, const Field& spdiv, Field& gpwind,
                                           const eckit::Configuration& config ) const {
    NOTIMP;
}

void TransLocalopt2::invtrans( const int nb_scalar_fields, const double scalar_spectra[], double gp_fields[],
                               const eckit::Configuration& config ) const {
    invtrans_uv( truncation_, nb_scalar_fields, 0, scalar_spectra, gp_fields, config );
}

void gp_transposeopt2( const int nb_size, const int nb_fields, const double gp_tmp[], double gp_fields[] ) {
    for ( int jgp = 0; jgp < nb_size; jgp++ ) {
        for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
            gp_fields[jfld * nb_size + jgp] = gp_tmp[jgp * nb_fields + jfld];
        }
    }
}

//-----------------------------------------------------------------------------
// Routine to compute the spectral transform by using a localopt2 Fourier
// transformation
// for a grid (same latitude for all longitudes, allows to compute Legendre
// functions
// once for all longitudes). U and v components are divided by cos(latitude) for
// nb_vordiv_fields > 0.
//
// Author:
// Andreas Mueller *ECMWF*
//
void TransLocalopt2::invtrans_uv( const int truncation, const int nb_scalar_fields, const int nb_vordiv_fields,
                                  const double scalar_spectra[], double gp_fields[],
                                  const eckit::Configuration& config ) const {
    if ( nb_scalar_fields > 0 ) {
        int nb_fields = nb_scalar_fields;

        // Transform
        if ( grid::StructuredGrid g = grid_ ) {
            ATLAS_TRACE( "invtrans_uv structured opt2" );
            int nlats            = g.ny();
            int nlons            = g.nxmax();
            int nlatsNH          = nlats_northernHemisphere( nlats );
            int size_fourier_max = nb_fields * 2 * nlats;
            double* scl_fourier;
            alloc_aligned( scl_fourier, size_fourier_max * ( truncation + 1 ) );

            // Legendre transform:
            {
                ATLAS_TRACE( "opt2 Legendre dgemm" );
                for ( int jm = 0; jm <= truncation_ + 1; jm++ ) {
#if 1  // 0: no symmetry, 1: use symmetry \
    // TODO: 0 is currently not working because it requires all latitudes to be included in legendre_ (which is currently not done)
                    int size_sym  = num_n( truncation_ + 1, jm, true );
                    int size_asym = num_n( truncation_ + 1, jm, false );
                    int n_imag    = 2;
                    if ( jm == 0 ) { n_imag = 1; }
                    int size_fourier = nb_fields * n_imag * nlatsNH;
                    double* scalar_sym;
                    double* scalar_asym;
                    double* scl_fourier_sym;
                    double* scl_fourier_asym;
                    alloc_aligned( scalar_sym, n_imag * nb_fields * size_sym );
                    alloc_aligned( scalar_asym, n_imag * nb_fields * size_asym );
                    alloc_aligned( scl_fourier_sym, size_fourier );
                    alloc_aligned( scl_fourier_asym, size_fourier );
                    {
                        //ATLAS_TRACE( "opt2 Legendre split" );
                        int idx = 0, is = 0, ia = 0, ioff = ( 2 * truncation + 3 - jm ) * jm / 2 * nb_fields * 2;
                        // the choice between the following two code lines determines whether
                        // total wavenumbers are summed in an ascending or descending order.
                        // The trans library in IFS uses descending order because it should
                        // be more accurate (higher wavenumbers have smaller contributions).
                        // This also needs to be changed when splitting the spectral data in
                        // compute_legendre_polynomialsopt2!
                        //for ( int jn = jm; jn <= truncation_ + 1; jn++ ) {
                        for ( int jn = truncation_ + 1; jn >= jm; jn-- ) {
                            for ( int imag = 0; imag < n_imag; imag++ ) {
                                for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                                    idx = jfld + nb_fields * ( imag + 2 * ( jn - jm ) );
                                    if ( ( jn - jm ) % 2 == 0 ) { scalar_sym[is++] = scalar_spectra[idx + ioff]; }
                                    else {
                                        scalar_asym[ia++] = scalar_spectra[idx + ioff];
                                    }
                                }
                            }
                        }
                        ASSERT( ia == n_imag * nb_fields * size_asym && is == n_imag * nb_fields * size_sym );
                    }
                    {
                        eckit::linalg::Matrix A( scalar_sym, nb_fields * n_imag, size_sym );
                        eckit::linalg::Matrix B( legendre_sym_ + legendre_sym_begin_[jm], size_sym, nlatsNH );
                        eckit::linalg::Matrix C( scl_fourier_sym, nb_fields * n_imag, nlatsNH );
                        eckit::linalg::LinearAlgebra::backend().gemm( A, B, C );
                    }
                    if ( size_asym > 0 ) {
                        eckit::linalg::Matrix A( scalar_asym, nb_fields * n_imag, size_asym );
                        eckit::linalg::Matrix B( legendre_asym_ + legendre_asym_begin_[jm], size_asym, nlatsNH );
                        eckit::linalg::Matrix C( scl_fourier_asym, nb_fields * n_imag, nlatsNH );
                        eckit::linalg::LinearAlgebra::backend().gemm( A, B, C );
                    }
                    {
                        //ATLAS_TRACE( "opt2 merge spheres" );
                        // northern hemisphere:
                        int ioff = jm * size_fourier_max;
                        int pos0 = ioff;
                        int idx  = 0;
                        for ( int jlat = 0; jlat < nlatsNH; jlat++ ) {
                            int poslat = pos0 + 2 * jlat;
                            for ( int imag = 0; imag < n_imag; imag++ ) {
                                int posimag = nb_fields * ( imag + poslat );
                                for ( int jfld = 0; jfld < nb_fields; jfld++, idx++ ) {
                                    int pos          = jfld + posimag;
                                    scl_fourier[pos] = scl_fourier_sym[idx] + scl_fourier_asym[idx];
                                }
                            }
                        }
                        // southern hemisphere:
                        idx  = 0;
                        pos0 = 2 * ( nlats - 1 ) + ioff;
                        for ( int jlat = 0; jlat < nlatsNH; jlat++ ) {
                            int poslat = pos0 - 2 * jlat;
                            for ( int imag = 0; imag < n_imag; imag++ ) {
                                int posimag = nb_fields * ( imag + poslat );
                                for ( int jfld = 0; jfld < nb_fields; jfld++, idx++ ) {
                                    int pos          = jfld + posimag;
                                    scl_fourier[pos] = scl_fourier_sym[idx] - scl_fourier_asym[idx];
                                }
                            }
                        }
                    }
                    free_aligned( scalar_sym );
                    free_aligned( scalar_asym );
                    free_aligned( scl_fourier_sym );
                    free_aligned( scl_fourier_asym );

#else
                    int noff = ( 2 * truncation + 3 - jm ) * jm / 2, ns = truncation - jm + 1;
                    eckit::linalg::Matrix A( eckit::linalg::Matrix(
                        const_cast<double*>( scalar_spectra ) + nb_fields * 2 * noff, nb_fields * 2, ns ) );
                    eckit::linalg::Matrix B( legendre_.data() + noff * g.ny(), ns, g.ny() );
                    eckit::linalg::Matrix C( scl_fourier.data() + jm * size_fourier, nb_fields * 2, g.ny() );
                    eckit::linalg::LinearAlgebra::backend().gemm( A, B, C );
#endif
                }
            }
#if ATLAS_HAVE_FFTW
            {
                auto position = [&]( int jfld, int imag, int jlat, int jm ) {
                    return jfld + nb_fields * ( imag + 2 * ( jlat + nlats * ( jm ) ) );
                };
                int num_complex = ( nlons / 2 ) + 1;
                {
                    ATLAS_TRACE( "opt2 FFTW" );
                    for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                        int idx = 0;
                        for ( int jlat = 0; jlat < g.ny(); jlat++ ) {
                            fft_in_[idx++][0] = scl_fourier[position( jfld, 0, jlat, 0 )];
                            for ( int jm = 1; jm < num_complex; jm++, idx++ ) {
                                for ( int imag = 0; imag < 2; imag++ ) {
                                    if ( jm <= truncation_ ) {
                                        fft_in_[idx][imag] = scl_fourier[position( jfld, imag, jlat, jm )] / 2.;
                                    }
                                    else {
                                        fft_in_[idx][imag] = 0.;
                                    }
                                }
                            }
                        }
                        fftw_execute_dft_c2r( plan_, fft_in_, fft_out_ );
                        for ( int j = 0; j < nlats * nlons; j++ ) {
                            gp_fields[j + jfld * nlats * nlons] = fft_out_[j];
                        }
                    }
                }
            }
#else
#if 0  // 1: better for small number of columns, large truncation; 0: better for large number of columns

            // Transposition in Fourier space:
            std::vector<double> scl_fourier_tp( size_fourier * ( truncation + 1 ) );
            {
                ATLAS_TRACE( "opt2 transposition in Fourier" );
                int idx = 0;
                for ( int jm = 0; jm < truncation_ + 1; jm++ ) {
                    for ( int jlat = 0; jlat < g.ny(); jlat++ ) {
                        for ( int imag = 0; imag < 2; imag++ ) {
                            for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                                int pos_tp = jfld + nb_fields * ( jlat + g.ny() * ( imag + 2 * ( jm ) ) );
                                //int pos  = jfld + nb_fields * ( imag + 2 * ( jlat + g.ny() * ( jm ) ) );
                                scl_fourier_tp[pos_tp] = scl_fourier[idx++];  // = scl_fourier[pos]
                            }
                        }
                    }
                }
            }

            // Fourier transformation:
            std::vector<double> gp_opt2( nb_fields * grid_.size(), 0. );
            {
                ATLAS_TRACE( "opt2 Fourier dgemm" );
                eckit::linalg::Matrix A( scl_fourier_tp.data(), nb_fields * g.ny(), ( truncation_ + 1 ) * 2 );
                eckit::linalg::Matrix B( fourier_.data(), ( truncation_ + 1 ) * 2, g.nxmax() );
                eckit::linalg::Matrix C( gp_opt2.data(), nb_fields * g.ny(), g.nxmax() );
                eckit::linalg::LinearAlgebra::backend().gemm( A, B, C );
            }

            // Transposition in grid point space:
            {
                ATLAS_TRACE( "opt2 transposition in gp-space" );
                int idx = 0;
                for ( int jlon = 0; jlon < g.nxmax(); jlon++ ) {
                    for ( int jlat = 0; jlat < g.ny(); jlat++ ) {
                        for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                            int pos_tp = jlon + g.nxmax() * ( jlat + g.ny() * ( jfld ) );
                            //int pos  = jfld + nb_fields * ( jlat + g.ny() * ( jlon ) );
                            gp_fields[pos_tp] = gp_opt2[idx++];  // = gp_opt2[pos]
                        }
                    }
                }
            }
#else
            // Transposition in Fourier space:
            std::vector<double> scl_fourier_tp( size_fourier * ( truncation + 1 ) );
            {
                ATLAS_TRACE( "opt2 transposition in Fourier" );
                int idx = 0;
                for ( int jm = 0; jm < truncation_ + 1; jm++ ) {
                    for ( int jlat = 0; jlat < g.ny(); jlat++ ) {
                        for ( int imag = 0; imag < 2; imag++ ) {
                            for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                                int pos_tp = imag + 2 * ( jm + ( truncation_ + 1 ) * ( jlat + g.ny() * ( jfld ) ) );
                                //int pos  = jfld + nb_fields * ( imag + 2 * ( jlat + g.ny() * ( jm ) ) );
                                scl_fourier_tp[pos_tp] = scl_fourier[idx++];  // = scl_fourier[pos]
                            }
                        }
                    }
                }
            }

            // Fourier transformation:
            std::vector<double> gp_opt2( nb_fields * grid_.size(), 0. );
            {
                ATLAS_TRACE( "opt2 Fourier dgemm" );
                eckit::linalg::Matrix A( fouriertp_.data(), g.nxmax(), ( truncation_ + 1 ) * 2 );
                eckit::linalg::Matrix B( scl_fourier_tp.data(), ( truncation_ + 1 ) * 2, nb_fields * g.ny() );
                eckit::linalg::Matrix C( gp_fields, g.nxmax(), nb_fields * g.ny() );
                eckit::linalg::LinearAlgebra::backend().gemm( A, B, C );
            }

#endif
#endif
            // Computing u,v from U,V:
            {
                if ( nb_vordiv_fields > 0 ) {
                    ATLAS_TRACE( "opt2 u,v from U,V" );
                    std::vector<double> coslats( nlats );
                    for ( size_t j = 0; j < nlats; ++j ) {
                        coslats[j] = std::cos( g.y( j ) * util::Constants::degreesToRadians() );
                    }
                    int idx = 0;
                    for ( int jfld = 0; jfld < nb_fields; jfld++ ) {
                        for ( int jlat = 0; jlat < g.ny(); jlat++ ) {
                            for ( int jlon = 0; jlon < g.nxmax(); jlon++ ) {
                                gp_fields[idx] /= coslats[jlat];
                                idx++;
                            }
                        }
                    }
                }
            }
            free_aligned( scl_fourier );
        }
        else {
            ATLAS_TRACE( "invtrans_uv unstructured opt2" );
            int idx = 0;
            for ( PointXY p : grid_.xy() ) {
                double lon   = p.x() * util::Constants::degreesToRadians();
                double lat   = p.y() * util::Constants::degreesToRadians();
                double trcFT = truncation;

                // Legendre transform:
                //invtrans_legendreopt2( truncation, trcFT, truncation_ + 1, legPol( lat, idx ), nb_fields, scalar_spectra,
                //                      legReal.data(), legImag.data() );

                // Fourier transform:
                //invtrans_fourieropt2( trcFT, lon, nb_fields, legReal.data(), legImag.data(),
                //                     gp_tmp.data() + ( nb_fields * idx ) );
                for ( int jfld = 0; jfld < nb_vordiv_fields; ++jfld ) {
                    //gp_tmp[nb_fields * idx + jfld] /= std::cos( lat );
                }
                ++idx;
            }
        }
    }
}  // namespace trans

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans( const int nb_vordiv_fields, const double vorticity_spectra[],
                               const double divergence_spectra[], double gp_fields[],
                               const eckit::Configuration& config ) const {
    invtrans( 0, nullptr, nb_vordiv_fields, vorticity_spectra, divergence_spectra, gp_fields, config );
}

void extend_truncationopt2( const int old_truncation, const int nb_fields, const double old_spectra[],
                            double new_spectra[] ) {
    int k = 0, k_old = 0;
    for ( int m = 0; m <= old_truncation + 1; m++ ) {             // zonal wavenumber
        for ( int n = m; n <= old_truncation + 1; n++ ) {         // total wavenumber
            for ( int imag = 0; imag < 2; imag++ ) {              // imaginary/real part
                for ( int jfld = 0; jfld < nb_fields; jfld++ ) {  // field
                    if ( m == old_truncation + 1 || n == old_truncation + 1 ) { new_spectra[k++] = 0.; }
                    else {
                        new_spectra[k++] = old_spectra[k_old++];
                    }
                }
            }
        }
    }
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::invtrans( const int nb_scalar_fields, const double scalar_spectra[], const int nb_vordiv_fields,
                               const double vorticity_spectra[], const double divergence_spectra[], double gp_fields[],
                               const eckit::Configuration& config ) const {
    ATLAS_TRACE( "TransLocalopt2::invtrans" );
    int nb_gp              = grid_.size();
    int nb_vordiv_spec_ext = 2 * legendre_size( truncation_ + 1 ) * nb_vordiv_fields;
    if ( nb_vordiv_fields > 0 ) {
        std::vector<double> vorticity_spectra_extended( nb_vordiv_spec_ext, 0. );
        std::vector<double> divergence_spectra_extended( nb_vordiv_spec_ext, 0. );
        std::vector<double> U_ext( nb_vordiv_spec_ext, 0. );
        std::vector<double> V_ext( nb_vordiv_spec_ext, 0. );

        {
            ATLAS_TRACE( "opt2 extend vordiv" );
            // increase truncation in vorticity_spectra and divergence_spectra:
            extend_truncationopt2( truncation_, nb_vordiv_fields, vorticity_spectra,
                                   vorticity_spectra_extended.data() );
            extend_truncationopt2( truncation_, nb_vordiv_fields, divergence_spectra,
                                   divergence_spectra_extended.data() );
        }

        {
            ATLAS_TRACE( "vordiv to UV opt2" );
            // call vd2uv to compute u and v in spectral space
            trans::VorDivToUV vordiv_to_UV_ext( truncation_ + 1, option::type( "localopt2" ) );
            vordiv_to_UV_ext.execute( nb_vordiv_spec_ext, nb_vordiv_fields, vorticity_spectra_extended.data(),
                                      divergence_spectra_extended.data(), U_ext.data(), V_ext.data() );
        }

        // perform spectral transform to compute all fields in grid point space
        invtrans_uv( truncation_ + 1, nb_vordiv_fields, nb_vordiv_fields, U_ext.data(), gp_fields, config );
        invtrans_uv( truncation_ + 1, nb_vordiv_fields, nb_vordiv_fields, V_ext.data(),
                     gp_fields + nb_gp * nb_vordiv_fields, config );
    }
    if ( nb_scalar_fields > 0 ) {
        int nb_scalar_spec_ext = 2 * legendre_size( truncation_ + 1 ) * nb_scalar_fields;
        std::vector<double> scalar_spectra_extended( nb_scalar_spec_ext, 0. );
        extend_truncationopt2( truncation_, nb_scalar_fields, scalar_spectra, scalar_spectra_extended.data() );
        invtrans_uv( truncation_ + 1, nb_scalar_fields, 0, scalar_spectra_extended.data(),
                     gp_fields + 2 * nb_gp * nb_vordiv_fields, config );
    }
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::dirtrans( const Field& gpfield, Field& spfield, const eckit::Configuration& config ) const {
    NOTIMP;
    // Not implemented and not planned.
    // Use the TransIFS implementation instead.
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::dirtrans( const FieldSet& gpfields, FieldSet& spfields,
                               const eckit::Configuration& config ) const {
    NOTIMP;
    // Not implemented and not planned.
    // Use the TransIFS implementation instead.
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::dirtrans_wind2vordiv( const Field& gpwind, Field& spvor, Field& spdiv,
                                           const eckit::Configuration& config ) const {
    NOTIMP;
    // Not implemented and not planned.
    // Use the TransIFS implementation instead.
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::dirtrans( const int nb_fields, const double scalar_fields[], double scalar_spectra[],
                               const eckit::Configuration& ) const {
    NOTIMP;
    // Not implemented and not planned.
    // Use the TransIFS implementation instead.
}

// --------------------------------------------------------------------------------------------------------------------

void TransLocalopt2::dirtrans( const int nb_fields, const double wind_fields[], double vorticity_spectra[],
                               double divergence_spectra[], const eckit::Configuration& ) const {
    NOTIMP;
    // Not implemented and not planned.
    // Use the TransIFS implementation instead.
}

// --------------------------------------------------------------------------------------------------------------------

}  // namespace trans
}  // namespace atlas
