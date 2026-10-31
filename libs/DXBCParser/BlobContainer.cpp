// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "DXBCUtils.h"
#include "BlobContainer.h"
#include <cstring>

namespace microsoft {

//=================================================================================================================================
// CDXBCParser

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::CDXBCParser
CDXBCParser::CDXBCParser() :
    m_pContainer(NULL), m_ContainerSize(0), m_BlobCount(0), m_Version{}, m_Hash{}
{
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::ReadDXBC()
HRESULT CDXBCParser::ReadDXBC( const void* pContainer, UINT ContainerSizeInBytes )
{
    DxmtShaderContainerInfo info;
    if (dxmt_shader_container_parse(pContainer, ContainerSizeInBytes, DXMT_SHADER_CONTAINER_LEGACY,
                                    &info) != DXMT_SHADER_BYTECODE_SUCCESS)
        return E_FAIL;
    m_pContainer = static_cast<const BYTE*>(pContainer);
    m_ContainerSize = info.size;
    m_BlobCount = info.chunk_count;
    m_Version = {static_cast<UINT16>(info.version_major), static_cast<UINT16>(info.version_minor)};
    std::memcpy(m_Hash.Digest, info.hash, sizeof(m_Hash.Digest));
    return S_OK;
}

bool CDXBCParser::GetChunk(UINT32 BlobIndex, DxmtShaderChunk& Chunk) const
{
    return m_pContainer &&
        dxmt_shader_container_chunk(m_pContainer, m_ContainerSize, DXMT_SHADER_CONTAINER_LEGACY,
                                    BlobIndex, &Chunk) == DXMT_SHADER_BYTECODE_SUCCESS;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::ReadDXBCAssumingValidSize()
HRESULT CDXBCParser::ReadDXBCAssumingValidSize( const void* pContainer )
{
    if (!pContainer)
    {
        return E_FAIL;
    }
    return ReadDXBC( (const BYTE*)pContainer, DXBCGetSizeAssumingValidPointer( (const BYTE*)pContainer ) );
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::FindNextMatchingBlob
UINT CDXBCParser::FindNextMatchingBlob( DXBCFourCC SearchFourCC, UINT SearchStartBlobIndex )
{
    for (UINT b = SearchStartBlobIndex; b < m_BlobCount; b++)
    {
        DxmtShaderChunk chunk;
        if (!GetChunk(b, chunk))
            return (UINT)DXBC_BLOB_NOT_FOUND;
        if (chunk.fourcc == static_cast<uint32_t>(SearchFourCC))
        {
            return b;
        }
    }
    return (UINT)DXBC_BLOB_NOT_FOUND;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetVersion()
const DXBCVersion* CDXBCParser::GetVersion()
{
    return m_pContainer ? &m_Version : NULL;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetHash()
const DXBCHash* CDXBCParser::GetHash()
{
    return m_pContainer ? &m_Hash : NULL;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetBlobCount()
UINT CDXBCParser::GetBlobCount()
{
    return m_BlobCount;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetBlob()
const void* CDXBCParser::GetBlob( UINT BlobIndex )
{
    DxmtShaderChunk chunk;
    if (!GetChunk(BlobIndex, chunk))
    {
        return NULL;
    }
    return m_pContainer + chunk.offset;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetBlobSize()
UINT CDXBCParser::GetBlobSize( UINT BlobIndex )
{
    DxmtShaderChunk chunk;
    if (!GetChunk(BlobIndex, chunk))
    {
        return 0;
    }
    return chunk.size;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::GetBlobFourCC()
UINT CDXBCParser::GetBlobFourCC( UINT BlobIndex )
{
    DxmtShaderChunk chunk;
    if (!GetChunk(BlobIndex, chunk))
    {
        return 0;
    }
    return chunk.fourcc;
}

//---------------------------------------------------------------------------------------------------------------------------------
// CDXBCParser::RelocateBytecode()
HRESULT CDXBCParser::RelocateBytecode( UINT ByteOffset )
{
    if (!m_pContainer)
    {
        // bad -- has not been initialized yet
        return E_FAIL;
    }
    uintptr_t address = reinterpret_cast<uintptr_t>(m_pContainer);
    if (ByteOffset > UINTPTR_MAX - address)
        return E_FAIL;
    m_pContainer = reinterpret_cast<const BYTE*>(address + ByteOffset);
    return S_OK;
}

}