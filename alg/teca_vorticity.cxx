#include "teca_vorticity.h"

#include "teca_cartesian_mesh.h"
#include "teca_array_collection.h"
#include "teca_variant_array.h"
#include "teca_metadata.h"

#include <algorithm>
#include <iostream>
#include <string>

#if defined(TECA_HAS_BOOST)
#include <boost/program_options.hpp>
#endif

using std::string;
using std::vector;
using std::cerr;
using std::endl;

//#define TECA_DEBUG

namespace {

template <typename num_t>
constexpr num_t deg_to_rad() { return num_t(M_PI)/num_t(180); }

template <typename num_t>
constexpr num_t earth_radius() { return num_t(6371.0e3); }

// compute vorticicty
template <typename num_t, typename pt_t>
void vorticity(num_t *w, const pt_t *lat, const pt_t *lon,
    const num_t *comp_0, const num_t *comp_1, unsigned long nx, unsigned long ny)
{
    // compute dx from degrees longitude
    num_t *dx = static_cast<num_t*>(malloc(nx*sizeof(num_t)));
    num_t dlon = (lon[1]- lon[0])*deg_to_rad<num_t>();
    for (unsigned long i = 0; i < nx; ++i)
        dx[i] = earth_radius<num_t>() * cos(lat[i]*deg_to_rad<num_t>()) * dlon;

    // compute dy from degrees latitude
    unsigned long max_j = ny - 1;
    num_t *dy = static_cast<num_t*>(malloc(ny*sizeof(num_t)));
    for (unsigned long i = 1; i < max_j; ++i)
        dy[i] = num_t(0.5)*earth_radius<num_t>()*deg_to_rad<num_t>()
            *(lat[i-1] - lat[i+1]);
    dy[0] = dy[1];
    dy[max_j] = dy[max_j - 1];

    // compute vorticity
    unsigned long nxy = nx*ny;
    memset(w, 0, nxy*sizeof(num_t));
    unsigned long max_i = nx - 1;
    for (unsigned long j = 1; j < max_j; ++j)
    {
        // TODO -- rewrite this in terms of unit stride passes
        // so that the compiler will auto-vectorize it
        unsigned long jj = j*nx;
        unsigned long jj0 = jj - nx;
        unsigned long jj1 = jj + nx;
        for (unsigned long i = 1; i < max_i; ++i)
        {
            w[jj+i] = num_t(0.5)*((comp_1[jj+i+1] - comp_1[jj+i-1])/dx[i]
                - (comp_0[jj0+i] - comp_0[jj1+i])/dy[j]);
        }
    }

    free(dx);
    free(dy);

    return;
}
};


// --------------------------------------------------------------------------
teca_vorticity::teca_vorticity() :
    component_0_variable(), component_1_variable(),
    vorticity_variable("vorticity")
{
    this->set_number_of_input_connections(1);
    this->set_number_of_output_ports(1);
}

// --------------------------------------------------------------------------
teca_vorticity::~teca_vorticity()
{}

#if defined(TECA_HAS_BOOST)
// --------------------------------------------------------------------------
void teca_vorticity::get_properties_description(
    const string &prefix, options_description &global_opts)
{
    options_description opts("Options for " + prefix + "(teca_vorticity)");

    opts.add_options()
        TECA_POPTS_GET(std::string, prefix, component_0_variable, "array containg lon-component of the vector")
        TECA_POPTS_GET(std::string, prefix, component_1_variable, "array containg lat-component of the vector")
        TECA_POPTS_GET(std::string, prefix, vorticity_variable, "array to store the computed vorticity in")
        ;

    global_opts.add(opts);
}

// --------------------------------------------------------------------------
void teca_vorticity::set_properties(
    const string &prefix, variables_map &opts)
{
    TECA_POPTS_SET(opts, std::string, prefix, component_0_variable)
    TECA_POPTS_SET(opts, std::string, prefix, component_1_variable)
    TECA_POPTS_SET(opts, std::string, prefix, vorticity_variable)
}
#endif

// --------------------------------------------------------------------------
std::string teca_vorticity::get_component_0_variable(
    const teca_metadata &request)
{
    std::string comp_0_var = this->component_0_variable;

    if (comp_0_var.empty() &&
        request.has("teca_vorticity::component_0_variable"))
            request.get("teca_vorticity::component_0_variable", comp_0_var);

    return comp_0_var;
}

// --------------------------------------------------------------------------
std::string teca_vorticity::get_component_1_variable(
    const teca_metadata &request)
{
    std::string comp_1_var = this->component_1_variable;

    if (comp_1_var.empty() &&
        request.has("teca_vorticity::component_1_variable"))
            request.get("teca_vorticity::component_1_variable", comp_1_var);

    return comp_1_var;
}

// --------------------------------------------------------------------------
std::string teca_vorticity::get_vorticity_variable(
    const teca_metadata &request)
{
    std::string vort_var = this->vorticity_variable;

    if (vort_var.empty())
    {
        if (request.has("teca_vorticity::vorticity_variable"))
            request.get("teca_vorticity::vorticity_variable", vort_var);
        else
            vort_var = "vorticity";
    }

    return vort_var;
}

// --------------------------------------------------------------------------
teca_metadata teca_vorticity::get_output_metadata(
    unsigned int port,
    const std::vector<teca_metadata> &input_md)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_vorticity::get_output_metadata" << endl;
#endif
    (void)port;

    // add in the array we will generate
    teca_metadata out_md(input_md[0]);
    out_md.append("variables", this->vorticity_variable);

    return out_md;
}

// --------------------------------------------------------------------------
std::vector<teca_metadata> teca_vorticity::get_upstream_request(
    unsigned int port,
    const std::vector<teca_metadata> &input_md,
    const teca_metadata &request)
{
    (void)port;
    (void)input_md;

    vector<teca_metadata> up_reqs;

    // get the name of the arrays we need to request
    std::string comp_0_var = this->get_component_0_variable(request);
    if (comp_0_var.empty())
    {
        TECA_ERROR("component 0 array was not specified")
        return up_reqs;
    }

    std::string comp_1_var = this->get_component_1_variable(request);
    if (comp_1_var.empty())
    {
        TECA_ERROR("component 0 array was not specified")
        return up_reqs;
    }

    // copy the incoming request to preserve the downstream
    // requirements and add the arrays we need
    teca_metadata req(request);

    std::set<std::string> arrays;
    if (req.has("arrays"))
        req.get("arrays", arrays);

    arrays.insert(this->component_0_variable);
    arrays.insert(this->component_1_variable);

    // capture the array we produce
    arrays.erase(this->get_vorticity_variable(request));

    // update the request
    req.insert("arrays", arrays);

    // send it up
    up_reqs.push_back(req);
    return up_reqs;
}

// --------------------------------------------------------------------------
const_p_teca_dataset teca_vorticity::execute(
    unsigned int port,
    const std::vector<const_p_teca_dataset> &input_data,
    const teca_metadata &request)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_vorticity::execute" << endl;
#endif
    (void)port;

    // get the input mesh
    const_p_teca_cartesian_mesh in_mesh
        = std::dynamic_pointer_cast<const teca_cartesian_mesh>(input_data[0]);

    if (!in_mesh)
    {
        TECA_ERROR("teca_cartesian_mesh is required")
        return nullptr;
    }

    // get component 0 array
    std::string comp_0_var = this->get_component_0_variable(request);

    if (comp_0_var.empty())
    {
        TECA_ERROR("component_0_variable was not specified")
        return nullptr;
    }

    const_p_teca_variant_array comp_0
        = in_mesh->get_point_arrays()->get(comp_0_var);

    if (!comp_0)
    {
        TECA_ERROR("requested array \"" << comp_0_var << "\" not present.")
        return nullptr;
    }

    // get component 1 array
    std::string comp_1_var = this->get_component_1_variable(request);

    if (comp_1_var.empty())
    {
        TECA_ERROR("component_1_variable was not specified")
        return nullptr;
    }

    const_p_teca_variant_array comp_1
        = in_mesh->get_point_arrays()->get(comp_1_var);

    if (!comp_1)
    {
        TECA_ERROR("requested array \"" << comp_1_var << "\" not present.")
        return nullptr;
    }

    // get the input coordinate arrays
    const_p_teca_variant_array lon = in_mesh->get_x_coordinates();
    const_p_teca_variant_array lat = in_mesh->get_y_coordinates();

    if (!lon || !lat)
    {
        TECA_ERROR("lat lon mesh cooridinates not present.")
        return nullptr;
    }

    // allocate the output array
    p_teca_variant_array vort = comp_0->new_instance();
    vort->resize(comp_0->size());

    // compute vorticity
    NESTED_TEMPLATE_DISPATCH_FP(
        const teca_variant_array_impl,
        lon.get(), 1,

        const NT1 *p_lon = dynamic_cast<const TT1*>(lon.get())->get();
        const NT1 *p_lat = dynamic_cast<const TT1*>(lat.get())->get();

        NESTED_TEMPLATE_DISPATCH_FP(
            teca_variant_array_impl,
            vort.get(), 2,

            const NT2 *p_comp_0 = dynamic_cast<const TT2*>(comp_0.get())->get();
            const NT2 *p_comp_1 = dynamic_cast<const TT2*>(comp_1.get())->get();
            NT2 *p_vort = dynamic_cast<TT2*>(vort.get())->get();

            ::vorticity(p_vort, p_lat, p_lon,
                p_comp_0, p_comp_1, lon->size(), lat->size());
            )
        )

    // create the output mesh, pass everything through, and
    // add the vorticity array
    p_teca_cartesian_mesh out_mesh = teca_cartesian_mesh::New();

    out_mesh->shallow_copy(
        std::const_pointer_cast<teca_cartesian_mesh>(in_mesh));

    out_mesh->get_point_arrays()->append(
        this->get_vorticity_variable(request), vort);

    return out_mesh;
}
