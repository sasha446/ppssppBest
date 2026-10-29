// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Common/Data/Text/I18n.h"
#include "Common/File/Path.h"
#include "Common/Net/Resolve.h"
#include "Common/StringUtils.h"
#include "Common/System/Request.h"
#include "Common/System/System.h"
#include "Common/UI/Notice.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Core/Config.h"
#include "Core/HLE/sceNetAdhoc.h"
#include "Core/System.h"
#include "Core/Util/PortManager.h"
#include "UI/AdhocServerScreen.h"
#include "UI/DiscordIntegration.h"
#include "UI/SettingsPage.h"
#include "UI/Settings/MainSettingsPages.h"

// Compound view just like the audio file choosers
class MacAddressChooser : public UI::LinearLayout {
public:
	MacAddressChooser(RequesterToken token, Path gamePath, std::string *value, std::string_view title, ScreenManager *screenManager, UI::LayoutParams *layoutParams = nullptr);
	void Update() override {
		UI::LinearLayout::Update();
		// Propagate rounding flags. TODO: Figure out a more general way.
		if (macInput_) {
			macInput_->SetRoundCorners((RoundCorners)((u32)roundCorners_ & (u32)RoundCorners::BottomLeft));
		}
		if (randomize_) {
			randomize_->SetRoundCorners((RoundCorners)((u32)roundCorners_ & (u32)RoundCorners::BottomRight));
		}
	}
private:
	UI::PopupTextInputChoice *macInput_ = nullptr;
	UI::Choice *randomize_ = nullptr;
};

MacAddressChooser::MacAddressChooser(RequesterToken token, Path gamePath, std::string *value, std::string_view title, ScreenManager *screenManager, UI::LayoutParams *layoutParams) : UI::LinearLayout(ORIENT_HORIZONTAL, layoutParams) {
	using namespace UI;
	SetSpacing(0.0f);
	if (!layoutParams) {
		layoutParams_->width = FILL_PARENT;
		layoutParams_->height = ITEM_HEIGHT;
	}
	auto n = GetI18NCategory(I18NCat::NETWORKING);

	std::string initialValue = *value;
	macInput_ = Add(new PopupTextInputChoice(token, value, title, g_Config.sMACAddress, 17, screenManager, new LinearLayoutParams(1.0f)));
	macInput_->OnChange.Add([=](UI::EventParams &e) {
		// Validate the chosen address, and restore to initialValue if bad.
		if (g_Config.sMACAddress.size() != 17) {
			// TODO: Alert the user
			*value = initialValue;
		}
	});
	randomize_ = Add(new Choice(n->T("Randomize"), new LinearLayoutParams(WRAP_CONTENT, ITEM_HEIGHT)));
	randomize_->OnClick.Add([=](UI::EventParams &) {
		auto n = GetI18NCategory(I18NCat::NETWORKING);
		auto di = GetI18NCategory(I18NCat::DIALOG);

		std::string_view confirmMessage = n->T("ChangeMacSaveConfirm", "Generate a new MAC address?");
		std::string_view warningMessage = n->T("ChangeMacSaveWarning", "Some games verify the MAC address when loading savedata, so this may break old saves.");
		std::string combined = g_Config.sMACAddress + "\n\n" + std::string(confirmMessage) + "\n\n" + std::string(warningMessage);

		auto confirmScreen = new MessagePopupScreen(
			title,
			combined, di->T("Yes"), di->T("No"),
			[](bool success) {
				if (success) {
					g_Config.sMACAddress = CreateRandMAC();
				}
			});
		screenManager->push(confirmScreen);
	});
}

// The nickname is shown on both the Networking and System tabs.
UI::View *AddNicknameChoice(SettingsPageHost *host, UI::ViewGroup *parent) {
	using namespace UI;

	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	PopupTextInputChoice *nickname = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sNickName, sy->T("Nickname"), "", 32, host->PageScreenManager()));
	if (g_Config.sNickName == "PPSSPP") {
		nickname->SetIconRight(ImageID("I_WARNING"));
	}
	nickname->OnChange.Add([host](UI::EventParams &e) {
		// Copy to infrastructure name if valid and not already set.
		if (g_Config.sInfrastructureUsername.empty()) {
			if (g_Config.sNickName == SanitizeString(g_Config.sNickName, StringRestriction::AlphaNumDashUnderscore, 3, 16)) {
				g_Config.sInfrastructureUsername = g_Config.sNickName;
			}
		}
		// Updates the warning icon, and the username notice on the Networking tab.
		host->RecreatePageViews();
	});
	return nickname;
}

// Room codes are an IPv4 address packed into 7 characters (5 bits each), which is easier to read out
// or type on a phone than dotted numbers. The alphabet leaves out I, L, O and U.
static const char kRoomCodeAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr int ROOM_CODE_LENGTH = 7;

static bool ParseIPv4(const std::string &str, uint32_t *out) {
	unsigned int parts[4];
	char extra = 0;
	if (sscanf(str.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &extra) != 4) {
		return false;
	}
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) {
		if (parts[i] > 255) {
			return false;
		}
		value = (value << 8) | parts[i];
	}
	*out = value;
	return true;
}

static std::string IPv4ToString(uint32_t ip) {
	return StringFromFormat("%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static std::string IPv4ToRoomCode(uint32_t ip) {
	std::string code(ROOM_CODE_LENGTH, '0');
	for (int i = ROOM_CODE_LENGTH - 1; i >= 0; i--) {
		code[i] = kRoomCodeAlphabet[ip & 31];
		ip >>= 5;
	}
	return code;
}

static bool RoomCodeToIPv4(std::string_view code, uint32_t *ip) {
	uint64_t value = 0;
	int count = 0;
	for (char ch : code) {
		if (ch == ' ' || ch == '-') {
			continue;
		}
		ch = (char)toupper((unsigned char)ch);
		if (ch == 'O') {
			ch = '0';
		} else if (ch == 'I' || ch == 'L') {
			ch = '1';
		}
		const char *pos = ch ? strchr(kRoomCodeAlphabet, ch) : nullptr;
		if (!pos) {
			return false;
		}
		value = value * 32 + (uint64_t)(pos - kRoomCodeAlphabet);
		count++;
		if (count > ROOM_CODE_LENGTH) {
			return false;
		}
	}
	if (count != ROOM_CODE_LENGTH || value > 0xFFFFFFFFULL) {
		return false;
	}
	*ip = (uint32_t)value;
	return true;
}

// Accepts a room code or an address. Anything with a dot in it is used as typed, so hostnames still work.
static std::string RoomInputToHost(const std::string &input) {
	std::string trimmed(StripSpaces(input));
	if (trimmed.empty()) {
		return std::string();
	}
	uint32_t ip;
	if (trimmed.find('.') == std::string::npos && RoomCodeToIPv4(trimmed, &ip)) {
		return IPv4ToString(ip);
	}
	return trimmed;
}

// Settings that rooms need, on both sides. Relay mode is for public servers, and rooms are direct.
static void EnableRoomNetworking(bool hosting, const std::string &server) {
	g_Config.bEnableWlan = true;
	g_Config.bEnableAdhocServer = hosting;
	g_Config.sProAdhocServer = server;
	if (!PSP_IsInited()) {
		g_Config.iAdhocServerRelayMode = (int)AdhocServerRelayMode::AlwaysOff;
	}
	System_RequestLocalNetworkPermission();
}

// Starts the built-in server, and shows the code that the other players type in to join.
class CreateRoomScreen : public UI::PopupScreen {
public:
	CreateRoomScreen() : PopupScreen(T(I18NCat::NETWORKING, "Create room"), T(I18NCat::DIALOG, "Back")) {
		EnableRoomNetworking(true, "localhost");
	}

	const char *tag() const override { return "CreateRoom"; }

protected:
	void CreatePopupContents(UI::ViewGroup *parent) override {
		using namespace UI;
		auto n = GetI18NCategory(I18NCat::NETWORKING);

		Margins contentMargins(12, 4);
		TextView *hint = parent->Add(new TextView(n->T("RoomHostHint", "Turn on your phone's hotspot and have your friends connect to it with Wi-Fi, or use the same Wi-Fi network. Then give them the room code and let them pick Join room."), new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, contentMargins)));
		hint->SetWordWrap();

		std::vector<std::string> ips;
		net::GetLocalIP4List(ips);
		int shown = 0;
		for (const std::string &ip : ips) {
			if (startsWith(ip, "127.") || startsWith(ip, "169.254.") || startsWith(ip, "0.")) {
				continue;
			}
			uint32_t value;
			if (!ParseIPv4(ip, &value)) {
				continue;
			}
			std::string code = IPv4ToRoomCode(value);

			LinearLayout *row = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT, contentMargins)));
			LinearLayout *lines = row->Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(1.0f, Gravity::G_VCENTER)));
			lines->SetSpacing(0.0f);
			lines->Add(new TextView(code));
			TextView *address = lines->Add(new TextView(ip));
			address->SetTextSize(TextSize::Small);
			row->Add(new Choice(ImageID("I_FILE_COPY"), new LinearLayoutParams(WRAP_CONTENT, WRAP_CONTENT)))->OnClick.Add([code](UI::EventParams &) {
				System_CopyStringToClipboard(code);
			});
			shown++;
		}

		if (shown == 0) {
			NoticeView *notice = parent->Add(new NoticeView(NoticeLevel::WARN, n->T("RoomNoNetwork", "No local network address found. Turn on the hotspot or connect to Wi-Fi first."), ""));
			notice->SetWrapText(true);
		}
	}
};

// Takes the code (or address) from the host, and points the game at their server.
class JoinRoomScreen : public UI::PopupScreen {
public:
	JoinRoomScreen() : PopupScreen(T(I18NCat::NETWORKING, "Join room"), T(I18NCat::NETWORKING, "Join"), T(I18NCat::DIALOG, "Cancel")) {}

	const char *tag() const override { return "JoinRoom"; }

protected:
	void CreatePopupContents(UI::ViewGroup *parent) override {
		using namespace UI;
		auto n = GetI18NCategory(I18NCat::NETWORKING);

		PopupTextInputChoice *input = parent->Add(new PopupTextInputChoice(GetRequesterToken(), &input_, n->T("Room code or IP address"), "", 64, screenManager()));
		input->SetShadowText(n->T("Room code or IP address"));
	}

	bool CanComplete(DialogResult result) override {
		return result != DR_OK || !RoomInputToHost(input_).empty();
	}

	void OnCompleted(DialogResult result) override {
		if (result == DR_OK) {
			EnableRoomNetworking(false, RoomInputToHost(input_));
		}
	}

private:
	std::string input_;
};

static void BuildChatSettingsPage(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();

	page->Header("Chat");
	page->Setting(&g_Config.bEnableNetworkChat);
	page->Setting(&g_Config.iChatButtonPosition).Enabled(&g_Config.bEnableNetworkChat);
	page->Setting(&g_Config.iChatScreenPosition).Enabled(&g_Config.bEnableNetworkChat);
	page->Setting(&g_Config.bChatTimestamps).Enabled(&g_Config.bEnableNetworkChat);

	page->Header("Quick chat");
	page->Setting(&g_Config.bEnableQuickChat).Enabled(&g_Config.bEnableNetworkChat);

	for (int i = 0; i < 5; i++) {
		page->Custom("Quick chat %1", [host, i](UI::ViewGroup *parent) -> UI::View * {
			return parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sQuickChat[i], ApplySafeSubstitutions(T(I18NCat::NETWORKING, "Quick chat %1"), i + 1), "", 32, host->PageScreenManager()));
		}).EnabledFunc([] {
			return g_Config.bEnableQuickChat && g_Config.bEnableNetworkChat;
		});
	}
}

void BuildNetworkingSettings(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();

	page->Header("Rooms");
	page->Choice("Create room", ImageID("I_PLUS")).OnClick([host](EventParams &e) {
		host->PageScreenManager()->push(new CreateRoomScreen());
	});
	page->Choice("Join room", ImageID("I_PLAY")).OnClick([host](EventParams &e) {
		host->PageScreenManager()->push(new JoinRoomScreen());
	});

	page->Header();

	page->Choice("Quick start guide for multiplayer", ImageID("I_LINK_OUT")).OnClick([](EventParams &e) {
		auto n = GetI18NCategory(I18NCat::NETWORKING);
		std::string url(n->T("MultiplayerQuickStartURL", "https://www.ppsspp.org/docs/multiplayer/quickstart/"));
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, url);
	});

	// Ad hoc play, the built-in server and UPnP all talk to the local network.
	auto requestLocalNetwork = [](UI::EventParams &e) {
		CheckBox *checkBox = static_cast<CheckBox *>(e.v);
		if (checkBox->Toggled()) {
			System_RequestLocalNetworkPermission();
		}
	};

	page->Setting(&g_Config.bEnableWlan).OnChange(requestLocalNetwork);
	page->Custom("MAC address", [host](UI::ViewGroup *parent) -> UI::View * {
		return parent->Add(new MacAddressChooser(host->PageRequesterToken(), host->PageGamePath(), &g_Config.sMACAddress, T(I18NCat::NETWORKING, "MAC address"), host->PageScreenManager()));
	});

	page->Header("Ad Hoc multiplayer");

	page->Custom("Ad hoc server address", [host](UI::ViewGroup *parent) -> UI::View * {
		auto n = GetI18NCategory(I18NCat::NETWORKING);
		auto launchAdhocServerScreen = [host](UI::EventParams &) {
			auto n = GetI18NCategory(I18NCat::NETWORKING);
			host->PageScreenManager()->push(new AdhocServerScreen(&g_Config.sProAdhocServer, n->T("Ad hoc server address")));
		};

		if (AdhocServerNameIsCustom()) {
			LinearLayout *serverRow = parent->Add(new LinearLayout(ORIENT_HORIZONTAL, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
			serverRow->SetSpacing(0.0f);
			serverRow->Add(new ChoiceWithValueDisplay(&g_Config.sProAdhocServer, n->T("Ad hoc server address"), I18NCat::NONE, new LinearLayoutParams(1.0f)))->OnClick.Add(launchAdhocServerScreen);
			serverRow->Add(new Choice(ImageID("I_EDIT_TEXT"), new LinearLayoutParams(ITEM_HEIGHT, ITEM_HEIGHT)))->OnClick.Add([host](UI::EventParams &) {
				AskToEditCurrentServer(host->PageRequesterToken(), host->PageScreenManager());
			});
			return serverRow;
		}

		AdhocServerListEntry entry;
		if (!g_Config.sProAdhocServer.empty() && AdhocGetServerByHost(g_Config.sProAdhocServer, &entry)) {
			Choice *choice = parent->Add(new ChoiceWithFixedValueDisplay(ApplySafeSubstitutions("%1 (%2)", entry.name, entry.host), n->T("Ad hoc server address")));
			choice->OnClick.Add(launchAdhocServerScreen);
			return choice;
		}
		// This case happens when updating from a previous version, as well as clicking on one of the local addresses
		Choice *choice = parent->Add(new ChoiceWithFixedValueDisplay(g_Config.sProAdhocServer, n->T("Ad hoc server address")));
		choice->OnClick.Add(launchAdhocServerScreen);
		return choice;
	});

	AdhocServerListEntry serverEntry;
	if (!AdhocServerNameIsCustom() && !g_Config.sProAdhocServer.empty() && AdhocGetServerByHost(g_Config.sProAdhocServer, &serverEntry)) {
		page->Custom("Server status: %1", [host, serverEntry](UI::ViewGroup *parent) -> UI::View * {
			Choice *status = parent->Add(new Choice(ApplySafeSubstitutions(T(I18NCat::NETWORKING, "Server status: %1"), serverEntry.name)));
			status->SetIconLeft(ImageID("I_INFO"));
			status->OnClick.Add([host, serverEntry](UI::EventParams &) {
				host->PageScreenManager()->push(new AdhocServerInfoScreen(serverEntry));
			});
			return status;
		});
	}

	page->Setting(&g_Config.iAdhocServerRelayMode).EnabledFunc([] {
		return !PSP_IsInited();
	});
	page->Hint({ I18NCat::NETWORKING, "PacketRelayHint", "Available on servers that provide 'aemu_postoffice' packet relay, like socom.cc. Disable this for LAN or VPN play. Can be more reliable, but sometimes slower." });

	page->Custom({ I18NCat::SYSTEM, "Nickname" }, [host](UI::ViewGroup *parent) {
		return AddNicknameChoice(host, parent);
	});
	page->Hint("Try to pick a unique nickname for ad hoc play");

	page->Setting(&g_Config.bEnableAdhocServer).OnChange(requestLocalNetwork);

	page->Setting(&g_Config.bAdhocUseUdp).EnabledFunc([] {
		return !PSP_IsInited();
	});
	page->Hint("Try switching between TCP/UDP if you suffer frequent adhoc server disconnects. Some network environments favor TCP and some UDP.");

	page->Setting(&g_Config.bAdhocRelayModeEnetPdpReliable).EnabledFunc([] {
		return !PSP_IsInited() && g_Config.bAdhocUseUdp;
	});
	page->Hint("When disabled, packet loss on datagram adhoc sockets will be exposed to the game's netcode. Keep this enabled if you don't know what that means.");

	page->Header("Infrastructure");
	if (g_Config.sInfrastructureUsername.empty()) {
		page->Custom("To play in Infrastructure Mode, you must enter a username", [](UI::ViewGroup *parent) -> UI::View * {
			NoticeView *notice = parent->Add(new NoticeView(NoticeLevel::WARN, T(I18NCat::NETWORKING, "To play in Infrastructure Mode, you must enter a username"), ""));
			notice->SetWrapText(true);
			return notice;
		});
	}
	page->Custom({ I18NCat::DIALOG, "Username" }, [host](UI::ViewGroup *parent) -> UI::View * {
		PopupTextInputChoice *usernameChoice = parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sInfrastructureUsername, T(I18NCat::DIALOG, "Username"), "", 16, host->PageScreenManager()));
		usernameChoice->SetRestriction(StringRestriction::AlphaNumDashUnderscore, 3);
		usernameChoice->OnChange.Add([host](UI::EventParams &e) {
			host->RecreatePageViews();
		});
		return usernameChoice;
	});

	page->Setting(&g_Config.bInfrastructureAutoDNS);
	page->Custom("DNS server", [host](UI::ViewGroup *parent) -> UI::View * {
		return parent->Add(new PopupTextInputChoice(host->PageRequesterToken(), &g_Config.sInfrastructureDNSServer, T(I18NCat::NETWORKING, "DNS server"), "", 32, host->PageScreenManager()));
	}).Disabled(&g_Config.bInfrastructureAutoDNS);

	page->Header("UPnP (port-forwarding)");
	// Only togglable outside a game - sceNet latches settings like UPnPUseOriginalPort at boot,
	// and a game that's already mapped its ports wouldn't cope with them disappearing.
	page->Setting(&g_Config.bEnableUPnP).EnabledFunc([] {
		return !PSP_IsInited();
	}).OnChange([requestLocalNetwork](UI::EventParams &e) {
		requestLocalNetwork(e);
		// Wake the UPnP service thread so it connects (or tears its mappings back down) right
		// away, instead of waiting for a port request that may never come.
		UPnP_Notify();
	});
	page->Setting(&g_Config.bUPnPUseOriginalPort).Enabled(&g_Config.bEnableUPnP);
	page->Hint({ I18NCat::NETWORKING, "UseOriginalPort Tip", "May not work for all devices or games, see wiki." });

	page->Header();
	page->Link({I18NCat::NETWORKING, "Chat"}, I18NCat::NETWORKING, &BuildChatSettingsPage);

	page->Header({ I18NCat::NETWORKING, "Misc", "Misc (default = compatibility)" });
	page->Choice("Open PPSSPP Multiplayer Wiki Page", ImageID("I_LINK_OUT")).OnClick([](EventParams &e) {
		auto n = GetI18NCategory(I18NCat::NETWORKING);
		std::string url(n->T("MultiplayerHowToURL", "https://github.com/hrydgard/ppsspp/wiki/How-to-play-multiplayer-games-with-PPSSPP"));
		System_LaunchUrl(LaunchUrlType::BROWSER_URL, url);
	});
	static const char *wlanChannels[] = {"Auto", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11"};
	page->Setting(&g_Config.iWlanAdhocChannel).Setup([](View *view) {
		PopupMultiChoice *wlanChannelChoice = static_cast<PopupMultiChoice *>(view);
		wlanChannelChoice->SetChoicesUntranslated(1, (int)ARRAY_SIZE(wlanChannels) - 1);
		for (int i = 0; i < 4; i++) {
			wlanChannelChoice->HideChoice(i + 2);
			wlanChannelChoice->HideChoice(i + 7);
		}
	});
	page->Setting(&g_Config.iPortOffset);
	page->Hint("PortOffsetHint");
	page->Setting(&g_Config.iMinTimeout);
	page->Setting(&g_Config.bForcedFirstConnect);
	page->Setting(&g_Config.bAllowSpeedControlWhileConnected);
	if (Discord::IsAvailable()) {
		page->Setting(&g_Config.bDiscordRichPresence);
	}
}
