#include "plugin.h"

// Coordinates plugin load/unload, public commands, map changes, frames, bake
// validation, and worker submission on the game thread. It activates filtering
// only after CPU, gamedata, schema, map source, and bake checks all succeed.

#include "vpk.h"

#include <ISmmPlugin.h>
#include <eiface.h>
#include <entity2/entitysystem.h>
#include <filesystem.h>
#include <inetchannelinfo.h>
#include <iserver.h>
#include <schemasystem/schemasystem.h>
#include <tier1/convar.h>
#include <tier1/utlstring.h>
#include <tier1/utlvector.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace cs2fow
{

plugin g_plugin;
constexpr const char *k_game_event_manager_interface = "GAMEEVENTSMANAGER002";

GameEventKeySymbol_t game_event_key(const char *name)
{
	return {CUtlStringToken(MurmurHash2LowerCase(name, STRINGTOKEN_MURMURHASH_SEED)), name};
}

void *module_base(const void *address)
{
#if defined(_WIN32)
	HMODULE module {};
	return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(address), &module) ? module : nullptr;
#else
	Dl_info info {};
	return dladdr(address, &info) != 0 ? info.dli_fbase : nullptr;
#endif
}

std::filesystem::path module_path(const void *address)
{
#if defined(_WIN32)
	HMODULE module {};
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(address), &module))
	{
		return {};
	}
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size())
	{
		return {};
	}
	path.resize(length);
	return path;
#else
	Dl_info info {};
	return dladdr(address, &info) != 0 && info.dli_fname != nullptr ? std::filesystem::path(info.dli_fname) : std::filesystem::path {};
#endif
}

void on_cs2fow_enable_changed(CConVar<bool> *, CSplitScreenSlot, const bool *new_value, const bool *old_value)
{
	if (new_value != nullptr && old_value != nullptr && *new_value != *old_value)
	{
		g_plugin.reset_transmit_state(false);
	}
}

void on_cs2fow_float_changed(CConVar<float> *, CSplitScreenSlot, const float *new_value, const float *old_value)
{
	if (new_value != nullptr && old_value != nullptr && *new_value != *old_value)
	{
		g_plugin.reset_transmit_state(false);
	}
}

CConVar<bool> cs2fow_enable("cs2fow_enable", FCVAR_NONE, "Enable CS2FOW when map data is valid", true, on_cs2fow_enable_changed);
CConVar<bool> cs2fow_smoke_occlusion("cs2fow_smoke_occlusion", FCVAR_NONE, "Use live CS2 smoke for visibility", true, on_cs2fow_enable_changed);
CConVar<float> cs2fow_he_clear_radius_units("cs2fow_he_clear_radius_units", FCVAR_NONE, "HE-cleared smoke channel radius", 100.0f,
	true, 0.0f, true, 320.0f, on_cs2fow_float_changed);
CConVar<float> cs2fow_he_clear_seconds("cs2fow_he_clear_seconds", FCVAR_NONE, "HE-cleared smoke channel duration", 2.5f,
	true, 0.0f, true, 10.0f, on_cs2fow_float_changed);
CConVar<bool> cs2fow_filter_teammates("cs2fow_filter_teammates", FCVAR_NONE, "Apply visibility filtering to teammates", false, on_cs2fow_enable_changed);
CConVar<int> cs2fow_update_interval_ms("cs2fow_update_interval_ms", FCVAR_NONE, "Visibility worker update interval", 1, true, 1, true, 250);
CConVar<int> cs2fow_base_lookahead_ms("cs2fow_base_lookahead_ms", FCVAR_NONE, "Fixed movement lookahead before recipient RTT", 75, true, 0, true, 500);
CConVar<float> cs2fow_rtt_lookahead_scale("cs2fow_rtt_lookahead_scale", FCVAR_NONE, "Recipient RTT multiplier for movement lookahead", 1.5f, true, 0.0f, true, 4.0f);
CConVar<int> cs2fow_max_lookahead_ms("cs2fow_max_lookahead_ms", FCVAR_NONE, "Maximum movement and latency lookahead", 375, true, 0, true, 500);
CConVar<float> cs2fow_max_prediction_units("cs2fow_max_prediction_units", FCVAR_NONE, "Maximum predicted movement per player", 96.0f, true, 0.0f, true, 256.0f);
CConVar<float> cs2fow_shoulder_base_units("cs2fow_shoulder_base_units", FCVAR_NONE, "Minimum sideways shoulder origin distance", 16.0f, true, 0.0f, true, 256.0f);
CConVar<float> cs2fow_shoulder_rtt_scale("cs2fow_shoulder_rtt_scale", FCVAR_NONE, "Sideways shoulder units per millisecond of recipient RTT", 0.48f, true, 0.0f, true, 4.0f);
CConVar<float> cs2fow_max_shoulder_units("cs2fow_max_shoulder_units", FCVAR_NONE, "Maximum sideways shoulder origin distance", 96.0f, true, 0.0f, true, 256.0f);
CConVar<int> cs2fow_visibility_hold_ms("cs2fow_visibility_hold_ms", FCVAR_NONE, "Minimum revealed duration", 16, true, 0, true, 1000);
CConVar<bool> cs2fow_debug("cs2fow_debug", FCVAR_NONE, "Enable CS2FOW diagnostic logging", false);

CON_COMMAND_F(cs2fow_status, "Report CS2FOW state", FCVAR_NONE)
{
	g_plugin.print_status();
}

CON_COMMAND_F(cs2fow_entity, "List, filter, or clear actual CS2FOW transmit clears", FCVAR_NONE)
{
	if (args.ArgC() == 1)
	{
		g_plugin.print_entities(-1);
		return;
	}
	if (args.ArgC() != 2)
	{
		META_CONPRINTF("[CS2FOW] usage: cs2fow_entity [<edict>|clear]\n");
		return;
	}
	const char *text = args.Arg(1);
	if (std::strcmp(text, "clear") == 0)
	{
		g_plugin.clear_entity_records();
		return;
	}
	int edict = -1;
	const auto result = std::from_chars(text, text + std::strlen(text), edict);
	if (result.ec != std::errc {} || *result.ptr != '\0')
	{
		META_CONPRINTF("[CS2FOW] invalid edict: %s\n", text);
		return;
	}
	g_plugin.print_entities(edict);
}

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, false, bool, bool, bool);
SH_DECL_HOOK7_void(ISource2GameEntities, CheckTransmit, SH_NOATTRIB, false, CCheckTransmitInfo **, int, CBitVec<MAX_EDICTS> &, CBitVec<MAX_EDICTS> &, const Entity2Networkable_t **, const uint16 *, int);
SH_DECL_HOOK2(IGameEventManager2, LoadEventsFromFile, SH_NOATTRIB, false, int, const char *, bool);

bool plugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	api_ = ismm;
	GET_V_IFACE_CURRENT(GetEngineFactory, engine_, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, cvar_, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, filesystem_, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, schema_, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, game_resource_, game_resource_service, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, server_, IServerGameDLL, INTERFACEVERSION_SERVERGAMEDLL);
	GET_V_IFACE_ANY(GetServerFactory, game_entities_, ISource2GameEntities, SOURCE2GAMEENTITIES_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pNetworkServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	g_pCVar = cvar_;
	game_frame_hook_id_ = SH_ADD_HOOK(IServerGameDLL, GameFrame, server_, SH_MEMBER(this, &plugin::hook_game_frame), true);
	check_transmit_hook_id_ = SH_ADD_HOOK(ISource2GameEntities, CheckTransmit, game_entities_, SH_MEMBER(this, &plugin::hook_check_transmit), true);
	if (game_frame_hook_id_ == 0 || check_transmit_hook_id_ == 0)
	{
		if (game_frame_hook_id_ != 0) SH_REMOVE_HOOK_ID(game_frame_hook_id_);
		if (check_transmit_hook_id_ != 0) SH_REMOVE_HOOK_ID(check_transmit_hook_id_);
		game_frame_hook_id_ = 0;
		check_transmit_hook_id_ = 0;
		if (error != nullptr && maxlen != 0) ismm->Format(error, maxlen, "Could not install required SourceHook hooks");
		return false;
	}
	game_events_ = static_cast<IGameEventManager2 *>(ismm->VInterfaceMatch(ismm->GetEngineFactory(), k_game_event_manager_interface));
	if (game_events_ == nullptr)
	{
		game_events_ = static_cast<IGameEventManager2 *>(ismm->VInterfaceMatch(ismm->GetServerFactory(), k_game_event_manager_interface));
	}
	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);
	teammates_are_enemies_ = cvar_->FindConVar("mp_teammates_are_enemies");
	engine_->ServerCommand("exec cs2fow.cfg");
	g_SMAPI->AddListener(this, this);

	std::string reason;
	const bool avx = cpu_supports_avx();
	prerequisites_valid_ = avx && read_gamedata(reason) && verify_server_binary(reason) && resolve_schema(reason);
	if (prerequisites_valid_ && game_event_manager_vtable_rva_ != 0)
	{
		void *base = module_base(*reinterpret_cast<void **>(game_entities_));
		if (base != nullptr)
		{
			auto *vtable = reinterpret_cast<IGameEventManager2 *>(static_cast<uint8_t *>(base) + game_event_manager_vtable_rva_);
			game_event_load_hook_id_ = SH_ADD_DVPHOOK(IGameEventManager2, LoadEventsFromFile, vtable,
				SH_MEMBER(this, &plugin::hook_load_events_from_file), true);
		}
	}
	if (!prerequisites_valid_)
	{
		disable(avx ? reason : "AVX and OS AVX state are required");
		META_CONPRINTF("[CS2FOW] disabled: %s\n", disabled_reason_.c_str());
	}
	else if (!smoke_gamedata_available_ || !smoke_schema_available_)
	{
		META_CONPRINTF("[CS2FOW] smoke occlusion unavailable; wall filtering remains active\n");
	}
	else if (!he_event_available_ && game_event_load_hook_id_ == 0)
	{
		META_CONPRINTF("[CS2FOW] HE smoke clearing unavailable; ordinary smoke remains active\n");
	}
	META_CONPRINTF("[CS2FOW] loaded; culling is fail-open until a map bake validates\n");
	return true;
}

bool plugin::Unload(char *error, size_t max_length)
{
	if (game_events_ != nullptr) game_events_->RemoveListener(this);
	game_events_ = nullptr;
	he_event_available_ = false;
	if (game_event_load_hook_id_ != 0) SH_REMOVE_HOOK_ID(game_event_load_hook_id_);
	game_event_load_hook_id_ = 0;
	automatic_baker_.stop();
	worker_.stop();
	if (game_frame_hook_id_ != 0) SH_REMOVE_HOOK_ID(game_frame_hook_id_);
	if (check_transmit_hook_id_ != 0) SH_REMOVE_HOOK_ID(check_transmit_hook_id_);
	game_frame_hook_id_ = 0;
	check_transmit_hook_id_ = 0;
	ConVar_Unregister();
	return true;
}

int plugin::hook_load_events_from_file(const char *, bool)
{
	game_events_ = META_IFACEPTR(IGameEventManager2);
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void plugin::FireGameEvent(IGameEvent *event)
{
	if (event == nullptr || std::strcmp(event->GetName(), "hegrenade_detonate") != 0)
	{
		return;
	}
	const float missing = std::numeric_limits<float>::quiet_NaN();
	const vec3 center {event->GetFloat(game_event_key("x"), missing), event->GetFloat(game_event_key("y"), missing),
		event->GetFloat(game_event_key("z"), missing)};
	INetworkGameServer *network_server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
	CGlobalVars *globals = network_server == nullptr ? nullptr : network_server->GetGlobals();
	const float game_time = globals == nullptr ? missing : globals->curtime;
	std::lock_guard<std::mutex> lock(transmit_state_mutex_);
	he_clearance_history_.record(center, game_time);
}

void plugin::OnLevelInit(char const *map_name, char const *, char const *, char const *, bool, bool)
{
	engine_->ServerCommand("exec cs2fow.cfg");
	if (map_name != nullptr && map_name[0] != '\0')
	{
		change_map(map_name);
	}
}

void plugin::OnLevelShutdown()
{
	automatic_baker_.stop();
	worker_.stop();
	data_ = {};
	source_ = {};
	reset_transmit_state();
	map_.clear();
	if (prerequisites_valid_)
	{
		disabled_reason_ = "no map loaded";
	}
}

bool plugin::read_gamedata(std::string &error)
{
	const std::filesystem::path path = std::filesystem::path(api_->GetBaseDir()) / "addons" / "cs2fow" / "gamedata" / "cs2fow.games.txt";
	std::ifstream stream(path);
	if (!stream)
	{
		error = "missing gamedata: " + path.string();
		return false;
	}
	recipient_slot_offset_ = 0;
	entity_system_offset_ = 0;
	server_binary_size_ = 0;
	server_binary_crc32_ = 0;
	transmit_offsets_ = {};
	smoke_layout_ = {};
	game_event_manager_vtable_rva_ = 0;
	smoke_gamedata_available_ = true;
#if defined(_WIN32)
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_windows";
	constexpr std::string_view k_server_size_key = "server_binary_size_windows";
	constexpr std::string_view k_server_crc_key = "server_binary_crc32_windows";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_windows";
	constexpr std::string_view k_full_update_key = "checktransmit_full_update_offset_windows";
	constexpr std::string_view k_smoke_volume_key = "smoke_volume_offset_windows";
	constexpr std::string_view k_smoke_storage_key = "smoke_storage_offset_windows";
	constexpr std::string_view k_smoke_frame_key = "smoke_frame_offset_windows";
	constexpr std::string_view k_smoke_center_key = "smoke_center_offset_windows";
	constexpr std::string_view k_smoke_start_time_key = "smoke_start_time_offset_windows";
	constexpr std::string_view k_game_event_vtable_key = "game_event_manager_vtable_rva_windows";
#else
	constexpr std::string_view k_recipient_key = "recipient_slot_offset_linux";
	constexpr std::string_view k_server_size_key = "server_binary_size_linux";
	constexpr std::string_view k_server_crc_key = "server_binary_crc32_linux";
	constexpr std::string_view k_entity_system_key = "game_entity_system_offset_linux";
	constexpr std::string_view k_full_update_key = "checktransmit_full_update_offset_linux";
	constexpr std::string_view k_smoke_volume_key = "smoke_volume_offset_linux";
	constexpr std::string_view k_smoke_storage_key = "smoke_storage_offset_linux";
	constexpr std::string_view k_smoke_frame_key = "smoke_frame_offset_linux";
	constexpr std::string_view k_smoke_center_key = "smoke_center_offset_linux";
	constexpr std::string_view k_smoke_start_time_key = "smoke_start_time_offset_linux";
	constexpr std::string_view k_game_event_vtable_key = "game_event_manager_vtable_rva_linux";
#endif
	std::string line;
	while (std::getline(stream, line))
	{
		const size_t equals = line.find('=');
		if (equals == std::string::npos || line.starts_with("//"))
		{
			continue;
		}
		const std::string key = line.substr(0, equals);
		const bool smoke_key = key == k_smoke_volume_key || key == k_smoke_storage_key || key == k_smoke_frame_key
			|| key == k_smoke_center_key || key == k_smoke_start_time_key;
		if (key != k_server_size_key && key != k_server_crc_key && key != k_recipient_key && key != k_entity_system_key && key != k_full_update_key
			&& key != k_game_event_vtable_key && !smoke_key)
		{
			continue;
		}
		std::string_view text(line.data() + equals + 1u, line.size() - equals - 1u);
		uint32_t value {};
		if (!parse_gamedata_uint32(text, value))
		{
			if (smoke_key || key == k_game_event_vtable_key)
			{
				if (smoke_key) smoke_gamedata_available_ = false;
				continue;
			}
			error = "invalid gamedata value for " + key;
			return false;
		}
		if (key == k_server_size_key) server_binary_size_ = value;
		if (key == k_server_crc_key) server_binary_crc32_ = value;
		if (key == k_recipient_key) recipient_slot_offset_ = value;
		if (key == k_entity_system_key) entity_system_offset_ = value;
		if (key == k_full_update_key) transmit_offsets_.full_update_offset = value;
		if (key == k_smoke_volume_key) smoke_layout_.volume = value;
		if (key == k_smoke_storage_key) smoke_layout_.storage = value;
		if (key == k_smoke_frame_key) smoke_layout_.frame = value;
		if (key == k_smoke_center_key) smoke_layout_.center = value;
		if (key == k_smoke_start_time_key) smoke_layout_.start_time = value;
		if (key == k_game_event_vtable_key) game_event_manager_vtable_rva_ = value;
	}
	if (server_binary_size_ == 0 || server_binary_crc32_ == 0 || recipient_slot_offset_ == 0
		|| entity_system_offset_ == 0 || transmit_offsets_.full_update_offset == 0)
	{
		error = "gamedata does not contain this platform's required values";
		return false;
	}
	if (recipient_slot_offset_ < sizeof(CCheckTransmitInfo) || recipient_slot_offset_ > k_max_gamedata_offset || recipient_slot_offset_ % alignof(int) != 0
		|| entity_system_offset_ < sizeof(void *) || entity_system_offset_ > k_max_gamedata_offset || entity_system_offset_ % alignof(void *) != 0
		|| !valid_gamedata_offset(transmit_offsets_.full_update_offset, static_cast<uint32_t>(alignof(bool)), k_max_gamedata_offset))
	{
		error = "gamedata contains invalid offsets for this platform";
		return false;
	}
	smoke_gamedata_available_ = smoke_gamedata_available_
		&& valid_gamedata_offset(smoke_layout_.volume, static_cast<uint32_t>(alignof(void *)), k_max_gamedata_offset)
		&& valid_gamedata_offset(smoke_layout_.storage, static_cast<uint32_t>(alignof(void *)), k_max_gamedata_offset)
		&& valid_gamedata_offset(smoke_layout_.frame, static_cast<uint32_t>(alignof(int32_t)), k_max_gamedata_offset)
		&& valid_gamedata_offset(smoke_layout_.center, static_cast<uint32_t>(alignof(float)), k_max_gamedata_offset)
		&& valid_gamedata_offset(smoke_layout_.start_time, static_cast<uint32_t>(alignof(float)), k_max_gamedata_offset);
	if (!valid_gamedata_offset(game_event_manager_vtable_rva_, static_cast<uint32_t>(alignof(void *)), k_max_module_rva))
	{
		game_event_manager_vtable_rva_ = 0;
	}
	return true;
}

bool plugin::verify_server_binary(std::string &error)
{
	std::ostringstream expected;
	expected << "expected size=" << server_binary_size_ << " crc=0x" << std::hex << server_binary_crc32_;
	if (game_entities_ == nullptr)
	{
		error = "server game-entities interface is unavailable; " + expected.str() + ", actual unavailable";
		return false;
	}
	const std::filesystem::path path = module_path(*reinterpret_cast<void **>(game_entities_));
	if (path.empty())
	{
		error = "could not resolve the loaded server binary path; " + expected.str() + ", actual unavailable";
		return false;
	}
	uint64_t actual_size = 0;
	uint32_t actual_crc = 0;
	std::string fingerprint_error;
	if (!file_crc32(path, actual_size, actual_crc, fingerprint_error))
	{
		error = "could not verify loaded server binary: " + fingerprint_error + "; " + expected.str() + ", actual unavailable";
		return false;
	}
	if (actual_size != server_binary_size_ || actual_crc != server_binary_crc32_)
	{
		std::ostringstream message;
		message << "server binary does not match verified gamedata: expected size=" << server_binary_size_
			<< " crc=0x" << std::hex << server_binary_crc32_ << std::dec << ", actual size=" << actual_size
			<< " crc=0x" << std::hex << actual_crc;
		error = message.str();
		return false;
	}
	return true;
}

void plugin::disable(std::string reason)
{
	worker_.stop();
	data_ = {};
	reset_transmit_state();
	disabled_reason_ = std::move(reason);
}

bool plugin::resolve_map_source(const std::string &map, map_source &source, std::string &error) const
{
	if (!valid_map_name(map))
	{
		error = "map name is not a safe relative path";
		return false;
	}
	const std::string virtual_path = "maps/" + map + ".vpk";
	CUtlVector<CUtlString> paths;
	filesystem_->FindFileAbsoluteList(paths, virtual_path.c_str(), "GAME");
	std::vector<std::filesystem::path> candidates;
	auto add_candidates = [&](const std::string &path)
	{
		for (const std::filesystem::path &candidate : vpk_path_candidates(path))
		{
			if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
			{
				candidates.push_back(candidate);
			}
		}
	};
	for (int i = 0; i < paths.Count(); ++i)
	{
		add_candidates(paths[i].Get());
	}
	add_candidates((std::filesystem::path(api_->GetBaseDir()) / "maps" / (map + ".vpk")).string());

	std::vector<std::string> tried;
	for (const std::filesystem::path &candidate : candidates)
	{
		tried.push_back(candidate.filename().string());
		std::string source_error;
		if (find_map_source(candidate, map, source, source_error))
		{
			return true;
		}
	}

	std::ostringstream message;
	message << "could not resolve mounted map VPK: " << virtual_path;
	if (!tried.empty())
	{
		message << " tried=";
		for (size_t i = 0; i < tried.size(); ++i)
		{
			if (i != 0)
			{
				message << ",";
			}
			message << tried[i];
		}
	}
	error = message.str();
	return false;
}

bool plugin::load_map_bake(const std::filesystem::path &path, const std::string &map, const map_source &source,
	bvh8_data &data, std::string &error) const
{
	if (!load_bvh8(path, data, error))
	{
		return false;
	}
	if (map != data.header.map_name)
	{
		error = "bake map name does not match current map";
		data = {};
		return false;
	}
	if (data.header.flags != source.flags || data.header.source_crc32 != source.metadata.crc32 || data.header.source_size != source.metadata.size)
	{
		error = "bake source CRC or size does not match current VPK";
		data = {};
		return false;
	}
	return true;
}

void plugin::activate(bvh8_data data)
{
	worker_.stop();
	reset_transmit_state();
	data_ = std::move(data);
	disabled_reason_.clear();
	worker_.start(&data_);
	META_CONPRINTF("[CS2FOW] active for %s: crc=0x%08x, triangles=%u, nodes=%u, packets=%u\n", map_.c_str(), data_.header.source_crc32,
		data_.header.triangle_count, data_.header.node_count, data_.header.packet_count);
}

void plugin::start_automatic_bake(const std::string &map, const map_source &source, const std::filesystem::path &output, const std::string &reason)
{
	const std::filesystem::path base = api_->GetBaseDir();
#if defined(_WIN32)
	const std::filesystem::path baker = base / "tools" / "cs2fow_baker.exe";
	const std::filesystem::path vrf = base / "tools" / "vrf" / "win64" / "Source2Viewer-CLI.exe";
#else
	const std::filesystem::path baker = base / "tools" / "cs2fow_baker";
	const std::filesystem::path vrf = base / "tools" / "vrf" / "linux64" / "Source2Viewer-CLI";
#endif
	if (!std::filesystem::is_regular_file(baker) || !std::filesystem::is_regular_file(vrf))
	{
		disable("automatic baker or VRF is missing");
		return;
	}
#if !defined(_WIN32)
	const auto check_executable = [&](const std::filesystem::path &path)
	{
		std::error_code ec;
		const auto status = std::filesystem::status(path, ec);
		return !ec && (status.permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) != std::filesystem::perms::none;
	};
	if (!check_executable(baker))
	{
		disable("baker missing execute permission (chmod +x " + baker.string() + ")");
		return;
	}
	if (!check_executable(vrf))
	{
		disable("VRF missing execute permission (chmod +x " + vrf.string() + ")");
		return;
	}
#endif
	disabled_reason_ = "automatic bake in progress";
	META_CONPRINTF("[CS2FOW] %s for %s; starting automatic bake\n", reason.c_str(), map.c_str());
	automatic_baker_.start({map, source, base.parent_path().parent_path(), output, baker, vrf});
}

void plugin::poll_automatic_bake()
{
	bake_completion completion;
	if (!automatic_baker_.poll(completion))
	{
		return;
	}
	if (completion.cancelled || completion.request.map != map_)
	{
		return;
	}
	if (!completion.success)
	{
		disable("automatic bake failed: " + completion.error);
		META_CONPRINTF("[CS2FOW] automatic bake failed for %s: %s\n", map_.c_str(), completion.error.c_str());
		return;
	}
	map_source current;
	std::string error;
	if (!resolve_map_source(map_, current, error) || !same_map_source(current, completion.request.source))
	{
		disable("map source changed during automatic bake");
		return;
	}
	source_ = std::move(current);
	activate(std::move(completion.data));
}

void plugin::change_map(const std::string &map)
{
	automatic_baker_.stop();
	worker_.stop();
	data_ = {};
	source_ = {};
	reset_transmit_state();
	map_ = map;
	if (!prerequisites_valid_)
	{
		return;
	}
	disabled_reason_ = "validating map";
	const std::filesystem::path base = api_->GetBaseDir();
	const std::filesystem::path bake = base / "addons" / "cs2fow" / "data" / "maps" / (map + ".bvh8");
	std::string error;
	if (!resolve_map_source(map, source_, error))
	{
		disable(error);
		return;
	}
	bvh8_data data;
	if (!load_map_bake(bake, map, source_, data, error))
	{
		start_automatic_bake(map, source_, bake, error);
		return;
	}
	activate(std::move(data));
}

void plugin::hook_game_frame(bool simulating, bool first_tick, bool last_tick)
{
	if (!he_event_available_ && game_events_ != nullptr)
	{
		he_event_available_ = game_events_->AddListener(this, "hegrenade_detonate", true);
	}
	INetworkGameServer *network_server = g_pNetworkServerService == nullptr ? nullptr : g_pNetworkServerService->GetIGameServer();
	if (network_server == nullptr)
	{
		return;
	}
	const char *current_map = network_server->GetMapName();
	if (current_map != nullptr && map_ != current_map)
	{
		change_map(current_map);
	}
	poll_automatic_bake();
	if (!simulating || !cs2fow_enable.Get() || !disabled_reason_.empty())
	{
		return;
	}
	CGameEntitySystem *system = entity_system();
	if (system == nullptr)
	{
		disable("game entity system is unavailable");
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - last_snapshot_ < std::chrono::milliseconds(cs2fow_update_interval_ms.Get()))
	{
		return;
	}
	visibility_snapshot value;
	CGlobalVars *globals = network_server->GetGlobals();
	const float game_time = globals == nullptr ? std::numeric_limits<float>::quiet_NaN() : globals->curtime;
	const auto capture_started = std::chrono::steady_clock::now();
	if (!capture(value, game_time))
	{
		disable("game entity system is unavailable");
		return;
	}
	const double capture_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - capture_started).count();
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		capture_timing_.record(capture_ms);
	}
	last_snapshot_ = now;
	worker_.submit(std::move(value), static_cast<uint32_t>(cs2fow_visibility_hold_ms.Get()), {
		static_cast<uint32_t>(cs2fow_base_lookahead_ms.Get()),
		cs2fow_rtt_lookahead_scale.Get(),
		static_cast<uint32_t>(cs2fow_max_lookahead_ms.Get()),
		cs2fow_max_prediction_units.Get(),
		cs2fow_shoulder_base_units.Get(),
		cs2fow_shoulder_rtt_scale.Get(),
		cs2fow_max_shoulder_units.Get()
	});
}

void plugin::print_status() const
{
	const worker_stats stats = worker_.stats();
	const std::shared_ptr<const visibility_result> result = worker_.result();
	runtime_timing_stats capture_timing;
	runtime_timing_stats transmit_timing;
	{
		std::lock_guard<std::mutex> lock(transmit_state_mutex_);
		capture_timing = capture_timing_;
		transmit_timing = transmit_timing_;
	}
	const double age_ms = result ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->captured).count() : -1.0;
	META_CONPRINTF("[CS2FOW] %s; map=%s crc=0x%08x version=%u triangles=%u nodes=%u packets=%u bytes=%llu depth=%u\n",
		disabled_reason_.empty() && cs2fow_enable.Get() ? "active" : (disabled_reason_.empty() ? "disabled by convar" : disabled_reason_.c_str()), map_.c_str(),
		data_.header.source_crc32, data_.header.version, data_.header.triangle_count, data_.header.node_count, data_.header.packet_count,
		static_cast<unsigned long long>(data_.header.file_size), data_.header.max_depth);
	META_CONPRINTF("[CS2FOW] worker latest=%.3fms average=%.3fms maximum=%.3fms snapshot_age=%.1fms pairs=%u visible=%u hidden=%u cycles=%llu\n",
		stats.latest_ms, stats.average_ms, stats.maximum_ms, age_ms, stats.evaluated_pairs, stats.visible_pairs, stats.hidden_pairs,
		static_cast<unsigned long long>(stats.cycles));
	META_CONPRINTF("[CS2FOW] capture latest=%.3fms average=%.3fms maximum=%.3fms calls=%llu\n",
		capture_timing.latest_ms, capture_timing.average_ms(), capture_timing.maximum_ms,
		static_cast<unsigned long long>(capture_timing.calls));
	META_CONPRINTF("[CS2FOW] transmit latest=%.3fms average=%.3fms maximum=%.3fms calls=%llu\n",
		transmit_timing.latest_ms, transmit_timing.average_ms(), transmit_timing.maximum_ms,
		static_cast<unsigned long long>(transmit_timing.calls));
	const bool smoke_available = result != nullptr ? result->smoke_available
		: smoke_gamedata_available_ && smoke_schema_available_;
	META_CONPRINTF("[CS2FOW] smoke enabled=%d available=%d captured=%u he_listener=%d he_active=%u\n",
		cs2fow_smoke_occlusion.Get() ? 1 : 0, smoke_available ? 1 : 0, result == nullptr ? 0u : result->smoke_count,
		he_event_available_ ? 1 : 0, result == nullptr ? 0u : result->he_clearance_count);
	const bool ffa = teammates_are_enemies();
	META_CONPRINTF("[CS2FOW] teammate filtering configured=%d ffa=%d effective=%d\n",
		cs2fow_filter_teammates.Get() ? 1 : 0, ffa ? 1 : 0,
		visibility_teammate_filter_enabled(cs2fow_filter_teammates.Get(), ffa) ? 1 : 0);
	std::string bake_map;
	double bake_elapsed_ms = 0;
	if (automatic_baker_.status(bake_map, bake_elapsed_ms))
	{
		META_CONPRINTF("[CS2FOW] auto-bake map=%s elapsed=%.1fms\n", bake_map.c_str(), bake_elapsed_ms);
	}
}

bool plugin::teammates_are_enemies() const
{
	return teammates_are_enemies_.IsValidRef() && ConVarRefAbstract(teammates_are_enemies_).GetBool();
}

} // namespace cs2fow

CEntityIdentity *CEntitySystem::GetEntityIdentity(CEntityIndex entity_index)
{
	if (entity_index.Get() < 0 || entity_index.Get() >= MAX_TOTAL_ENTITIES - 1)
	{
		return nullptr;
	}
	CEntityIdentity *chunk = m_EntityList.m_pIdentityChunks[entity_index.Get() / MAX_ENTITIES_IN_LIST];
	if (chunk == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity *identity = &chunk[entity_index.Get() % MAX_ENTITIES_IN_LIST];
	return identity->GetEntityIndex() == entity_index ? identity : nullptr;
}

CEntityIdentity *CEntitySystem::GetEntityIdentity(const CEntityHandle &handle)
{
	if (!handle.IsValid())
	{
		return nullptr;
	}
	const int index = handle.GetEntryIndex();
	if (index < 0 || index >= MAX_TOTAL_ENTITIES - 1)
	{
		return nullptr;
	}
	CEntityIdentity *chunk = m_EntityList.m_pIdentityChunks[index / MAX_ENTITIES_IN_LIST];
	if (chunk == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity *identity = &chunk[index % MAX_ENTITIES_IN_LIST];
	return identity->GetRefEHandle() == handle ? identity : nullptr;
}

PLUGIN_EXPOSE(cs2fow::plugin, cs2fow::g_plugin);
