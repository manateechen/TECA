#include "teca_cartesian_mesh_subset.h"

#include "teca_cartesian_mesh.h"
#include "teca_array_collection.h"
#include "teca_variant_array.h"
#include "teca_metadata.h"
#include "teca_cartesian_mesh_util.h"

#include <algorithm>
#include <iostream>

using std::string;
using std::vector;
using std::cerr;
using std::endl;

//#define TECA_DEBUG

// --------------------------------------------------------------------------
teca_cartesian_mesh_subset::teca_cartesian_mesh_subset()
    : bounds({0l,0l,0l,0l,0l,0l}), cover_bounds(false)
{
    this->set_number_of_input_connections(1);
    this->set_number_of_output_ports(1);
}

// --------------------------------------------------------------------------
teca_cartesian_mesh_subset::~teca_cartesian_mesh_subset()
{}

// --------------------------------------------------------------------------
teca_metadata teca_cartesian_mesh_subset::get_output_metadata(
    unsigned int port,
    const std::vector<teca_metadata> &input_md)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::get_output_metadata" << endl;
#endif
    (void)port;

    teca_metadata coords;
    const_p_teca_variant_array x;
    const_p_teca_variant_array y;
    const_p_teca_variant_array z;

    if (input_md[0].get("coordinates", coords)
        || !(x = coords.get("x")) || !(y = coords.get("y"))
        || !(z = coords.get("z")))
    {
        TECA_ERROR("metadata has invalid coordinates")
        return teca_metadata();
    }

    vector<unsigned long> ext(6, 0l);

    TEMPLATE_DISPATCH_FP(
        const teca_variant_array_impl,
        x.get(),

        const NT *p_x = static_cast<TT*>(x.get())->get();
        const NT *p_y = static_cast<TT*>(y.get())->get();
        const NT *p_z = static_cast<TT*>(z.get())->get();

        if (bounds_to_extent(
            static_cast<NT>(this->bounds[0]), static_cast<NT>(this->bounds[1]),
            static_cast<NT>(this->bounds[2]), static_cast<NT>(this->bounds[3]),
            static_cast<NT>(this->bounds[4]), static_cast<NT>(this->bounds[5]),
            p_x, p_y, p_z, x->size()-1, y->size()-1, z->size()-1,
            cover_bounds, ext))
        {
            vector<double> actual(6, 0.0);

            x->get(0, actual[0]);
            x->get(x->size()-1, actual[1]);

            y->get(0, actual[2]);
            y->get(y->size()-1, actual[3]);

            z->get(0, actual[4]);
            z->get(z->size()-1, actual[5]);

            TECA_ERROR("requested bounds ["
                << this->bounds[0] << ", " << this->bounds[1] << ", "
                << this->bounds[2] << ", " << this->bounds[3] << ", "
                << this->bounds[4] << ", " << this->bounds[5]
                << "]  does not fall in the valid range ["
                << actual[0] << ", " << actual[1] << ", "
                << actual[2] << ", " << actual[3] << ", "
                << actual[4] << ", " << actual[5] << "]")

            return teca_metadata();
        }

        this->extent = ext;

        teca_metadata out_md(input_md[0]);
        out_md.insert("whole_extent", ext);
        return out_md;
        )

    TECA_ERROR("get_output_metadata failed")
    return teca_metadata();
}

// --------------------------------------------------------------------------
std::vector<teca_metadata> teca_cartesian_mesh_subset::get_upstream_request(
    unsigned int port,
    const std::vector<teca_metadata> &input_md,
    const teca_metadata &request)
{
    (void)port;
    (void)input_md;

    vector<teca_metadata> up_reqs(1, request);

    up_reqs[0].insert("extent", this->extent);

    return up_reqs;
}

// --------------------------------------------------------------------------
const_p_teca_dataset teca_cartesian_mesh_subset::execute(
    unsigned int port,
    const std::vector<const_p_teca_dataset> &input_data,
    const teca_metadata &request)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cartesian_mesh_subset::execute" << endl;
#endif
    (void)port;
    (void)request;

    p_teca_cartesian_mesh in_target
        = std::dynamic_pointer_cast<teca_cartesian_mesh>(std::const_pointer_cast<teca_dataset>(input_data[0]));

    if (!in_target)
    {
        TECA_ERROR("invalid input dataset")
        return nullptr;
    }

    // pass input through via shallow copy
    p_teca_cartesian_mesh target = teca_cartesian_mesh::New();
    target->shallow_copy(in_target);

    return target;
}