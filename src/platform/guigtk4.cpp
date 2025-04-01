//-----------------------------------------------------------------------------
//
// Copyright 2018 whitequark
//-----------------------------------------------------------------------------
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <json-c/json_object.h>
#include <json-c/json_util.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#include "config.h"

#if defined(HAVE_SPACEWARE)
#   include <spnav.h>
#   include <gdk/gdk.h>
#   if defined(GDK_WINDOWING_X11)
#       include <gdk/gdkx.h>
#   endif
#   if defined(GDK_WINDOWING_WAYLAND)
#       include <gdk/gdkwayland.h>
#   endif
#   if GTK_CHECK_VERSION(3, 20, 0)
#       include <gdkmm/seat.h>
#   else
#       include <gdkmm/devicemanager.h>
#   endif
#endif

#include "solvespace.h"

namespace SolveSpace {
namespace Platform {

//-----------------------------------------------------------------------------
// Utility functions
//-----------------------------------------------------------------------------

static std::string PrepareMnemonics(std::string label) {
    std::replace(label.begin(), label.end(), '&', '_');
    return label;
}

static std::string PrepareTitle(const std::string &title) {
    return title + " — SolveSpace";
}

//-----------------------------------------------------------------------------
// Fatal errors
//-----------------------------------------------------------------------------

void FatalError(const std::string &message) {
    fprintf(stderr, "%s", message.c_str());
    abort();
}

//-----------------------------------------------------------------------------
// Settings
//-----------------------------------------------------------------------------

class SettingsImplGtk final : public Settings {
public:
    // Why aren't we using GSettings? Two reasons. It doesn't allow to easily see whether
    // the setting had the default value, and it requires to install a schema globally.
    Path         _path;
    json_object *_json = NULL;

    static Path GetConfigPath() {
        Path configHome;
        if(getenv("XDG_CONFIG_HOME")) {
            configHome = Path::From(getenv("XDG_CONFIG_HOME"));
        } else if(getenv("HOME")) {
            configHome = Path::From(getenv("HOME")).Join(".config");
        } else {
            dbp("neither XDG_CONFIG_HOME nor HOME are set");
            return Path::From("");
        }
        if(!configHome.IsEmpty()) {
            configHome = configHome.Join("solvespace");
        }

        const char *configHomeC = configHome.raw.c_str();
        struct stat st;
        if(stat(configHomeC, &st)) {
            if(errno == ENOENT) {
                if(mkdir(configHomeC, 0777)) {
                    dbp("cannot mkdir %s: %s", configHomeC, strerror(errno));
                    return Path::From("");
                }
            } else {
                dbp("cannot stat %s: %s", configHomeC, strerror(errno));
                return Path::From("");
            }
        } else if(!S_ISDIR(st.st_mode)) {
            dbp("%s is not a directory", configHomeC);
            return Path::From("");
        }

        return configHome.Join("settings.json");
    }

    SettingsImplGtk() {
        _path = GetConfigPath();
        if(_path.IsEmpty()) {
            dbp("settings will not be saved");
        } else {
            _json = json_object_from_file(_path.raw.c_str());
            if(!_json && errno != ENOENT) {
                dbp("cannot load settings: %s", strerror(errno));
            }
        }

        if(_json == NULL) {
            _json = json_object_new_object();
        }
    }

    ~SettingsImplGtk() override {
        if(!_path.IsEmpty()) {
            // json-c <0.12 has the first argument non-const
            if(json_object_to_file_ext((char *)_path.raw.c_str(), _json,
                                       JSON_C_TO_STRING_PRETTY)) {
                dbp("cannot save settings: %s", strerror(errno));
            }
        }

        json_object_put(_json);
    }

    void FreezeInt(const std::string &key, uint32_t value) override {
        struct json_object *jsonValue = json_object_new_int(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    uint32_t ThawInt(const std::string &key, uint32_t defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_int(jsonValue);
        }
        return defaultValue;
    }

    void FreezeBool(const std::string &key, bool value) override {
        struct json_object *jsonValue = json_object_new_boolean(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    bool ThawBool(const std::string &key, bool defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_boolean(jsonValue);
        }
        return defaultValue;
    }

    void FreezeFloat(const std::string &key, double value) override {
        struct json_object *jsonValue = json_object_new_double(value);
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    double ThawFloat(const std::string &key, double defaultValue) override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_double(jsonValue);
        }
        return defaultValue;
    }

    void FreezeString(const std::string &key, const std::string &value) override {
        struct json_object *jsonValue = json_object_new_string(value.c_str());
        json_object_object_add(_json, key.c_str(), jsonValue);
    }

    std::string ThawString(const std::string &key,
                           const std::string &defaultValue = "") override {
        struct json_object *jsonValue;
        if(json_object_object_get_ex(_json, key.c_str(), &jsonValue)) {
            return json_object_get_string(jsonValue);
        }
        return defaultValue;
    }
};

SettingsRef GetSettings() {
    static std::shared_ptr<SettingsImplGtk> settings;
    if(!settings) {
        settings = std::make_shared<SettingsImplGtk>();
    }
    return settings;
}

//-----------------------------------------------------------------------------
// Timers
//-----------------------------------------------------------------------------

class TimerImplGtk final : public Timer {
public:
    sigc::connection    _connection;

    void RunAfter(unsigned milliseconds) override {
        if(!_connection.empty()) {
            _connection.disconnect();
        }

        auto handler = [this]() {
            if(this->onTimeout) {
                this->onTimeout();
            }
            return false;
        };
        // Note: asan warnings about new-delete-type-mismatch are false positives here:
        // https://gitlab.gnome.org/GNOME/gtkmm/-/issues/65
        // Pass new_delete_type_mismatch=0 to ASAN_OPTIONS to disable those warnings.
        // Unfortunately they won't go away until upgrading to gtkmm4
        _connection = Glib::signal_timeout().connect(handler, milliseconds);
    }
};

TimerRef CreateTimer() {
    return std::make_shared<TimerImplGtk>();
}

//-----------------------------------------------------------------------------
// GTK menu extensions
//-----------------------------------------------------------------------------

class GtkMenuItem {
public:
    Platform::MenuItem *_receiver;
    sigc::connection    _onActivateConnection;
    std::string         _label;
    std::string         _action_name;
    bool                _has_indicator;
    bool                _is_active;
    bool                _draw_as_radio;
    Glib::RefPtr<Gio::MenuItem> _menuItem;
    Glib::RefPtr<Gio::SimpleAction> _action;
    
    GtkMenuItem(Platform::MenuItem *receiver, Gio::Menu *menu, Glib::RefPtr<Gio::SimpleActionGroup> actionGroup) :
        _receiver(receiver), _has_indicator(false), _is_active(false), _draw_as_radio(false) {
        
        _action_name = "action_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        
        _action = Gio::SimpleAction::create(_action_name);
        
        _onActivateConnection = _action->signal_activate().connect(
            sigc::mem_fun(*this, &GtkMenuItem::on_activate));
        
        actionGroup->add_action(_action);
        
        _menuItem = Gio::MenuItem::create();
    }
    
    void set_label(const std::string &label) {
        _label = label;
        _menuItem->set_label(PrepareMnemonics(label));
    }
    
    void set_action(const std::string &action) {
        _menuItem->set_action_and_target_value(action, Glib::Variant<Glib::ustring>::create(_action_name));
    }
    
    void add_to_menu(Gio::Menu *menu) {
        _menuItem->set_detailed_action("menu." + _action_name);
        menu->append_item(_menuItem);
    }
    
    void set_accel_key(const Gtk::AccelKey &accel_key) {
        if (_menuItem) {
            guint key = accel_key.get_key();
            Gdk::ModifierType mods = accel_key.get_mod();
            
            std::string accelString;
            if ((mods & Gdk::ModifierType::CONTROL_MASK) != 0) accelString += "<Control>";
            if ((mods & Gdk::ModifierType::SHIFT_MASK) != 0) accelString += "<Shift>";
            if ((mods & Gdk::ModifierType::ALT_MASK) != 0) accelString += "<Alt>";
            
            if (key >= GDK_KEY_F1 && key <= GDK_KEY_F12) {
                accelString += "F" + std::to_string(key - GDK_KEY_F1 + 1);
            } else if (key == GDK_KEY_Tab) {
                accelString += "Tab";
            } else if (key == GDK_KEY_Escape) {
                accelString += "Escape";
            } else if (key == GDK_KEY_Delete) {
                accelString += "Delete";
            } else {
                char c = gdk_keyval_to_unicode(key);
                if (c) accelString += c;
            }
            
            if (!accelString.empty()) {
                _menuItem->set_attribute_value("accel", 
                    Glib::Variant<std::string>::create(accelString));
            }
        }
    }
    
    bool has_indicator() const {
        return _has_indicator;
    }
    
    void set_has_indicator(bool has_indicator) {
        _has_indicator = has_indicator;
        
        if (has_indicator && _action) {
            auto statefulAction = Gio::SimpleAction::create_bool(_action_name, _is_active);
            
            _onActivateConnection.disconnect();
            _onActivateConnection = statefulAction->signal_change_state().connect(
                [this](const Glib::VariantBase& value) {
                    auto state = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(value);
                    _is_active = state.get();
                    _action->set_state(state);
                    if (_receiver && _receiver->onTrigger) {
                        _receiver->onTrigger();
                    }
                });
            
            _action = statefulAction;
        }
    }
    
    void set_draw_as_radio(bool draw_as_radio) {
        _draw_as_radio = draw_as_radio;
        
        if (_has_indicator && _menuItem) {
            if (draw_as_radio) {
                _menuItem->set_attribute_value("icon", 
                    Glib::Variant<std::string>::create("radio-checked-symbolic"));
            }
        }
    }
    
    bool get_draw_as_radio() const {
        return _draw_as_radio;
    }
    
    void set_active(bool active) {
        if(_is_active == active)
            return;
        
        _is_active = active;
        
        if(_has_indicator && _action) {
            _action->change_state(Glib::Variant<bool>::create(active));
        }
    }
    
    void set_sensitive(bool sensitive) {
        if(_action) {
            _action->set_enabled(sensitive);
        }
    }
    
protected:
    void on_activate(const Glib::VariantBase& parameter) {
        if(_receiver && _receiver->onTrigger) {
            _receiver->onTrigger();
        }
    }
};

//-----------------------------------------------------------------------------
// Menus
//-----------------------------------------------------------------------------

class MenuItemImplGtk final : public MenuItem {
public:
    std::shared_ptr<GtkMenuItem> gtkMenuItem;
    std::function<void()> onTrigger;
    GSimpleAction *action;
    Indicator indicatorType = Indicator::NONE;
    bool isActive = false;

    MenuItemImplGtk() {}

    void SetAccelerator(KeyboardEvent accel) override {
        guint accelKey = 0;
        if(accel.key == KeyboardEvent::Key::CHARACTER) {
            if(accel.chr == '\t') {
                accelKey = GDK_KEY_Tab;
            } else if(accel.chr == '\x1b') {
                accelKey = GDK_KEY_Escape;
            } else if(accel.chr == '\x7f') {
                accelKey = GDK_KEY_Delete;
            } else {
                accelKey = gdk_unicode_to_keyval(accel.chr);
            }
        } else if(accel.key == KeyboardEvent::Key::FUNCTION) {
            accelKey = GDK_KEY_F1 + accel.num - 1;
        }

        Gdk::ModifierType accelMods = {};
        if(accel.shiftDown) {
            accelMods |= Gdk::SHIFT_MASK;
        }
        if(accel.controlDown) {
            accelMods |= Gdk::CONTROL_MASK;
        }

        if (gtkMenuItem && action) {
            std::string accelString;
            if (accel.controlDown) accelString += "<Control>";
            if (accel.shiftDown) accelString += "<Shift>";
            
            char keyChar = accel.chr;
            if (accel.key == KeyboardEvent::Key::FUNCTION) {
                accelString += "F" + std::to_string(accel.num);
            } else if (keyChar == '\t') {
                accelString += "Tab";
            } else if (keyChar == '\x1b') {
                accelString += "Escape";
            } else if (keyChar == '\x7f') {
                accelString += "Delete";
            } else {
                accelString += keyChar;
            }
            
            if (gtkMenuItem->_menuItem) {
                gtkMenuItem->_menuItem->set_attribute_value("accel", 
                    Glib::Variant<std::string>::create(accelString));
            }
        }
    }

    void SetIndicator(Indicator type) override {
        indicatorType = type;
        
        if (gtkMenuItem) {
            switch(type) {
                case Indicator::NONE:
                    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), false);
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), false);
                    break;

                case Indicator::CHECK_MARK:
                    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), false);
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), isActive);
                    break;

                case Indicator::RADIO_MARK:
                    gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), true);
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), isActive);
                    break;
            }
        }
    }

    void SetActive(bool active) override {
        isActive = active;
        
        if (gtkMenuItem && indicatorType != Indicator::NONE) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(gtkMenuItem->gtkWidget), active);
        }
    }

    void SetEnabled(bool enabled) override {
        if (action) {
            g_simple_action_set_enabled(action, enabled);
        }
    }
};

class MenuImplGtk final : public Menu {
public:
    GMenu *menuModel;
    GSimpleActionGroup *actionGroup;
    std::vector<std::shared_ptr<MenuItemImplGtk>> menuItems;
    std::vector<std::shared_ptr<MenuImplGtk>> subMenus;
    GtkPopoverMenu* popoverMenu;
    
    MenuImplGtk() {
        menuModel = g_menu_new();
        actionGroup = g_simple_action_group_new();
        popoverMenu = nullptr;
    }
    
    void SetPopoverMenu(GtkPopoverMenu* menu) {
        popoverMenu = menu;
        if (popoverMenu) {
            gtk_popover_menu_set_menu_model(popoverMenu, G_MENU_MODEL(menuModel));
            gtk_widget_insert_action_group(GTK_WIDGET(popoverMenu), "menu", G_ACTION_GROUP(actionGroup));
        }
    }

    MenuItemRef AddItem(const std::string &label,
                        std::function<void()> onTrigger = NULL,
                        bool mnemonics = true) override {
        auto menuItem = std::make_shared<MenuItemImplGtk>();
        menuItems.push_back(menuItem);
        
        std::string actionName = "action_" + std::to_string(reinterpret_cast<uintptr_t>(menuItem.get()));
        
        GSimpleAction *action = g_simple_action_new(actionName.c_str(), NULL);
        
        g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, void* user_data) {
            auto menuItem = static_cast<MenuItemImplGtk*>(user_data);
            if (menuItem->onTrigger) {
                menuItem->onTrigger();
            }
        }), menuItem.get());
        
        g_action_map_add_action(G_ACTION_MAP(actionGroup), G_ACTION(action));
        
        GMenuItem *item = g_menu_item_new(
            mnemonics ? PrepareMnemonics(label).c_str() : label.c_str(), 
            ("menu." + actionName).c_str());
        g_menu_append_item(menuModel, item);
        g_object_unref(item);
        
        menuItem->action = action;
        
        return menuItem;
    }

    MenuRef AddSubMenu(const std::string &label) override {
        auto subMenu = std::make_shared<MenuImplGtk>();
        subMenus.push_back(subMenu);
        
        auto subMenuModel = Gio::Menu::create();
        menuModel->append_submenu(PrepareMnemonics(label), subMenuModel);
        
        subMenu->menuModel = subMenuModel;
        subMenu->actionGroup = actionGroup;
        
        return subMenu;
    }

    void AddSeparator() override {
        menuModel->append_section("", Gio::Menu::create());
    }

    void PopUp() override {
        if (!popoverMenu) {
            auto tempPopover = Gtk::make_managed<Gtk::PopoverMenu>();
            tempPopover->set_menu_model(menuModel);
            tempPopover->insert_action_group("menu", actionGroup);
            tempPopover->set_has_arrow(false);
            tempPopover->popup();
        } else {
            popoverMenu->popup();
        }
    }

    void Clear() override {
        while(menuModel->get_n_items() > 0) {
            menuModel->remove(0);
        }
        menuItems.clear();
        subMenus.clear();
    }
};

MenuRef CreateMenu() {
    return std::make_shared<MenuImplGtk>();
}

class MenuBarImplGtk final : public MenuBar {
public:
    GtkWidget *gtkMenuBar;
    GMenu *menuModel;
    GSimpleActionGroup *actionGroup;
    std::vector<std::shared_ptr<MenuImplGtk>> subMenus;

    MenuBarImplGtk() {
        menuModel = g_menu_new();
        actionGroup = g_simple_action_group_new();
        gtkMenuBar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menuModel));
        gtk_widget_insert_action_group(gtkMenuBar, "menubar", G_ACTION_GROUP(actionGroup));
    }

    MenuRef AddSubMenu(const std::string &label) override {
        auto subMenu = std::make_shared<MenuImplGtk>();
        subMenus.push_back(subMenu);
        
        GMenu *subMenuModel = g_menu_new();
        g_menu_append_submenu(menuModel, PrepareMnemonics(label).c_str(), G_MENU_MODEL(subMenuModel));
        
        subMenu->menuModel = subMenuModel;
        subMenu->actionGroup = actionGroup;
        
        return subMenu;
    }

    void Clear() override {
        while(g_menu_model_get_n_items(G_MENU_MODEL(menuModel)) > 0) {
            g_menu_remove(menuModel, 0);
        }
        subMenus.clear();
    }
};

MenuBarRef GetOrCreateMainMenu(bool *unique) {
    *unique = false;
    return std::make_shared<MenuBarImplGtk>();
}

//-----------------------------------------------------------------------------
// GTK GL and window extensions
//-----------------------------------------------------------------------------

class GtkGLWidget {
    Window *_receiver;
    GtkWidget *_widget;
    
    GtkEventController *_motion_controller;
    GtkGesture *_click_controller;
    GtkEventController *_scroll_controller;
    GtkEventController *_key_controller;

public:
    GtkGLWidget(Platform::Window *receiver) : _receiver(receiver) {
        _widget = gtk_gl_area_new();
        gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(_widget), TRUE);
        gtk_widget_set_can_focus(_widget, TRUE);
        gtk_gl_area_set_has_alpha(GTK_GL_AREA(_widget), TRUE);
        
        _motion_controller = gtk_event_controller_motion_new();
        g_signal_connect(_motion_controller, "motion", G_CALLBACK(+[](GtkEventControllerMotion*, double x, double y, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            self->on_motion(x, y);
        }), this);
        g_signal_connect(_motion_controller, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            self->on_leave();
        }), this);
        gtk_widget_add_controller(_widget, _motion_controller);
        
        _click_controller = gtk_gesture_click_new();
        g_signal_connect(_click_controller, "pressed", G_CALLBACK(+[](GtkGestureClick*, int n_press, double x, double y, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            self->on_button_press(n_press, x, y);
        }), this);
        g_signal_connect(_click_controller, "released", G_CALLBACK(+[](GtkGestureClick*, int n_press, double x, double y, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            self->on_button_release(n_press, x, y);
        }), this);
        gtk_widget_add_controller(_widget, GTK_EVENT_CONTROLLER(_click_controller));
        
        _scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(_scroll_controller, "scroll", G_CALLBACK(+[](GtkEventControllerScroll*, double dx, double dy, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            return self->on_scroll(dx, dy);
        }), this);
        gtk_widget_add_controller(_widget, _scroll_controller);
        
        _key_controller = gtk_event_controller_key_new();
        g_signal_connect(_key_controller, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            return self->on_key_press(keyval, keycode, state);
        }), this);
        g_signal_connect(_key_controller, "key-released", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            return self->on_key_release(keyval, keycode, state);
        }), this);
        gtk_widget_add_controller(_widget, _key_controller);
        
        g_signal_connect(_widget, "realize", G_CALLBACK(+[](GtkWidget* widget, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            self->on_realize();
        }), this);
        
        g_signal_connect(_widget, "render", G_CALLBACK(+[](GtkGLArea* area, GdkGLContext* context, gpointer user_data) {
            auto self = static_cast<GtkGLWidget*>(user_data);
            return self->on_render(context);
        }), this);
    }

protected:
    void on_realize() {
        gtk_gl_area_make_current(GTK_GL_AREA(_widget));
    }

    bool on_render(GdkGLContext *context) {
        if(_receiver->onRender) {
            _receiver->onRender();
        }
        return true;
    }

    bool process_pointer_event(MouseEvent::Type type, double x, double y,
                               guint state, guint button = 0, double scroll_delta = 0) {
        MouseEvent event = {};
        event.type = type;
        event.x = x;
        event.y = y;
        if(button == 1 || (state & GDK_BUTTON1_MASK) != 0) {
            event.button = MouseEvent::Button::LEFT;
        } else if(button == 2 || (state & GDK_BUTTON2_MASK) != 0) {
            event.button = MouseEvent::Button::MIDDLE;
        } else if(button == 3 || (state & GDK_BUTTON3_MASK) != 0) {
            event.button = MouseEvent::Button::RIGHT;
        }
        if((state & GDK_SHIFT_MASK) != 0) {
            event.shiftDown = true;
        }
        if((state & GDK_CONTROL_MASK) != 0) {
            event.controlDown = true;
        }
        if(scroll_delta != 0) {
            event.scrollDelta = scroll_delta;
        }

        if(_receiver->onMouseEvent) {
            return _receiver->onMouseEvent(event);
        }

        return false;
    }

    void on_motion(double x, double y) {
        auto controller = _motion_controller->get_current_event_state();
        process_pointer_event(MouseEvent::Type::MOTION, x, y, controller);
    }
    
    void on_leave() {
    }
    
    void on_button_press(int n_press, double x, double y) {
        MouseEvent::Type type = (n_press > 1) ? MouseEvent::Type::DBL_PRESS : MouseEvent::Type::PRESS;
        auto button = _click_controller->get_current_button();
        auto controller = _click_controller->get_current_event_state();
        process_pointer_event(type, x, y, controller, button);
    }
    
    void on_button_release(int n_press, double x, double y) {
        auto button = _click_controller->get_current_button();
        auto controller = _click_controller->get_current_event_state();
        process_pointer_event(MouseEvent::Type::RELEASE, x, y, controller, button);
    }
    
    bool on_scroll(double dx, double dy) {
        auto controller = _scroll_controller->get_current_event_state();
        return process_pointer_event(MouseEvent::Type::SCROLL, 0, 0, controller, 0, dy);
    }
    
    bool on_key_press(guint keyval, guint keycode, Gdk::ModifierType state) {
        KeyboardEvent event = {};
        event.type = KeyboardEvent::Type::PRESS;
        event.key = keyval;
        if((state & Gdk::ModifierType::SHIFT_MASK) != 0) {
            event.shiftDown = true;
        }
        if((state & Gdk::ModifierType::CONTROL_MASK) != 0) {
            event.controlDown = true;
        }
        
        if(_receiver->onKeyboardEvent) {
            return _receiver->onKeyboardEvent(event);
        }
        return false;
    }
    
    bool on_key_release(guint keyval, guint keycode, Gdk::ModifierType state) {
        KeyboardEvent event = {};
        event.type = KeyboardEvent::Type::RELEASE;
        event.key = keyval;
        if((state & Gdk::ModifierType::SHIFT_MASK) != 0) {
            event.shiftDown = true;
        }
        if((state & Gdk::ModifierType::CONTROL_MASK) != 0) {
            event.controlDown = true;
        }
        
        if(_receiver->onKeyboardEvent) {
            return _receiver->onKeyboardEvent(event);
        }
        return false;
    }

    bool on_scroll_event(GdkEventScroll *gdk_event) override {
        double dx, dy;
        GdkScrollDirection dir;
        double delta;

// for gtk4 ??
//        gdk_scroll_event_get_deltas((GdkEvent*)gdk_event, &dx, &dy);
//        gdk_scroll_event_get_direction((GdkEvent*)gdk_event, &dir);

        if(gdk_event_get_scroll_deltas((GdkEvent*)gdk_event, &dx, &dy)) {
            delta = -dy;
        } else if(gdk_event_get_scroll_direction((GdkEvent*)gdk_event, &dir)) {
            if(dir == GDK_SCROLL_UP) {
                delta = 1;
            } else if(dir == GDK_SCROLL_DOWN) {
                delta = -1;
            } else {
                return false;
            }
        } else {
            return false;
        }

        double x,y;
        gdk_event_get_coords((GdkEvent*)gdk_event, &x, &y);
        GdkModifierType state;
        gdk_event_get_state((GdkEvent*)gdk_event, &state);

        if(process_pointer_event(MouseEvent::Type::SCROLL_VERT,
                                 x, y, state, 0, delta))
            return true;

        return Gtk::GLArea::on_scroll_event(gdk_event);
    }

    bool on_leave_notify_event(GdkEventCrossing *gdk_event) override {
        double x,y;
        gdk_event_get_coords((GdkEvent*)gdk_event, &x, &y);
        GdkModifierType state;
        gdk_event_get_state((GdkEvent*)gdk_event, &state);
        
        if(process_pointer_event(MouseEvent::Type::LEAVE, x, y, state))
            return true;

        return Gtk::GLArea::on_leave_notify_event(gdk_event);
    }

    bool process_key_event(KeyboardEvent::Type type, GdkEventKey *gdk_event) {
        KeyboardEvent event = {};
        event.type = type;

        GdkModifierType state;
        gdk_event_get_state((GdkEvent*)gdk_event, &state);

        Gdk::ModifierType mod_mask = get_modifier_mask(Gdk::MODIFIER_INTENT_DEFAULT_MOD_MASK);
        if((state & mod_mask) & ~(GDK_SHIFT_MASK|GDK_CONTROL_MASK)) {
            return false;
        }

        event.shiftDown   = (state & GDK_SHIFT_MASK)   != 0;
        event.controlDown = (state & GDK_CONTROL_MASK) != 0;
        
        guint keyval;
        gdk_event_get_keyval((GdkEvent*)gdk_event, &keyval);

        char32_t chr = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
        if(chr != 0) {
            event.key = KeyboardEvent::Key::CHARACTER;
            event.chr = chr;
        } else if(keyval >= GDK_KEY_F1 &&
                  keyval <= GDK_KEY_F12) {
            event.key = KeyboardEvent::Key::FUNCTION;
            event.num = keyval - GDK_KEY_F1 + 1;
        } else {
            return false;
        }

        if(_receiver->onKeyboardEvent) {
            return _receiver->onKeyboardEvent(event);
        }

        return false;
    }

    bool on_key_press_event(GdkEventKey *gdk_event) override {
        if(process_key_event(KeyboardEvent::Type::PRESS, gdk_event))
            return true;

        return Gtk::GLArea::on_key_press_event(gdk_event);
    }

    bool on_key_release_event(GdkEventKey *gdk_event) override {
        if(process_key_event(KeyboardEvent::Type::RELEASE, gdk_event))
            return true;

        return Gtk::GLArea::on_key_release_event(gdk_event);
    }
};

class GtkEditorOverlay {
    Window      *_receiver;
    GtkGLWidget _gl_widget;
    GtkWidget   *_widget;
    GtkWidget   *_entry;

public:
    GtkEditorOverlay(Platform::Window *receiver) : _receiver(receiver), _gl_widget(receiver) {
        _widget = gtk_fixed_new();
        gtk_fixed_put(GTK_FIXED(_widget), _gl_widget._widget, 0, 0);

        _entry = gtk_entry_new();
        gtk_widget_set_visible(_entry, FALSE);
        gtk_entry_set_has_frame(GTK_ENTRY(_entry), FALSE);
        gtk_fixed_put(GTK_FIXED(_widget), _entry, 0, 0);

        g_signal_connect(_entry, "activate", G_CALLBACK(+[](GtkEntry* entry, gpointer user_data) {
            auto self = static_cast<GtkEditorOverlay*>(user_data);
            self->on_activate();
        }), this);
    }

    bool is_editing() const {
        return gtk_widget_get_visible(_entry);
    }

    void start_editing(int x, int y, int font_height, int min_width, bool is_monospace,
                       const std::string &val) {
        PangoFontDescription *font_desc = pango_font_description_new();
        pango_font_description_set_family(font_desc, is_monospace ? "monospace" : "normal");
        pango_font_description_set_absolute_size(font_desc, font_height * PANGO_SCALE);
        gtk_widget_override_font(_entry, font_desc);

        // The y coordinate denotes baseline.
        PangoContext *pango_context = gtk_widget_get_pango_context(_entry);
        PangoFontMetrics *font_metrics = pango_context_get_metrics(pango_context, font_desc, NULL);
        int ascent = pango_font_metrics_get_ascent(font_metrics) / PANGO_SCALE;
        int descent = pango_font_metrics_get_descent(font_metrics) / PANGO_SCALE;
        y -= ascent;

        PangoLayout *layout = pango_layout_new(pango_context);
        pango_layout_set_font_description(layout, font_desc);
        // Add one extra char width to avoid scrolling.
        pango_layout_set_text(layout, (val + " ").c_str(), -1);
        PangoRectangle rect;
        pango_layout_get_extents(layout, NULL, &rect);
        int width = rect.width;
        g_object_unref(layout);

        GtkStyleContext *style_context = gtk_widget_get_style_context(_entry);
        GtkBorder margin, border, padding;
        gtk_style_context_get_margin(style_context, GTK_STATE_FLAG_NORMAL, &margin);
        gtk_style_context_get_border(style_context, GTK_STATE_FLAG_NORMAL, &border);
        gtk_style_context_get_padding(style_context, GTK_STATE_FLAG_NORMAL, &padding);
        gtk_fixed_move(GTK_FIXED(_widget), _entry,
             x - margin.left - border.left - padding.left,
             y - margin.top  - border.top  - padding.top);

        int fitWidth = width / PANGO_SCALE + padding.left + padding.right;
        gtk_widget_set_size_request(_entry, std::max(fitWidth, min_width), -1);
        gtk_widget_queue_resize(_widget);

        gtk_entry_set_text(GTK_ENTRY(_entry), val.c_str());

        if(!gtk_widget_get_visible(_entry)) {
            gtk_widget_set_visible(_entry, TRUE);
            gtk_widget_grab_focus(_entry);

            // We grab the input for ourselves and not the entry to still have
            // the pointer events go through the underlay.
            gtk_grab_add(_widget);
        }
    }

    void stop_editing() {
        if(gtk_widget_get_visible(_entry)) {
            gtk_grab_remove(_widget);
            gtk_widget_set_visible(_entry, FALSE);
            gtk_widget_grab_focus(_gl_widget._widget);
        }
    }

    GtkWidget *get_widget() {
        return _widget;
    }

    GtkGLWidget &get_gl_widget() {
        return _gl_widget;
    }

private:
    void on_activate() {
        if(_receiver->onEditingDone) {
            _receiver->onEditingDone(true);
        }
    }
};

class GtkWindow {
    Platform::Window   *_receiver;
    GtkWidget          *_widget;
    GtkWidget          *_vbox;
    GtkWidget          *_menu_bar = NULL;
    GtkWidget          *_hbox;
    GtkEditorOverlay    _editor_overlay;
    GtkWidget          *_scrollbar;
    bool                _is_under_cursor = false;
    bool                _is_fullscreen = false;
    std::string         _tooltip_text;
    GdkRectangle        _tooltip_area;
    
    GtkEventController *_motion_controller;
    GtkEventController *_key_controller;

public:
    GtkWindow(Platform::Window *receiver) : _receiver(receiver), _editor_overlay(receiver) {
        _widget = gtk_window_new();
        _vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        _hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        
        gtk_widget_set_hexpand(_editor_overlay.get_widget(), TRUE);
        gtk_widget_set_vexpand(_editor_overlay.get_widget(), TRUE);
        
        _scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, NULL);
        gtk_widget_set_hexpand(_scrollbar, FALSE);
        
        gtk_box_append(GTK_BOX(_hbox), _editor_overlay.get_widget());
        gtk_box_append(GTK_BOX(_hbox), _scrollbar);
        gtk_box_append(GTK_BOX(_vbox), _hbox);
        gtk_window_set_child(GTK_WINDOW(_widget), _vbox);
        
        GtkAdjustment *adjustment = gtk_scrollbar_get_adjustment(GTK_SCROLLBAR(_scrollbar));
        g_signal_connect(adjustment, "value-changed", G_CALLBACK(+[](GtkAdjustment *adj, gpointer user_data) {
            auto self = static_cast<GtkWindow*>(user_data);
            self->on_scrollbar_value_changed();
        }), this);
        
        get_gl_widget().set_has_tooltip(true);
        get_gl_widget().signal_query_tooltip().
            connect(sigc::mem_fun(this, &GtkWindow::on_query_tooltip));
            
        _motion_controller = Gtk::EventControllerMotion::create();
        _motion_controller->signal_enter().connect(
            sigc::mem_fun(this, &GtkWindow::on_motion_enter));
        _motion_controller->signal_leave().connect(
            sigc::mem_fun(this, &GtkWindow::on_motion_leave));
        add_controller(_motion_controller);
        
        signal_close_request().connect(
            sigc::mem_fun(this, &GtkWindow::on_close_request), false);
            
        property_fullscreened().signal_changed().connect(
            sigc::mem_fun(this, &GtkWindow::on_fullscreen_changed));
    }

    bool is_full_screen() const {
        return _is_fullscreen;
    }

    Gtk::PopoverMenuBar *get_menu_bar() const {
        return _menu_bar;
    }

    void set_menu_bar(Gtk::PopoverMenuBar *menu_bar) {
        if(_menu_bar) {
            _vbox.remove(*_menu_bar);
        }
        _menu_bar = menu_bar;
        if(_menu_bar) {
            _vbox.prepend(*_menu_bar);
        }
    }

    GtkEditorOverlay &get_editor_overlay() {
        return _editor_overlay;
    }

    GtkGLWidget &get_gl_widget() {
        return _editor_overlay.get_gl_widget();
    }

    Gtk::Scrollbar &get_scrollbar() {
        return _scrollbar;
    }

    void set_tooltip(const std::string &text, const Gdk::Rectangle &rect) {
        if(_tooltip_text != text) {
            _tooltip_text = text;
            _tooltip_area = rect;
            get_gl_widget().trigger_tooltip_query();
        }
    }

protected:
    bool on_query_tooltip(int x, int y, bool keyboard_tooltip,
                          const Glib::RefPtr<Gtk::Tooltip> &tooltip) {
        tooltip->set_text(_tooltip_text);
        tooltip->set_tip_area(_tooltip_area);
        return !_tooltip_text.empty() && (keyboard_tooltip || _is_under_cursor);
    }

    void on_motion_enter(double x, double y) {
        _is_under_cursor = true;
    }

    void on_motion_leave() {
        _is_under_cursor = false;
    }

    bool on_close_request() {
        if(_receiver->onClose) {
            _receiver->onClose();
            return true; // Prevent the window from closing
        }
        return false; // Allow the window to close
    }

    void on_fullscreen_changed() {
        _is_fullscreen = property_fullscreened().get_value();
        if(_receiver->onFullScreen) {
            _receiver->onFullScreen(_is_fullscreen);
        }
    }

    void on_scrollbar_value_changed() {
        if(_receiver->onScrollbarAdjusted) {
            _receiver->onScrollbarAdjusted(_scrollbar.get_adjustment()->get_value());
        }
    }
};

//-----------------------------------------------------------------------------
// Windows
//-----------------------------------------------------------------------------

class WindowImplGtk final : public Window {
public:
    GtkWindow       gtkWindow;
    MenuBarRef      menuBar;

    WindowImplGtk(Window::Kind kind) : gtkWindow(this) {
        switch(kind) {
            case Kind::TOPLEVEL:
                break;

            case Kind::TOOL:
                gtkWindow.set_modal(true);
                gtkWindow.set_deletable(false);
                gtkWindow.set_hide_on_close(true);
                break;
        }

        auto icon = LoadPng("freedesktop/solvespace-48x48.png");
        auto gdkIcon =
            Gdk::Pixbuf::create_from_data(&icon->data[0], Gdk::COLORSPACE_RGB,
                                          icon->format == Pixmap::Format::RGBA, 8,
                                          icon->width, icon->height, icon->stride);
        gtkWindow.set_icon(gdkIcon->copy());
    }

    double GetPixelDensity() override {
        auto display = gtkWindow.get_display();
        auto monitor = display->get_monitor_at_window(gtkWindow.get_surface());
        return monitor->get_geometry().width / monitor->get_width_mm() * 25.4;
    }

    double GetDevicePixelRatio() override {
        return gtkWindow.get_scale_factor();
    }

    bool IsVisible() override {
        return gtkWindow.is_visible();
    }

    void SetVisible(bool visible) override {
        if(visible) {
            gtkWindow.show();
        } else {
            gtkWindow.hide();
        }
    }

    void Focus() override {
        gtkWindow.present();
    }

    bool IsFullScreen() override {
        return gtkWindow.is_full_screen();
    }

    void SetFullScreen(bool fullScreen) override {
        if(fullScreen) {
            gtkWindow.fullscreen();
        } else {
            gtkWindow.unfullscreen();
        }
    }

    void SetTitle(const std::string &title) override {
        gtkWindow.set_title(PrepareTitle(title));
    }

    void SetMenuBar(MenuBarRef newMenuBar) override {
        if(newMenuBar) {
            Gtk::PopoverMenuBar *gtkMenuBar = &((MenuBarImplGtk*)&*newMenuBar)->gtkMenuBar;
            gtkWindow.set_menu_bar(gtkMenuBar);
        } else {
            gtkWindow.set_menu_bar(NULL);
        }
        menuBar = newMenuBar;
    }

    void GetContentSize(double *width, double *height) override {
        *width  = gtkWindow.get_gl_widget().get_width();
        *height = gtkWindow.get_gl_widget().get_height();
    }

    void SetMinContentSize(double width, double height) override {
        gtkWindow.get_gl_widget().set_size_request((int)width, (int)height);
    }

    void FreezePosition(SettingsRef settings, const std::string &key) override {
        if(!gtkWindow.is_visible()) return;

        int left, top, width, height;
        gtkWindow.get_position(left, top);
        gtkWindow.get_size(width, height);
        bool isMaximized = gtkWindow.is_maximized();

        settings->FreezeInt(key + "_Left",       left);
        settings->FreezeInt(key + "_Top",        top);
        settings->FreezeInt(key + "_Width",      width);
        settings->FreezeInt(key + "_Height",     height);
        settings->FreezeBool(key + "_Maximized", isMaximized);
    }

    void ThawPosition(SettingsRef settings, const std::string &key) override {
        int left, top, width, height;
        gtkWindow.get_position(left, top);
        gtkWindow.get_size(width, height);

        left   = settings->ThawInt(key + "_Left",   left);
        top    = settings->ThawInt(key + "_Top",    top);
        width  = settings->ThawInt(key + "_Width",  width);
        height = settings->ThawInt(key + "_Height", height);

        gtkWindow.set_default_size(width, height);
        gtkWindow.move(left, top);

        if(settings->ThawBool(key + "_Maximized", false)) {
            gtkWindow.maximize();
        }
    }

    void SetCursor(Cursor cursor) override {
        std::string cursor_name;
        switch(cursor) {
            case Cursor::POINTER: cursor_name = "default"; break;
            case Cursor::HAND:    cursor_name = "pointer"; break;
            default: ssassert(false, "Unexpected cursor");
        }

        auto gdkSurface = gtkWindow.get_gl_widget().get_surface();
        if(gdkSurface) {
            auto display = gtkWindow.get_display();
            auto cursor = Gdk::Cursor::create(display, cursor_name);
            gdkSurface->set_cursor(cursor);
        }
    }

    void SetTooltip(const std::string &text, double x, double y,
                    double width, double height) override {
        gtkWindow.set_tooltip(text, { (int)x, (int)y, (int)width, (int)height });
    }

    bool IsEditorVisible() override {
        return gtkWindow.get_editor_overlay().is_editing();
    }

    void ShowEditor(double x, double y, double fontHeight, double minWidth,
                    bool isMonospace, const std::string &text) override {
        gtkWindow.get_editor_overlay().start_editing(
            (int)x, (int)y, (int)fontHeight, (int)minWidth, isMonospace, text);
    }

    void HideEditor() override {
        gtkWindow.get_editor_overlay().stop_editing();
    }

    void SetScrollbarVisible(bool visible) override {
        if(visible) {
            gtkWindow.get_scrollbar().set_visible(true);
        } else {
            gtkWindow.get_scrollbar().set_visible(false);
        }
    }

    void ConfigureScrollbar(double min, double max, double pageSize) override {
        auto adjustment = gtkWindow.get_scrollbar().get_adjustment();
        adjustment->configure(adjustment->get_value(), min, max, 1, 4, pageSize);
    }

    double GetScrollbarPosition() override {
        return gtkWindow.get_scrollbar().get_adjustment()->get_value();
    }

    void SetScrollbarPosition(double pos) override {
        return gtkWindow.get_scrollbar().get_adjustment()->set_value(pos);
    }

    void Invalidate() override {
        gtkWindow.get_gl_widget().queue_render();
    }
};

WindowRef CreateWindow(Window::Kind kind, WindowRef parentWindow) {
    auto window = std::make_shared<WindowImplGtk>(kind);
    if(parentWindow) {
        window->gtkWindow.set_transient_for(
            std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow);
    }
    return window;
}

//-----------------------------------------------------------------------------
// 3DConnexion support
//-----------------------------------------------------------------------------

void Open3DConnexion() {}
void Close3DConnexion() {}

#if defined(HAVE_SPACEWARE) && (defined(GDK_WINDOWING_X11) || defined(GDK_WINDOWING_WAYLAND))
static void ProcessSpnavEvent(WindowImplGtk *window, const spnav_event &spnavEvent, bool shiftDown, bool controlDown) {
    switch(spnavEvent.type) {
        case SPNAV_EVENT_MOTION: {
            SixDofEvent event = {};
            event.type = SixDofEvent::Type::MOTION;
            event.translationX = (double)spnavEvent.motion.x;
            event.translationY = (double)spnavEvent.motion.y;
            event.translationZ = (double)spnavEvent.motion.z  * -1.0;
            event.rotationX    = (double)spnavEvent.motion.rx *  0.001;
            event.rotationY    = (double)spnavEvent.motion.ry *  0.001;
            event.rotationZ    = (double)spnavEvent.motion.rz * -0.001;
            event.shiftDown    = shiftDown;
            event.controlDown  = controlDown;
            if(window->onSixDofEvent) {
                window->onSixDofEvent(event);
            }
            break;
        }

        case SPNAV_EVENT_BUTTON:
            SixDofEvent event = {};
            if(spnavEvent.button.press) {
                event.type = SixDofEvent::Type::PRESS;
            } else {
                event.type = SixDofEvent::Type::RELEASE;
            }
            switch(spnavEvent.button.bnum) {
                case 0:  event.button = SixDofEvent::Button::FIT; break;
                default: return;
            }
            event.shiftDown   = shiftDown;
            event.controlDown = controlDown;
            if(window->onSixDofEvent) {
                window->onSixDofEvent(event);
            }
            break;
    }
}

static GdkFilterReturn GdkSpnavFilter(GdkXEvent *gdkXEvent, GdkEvent *gdkEvent, gpointer data) {
    XEvent *xEvent = (XEvent *)gdkXEvent;
    WindowImplGtk *window = (WindowImplGtk *)data;
    bool shiftDown   = (xEvent->xmotion.state & ShiftMask)   != 0;
    bool controlDown = (xEvent->xmotion.state & ControlMask) != 0;

    spnav_event spnavEvent;
    if(spnav_x11_event(xEvent, &spnavEvent)) {
        ProcessSpnavEvent(window, spnavEvent, shiftDown, controlDown);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static gboolean ConsumeSpnavQueue(GIOChannel *, GIOCondition, gpointer data) {
    WindowImplGtk *window = (WindowImplGtk *)data;
    Glib::RefPtr<Gdk::Window> gdkWindow = window->gtkWindow.get_window();

    // We don't get modifier state through the socket.
    int x, y;
    Gdk::ModifierType mask{};
#if GTK_CHECK_VERSION(3, 20, 0)
    Glib::RefPtr<Gdk::Device> device = gdkWindow->get_display()->get_default_seat()->get_pointer();
#else
    Glib::RefPtr<Gdk::Device> device = gdkWindow->get_display()->get_device_manager()->get_client_pointer();
#endif
    gdkWindow->get_device_position(device, x, y, mask);
    bool shiftDown   = (mask & Gdk::SHIFT_MASK)   != 0;
    bool controlDown = (mask & Gdk::CONTROL_MASK) != 0;

    spnav_event spnavEvent;
    while(spnav_poll_event(&spnavEvent)) {
        ProcessSpnavEvent(window, spnavEvent, shiftDown, controlDown);
    }
    return TRUE;
}

void Request3DConnexionEventsForWindow(WindowRef window) {
    std::shared_ptr<WindowImplGtk> windowImpl =
        std::static_pointer_cast<WindowImplGtk>(window);

    Glib::RefPtr<Gdk::Window> gdkWindow = windowImpl->gtkWindow.get_window();
#if defined(GDK_WINDOWING_X11)
    if(GDK_IS_X11_DISPLAY(gdkWindow->get_display()->gobj())) {
        if(spnav_x11_open(gdk_x11_get_default_xdisplay(),
                          gdk_x11_window_get_xid(gdkWindow->gobj())) != -1) {
            gdkWindow->add_filter(GdkSpnavFilter, windowImpl.get());
        } else if(spnav_open() != -1) {
            g_io_add_watch(g_io_channel_unix_new(spnav_fd()), G_IO_IN,
                           ConsumeSpnavQueue, windowImpl.get());
        }
    }
#endif
#if defined(GDK_WINDOWING_WAYLAND)
    if(GDK_IS_WAYLAND_DISPLAY(gdkWindow->get_display()->gobj())) {
	if(spnav_open() != -1) {
            g_io_add_watch(g_io_channel_unix_new(spnav_fd()), G_IO_IN,
                           ConsumeSpnavQueue, windowImpl.get());
        }
    }
#endif

}
#else
void Request3DConnexionEventsForWindow(WindowRef window) {}
#endif

//-----------------------------------------------------------------------------
// Message dialogs
//-----------------------------------------------------------------------------

class MessageDialogImplGtk;

static std::vector<std::shared_ptr<MessageDialogImplGtk>> shownMessageDialogs;

class MessageDialogImplGtk final : public MessageDialog,
                                   public std::enable_shared_from_this<MessageDialogImplGtk> {
public:
    Gtk::Image         gtkImage;
    Gtk::MessageDialog gtkDialog;

    MessageDialogImplGtk(Gtk::Window &parent)
        : gtkDialog(parent, "", /*use_markup=*/false, Gtk::MESSAGE_INFO,
                    Gtk::BUTTONS_NONE, /*modal=*/true)
    {
        SetTitle("Message");
    }

    void SetType(Type type) override {
        switch(type) {
            case Type::INFORMATION:
                gtkImage.set_from_icon_name("dialog-information", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::QUESTION:
                gtkImage.set_from_icon_name("dialog-question", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::WARNING:
                gtkImage.set_from_icon_name("dialog-warning", Gtk::ICON_SIZE_DIALOG);
                break;

            case Type::ERROR:
                gtkImage.set_from_icon_name("dialog-error", Gtk::ICON_SIZE_DIALOG);
                break;
        }
        gtkDialog.set_image(gtkImage);
    }

    void SetTitle(std::string title) override {
        gtkDialog.set_title(PrepareTitle(title));
    }

    void SetMessage(std::string message) override {
        gtkDialog.set_message(message);
    }

    void SetDescription(std::string description) override {
        gtkDialog.set_secondary_text(description);
    }

    void AddButton(std::string label, Response response, bool isDefault) override {
        int responseId = 0;
        switch(response) {
            case Response::NONE:   ssassert(false, "Unexpected response");
            case Response::OK:     responseId = Gtk::RESPONSE_OK;     break;
            case Response::YES:    responseId = Gtk::RESPONSE_YES;    break;
            case Response::NO:     responseId = Gtk::RESPONSE_NO;     break;
            case Response::CANCEL: responseId = Gtk::RESPONSE_CANCEL; break;
        }
        gtkDialog.add_button(PrepareMnemonics(label), responseId);
        if(isDefault) {
            gtkDialog.set_default_response(responseId);
        }
    }

    Response ProcessResponse(int gtkResponse) {
        Response response;
        switch(gtkResponse) {
            case Gtk::RESPONSE_OK:     response = Response::OK;     break;
            case Gtk::RESPONSE_YES:    response = Response::YES;    break;
            case Gtk::RESPONSE_NO:     response = Response::NO;     break;
            case Gtk::RESPONSE_CANCEL: response = Response::CANCEL; break;

            case Gtk::RESPONSE_NONE:
            case Gtk::RESPONSE_CLOSE:
            case Gtk::RESPONSE_DELETE_EVENT:
                response = Response::NONE;
                break;

            default: ssassert(false, "Unexpected response");
        }

        if(onResponse) {
            onResponse(response);
        }
        return response;
    }

    void ShowModal() override {
        gtkDialog.signal_hide().connect([this] {
            auto it = std::remove(shownMessageDialogs.begin(), shownMessageDialogs.end(),
                                  shared_from_this());
            shownMessageDialogs.erase(it);
        });
        shownMessageDialogs.push_back(shared_from_this());

        gtkDialog.signal_response().connect([this](int gtkResponse) {
            ProcessResponse(gtkResponse);
            gtkDialog.hide();
        });
        gtkDialog.show();
    }

    Response RunModal() override {
        return ProcessResponse(gtkDialog.run());
    }
};

MessageDialogRef CreateMessageDialog(WindowRef parentWindow) {
    return std::make_shared<MessageDialogImplGtk>(
                std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow);
}

//-----------------------------------------------------------------------------
// File dialogs
//-----------------------------------------------------------------------------

class FileDialogImplGtk : public FileDialog {
public:
    Gtk::FileChooser           *gtkChooser;
    std::vector<std::string>    extensions;

    void InitFileChooser(Gtk::FileChooser &chooser) {
        gtkChooser = &chooser;
        gtkChooser->signal_selection_changed().
            connect(sigc::mem_fun(this, &FileDialogImplGtk::FilterChanged));
    }

    void SetCurrentName(std::string name) override {
        gtkChooser->set_current_name(name);
    }

    Platform::Path GetFilename() override {
        auto file = gtkChooser->get_file();
        if (file) {
            return Path::From(file->get_path());
        }
        return Path::From("");
    }

    void SetFilename(Platform::Path path) override {
        auto file = Gio::File::create_for_path(path.raw);
        gtkChooser->set_file(file);
    }

    void SuggestFilename(Platform::Path path) override {
        gtkChooser->set_current_name(path.FileStem()+"."+GetExtension());
    }

    void AddFilter(std::string name, std::vector<std::string> extensions) override {
        Glib::RefPtr<Gtk::FileFilter> gtkFilter = Gtk::FileFilter::create();
        Glib::ustring desc;
        for(auto extension : extensions) {
            Glib::ustring pattern = "*";
            if(!extension.empty()) {
                pattern = "*." + extension;
                gtkFilter->add_pattern(pattern);
                gtkFilter->add_pattern(Glib::ustring(pattern).uppercase());
            }
            if(!desc.empty()) {
                desc += ", ";
            }
            desc += pattern;
        }
        gtkFilter->set_name(name + " (" + desc + ")");

        this->extensions.push_back(extensions.front());
        gtkChooser->add_filter(gtkFilter);
    }

    std::string GetExtension() {
        auto filters = gtkChooser->list_filters();
        size_t filterIndex =
            std::find(filters.begin(), filters.end(), gtkChooser->get_filter()) -
            filters.begin();
        if(filterIndex < extensions.size()) {
            return extensions[filterIndex];
        } else {
            return extensions.front();
        }
    }

    void SetExtension(std::string extension) {
        auto filters = gtkChooser->list_filters();
        size_t extensionIndex =
            std::find(extensions.begin(), extensions.end(), extension) -
            extensions.begin();
        if(extensionIndex < filters.size()) {
            gtkChooser->set_filter(filters[extensionIndex]);
        } else {
            gtkChooser->set_filter(filters.front());
        }
    }

    void FilterChanged() {
        std::string extension = GetExtension();
        if(extension.empty())
            return;

        Platform::Path path = GetFilename();
        if(gtkChooser->get_action() != Gtk::FileChooser::Action::OPEN) {
            SetCurrentName(path.WithExtension(extension).FileName());
        }
    }

    void FreezeChoices(SettingsRef settings, const std::string &key) override {
        auto folder = gtkChooser->get_current_folder();
        if (folder) {
            settings->FreezeString("Dialog_" + key + "_Folder", folder->get_path());
        }
        settings->FreezeString("Dialog_" + key + "_Filter", GetExtension());
    }

    void ThawChoices(SettingsRef settings, const std::string &key) override {
        auto folder = Gio::File::create_for_path(settings->ThawString("Dialog_" + key + "_Folder"));
        gtkChooser->set_current_folder(folder);
        SetExtension(settings->ThawString("Dialog_" + key + "_Filter"));
    }

    void CheckForUntitledFile() {
        if(gtkChooser->get_action() == Gtk::FileChooser::Action::SAVE &&
                Path::From(gtkChooser->get_current_name()).FileStem().empty()) {
            gtkChooser->set_current_name(std::string(_("untitled")) + "." + GetExtension());
        }
    }
};

class FileDialogGtkImplGtk final : public FileDialogImplGtk {
public:
    Gtk::FileChooserDialog      gtkDialog;

    FileDialogGtkImplGtk(Gtk::Window &gtkParent, bool isSave)
        : gtkDialog(gtkParent,
                    isSave ? C_("title", "Save File")
                           : C_("title", "Open File"),
                    isSave ? Gtk::FILE_CHOOSER_ACTION_SAVE
                           : Gtk::FILE_CHOOSER_ACTION_OPEN) {
        gtkDialog.add_button(C_("button", "_Cancel"), Gtk::RESPONSE_CANCEL);
        gtkDialog.add_button(isSave ? C_("button", "_Save")
                                    : C_("button", "_Open"), Gtk::RESPONSE_OK);
        gtkDialog.set_default_response(Gtk::RESPONSE_OK);
        if(isSave) {
            gtkDialog.set_do_overwrite_confirmation(true);
        }
        InitFileChooser(gtkDialog);
    }

    void SetTitle(std::string title) override {
        gtkDialog.set_title(PrepareTitle(title));
    }

    bool RunModal() override {
        CheckForUntitledFile();
        if(gtkDialog.run() == Gtk::RESPONSE_OK) {
            return true;
        } else {
            return false;
        }
    }
};

#if defined(HAVE_GTK_FILECHOOSERNATIVE)

class FileDialogNativeImplGtk final : public FileDialogImplGtk {
public:
    Glib::RefPtr<Gtk::FileChooserNative> gtkNative;

    FileDialogNativeImplGtk(Gtk::Window &gtkParent, bool isSave) {
        gtkNative = Gtk::FileChooserNative::create(
            isSave ? C_("title", "Save File")
                   : C_("title", "Open File"),
            gtkParent,
            isSave ? Gtk::FILE_CHOOSER_ACTION_SAVE
                   : Gtk::FILE_CHOOSER_ACTION_OPEN,
            isSave ? C_("button", "_Save")
                   : C_("button", "_Open"),
            C_("button", "_Cancel"));
        if(isSave) {
            gtkNative->set_do_overwrite_confirmation(true);
        }
        InitFileChooser(*gtkNative);
    }

    void SetTitle(std::string title) override {
        gtkNative->set_title(PrepareTitle(title));
    }

    bool RunModal() override {
        CheckForUntitledFile();
        if(gtkNative->run() == Gtk::RESPONSE_ACCEPT) {
            return true;
        } else {
            return false;
        }
    }
};

#endif

#if defined(HAVE_GTK_FILECHOOSERNATIVE)
#   define FILE_DIALOG_IMPL FileDialogNativeImplGtk
#else
#   define FILE_DIALOG_IMPL FileDialogGtkImplGtk
#endif

FileDialogRef CreateOpenFileDialog(WindowRef parentWindow) {
    Gtk::Window &gtkParent = std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow;
    return std::make_shared<FILE_DIALOG_IMPL>(gtkParent, /*isSave=*/false);
}

FileDialogRef CreateSaveFileDialog(WindowRef parentWindow) {
    Gtk::Window &gtkParent = std::static_pointer_cast<WindowImplGtk>(parentWindow)->gtkWindow;
    return std::make_shared<FILE_DIALOG_IMPL>(gtkParent, /*isSave=*/true);
}

//-----------------------------------------------------------------------------
// Application-wide APIs
//-----------------------------------------------------------------------------

std::vector<Platform::Path> GetFontFiles() {
    std::vector<Platform::Path> fonts;

    // fontconfig is already initialized by GTK
    FcPattern   *pat = FcPatternCreate();
    FcObjectSet *os  = FcObjectSetBuild(FC_FILE, (char *)0);
    FcFontSet   *fs  = FcFontList(0, pat, os);

    for(int i = 0; i < fs->nfont; i++) {
        FcChar8 *filenameFC = FcPatternFormat(fs->fonts[i], (const FcChar8*) "%{file}");
        fonts.push_back(Platform::Path::From((const char *)filenameFC));
        FcStrFree(filenameFC);
    }

    FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);

    return fonts;
}

void OpenInBrowser(const std::string &url) {
    GError *error = NULL;
    gtk_show_uri_on_window(NULL, url.c_str(), GDK_CURRENT_TIME, &error);
    if (error) {
        g_error_free(error);
    }
}

GtkApplication *gtkApp = NULL;

std::vector<std::string> InitGui(int argc, char **argv) {
    // but it's not really worth the effort.
    // We set it back to C after all so that printf() and friends behave in a consistent way.
    setlocale(LC_ALL, "");
    gboolean is_utf8 = g_get_charset(NULL);
    if(!is_utf8) {
        dbp("Sorry, only UTF-8 locales are supported.");
        exit(1);
    }
    setlocale(LC_ALL, "C");

    gtkApp = gtk_application_new("org.solvespace.solvespace", G_APPLICATION_DEFAULT_FLAGS);
    
    // Now that GTK arguments are removed, grab arguments for ourselves.
    std::vector<std::string> args = SolveSpace::Platform::InitCli(argc, argv);

    // Add our application-specific styles, to override GTK defaults.
    GtkCssProvider *style_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(style_provider, 
        "entry { background: white; color: black; }", -1);
    
    GdkDisplay *display = gdk_display_get_default();
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(style_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(style_provider);

    // Set locale from user preferences.
    // This apparently only consults the LANGUAGE environment variable.
    const char* const* langNames = g_get_language_names();
    while(*langNames) {
        if(SetLocale(*langNames++)) break;
    }
    if(!*langNames) {
        SetLocale("en_US");
    }

    return args;
}

void RunGui() {
    g_application_run(G_APPLICATION(gtkApp), 0, NULL);
}

void ExitGui() {
    g_application_quit(G_APPLICATION(gtkApp));
}

void ClearGui() {
    if (gtkApp) {
        g_object_unref(gtkApp);
        gtkApp = NULL;
    }
}

}
}
