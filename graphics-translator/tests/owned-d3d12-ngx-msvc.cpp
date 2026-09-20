#include <nvsdk_ngx.h>

// This translation unit is compiled with the Microsoft C++ ABI. It is the only
// fixture code that invokes NVSDK_NGX_Parameter virtual methods. The main PE is
// MinGW and talks to this object through the C functions below.
extern "C" {

int _fltused = 0;

unsigned int ngx_fixture_result_success() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_Success);
}

unsigned int ngx_fixture_result_feature_not_supported() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_FeatureNotSupported);
}

unsigned int ngx_fixture_result_feature_not_found() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_FeatureNotFound);
}

unsigned int ngx_fixture_result_invalid_parameter() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
}

unsigned int ngx_fixture_result_not_initialized() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_NotInitialized);
}

unsigned int ngx_fixture_result_missing_input() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_MissingInput);
}

unsigned int ngx_fixture_result_unsupported_parameter() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_UnsupportedParameter);
}

unsigned int ngx_fixture_sdk_version() {
    return static_cast<unsigned int>(NVSDK_NGX_Version_API);
}

unsigned int ngx_fixture_feature_supersampling() {
    return static_cast<unsigned int>(NVSDK_NGX_Feature_SuperSampling);
}

unsigned int ngx_fixture_flags74() {
    return static_cast<unsigned int>(
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
}

void ngx_fixture_reset(NVSDK_NGX_Parameter* params) {
    params->Reset();
}

void ngx_fixture_set_create_flags74(NVSDK_NGX_Parameter* params,
                                    unsigned int input_width,
                                    unsigned int input_height,
                                    unsigned int output_width,
                                    unsigned int output_height) {
    params->Reset();
    params->Set(NVSDK_NGX_Parameter_Width, input_width);
    params->Set(NVSDK_NGX_Parameter_Height, input_height);
    params->Set(NVSDK_NGX_Parameter_OutWidth, output_width);
    params->Set(NVSDK_NGX_Parameter_OutHeight, output_height);
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
    params->Set(
        NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<int>(
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
            NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
}

void ngx_fixture_set_frame_flags74(NVSDK_NGX_Parameter* params,
                                   ID3D12Resource* color,
                                   ID3D12Resource* output,
                                   ID3D12Resource* depth,
                                   ID3D12Resource* motion,
                                   int reset) {
    params->Reset();
    params->Set(NVSDK_NGX_Parameter_Color, color);
    params->Set(NVSDK_NGX_Parameter_Output, output);
    params->Set(NVSDK_NGX_Parameter_Depth, depth);
    params->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
    params->Set(NVSDK_NGX_Parameter_ExposureTexture,
                static_cast<ID3D12Resource*>(nullptr));
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Reset, reset);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    // Real captured raw sentinel values. The D3D12 frontend must normalize both
    // to semantic identity 1.0 before it calls the owned backend COM interface.
    params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 0.0f);
    params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 0.0f);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 64u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 64u);
}

void ngx_fixture_set_frame_output_offset(NVSDK_NGX_Parameter* params,
                                         unsigned int output_base_x) {
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, output_base_x);
}

void ngx_fixture_set_frame_nan_jitter(NVSDK_NGX_Parameter* params) {
    union {
        unsigned int bits;
        float value;
    } nan = {0x7fc00000u};
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, nan.value);
}

}
