// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "DXBCUtils.h"
#include "BlobContainer.h"
#include "DxbcSignature.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <ctype.h>

namespace microsoft {

//---------------------------------------------------------------------------------------------------------------------------------
// DXBCGetSizeAssumingValidPointer()
UINT DXBCGetSizeAssumingValidPointer(const void* pDXBC)
{
    if( !pDXBC ) return 0;

    uint32_t size = 0;
    dxmt_shader_container_declared_size(pDXBC, sizeof(DXBCHeader), &size);
    return size;
}

//---------------------------------------------------------------------------------------------------------------------------------
// DXBCGetInputSignature()
HRESULT DXBCGetInputSignature( const void* pBlobContainer, CSignatureParser* pParserToUse, bool bForceStringReference )
{
    CDXBCParser DXBCParser;
    HRESULT hr = S_OK;
    if( FAILED(hr = DXBCParser.ReadDXBCAssumingValidSize(pBlobContainer) ) )
    {
        return hr;
    }
    UINT BlobIndex = DXBCParser.FindNextMatchingBlob(DXBC_InputSignature11_1, 0);
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature11_1(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    BlobIndex = DXBCParser.FindNextMatchingBlob(DXBC_InputSignature, 0);
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature4(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    return E_FAIL;
}

//---------------------------------------------------------------------------------------------------------------------------------
// DXBCGetOutputSignature()
HRESULT DXBCGetOutputSignature( const void* pBlobContainer, CSignatureParser5* pParserToUse, bool bForceStringReference )
{
    CDXBCParser DXBCParser;
    HRESULT hr = S_OK;
    if( FAILED(hr = DXBCParser.ReadDXBCAssumingValidSize(pBlobContainer) ) )
    {
        return hr;
    }
    UINT BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_OutputSignature11_1, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature11_1(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_OutputSignature, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature4(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_OutputSignature5, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature5(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    return E_FAIL;
}

HRESULT DXBCGetOutputSignature( const void* pBlobContainer, CSignatureParser* pParserToUse, bool bForceStringReference)
{
    CDXBCParser DXBCParser;
    HRESULT hr = S_OK;
    if( FAILED(hr = DXBCParser.ReadDXBCAssumingValidSize(pBlobContainer) ) )
    {
        return hr;
    }
    UINT BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_OutputSignature11_1, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature11_1(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_OutputSignature, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND  )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature4(pUnParsedSignature, BlobSize, bForceStringReference);
    }
    return E_FAIL;
}

HRESULT DXBCGetPatchConstantSignature( const void* pBlobContainer, CSignatureParser* pParserToUse, bool bForceStringReference)
{
    CDXBCParser DXBCParser;
    HRESULT hr = S_OK;
    if( FAILED(hr = DXBCParser.ReadDXBCAssumingValidSize(pBlobContainer) ) )
    {
        return hr;
    }

    UINT BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_PatchConstantSignature11_1, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature11_1(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    BlobIndex = DXBCParser.FindNextMatchingBlob( DXBC_PatchConstantSignature, 0 );
    if( BlobIndex != DXBC_BLOB_NOT_FOUND )
    {
        const void* pUnParsedSignature = DXBCParser.GetBlob(BlobIndex);
        UINT BlobSize = DXBCParser.GetBlobSize(BlobIndex);
        if( !BlobSize )
        {
            return E_FAIL;
        }
        assert(pUnParsedSignature);
        return pParserToUse->ReadSignature4(pUnParsedSignature, BlobSize, bForceStringReference);
    }

    return E_FAIL;
}

//=================================================================================================================================
// CSignatureParser

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::Init()
void CSignatureParser::Init()
{
    m_pSignatureParameters = NULL;
    m_pSemanticNameCharSums = NULL;
    m_cParameters = 0;
    m_OwnParameters = TRUE;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::Cleanup()
void CSignatureParser::Cleanup()
{
    if( m_pSignatureParameters )
    {
        if( m_OwnParameters )
        {
            free(m_pSignatureParameters);
        }
        m_pSignatureParameters = NULL;
    }
    m_pSemanticNameCharSums = NULL;
    m_cParameters = 0;
    m_OwnParameters = TRUE;
}

inline D3D10_SB_NAME ConvertToSB(D3D10_NAME Value, UINT SemanticIndex)
{
    switch (Value)
    {
    case D3D10_NAME_TARGET:
    case D3D10_NAME_DEPTH:
    case D3D10_NAME_COVERAGE:
    case D3D10_NAME_UNDEFINED:
        return D3D10_SB_NAME_UNDEFINED;
    case D3D10_NAME_POSITION:
        return D3D10_SB_NAME_POSITION;
    case D3D10_NAME_CLIP_DISTANCE:
        return D3D10_SB_NAME_CLIP_DISTANCE;
    case D3D10_NAME_CULL_DISTANCE:
        return D3D10_SB_NAME_CULL_DISTANCE;
    case D3D10_NAME_RENDER_TARGET_ARRAY_INDEX:
        return D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX;
    case D3D10_NAME_VIEWPORT_ARRAY_INDEX:
        return D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX;
    case D3D10_NAME_VERTEX_ID:
        return D3D10_SB_NAME_VERTEX_ID;
    case D3D10_NAME_PRIMITIVE_ID:
        return D3D10_SB_NAME_PRIMITIVE_ID;
    case D3D10_NAME_INSTANCE_ID:
        return D3D10_SB_NAME_INSTANCE_ID;
    case D3D10_NAME_IS_FRONT_FACE:
        return D3D10_SB_NAME_IS_FRONT_FACE;
    case D3D10_NAME_SAMPLE_INDEX:
        return D3D10_SB_NAME_SAMPLE_INDEX;
    case D3D11_NAME_FINAL_QUAD_EDGE_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 0:
            return D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR;
        case 1:
            return D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR;
        case 2:
            return D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR;
        case 3:
            return D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    case D3D11_NAME_FINAL_QUAD_INSIDE_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 0:
            return D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR;
        case 1:
            return D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    case D3D11_NAME_FINAL_TRI_EDGE_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 0:
            return D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR;
        case 1:
            return D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR;
        case 2:
            return D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    case D3D11_NAME_FINAL_TRI_INSIDE_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 0:
            return D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    case D3D11_NAME_FINAL_LINE_DETAIL_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 1:
            return D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    case D3D11_NAME_FINAL_LINE_DENSITY_TESSFACTOR:
        switch (SemanticIndex)
        {
        case 0:
            return D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR;
        }
        assert("Invalid D3D10_NAME");
        break;
    default:
        assert("Invalid D3D10_NAME");
    }
    // in retail the assert won't get hit and we'll just pass through anything bad.
    return (D3D10_SB_NAME)Value;
}
inline D3D10_SB_RESOURCE_DIMENSION ConvertToSB(D3D11_SRV_DIMENSION Value)
{

    switch (Value)
    {
    case D3D11_SRV_DIMENSION_UNKNOWN:
        return D3D10_SB_RESOURCE_DIMENSION_UNKNOWN;
    case D3D11_SRV_DIMENSION_BUFFER:
    case D3D11_SRV_DIMENSION_BUFFEREX:
        return D3D10_SB_RESOURCE_DIMENSION_BUFFER;
    case D3D11_SRV_DIMENSION_TEXTURE1D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY;
    default:
        assert("Invalid D3D11_RESOURCE_DIMENSION");
    }
    // in retail the assert won't get hit and we'll just pass through anything bad.
    return (D3D10_SB_RESOURCE_DIMENSION)Value;
}

inline D3D10_SB_RESOURCE_DIMENSION ConvertToSB(D3D11_UAV_DIMENSION Value)
{

    switch (Value)
    {
    case D3D11_UAV_DIMENSION_UNKNOWN:
        return D3D10_SB_RESOURCE_DIMENSION_UNKNOWN;
    case D3D11_UAV_DIMENSION_BUFFER:
        return D3D10_SB_RESOURCE_DIMENSION_BUFFER;
    case D3D11_UAV_DIMENSION_TEXTURE1D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
        return D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY;
    default:
        assert("Invalid D3D11_RESOURCE_DIMENSION");
    }
    // in retail the assert won't get hit and we'll just pass through anything bad.
    return (D3D10_SB_RESOURCE_DIMENSION)Value;
}

inline D3D10_SB_REGISTER_COMPONENT_TYPE ConvertToSB(D3D10_REGISTER_COMPONENT_TYPE Value)
{

    switch (Value)
    {
    case D3D10_REGISTER_COMPONENT_UNKNOWN:
        return D3D10_SB_REGISTER_COMPONENT_UNKNOWN;
    case D3D10_REGISTER_COMPONENT_UINT32:
        return D3D10_SB_REGISTER_COMPONENT_UINT32;
    case D3D10_REGISTER_COMPONENT_SINT32:
        return D3D10_SB_REGISTER_COMPONENT_SINT32;
    case D3D10_REGISTER_COMPONENT_FLOAT32:
        return D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    default:
        assert("Invalid D3D10_REGISTER_COMPONENT_TYPE");
    }
    // in retail the assert won't get hit and we'll just pass through anything bad.
    return (D3D10_SB_REGISTER_COMPONENT_TYPE)Value;
}

//---------------------------------------------------------------------------------------------------------------------------------
// ReadSignatureParameters
//
// Rust validates the header, record table, every name and the register/stream ordering before
// anything is allocated. One allocation then holds the parameters, their lowercase name char sums
// and, unless names reference the blob, copies of the names.
static HRESULT ReadSignatureParameters(const void* pSignature, UINT BlobSize, uint32_t Format, bool bGrouped,
                                       bool bForceStringReference, DxmtDxbcSignatureInfo& Info,
                                       D3D11_SIGNATURE_PARAMETER*& pParameters, UINT*& pCharSums)
{
    if( !pSignature ||
        dxmt_dxbc_signature_parse(pSignature, BlobSize, Format, bGrouped, &Info) != DXMT_DXBC_SIGNATURE_SUCCESS )
    {
        return E_FAIL;
    }
    if( !Info.parameter_count )
    {
        return S_OK;
    }
    const uint64_t ParameterBytes = uint64_t(Info.parameter_count) * sizeof(D3D11_SIGNATURE_PARAMETER);
    const uint64_t CharSumBytes = uint64_t(Info.parameter_count) * sizeof(UINT);
    const uint64_t TotalBytes = ParameterBytes + CharSumBytes + (bForceStringReference ? 0 : Info.string_bytes);
    if( TotalBytes > SIZE_MAX )
    {
        return E_OUTOFMEMORY;
    }
    BYTE* pStorage = (BYTE*)malloc(size_t(TotalBytes));
    if( !pStorage )
    {
        return E_OUTOFMEMORY;
    }
    D3D11_SIGNATURE_PARAMETER* pDst = (D3D11_SIGNATURE_PARAMETER*)pStorage;
    UINT* pSums = (UINT*)(pStorage + ParameterBytes);
    char* pNextDstString = (char*)(pStorage + ParameterBytes + CharSumBytes);
    for( UINT i = 0; i < Info.parameter_count; i++ )
    {
        DxmtDxbcSignatureParameter Record;
        if( dxmt_dxbc_signature_parameter(pSignature, BlobSize, &Info, i, &Record) != DXMT_DXBC_SIGNATURE_SUCCESS )
        {
            free(pStorage);
            return E_FAIL;
        }
        char* pSrcString = (char*)pSignature + Record.name_offset;
        const UINT Length = Record.name_size + 1;

        // Ungrouped parsers expose a single signature, so every parameter belongs to stream 0.
        pDst[i].Stream = bGrouped ? Record.stream : 0;
        pDst[i].ComponentType = ConvertToSB((D3D10_REGISTER_COMPONENT_TYPE)Record.component_type);
        pDst[i].Mask = (BYTE)Record.mask;
        pDst[i].Register = Record.register_index;
        pDst[i].SemanticIndex = Record.semantic_index;
        pDst[i].SystemValue = ConvertToSB((D3D10_NAME)Record.system_value, Record.semantic_index);
        pDst[i].NeverWrites_Mask = (BYTE)Record.never_writes_mask; // union with AlwaysReadMask
        // Only 11.1 records carry a precision; Rust reports D3D_MIN_PRECISION_DEFAULT otherwise.
        pDst[i].MinPrecision = (D3D_MIN_PRECISION)Record.min_precision;
        if( bForceStringReference )
        {
            pDst[i].SemanticName = pSrcString;
        }
        else
        {
            // Rust summed every validated name length into string_bytes.
            memcpy(pNextDstString, pSrcString, Length);
            pDst[i].SemanticName = pNextDstString;
            pNextDstString += Length;
        }

        pSums[i] = 0;
        for( UINT j = 0; j < Length; j++ )
        {
            pSums[i] += tolower(pSrcString[j]);
        }
    }
    pParameters = pDst;
    pCharSums = pSums;
    return S_OK;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::ReadSignature11_1()
HRESULT CSignatureParser::ReadSignature11_1(const void* pSignature, UINT BlobSize, bool bForceStringReference )
{
    if( m_cParameters )
    {
        Cleanup();
    }
    DxmtDxbcSignatureInfo Info;
    HRESULT hr = ReadSignatureParameters(pSignature, BlobSize, DXMT_DXBC_SIGNATURE_11_1, false, bForceStringReference,
                                         Info, m_pSignatureParameters, m_pSemanticNameCharSums);
    if( FAILED(hr) )
    {
        return hr;
    }
    m_OwnParameters = TRUE;
    m_cParameters = Info.parameter_count;
    return S_OK;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::ReadSignature4()
HRESULT CSignatureParser::ReadSignature4(const void* pSignature, UINT BlobSize, bool bForceStringReference )
{
    if( m_cParameters )
    {
        Cleanup();
    }
    DxmtDxbcSignatureInfo Info;
    HRESULT hr = ReadSignatureParameters(pSignature, BlobSize, DXMT_DXBC_SIGNATURE_LEGACY, false, bForceStringReference,
                                         Info, m_pSignatureParameters, m_pSemanticNameCharSums);
    if( FAILED(hr) )
    {
        return hr;
    }
    m_OwnParameters = TRUE;
    m_cParameters = Info.parameter_count;
    return S_OK;
}

HRESULT CSignatureParser::ReadSignature11_1( D3D11_SIGNATURE_PARAMETER* pParamList, UINT* pCharSums, UINT NumParameters )
{
    if( m_cParameters )
    {
        Cleanup();
    }
    if( !pParamList ) 
    {
        return S_OK;
    }

    m_pSignatureParameters = pParamList;
    m_pSemanticNameCharSums = pCharSums;
    m_cParameters = NumParameters;
    m_OwnParameters = FALSE;
    return S_OK;
}


HRESULT CSignatureParser::ReadSignature5( D3D11_SIGNATURE_PARAMETER* pParamList, UINT* pCharSums, UINT NumParameters )
{
    if( m_cParameters )
    {
        Cleanup();
    }
    if( !pParamList ) 
    {
        return S_OK;
    }

    m_pSignatureParameters = pParamList;
    m_pSemanticNameCharSums = pCharSums;
    m_cParameters = NumParameters;
    m_OwnParameters = FALSE;
    return S_OK;
}


//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser5::Init()
void CSignatureParser5::Init()
{
    m_NumSigs = 0;
    m_cParameters = 0;
    m_pSignatureParameters = NULL;
    m_RasterizedStream = 0;
    
    for( UINT i=0; i < D3D11_SO_STREAM_COUNT; i++ ) 
    {
        m_Sig[i].Init(); 
    }
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser5::Cleanup()
void CSignatureParser5::Cleanup()
{ 
    for( UINT i = 0; i < D3D11_SO_STREAM_COUNT; i++ ) 
    {
        if( m_Sig[i].m_cParameters )
        {
            m_Sig[i].Cleanup(); 
        }
    }
    if( m_pSignatureParameters )
    {
        free( m_pSignatureParameters );
        m_pSignatureParameters = NULL;
    }
    m_NumSigs = 0;
    m_RasterizedStream = 0;
    m_cParameters = 0;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser5::ReadSignature11_1()
HRESULT CSignatureParser5::ReadSignature11_1(const void* pSignature, UINT BlobSize, bool bForceStringReference )
{
    Cleanup();
    DxmtDxbcSignatureInfo Info;
    HRESULT hr = ReadSignatureParameters(pSignature, BlobSize, DXMT_DXBC_SIGNATURE_11_1, true, bForceStringReference,
                                         Info, m_pSignatureParameters, m_pSemanticNameCharSums);
    if( FAILED(hr) )
    {
        return hr;
    }
    m_cParameters = Info.parameter_count;
    m_NumSigs = Info.stream_count;

    UINT PreviousParams = 0;
    for( UINT i = 0; i < m_NumSigs; i++ )
    {
        m_Sig[i].ReadSignature11_1( &m_pSignatureParameters[PreviousParams], &m_pSemanticNameCharSums[PreviousParams], Info.stream_parameters[i] );
        PreviousParams += Info.stream_parameters[i];
    }

    return S_OK;
}


//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser5::ReadSignature5()
HRESULT CSignatureParser5::ReadSignature5(const void* pSignature, UINT BlobSize, bool bForceStringReference )
{
    Cleanup();
    DxmtDxbcSignatureInfo Info;
    HRESULT hr = ReadSignatureParameters(pSignature, BlobSize, DXMT_DXBC_SIGNATURE_STREAM, true, bForceStringReference,
                                         Info, m_pSignatureParameters, m_pSemanticNameCharSums);
    if( FAILED(hr) )
    {
        return hr;
    }
    m_cParameters = Info.parameter_count;
    m_NumSigs = Info.stream_count;

    UINT PreviousParams = 0;
    for( UINT i = 0; i < m_NumSigs; i++ )
    {
        m_Sig[i].ReadSignature5( &m_pSignatureParameters[PreviousParams], &m_pSemanticNameCharSums[PreviousParams], Info.stream_parameters[i] );
        PreviousParams += Info.stream_parameters[i];
    }

    return S_OK;
}

HRESULT CSignatureParser5::ReadSignature4( const void* pSignature, UINT BlobSize, bool bForceStringReference )
{
    Cleanup();
    if( !pSignature ) 
    {
        return E_FAIL;
    }

    if( FAILED( m_Sig[0].ReadSignature4( pSignature, BlobSize, bForceStringReference ) ) )
    {
        return E_FAIL;
    }

    m_pSignatureParameters = m_Sig[0].m_pSignatureParameters;
    m_Sig[0].m_OwnParameters = FALSE; // steal the signature from our child
    m_pSemanticNameCharSums = m_Sig[0].m_pSemanticNameCharSums;
    m_cParameters = m_Sig[0].m_cParameters;
    m_RasterizedStream = 0;
    m_NumSigs = 1;

    return S_OK;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::GetParameters()
UINT CSignatureParser::GetParameters( D3D11_SIGNATURE_PARAMETER const** ppParameters ) const
{
    if( ppParameters )
    {
        *ppParameters = m_pSignatureParameters;
    }
    return m_cParameters;
}

//---------------------------------------------------------------------------------------------------------------------------------
// LowerCaseCharSum
static UINT LowerCaseCharSum(LPCSTR pStr)
{
    if (!pStr)
        return 0;

    UINT sum = 0;
    while (*pStr != '\0')
    {
        sum += tolower(*pStr);
        pStr++;
    }
    return sum;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::FindParameter()
HRESULT CSignatureParser::FindParameter( LPCSTR SemanticName, UINT  SemanticIndex, 
                                         D3D11_SIGNATURE_PARAMETER** ppFoundParameter ) const
{
    UINT InputNameCharSum = LowerCaseCharSum(SemanticName);
    for(UINT i = 0; i < m_cParameters; i++ )
    {
        if( (SemanticIndex == m_pSignatureParameters[i].SemanticIndex) &&
            (InputNameCharSum == m_pSemanticNameCharSums[i]) &&
            (strcasecmp(SemanticName,m_pSignatureParameters[i].SemanticName) == 0) )
        {
            if(ppFoundParameter) *ppFoundParameter = &m_pSignatureParameters[i];
            return S_OK;
        }
    }
    if(ppFoundParameter) *ppFoundParameter = NULL;
    return E_FAIL;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::FindParameterRegister()
HRESULT CSignatureParser::FindParameterRegister( LPCSTR SemanticName, UINT  SemanticIndex, 
                                                 UINT* pFoundParameterRegister )
{
    UINT InputNameCharSum = LowerCaseCharSum(SemanticName);
    for(UINT i = 0; i < m_cParameters; i++ )
    {
        if( (SemanticIndex == m_pSignatureParameters[i].SemanticIndex) &&
            (InputNameCharSum == m_pSemanticNameCharSums[i]) &&
            (strcasecmp(SemanticName,m_pSignatureParameters[i].SemanticName) == 0) )
        {
            if(pFoundParameterRegister) *pFoundParameterRegister = m_pSignatureParameters[i].Register;
            return S_OK;
        }
    }
    return E_FAIL;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::GetSemanticNameCharSum()
UINT CSignatureParser::GetSemanticNameCharSum( UINT parameter )
{
    if( parameter >= m_cParameters )
    {
        return 0;
    }
    return m_pSemanticNameCharSums[parameter];
}


//---------------------------------------------------------------------------------------------------------------------------------
// CSignatureParser::CanOutputTo()
bool CSignatureParser::CanOutputTo( CSignatureParser* pTargetSignature )
{
    if( !pTargetSignature )
    {
        return false;
    }
    const D3D11_SIGNATURE_PARAMETER* pDstParam;
    UINT cTargetParameters = pTargetSignature->GetParameters(&pDstParam);
    if( cTargetParameters > m_cParameters )
    {
        return false;
    }
    assert(cTargetParameters <= 64); // if cTargetParameters is allowed to be much larger, rethink this n^2 algorithm.
    for( UINT i = 0; i < cTargetParameters; i++ )
    {
        bool bFoundMatch = false;
        for( UINT j = 0; j < m_cParameters; j++ )
        {
            UINT srcIndex = (i + j) % m_cParameters; // start at the same location in the src as the dest, but loop through all.
            D3D11_SIGNATURE_PARAMETER* pSrcParam = &(m_pSignatureParameters[srcIndex]);
            if( ( m_pSemanticNameCharSums[srcIndex] == pTargetSignature->m_pSemanticNameCharSums[i] ) &&
                ( strcasecmp(pSrcParam->SemanticName, pDstParam->SemanticName) == 0 ) &&
                ( pSrcParam->SemanticIndex == pDstParam->SemanticIndex ) &&
                ( pSrcParam->Register == pDstParam->Register ) &&
                ( pSrcParam->SystemValue == pDstParam->SystemValue ) &&
                ( pSrcParam->ComponentType == pDstParam->ComponentType ) &&
                ( ( pSrcParam->Mask & pDstParam->Mask ) == pDstParam->Mask ) &&
                // Check shader dependent read/write of input/output...
                // If the output shader never writes a value and the input always reads it, that's a problem:
                !((pSrcParam->NeverWrites_Mask & pSrcParam->Mask) & (pDstParam->AlwaysReads_Mask & pDstParam->Mask))
              )
            {
                bFoundMatch = true;
                break;
            }
        }
        if( !bFoundMatch )
        {
            return false;
        }
        pDstParam++;
    }

    return true;
}

//---------------------------------------------------------------------------------------------------------------------------------
// Clear out AlwaysReads_Mask / NeverWrites_Mask for all elements in the 
// signature to force the signature not to cause linkage errors when
// passing the signature around after extracting it from a shader.
void CSignatureParser::ClearAlwaysReadsNeverWritesMask()
{
    for( UINT i = 0; i < m_cParameters; i++ )
    {
        m_pSignatureParameters[i].NeverWrites_Mask = 0; // union with AlwaysReads_Mask
    }
}

}
