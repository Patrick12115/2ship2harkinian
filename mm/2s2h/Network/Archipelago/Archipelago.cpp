#include "Archipelago.h"
#include <libultraship/libultraship.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/Rando/StaticData/StaticData.h"
#include "2s2h/Network/Archipelago/ArchipelagoConsoleWindow.h"
#include "2s2h/BenGui/Notification.h"
#include "2s2h/Rando/Rando.h"
#include "2s2h/CustomItem/CustomItem.h"
#include "2s2h/Rando/MiscBehavior/Traps.h"
#include "2s2h/CustomMessage/CustomMessage.h"
#include "2s2h/ShipInit.hpp"
#include "2s2h/Rando/CheckTracker/CheckTracker.h"
#include "2s2h/Rando/ActorBehavior/ActorBehavior.h"
#include "2s2h/Rando/MiscBehavior/MiscBehavior.h"
#include "2s2h/Rando/MiscBehavior/ClockShuffle.h"

// Must be defined BEFORE any websocketpp / wswrap / apclientpp includes.
#ifdef _WIN32
// Prevent windows.h from dragging in winsock.h (which conflicts with WinSock2.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Make sure Winsock2 is used, not winsock.h
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

// apclientpp/wswrap note this sometimes matters for Asio on Windows
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

// Force standalone Asio mode (no Boost.Asio)
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

// Force websocketpp to use std::type_traits instead of Boost type_traits
#ifndef _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#define _WEBSOCKETPP_CPP11_TYPE_TRAITS_
#endif

// Force websocketpp to use std::random (avoids Boost random)
#ifndef _WEBSOCKETPP_CPP11_RANDOM_DEVICE_
#define _WEBSOCKETPP_CPP11_RANDOM_DEVICE_
#endif
#ifndef _WEBSOCKETPP_CPP11_RANDOM_
#define _WEBSOCKETPP_CPP11_RANDOM_
#endif

static constexpr int MAX_RETRIES = 3;

static constexpr char const* AP_GAME_NAME = "2 Ship 2 Harkinian (MM)";
static constexpr char const* AP_WORLD_VERSION_MAJOR = "0";
static constexpr char const* AP_WORLD_VERSION_MINOR = "0";

#include <apuuid.hpp>
#include <apclient.hpp>

extern "C" {
void TitleSetup_Init(GameState*);
void FileSelect_Init(GameState*);
void ConsoleLogo_Init(GameState*);
}

// Storing statically here, because APClient isn't useable from header...
static std::unique_ptr<APClient> sAPClient;

static std::string GetCertPath() {
    std::filesystem::path base =
        std::filesystem::absolute(Ship::Context::GetInstance()->GetAppDirectoryPath()).lexically_normal();

    // Try: <appDir>/networking/cacert.pem
    std::filesystem::path p1 = (base / "networking" / "cacert.pem").lexically_normal();
    if (std::filesystem::exists(p1)) {
        return p1.string();
    }

    // Try: <appDir>/../networking/cacert.pem  (your confirmed existing location)
    std::filesystem::path p2 = (base.parent_path() / "networking" / "cacert.pem").lexically_normal();
    if (std::filesystem::exists(p2)) {
        return p2.string();
    }

    // On Linux, fall back to common system CA bundle locations
#if defined(__linux__)
    static const char* sLinuxCaPaths[] = { "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu
                                           "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL
                                           "/etc/ssl/ca-bundle.pem",             // OpenSUSE
                                           "/etc/pki/tls/cacert.pem",            // older RHEL
                                           nullptr };
    for (int i = 0; sLinuxCaPaths[i] != nullptr; i++) {
        if (std::filesystem::exists(sLinuxCaPaths[i])) {
            return sLinuxCaPaths[i];
        }
    }
#endif

    // Fall back to the original (so logs still show what we'd try)
    return p1.string();
}

bool Archipelago::IsAPItem(RandoItemId randoItemId) {
    return randoItemId == RI_ARCHIPELAGO_PROGRESSIVE || randoItemId == RI_ARCHIPELAGO_USEFUL ||
           randoItemId == RI_ARCHIPELAGO_JUNK;
}

RandoItemId Archipelago::GetRandoItemIdFromNetworkItem(NetworkItem networkItem,
                                                       bool convertOtherPlayerItems /*= false*/) {
    if (networkItem.player != 0 && networkItem.player != sAPClient->get_player_number() && convertOtherPlayerItems) {
        // This is an item for another player - return based on flags
        if (networkItem.flags & 0x1) { // Progression
            return RI_ARCHIPELAGO_PROGRESSIVE;
        } else if (networkItem.flags & 0x2) { // Useful
            return RI_ARCHIPELAGO_USEFUL;
        } else { // Junk/Filler
            return RI_ARCHIPELAGO_JUNK;
        }
    }

    // Try resolving from local game first (items sent TO us), then fall back to player's game
    std::string itemName = sAPClient->get_item_name(networkItem.item, AP_GAME_NAME);
    if (itemName == "Unknown" || itemName.empty()) {
        itemName = sAPClient->get_item_name(networkItem.item, sAPClient->get_player_game(networkItem.player));
    }

    // Special-case: both triforce RIs share the same name; always prefer the canonical one.
    if (itemName == "Piece of the Triforce") {
        return RI_TRIFORCE_PIECE;
    }

    for (const auto& [id, staticItem] : Rando::StaticData::Items) {
        if (staticItem.name == itemName) {
            return id;
        }
    }

    return RI_UNKNOWN;
}

void Archipelago::Reset() {
    connectionRetryCount = 0;
    isConnectionReady = false;
    isSlotDataReady = false;
    isCheckInfoReady = false;
    isSaveSynced = false;
    isItemQueued = false;
    checkInfo.clear();
    slotData.clear();
    incomingCheckedLocations.clear();
    incomingItems.clear();
}

void Archipelago::Enable() {
    if (sAPClient) {
        SPDLOG_WARN("Archipelago client already initialized, skipping re-initialization.");
        return;
    }

    Reset();

    const std::string uri = CVarGetString("gArchipelago.ServerAddress", "archipelago.gg:38281");
    const std::string uuid = ap_get_uuid("uuid");
    const std::string cert = GetCertPath();

    sAPClient = std::unique_ptr<APClient>(new APClient(uuid, AP_GAME_NAME, uri, cert));

    RegisterHooks();
}

void Archipelago::Disable() {
    if (!sAPClient) {
        SPDLOG_WARN("Archipelago client not initialized, skipping disable.");
        return;
    }

    Reset();

    sAPClient->reset();
    sAPClient.reset();

    RegisterHooks();
}

u8 Archipelago::GetState() {
    if (!sAPClient) {
        return static_cast<u8>(APClient::State::DISCONNECTED);
    }

    u8 state = static_cast<u8>(sAPClient->get_state());

    // If we're disconnected but have an sAPClient, it means we're trying to connect - report that so UI can show
    // feedback
    if (state == static_cast<u8>(APClient::State::DISCONNECTED)) {
        return static_cast<u8>(APClient::State::SOCKET_CONNECTING);
    }

    return state;
}

bool Archipelago::IsConnected() {
    return GetState() == static_cast<u8>(APClient::State::SLOT_CONNECTED);
}

void Archipelago::SendChat(const char* msg) {
    if (msg == nullptr || msg[0] == '\0') {
        return;
    }

    if (!IsConnected()) {
        return;
    }

    sAPClient->Say(std::string(msg));
}

void Archipelago::RegisterHooks() {
    bool shouldRegister = GetState() != static_cast<u8>(APClient::State::DISCONNECTED);

    COND_HOOK(GameInteractor::OnGameStateUpdate, shouldRegister, [&]() { OnGameTick(); });

    COND_HOOK(GameInteractor::OnGameCompletion, shouldRegister,
              [&]() { sAPClient->StatusUpdate(APClient::ClientStatus::GOAL); });

    if (!shouldRegister)
        return;
    SPDLOG_INFO("Registering Archipelago client hooks");

    sAPClient->set_socket_error_handler([&](const std::string& msg) {
        connectionRetryCount++;
        if (connectionRetryCount > MAX_RETRIES) {
            ArchipelagoConsole_SendMessage("[ERROR] Could not connect to server after several tries.\n"
                                           "Are the entered server address and port correct?");

            // Emit error notification
            Notification::Emit({ .message = "Failed to connect to Archipelago server",
                                 .messageColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                 .remainingTime = 5.0f });
        }
    });

    sAPClient->set_room_info_handler([&]() {
        std::list<std::string> tags;
        if (CVarGetInteger("gArchipelago.DeathLink", 0)) {
            tags.push_back("DeathLink");
        }

        std::string slot = CVarGetString("gArchipelago.Slot", "");
        std::string password = CVarGetString("gArchipelago.Password", "");
        sAPClient->ConnectSlot(slot, password, 0b0111, tags, { 0, 6, 3 });
    });

    sAPClient->set_slot_refused_handler([](const std::list<std::string>& msgs) {
        // Collect all error messages
        std::string allErrors;
        for (const std::string& msg : msgs) {
            ArchipelagoConsole_SendMessage("[ERROR] %s", msg.c_str());
            if (!allErrors.empty()) {
                allErrors += "; ";
            }
            allErrors += msg;
        }

        // Emit error notification with the error messages
        if (!allErrors.empty()) {
            Notification::Emit({ .message = allErrors.c_str(),
                                 .messageColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                 .remainingTime = 7.0f });
        }
    });

    sAPClient->set_slot_connected_handler([&](const nlohmann::json data) {
        // Store this object to be used when we've determined the file is loaded
        slotData = data.get<nlohmann::json::object_t>();
        isSlotDataReady = true;

        // Fetch all item data
        auto allLocations = sAPClient->get_checked_locations();
        incomingCheckedLocations.insert(allLocations.begin(), allLocations.end());
        auto missingLocations = sAPClient->get_missing_locations();
        allLocations.insert(missingLocations.begin(), missingLocations.end());
        std::list<int64_t> allLocationsList;
        for (auto& loc : allLocations) {
            allLocationsList.push_back(loc);
        }
        // This data comes back in set_location_info_handler
        sAPClient->LocationScouts(allLocationsList);

        std::string slotName = sAPClient->get_player_alias(sAPClient->get_player_number());
        std::string message = "Connected to slot: " + slotName;
        Notification::Emit(
            { .message = message.c_str(), .messageColor = ImVec4(0.5f, 1.0f, 0.5f, 1.0f), .remainingTime = 5.0f });
    });

    sAPClient->set_location_info_handler([&](const std::list<APClient::NetworkItem>& networkItems) {
        for (const auto& networkItem : networkItems) {
            checkInfo[static_cast<RandoCheckId>(networkItem.location)] = { networkItem.item, networkItem.location,
                                                                           networkItem.player, networkItem.flags,
                                                                           networkItem.index };
        }

        isCheckInfoReady = true;
    });

    sAPClient->set_items_received_handler([&](const std::list<APClient::NetworkItem>& networkItems) {
        // The AP server sends ReceivedItems in two ways:
        //   Full sync  (first item index == 0): all items from the beginning, on initial connect.
        //   Delta      (first item index  > 0): only newly unlocked items, e.g. after a location check.
        //
        // We must NOT clear the cache on a delta — doing so would wipe the full history and leave
        // ResyncItems with only the 1 new item, breaking re-sync when a new save is created.
        // Only clear when it's a full sync (index 0).
        if (!networkItems.empty() && networkItems.front().index == 0) {
            incomingItems.clear();
        }
        incomingItems.reserve(incomingItems.size() + networkItems.size());
        for (const auto& networkItem : networkItems) {
            incomingItems.push_back(
                { networkItem.item, networkItem.location, networkItem.player, networkItem.flags, networkItem.index });
        }
    });

    sAPClient->set_location_checked_handler([&](const std::list<int64_t> locations) {
        for (auto location : locations) {
            incomingCheckedLocations.insert(location);
        }
    });

    sAPClient->set_print_json_handler([](const APClient::PrintJSONArgs& arg) {
        std::vector<AP_Text::ColoredTextNode> coloredNodes;
        coloredNodes.reserve(arg.data.size());

        for (const APClient::TextNode& node : arg.data) {
            AP_Text::TextColor color = AP_Text::TextColor::COLOR_DEFAULT;
            std::string text;

            // This mapping is ported from SoH. It depends on APClient�s TextNode shape.
            // If your APClient version differs, adjust fields accordingly.
            if (node.type == "player_id") {
                int id = std::stoi(node.text);
                if (color == AP_Text::TextColor::COLOR_DEFAULT && id == sAPClient->get_player_number()) {
                    color = AP_Text::TextColor::COLOR_MAGENTA;
                } else if (color == AP_Text::TextColor::COLOR_DEFAULT) {
                    color = AP_Text::TextColor::COLOR_YELLOW;
                }
                text = sAPClient->get_player_alias(id);
            } else if (node.type == "item_id") {
                int64_t id = std::stoll(node.text);
                if (color == AP_Text::TextColor::COLOR_DEFAULT) {
                    if (node.flags & APClient::ItemFlags::FLAG_ADVANCEMENT) {
                        color = AP_Text::TextColor::COLOR_PLUM;
                    } else if (node.flags & APClient::ItemFlags::FLAG_NEVER_EXCLUDE) {
                        color = AP_Text::TextColor::COLOR_SLATEBLUE;
                    } else if (node.flags & APClient::ItemFlags::FLAG_TRAP) {
                        color = AP_Text::TextColor::COLOR_SALMON;
                    } else {
                        color = AP_Text::TextColor::COLOR_CYAN;
                    }
                }
                text = sAPClient->get_item_name(id, sAPClient->get_player_game(node.player));
            } else if (node.type == "location_id") {
                int64_t id = std::stoll(node.text);
                if (color == AP_Text::TextColor::COLOR_DEFAULT) {
                    color = AP_Text::TextColor::COLOR_BLUE;
                }
                text = sAPClient->get_location_name(id, sAPClient->get_player_game(node.player));
            } else if (node.type == "hint_status") {
                text = node.text;
                if (node.hintStatus == APClient::HINT_FOUND) {
                    color = AP_Text::TextColor::COLOR_GREEN;
                } else if (node.hintStatus == APClient::HINT_UNSPECIFIED) {
                    color = AP_Text::TextColor::COLOR_GRAY;
                } else if (node.hintStatus == APClient::HINT_NO_PRIORITY) {
                    color = AP_Text::TextColor::COLOR_SLATEBLUE;
                } else if (node.hintStatus == APClient::HINT_AVOID) {
                    color = AP_Text::TextColor::COLOR_SALMON;
                } else if (node.hintStatus == APClient::HINT_PRIORITY) {
                    color = AP_Text::TextColor::COLOR_PLUM;
                } else {
                    color = AP_Text::TextColor::COLOR_RED;
                }
            } else if (node.type == "ERROR") {
                color = AP_Text::TextColor::COLOR_ERROR;
                text = node.text;
            } else if (node.type == "LOG") {
                color = AP_Text::TextColor::COLOR_LOG;
                text = node.text;
            } else {
                color = AP_Text::TextColor::COLOR_WHITE;
                text = node.text;
            }

            AP_Text::ColoredTextNode out;
            out.color = color;
            out.text = text;
            coloredNodes.push_back(out);
        }

        ArchipelagoConsole_PrintJson(coloredNodes);
    });

    sAPClient->set_bounced_handler([](const nlohmann::json data) {
        // if (sDisconnecting || !data.contains("tags")) {
        //     return;
        // }

        // std::list<std::string> tags = data["tags"];
        // const bool deathLink = (std::find(tags.begin(), tags.end(), "DeathLink") != tags.end());
        // if (!deathLink || !data.contains("data")) {
        //     return;
        // }

        // // Mirror SoH logic shape, but keep gameplay in Bridge.
        // // NOTE: AP "source" in SoH is compared vs apClient->get_slot() (string).
        // try {
        //     const std::string source = data["data"]["source"];
        //     const std::string cause = data["data"]["cause"];

        //     // Don�t self-kill if it�s our own bounce (SoH checks this).
        //     if (sClient && source != sClient->get_slot()) {
        //         // Apply only if in-game (SoH does).
        //         if (ArchipelagoBridge::IsInGame()) {
        //             // Queue to bridge; bridge applies on main thread via Tick()
        //             ArchipelagoBridge::EnqueueDeathLink(source, cause);

        //             ArchipelagoConsole_SendMessage("[LOG] Received death link from %s. Cause: %s", source.c_str(),
        //                                            cause.c_str());
        //             sIsDeathLinkedDeath = true;
        //         }
        //     }
        // } catch (...) {
        //     // ignore malformed bounce payload
        // }
    });
}

void Archipelago::OnGameTick() {
    if (IsConnected() && !isConnectionReady) {
        isConnectionReady = true;
    }

    if (isSaveSynced && (gGameState->init == TitleSetup_Init || gGameState->init == FileSelect_Init ||
                         gGameState->init == ConsoleLogo_Init)) {
        isSaveSynced = false;
    }

    if (IS_ARCHI && gPlayState && isConnectionReady && isSlotDataReady && isCheckInfoReady && !isSaveSynced) {
        // First apply all options
        for (const auto& [randoOptionId, randoStaticOption] : Rando::StaticData::Options) {
            if (slotData.contains(randoStaticOption.apName)) {
                try {
                    uint32_t value = slotData[randoStaticOption.apName];
                    RANDO_SAVE_OPTIONS[randoOptionId] = value;
                } catch (const std::exception& e) {
                    SPDLOG_ERROR("[AP][Bridge] Error applying option {}: {}", randoStaticOption.apName, e.what());
                }
            }
        }

        // Reset all checks to unshuffled
        for (auto& [randoCheckId, randoStaticCheck] : Rando::StaticData::Checks) {
            if (randoCheckId != RC_UNKNOWN) {
                RANDO_SAVE_CHECKS[randoCheckId].shuffled = false;
                RANDO_SAVE_CHECKS[randoCheckId].randoItemId = randoStaticCheck.randoItemId;
            }
        }

        // Apply shuffled from checkInfo
        for (const auto& [randoCheckId, networkItem] : checkInfo) {
            if (randoCheckId != RC_UNKNOWN) {
                RANDO_SAVE_CHECKS[randoCheckId].shuffled = true;
                RANDO_SAVE_CHECKS[randoCheckId].randoItemId = GetRandoItemIdFromNetworkItem(networkItem, true);
            } else {
                SPDLOG_WARN("[AP][Bridge] Could not find RandoCheckId for location: {}", networkItem.location);
            }
        }

        // Apply shop prices from slot_data
        if (slotData.contains("shop_prices") && slotData["shop_prices"].is_object()) {
            for (auto& it : slotData["shop_prices"].items()) {
                try {
                    const std::string& locationName = it.key();
                    const auto& price = it.value();
                    RandoCheckId foundCheck = Rando::StaticData::GetCheckIdFromName(locationName.c_str());
                    if (foundCheck != RC_UNKNOWN) {
                        int priceValue = price.get<int>();
                        RANDO_SAVE_CHECKS[foundCheck].price = priceValue;
                    } else {
                        SPDLOG_WARN("[AP][Bridge] Could not find RandoCheckId for shop location: {}", locationName);
                    }
                } catch (const std::exception& e) { SPDLOG_ERROR("[AP][Bridge] Error applying price {}", e.what()); }
            }
        }

        if (!gSaveContext.save.shipSaveInfo.rando.archipelago.startingItemsGranted) {
            Rando::GrantStartingItems();
            gSaveContext.save.shipSaveInfo.rando.archipelago.startingItemsGranted = 1;
        }

        // Sync all checks already obtained in case any were collected while disconnected
        std::list<int64_t> checkedLocations;
        for (const auto& [randoCheckId, networkItem] : checkInfo) {
            if (RANDO_SAVE_CHECKS[randoCheckId].obtained) {
                checkedLocations.push_back(networkItem.location);
            }
        }
        if (!checkedLocations.empty()) {
            sAPClient->LocationChecks(checkedLocations);
        }

        Rando::MiscBehavior::OnFileLoad();
        Rando::ActorBehavior::OnFileLoad();
        Rando::CheckTracker::OnFileLoad();
        Rando::ClockShuffle::OnFileLoad();
        ShipInit::Init("IS_RANDO");
        isSaveSynced = true;
        SPDLOG_INFO("[AP][Bridge] Save synced with server.");
    }

    if (IS_ARCHI && gPlayState && isSaveSynced) {
        // Apply all checked locations
        if (incomingCheckedLocations.size() != gSaveContext.save.shipSaveInfo.rando.archipelago.checkedLocationCount) {
            for (const auto& location : incomingCheckedLocations) {
                RandoCheckId randoCheckId = static_cast<RandoCheckId>(location);
                if (!RANDO_SAVE_CHECKS[location].obtained) {
                    SPDLOG_INFO("[AP][Bridge] Marking location checked: {} ({})",
                                sAPClient->get_location_name(
                                    location, sAPClient->get_player_game(checkInfo[randoCheckId].player)),
                                location);
                }
                RANDO_SAVE_CHECKS[location].obtained = true;
            }
            gSaveContext.save.shipSaveInfo.rando.archipelago.checkedLocationCount = incomingCheckedLocations.size();
        }

        if (!RANDO_SAVE_CHECKS[RC_STARTING_ITEM_DEKU_MASK].obtained) {
            RANDO_SAVE_CHECKS[RC_STARTING_ITEM_DEKU_MASK].eligible = true;
            RANDO_SAVE_CHECKS[RC_STARTING_ITEM_SONG_OF_HEALING].eligible = true;
        }

        // Queue Item Gives
        ProcessItemQueue();
    }

    if (connectionRetryCount > MAX_RETRIES) {
        Disable();
    } else {
        sAPClient->poll();
    }
}

void Archipelago::ProcessItemQueue() {
    u32 nextItemIndex = gSaveContext.save.shipSaveInfo.rando.archipelago.receivedItemCount;

    if (isItemQueued || incomingItems.empty() || incomingItems.size() <= nextItemIndex) {
        return;
    }

    auto& item = incomingItems[nextItemIndex];
    isItemQueued = true;

    RandoItemId randoItemId = GetRandoItemIdFromNetworkItem(item);
    SPDLOG_INFO("[AP][Bridge] Processing incoming item: {} (Location: {}, Player: {} ri: {})",
                sAPClient->get_item_name(item.item, sAPClient->get_player_game(item.player)), item.location,
                item.player, (int)randoItemId);

    // Queue a GameInteractor event for proper item display
    GameInteractor::Instance->events.emplace_back(GIEventGiveItem{
        .showGetItemCutscene =
            Rando::StaticData::ShouldShowGetItemCutscene(Rando::ConvertItem(GetRandoItemIdFromNetworkItem(item))),
        .param = (s16)nextItemIndex,
        .giveItem =
            [](Actor* actor, PlayState* play) {
                auto& item = Archipelago::Instance->incomingItems[CUSTOM_ITEM_PARAM];
                RandoItemId randoItemId =
                    Rando::ConvertItem(Archipelago::Instance->GetRandoItemIdFromNetworkItem(item));
                RandoCheckId randoCheckId;
                if (item.location < 0 || item.location >= RC_MAX || item.player != sAPClient->get_player_number()) {
                    randoCheckId = RC_UNKNOWN;
                } else {
                    randoCheckId = static_cast<RandoCheckId>(item.location);
                }

                // Determine if item is from another player
                int localPlayer = sAPClient->get_player_number();
                bool fromOtherPlayer = (item.player != localPlayer && item.player >= 0 && localPlayer >= 0);

                std::string message = "%g" + Rando::StaticData::GetItemName(randoItemId, true, randoCheckId) + "%w";
                std::string prefix = "You found";

                if (randoItemId == RI_JUNK && randoCheckId != RC_UNKNOWN) {
                    // This field is only used for rando seed so we just pass it a unique value
                    randoItemId = Rando::CurrentJunkItem(randoCheckId);
                    // message = "%gJunk%w";
                }

                // Get player name for items from other players
                if (fromOtherPlayer) {
                    prefix = "You received";
                    std::string playerName = sAPClient->get_player_alias(item.player);
                    message += " from %y" + playerName + "%w";
                }

                bool isTrap = (randoItemId == RI_TRAP);
                if (randoItemId == RI_TRAP) {
                    prefix = "";
                    message = GetTrapMessage();
                }

                CustomMessage::Entry entry = {
                    .textboxType = 2,
                    .icon = Rando::StaticData::GetIconForZMessage(randoItemId),
                    .msg = (prefix.empty() ? "" : prefix + " ") + message + (isTrap ? "" : "!"),
                };

                // Show message based on cutscene settings
                if (CUSTOM_ITEM_FLAGS & CustomItem::GIVE_ITEM_CUTSCENE) {
                    CustomMessage::SetActiveCustomMessage(entry.msg, entry);
                } else if (Rando::StaticData::ShouldShowGetItemCutscene(randoItemId)) {
                    CustomMessage::StartTextbox(entry.msg + "\x1C\x02\x10", entry);
                } else {
                    if (Rando::StaticData::Items[randoItemId].randoItemType != RITYPE_JUNK) {
                        message = CustomMessage::RemoveColorCodes(message);
                        Notification::Emit({
                            .itemIcon = Rando::StaticData::GetIconTexturePath(randoItemId),
                            .message = prefix,
                            .suffix = message,
                        });
                    }
                }
                Rando::GiveItem(randoItemId);
                CUSTOM_ITEM_PARAM = (s16)randoItemId;
                gSaveContext.save.shipSaveInfo.rando.archipelago.receivedItemCount++;
                Archipelago::Instance->isItemQueued = false;
            },
        .drawItem =
            [](Actor* actor, PlayState* play) {
                RandoItemId randoItemId = RI_UNKNOWN;

                if (CUSTOM_ITEM_FLAGS & CustomItem::CALLED_ACTION) {
                    randoItemId = (RandoItemId)CUSTOM_ITEM_PARAM;
                } else {
                    auto& item = Archipelago::Instance->incomingItems[CUSTOM_ITEM_PARAM];
                    randoItemId = Rando::ConvertItem(Archipelago::Instance->GetRandoItemIdFromNetworkItem(item));
                    RandoCheckId randoCheckId = static_cast<RandoCheckId>(item.location);
                    if (randoItemId == RI_JUNK && randoCheckId != RC_UNKNOWN &&
                        item.player == sAPClient->get_player_number()) {
                        randoItemId = Rando::CurrentJunkItem(randoCheckId);
                    }
                }

                Matrix_Scale(30.0f, 30.0f, 30.0f, MTXMODE_APPLY);
                Rando::DrawItem(randoItemId, (RandoCheckId)CUSTOM_ITEM_PARAM, actor);
            } });
}

void Archipelago::SendLocationCheck(RandoCheckId randoCheckId) {
    incomingCheckedLocations.insert(randoCheckId);
    sAPClient->LocationChecks({ randoCheckId });
}

bool Archipelago::IsCheckForSameGame(RandoCheckId checkId) {
    if (!sAPClient || !checkInfo.count(checkId)) {
        return false;
    }
    int player = checkInfo[checkId].player;
    return player == sAPClient->get_player_number() || sAPClient->get_player_game(player) == AP_GAME_NAME;
}

void Archipelago::GetArchipelagoItemInfo(RandoCheckId checkId, std::string& playerName, std::string& itemName) {
    if (checkInfo.count(checkId)) {
        auto networkItem = checkInfo[checkId];
        playerName = sAPClient->get_player_alias(networkItem.player);
        itemName = sAPClient->get_item_name(networkItem.item, sAPClient->get_player_game(networkItem.player));
    }
    if (playerName.empty()) {
        playerName = "Unknown Player";
    }
    if (itemName.empty()) {
        itemName = "Unknown Item";
    }
}
