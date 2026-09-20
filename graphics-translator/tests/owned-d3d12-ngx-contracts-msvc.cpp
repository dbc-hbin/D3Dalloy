#include <nvsdk_ngx.h>

extern "C" {

int _fltused = 0;

unsigned int ngx_contract_result_success() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_Success);
}

unsigned int ngx_contract_result_invalid_parameter() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
}

unsigned int ngx_contract_result_feature_not_found() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_FeatureNotFound);
}

unsigned int ngx_contract_result_not_initialized() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_NotInitialized);
}

unsigned int ngx_contract_result_unsupported_parameter() {
    return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_UnsupportedParameter);
}

unsigned int ngx_contract_sdk_version() {
    return static_cast<unsigned int>(NVSDK_NGX_Version_API);
}

unsigned int ngx_contract_feature_supersampling() {
    return static_cast<unsigned int>(NVSDK_NGX_Feature_SuperSampling);
}

unsigned int ngx_contract_get_output_subrect_default(
    NVSDK_NGX_Parameter* params, int* value) {
    if (!params || !value)
        return static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_InvalidParameter);
    return static_cast<unsigned int>(
        params->Get(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, value));
}

void ngx_contract_set_create(
    NVSDK_NGX_Parameter* params,
    unsigned int input_width,
    unsigned int input_height,
    unsigned int output_width,
    unsigned int output_height,
    int enable_output_subrects) {
    params->Reset();
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
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
    params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,
                enable_output_subrects ? 1 : 0);
}

void ngx_contract_set_frame(
    NVSDK_NGX_Parameter* params,
    ID3D12Resource* color,
    ID3D12Resource* output,
    ID3D12Resource* depth,
    ID3D12Resource* motion,
    unsigned int output_base_x,
    unsigned int output_base_y) {
    params->Reset();
    params->Set(NVSDK_NGX_Parameter_Color, color);
    params->Set(NVSDK_NGX_Parameter_Output, output);
    params->Set(NVSDK_NGX_Parameter_Depth, depth);
    params->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
    params->Set(NVSDK_NGX_Parameter_ExposureTexture,
                static_cast<ID3D12Resource*>(nullptr));
    params->Set(NVSDK_NGX_Parameter_TransparencyMask,
                static_cast<ID3D12Resource*>(nullptr));
    params->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                static_cast<ID3D12Resource*>(nullptr));
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Reset, 0);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 64u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 64u);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, output_base_x);
    params->Set(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, output_base_y);
}

}
