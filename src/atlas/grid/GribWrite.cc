/*
 * (C) Copyright 1996-2014 ECMWF.
 * 
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0. 
 * In applying this licence, ECMWF does not waive the privileges and immunities 
 * granted to it by virtue of its status as an intergovernmental organisation nor
 * does it submit to any jurisdiction.
 */

#include "grib_api.h"

#include "eckit/eckit_config.h"
#include "eckit/config/Resource.h"
#include "eckit/exception/Exceptions.h"
#include "eckit/filesystem/PathName.h"
#include "eckit/utils/Translator.h"
#include "eckit/memory/ScopedPtr.h"
#include "eckit/io/DataHandle.h"
#include "eckit/io/FileHandle.h"
#include "eckit/filesystem/LocalPathName.h"
#include "eckit/parser/StringTools.h"

#include "eckit/grib/GribParams.h"
#include "eckit/grib/GribHandle.h"
#include "eckit/grib/GribMutator.h"

#include "atlas/mesh/Field.hpp"
#include "atlas/mesh/FunctionSpace.hpp"
#include "atlas/mesh/Mesh.hpp"
#include "atlas/mesh/Parameters.hpp"

#include "atlas/grid/FieldSet.h"
#include "atlas/grid/GribWrite.h"
#include "atlas/grid/GridSpec.h"
#include "atlas/grid/StackGribFile.h"

//------------------------------------------------------------------------------------------------------

using namespace std;
using namespace eckit;
using namespace eckit::grib;
using namespace atlas;
using namespace atlas::grid;

namespace atlas {
namespace grid {

static std::string map_uid_to_grib_sample_file(const std::string& short_name, long edition);

//------------------------------------------------------------------------------------------------------

Grid::Ptr GribWrite::create_grid(GribHandle& gh)
{
	GribParams* gp = GribParams::create(gh);
	ASSERT( gp );
	DEBUG_HERE;
	return Grid::create( *gp );
}

GribHandle::Ptr GribWrite::create_handle( const Grid& grid, long edition )
{
    // determine choice of editionNumber from a resorce

    if( edition == 0 )
        edition = Resource<unsigned>( "NewGribEditionNumber", 2 );

    // From the Grid get the Grid Spec
	GridSpec grid_spec = grid.spec();

    grib_handle* gh = 0;
    std::string sample_file;

    // first match GridSpec uid, directly to a samples file

	sample_file = map_uid_to_grib_sample_file( grid_spec.uid(), edition );
    if( !sample_file.empty() )
    {
        gh = grib_handle_new_from_samples(0,sample_file.c_str() );
        if( !gh )
            throw SeriousBug( "Failed to create GribHandle from sample: " + sample_file, Here() );
    }
    else // if this fails, then try looking on disk
    {
		sample_file = GribWrite::grib_sample_file(grid_spec,edition);
        if (!sample_file.empty())
        {
            gh = grib_handle_new_from_samples(0,sample_file.c_str() );
            if( !gh )
                throw SeriousBug( "Failed to create GribHandle from sample: " + sample_file, Here() );
        }
    }

    if(!gh)
        throw SeriousBug( "Failed to create GribHandle from Grib", Here() );

    return GribHandle::Ptr( new GribHandle(gh) );
}


void GribWrite::determine_grib_samples_dir(std::vector<std::string>& sample_paths)
{
   char* paths = grib_samples_path(NULL);
   if (paths) {
      // Expect <path1>:<path2>:<path3:
      // TODO: Need abstraction for path separator.
      sample_paths = StringTools::split(":", std::string(paths));
      return;
   }

   char* include_dir = getenv("GRIB_API_INCLUDE");
   if (!include_dir) throw SeriousBug(string("grib_samples_path(NULL) returned a NULL path"),Here()) ;

   std::string grib_include_dir(include_dir);
   if (grib_include_dir.find("grib_api") == std::string::npos) {
      // "grib-api not found on directory " << grib_include_dir
      return throw SeriousBug(string("grib_samples_path(NULL) returned a NULL path"),Here()) ;
   }

   if (grib_include_dir.find("-I") != std::string::npos) {
      //std::cout << "GRIB_API_INCLUDE=" << grib_include_dir << "\n";
      grib_include_dir.erase(grib_include_dir.begin(),grib_include_dir.begin()+2); // remove -I
   }

   // Handle multiple include dirs
   // If there are any spaces in the string, only take the first include
   size_t space_pos = grib_include_dir.find(" ");
   if (space_pos != std::string::npos) {
      grib_include_dir = grib_include_dir.substr(0,space_pos);
      //std::cout << "GRIB_API_INCLUDE=" << grib_include_dir << "\n";
   }

   // Remove the 'include' and replace with, 'share/grib_api/samples'
   size_t pos = grib_include_dir.find("/include");
   if ( pos == string::npos) {
      // include not found in directory " << grib_include_dir);
      throw SeriousBug(string("grib_samples_path(NULL) returned a NULL path"),Here()) ;
   }

   std::string grib_samples_dir = grib_include_dir.replace(pos,grib_include_dir.length(),"/share/grib_api/samples");
   //std::cout << " GRIB SAMPLES=" << grib_include_dir << "\n";
   sample_paths.push_back( grib_samples_dir );
}

bool match_grid_spec_with_sample_file(
        const GridSpec& g_spec,
        grib_handle& handle,
        long edition,
        const std::string& file_path )
{
    DEBUG_VAR(file_path);

    char string_value[64];
    size_t len = sizeof(string_value)/sizeof(char);
    int err = grib_get_string(&handle,"gridType",string_value,&len);
    if (err != 0) {
        //Log::error() << "GribWrite::match_grid_spec_with_sample_file, grib_get_string(gridType) failed for \nfile " << file_path << " IGNORING !! " << std::endl;
        return false;
    }

    std::string grib_grid_type = string_value;
    if ( g_spec.grid_type() != grib_grid_type ) {
        //Log::info() << "grid_type in GridSpec " << g_spec.grid_type() << " does not match " << grib_grid_type << " in samples file " << file_path << " IGNORING " << std::endl;
        return false;
    }

//    eckit::Value spec_nj = g_spec.get("Nj");
//    if (!spec_nj.isNil()) {
//        long spec_nj = spec_nj;
//        long grib_nj = 0;
//        if (grib_get_long(&handle,"Nj",&grib_nj) == 0 ) {
//            if (spec_nj != grib_nj ) {
//                //Log::info() << "GribWrite::match_grid_spec_with_sample_file, Nj in GridSpec " << spec_nj << " does not match  " << grib_nj << " in samples file " << file_path << " IGNORING " << std::endl;
//                return false;
//            }
//        }
//    }
//    eckit::Value spec_ni = g_spec.get("Ni");
//    if (!spec_ni.isNil()) {
//        long spec_ni = spec_ni;
//        long grib_ni = 0;
//        if (grib_get_long(&handle,"Ni",&grib_ni) == 0 ) {
//            if (spec_ni != grib_ni ) {
//                //Log::info() << "GribWrite::match_grid_spec_with_sample_file, Ni in GridSpec " << spec_ni << " does not match  " << grib_ni << " in samples file " << file_path << " IGNORING " << std::endl;
//                return false;
//            }
//        }
//    }

    long grib_edition = 0;
    GRIB_CHECK(grib_get_long(&handle,"editionNumber",&grib_edition),0);
    if (grib_edition != edition ) {
        //Log::info() << "GribWrite::match_grid_spec_with_sample_file, edition_number passed in " << edition << " does not match grib" << edition << " in samples file " << file_path << " IGNORING " << std::endl;
        return false;
    }

    return true;
}

std::string GribWrite::grib_sample_file( const grid::GridSpec& g_spec, long edition )
{
    // Note: many of the grib samples files are not UNIQUE in their grid specification:
    // i.e
    //   GRIB2.tmpl                        -> GridSpec[ regular_ll, LL31_16_2, Ni:16, Nj:31, typeOfLevel:surface ]
    //   regular_ll_pl_grib2.tmpl          -> GridSpec[ regular_ll, LL31_16_2, Ni:16, Nj:31 ]
    //   regular_ll_sfc_grib2.tmpl         -> GridSpec[ regular_ll, LL31_16_2, Ni:16, Nj:31 ]
    //
    //   reduced_gg_ml_grib1               -> GridSpec[ reduced_gg, QG32_1, Nj:64 ]
    //   reduced_gg_pl_32_grib1            -> GridSpec[ reduced_gg, QG32_1, Nj:64 ]
    //   reduced_gg_ml_grib2               -> GridSpec[ reduced_gg, QG32_2, Nj:64 ]
    //   reduced_gg_pl_32_grib2            -> GridSpec[ reduced_gg, QG32_2, Nj:64 ]
    //
    // Others are just plain wrong, i.e
    //   polar_stereographic_pl_grib2.tmpl -> GridSpec[ rotated_ll, RL31_2, Ni:16, Nj:31, editionNumber:2 ]
    //
    // From the grid spec, we will look at the grid samples, and find the closest match

    std::vector<std::string> sample_paths;
    determine_grib_samples_dir(sample_paths);

    if ( sample_paths.empty() )
        throw SeriousBug(string("Error no sample paths found"),Here()) ;

    for(size_t path = 0; path < sample_paths.size(); ++path)
    {
        std::string grib_samples_dir = sample_paths[path];

        if (grib_samples_dir.empty())
            throw SeriousBug(string("Error, empty samples path. Could not create handle from grid"),Here()) ;

        PathName dir_path(grib_samples_dir);

        if( !dir_path.exists() ) continue;
        if( !dir_path.isDir()  ) continue;

        std::vector<PathName> files;
        std::vector<PathName> directories;
        dir_path.children(files,directories);

        for(size_t i = 0; i < files.size(); i++)
        {
            try
            {
                StackGribFile grib_file(string(files[i].localPath()));

                std::string grib_sample_file_tmpl = files[i].localPath();
                if( match_grid_spec_with_sample_file(g_spec,grib_file.handle(),edition,grib_sample_file_tmpl))
                {
                    // remove .tmpl extension
                    eckit::LocalPathName path(grib_sample_file_tmpl);
                    LocalPathName base_name = path.baseName(false);
                    string grib_sample_file = base_name.localPath();
                    return grib_sample_file;
                }
            }
            catch ( const std::exception & ex )
            {
                Log::info() << files[i].localPath() << " " << ex.what() << std::endl;
            }
        }
    }

    Log::info() << "Could find grib samples match for grid_spec " << g_spec << std::endl;
    return std::string();
}

static std::string map_uid_to_grib_sample_file(const std::string& uid, long edition)
{
    using std::string;

    long ns[14] = {32,48,80,128,160,200,256,320,400,512,640,1024,1280,2000};

    std::map<std::string,std::string> uid_to_sample;

    for( size_t i = 0; i < sizeof(ns)/sizeof(long); ++i)
        uid_to_sample[ "reduced_gg_" + Translator<long,string>()(ns[i]) ] = string("reduced_gg_pl_" + Translator<long,string>()(ns[i]) );

    string r;

    std::map<string,string>::const_iterator i = uid_to_sample.find(uid);
    if (i != uid_to_sample.end())
    {
      r = (*i).second + "_grib" + Translator<long,string>()(edition);
    }

    return r;
}

void GribWrite::write( const FieldSet& fields, const PathName& opath )
{
    for( size_t i = 0; i < fields.size(); ++i )
    {
        PathName pi( opath.asString() + "." + Translator<size_t,std::string>()(i) );
        GribWrite::write(fields[i], pi);
    }
}

void GribWrite::write(const FieldHandle& fh, DataHandle& dh)
{
    GribHandle::Ptr gh = GribWrite::create_handle( fh.grid(), fh.grib().edition() );

    if( !gh )
        throw SeriousBug("Failed to create GribHandle from FieldHandle", Here());

    if( !gh->raw() )
        throw SeriousBug("Failed to create GribHandle from FieldHandle", Here());

    GribHandle::Ptr h = clone(fh,*gh);

    // dump the handle to the DataHandle
    const void* buffer = NULL;
    size_t size = 0;

    GRIB_CHECK( grib_get_message( h->raw(), &buffer, &size), 0);

    dh.write(buffer, size);
}

GribHandle::Ptr GribWrite::write(const FieldHandle& fh)
{
    GribHandle::Ptr gh = GribWrite::create_handle( fh.grid(), fh.grib().edition() );

    if( !gh )
        throw SeriousBug("Failed to create GribHandle from FieldHandle", Here());

    return clone(fh,*gh);
}

void GribWrite::clone( const FieldSet& fields, const PathName& src, const PathName& opath )
{
    bool overwrite = true;

    if( opath.exists() )
        opath.unlink();

    eckit::ScopedPtr<DataHandle> of( opath.fileHandle(overwrite) ); AutoClose of_close(*of);

    ASSERT(of);

    of->openForWrite(0);

    for( size_t i = 0; i < fields.size(); ++i )
    {
        GribWrite::clone(fields[i], src, *of);
    }
}

void GribWrite::write(const FieldHandle& f, const PathName& opath)
{
    FileHandle fh( opath );

    Length len;
    fh.openForWrite(len);

    write(f, fh);

    fh.close();
}

void GribWrite::clone(const FieldHandle& field, const PathName& gridsec, DataHandle& out )
{
    FILE* fh = ::fopen( gridsec.asString().c_str(), "r" );
    if( fh == 0 )
        throw ReadError( std::string("error opening file ") + gridsec );

    int err = 0;
    grib_handle* clone_h = grib_handle_new_from_file(0,fh,&err);
    if( clone_h == 0 || err != 0 )
        throw ReadError( std::string("error reading grib file ") + gridsec );

    GribHandle ch(clone_h);

    GribHandle::Ptr h = GribWrite::clone( field, ch );

    //    GRIB_CHECK( grib_write_message(h->raw(),fname.asString().c_str(),"w"), 0 );

    // dump the handle to the DataHandle
    const void* buffer = NULL;
    size_t size = 0;

    GRIB_CHECK( grib_get_message( h->raw(), &buffer, &size), 0);

    out.write(buffer, size);
}

GribHandle::Ptr GribWrite::clone(const FieldHandle& field, GribHandle& gridsec )
{
    const Field& f = field.data();
    const size_t npts = f.size();

    // check number of points matches

    size_t nb_nodes = gridsec.nbDataPoints();
    ASSERT( nb_nodes == f.size() );

    ///@todo move this to the eckit::grib interface
    int err=0;
    int what = GRIB_SECTION_GRID;

    GribHandle& meta = field.grib();

    grib_handle* h = grib_util_sections_copy( gridsec.raw(), meta.raw(), what, &err);
    GRIB_CHECK(err,"grib_util_sections_copy()");
    ASSERT( h );

    GribHandle::Ptr gh ( new GribHandle( h ) );

    ASSERT( gh );

	GridSpec grid_spec = field.grid().spec();

	write_gridspec_to_grib( grid_spec , *gh );

    gh->setDataValues(f.data<double>(),npts);

	return gh;
}

struct gridspec_to_grib
{
	gridspec_to_grib(GridSpec& gspec, GribHandle& gh) :
		gspec_(gspec),
		gh_(gh)
	{}

	GribHandle& gh_;
	GridSpec& gspec_;

	template <typename T>
	void set( std::string spec, std::string grib )
	{
		if( gspec_.has(spec) )
		{
			GribMutator<T>(grib).set( gh_, gspec_[spec] );
		}
	}
};

void GribWrite::write_gridspec_to_grib(GridSpec& gspec, GribHandle& gh)
{
	gridspec_to_grib gspec2grib(gspec,gh);

	gspec2grib.set<long>( "Ni", "Ni" );
	gspec2grib.set<long>( "Nj", "Nj" );

	gspec2grib.set<double>( "grid_ns_inc", "jDirectionIncrementInDegrees" );
	gspec2grib.set<double>( "grid_ew_inc", "iDirectionIncrementInDegrees" );

	gspec2grib.set<long>( "GaussN", "numberOfParallelsBetweenAPoleAndTheEquator" );

	gspec2grib.set<double>( "SouthPoleLat", "latitudeOfSouthernPoleInDegrees" );
	gspec2grib.set<double>( "SouthPoleLon", "longitudeOfSouthernPoleInDegrees" );
	gspec2grib.set<double>( "SouthPoleRotAngle", "angleOfRotation" );


	gspec2grib.set<double>( "grib_bbox_n", "latitudeOfFirstGridPointInDegrees" );
	gspec2grib.set<double>( "grid_bbox_s", "latitudeOfLastGridPointInDegrees" );
	gspec2grib.set<double>( "grid_bbox_w", "longitudeOfFirstGridPointInDegrees" );
	gspec2grib.set<double>( "grid_bbox_e", "longitudeOfLastGridPointInDegrees" );
}

//------------------------------------------------------------------------------------------------------

} // namespace grid
} // namespace atlas

