// Generated from actual DXC resource reflection; do not edit.
#include "framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.h"
#include "framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.h"
#include "fi0_p0.h"
static FfxShaderBlob fi0_p0_blob() { FfxShaderBlob b{}; b.data=fi0_p0; b.size=fi0_p0_len;
static const char* fi0_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p0_ConstantBuffer_names; b.boundConstantBuffers=fi0_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p0_ConstantBuffer_spaces;
static const char* fi0_p0_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p0_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p0_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p0_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p0_SRVTexture_names; b.boundSRVTextures=fi0_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p0_SRVTexture_spaces;
static const char* fi0_p0_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p0_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p0_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p0_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p0_UAVTexture_names; b.boundUAVTextures=fi0_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p0_UAVTexture_spaces;
return b; }
#include "fi0_p1.h"
static FfxShaderBlob fi0_p1_blob() { FfxShaderBlob b{}; b.data=fi0_p1; b.size=fi0_p1_len;
static const char* fi0_p1_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p1_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p1_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p1_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p1_ConstantBuffer_names; b.boundConstantBuffers=fi0_p1_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p1_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p1_ConstantBuffer_spaces;
static const char* fi0_p1_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p1_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p1_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p1_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p1_SRVTexture_names; b.boundSRVTextures=fi0_p1_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p1_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p1_SRVTexture_spaces;
static const char* fi0_p1_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p1_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p1_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p1_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p1_UAVTexture_names; b.boundUAVTextures=fi0_p1_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p1_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p1_UAVTexture_spaces;
return b; }
#include "fi0_p2.h"
static FfxShaderBlob fi0_p2_blob() { FfxShaderBlob b{}; b.data=fi0_p2; b.size=fi0_p2_len;
static const char* fi0_p2_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p2_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p2_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p2_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p2_ConstantBuffer_names; b.boundConstantBuffers=fi0_p2_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p2_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p2_ConstantBuffer_spaces;
static const char* fi0_p2_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p2_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p2_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p2_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p2_SRVTexture_names; b.boundSRVTextures=fi0_p2_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p2_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p2_SRVTexture_spaces;
static const char* fi0_p2_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p2_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p2_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p2_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p2_UAVTexture_names; b.boundUAVTextures=fi0_p2_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p2_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p2_UAVTexture_spaces;
return b; }
#include "fi0_p3.h"
static FfxShaderBlob fi0_p3_blob() { FfxShaderBlob b{}; b.data=fi0_p3; b.size=fi0_p3_len;
static const char* fi0_p3_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p3_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p3_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p3_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p3_ConstantBuffer_names; b.boundConstantBuffers=fi0_p3_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p3_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p3_ConstantBuffer_spaces;
static const char* fi0_p3_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p3_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p3_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p3_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p3_SRVTexture_names; b.boundSRVTextures=fi0_p3_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p3_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p3_SRVTexture_spaces;
static const char* fi0_p3_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p3_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p3_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p3_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p3_UAVTexture_names; b.boundUAVTextures=fi0_p3_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p3_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p3_UAVTexture_spaces;
return b; }
#include "fi0_p4.h"
static FfxShaderBlob fi0_p4_blob() { FfxShaderBlob b{}; b.data=fi0_p4; b.size=fi0_p4_len;
static const char* fi0_p4_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p4_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p4_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p4_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p4_ConstantBuffer_names; b.boundConstantBuffers=fi0_p4_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p4_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p4_ConstantBuffer_spaces;
static const char* fi0_p4_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p4_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p4_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p4_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p4_SRVTexture_names; b.boundSRVTextures=fi0_p4_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p4_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p4_SRVTexture_spaces;
static const char* fi0_p4_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p4_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p4_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p4_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p4_UAVTexture_names; b.boundUAVTextures=fi0_p4_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p4_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p4_UAVTexture_spaces;
return b; }
#include "fi0_p5.h"
static FfxShaderBlob fi0_p5_blob() { FfxShaderBlob b{}; b.data=fi0_p5; b.size=fi0_p5_len;
static const char* fi0_p5_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p5_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p5_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p5_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p5_ConstantBuffer_names; b.boundConstantBuffers=fi0_p5_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p5_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p5_ConstantBuffer_spaces;
static const char* fi0_p5_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p5_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p5_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p5_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p5_SRVTexture_names; b.boundSRVTextures=fi0_p5_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p5_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p5_SRVTexture_spaces;
static const char* fi0_p5_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p5_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p5_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p5_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p5_UAVTexture_names; b.boundUAVTextures=fi0_p5_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p5_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p5_UAVTexture_spaces;
return b; }
#include "fi0_p6.h"
static FfxShaderBlob fi0_p6_blob() { FfxShaderBlob b{}; b.data=fi0_p6; b.size=fi0_p6_len;
static const char* fi0_p6_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p6_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p6_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p6_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p6_ConstantBuffer_names; b.boundConstantBuffers=fi0_p6_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p6_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p6_ConstantBuffer_spaces;
static const char* fi0_p6_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p6_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p6_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p6_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p6_SRVTexture_names; b.boundSRVTextures=fi0_p6_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p6_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p6_SRVTexture_spaces;
static const char* fi0_p6_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p6_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p6_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p6_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p6_UAVTexture_names; b.boundUAVTextures=fi0_p6_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p6_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p6_UAVTexture_spaces;
return b; }
#include "fi0_p7.h"
static FfxShaderBlob fi0_p7_blob() { FfxShaderBlob b{}; b.data=fi0_p7; b.size=fi0_p7_len;
static const char* fi0_p7_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi0_p7_ConstantBuffer_slots[]={0};
static const uint32_t fi0_p7_ConstantBuffer_counts[]={1};
static const uint32_t fi0_p7_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi0_p7_ConstantBuffer_names; b.boundConstantBuffers=fi0_p7_ConstantBuffer_slots; b.boundConstantBufferCounts=fi0_p7_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi0_p7_ConstantBuffer_spaces;
static const char* fi0_p7_SRVTexture_names[]={"r_input_depth","r_input_motion_vectors"};
static const uint32_t fi0_p7_SRVTexture_slots[]={1,0};
static const uint32_t fi0_p7_SRVTexture_counts[]={1,1};
static const uint32_t fi0_p7_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi0_p7_SRVTexture_names; b.boundSRVTextures=fi0_p7_SRVTexture_slots; b.boundSRVTextureCounts=fi0_p7_SRVTexture_counts; b.boundSRVTextureSpaces=fi0_p7_SRVTexture_spaces;
static const char* fi0_p7_UAVTexture_names[]={"rw_dilated_motion_vectors","rw_dilated_depth","rw_reconstructed_depth_previous_frame"};
static const uint32_t fi0_p7_UAVTexture_slots[]={1,2,0};
static const uint32_t fi0_p7_UAVTexture_counts[]={1,1,1};
static const uint32_t fi0_p7_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=fi0_p7_UAVTexture_names; b.boundUAVTextures=fi0_p7_UAVTexture_slots; b.boundUAVTextureCounts=fi0_p7_UAVTexture_counts; b.boundUAVTextureSpaces=fi0_p7_UAVTexture_spaces;
return b; }
#include "fi1_p0.h"
static FfxShaderBlob fi1_p0_blob() { FfxShaderBlob b{}; b.data=fi1_p0; b.size=fi1_p0_len;
static const char* fi1_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi1_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi1_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi1_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi1_p0_ConstantBuffer_names; b.boundConstantBuffers=fi1_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi1_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi1_p0_ConstantBuffer_spaces;
static const char* fi1_p0_SRVTexture_names[]={"r_optical_flow_scd"};
static const uint32_t fi1_p0_SRVTexture_slots[]={0};
static const uint32_t fi1_p0_SRVTexture_counts[]={1};
static const uint32_t fi1_p0_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=fi1_p0_SRVTexture_names; b.boundSRVTextures=fi1_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi1_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi1_p0_SRVTexture_spaces;
static const char* fi1_p0_UAVTexture_names[]={"rw_disocclusion_mask","rw_game_motion_vector_field_x","rw_game_motion_vector_field_y","rw_optical_flow_motion_vector_field_x","rw_optical_flow_motion_vector_field_y"};
static const uint32_t fi1_p0_UAVTexture_slots[]={4,0,1,2,3};
static const uint32_t fi1_p0_UAVTexture_counts[]={1,1,1,1,1};
static const uint32_t fi1_p0_UAVTexture_spaces[5]={};
b.uavTextureCount=5; b.boundUAVTextureNames=fi1_p0_UAVTexture_names; b.boundUAVTextures=fi1_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi1_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi1_p0_UAVTexture_spaces;
static const char* fi1_p0_UAVBuffer_names[]={"rw_counters"};
static const uint32_t fi1_p0_UAVBuffer_slots[]={5};
static const uint32_t fi1_p0_UAVBuffer_counts[]={1};
static const uint32_t fi1_p0_UAVBuffer_spaces[1]={};
b.uavBufferCount=1; b.boundUAVBufferNames=fi1_p0_UAVBuffer_names; b.boundUAVBuffers=fi1_p0_UAVBuffer_slots; b.boundUAVBufferCounts=fi1_p0_UAVBuffer_counts; b.boundUAVBufferSpaces=fi1_p0_UAVBuffer_spaces;
return b; }
#include "fi2_p0.h"
static FfxShaderBlob fi2_p0_blob() { FfxShaderBlob b{}; b.data=fi2_p0; b.size=fi2_p0_len;
static const char* fi2_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi2_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi2_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi2_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi2_p0_ConstantBuffer_names; b.boundConstantBuffers=fi2_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi2_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi2_p0_ConstantBuffer_spaces;
static const char* fi2_p0_SRVTexture_names[]={"r_dilated_motion_vectors","r_dilated_depth","r_input_distortion_field"};
static const uint32_t fi2_p0_SRVTexture_slots[]={0,1,3};
static const uint32_t fi2_p0_SRVTexture_counts[]={1,1,1};
static const uint32_t fi2_p0_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=fi2_p0_SRVTexture_names; b.boundSRVTextures=fi2_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi2_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi2_p0_SRVTexture_spaces;
static const char* fi2_p0_UAVTexture_names[]={"rw_reconstructed_depth_interpolated_frame"};
static const uint32_t fi2_p0_UAVTexture_slots[]={0};
static const uint32_t fi2_p0_UAVTexture_counts[]={1};
static const uint32_t fi2_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=fi2_p0_UAVTexture_names; b.boundUAVTextures=fi2_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi2_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi2_p0_UAVTexture_spaces;
static const char* fi2_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi2_p0_Sampler_slots[]={0};
static const uint32_t fi2_p0_Sampler_counts[]={1};
static const uint32_t fi2_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi2_p0_Sampler_names; b.boundSamplers=fi2_p0_Sampler_slots; b.boundSamplerCounts=fi2_p0_Sampler_counts; b.boundSamplerSpaces=fi2_p0_Sampler_spaces;
return b; }
#include "fi2_p4.h"
static FfxShaderBlob fi2_p4_blob() { FfxShaderBlob b{}; b.data=fi2_p4; b.size=fi2_p4_len;
static const char* fi2_p4_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi2_p4_ConstantBuffer_slots[]={0};
static const uint32_t fi2_p4_ConstantBuffer_counts[]={1};
static const uint32_t fi2_p4_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi2_p4_ConstantBuffer_names; b.boundConstantBuffers=fi2_p4_ConstantBuffer_slots; b.boundConstantBufferCounts=fi2_p4_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi2_p4_ConstantBuffer_spaces;
static const char* fi2_p4_SRVTexture_names[]={"r_dilated_motion_vectors","r_dilated_depth","r_input_distortion_field"};
static const uint32_t fi2_p4_SRVTexture_slots[]={0,1,3};
static const uint32_t fi2_p4_SRVTexture_counts[]={1,1,1};
static const uint32_t fi2_p4_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=fi2_p4_SRVTexture_names; b.boundSRVTextures=fi2_p4_SRVTexture_slots; b.boundSRVTextureCounts=fi2_p4_SRVTexture_counts; b.boundSRVTextureSpaces=fi2_p4_SRVTexture_spaces;
static const char* fi2_p4_UAVTexture_names[]={"rw_reconstructed_depth_interpolated_frame"};
static const uint32_t fi2_p4_UAVTexture_slots[]={0};
static const uint32_t fi2_p4_UAVTexture_counts[]={1};
static const uint32_t fi2_p4_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=fi2_p4_UAVTexture_names; b.boundUAVTextures=fi2_p4_UAVTexture_slots; b.boundUAVTextureCounts=fi2_p4_UAVTexture_counts; b.boundUAVTextureSpaces=fi2_p4_UAVTexture_spaces;
static const char* fi2_p4_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi2_p4_Sampler_slots[]={0};
static const uint32_t fi2_p4_Sampler_counts[]={1};
static const uint32_t fi2_p4_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi2_p4_Sampler_names; b.boundSamplers=fi2_p4_Sampler_slots; b.boundSamplerCounts=fi2_p4_Sampler_counts; b.boundSamplerSpaces=fi2_p4_Sampler_spaces;
return b; }
#include "fi3_p0.h"
static FfxShaderBlob fi3_p0_blob() { FfxShaderBlob b{}; b.data=fi3_p0; b.size=fi3_p0_len;
static const char* fi3_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi3_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi3_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi3_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi3_p0_ConstantBuffer_names; b.boundConstantBuffers=fi3_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi3_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi3_p0_ConstantBuffer_spaces;
static const char* fi3_p0_SRVTexture_names[]={"r_previous_interpolation_source","r_current_interpolation_source","r_dilated_motion_vectors","r_dilated_depth","r_input_distortion_field"};
static const uint32_t fi3_p0_SRVTexture_slots[]={2,3,0,1,4};
static const uint32_t fi3_p0_SRVTexture_counts[]={1,1,1,1,1};
static const uint32_t fi3_p0_SRVTexture_spaces[5]={};
b.srvTextureCount=5; b.boundSRVTextureNames=fi3_p0_SRVTexture_names; b.boundSRVTextures=fi3_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi3_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi3_p0_SRVTexture_spaces;
static const char* fi3_p0_UAVTexture_names[]={"rw_game_motion_vector_field_x","rw_game_motion_vector_field_y"};
static const uint32_t fi3_p0_UAVTexture_slots[]={0,1};
static const uint32_t fi3_p0_UAVTexture_counts[]={1,1};
static const uint32_t fi3_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=fi3_p0_UAVTexture_names; b.boundUAVTextures=fi3_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi3_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi3_p0_UAVTexture_spaces;
static const char* fi3_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi3_p0_Sampler_slots[]={0};
static const uint32_t fi3_p0_Sampler_counts[]={1};
static const uint32_t fi3_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi3_p0_Sampler_names; b.boundSamplers=fi3_p0_Sampler_slots; b.boundSamplerCounts=fi3_p0_Sampler_counts; b.boundSamplerSpaces=fi3_p0_Sampler_spaces;
return b; }
#include "fi4_p0.h"
static FfxShaderBlob fi4_p0_blob() { FfxShaderBlob b{}; b.data=fi4_p0; b.size=fi4_p0_len;
static const char* fi4_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi4_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi4_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi4_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi4_p0_ConstantBuffer_names; b.boundConstantBuffers=fi4_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi4_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi4_p0_ConstantBuffer_spaces;
static const char* fi4_p0_SRVTexture_names[]={"r_previous_interpolation_source","r_current_interpolation_source","r_optical_flow"};
static const uint32_t fi4_p0_SRVTexture_slots[]={3,4,0};
static const uint32_t fi4_p0_SRVTexture_counts[]={1,1,1};
static const uint32_t fi4_p0_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=fi4_p0_SRVTexture_names; b.boundSRVTextures=fi4_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi4_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi4_p0_SRVTexture_spaces;
static const char* fi4_p0_UAVTexture_names[]={"rw_optical_flow_motion_vector_field_x","rw_optical_flow_motion_vector_field_y"};
static const uint32_t fi4_p0_UAVTexture_slots[]={0,1};
static const uint32_t fi4_p0_UAVTexture_counts[]={1,1};
static const uint32_t fi4_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=fi4_p0_UAVTexture_names; b.boundUAVTextures=fi4_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi4_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi4_p0_UAVTexture_spaces;
static const char* fi4_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi4_p0_Sampler_slots[]={0};
static const uint32_t fi4_p0_Sampler_counts[]={1};
static const uint32_t fi4_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi4_p0_Sampler_names; b.boundSamplers=fi4_p0_Sampler_slots; b.boundSamplerCounts=fi4_p0_Sampler_counts; b.boundSamplerSpaces=fi4_p0_Sampler_spaces;
return b; }
#include "fi5_p0.h"
static FfxShaderBlob fi5_p0_blob() { FfxShaderBlob b{}; b.data=fi5_p0; b.size=fi5_p0_len;
static const char* fi5_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi5_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi5_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi5_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi5_p0_ConstantBuffer_names; b.boundConstantBuffers=fi5_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi5_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi5_p0_ConstantBuffer_spaces;
static const char* fi5_p0_SRVTexture_names[]={"r_dilated_depth","r_reconstructed_depth_interpolated_frame","r_game_motion_vector_field_x","r_game_motion_vector_field_y","r_inpainting_pyramid","r_input_distortion_field","r_previous_real_depth"};
static const uint32_t fi5_p0_SRVTexture_slots[]={3,4,0,1,5,6,7};
static const uint32_t fi5_p0_SRVTexture_counts[]={1,1,1,1,1,1,1};
static const uint32_t fi5_p0_SRVTexture_spaces[7]={};
b.srvTextureCount=7; b.boundSRVTextureNames=fi5_p0_SRVTexture_names; b.boundSRVTextures=fi5_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi5_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi5_p0_SRVTexture_spaces;
static const char* fi5_p0_UAVTexture_names[]={"rw_disocclusion_mask"};
static const uint32_t fi5_p0_UAVTexture_slots[]={0};
static const uint32_t fi5_p0_UAVTexture_counts[]={1};
static const uint32_t fi5_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=fi5_p0_UAVTexture_names; b.boundUAVTextures=fi5_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi5_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi5_p0_UAVTexture_spaces;
static const char* fi5_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi5_p0_Sampler_slots[]={0};
static const uint32_t fi5_p0_Sampler_counts[]={1};
static const uint32_t fi5_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi5_p0_Sampler_names; b.boundSamplers=fi5_p0_Sampler_slots; b.boundSamplerCounts=fi5_p0_Sampler_counts; b.boundSamplerSpaces=fi5_p0_Sampler_spaces;
return b; }
#include "fi5_p4.h"
static FfxShaderBlob fi5_p4_blob() { FfxShaderBlob b{}; b.data=fi5_p4; b.size=fi5_p4_len;
static const char* fi5_p4_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi5_p4_ConstantBuffer_slots[]={0};
static const uint32_t fi5_p4_ConstantBuffer_counts[]={1};
static const uint32_t fi5_p4_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi5_p4_ConstantBuffer_names; b.boundConstantBuffers=fi5_p4_ConstantBuffer_slots; b.boundConstantBufferCounts=fi5_p4_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi5_p4_ConstantBuffer_spaces;
static const char* fi5_p4_SRVTexture_names[]={"r_dilated_depth","r_reconstructed_depth_interpolated_frame","r_game_motion_vector_field_x","r_game_motion_vector_field_y","r_inpainting_pyramid","r_input_distortion_field","r_previous_real_depth"};
static const uint32_t fi5_p4_SRVTexture_slots[]={3,4,0,1,5,6,7};
static const uint32_t fi5_p4_SRVTexture_counts[]={1,1,1,1,1,1,1};
static const uint32_t fi5_p4_SRVTexture_spaces[7]={};
b.srvTextureCount=7; b.boundSRVTextureNames=fi5_p4_SRVTexture_names; b.boundSRVTextures=fi5_p4_SRVTexture_slots; b.boundSRVTextureCounts=fi5_p4_SRVTexture_counts; b.boundSRVTextureSpaces=fi5_p4_SRVTexture_spaces;
static const char* fi5_p4_UAVTexture_names[]={"rw_disocclusion_mask"};
static const uint32_t fi5_p4_UAVTexture_slots[]={0};
static const uint32_t fi5_p4_UAVTexture_counts[]={1};
static const uint32_t fi5_p4_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=fi5_p4_UAVTexture_names; b.boundUAVTextures=fi5_p4_UAVTexture_slots; b.boundUAVTextureCounts=fi5_p4_UAVTexture_counts; b.boundUAVTextureSpaces=fi5_p4_UAVTexture_spaces;
static const char* fi5_p4_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi5_p4_Sampler_slots[]={0};
static const uint32_t fi5_p4_Sampler_counts[]={1};
static const uint32_t fi5_p4_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi5_p4_Sampler_names; b.boundSamplers=fi5_p4_Sampler_slots; b.boundSamplerCounts=fi5_p4_Sampler_counts; b.boundSamplerSpaces=fi5_p4_Sampler_spaces;
return b; }
#include "fi6_p0.h"
static FfxShaderBlob fi6_p0_blob() { FfxShaderBlob b{}; b.data=fi6_p0; b.size=fi6_p0_len;
static const char* fi6_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi6_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi6_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi6_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi6_p0_ConstantBuffer_names; b.boundConstantBuffers=fi6_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi6_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi6_p0_ConstantBuffer_spaces;
static const char* fi6_p0_SRVTexture_names[]={"r_previous_interpolation_source","r_current_interpolation_source","r_disocclusion_mask","r_game_motion_vector_field_x","r_game_motion_vector_field_y","r_optical_flow_motion_vector_field_x","r_optical_flow_motion_vector_field_y","r_inpainting_pyramid"};
static const uint32_t fi6_p0_SRVTexture_slots[]={4,5,6,0,1,2,3,7};
static const uint32_t fi6_p0_SRVTexture_counts[]={1,1,1,1,1,1,1,1};
static const uint32_t fi6_p0_SRVTexture_spaces[8]={};
b.srvTextureCount=8; b.boundSRVTextureNames=fi6_p0_SRVTexture_names; b.boundSRVTextures=fi6_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi6_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi6_p0_SRVTexture_spaces;
static const char* fi6_p0_UAVTexture_names[]={"rw_output","rw_inpainting_mask"};
static const uint32_t fi6_p0_UAVTexture_slots[]={0,1};
static const uint32_t fi6_p0_UAVTexture_counts[]={1,1};
static const uint32_t fi6_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=fi6_p0_UAVTexture_names; b.boundUAVTextures=fi6_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi6_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi6_p0_UAVTexture_spaces;
static const char* fi6_p0_SRVBuffer_names[]={"r_counters"};
static const uint32_t fi6_p0_SRVBuffer_slots[]={8};
static const uint32_t fi6_p0_SRVBuffer_counts[]={1};
static const uint32_t fi6_p0_SRVBuffer_spaces[1]={};
b.srvBufferCount=1; b.boundSRVBufferNames=fi6_p0_SRVBuffer_names; b.boundSRVBuffers=fi6_p0_SRVBuffer_slots; b.boundSRVBufferCounts=fi6_p0_SRVBuffer_counts; b.boundSRVBufferSpaces=fi6_p0_SRVBuffer_spaces;
static const char* fi6_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi6_p0_Sampler_slots[]={0};
static const uint32_t fi6_p0_Sampler_counts[]={1};
static const uint32_t fi6_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi6_p0_Sampler_names; b.boundSamplers=fi6_p0_Sampler_slots; b.boundSamplerCounts=fi6_p0_Sampler_counts; b.boundSamplerSpaces=fi6_p0_Sampler_spaces;
return b; }
#include "fi7_p0.h"
static FfxShaderBlob fi7_p0_blob() { FfxShaderBlob b{}; b.data=fi7_p0; b.size=fi7_p0_len;
static const char* fi7_p0_ConstantBuffer_names[]={"cbFI","cbInpaintingPyramid"};
static const uint32_t fi7_p0_ConstantBuffer_slots[]={0,1};
static const uint32_t fi7_p0_ConstantBuffer_counts[]={1,1};
static const uint32_t fi7_p0_ConstantBuffer_spaces[2]={};
b.cbvCount=2; b.boundConstantBufferNames=fi7_p0_ConstantBuffer_names; b.boundConstantBuffers=fi7_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi7_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi7_p0_ConstantBuffer_spaces;
static const char* fi7_p0_SRVTexture_names[]={"r_output","r_inpainting_mask"};
static const uint32_t fi7_p0_SRVTexture_slots[]={0,1};
static const uint32_t fi7_p0_SRVTexture_counts[]={1,1};
static const uint32_t fi7_p0_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi7_p0_SRVTexture_names; b.boundSRVTextures=fi7_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi7_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi7_p0_SRVTexture_spaces;
static const char* fi7_p0_UAVTexture_names[]={"rw_inpainting_pyramid0","rw_inpainting_pyramid1","rw_inpainting_pyramid2","rw_inpainting_pyramid3","rw_inpainting_pyramid4","rw_inpainting_pyramid5","rw_inpainting_pyramid6","rw_inpainting_pyramid7","rw_inpainting_pyramid8","rw_inpainting_pyramid9","rw_inpainting_pyramid10","rw_inpainting_pyramid11"};
static const uint32_t fi7_p0_UAVTexture_slots[]={1,2,3,4,5,6,7,8,9,10,11,12};
static const uint32_t fi7_p0_UAVTexture_counts[]={1,1,1,1,1,1,1,1,1,1,1,1};
static const uint32_t fi7_p0_UAVTexture_spaces[12]={};
b.uavTextureCount=12; b.boundUAVTextureNames=fi7_p0_UAVTexture_names; b.boundUAVTextures=fi7_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi7_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi7_p0_UAVTexture_spaces;
static const char* fi7_p0_UAVBuffer_names[]={"rw_counters"};
static const uint32_t fi7_p0_UAVBuffer_slots[]={0};
static const uint32_t fi7_p0_UAVBuffer_counts[]={1};
static const uint32_t fi7_p0_UAVBuffer_spaces[1]={};
b.uavBufferCount=1; b.boundUAVBufferNames=fi7_p0_UAVBuffer_names; b.boundUAVBuffers=fi7_p0_UAVBuffer_slots; b.boundUAVBufferCounts=fi7_p0_UAVBuffer_counts; b.boundUAVBufferSpaces=fi7_p0_UAVBuffer_spaces;
return b; }
#include "fi8_p0.h"
static FfxShaderBlob fi8_p0_blob() { FfxShaderBlob b{}; b.data=fi8_p0; b.size=fi8_p0_len;
static const char* fi8_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi8_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi8_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi8_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi8_p0_ConstantBuffer_names; b.boundConstantBuffers=fi8_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi8_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi8_p0_ConstantBuffer_spaces;
static const char* fi8_p0_SRVTexture_names[]={"r_current_interpolation_source","r_optical_flow_scd","r_inpainting_pyramid","r_present_backbuffer"};
static const uint32_t fi8_p0_SRVTexture_slots[]={3,0,1,2};
static const uint32_t fi8_p0_SRVTexture_counts[]={1,1,1,1};
static const uint32_t fi8_p0_SRVTexture_spaces[4]={};
b.srvTextureCount=4; b.boundSRVTextureNames=fi8_p0_SRVTexture_names; b.boundSRVTextures=fi8_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi8_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi8_p0_SRVTexture_spaces;
static const char* fi8_p0_UAVTexture_names[]={"rw_output","rw_inpainting_mask"};
static const uint32_t fi8_p0_UAVTexture_slots[]={0,1};
static const uint32_t fi8_p0_UAVTexture_counts[]={1,1};
static const uint32_t fi8_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=fi8_p0_UAVTexture_names; b.boundUAVTextures=fi8_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi8_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi8_p0_UAVTexture_spaces;
static const char* fi8_p0_SRVBuffer_names[]={"r_counters"};
static const uint32_t fi8_p0_SRVBuffer_slots[]={4};
static const uint32_t fi8_p0_SRVBuffer_counts[]={1};
static const uint32_t fi8_p0_SRVBuffer_spaces[1]={};
b.srvBufferCount=1; b.boundSRVBufferNames=fi8_p0_SRVBuffer_names; b.boundSRVBuffers=fi8_p0_SRVBuffer_slots; b.boundSRVBufferCounts=fi8_p0_SRVBuffer_counts; b.boundSRVBufferSpaces=fi8_p0_SRVBuffer_spaces;
return b; }
#include "fi9_p0.h"
static FfxShaderBlob fi9_p0_blob() { FfxShaderBlob b{}; b.data=fi9_p0; b.size=fi9_p0_len;
static const char* fi9_p0_ConstantBuffer_names[]={"cbFI","cbInpaintingPyramid"};
static const uint32_t fi9_p0_ConstantBuffer_slots[]={0,1};
static const uint32_t fi9_p0_ConstantBuffer_counts[]={1,1};
static const uint32_t fi9_p0_ConstantBuffer_spaces[2]={};
b.cbvCount=2; b.boundConstantBufferNames=fi9_p0_ConstantBuffer_names; b.boundConstantBuffers=fi9_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi9_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi9_p0_ConstantBuffer_spaces;
static const char* fi9_p0_SRVTexture_names[]={"r_game_motion_vector_field_x","r_game_motion_vector_field_y"};
static const uint32_t fi9_p0_SRVTexture_slots[]={0,1};
static const uint32_t fi9_p0_SRVTexture_counts[]={1,1};
static const uint32_t fi9_p0_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=fi9_p0_SRVTexture_names; b.boundSRVTextures=fi9_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi9_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi9_p0_SRVTexture_spaces;
static const char* fi9_p0_UAVTexture_names[]={"rw_inpainting_pyramid0","rw_inpainting_pyramid1","rw_inpainting_pyramid2","rw_inpainting_pyramid3","rw_inpainting_pyramid4","rw_inpainting_pyramid5","rw_inpainting_pyramid6","rw_inpainting_pyramid7","rw_inpainting_pyramid8","rw_inpainting_pyramid9","rw_inpainting_pyramid10","rw_inpainting_pyramid11"};
static const uint32_t fi9_p0_UAVTexture_slots[]={1,2,3,4,5,6,7,8,9,10,11,12};
static const uint32_t fi9_p0_UAVTexture_counts[]={1,1,1,1,1,1,1,1,1,1,1,1};
static const uint32_t fi9_p0_UAVTexture_spaces[12]={};
b.uavTextureCount=12; b.boundUAVTextureNames=fi9_p0_UAVTexture_names; b.boundUAVTextures=fi9_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi9_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi9_p0_UAVTexture_spaces;
static const char* fi9_p0_UAVBuffer_names[]={"rw_counters"};
static const uint32_t fi9_p0_UAVBuffer_slots[]={0};
static const uint32_t fi9_p0_UAVBuffer_counts[]={1};
static const uint32_t fi9_p0_UAVBuffer_spaces[1]={};
b.uavBufferCount=1; b.boundUAVBufferNames=fi9_p0_UAVBuffer_names; b.boundUAVBuffers=fi9_p0_UAVBuffer_slots; b.boundUAVBufferCounts=fi9_p0_UAVBuffer_counts; b.boundUAVBufferSpaces=fi9_p0_UAVBuffer_spaces;
return b; }
#include "fi10_p0.h"
static FfxShaderBlob fi10_p0_blob() { FfxShaderBlob b{}; b.data=fi10_p0; b.size=fi10_p0_len;
static const char* fi10_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t fi10_p0_ConstantBuffer_slots[]={0};
static const uint32_t fi10_p0_ConstantBuffer_counts[]={1};
static const uint32_t fi10_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=fi10_p0_ConstantBuffer_names; b.boundConstantBuffers=fi10_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=fi10_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=fi10_p0_ConstantBuffer_spaces;
static const char* fi10_p0_SRVTexture_names[]={"r_current_interpolation_source","r_disocclusion_mask","r_game_motion_vector_field_x","r_game_motion_vector_field_y","r_optical_flow_motion_vector_field_x","r_optical_flow_motion_vector_field_y","r_inpainting_pyramid","r_present_backbuffer","r_input_distortion_field"};
static const uint32_t fi10_p0_SRVTexture_slots[]={7,4,0,1,2,3,6,5,8};
static const uint32_t fi10_p0_SRVTexture_counts[]={1,1,1,1,1,1,1,1,1};
static const uint32_t fi10_p0_SRVTexture_spaces[9]={};
b.srvTextureCount=9; b.boundSRVTextureNames=fi10_p0_SRVTexture_names; b.boundSRVTextures=fi10_p0_SRVTexture_slots; b.boundSRVTextureCounts=fi10_p0_SRVTexture_counts; b.boundSRVTextureSpaces=fi10_p0_SRVTexture_spaces;
static const char* fi10_p0_UAVTexture_names[]={"rw_output"};
static const uint32_t fi10_p0_UAVTexture_slots[]={0};
static const uint32_t fi10_p0_UAVTexture_counts[]={1};
static const uint32_t fi10_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=fi10_p0_UAVTexture_names; b.boundUAVTextures=fi10_p0_UAVTexture_slots; b.boundUAVTextureCounts=fi10_p0_UAVTexture_counts; b.boundUAVTextureSpaces=fi10_p0_UAVTexture_spaces;
static const char* fi10_p0_SRVBuffer_names[]={"r_counters"};
static const uint32_t fi10_p0_SRVBuffer_slots[]={9};
static const uint32_t fi10_p0_SRVBuffer_counts[]={1};
static const uint32_t fi10_p0_SRVBuffer_spaces[1]={};
b.srvBufferCount=1; b.boundSRVBufferNames=fi10_p0_SRVBuffer_names; b.boundSRVBuffers=fi10_p0_SRVBuffer_slots; b.boundSRVBufferCounts=fi10_p0_SRVBuffer_counts; b.boundSRVBufferSpaces=fi10_p0_SRVBuffer_spaces;
static const char* fi10_p0_Sampler_names[]={"s_LinearClamp"};
static const uint32_t fi10_p0_Sampler_slots[]={0};
static const uint32_t fi10_p0_Sampler_counts[]={1};
static const uint32_t fi10_p0_Sampler_spaces[1]={};
b.samplerCount=1; b.boundSamplerNames=fi10_p0_Sampler_names; b.boundSamplers=fi10_p0_Sampler_slots; b.boundSamplerCounts=fi10_p0_Sampler_counts; b.boundSamplerSpaces=fi10_p0_Sampler_spaces;
return b; }
#include "bi0_p0.h"
static FfxShaderBlob bi0_p0_blob() { FfxShaderBlob b{}; b.data=bi0_p0; b.size=bi0_p0_len;
static const char* bi0_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t bi0_p0_ConstantBuffer_slots[]={0};
static const uint32_t bi0_p0_ConstantBuffer_counts[]={1};
static const uint32_t bi0_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=bi0_p0_ConstantBuffer_names; b.boundConstantBuffers=bi0_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=bi0_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=bi0_p0_ConstantBuffer_spaces;
static const char* bi0_p0_SRVTexture_names[]={"r_dilated_motion_vectors","r_dilated_depth","r_previous_real_depth"};
static const uint32_t bi0_p0_SRVTexture_slots[]={1,0,2};
static const uint32_t bi0_p0_SRVTexture_counts[]={1,1,1};
static const uint32_t bi0_p0_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=bi0_p0_SRVTexture_names; b.boundSRVTextures=bi0_p0_SRVTexture_slots; b.boundSRVTextureCounts=bi0_p0_SRVTexture_counts; b.boundSRVTextureSpaces=bi0_p0_SRVTexture_spaces;
static const char* bi0_p0_UAVTexture_names[]={"rw_previous_vector_x","rw_previous_vector_y"};
static const uint32_t bi0_p0_UAVTexture_slots[]={3,4};
static const uint32_t bi0_p0_UAVTexture_counts[]={1,1};
static const uint32_t bi0_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=bi0_p0_UAVTexture_names; b.boundUAVTextures=bi0_p0_UAVTexture_slots; b.boundUAVTextureCounts=bi0_p0_UAVTexture_counts; b.boundUAVTextureSpaces=bi0_p0_UAVTexture_spaces;
return b; }
#include "bi1_p0.h"
static FfxShaderBlob bi1_p0_blob() { FfxShaderBlob b{}; b.data=bi1_p0; b.size=bi1_p0_len;
static const char* bi1_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t bi1_p0_ConstantBuffer_slots[]={0};
static const uint32_t bi1_p0_ConstantBuffer_counts[]={1};
static const uint32_t bi1_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=bi1_p0_ConstantBuffer_names; b.boundConstantBuffers=bi1_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=bi1_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=bi1_p0_ConstantBuffer_spaces;
static const char* bi1_p0_SRVTexture_names[]={"r_previous_vector_x"};
static const uint32_t bi1_p0_SRVTexture_slots[]={3};
static const uint32_t bi1_p0_SRVTexture_counts[]={1};
static const uint32_t bi1_p0_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=bi1_p0_SRVTexture_names; b.boundSRVTextures=bi1_p0_SRVTexture_slots; b.boundSRVTextureCounts=bi1_p0_SRVTexture_counts; b.boundSRVTextureSpaces=bi1_p0_SRVTexture_spaces;
static const char* bi1_p0_UAVTexture_names[]={"rw_previous_seed"};
static const uint32_t bi1_p0_UAVTexture_slots[]={5};
static const uint32_t bi1_p0_UAVTexture_counts[]={1};
static const uint32_t bi1_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=bi1_p0_UAVTexture_names; b.boundUAVTextures=bi1_p0_UAVTexture_slots; b.boundUAVTextureCounts=bi1_p0_UAVTexture_counts; b.boundUAVTextureSpaces=bi1_p0_UAVTexture_spaces;
return b; }
#include "bi2_p0.h"
static FfxShaderBlob bi2_p0_blob() { FfxShaderBlob b{}; b.data=bi2_p0; b.size=bi2_p0_len;
static const char* bi2_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t bi2_p0_ConstantBuffer_slots[]={0};
static const uint32_t bi2_p0_ConstantBuffer_counts[]={1};
static const uint32_t bi2_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=bi2_p0_ConstantBuffer_names; b.boundConstantBuffers=bi2_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=bi2_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=bi2_p0_ConstantBuffer_spaces;
static const char* bi2_p0_SRVTexture_names[]={"r_previous_real_depth","r_previous_seed"};
static const uint32_t bi2_p0_SRVTexture_slots[]={2,5};
static const uint32_t bi2_p0_SRVTexture_counts[]={1,1};
static const uint32_t bi2_p0_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=bi2_p0_SRVTexture_names; b.boundSRVTextures=bi2_p0_SRVTexture_slots; b.boundSRVTextureCounts=bi2_p0_SRVTexture_counts; b.boundSRVTextureSpaces=bi2_p0_SRVTexture_spaces;
static const char* bi2_p0_UAVTexture_names[]={"rw_previous_seed"};
static const uint32_t bi2_p0_UAVTexture_slots[]={5};
static const uint32_t bi2_p0_UAVTexture_counts[]={1};
static const uint32_t bi2_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=bi2_p0_UAVTexture_names; b.boundUAVTextures=bi2_p0_UAVTexture_slots; b.boundUAVTextureCounts=bi2_p0_UAVTexture_counts; b.boundUAVTextureSpaces=bi2_p0_UAVTexture_spaces;
return b; }
#include "bi3_p0.h"
static FfxShaderBlob bi3_p0_blob() { FfxShaderBlob b{}; b.data=bi3_p0; b.size=bi3_p0_len;
static const char* bi3_p0_ConstantBuffer_names[]={"cbFI"};
static const uint32_t bi3_p0_ConstantBuffer_slots[]={0};
static const uint32_t bi3_p0_ConstantBuffer_counts[]={1};
static const uint32_t bi3_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=bi3_p0_ConstantBuffer_names; b.boundConstantBuffers=bi3_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=bi3_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=bi3_p0_ConstantBuffer_spaces;
static const char* bi3_p0_SRVTexture_names[]={"r_previous_real_depth","r_previous_vector_x","r_previous_vector_y","r_previous_seed"};
static const uint32_t bi3_p0_SRVTexture_slots[]={2,3,4,5};
static const uint32_t bi3_p0_SRVTexture_counts[]={1,1,1,1};
static const uint32_t bi3_p0_SRVTexture_spaces[4]={};
b.srvTextureCount=4; b.boundSRVTextureNames=bi3_p0_SRVTexture_names; b.boundSRVTextures=bi3_p0_SRVTexture_slots; b.boundSRVTextureCounts=bi3_p0_SRVTexture_counts; b.boundSRVTextureSpaces=bi3_p0_SRVTexture_spaces;
static const char* bi3_p0_UAVTexture_names[]={"rw_reconstructed_depth_interpolated_frame","rw_game_motion_vector_field_x","rw_game_motion_vector_field_y"};
static const uint32_t bi3_p0_UAVTexture_slots[]={0,1,2};
static const uint32_t bi3_p0_UAVTexture_counts[]={1,1,1};
static const uint32_t bi3_p0_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=bi3_p0_UAVTexture_names; b.boundUAVTextures=bi3_p0_UAVTexture_slots; b.boundUAVTextureCounts=bi3_p0_UAVTexture_counts; b.boundUAVTextureSpaces=bi3_p0_UAVTexture_spaces;
return b; }
#include "bi3_p4.h"
static FfxShaderBlob bi3_p4_blob() { FfxShaderBlob b{}; b.data=bi3_p4; b.size=bi3_p4_len;
static const char* bi3_p4_ConstantBuffer_names[]={"cbFI"};
static const uint32_t bi3_p4_ConstantBuffer_slots[]={0};
static const uint32_t bi3_p4_ConstantBuffer_counts[]={1};
static const uint32_t bi3_p4_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=bi3_p4_ConstantBuffer_names; b.boundConstantBuffers=bi3_p4_ConstantBuffer_slots; b.boundConstantBufferCounts=bi3_p4_ConstantBuffer_counts; b.boundConstantBufferSpaces=bi3_p4_ConstantBuffer_spaces;
static const char* bi3_p4_SRVTexture_names[]={"r_previous_real_depth","r_previous_vector_x","r_previous_vector_y","r_previous_seed"};
static const uint32_t bi3_p4_SRVTexture_slots[]={2,3,4,5};
static const uint32_t bi3_p4_SRVTexture_counts[]={1,1,1,1};
static const uint32_t bi3_p4_SRVTexture_spaces[4]={};
b.srvTextureCount=4; b.boundSRVTextureNames=bi3_p4_SRVTexture_names; b.boundSRVTextures=bi3_p4_SRVTexture_slots; b.boundSRVTextureCounts=bi3_p4_SRVTexture_counts; b.boundSRVTextureSpaces=bi3_p4_SRVTexture_spaces;
static const char* bi3_p4_UAVTexture_names[]={"rw_reconstructed_depth_interpolated_frame","rw_game_motion_vector_field_x","rw_game_motion_vector_field_y"};
static const uint32_t bi3_p4_UAVTexture_slots[]={0,1,2};
static const uint32_t bi3_p4_UAVTexture_counts[]={1,1,1};
static const uint32_t bi3_p4_UAVTexture_spaces[3]={};
b.uavTextureCount=3; b.boundUAVTextureNames=bi3_p4_UAVTexture_names; b.boundUAVTextures=bi3_p4_UAVTexture_slots; b.boundUAVTextureCounts=bi3_p4_UAVTexture_counts; b.boundUAVTextureSpaces=bi3_p4_UAVTexture_spaces;
return b; }
#include "of0_p0.h"
static FfxShaderBlob of0_p0_blob() { FfxShaderBlob b{}; b.data=of0_p0; b.size=of0_p0_len;
static const char* of0_p0_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of0_p0_ConstantBuffer_slots[]={0};
static const uint32_t of0_p0_ConstantBuffer_counts[]={1};
static const uint32_t of0_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of0_p0_ConstantBuffer_names; b.boundConstantBuffers=of0_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=of0_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=of0_p0_ConstantBuffer_spaces;
static const char* of0_p0_SRVTexture_names[]={"r_input_color"};
static const uint32_t of0_p0_SRVTexture_slots[]={0};
static const uint32_t of0_p0_SRVTexture_counts[]={1};
static const uint32_t of0_p0_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of0_p0_SRVTexture_names; b.boundSRVTextures=of0_p0_SRVTexture_slots; b.boundSRVTextureCounts=of0_p0_SRVTexture_counts; b.boundSRVTextureSpaces=of0_p0_SRVTexture_spaces;
static const char* of0_p0_UAVTexture_names[]={"rw_optical_flow_input"};
static const uint32_t of0_p0_UAVTexture_slots[]={0};
static const uint32_t of0_p0_UAVTexture_counts[]={1};
static const uint32_t of0_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of0_p0_UAVTexture_names; b.boundUAVTextures=of0_p0_UAVTexture_slots; b.boundUAVTextureCounts=of0_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of0_p0_UAVTexture_spaces;
return b; }
#include "of1_p0.h"
static FfxShaderBlob of1_p0_blob() { FfxShaderBlob b{}; b.data=of1_p0; b.size=of1_p0_len;
static const char* of1_p0_UAVTexture_names[]={"rw_optical_flow_input","rw_optical_flow_input_level_1","rw_optical_flow_input_level_2","rw_optical_flow_input_level_3","rw_optical_flow_input_level_4","rw_optical_flow_input_level_5","rw_optical_flow_input_level_6"};
static const uint32_t of1_p0_UAVTexture_slots[]={0,1,2,3,4,5,6};
static const uint32_t of1_p0_UAVTexture_counts[]={1,1,1,1,1,1,1};
static const uint32_t of1_p0_UAVTexture_spaces[7]={};
b.uavTextureCount=7; b.boundUAVTextureNames=of1_p0_UAVTexture_names; b.boundUAVTextures=of1_p0_UAVTexture_slots; b.boundUAVTextureCounts=of1_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of1_p0_UAVTexture_spaces;
return b; }
#include "of2_p0.h"
static FfxShaderBlob of2_p0_blob() { FfxShaderBlob b{}; b.data=of2_p0; b.size=of2_p0_len;
static const char* of2_p0_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of2_p0_ConstantBuffer_slots[]={0};
static const uint32_t of2_p0_ConstantBuffer_counts[]={1};
static const uint32_t of2_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of2_p0_ConstantBuffer_names; b.boundConstantBuffers=of2_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=of2_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=of2_p0_ConstantBuffer_spaces;
static const char* of2_p0_SRVTexture_names[]={"r_optical_flow_input"};
static const uint32_t of2_p0_SRVTexture_slots[]={0};
static const uint32_t of2_p0_SRVTexture_counts[]={1};
static const uint32_t of2_p0_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of2_p0_SRVTexture_names; b.boundSRVTextures=of2_p0_SRVTexture_slots; b.boundSRVTextureCounts=of2_p0_SRVTexture_counts; b.boundSRVTextureSpaces=of2_p0_SRVTexture_spaces;
static const char* of2_p0_UAVTexture_names[]={"rw_optical_flow_scd_histogram"};
static const uint32_t of2_p0_UAVTexture_slots[]={0};
static const uint32_t of2_p0_UAVTexture_counts[]={1};
static const uint32_t of2_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of2_p0_UAVTexture_names; b.boundUAVTextures=of2_p0_UAVTexture_slots; b.boundUAVTextureCounts=of2_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of2_p0_UAVTexture_spaces;
return b; }
#include "of3_p0.h"
static FfxShaderBlob of3_p0_blob() { FfxShaderBlob b{}; b.data=of3_p0; b.size=of3_p0_len;
static const char* of3_p0_UAVTexture_names[]={"rw_optical_flow_scd_histogram","rw_optical_flow_scd_previous_histogram","rw_optical_flow_scd_temp","rw_optical_flow_scd_output"};
static const uint32_t of3_p0_UAVTexture_slots[]={0,1,2,3};
static const uint32_t of3_p0_UAVTexture_counts[]={1,1,1,1};
static const uint32_t of3_p0_UAVTexture_spaces[4]={};
b.uavTextureCount=4; b.boundUAVTextureNames=of3_p0_UAVTexture_names; b.boundUAVTextures=of3_p0_UAVTexture_slots; b.boundUAVTextureCounts=of3_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of3_p0_UAVTexture_spaces;
return b; }
#include "of4_p0.h"
static FfxShaderBlob of4_p0_blob() { FfxShaderBlob b{}; b.data=of4_p0; b.size=of4_p0_len;
static const char* of4_p0_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of4_p0_ConstantBuffer_slots[]={0};
static const uint32_t of4_p0_ConstantBuffer_counts[]={1};
static const uint32_t of4_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of4_p0_ConstantBuffer_names; b.boundConstantBuffers=of4_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=of4_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=of4_p0_ConstantBuffer_spaces;
static const char* of4_p0_SRVTexture_names[]={"r_optical_flow_input","r_optical_flow_previous_input"};
static const uint32_t of4_p0_SRVTexture_slots[]={0,1};
static const uint32_t of4_p0_SRVTexture_counts[]={1,1};
static const uint32_t of4_p0_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=of4_p0_SRVTexture_names; b.boundSRVTextures=of4_p0_SRVTexture_slots; b.boundSRVTextureCounts=of4_p0_SRVTexture_counts; b.boundSRVTextureSpaces=of4_p0_SRVTexture_spaces;
static const char* of4_p0_UAVTexture_names[]={"rw_optical_flow","rw_optical_flow_scd_output"};
static const uint32_t of4_p0_UAVTexture_slots[]={0,1};
static const uint32_t of4_p0_UAVTexture_counts[]={1,1};
static const uint32_t of4_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=of4_p0_UAVTexture_names; b.boundUAVTextures=of4_p0_UAVTexture_slots; b.boundUAVTextureCounts=of4_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of4_p0_UAVTexture_spaces;
return b; }
#include "of5_p0.h"
static FfxShaderBlob of5_p0_blob() { FfxShaderBlob b{}; b.data=of5_p0; b.size=of5_p0_len;
static const char* of5_p0_SRVTexture_names[]={"r_optical_flow_previous"};
static const uint32_t of5_p0_SRVTexture_slots[]={0};
static const uint32_t of5_p0_SRVTexture_counts[]={1};
static const uint32_t of5_p0_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of5_p0_SRVTexture_names; b.boundSRVTextures=of5_p0_SRVTexture_slots; b.boundSRVTextureCounts=of5_p0_SRVTexture_counts; b.boundSRVTextureSpaces=of5_p0_SRVTexture_spaces;
static const char* of5_p0_UAVTexture_names[]={"rw_optical_flow"};
static const uint32_t of5_p0_UAVTexture_slots[]={0};
static const uint32_t of5_p0_UAVTexture_counts[]={1};
static const uint32_t of5_p0_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of5_p0_UAVTexture_names; b.boundUAVTextures=of5_p0_UAVTexture_slots; b.boundUAVTextureCounts=of5_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of5_p0_UAVTexture_spaces;
return b; }
#include "of6_p0.h"
static FfxShaderBlob of6_p0_blob() { FfxShaderBlob b{}; b.data=of6_p0; b.size=of6_p0_len;
static const char* of6_p0_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of6_p0_ConstantBuffer_slots[]={0};
static const uint32_t of6_p0_ConstantBuffer_counts[]={1};
static const uint32_t of6_p0_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of6_p0_ConstantBuffer_names; b.boundConstantBuffers=of6_p0_ConstantBuffer_slots; b.boundConstantBufferCounts=of6_p0_ConstantBuffer_counts; b.boundConstantBufferSpaces=of6_p0_ConstantBuffer_spaces;
static const char* of6_p0_SRVTexture_names[]={"r_optical_flow_input","r_optical_flow_previous_input","r_optical_flow"};
static const uint32_t of6_p0_SRVTexture_slots[]={0,1,2};
static const uint32_t of6_p0_SRVTexture_counts[]={1,1,1};
static const uint32_t of6_p0_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=of6_p0_SRVTexture_names; b.boundSRVTextures=of6_p0_SRVTexture_slots; b.boundSRVTextureCounts=of6_p0_SRVTexture_counts; b.boundSRVTextureSpaces=of6_p0_SRVTexture_spaces;
static const char* of6_p0_UAVTexture_names[]={"rw_optical_flow_next_level","rw_optical_flow_scd_output"};
static const uint32_t of6_p0_UAVTexture_slots[]={0,1};
static const uint32_t of6_p0_UAVTexture_counts[]={1,1};
static const uint32_t of6_p0_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=of6_p0_UAVTexture_names; b.boundUAVTextures=of6_p0_UAVTexture_slots; b.boundUAVTextureCounts=of6_p0_UAVTexture_counts; b.boundUAVTextureSpaces=of6_p0_UAVTexture_spaces;
return b; }
#include "of0_p1.h"
static FfxShaderBlob of0_p1_blob() { FfxShaderBlob b{}; b.data=of0_p1; b.size=of0_p1_len;
static const char* of0_p1_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of0_p1_ConstantBuffer_slots[]={0};
static const uint32_t of0_p1_ConstantBuffer_counts[]={1};
static const uint32_t of0_p1_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of0_p1_ConstantBuffer_names; b.boundConstantBuffers=of0_p1_ConstantBuffer_slots; b.boundConstantBufferCounts=of0_p1_ConstantBuffer_counts; b.boundConstantBufferSpaces=of0_p1_ConstantBuffer_spaces;
static const char* of0_p1_SRVTexture_names[]={"r_input_color"};
static const uint32_t of0_p1_SRVTexture_slots[]={0};
static const uint32_t of0_p1_SRVTexture_counts[]={1};
static const uint32_t of0_p1_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of0_p1_SRVTexture_names; b.boundSRVTextures=of0_p1_SRVTexture_slots; b.boundSRVTextureCounts=of0_p1_SRVTexture_counts; b.boundSRVTextureSpaces=of0_p1_SRVTexture_spaces;
static const char* of0_p1_UAVTexture_names[]={"rw_optical_flow_input"};
static const uint32_t of0_p1_UAVTexture_slots[]={0};
static const uint32_t of0_p1_UAVTexture_counts[]={1};
static const uint32_t of0_p1_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of0_p1_UAVTexture_names; b.boundUAVTextures=of0_p1_UAVTexture_slots; b.boundUAVTextureCounts=of0_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of0_p1_UAVTexture_spaces;
return b; }
#include "of1_p1.h"
static FfxShaderBlob of1_p1_blob() { FfxShaderBlob b{}; b.data=of1_p1; b.size=of1_p1_len;
static const char* of1_p1_UAVTexture_names[]={"rw_optical_flow_input","rw_optical_flow_input_level_1","rw_optical_flow_input_level_2","rw_optical_flow_input_level_3","rw_optical_flow_input_level_4","rw_optical_flow_input_level_5","rw_optical_flow_input_level_6"};
static const uint32_t of1_p1_UAVTexture_slots[]={0,1,2,3,4,5,6};
static const uint32_t of1_p1_UAVTexture_counts[]={1,1,1,1,1,1,1};
static const uint32_t of1_p1_UAVTexture_spaces[7]={};
b.uavTextureCount=7; b.boundUAVTextureNames=of1_p1_UAVTexture_names; b.boundUAVTextures=of1_p1_UAVTexture_slots; b.boundUAVTextureCounts=of1_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of1_p1_UAVTexture_spaces;
return b; }
#include "of2_p1.h"
static FfxShaderBlob of2_p1_blob() { FfxShaderBlob b{}; b.data=of2_p1; b.size=of2_p1_len;
static const char* of2_p1_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of2_p1_ConstantBuffer_slots[]={0};
static const uint32_t of2_p1_ConstantBuffer_counts[]={1};
static const uint32_t of2_p1_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of2_p1_ConstantBuffer_names; b.boundConstantBuffers=of2_p1_ConstantBuffer_slots; b.boundConstantBufferCounts=of2_p1_ConstantBuffer_counts; b.boundConstantBufferSpaces=of2_p1_ConstantBuffer_spaces;
static const char* of2_p1_SRVTexture_names[]={"r_optical_flow_input"};
static const uint32_t of2_p1_SRVTexture_slots[]={0};
static const uint32_t of2_p1_SRVTexture_counts[]={1};
static const uint32_t of2_p1_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of2_p1_SRVTexture_names; b.boundSRVTextures=of2_p1_SRVTexture_slots; b.boundSRVTextureCounts=of2_p1_SRVTexture_counts; b.boundSRVTextureSpaces=of2_p1_SRVTexture_spaces;
static const char* of2_p1_UAVTexture_names[]={"rw_optical_flow_scd_histogram"};
static const uint32_t of2_p1_UAVTexture_slots[]={0};
static const uint32_t of2_p1_UAVTexture_counts[]={1};
static const uint32_t of2_p1_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of2_p1_UAVTexture_names; b.boundUAVTextures=of2_p1_UAVTexture_slots; b.boundUAVTextureCounts=of2_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of2_p1_UAVTexture_spaces;
return b; }
#include "of3_p1.h"
static FfxShaderBlob of3_p1_blob() { FfxShaderBlob b{}; b.data=of3_p1; b.size=of3_p1_len;
static const char* of3_p1_UAVTexture_names[]={"rw_optical_flow_scd_histogram","rw_optical_flow_scd_previous_histogram","rw_optical_flow_scd_temp","rw_optical_flow_scd_output"};
static const uint32_t of3_p1_UAVTexture_slots[]={0,1,2,3};
static const uint32_t of3_p1_UAVTexture_counts[]={1,1,1,1};
static const uint32_t of3_p1_UAVTexture_spaces[4]={};
b.uavTextureCount=4; b.boundUAVTextureNames=of3_p1_UAVTexture_names; b.boundUAVTextures=of3_p1_UAVTexture_slots; b.boundUAVTextureCounts=of3_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of3_p1_UAVTexture_spaces;
return b; }
#include "of4_p1.h"
static FfxShaderBlob of4_p1_blob() { FfxShaderBlob b{}; b.data=of4_p1; b.size=of4_p1_len;
static const char* of4_p1_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of4_p1_ConstantBuffer_slots[]={0};
static const uint32_t of4_p1_ConstantBuffer_counts[]={1};
static const uint32_t of4_p1_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of4_p1_ConstantBuffer_names; b.boundConstantBuffers=of4_p1_ConstantBuffer_slots; b.boundConstantBufferCounts=of4_p1_ConstantBuffer_counts; b.boundConstantBufferSpaces=of4_p1_ConstantBuffer_spaces;
static const char* of4_p1_SRVTexture_names[]={"r_optical_flow_input","r_optical_flow_previous_input"};
static const uint32_t of4_p1_SRVTexture_slots[]={0,1};
static const uint32_t of4_p1_SRVTexture_counts[]={1,1};
static const uint32_t of4_p1_SRVTexture_spaces[2]={};
b.srvTextureCount=2; b.boundSRVTextureNames=of4_p1_SRVTexture_names; b.boundSRVTextures=of4_p1_SRVTexture_slots; b.boundSRVTextureCounts=of4_p1_SRVTexture_counts; b.boundSRVTextureSpaces=of4_p1_SRVTexture_spaces;
static const char* of4_p1_UAVTexture_names[]={"rw_optical_flow","rw_optical_flow_scd_output"};
static const uint32_t of4_p1_UAVTexture_slots[]={0,1};
static const uint32_t of4_p1_UAVTexture_counts[]={1,1};
static const uint32_t of4_p1_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=of4_p1_UAVTexture_names; b.boundUAVTextures=of4_p1_UAVTexture_slots; b.boundUAVTextureCounts=of4_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of4_p1_UAVTexture_spaces;
return b; }
#include "of5_p1.h"
static FfxShaderBlob of5_p1_blob() { FfxShaderBlob b{}; b.data=of5_p1; b.size=of5_p1_len;
static const char* of5_p1_SRVTexture_names[]={"r_optical_flow_previous"};
static const uint32_t of5_p1_SRVTexture_slots[]={0};
static const uint32_t of5_p1_SRVTexture_counts[]={1};
static const uint32_t of5_p1_SRVTexture_spaces[1]={};
b.srvTextureCount=1; b.boundSRVTextureNames=of5_p1_SRVTexture_names; b.boundSRVTextures=of5_p1_SRVTexture_slots; b.boundSRVTextureCounts=of5_p1_SRVTexture_counts; b.boundSRVTextureSpaces=of5_p1_SRVTexture_spaces;
static const char* of5_p1_UAVTexture_names[]={"rw_optical_flow"};
static const uint32_t of5_p1_UAVTexture_slots[]={0};
static const uint32_t of5_p1_UAVTexture_counts[]={1};
static const uint32_t of5_p1_UAVTexture_spaces[1]={};
b.uavTextureCount=1; b.boundUAVTextureNames=of5_p1_UAVTexture_names; b.boundUAVTextures=of5_p1_UAVTexture_slots; b.boundUAVTextureCounts=of5_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of5_p1_UAVTexture_spaces;
return b; }
#include "of6_p1.h"
static FfxShaderBlob of6_p1_blob() { FfxShaderBlob b{}; b.data=of6_p1; b.size=of6_p1_len;
static const char* of6_p1_ConstantBuffer_names[]={"cbOF"};
static const uint32_t of6_p1_ConstantBuffer_slots[]={0};
static const uint32_t of6_p1_ConstantBuffer_counts[]={1};
static const uint32_t of6_p1_ConstantBuffer_spaces[1]={};
b.cbvCount=1; b.boundConstantBufferNames=of6_p1_ConstantBuffer_names; b.boundConstantBuffers=of6_p1_ConstantBuffer_slots; b.boundConstantBufferCounts=of6_p1_ConstantBuffer_counts; b.boundConstantBufferSpaces=of6_p1_ConstantBuffer_spaces;
static const char* of6_p1_SRVTexture_names[]={"r_optical_flow_input","r_optical_flow_previous_input","r_optical_flow"};
static const uint32_t of6_p1_SRVTexture_slots[]={0,1,2};
static const uint32_t of6_p1_SRVTexture_counts[]={1,1,1};
static const uint32_t of6_p1_SRVTexture_spaces[3]={};
b.srvTextureCount=3; b.boundSRVTextureNames=of6_p1_SRVTexture_names; b.boundSRVTextures=of6_p1_SRVTexture_slots; b.boundSRVTextureCounts=of6_p1_SRVTexture_counts; b.boundSRVTextureSpaces=of6_p1_SRVTexture_spaces;
static const char* of6_p1_UAVTexture_names[]={"rw_optical_flow_next_level","rw_optical_flow_scd_output"};
static const uint32_t of6_p1_UAVTexture_slots[]={0,1};
static const uint32_t of6_p1_UAVTexture_counts[]={1,1};
static const uint32_t of6_p1_UAVTexture_spaces[2]={};
b.uavTextureCount=2; b.boundUAVTextureNames=of6_p1_UAVTexture_names; b.boundUAVTextures=of6_p1_UAVTexture_slots; b.boundUAVTextureCounts=of6_p1_UAVTexture_counts; b.boundUAVTextureSpaces=of6_p1_UAVTexture_spaces;
return b; }
extern "C" FfxErrorCode frameInterpolationGetPermutationBlobByIndex(FfxFrameInterpolationPass pass,uint32_t options,FfxShaderBlob* out) { switch(options & 7u) {
case 0: switch(pass) {
case 0: *out=fi0_p0_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p0_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p0_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p0_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 1: switch(pass) {
case 0: *out=fi0_p1_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p0_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p0_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p0_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 2: switch(pass) {
case 0: *out=fi0_p2_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p0_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p0_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p0_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 3: switch(pass) {
case 0: *out=fi0_p3_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p0_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p0_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p0_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 4: switch(pass) {
case 0: *out=fi0_p4_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p4_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p4_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p4_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 5: switch(pass) {
case 0: *out=fi0_p5_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p4_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p4_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p4_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 6: switch(pass) {
case 0: *out=fi0_p6_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p4_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p4_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p4_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 7: switch(pass) {
case 0: *out=fi0_p7_blob(); return FFX_OK;
case 1: *out=fi1_p0_blob(); return FFX_OK;
case 2: *out=fi2_p4_blob(); return FFX_OK;
case 3: *out=fi3_p0_blob(); return FFX_OK;
case 4: *out=fi4_p0_blob(); return FFX_OK;
case 5: *out=fi5_p4_blob(); return FFX_OK;
case 6: *out=fi6_p0_blob(); return FFX_OK;
case 7: *out=fi7_p0_blob(); return FFX_OK;
case 8: *out=fi8_p0_blob(); return FFX_OK;
case 9: *out=fi9_p0_blob(); return FFX_OK;
case 10: *out=fi10_p0_blob(); return FFX_OK;
case 11: *out=bi0_p0_blob(); return FFX_OK;
case 12: *out=bi1_p0_blob(); return FFX_OK;
case 13: *out=bi2_p0_blob(); return FFX_OK;
case 14: *out=bi3_p4_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
default:return FFX_ERROR_INVALID_ARGUMENT;} }
extern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(FfxOpticalflowPass pass,uint32_t options,FfxShaderBlob* out) { switch((options >> 2) & 1u) {
case 0: switch(pass) {
case 0: *out=of0_p0_blob(); return FFX_OK;
case 1: *out=of1_p0_blob(); return FFX_OK;
case 2: *out=of2_p0_blob(); return FFX_OK;
case 3: *out=of3_p0_blob(); return FFX_OK;
case 4: *out=of4_p0_blob(); return FFX_OK;
case 5: *out=of5_p0_blob(); return FFX_OK;
case 6: *out=of6_p0_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
case 1: switch(pass) {
case 0: *out=of0_p1_blob(); return FFX_OK;
case 1: *out=of1_p1_blob(); return FFX_OK;
case 2: *out=of2_p1_blob(); return FFX_OK;
case 3: *out=of3_p1_blob(); return FFX_OK;
case 4: *out=of4_p1_blob(); return FFX_OK;
case 5: *out=of5_p1_blob(); return FFX_OK;
case 6: *out=of6_p1_blob(); return FFX_OK;
default:return FFX_ERROR_INVALID_ENUM;}
default:return FFX_ERROR_INVALID_ARGUMENT;} }
