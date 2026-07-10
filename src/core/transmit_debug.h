#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cs2fow
{

enum class transmit_member_kind : uint8_t
{
	direct,
	owner_link,
	effect_link,
	owner_effect_link
};

inline transmit_member_kind transmit_member_from_links(bool owner, bool effect)
{
	if (owner && effect) return transmit_member_kind::owner_effect_link;
	if (owner) return transmit_member_kind::owner_link;
	return effect ? transmit_member_kind::effect_link : transmit_member_kind::direct;
}

inline constexpr uint8_t k_transmit_reason_current = 1u << 0u;
inline constexpr uint8_t k_transmit_reason_quarantine = 1u << 1u;

struct transmit_debug_event
{
	int edict {-1};
	uint32_t handle {};
	uint32_t source {};
	uint32_t owner {};
	uint32_t effect {};
	int recipient_slot {-1};
	transmit_member_kind member {transmit_member_kind::direct};
	uint8_t reason {};
	std::chrono::steady_clock::time_point when;
};

template <size_t name_size>
struct transmit_debug_record
{
	int edict {-1};
	uint32_t handle {};
	uint32_t source {};
	uint32_t owner {};
	uint32_t effect {};
	transmit_member_kind member {transmit_member_kind::direct};
	uint8_t reasons {};
	uint64_t recipients {};
	uint64_t clears {};
	std::chrono::steady_clock::time_point first_seen;
	std::chrono::steady_clock::time_point last_seen;
	std::array<char, name_size> name {};
	bool valid {};
};

template <size_t capacity, size_t name_size>
class transmit_debug_log
{
public:
	using record_type = transmit_debug_record<name_size>;

	void record(const transmit_debug_event &event, const char *name)
	{
		for (record_type &record : records_)
		{
			if (record.valid && same_key(record, event))
			{
				update(record, event);
				return;
			}
		}
		record_type &record = records_[next_++ % records_.size()];
		record = {};
		record.valid = true;
		record.edict = event.edict;
		record.handle = event.handle;
		record.source = event.source;
		record.owner = event.owner;
		record.effect = event.effect;
		record.member = event.member;
		record.first_seen = event.when;
		std::snprintf(record.name.data(), record.name.size(), "%s", name == nullptr ? "<unknown>" : name);
		update(record, event);
	}

	void clear()
	{
		records_ = {};
		next_ = 0;
	}

	const std::array<record_type, capacity> &records() const
	{
		return records_;
	}

private:
	static bool same_key(const record_type &record, const transmit_debug_event &event)
	{
		return record.handle == event.handle && record.source == event.source && record.owner == event.owner
			&& record.effect == event.effect && record.member == event.member;
	}

	static void update(record_type &record, const transmit_debug_event &event)
	{
		record.edict = event.edict;
		record.reasons |= event.reason;
		if (event.recipient_slot >= 0 && event.recipient_slot < 64)
		{
			record.recipients |= uint64_t {1} << event.recipient_slot;
		}
		++record.clears;
		record.last_seen = event.when;
	}

	std::array<record_type, capacity> records_ {};
	size_t next_ {};
};

template <typename mask_type>
inline bool clear_transmit_bit(mask_type &mask, int index, bool debug)
{
	const bool record = debug && mask.IsBitSet(index);
	mask.Clear(index);
	return record;
}

} // namespace cs2fow
