#include "teca_cf_reader.h"
#include "teca_array_attributes.h"
#include "teca_file_util.h"
#include "teca_cartesian_mesh.h"
#include "teca_thread_pool.h"
#include "teca_coordinate_util.h"
#include "teca_netcdf_util.h"
#include "teca_system_util.h"
#include "calcalcs.h"

#include <netcdf.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <utility>
#include <memory>
#include <iomanip>

using std::endl;
using std::cerr;

#if defined(TECA_HAS_MPI)
#include <mpi.h>
#endif

#if defined(TECA_HAS_BOOST)
#include <boost/program_options.hpp>
#endif

#if defined(TECA_HAS_OPENSSL)
#include <openssl/sha.h>
#endif

// internals for the cf reader
class teca_cf_reader_internals
{
public:
    teca_cf_reader_internals()
    {}

#if defined(TECA_HAS_OPENSSL)
    // create a key used to identify metadata
    std::string create_metadata_cache_key(teca_binary_stream &bs);
#endif

public:
    teca_metadata metadata;
};

#if defined(TECA_HAS_OPENSSL)
// --------------------------------------------------------------------------
std::string teca_cf_reader_internals::create_metadata_cache_key(
    teca_binary_stream &bs)
{
    // create the hash using the version, file names, and path
    SHA_CTX ctx;
    SHA1_Init(&ctx);

    // include the version since metadata could change between releases
    SHA1_Update(&ctx, TECA_VERSION_DESCR, strlen(TECA_VERSION_DESCR));

    // include run time parameters that would lead to a change in the metadata
    SHA1_Update(&ctx, bs.get_data(), bs.size());

    unsigned char key[SHA_DIGEST_LENGTH] = {0};
    SHA1_Final(key, &ctx);

    // convert to ascii
    std::ostringstream oss;
    oss.fill('0');
    oss << std::hex;

    for (unsigned int i = 0; i < SHA_DIGEST_LENGTH; ++i)
        oss << std::setw(2) << static_cast<unsigned int>(key[i]);

    return oss.str();
}
#endif


// --------------------------------------------------------------------------
teca_cf_reader::teca_cf_reader() :
    files_regex(""),
    x_axis_variable("lon"),
    y_axis_variable("lat"),
    z_axis_variable(""),
    t_axis_variable("time"),
    t_calendar(""),
    t_units(""),
    filename_time_template(""),
    periodic_in_x(0),
    periodic_in_y(0),
    periodic_in_z(0),
    thread_pool_size(-1),
    cache_metadata(1),
    internals(new teca_cf_reader_internals)
{
    bool tmp = true;
    if (teca_system_util::get_environment_variable(
        "TECA_CF_READER_CACHE_METADATA", tmp) == 0)
    {
        cache_metadata = tmp;
        TECA_STATUS("TECA_CF_READER_CACHE_METADATA = " << (tmp ? "TRUE" : "FALSE")
            << " metadata cache " << (tmp ? "enabled" : "disabled"))
    }
}

// --------------------------------------------------------------------------
teca_cf_reader::~teca_cf_reader()
{}

#if defined(TECA_HAS_BOOST)
// --------------------------------------------------------------------------
void teca_cf_reader::get_properties_description(
    const std::string &prefix, options_description &global_opts)
{
    options_description opts("Options for "
        + (prefix.empty()?"teca_cf_reader":prefix));

    opts.add_options()
        TECA_POPTS_GET(std::vector<std::string>, prefix, file_names,
            "paths/file names to read")
        TECA_POPTS_GET(std::string, prefix, files_regex,
            "a regular expression that matches the set of files "
            "comprising the dataset")
        TECA_POPTS_GET(std::string, prefix, metadata_cache_dir,
            "a directory where metadata caches can be stored ()")
        TECA_POPTS_GET(std::string, prefix, x_axis_variable,
            "name of variable that has x axis coordinates (lon)")
        TECA_POPTS_GET(std::string, prefix, y_axis_variable,
            "name of variable that has y axis coordinates (lat)")
        TECA_POPTS_GET(std::string, prefix, z_axis_variable,
            "name of variable that has z axis coordinates ()")
        TECA_POPTS_GET(std::string, prefix, t_axis_variable,
            "name of variable that has t axis coordinates (time)")
        TECA_POPTS_GET(std::string, prefix, t_calendar,
            "name of variable that has the time calendar (calendar)")
        TECA_POPTS_GET(std::string, prefix, t_units,
            "a std::get_time template for decoding time from the input filename")
        TECA_POPTS_GET(std::string, prefix, filename_time_template,
            "name of variable that has the time unit (units)")
        TECA_POPTS_GET(std::vector<double>, prefix, t_values,
            "name of variable that has t axis values set by the"
            "the user if the file doesn't have time variable set ()")
        TECA_POPTS_GET(int, prefix, periodic_in_x,
            "the dataset has apriodic boundary in the x direction (0)")
        TECA_POPTS_GET(int, prefix, periodic_in_y,
            "the dataset has apriodic boundary in the y direction (0)")
        TECA_POPTS_GET(int, prefix, periodic_in_z,
            "the dataset has apriodic boundary in the z direction (0)")
        TECA_POPTS_GET(int, prefix, thread_pool_size,
            "set the number of I/O threads (-1)")
        TECA_POPTS_GET(int, prefix, cache_metadata,
            "a flag when set enables the use of cached metadata (1)")
        ;

    global_opts.add(opts);
}

// --------------------------------------------------------------------------
void teca_cf_reader::set_properties(const std::string &prefix,
    variables_map &opts)
{
    TECA_POPTS_SET(opts, std::vector<std::string>, prefix, file_names)
    TECA_POPTS_SET(opts, std::string, prefix, files_regex)
    TECA_POPTS_SET(opts, std::string, prefix, metadata_cache_dir)
    TECA_POPTS_SET(opts, std::string, prefix, x_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, y_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, z_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, t_axis_variable)
    TECA_POPTS_SET(opts, std::string, prefix, t_calendar)
    TECA_POPTS_SET(opts, std::string, prefix, t_units)
    TECA_POPTS_SET(opts, std::string, prefix, filename_time_template)
    TECA_POPTS_SET(opts, std::vector<double>, prefix, t_values)
    TECA_POPTS_SET(opts, int, prefix, periodic_in_x)
    TECA_POPTS_SET(opts, int, prefix, periodic_in_y)
    TECA_POPTS_SET(opts, int, prefix, periodic_in_z)
    TECA_POPTS_SET(opts, int, prefix, thread_pool_size)
    TECA_POPTS_SET(opts, int, prefix, cache_metadata)
}
#endif

// --------------------------------------------------------------------------
void teca_cf_reader::set_modified()
{
    // clear cached metadata before forwarding on to
    // the base class.
    this->clear_cached_metadata();
    teca_algorithm::set_modified();
}

// --------------------------------------------------------------------------
void teca_cf_reader::clear_cached_metadata()
{
    this->internals->metadata.clear();
}

// --------------------------------------------------------------------------
teca_metadata teca_cf_reader::get_output_metadata(
    unsigned int port,
    const std::vector<teca_metadata> &input_md)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::get_output_metadata" << endl;
#endif
    (void)port;
    (void)input_md;

    // return cached metadata. cache is cleared if
    // any of the algorithms properties are modified
    if (this->internals->metadata)
        return this->internals->metadata;

    int rank = 0;
    int n_ranks = 1;

#if defined(TECA_HAS_MPI)
    MPI_Comm comm = this->get_communicator();

    int is_init = 0;
    MPI_Initialized(&is_init);
    if (is_init)
    {
        MPI_Comm_rank(comm, &rank);
        MPI_Comm_size(comm, &n_ranks);
    }
#endif
    teca_binary_stream stream;

    // only rank 0 will parse the dataset. once
    // parsed metadata is broadcast to all
    int root_rank = n_ranks - 1;
    if (rank == root_rank)
    {
        std::vector<std::string> files;
        std::string path;

        if (!this->file_names.empty())
        {
            // use file name
            size_t file_names_size = this->file_names.size();
            for (size_t i = 0; i < file_names_size; ++i)
            {
                std::string file_name = this->file_names[i];
                path = teca_file_util::path(file_name);
                files.push_back(teca_file_util::filename(file_name));
            }
        }
        else
        {
            // use regex
            std::string regex = teca_file_util::filename(this->files_regex);
            path = teca_file_util::path(this->files_regex);

            if (teca_file_util::locate_files(path, regex, files))
            {
                TECA_ERROR(
                    << "Failed to locate any files" << endl
                    << this->files_regex << endl
                    << path << endl
                    << regex)
                return teca_metadata();
            }
        }

#if defined(TECA_HAS_OPENSSL)
        // look for a metadata cache. we are caching it on disk as for large
        // datasets on Lustre, scanning the time dimension is costly because of
        // NetCDF CF convention that time is unlimitted and thus not layed out
        // contiguously in the files.
        std::string metadata_cache_key;

        std::string metadata_cache_path[4] =
            {(getenv("HOME") ? : "."), ".", path, metadata_cache_dir};

        int n_metadata_cache_paths = metadata_cache_dir.empty() ? 2 : 3;

        if (this->cache_metadata)
        {
            // the key should include runtime attributes that change the metadata
            teca_binary_stream bs;

            bs.pack(path);
            bs.pack(files);

            bs.pack(this->files_regex);
            bs.pack(this->file_names);
            bs.pack(this->x_axis_variable);
            bs.pack(this->y_axis_variable);
            bs.pack(this->z_axis_variable);
            bs.pack(this->t_axis_variable);
            bs.pack(this->t_units);
            bs.pack(this->t_calendar);
            bs.pack(this->t_values);
            bs.pack(this->filename_time_template);
            bs.pack(this->periodic_in_x);
            bs.pack(this->periodic_in_y);
            bs.pack(this->periodic_in_z);

            metadata_cache_key =
                this->internals->create_metadata_cache_key(bs);

            for (int i = n_metadata_cache_paths; i >= 0; --i)
            {
                std::string metadata_cache_file =
                    metadata_cache_path[i] + PATH_SEP + "." + metadata_cache_key + ".tmd";

                if (teca_file_util::file_exists(metadata_cache_file.c_str()))
                {
                    // read the cache
                    if (teca_file_util::read_stream(metadata_cache_file.c_str(),
                        "teca_cf_reader::metadata_cache_file", stream))
                    {
                        TECA_WARNING("Failed to read metadata cache \""
                            << metadata_cache_file << "\"")
                    }
                    else
                    {
                        TECA_STATUS("Found metadata cache \""
                            << metadata_cache_file << "\"")
                        // recover metadata
                        this->internals->metadata.from_stream(stream);
                        // stop
                        break;
                    }
                }
            }
        }
#endif

        // load from cache failed, generate from scratch
        if (!this->internals->metadata)
        {
            int ierr = 0;
            std::string file = path + PATH_SEP + files[0];

            // open the file
            teca_netcdf_util::netcdf_handle fh;
            if (fh.open(file.c_str(), NC_NOWRITE))
            {
                TECA_ERROR("Failed to open " << file << endl << nc_strerror(ierr))
                return teca_metadata();
            }

            // enumerate mesh arrays and their attributes
            int n_vars = 0;
            teca_metadata atrs;
            std::vector<std::string> vars;
#if !defined(HDF5_THREAD_SAFE)
            {
            std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
            if (((ierr = nc_inq_nvars(fh.get(), &n_vars)) != NC_NOERR))
            {
                this->clear_cached_metadata();
                TECA_ERROR(
                    << "Failed to get the number of variables in file \""
                    << file << "\"" << endl
                    << nc_strerror(ierr))
                return teca_metadata();
            }
#if !defined(HDF5_THREAD_SAFE)
            }
#endif
            for (int i = 0; i < n_vars; ++i)
            {
                std::string name;
                teca_metadata atts;

                if (teca_netcdf_util::read_variable_attributes(fh, i, name, atts))
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to read " << i <<"th variable attributes")
                    return teca_metadata();
                }

                vars.push_back(name);
                atrs.set(name, atts);
            }

            // read spatial coordinate arrays
            double bounds[6] = {0.0};
            unsigned long whole_extent[6] = {0ul};

            int x_id = 0;
            size_t n_x = 1;
            nc_type x_t = 0;
            teca_metadata x_atts;
            p_teca_variant_array x_axis;

            if (atrs.get(x_axis_variable, x_atts) ||
                x_atts.get("cf_dims", n_x) ||
                x_atts.get("cf_type_code", x_t) ||
                x_atts.get("cf_id", x_id))
            {
                this->clear_cached_metadata();
                TECA_ERROR(
                    << "Failed to get the attributes for x-axis variable \""
                    << x_axis_variable << "\"")
                return teca_metadata();
            }

            NC_DISPATCH_FP(x_t,
                size_t x_0 = 0;
                p_teca_variant_array_impl<NC_T> x = teca_variant_array_impl<NC_T>::New(n_x);
#if !defined(HDF5_THREAD_SAFE)
                {
                std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
                if ((ierr = nc_get_vara(fh.get(), x_id, &x_0, &n_x, x->get())) != NC_NOERR)
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to read x axis, " << x_axis_variable << endl
                        << file << endl << nc_strerror(ierr))
                    return teca_metadata();
                }
#if !defined(HDF5_THREAD_SAFE)
                }
#endif
                x_axis = x;
                whole_extent[1] = n_x - 1;
                bounds[0] = x->get(0);
                bounds[1] = x->get(whole_extent[1]);
                )

            int y_id = 0;
            size_t n_y = 1;
            nc_type y_t = 0;
            teca_metadata y_atts;
            p_teca_variant_array y_axis;
            if (!y_axis_variable.empty())
            {
                if (atrs.get(y_axis_variable, y_atts) ||
                    y_atts.get("cf_dims", n_y) ||
                    y_atts.get("cf_type_code", y_t) ||
                    y_atts.get("cf_id", y_id))
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to get the attributes for y-axis variable \""
                        << y_axis_variable << "\"")
                    return teca_metadata();
                }

                NC_DISPATCH_FP(y_t,
                    size_t y_0 = 0;
                    p_teca_variant_array_impl<NC_T> y = teca_variant_array_impl<NC_T>::New(n_y);
#if !defined(HDF5_THREAD_SAFE)
                    {
                    std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
                    if ((ierr = nc_get_vara(fh.get(), y_id, &y_0, &n_y, y->get())) != NC_NOERR)
                    {
                        this->clear_cached_metadata();
                        TECA_ERROR(
                            << "Failed to read y axis, " << y_axis_variable << endl
                            << file << endl << nc_strerror(ierr))
                        return teca_metadata();
                    }
#if !defined(HDF5_THREAD_SAFE)
                    }
#endif
                    y_axis = y;
                    whole_extent[3] = n_y - 1;
                    bounds[2] = y->get(0);
                    bounds[3] = y->get(whole_extent[3]);
                    )
            }
            else
            {
                NC_DISPATCH_FP(x_t,
                    p_teca_variant_array_impl<NC_T> y = teca_variant_array_impl<NC_T>::New(1);
                    y->set(0, NC_T());
                    y_axis = y;
                    )
            }

            int z_id = 0;
            size_t n_z = 1;
            nc_type z_t = 0;
            teca_metadata z_atts;
            p_teca_variant_array z_axis;
            if (!z_axis_variable.empty())
            {
                if (atrs.get(z_axis_variable, z_atts) ||
                    z_atts.get("cf_dims", n_z) ||
                    z_atts.get("cf_type_code", z_t) ||
                    z_atts.get("cf_id", z_id))
                {
                    this->clear_cached_metadata();
                    TECA_ERROR(
                        << "Failed to get the attributes for z-axis variable \""
                        << z_axis_variable << "\"")
                    return teca_metadata();
                }

                NC_DISPATCH_FP(z_t,
                    size_t z_0 = 0;
                    p_teca_variant_array_impl<NC_T> z = teca_variant_array_impl<NC_T>::New(n_z);
#if !defined(HDF5_THREAD_SAFE)
                    {
                    std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
                    if ((ierr = nc_get_vara(fh.get(), z_id, &z_0, &n_z, z->get())) != NC_NOERR)
                    {
                        this->clear_cached_metadata();
                        TECA_ERROR(
                            << "Failed to read z axis, " << z_axis_variable << endl
                            << file << endl << nc_strerror(ierr))
                        return teca_metadata();
                    }
#if !defined(HDF5_THREAD_SAFE)
                    }
#endif
                    z_axis = z;
                    whole_extent[5] = n_z - 1;
                    bounds[4] = z->get(0);
                    bounds[5] = z->get(whole_extent[5]);
                    )
            }
            else
            {
                NC_DISPATCH_FP(x_t,
                    p_teca_variant_array_impl<NC_T> z = teca_variant_array_impl<NC_T>::New(1);
                    z->set(0, NC_T());
                    z_axis = z;
                    )
            }

            // collect time steps from this and the rest of the files.
            // there are a couple of  performance issues on Lustre.
            // 1) opening a file is slow, there's latency due to contentions
            // 2) reading the time axis is very slow as it's not stored
            //    contiguously by convention. ie. time is an "unlimted"
            //    NetCDF dimension.
            // when procesing large numbers of files these issues kill
            // serial performance. hence we are reading time dimension
            // in parallel.
            using teca_netcdf_util::read_variable_and_attributes;

            read_variable_and_attributes::queue_t
                thread_pool(MPI_COMM_SELF, this->thread_pool_size, true, false);

            // we rely t_axis_variable being empty to indicate either that
            // there is no time axis, or that a time axis will be defined by
            // other algorithm properties. This temporary is used for metadata
            // consistency across those cases.
            std::string t_axis_var = t_axis_variable;

            p_teca_variant_array t_axis;
            teca_metadata t_atts;

            std::vector<unsigned long> step_count;
            if (!t_axis_variable.empty())
            {
                // validate the time axis calendaring metadata. this code is to
                // let us know when the time axis is not correctly specified in
                // the input file.
                teca_metadata time_atts;
                if (atrs.get(t_axis_variable, time_atts))
                {
                    TECA_WARNING("Attribute metadata for time axis variable \""
                        << t_axis_variable << "\" is missing, Temporal analysis is "
                        << "likely to fail.")
                }

                // override the calendar
                if (!this->t_calendar.empty())
                {
                    TECA_WARNING("Overriding the calendar with the runtime "
                        "provided value \"" << this->t_calendar << "\"")
                    time_atts.set("calendar", this->t_calendar);
                }

                // override the units
                if (!this->t_units.empty())
                {
                    TECA_WARNING("Overriding the time units with the runtime "
                        "provided value \"" << this->t_units << "\"")
                    time_atts.set("units", this->t_units);
                }

                // check for units. units are necessary.
                int has_units = 0;
                if (!(has_units = time_atts.has("units")))
                {
                    TECA_WARNING("The units attribute for the time axis variable \""
                        << t_axis_variable << "\" is missing. Temporal analysis is "
                        << "likely to fail.")
                }

                // check for calendar. calendar, if missing will be set to "standard"
                int has_calendar = 0;
                if (!(has_calendar = time_atts.has("calendar")))
                {
                    TECA_WARNING("The calendar attribute for the time axis variable \""
                        << t_axis_variable << "\" is missing. Using the \"standard\" "
                        "calendar")
                    time_atts.set("calendar", std::string("standard"));
                }

                // correct the data type if applying a user provided override
                if (!this->t_values.empty())
                {
                    time_atts.set("cf_type_code",
                        int(teca_netcdf_util::netcdf_tt<double>::type_code));

                    time_atts.set("type_code",
                        teca_variant_array_code<double>::get());
                }

                // get the base calendar and units. all the files are required to
                // use the same calendar, but in the case that some of the files
                // have different untis we will convert them into the base units.
                std::string base_calendar;
                time_atts.get("calendar", base_calendar);

                std::string base_units;
                time_atts.get("units", base_units);

                // save the updates
                atrs.set(t_axis_variable, time_atts);

                // assign the reads to threads
                size_t n_files = files.size();
                for (size_t i = 0; i < n_files; ++i)
                {
                    read_variable_and_attributes
                         reader(path, files[i], i, t_axis_variable);

                    read_variable_and_attributes::task_t task(reader);

                    thread_pool.push_task(task);
                }

                // wait for the results
                std::vector<read_variable_and_attributes::data_t> tmp;
                tmp.reserve(n_files);
                thread_pool.wait_all(tmp);

                // unpack the results. map is used to ensure the correct
                // file to time association.
                std::map<unsigned long, read_variable_and_attributes::data_elem_t>
                    time_arrays(tmp.begin(), tmp.end());

                p_teca_variant_array t0 = time_arrays[0].first;
                if (!t0)
                {
                    TECA_ERROR("Failed to read time axis")
                    return teca_metadata();
                }
                t_axis = t0->new_instance();

                for (size_t i = 0; i < n_files; ++i)
                {
                    auto &elem_i = time_arrays[i];

                    // ge the values read
                    p_teca_variant_array tmp = elem_i.first;
                    if (!tmp || !tmp->size())
                    {
                        TECA_ERROR("File " << i << " \"" << files[i]
                            << "\" had no time values")
                        return teca_metadata();
                    }

                    // it is an error for the files to have different calendars
                    std::string calendar_i;
                    elem_i.second.get("calendar", calendar_i);
                    if ((!has_calendar && !calendar_i.empty())
                        || (has_calendar && (calendar_i != base_calendar)))
                    {
                        TECA_ERROR("The base calendar is \"" << base_calendar
                            << "\" but file " << i << " \"" << files[i]
                            << "\" has the \"" << calendar_i <<  "\" calendar")
                        return teca_metadata();
                    }

                    // update the step map
                    size_t n_ti = tmp->size();
                    step_count.push_back(n_ti);

                    // allocate space to hold incoming values
                    size_t n_t = t_axis->size();
                    t_axis->resize(n_t + n_ti);

                    std::string units_i;
                    elem_i.second.get("units", units_i);
                    if (units_i == base_units)
                    {
                        // the files are in the same units copy the data
                        TEMPLATE_DISPATCH(teca_variant_array_impl,
                            t_axis.get(),
                            NT *p_ti = static_cast<TT*>(elem_i.first.get())->get();
                            NT *p_t = static_cast<TT*>(t_axis.get())->get() + n_t;
                            memcpy(p_t, p_ti, sizeof(NT)*n_ti);
                            )
                    }
                    else
                    {
                        // if there are no units present then we can not do a conversion
                        if (!has_units)
                        {
                            TECA_ERROR("Calendaring conversion requires time units")
                            return teca_metadata();
                        }

                        // the files are in a different units, warn and convert
                        // to the base units
                        TECA_WARNING("File " << i << " \"" << files[i] << "\" units \""
                            << units_i << "\" differs from base units \"" << base_units
                            << "\" a conversion will be made.")

                        TEMPLATE_DISPATCH(teca_variant_array_impl,
                            t_axis.get(),
                            NT *p_ti = static_cast<TT*>(elem_i.first.get())->get();
                            NT *p_t = static_cast<TT*>(t_axis.get())->get() + n_t;
                            for (size_t j = 0; j < n_ti; ++j)
                            {
                                // convert offset from units_i to time
                                int YY=0;
                                int MM=0;
                                int DD=0;
                                int hh=0;
                                int mm=0;
                                double ss=0.0;
                                if (calcalcs::date(double(p_ti[j]), &YY, &MM, &DD, &hh, &mm, &ss,
                                    units_i.c_str(), base_calendar.c_str()))
                                {
                                    TECA_ERROR("Failed to convert offset ti[" << j << "] = "
                                        << p_ti[j] << " calendar \"" << base_calendar
                                        << "\" units \"" << units_i << "\" to time")
                                    return teca_metadata();
                                }

                                // convert time to offsets from base units
                                double offs = 0.0;
                                if (calcalcs::coordinate(YY, MM, DD, hh, mm, ss,
                                    base_units.c_str(), base_calendar.c_str(), &offs))
                                {
                                    TECA_ERROR("Failed to convert time "
                                        << YY << "-" << MM << "-" << DD << " " << hh << ":"
                                        << mm << ":" << ss << " to offset in calendar \""
                                        << base_calendar << "\" units \"" << base_units
                                        << "\"")
                                    return teca_metadata();
                                }

                                p_t[j] = offs;
#ifdef TECA_DEBUG
                                std::cerr
                                    << YY << "-" << MM << "-" << DD << " " << hh << ":"
                                    << mm << ":" << ss << " "  << p_ti[j] << " -> " << offs
                                    << std::endl;
#endif
                            }
                            )
                    }
                }

                // override the time values read from disk with user supplied set
                if (!this->t_values.empty())
                {

                    TECA_WARNING("Overriding the time coordinates stored on disk "
                        "with runtime provided values.")

                    size_t n_t_vals = this->t_values.size();
                    if (n_t_vals != t_axis->size())
                    {
                        TECA_ERROR("Number of timesteps detected doesn't match "
                            "the number of time values provided; " << n_t_vals
                            << " given, " << t_axis->size() << " are necessary.")
                        return teca_metadata();
                    }

                    p_teca_double_array t =
                        teca_double_array::New(this->t_values.data(), n_t_vals);

                    t_axis = t;
                }
            }
            else if (!this->t_values.empty())
            {
                TECA_STATUS("The t_axis_variable was unspecified, using the "
                    "provided time values")

                if (this->t_calendar.empty() || this->t_units.empty())
                {
                    TECA_ERROR("The calendar and units must to be specified when "
                        " providing time values")
                    return teca_metadata();
                }

                // if time axis is provided manually by the user
                size_t n_t_vals = this->t_values.size();
                if (n_t_vals != files.size())
                {
                    TECA_ERROR("Number of files choosen doesn't match the"
                        " number of time values provided; " << n_t_vals <<
                        " given, " << files.size() << " detected.")
                    return teca_metadata();
                }

                teca_metadata time_atts;
                time_atts.set("calendar", this->t_calendar);
                time_atts.set("units", this->t_units);
                time_atts.set("cf_dims", n_t_vals);
                time_atts.set("cf_type_code", int(teca_netcdf_util::netcdf_tt<double>::type_code));
                time_atts.set("type_code", teca_variant_array_code<double>::get());
                time_atts.set("centering", int(teca_array_attributes::point_centering));

                atrs.set("time", time_atts);

                p_teca_variant_array_impl<double> t =
                    teca_variant_array_impl<double>::New(
                            this->t_values.data(), n_t_vals);

                step_count.resize(n_t_vals, 1);

                t_axis = t;

                t_axis_var = "time";
            }
            // infer the time from the filenames
            else if (!this->filename_time_template.empty())
            {
                std::vector<double> t_values;

                std::string t_units = this->t_units;
                std::string t_calendar = this->t_calendar;

                // assume that this is a standard calendar if none is provided
                if (this->t_calendar.empty())
                {
                    t_calendar = "standard";
                }

                // loop over all files and infer dates from names
                size_t n_files = files.size();
                for (size_t i = 0; i < n_files; ++i)
                {
                    std::istringstream ss(files[i].c_str());
                    std::tm current_tm;
                    current_tm.tm_year = 0;
                    current_tm.tm_mon = 0;
                    current_tm.tm_mday = 0;
                    current_tm.tm_hour = 0;
                    current_tm.tm_min = 0;
                    current_tm.tm_sec = 0;

                    // attempt to convert the filename into a time
                    ss >> std::get_time(&current_tm,
                        this->filename_time_template.c_str());

                    // check whether the conversion failed
                    if(ss.fail())
                    {
                        TECA_ERROR("Failed to infer time from filename \"" <<
                            files[i] << "\" using format \"" <<
                            this->filename_time_template << "\"")
                        return teca_metadata();
                    }

                    // set the time units based on the first file date if we
                    // don't have time units
                    if ((i == 0) && t_units.empty())
                    {
                        std::string t_units_fmt =
                            "days since %Y-%m-%d 00:00:00";

                        // convert the time data to a string
                        char tmp[256];
                        if (strftime(tmp, sizeof(tmp), t_units_fmt.c_str(),
                              &current_tm) == 0)
                        {
                            TECA_ERROR(
                                "failed to convert the time as a string with \""
                                << t_units_fmt << "\"")
                            return teca_metadata();
                        }
                        // save the time units
                        t_units = tmp;
                    }
#if defined(TECA_HAS_UDUNITS)
                    // convert the time to a double using calcalcs
                    int year = current_tm.tm_year + 1900;
                    int mon = current_tm.tm_mon + 1;
                    int day = current_tm.tm_mday;
                    int hour = current_tm.tm_hour;
                    int minute = current_tm.tm_min;
                    double second = current_tm.tm_sec;
                    double current_time = 0;
                    if (calcalcs::coordinate(year, mon, day, hour, minute,
                        second, t_units.c_str(), t_calendar.c_str(), &current_time))
                    {
                        TECA_ERROR("conversion of date inferred from "
                            "filename failed");
                        return teca_metadata();
                    }
                    // add the current time to the list
                    t_values.push_back(current_time);
#else
                    TECA_ERROR("The UDUnits package is required for this operation")
                    return teca_metadata();
#endif
                }

                TECA_STATUS("The time axis will be infered from file names using "
                    "the user provided template \"" << this->filename_time_template
                    << "\" with the \"" << t_calendar << "\" in units \"" << t_units
                    << "\"")

                // create a teca variant array from the times
                size_t n_t_vals = t_values.size();
                p_teca_variant_array_impl<double> t =
                    teca_variant_array_impl<double>::New(t_values.data(),
                            n_t_vals);

                // set the number of time steps
                step_count.resize(n_t_vals, 1);

                // set the time metadata
                teca_metadata time_atts;
                time_atts.set("calendar", t_calendar);
                time_atts.set("units", t_units);
                time_atts.set("cf_dims", n_t_vals);
                time_atts.set("cf_type_code", int(teca_netcdf_util::netcdf_tt<double>::type_code));
                time_atts.set("type_code", teca_variant_array_code<double>::get());
                time_atts.set("centering", int(teca_array_attributes::point_centering));
                atrs.set("time", time_atts);

                // set the time axis
                t_axis = t;
                t_axis_var = "time";

            }
            else
            {
                // make a dummy time axis, this enables parallelization over
                // file sets that do not have time dimension. However, there is
                // no guarantee on the order of the dummy axis to the lexical
                // ordering of the files and there will be no calendaring
                // information. As a result many time aware algorithms will not
                // work.
                size_t n_files = files.size();
                NC_DISPATCH_FP(x_t,
                    p_teca_variant_array_impl<NC_T> t =
                        teca_variant_array_impl<NC_T>::New(n_files);
                    for (size_t i = 0; i < n_files; ++i)
                    {
                        t->set(i, NC_T(i));
                        step_count.push_back(1);
                    }
                    t_axis = t;
                    )

                t_axis_var = "time";

                TECA_STATUS("The time axis will be generated, with 1 step per file")
            }

            this->internals->metadata.set("variables", vars);
            this->internals->metadata.set("attributes", atrs);

            teca_metadata coords;
            coords.set("x_variable", x_axis_variable);
            coords.set("y_variable", (y_axis_variable.empty() ? "y" : y_axis_variable));
            coords.set("z_variable", (z_axis_variable.empty() ? "z" : z_axis_variable));
            coords.set("t_variable", t_axis_var);
            coords.set("x", x_axis);
            coords.set("y", y_axis);
            coords.set("z", z_axis);
            coords.set("t", t_axis);
            coords.set("periodic_in_x", this->periodic_in_x);
            coords.set("periodic_in_y", this->periodic_in_y);
            coords.set("periodic_in_z", this->periodic_in_z);
            this->internals->metadata.set("whole_extent", whole_extent);
            this->internals->metadata.set("bounds", bounds);
            this->internals->metadata.set("coordinates", coords);
            this->internals->metadata.set("files", files);
            this->internals->metadata.set("root", path);
            this->internals->metadata.set("step_count", step_count);
            this->internals->metadata.set("number_of_time_steps",
                    t_axis->size());

            // inform the executive how many and how to request time steps
            this->internals->metadata.set(
                "index_initializer_key", std::string("number_of_time_steps"));

            this->internals->metadata.set(
                "index_request_key", std::string("time_step"));

            this->internals->metadata.to_stream(stream);

#if defined(TECA_HAS_OPENSSL)
            if (this->cache_metadata)
            {
                // cache metadata on disk
                bool cached_metadata = false;
                for (int i = n_metadata_cache_paths; i >= 0; --i)
                {
                    std::string metadata_cache_file =
                        metadata_cache_path[i] + PATH_SEP + "." + metadata_cache_key + ".tmd";

                    if (!teca_file_util::write_stream(metadata_cache_file.c_str(),
                        S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH,
                        "teca_cf_reader::metadata_cache_file", stream, false))
                    {
                        cached_metadata = true;
                        TECA_STATUS("Wrote metadata cache \""
                            << metadata_cache_file << "\"")
                        break;
                    }
                }
                if (!cached_metadata)
                {
                    TECA_ERROR("failed to create a metadata cache")
                }
            }
#endif
        }

#if defined(TECA_HAS_MPI)
        // broadcast the metadata to other ranks
        if (is_init)
            stream.broadcast(comm, root_rank);
#endif
    }
#if defined(TECA_HAS_MPI)
    else
    if (is_init)
    {
        // all other ranks receive the metadata from the root
        stream.broadcast(comm, root_rank);

        this->internals->metadata.from_stream(stream);

        // initialize the file map
        std::vector<std::string> files;
        this->internals->metadata.get("files", files);
    }
#endif

    return this->internals->metadata;
}

// --------------------------------------------------------------------------
const_p_teca_dataset teca_cf_reader::execute(unsigned int port,
    const std::vector<const_p_teca_dataset> &input_data,
    const teca_metadata &request)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::execute" << endl;
#endif
    (void)port;
    (void)input_data;

    // get coordinates
    teca_metadata coords;
    if (this->internals->metadata.get("coordinates", coords))
    {
        TECA_ERROR("metadata is missing \"coordinates\"")
        return nullptr;
    }

    p_teca_variant_array in_x, in_y, in_z, in_t;
    if (!(in_x = coords.get("x")) || !(in_y = coords.get("y"))
        || !(in_z = coords.get("z")) || !(in_t = coords.get("t")))
    {
        TECA_ERROR("metadata is missing coordinate arrays")
        return nullptr;
    }

    // get names, need to be careful since some of these depend
    // on run time information. eg: user can specify a time axis
    // via algorithm properties
    std::string x_axis_var;
    std::string y_axis_var;
    std::string z_axis_var;
    std::string t_axis_var;
    coords.get("x_variable", x_axis_var);
    coords.get("y_variable", y_axis_var);
    coords.get("z_variable", z_axis_var);
    coords.get("t_variable", t_axis_var);

    // get request
    unsigned long time_step = 0;
    double t = 0.0;
    if (!request.get("time", t))
    {
        // translate time to a time step
        TEMPLATE_DISPATCH_FP(teca_variant_array_impl,
            in_t.get(),

            NT *pin_t = dynamic_cast<TT*>(in_t.get())->get();

            if (teca_coordinate_util::index_of(pin_t, 0,
                in_t->size()-1, static_cast<NT>(t), time_step))
            {
                TECA_ERROR("requested time " << t << " not found")
                return nullptr;
            }
            )
    }
    else
    {
        // TODO -- there is currently no error checking here to
        // support case where only 1 time step is present in a file.
        request.get("time_step", time_step);
        if ((in_t) && (time_step < in_t->size()))
        {
            in_t->get(time_step, t);
        }
        else if ((in_t) && in_t->size() != 1)
        {
            TECA_ERROR("Invalid time step " << time_step
                << " requested from data set with " << in_t->size()
                << " steps")
            return nullptr;
        }
    }

    unsigned long whole_extent[6] = {0};
    if (this->internals->metadata.get("whole_extent", whole_extent, 6))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata is missing \"whole_extent\"")
        return nullptr;
    }

    unsigned long extent[6] = {0};
    double bounds[6] = {0.0};
    if (request.get("bounds", bounds, 6))
    {
        // bounds key not present, check for extent key
        // if not present use whole_extent
        if (request.get("extent", extent, 6))
        {
            memcpy(extent, whole_extent, 6*sizeof(unsigned long));
        }
        // get bounds of the extent being read
        in_x->get(extent[0], bounds[0]);
        in_x->get(extent[1], bounds[1]);
        in_y->get(extent[2], bounds[2]);
        in_y->get(extent[3], bounds[3]);
        in_z->get(extent[4], bounds[4]);
        in_z->get(extent[5], bounds[5]);
    }
    else
    {
        // bounds key was present, convert the bounds to an
        // an extent that covers them.
        if (teca_coordinate_util::bounds_to_extent(
            bounds, in_x, in_y, in_z, extent))
        {
            TECA_ERROR("invalid bounds requested.")
            return nullptr;
        }
    }

    // requesting arrays is optional, but it's an error
    // to request an array that isn't present.
    std::vector<std::string> arrays;
    request.get("arrays", arrays);
    size_t n_arrays = arrays.size();

    // slice axes on the requested extent
    p_teca_variant_array out_x = in_x->new_copy(extent[0], extent[1]);
    p_teca_variant_array out_y = in_y->new_copy(extent[2], extent[3]);
    p_teca_variant_array out_z = in_z->new_copy(extent[4], extent[5]);

    // locate file with this time step
    std::vector<unsigned long> step_count;
    if (this->internals->metadata.get("step_count", step_count))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata is missing \"step_count\"")
        return nullptr;
    }

    unsigned long idx = 0;
    unsigned long count = 0;
    for (unsigned int i = 1;
        (i < step_count.size()) && ((count + step_count[i-1]) <= time_step);
        ++idx, ++i)
    {
        count += step_count[i-1];
    }
    unsigned long offs = time_step - count;

    std::string path;
    std::string file;
    if (this->internals->metadata.get("root", path)
        || this->internals->metadata.get("files", idx, file))
    {
        TECA_ERROR("time_step=" << time_step
            << " Failed to locate file for time step " << time_step)
        return nullptr;
    }

    // get the file handle for this step
    int ierr = 0;
    std::string file_path = path + PATH_SEP + file;
    teca_netcdf_util::netcdf_handle fh;
    if (fh.open(file_path, NC_NOWRITE))
    {
        TECA_ERROR("time_step=" << time_step << " Failed to open \"" << file << "\"")
        return nullptr;
    }
    int file_id = fh.get();

    // create output dataset
    p_teca_cartesian_mesh mesh = teca_cartesian_mesh::New();
    mesh->set_x_coordinates(x_axis_var, out_x);
    mesh->set_y_coordinates(y_axis_var, out_y);
    mesh->set_z_coordinates(z_axis_var, out_z);
    mesh->set_time(t);
    mesh->set_time_step(time_step);
    mesh->set_whole_extent(whole_extent);
    mesh->set_extent(extent);
    mesh->set_bounds(bounds);
    mesh->set_periodic_in_x(this->periodic_in_x);
    mesh->set_periodic_in_y(this->periodic_in_y);
    mesh->set_periodic_in_z(this->periodic_in_z);

    // get the array attributes
    teca_metadata atrs;
    if (this->internals->metadata.get("attributes", atrs))
    {
        TECA_ERROR("time_step=" << time_step
            << " metadata missing \"attributes\"")
        return nullptr;
    }

    // pass time axis attributes
    teca_metadata time_atts;
    std::string calendar;
    std::string units;
    if (!atrs.get(t_axis_var, time_atts)
       && !time_atts.get("calendar", calendar)
       && !time_atts.get("units", units))
    {
        mesh->set_calendar(calendar);
        mesh->set_time_units(units);
    }

    // add the pipeline keys
    teca_metadata &md = mesh->get_metadata();
    md.set("index_request_key", std::string("time_step"));
    md.set("time_step", time_step);

    // pass the attributes for the arrays read
    teca_metadata out_atrs;
    for (unsigned int i = 0; i < n_arrays; ++i)
        out_atrs.set(arrays[i], atrs.get(arrays[i]));

    // pass coordinate axes attributes
    if (atrs.has(x_axis_var))
        out_atrs.set(x_axis_var, atrs.get(x_axis_var));
    if (atrs.has(y_axis_var))
        out_atrs.set(y_axis_var, atrs.get(y_axis_var));
    if (atrs.has(z_axis_var))
        out_atrs.set(z_axis_var, atrs.get(z_axis_var));
    if (!time_atts.empty())
        out_atrs.set(t_axis_var, time_atts);

    md.set("attributes", out_atrs);

    // figure out the mapping between our extent and netcdf
    // representation
    std::vector<std::string> mesh_dim_names;
    std::vector<size_t> starts;
    std::vector<size_t> counts;
    size_t mesh_size = 1;
    if (!t_axis_variable.empty())
    {
        mesh_dim_names.push_back(t_axis_variable);
        starts.push_back(offs);
        counts.push_back(1);
    }
    if (!z_axis_variable.empty())
    {
        mesh_dim_names.push_back(z_axis_variable);
        starts.push_back(extent[4]);
        size_t count = extent[5] - extent[4] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }
    if (!y_axis_variable.empty())
    {
        mesh_dim_names.push_back(y_axis_variable);
        starts.push_back(extent[2]);
        size_t count = extent[3] - extent[2] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }
    if (!x_axis_variable.empty())
    {
        mesh_dim_names.push_back(x_axis_variable);
        starts.push_back(extent[0]);
        size_t count = extent[1] - extent[0] + 1;
        counts.push_back(count);
        mesh_size *= count;
    }

    // read requested arrays
    for (size_t i = 0; i < n_arrays; ++i)
    {
        // get metadata
        teca_metadata atts;
        int type = 0;
        int id = 0;
        p_teca_size_t_array dims;
        p_teca_string_array dim_names;

        if (atrs.get(arrays[i], atts)
            || atts.get("cf_type_code", 0, type)
            || atts.get("cf_id", 0, id)
            || !(dims = std::dynamic_pointer_cast<teca_size_t_array>(atts.get("cf_dims")))
            || !(dim_names = std::dynamic_pointer_cast<teca_string_array>(atts.get("cf_dim_names"))))
        {
            TECA_ERROR("metadata issue can't read \"" << arrays[i] << "\"")
            continue;
        }

        // check if it's a mesh variable, if it is not a mesh variable
        // it is an information variable (ie non-spatial)
        bool mesh_var = false;
        unsigned int n_dims = dim_names->size();

        if (n_dims == mesh_dim_names.size())
        {
            mesh_var = true;
            for (unsigned int ii = 0; ii < n_dims; ++ii)
            {
                if (dim_names->get(ii) != mesh_dim_names[ii])
                {
                    mesh_var = false;
                    break;
                }
            }
        }

        // read requested variables
        if (mesh_var)
        {
            // read mesh based data
            p_teca_variant_array array;
            NC_DISPATCH(type,
                p_teca_variant_array_impl<NC_T> a = teca_variant_array_impl<NC_T>::New(mesh_size);
#if !defined(HDF5_THREAD_SAFE)
                {
                std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
                if ((ierr = nc_get_vara(file_id,  id, &starts[0], &counts[0], a->get())) != NC_NOERR)
                {
                    TECA_ERROR("time_step=" << time_step
                        << " Failed to read variable \"" << arrays[i] << "\" "
                        << file << endl << nc_strerror(ierr))
                    continue;
                }
#if !defined(HDF5_THREAD_SAFE)
                }
#endif
                array = a;
                )
            mesh->get_point_arrays()->append(arrays[i], array);
        }
        else
        {
            // read non-spatial data
            // if the first dimension is time then select the requested time
            // step. otherwise read the entire thing
            std::vector<size_t> starts(n_dims);
            std::vector<size_t> counts(n_dims);
            size_t n_vals = 1;
            if (!t_axis_variable.empty() && (dim_names->get(0) == t_axis_variable))
            {
                starts[0] = offs;
                counts[0] = 1;
            }
            else
            {
                starts[0] = 0;
                size_t dim_len = dims->get(0);
                counts[0] = dim_len;
                n_vals = dim_len;
            }

            for (unsigned int ii = 1; ii < n_dims; ++ii)
            {
                size_t dim_len = dims->get(ii);
                counts[ii] = dim_len;
                n_vals *= dim_len;
            }

            p_teca_variant_array array;

            NC_DISPATCH(type,
                p_teca_variant_array_impl<NC_T> a = teca_variant_array_impl<NC_T>::New(n_vals);
#if !defined(HDF5_THREAD_SAFE)
                {
                std::lock_guard<std::mutex> lock(teca_netcdf_util::get_netcdf_mutex());
#endif
                if ((ierr = nc_get_vara(file_id,  id, &starts[0], &counts[0], a->get())) != NC_NOERR)
                {
                    TECA_ERROR("time_step=" << time_step
                        << " Failed to read \"" << arrays[i] << "\" "
                        << file << endl << nc_strerror(ierr))
                    continue;
                }
#if !defined(HDF5_THREAD_SAFE)
                }
#endif
                array = a;
                )

            mesh->get_information_arrays()->append(arrays[i], array);
        }
    }

    return mesh;
}
