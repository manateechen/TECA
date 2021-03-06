#ifndef teca_integrated_vapor_transport_h
#define teca_integrated_vapor_transport_h

#include "teca_shared_object.h"
#include "teca_vertical_reduction.h"
#include "teca_metadata.h"

#include <string>
#include <vector>

TECA_SHARED_OBJECT_FORWARD_DECL(teca_integrated_vapor_transport)

/// an algorithm that computes integrated vapor transport (IVT)
/**
Compute integrated vaport transport (IVT) from wind vector and
specific humidity.

IVT = - \frac{1}{g} \int_{p_0}^{p_1} \vec{v} q dp

where q is the specific humidity, and \vec{v} = (u, v) are the
longitudinal and latitudinal components of wind.

This calculation is an instance of a vertical reduction where
a 3D mesh is transformed into a 2D one.
*/
class teca_integrated_vapor_transport : public teca_vertical_reduction
{
public:
    TECA_ALGORITHM_STATIC_NEW(teca_integrated_vapor_transport)
    TECA_ALGORITHM_DELETE_COPY_ASSIGN(teca_integrated_vapor_transport)
    TECA_ALGORITHM_CLASS_NAME(teca_integrated_vapor_transport)
    ~teca_integrated_vapor_transport();

    // report/initialize to/from Boost program options
    // objects.
    TECA_GET_ALGORITHM_PROPERTIES_DESCRIPTION()
    TECA_SET_ALGORITHM_PROPERTIES()

    // set the name of the varaiable that contains the longitudinal
    // component of the wind vector ("ua")
    TECA_ALGORITHM_PROPERTY(std::string, wind_u_variable)

    // set the name of the varaiable that contains the latitudinal
    // component of the wind vector ("va")
    TECA_ALGORITHM_PROPERTY(std::string, wind_v_variable)

    // set the name of the variable that contains the specific
    // humidity ("hus")
    TECA_ALGORITHM_PROPERTY(std::string,
        specific_humidity_variable)

    // set the name of the varaiable that contains the longitudinal
    // component of the ivt vector ("ivt_u")
    TECA_ALGORITHM_PROPERTY(std::string, ivt_u_variable)

    // set the name of the varaiable that contains the latitudinal
    // component of the ivt vector ("ivt_v")
    TECA_ALGORITHM_PROPERTY(std::string, ivt_v_variable)

protected:
    teca_integrated_vapor_transport();

private:
    teca_metadata get_output_metadata(
        unsigned int port,
        const std::vector<teca_metadata> &input_md) override;

    std::vector<teca_metadata> get_upstream_request(
        unsigned int port,
        const std::vector<teca_metadata> &input_md,
        const teca_metadata &request) override;

    const_p_teca_dataset execute(
        unsigned int port,
        const std::vector<const_p_teca_dataset> &input_data,
        const teca_metadata &request) override;

private:
    std::string wind_u_variable;
    std::string wind_v_variable;
    std::string specific_humidity_variable;
    std::string ivt_u_variable;
    std::string ivt_v_variable;
};

#endif
