#pragma once

// Versioned, read-only visibility snapshot ABI for other Metamod plugins.
// Query CS2FOW_VISIBILITY_INTERFACE through ISmmAPI::MetaFactory, keep the
// returned provider pointer only while its PluginId is loaded and unpaused,
// and call CopyLatest on the game thread. The caller owns every output byte.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#define CS2FOW_VISIBILITY_INTERFACE "CS2FOWVisibility001"

namespace cs2fow
{

inline constexpr std::uint32_t k_visibility_abi_version = 1;
inline constexpr std::uint32_t k_visibility_semantics_version = 3;
inline constexpr std::uint32_t k_visibility_max_players_v1 = 64;

enum class VisibilityStatus : std::uint32_t
{
	ok = 0,
	unavailable = 1,
	stale = 2,
	bad_size = 3
};

enum class ProviderStateV1 : std::uint32_t
{
	unavailable = 0,
	disabled = 1,
	baking = 2,
	ready = 3
};

enum VisibilitySnapshotFlags : std::uint32_t
{
	SNAPSHOT_FILTER_TEAMMATES = 1u << 0u,
	SNAPSHOT_SMOKE_ENABLED = 1u << 1u,
	SNAPSHOT_SMOKE_AVAILABLE = 1u << 2u
};

enum VisibilityPairFlags : std::uint8_t
{
	PAIR_EVALUATED = 1u << 0u,
	PAIR_SAMPLE_CLEAR = 1u << 1u,
	PAIR_PLAUSIBLE_VISIBLE = 1u << 2u
};

struct VisibilityIdentityV1
{
	std::int32_t pawn_entity;
	std::uint32_t pawn_handle;
	std::uint32_t pawn_generation;
	std::uint8_t team;
	std::uint8_t reserved[3];
};

struct VisibilityMapIdentityV1
{
	char map_name[64];
	std::uint64_t source_size;
	std::uint32_t source_crc32;
	std::uint32_t payload_crc32;
	std::uint32_t bvh8_version;
	std::uint32_t bake_recipe_version;
	std::uint32_t bvh8_flags;
	std::uint32_t reserved;
};

struct VisibilitySettingsV1
{
	std::uint32_t update_interval_ms;
	std::uint32_t visibility_hold_ms;
	std::uint32_t base_lookahead_ms;
	std::uint32_t max_lookahead_ms;
	float rtt_lookahead_scale;
	float max_prediction_units;
	float shoulder_base_units;
	float shoulder_rtt_scale;
	float max_shoulder_units;
	float he_clear_radius_units;
	float he_clear_seconds;
	std::uint32_t reserved;
};

struct SnapshotV1
{
	std::uint32_t struct_size;
	std::uint32_t abi_version;
	std::uint32_t semantics_version;
	std::uint32_t snapshot_flags;
	std::uint64_t sequence;
	std::uint64_t captured_monotonic_ns;
	std::uint64_t completed_monotonic_ns;
	std::int32_t server_tick;
	float server_time;
	VisibilityMapIdentityV1 map;
	VisibilitySettingsV1 settings;
	std::uint32_t smoke_count;
	std::uint32_t he_clearance_count;
	std::uint64_t valid_slots;
	VisibilityIdentityV1 identities[k_visibility_max_players_v1];
	std::uint16_t recipient_lookahead_ms[k_visibility_max_players_v1];
	// pair_flags[recipient][target]; zero means unknown/not evaluated.
	// SAMPLE_CLEAR is the raw current sample; PLAUSIBLE_VISIBLE includes reveal hold.
	std::uint8_t pair_flags[k_visibility_max_players_v1][k_visibility_max_players_v1];
	ProviderStateV1 provider_state;
	std::uint8_t reserved[60];
};

class ICS2FOWVisibility
{
public:
	// Requires output_size == sizeof(SnapshotV1). Every status except bad_size
	// writes the ABI header and provider state. Pair data is present only for ok;
	// unavailable and stale leave all other fields zero. bad_size does not touch it.
	virtual VisibilityStatus CopyLatest(SnapshotV1 *output, std::uint32_t output_size) const noexcept = 0;
};

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(static_cast<std::uint32_t>(VisibilityStatus::ok) == 0);
static_assert(static_cast<std::uint32_t>(VisibilityStatus::bad_size) == 3);
static_assert(sizeof(ProviderStateV1) == 4);
static_assert(static_cast<std::uint32_t>(ProviderStateV1::unavailable) == 0);
static_assert(static_cast<std::uint32_t>(ProviderStateV1::ready) == 3);
static_assert(sizeof(VisibilityIdentityV1) == 16);
static_assert(sizeof(VisibilityMapIdentityV1) == 96);
static_assert(sizeof(VisibilitySettingsV1) == 48);
static_assert(offsetof(SnapshotV1, map) == 48);
static_assert(offsetof(SnapshotV1, settings) == 144);
static_assert(offsetof(SnapshotV1, valid_slots) == 200);
static_assert(offsetof(SnapshotV1, pair_flags) == 1360);
static_assert(offsetof(SnapshotV1, provider_state) == 5456);
static_assert(offsetof(SnapshotV1, reserved) == 5460);
static_assert(sizeof(SnapshotV1) == 5520);
static_assert(alignof(SnapshotV1) == 8);
static_assert(std::is_trivial_v<VisibilityIdentityV1> && std::is_standard_layout_v<VisibilityIdentityV1>);
static_assert(std::is_trivial_v<VisibilityMapIdentityV1> && std::is_standard_layout_v<VisibilityMapIdentityV1>);
static_assert(std::is_trivial_v<VisibilitySettingsV1> && std::is_standard_layout_v<VisibilitySettingsV1>);
static_assert(std::is_trivial_v<SnapshotV1> && std::is_standard_layout_v<SnapshotV1>);

} // namespace cs2fow
