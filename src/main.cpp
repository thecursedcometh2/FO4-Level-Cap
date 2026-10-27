#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>

#include <Windows.h>

namespace
{
	using SFValue = Scaleform::GFx::Value;
	using SteadyClock = std::chrono::steady_clock;
	using PlayMenuSound_t = void (*)(const char*);

	constexpr const char*       kGroupPath = "root.HUDNotificationsGroup_mc";
	constexpr const char*       kMeterName = "XPMeter_mc";
	constexpr const char*       kPieces[] = { "LeveUpTextClip", "LevelUPBar", "LevelUpBracket", "NumberText", "PlusSign", "xptext" };
	constexpr std::size_t       kPieceCount = std::size(kPieces);
	constexpr auto              kResetDelay = std::chrono::milliseconds(2000);
	constexpr int               kTickMs = 16;
	constexpr int               kEvalMs = 100;

	int                       g_cap{ 100 };
	double                    g_xpBase{ 200.0 };
	double                    g_xpBump{ 75.0 };
	int                       g_lastLevel{ -1 };
	float                     g_lastXP{ -1.0F };
	SteadyClock::time_point   g_lastChange{ SteadyClock::now() };
	unsigned                  g_hiddenMask{ 0 };
	PlayMenuSound_t           g_origPlayMenuSound{ nullptr };
	std::atomic<bool>         g_pending{ false };
	std::atomic<bool>         g_started{ false };
	std::atomic<bool>         g_atCap{ false };

	std::filesystem::path GetPluginDir()
	{
		HMODULE module = nullptr;
		::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&GetPluginDir),
			&module);
		char buf[MAX_PATH]{};
		::GetModuleFileNameA(module, buf, MAX_PATH);
		return std::filesystem::path(buf).parent_path();
	}

	void LoadConfig()
	{
		const auto ini = (GetPluginDir() / "HardLevelCap.ini").string();
		g_cap = (std::max)(static_cast<int>(::GetPrivateProfileIntA("General", "LevelCap", 100, ini.c_str())), 2);
	}

	void ReadNumberSetting(const char* a_name, double& a_out)
	{
		const auto settings = RE::GameSettingCollection::GetSingleton();
		if (!settings) {
			return;
		}
		const auto setting = settings->GetSetting(a_name);
		if (!setting) {
			return;
		}
		switch (setting->GetType()) {
			case RE::Setting::SETTING_TYPE::kInt:
				a_out = static_cast<double>(setting->GetInt());
				break;
			case RE::Setting::SETTING_TYPE::kFloat:
				a_out = static_cast<double>(setting->GetFloat());
				break;
			default:
				break;
		}
	}

	double XPStart(int a_level)
	{
		if (a_level <= 1) {
			return 0.0;
		}
		const double m = static_cast<double>(a_level - 1);
		return g_xpBase * m + g_xpBump * m * (m - 1.0) / 2.0;
	}

	bool WriteBaseXPDirect(RE::PlayerCharacter* a_player, const RE::ActorValueInfo& a_info, float a_expectedOld, float a_value)
	{
		auto&      storage = a_player->avStorage;
		const auto id = a_info.GetFormID();
		bool       done = false;

		storage.avLock.lock_write();
		for (auto& entry : storage.baseValues) {
			if (entry.first == id) {
				if (std::fabs(entry.second - a_expectedOld) < 1.0F) {
					entry.second = a_value;
					done = true;
				}
				break;
			}
		}
		storage.avLock.unlock_write();
		return done;
	}

	void Evaluate()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto avs = RE::ActorValue::GetSingleton();
		if (!player || !avs || !avs->experience) {
			return;
		}

		const int   level = static_cast<int>(player->GetLevel());
		const float xp = player->GetBaseActorValue(*avs->experience);
		const auto  now = SteadyClock::now();

		if (level != g_lastLevel || xp != g_lastXP) {
			g_lastLevel = level;
			g_lastXP = xp;
			g_lastChange = now;
		}

		if (level < g_cap) {
			return;
		}

		const double start = XPStart(level);
		if (static_cast<double>(xp) < start - 5.0) {
			return;
		}

		const double target = start + 2.0;
		if (static_cast<double>(xp) <= target + 0.5) {
			return;
		}

		if (now - g_lastChange < kResetDelay) {
			return;
		}

		if (!WriteBaseXPDirect(player, *avs->experience, xp, static_cast<float>(target))) {
			player->SetBaseActorValue(*avs->experience, static_cast<float>(target));
		}
		g_lastXP = static_cast<float>(target);
		g_lastChange = now;
	}

	bool IsVisible(const SFValue& a_val)
	{
		SFValue member;
		if (a_val.GetMember("visible", &member) && member.IsBoolean()) {
			return member.GetBoolean();
		}
		return true;
	}

	Scaleform::GFx::Movie* GetHUDMovie()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return nullptr;
		}
		const auto menu = ui->GetMenu<RE::HUDMenu>();
		if (!menu) {
			return nullptr;
		}
		return menu->uiMovie.get();
	}

	void HudTick(bool a_atCap)
	{
		if (!a_atCap && g_hiddenMask == 0) {
			return;
		}

		const auto movie = GetHUDMovie();
		if (!movie) {
			return;
		}

		SFValue group;
		if (!movie->GetVariable(&group, kGroupPath) || !group.IsDisplayObject()) {
			return;
		}

		SFValue meter;
		if (!group.GetMember(kMeterName, &meter) || !meter.IsDisplayObject()) {
			return;
		}

		for (std::size_t i = 0; i < kPieceCount; ++i) {
			SFValue piece;
			if (!meter.GetMember(kPieces[i], &piece) || !piece.IsDisplayObject()) {
				continue;
			}
			const unsigned bit = 1u << i;
			if (a_atCap) {
				if (IsVisible(piece)) {
					piece.SetMember("visible", SFValue(false));
					g_hiddenMask |= bit;
				}
			} else if (g_hiddenMask & bit) {
				piece.SetMember("visible", SFValue(true));
				g_hiddenMask &= ~bit;
			}
		}
	}

	void Tick(bool a_eval)
	{
		g_pending = false;
		if (a_eval) {
			Evaluate();
		}

		const auto player = RE::PlayerCharacter::GetSingleton();
		const bool atCap = player && static_cast<int>(player->GetLevel()) >= g_cap;
		g_atCap = atCap;
		HudTick(atCap);
	}

	void PollLoop()
	{
		int sinceEval = 0;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
			sinceEval += kTickMs;
			const bool doEval = sinceEval >= kEvalMs;
			if (doEval) {
				sinceEval = 0;
			}
			if (g_pending.exchange(true)) {
				continue;
			}
			F4SE::GetTaskInterface()->AddTask([doEval]() { Tick(doEval); });
		}
	}

	bool IsExperienceSound(const char* a_name)
	{
		std::string name(a_name);
		for (auto& c : name) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return name.find("experience") != std::string::npos;
	}

	void Hook_PlayMenuSound(const char* a_name)
	{
		if (a_name && g_atCap.load() && IsExperienceSound(a_name)) {
			return;
		}
		g_origPlayMenuSound(a_name);
	}

	void InstallSoundHook()
	{
		const auto init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
			return;
		}

		const auto target = reinterpret_cast<LPVOID>(RE::ID::UIUtils::PlayMenuSound.address());
		if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_PlayMenuSound), reinterpret_cast<LPVOID*>(&g_origPlayMenuSound)) == MH_OK) {
			MH_EnableHook(target);
		}
	}

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || a_msg->type != F4SE::MessagingInterface::kGameDataReady) {
			return;
		}
		if (g_started.exchange(true)) {
			return;
		}

		ReadNumberSetting("iXPBase", g_xpBase);
		ReadNumberSetting("iXPBumpBase", g_xpBump);
		InstallSoundHook();
		std::thread(PollLoop).detach();
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, { .log = false });
	LoadConfig();

	const auto messaging = F4SE::GetMessagingInterface();
	return messaging && messaging->RegisterListener(OnMessage);
}
