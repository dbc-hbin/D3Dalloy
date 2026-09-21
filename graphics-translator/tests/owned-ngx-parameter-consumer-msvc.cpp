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
