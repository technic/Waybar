#pragma once

#include <fmt/core.h>
#include <giomm.h>
#include <glibmm/keyfile.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

#include "util/format.hpp"
#include "util/string.hpp"

/**
 * @file
 * Helper utilities for finding AppInfo of the running programs.
 * Application information is obtained by finding the corresponding .desktop file.
 */

namespace waybar::AppInfoProvider {

using AppInfo = Glib::RefPtr<Gio::DesktopAppInfo>;

// snap logic taken from
// https://gitlab.gnome.org/GNOME/mutter/-/blob/main/src/core/window.c#L867-L915
//
// BAMF_DESKTOP_FILE_HINT logic taken from
// https://invent.kde.org/plasma/plasma-workspace/-/blob/master/libnotificationmanager/utils.cpp#L82-99
//
// flatpak logic taken from
// https://gitlab.com/carbonOS/gde/gde-panel/-/blob/master/panel/appsys.vala#L59-L76
// https://invent.kde.org/plasma/plasma-workspace/-/blob/master/libnotificationmanager/utils.cpp#L69-80
//
// Some more logic is taken form Gnome shell
// https://github.com/GNOME/gnome-shell/blob/54f803dfee1c1896c97bdc79b289d6e742212dfe/src/shell-window-tracker.c#L132-L215

/**
 * @brief Find desktop file for process running in snap container
 *
 * Tries to read profile name from the security attributes of the process and find a .dekstop file
 * with the matching name
 * @param pid process id
 * @return info from the matched desktop file, null when not found
 */
AppInfo get_app_info_from_snap(uint32_t pid) {
  using namespace std::literals;

  // Example of the content for firefox snap on ubuntu 21.10:
  // snap.firefox.firefox (enforce)
  // We want to lookup:
  // firefox_firefox.desktop

  std::ifstream f{fmt::format("/proc/{}/attr/current", pid)};
  std::string   data;
  if (!std::getline(f, data)) {
    return {};
  };

  constexpr auto SNAP_PREFIX = "snap."sv;
  if (!starts_with(data, SNAP_PREFIX)) {
    return {};
  }
  data = data.substr(SNAP_PREFIX.size(), data.find(' ') - SNAP_PREFIX.size());
  std::replace(data.begin(), data.end(), '.', '_');

  return Gio::DesktopAppInfo::create(data + ".desktop");
}

/**
 * @brief Find desktop file for process based on BAMF_DESKTOP_FILE_HINT environment variable
 * @param pid process id
 * @return info from matched desktop file, null when not found
 */
AppInfo get_app_info_from_bamf_env(uint32_t pid) {
  using namespace std::literals;

  constexpr auto ENV_PREFIX = "BAMF_DESKTOP_FILE_HINT="sv;
  std::ifstream  f{fmt::format("/proc/{}/environ", pid)};
  std::string    data;
  while (std::getline(f, data, '\0')) {
    if (starts_with(data, ENV_PREFIX)) {
      data = data.substr(ENV_PREFIX.size());
      return Gio::DesktopAppInfo::create_from_filename(data);
    }
  }
  return {};
}

/**
 * @brief Find desktop file for process by looking for .flatpak-info file in the process's root
 * @param pid process id
 * @return info from matched desktop file, null when not found
 */
AppInfo get_app_info_from_flatpak(uint32_t pid) {
  namespace fs = std::filesystem;

  fs::path flatpakInfo{fmt::format("/proc/{}/root/.flatpak-info", pid)};
  try {
    if (!fs::is_regular_file(flatpakInfo)) {
      return {};
    }
  } catch (const std::exception& e) {
    spdlog::error("fs error: {}", e.what());
  }
  Glib::KeyFile keyFile;
  try {
    keyFile.load_from_file(flatpakInfo);
    auto appName = keyFile.get_string("Application", "name");
    return Gio::DesktopAppInfo::create(appName + ".desktop");
  } catch (const Glib::Error& e) {
    spdlog::error("Keyfile load failed {}", e.what());
    return {};
  }
}

/**
 * @brief wrapper for GAppInfoMonitor
 * Unfortunately not provided by giomm, see: https://gitlab.gnome.org/GNOME/glibmm/-/issues/97
 */
class AppInfoMonitor {
public:
    AppInfoMonitor() {
        gmonitor_ = g_app_info_monitor_get();
        if (gmonitor_ != nullptr) {
            g_signal_connect(gmonitor_, "changed", G_CALLBACK(handle_changed), this);
        }
    }

    sigc::signal<void> changed_signal;

    ~AppInfoMonitor() {
        if(gmonitor_ != nullptr){
            g_object_unref(gmonitor_);
        }
    }
private:
    static void handle_changed(GAppInfoMonitor* monitor, gpointer data) {
        static_cast<AppInfoMonitor*>(data)->emit_changed();
    }

    void emit_changed() {
        spdlog::info("AppInfo cache updated");
        changed_signal.emit();
    }

    GAppInfoMonitor* gmonitor_;
};

/**
 * @brief Keeps hashmap from StartupWMClass to AppInfo
 * 
 */
class AppInfoCacheService {
public:
    AppInfoCacheService() {
        load();
        monitor_.changed_signal.connect(sigc::mem_fun(*this, &AppInfoCacheService::load));
    }

    AppInfo lookup(const std::string& StartupWMClass) const {
        auto it = wm_class_hash_.find(StartupWMClass);
        if (it != wm_class_hash_.end()) {
            return it->second;
        }
        return {};
    }

    static const AppInfoCacheService& instance() {
        static AppInfoCacheService instance;
        return instance;
    }

private:
    void load() {
        wm_class_hash_.clear();
        auto all = Gio::DesktopAppInfo::get_all();
        for (const auto& info: all) {
            auto desktop_info = Gio::DesktopAppInfo::create(info->get_id());
            wm_class_hash_.emplace(desktop_info->get_startup_wm_class(), std::move(desktop_info));
        }
    }

    std::unordered_map<std::string, AppInfo> wm_class_hash_;
    AppInfoMonitor monitor_;
};

};  // namespace waybar::AppInfoProvider
