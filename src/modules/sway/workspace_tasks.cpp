#include "modules/sway/workspace_tasks.hpp"

#include <giomm/desktopappinfo.h>
#include <gtkmm/button.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <sigc++/sigc++.h>
#include <spdlog/spdlog.h>

#include "bar.hpp"
#include "modules/sway/ipc/client.hpp"
#include "util/appinfo_provider.hpp"
#include "util/json.hpp"

namespace waybar::modules::sway {

// Copied from wf-shell
namespace IconProvider {

using Icon = Glib::RefPtr<Gio::Icon>;

static Glib::RefPtr<Gdk::Pixbuf> load_icon_from_file(std::string icon_path, int size) {
  try {
    auto pb = Gdk::Pixbuf::create_from_file(icon_path, size, size);
    return pb;
  } catch (...) {
    return {};
  }
}

void set_image_icon(Gtk::Image &image, std::string icon_name, int size, int scale) {
  if (icon_name.find('/') == 0) {
    auto pbuf = load_icon_from_file(icon_name, size);
    image.set(pbuf);
    return;
  }

  auto icon_theme = Gtk::IconTheme::get_default();
  int  scaled_size = size * scale;

  if (!icon_theme->lookup_icon(icon_name, scaled_size)) {
    spdlog::error("Failed to load icon {}", icon_name);
    return;
  }

  auto pbuff = icon_theme->load_icon(icon_name, scaled_size)
                   ->scale_simple(scaled_size, scaled_size, Gdk::INTERP_BILINEAR);
  image.set(pbuff);
}

};  // namespace IconProvider

static void enableStyleClass(const Gtk::Widget &widget, const Glib::ustring &class_name,
                             bool enabled) {
  if (enabled) {
    widget.get_style_context()->add_class(class_name);
  } else {
    widget.get_style_context()->remove_class(class_name);
  }
}

struct WorkspaceTasksConfig {};

struct WindowProperties {
  std::string title;
  std::string app_id;
  std::string instance;
  uint32_t    pid;
  bool        focused;
  bool        urgent;
  bool        visible;

  static WindowProperties fromJson(const Json::Value &data) {
    std::string app_id;
    std::string instance;
    if (!data["app_id"].isNull()) {
      // Wayland
      app_id = data["app_id"].asString();
    } else {
      // XWayland
      app_id = data["window_properties"]["class"].asString();
      instance = data["window_properties"]["instance"].asString();
    }
    return {
        .title = data["name"].asString(),
        .app_id = std::move(app_id),
        .instance = std::move(instance),
        .pid = data["pid"].asUInt(),
        .focused = data["focused"].asBool(),
        .urgent = data["urgent"].asBool(),
        .visible = data["visible"].asBool(),
    };
  }
};

struct WorkspaceProperties {
  std::string title;
  int         num;
  bool        focused;
  bool        urgent;
  bool        visible;

  static WorkspaceProperties fromJson(const Json::Value &data) {
    return {
        .title = data["name"].asString(),
        .num = data["num"].asInt(),
        .focused = data["focused"].asBool(),
        .urgent = data["urgent"].asBool(),
        .visible = data["visible"].asBool(),
    };
  }
};

class WorkspaceTasksImpl {
 public:
  WorkspaceTasksImpl(const std::string &id, const Bar &bar, WorkspaceTasks &parent)
      : box_(bar.vertical ? Gtk::ORIENTATION_VERTICAL : Gtk::ORIENTATION_HORIZONTAL, 0),
        parent_(parent) {
    box_.set_name("workspace_tasks");
    if (!id.empty()) {
      box_.get_style_context()->add_class(id);
    }
    ipc_.subscribe(R"(["workspace","window"])");
    ipc_.signal_event.connect(sigc::mem_fun(*this, &WorkspaceTasksImpl::onEvent));
    ipc_.signal_cmd.connect(sigc::mem_fun(*this, &WorkspaceTasksImpl::onCmd));
    ipc_.setWorker([this] {
      try {
        ipc_.handleEvent();
      } catch (const std::exception &e) {
        spdlog::error("handleEvent: {}", e.what());
      }
    });
    getTree();
  }

  // Disable copy because *this is captured in signals
  WorkspaceTasksImpl(const WorkspaceTasksImpl &) = delete;
  WorkspaceTasksImpl &operator=(const WorkspaceTasksImpl &) = delete;

 public:
  void update() {
    std::lock_guard<std::mutex> lock{mutex_};
    spdlog::debug("update()");

    workspaces_.clear();
    buildTree(payload_);
    std::sort(workspaces_.begin(), workspaces_.end(), [](const auto &a, const auto &b) {
      return a->props.num < b->props.num;
    });

    for (auto &workspace : workspaces_) {
      workspace->update();
    }

    for (auto &workspace : workspaces_) {
      box_.pack_start(workspace->button, false, false, 0);
      workspace->button.signal_button_press_event().connect(
          [this, n = workspace->props.num](GdkEventButton *bt) {
            if (bt->type == GDK_BUTTON_PRESS && bt->button == 1) {
              activateWorkspace(n);
            }
            return true;
          },
          false);
      workspace->button.show();
    }
  }

  void cycleWorkspace(int delta) {
    auto it = std::find_if(
        workspaces_.begin(), workspaces_.end(), [](const auto &w) { return w->props.focused; });
    if (it != workspaces_.end()) {
      ssize_t i = it - workspaces_.begin();
      auto   &target = workspaces_[(i + delta) % workspaces_.size()];
      activateWorkspace(target->props.num);
    }
  }

  Gtk::Box &widget() { return box_; }

 private:
  struct Window {
    int32_t          id;
    std::string      resolved_app_id;
    WindowProperties props;
    Gtk::Button      gbutton;
    Gtk::Image       image;

    Window(int32_t id, WindowProperties props) : id(id), props(props) {
      update();
      gbutton.add(image);
      gbutton.set_image(image);
      gbutton.set_relief(Gtk::RELIEF_NONE);
      gbutton.show_all();
      gbutton.set_events(Gdk::BUTTON_PRESS_MASK);
    }

    void update() {
      enableStyleClass(gbutton, "focused", props.focused);
      enableStyleClass(gbutton, "urgent", props.urgent);
      gbutton.set_tooltip_text(props.title);

      constexpr int icon_size = 24;

      auto info = findInfo();
      if (info) {
        resolved_app_id = info->get_id();
      } else {
        resolved_app_id = props.app_id + ".desktop";
      }
      if (info) {
        auto icon = info->get_icon();
        if (icon) {
          IconProvider::set_image_icon(
              image, icon->to_string(), icon_size, gbutton.get_scale_factor());
          return;
        }
      }
      IconProvider::set_image_icon(
          image, "application-x-executable", icon_size, gbutton.get_scale_factor());
    }

    AppInfoProvider::AppInfo findInfo() {
      AppInfoProvider::AppInfo info;
      if (!props.instance.empty()) {
        // Most likely This is an x11 chrome window in the web app mode
        info = AppInfoProvider::AppInfoCacheService::instance().lookup(props.instance);
        if (info) {
          return info;
        }
      }

      info = Gio::DesktopAppInfo::create(props.app_id + ".desktop");
      if (info) {
        return info;
      }

      info = AppInfoProvider::get_app_info_from_bamf_env(props.pid);
      if (info) {
        return info;
      }

      info = AppInfoProvider::get_app_info_from_snap(props.pid);
      if (info) {
        return info;
      }

      info = AppInfoProvider::get_app_info_from_flatpak(props.pid);
      if (info) {
        return info;
      }

      std::string lower_app_id;
      std::transform(
          props.app_id.begin(), props.app_id.end(), std::back_inserter(lower_app_id), [](char c) {
            return std::tolower(c);
          });
      info = Gio::DesktopAppInfo::create(lower_app_id + ".desktop");

      return info;
    }

    std::string getResolvedAppId() const { return resolved_app_id; }
  };

  struct Workspace : public sigc::trackable {
    int32_t             id;
    WorkspaceProperties props;
    std::vector<Window> windows;
    Gtk::EventBox       button;
    Gtk::Box            content;
    Gtk::Label          name;

    Workspace(int32_t id, WorkspaceProperties props, Gtk::Orientation orientation)
        : id(id), props(props), content{orientation, 0} {
      content.set_name("workspace");
      content.add(name);
      update();
      button.add(content);
      button.show_all();

      button.set_events(Gdk::BUTTON_PRESS_MASK | Gdk::ENTER_NOTIFY_MASK | Gdk::LEAVE_NOTIFY_MASK);
      button.signal_enter_notify_event().connect(sigc::mem_fun(*this, &Workspace::enter));
      button.signal_leave_notify_event().connect(sigc::mem_fun(*this, &Workspace::leave));
    }

    // Disable copy because *this is captured in signals
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;

    template <typename F>
    void connect_clicked(F &&callback) {}

    bool enter(GdkEventCrossing *) {
      content.set_state_flags(Gtk::STATE_FLAG_PRELIGHT);
      return true;
    }

    bool leave(GdkEventCrossing *) {
      content.unset_state_flags(Gtk::STATE_FLAG_PRELIGHT);
      return true;
    }

    void update() {
      name.set_text(props.title);
      // When workspace is empty sway reports that it is focused,
      // otherwise one of the window can be focused. Same goes for visible.
      for (const auto &window : windows) {
        props.focused |= window.props.focused;
        props.visible |= window.props.visible;
      }
      enableStyleClass(content, "focused", props.focused);
      enableStyleClass(content, "visible", !props.focused && props.visible);
      // Urgent status sway reports both for workspace and for window
      enableStyleClass(content, "urgent", props.urgent);
    }

    void removeWindow(int32_t id) {
      std::remove_if(windows.begin(), windows.end(), [id](auto &w) { return w.id == id; });
    }

    void addWindow(int32_t id, WindowProperties props, WorkspaceTasksImpl *ptr) {
      windows.emplace_back(id, props);
      bool repeated = std::any_of(windows.begin(), windows.end() - 1, [&](auto &x) {
        return x.getResolvedAppId() == windows.back().getResolvedAppId();
      });

      if (!repeated) {
        content.pack_end(windows.back().gbutton);
        windows.back().gbutton.signal_button_press_event().connect(
            [ptr, id](GdkEventButton *bt) {
              if (bt->type == GDK_BUTTON_PRESS && bt->button == 1) {
                ptr->activateWindow(id);
                return true;
              }
              return false;
            },
            false);
        windows.back().gbutton.signal_enter_notify_event().connect(
            sigc::mem_fun(*this, &Workspace::enter));
        windows.back().gbutton.signal_leave_notify_event().connect(
            sigc::mem_fun(*this, &Workspace::leave));
      }
    }
  };

  void onEvent(const struct Ipc::ipc_response &res) {
    // FIXME: a bit inneficient
    getTree();
  }

  void getTree() {
    try {
      ipc_.sendCmd(IPC_GET_TREE);
    } catch (const std::exception &e) {
      spdlog::error("WorkspaceTasks: {}", e.what());
    }
  }

  void onCmd(const struct Ipc::ipc_response &res) {
    if (res.type == IPC_GET_TREE) {
      try {
        std::lock_guard<std::mutex> lock{mutex_};
        payload_ = parser_.parse(res.payload);
      } catch (const std::exception &e) {
        spdlog::error("WorkspaceTasks: {}");
        return;
      }
      parent_.dp.emit();
    }
  }

  void buildTree(const Json::Value &node) {
    auto    t = node["type"];
    int32_t id = node["id"].asInt();

    if (t == "root" || t == "output") {
      // pass to recursion
    } else if (t == "workspace") {
      auto props = WorkspaceProperties::fromJson(node);
      if (std::string_view{props.title}.substr(0, 4) == "__i3") {
        return;
      }
      workspaces_.push_back(std::make_unique<Workspace>(id, props, box_.get_orientation()));
    } else if (t == "con" || t == "floating_con") {
      if (node.isMember("app_id")) {
        if (workspaces_.size() > 0) {
          workspaces_.back()->addWindow(id, WindowProperties::fromJson(node), this);
        } else {
          spdlog::error("Encounted window before workspace");
        }
        return;
      }
    } else {
      spdlog::warn("Unknown type {}", t.asString());
      return;
    }

    // recurse
    for (const auto &child : node["nodes"]) {
      buildTree(child);
    }
    for (const auto &child : node["floating_nodes"]) {
      buildTree(child);
    }
  }

  void activateWorkspace(int num) {
    spdlog::debug("Activate workspace {}", num);
    try {
      ipc_.sendCmd(IPC_COMMAND, fmt::format("workspace number {}", num));
    } catch (const std::exception &e) {
      spdlog::error("WorkspaceTasks: {}", e.what());
    }
  }

  void activateWindow(int32_t id) {
    spdlog::debug("Activate window {}", id);
    try {
      ipc_.sendCmd(IPC_COMMAND, fmt::format("[con_id={}] focus", id));
    } catch (const std::exception &e) {
      spdlog::error("WorkspaceTasks: {}", e.what());
    }
  }

  Gtk::Box                                box_;
  WorkspaceTasks                         &parent_;
  Ipc                                     ipc_;
  util::JsonParser                        parser_;
  std::mutex                              mutex_;
  std::vector<std::unique_ptr<Workspace>> workspaces_;
  Json::Value                             payload_;
};

WorkspaceTasks::WorkspaceTasks(const std::string &id, const waybar::Bar &bar,
                               const Json::Value &config)
    : AModule(config, "workspace_tasks", id, true, true),
      impl_(std::make_unique<WorkspaceTasksImpl>(id, bar, *this)) {
  event_box_.add(impl_->widget());
}

void WorkspaceTasks::update() {
  impl_->update();
  AModule::update();
}

bool WorkspaceTasks::handleScroll(GdkEventScroll *e) {
  auto direction = AModule::getScrollDir(e);
  switch (direction) {
    case SCROLL_DIR::DOWN:
    case SCROLL_DIR::RIGHT:
      impl_->cycleWorkspace(1);
      break;
    case SCROLL_DIR::UP:
    case SCROLL_DIR::LEFT:
      impl_->cycleWorkspace(-1);
    default:
      break;
  }
  return true;
}

}  // namespace waybar::modules::sway
