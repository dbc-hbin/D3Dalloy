#ifdef OWNED_NGX_PARAMETER_CPU_TEST
#include "nvngx_parameter.hpp"
#include <cstdio>

int main() {
    d3dalloy::ParametersImpl params;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("NGX_PARAMETER_CONTRACT_%s case=%s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    int token = 0;
    void* pointer = &token;
    auto* resource = reinterpret_cast<ID3D12Resource*>(pointer);
    ID3D12Resource* out_resource = nullptr;
    void* out_pointer = nullptr;
    params.Set("Color", pointer);
    check(params.Get("Color", &out_resource) == NVNGX_RESULT_OK && out_resource == resource,
          "void_to_d3d12");
    params.Set("typed", resource);
    check(params.Get("typed", &out_pointer) == NVNGX_RESULT_OK && out_pointer == pointer,
          "d3d12_to_void");
    params.Set("other_api", reinterpret_cast<ID3D11Resource*>(pointer));
    check(params.Get("other_api", &out_resource) == NVNGX_RESULT_FAIL, "cross_api_rejected");
    check(params.Get("missing", &out_resource) == NVNGX_RESULT_FAIL, "missing_rejected");
    params.Set("null", static_cast<void*>(nullptr));
    out_resource = resource;
    check(params.Get("null", &out_resource) == NVNGX_RESULT_OK && out_resource == nullptr,
          "stored_null_preserved");
    params.Set("numeric", 7u);
    check(params.Get("numeric", &out_resource) == NVNGX_RESULT_FAIL, "numeric_resource_rejected");
    float number = 0;
    check(params.Get("numeric", &number) == NVNGX_RESULT_OK && number == 7.0f,
          "numeric_conversion_preserved");
    params.Set("numeric", 2.75f);
    int integer = 0;
    unsigned int unsigned_integer = 0;
    check(params.Get("numeric", &integer) == NVNGX_RESULT_OK && integer == 2, "overwrite_float_truncates");
    params.Set("numeric", __builtin_nanf(""));
    check(params.Get("numeric", &integer) == NVNGX_RESULT_FAIL &&
              params.Get("numeric", &unsigned_integer) == NVNGX_RESULT_FAIL,
          "nan_integer_rejected");
    params.Set("numeric", 1e10f);
    check(params.Get("numeric", &integer) == NVNGX_RESULT_FAIL &&
              params.Get("numeric", &unsigned_integer) == NVNGX_RESULT_FAIL,
          "out_of_range_integer_rejected");
    params.Set("numeric", -0.5f);
    check(params.Get("numeric", &unsigned_integer) == NVNGX_RESULT_OK && unsigned_integer == 0,
          "negative_fraction_truncates_to_zero");
    params.Reset();
    check(params.Get("Color", &out_resource) == NVNGX_RESULT_FAIL, "reset_clears_resources");
    return failures ? 1 : 0;
}
#else
#include <nvsdk_ngx_params.h>

// Official-header consumer bridge compiled with the Microsoft C++ ABI. It is
// the only fixture code that invokes NVSDK_NGX_Parameter virtual methods, so the
// emitted vcall slots pin the owned facade's method surface to NVIDIA's public
// declaration. The owned test inspects these objects without linking them.

extern "C" {

int _fltused = 0;

void ngx_smoke_set_float(NVSDK_NGX_Parameter* params, const char* name, float value) {
    params->Set(name, value);
}

void ngx_smoke_set_uint(NVSDK_NGX_Parameter* params, const char* name, unsigned int value) {
    params->Set(name, value);
}

void ngx_smoke_set_int(NVSDK_NGX_Parameter* params, const char* name, int value) {
    params->Set(name, value);
}

void ngx_smoke_set_resource(NVSDK_NGX_Parameter* params, const char* name,
                            ID3D12Resource* value) {
    params->Set(name, value);
}

void ngx_smoke_set_pointer(NVSDK_NGX_Parameter* params, const char* name, void* value) {
    params->Set(name, value);
}

NVSDK_NGX_Result ngx_smoke_get_float(const NVSDK_NGX_Parameter* params, const char* name,
                                     float* value) {
    return params->Get(name, value);
}

NVSDK_NGX_Result ngx_smoke_get_uint(const NVSDK_NGX_Parameter* params, const char* name,
                                    unsigned int* value) {
    return params->Get(name, value);
}

NVSDK_NGX_Result ngx_smoke_get_int(const NVSDK_NGX_Parameter* params, const char* name,
                                   int* value) {
    return params->Get(name, value);
}

NVSDK_NGX_Result ngx_smoke_get_resource(const NVSDK_NGX_Parameter* params, const char* name,
                                        ID3D12Resource** value) {
    return params->Get(name, value);
}

void ngx_smoke_reset(NVSDK_NGX_Parameter* params) {
    params->Reset();
}

}
#endif
