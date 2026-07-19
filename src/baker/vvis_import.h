#pragma once

// Parsers for the compiled Source 2 resource shell and
// the KV3 text VRF's --decompile emits for the DATA block.

#include "pvs.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cs2fow
{

struct resource_block
{
	char fourcc[4];
	uint32_t offset;
	uint32_t size;
};


struct vvis_constants
{
	uint32_t m_nBaseClusterCount;
	uint32_t m_nPVSBytesPerCluster;
	float m_vMinBounds[3];
	float m_vMaxBounds[3];
	float m_flGridSize;
	uint32_t m_nSkyVisibilityCluster;
	uint32_t m_nSunVisibilityCluster;
	uint32_t m_NodeBlock_m_nOffset;
	uint32_t m_NodeBlock_m_nElementCount;
	uint32_t m_RegionBlock_m_nOffset;
	uint32_t m_RegionBlock_m_nElementCount;
	
	uint32_t m_EnclosedClusterListBlock_m_nOffset;
	uint32_t m_EnclosedClusterListBlock_m_nElementCount;
	uint32_t m_EnclosedClustersBlock_m_nOffset;
	uint32_t m_EnclosedClustersBlock_m_nElementCount;

	uint32_t m_MasksBlock_m_nOffset;
	uint32_t m_MasksBlock_m_nElementCount;
	uint32_t m_nVisBlocks_m_nOffset;
	uint32_t m_nVisBlocks_m_nElementCount;
};

bool parse_vxvs_block(std::span<const std::byte> bytes, resource_block &out, std::string &error);
bool parse_vvis_text(std::string_view text, vvis_constants &out, std::string &error);
bool import_vvis(std::span<const std::byte> vvis_c_bytes, std::string_view kv3_text, pvs_data &out, std::string &error);

} // namespace cs2fow
