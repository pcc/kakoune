#include "client.hh"

#include "face_registry.hh"
#include "context.hh"
#include "buffer_manager.hh"
#include "buffer_utils.hh"
#include "file.hh"
#include "remote.hh"
#include "option.hh"
#include "client_manager.hh"
#include "command_manager.hh"
#include "event_manager.hh"
#include "user_interface.hh"
#include "window.hh"
#include "hash_map.hh"

#include <csignal>
#include <unistd.h>

#include <utility>

namespace Kakoune
{

Client::Client(std::unique_ptr<UserInterface>&& ui,
               std::unique_ptr<Window>&& window,
               SelectionList selections,
               EnvVarMap env_vars,
               String name)
    : m_ui{std::move(ui)}, m_window{std::move(window)},
      m_input_handler{std::move(selections), Context::Flags::None,
                      std::move(name)},
      m_env_vars(std::move(env_vars))
{
    m_window->set_client(this);

    context().set_client(*this);
    context().set_window(*m_window);

    m_window->set_dimensions(m_ui->dimensions());
    m_window->options().register_watcher(*this);

    m_ui->set_ui_options(m_window->options()["ui_options"].get<UserInterface::Options>());
    m_ui->set_on_key([this](Key key) {
        if (key == ctrl('c'))
            killpg(getpgrp(), SIGINT);
        else
            m_pending_keys.push_back(key);
    });

    m_window->hooks().run_hook("WinDisplay", m_window->buffer().name(), context());

    force_redraw();
}

Client::~Client()
{
    m_window->options().unregister_watcher(*this);
    m_window->set_client(nullptr);
    // Do not move the selections here, as we need them to be valid
    // in order to correctly destroy the input handler
    ClientManager::instance().add_free_window(std::move(m_window),
                                              context().selections());
}

bool Client::process_pending_inputs()
{
    const bool debug_keys = (bool)(context().options()["debug"].get<DebugFlags>() & DebugFlags::Keys);
    // steal keys as we might receive new keys while handling them.
    Vector<Key, MemoryDomain::Client> keys = std::move(m_pending_keys);
    for (auto& key : keys)
    {
        try
        {
            if (debug_keys)
                write_to_debug_buffer(format("Client '{}' got key '{}'",
                                             context().name(), key_to_str(key)));

            if (key == Key::FocusIn)
                context().hooks().run_hook("FocusIn", context().name(), context());
            else if (key == Key::FocusOut)
                context().hooks().run_hook("FocusOut", context().name(), context());
            else if (key.modifiers == Key::Modifiers::Resize)
            {
                m_window->set_dimensions(m_ui->dimensions());
                force_redraw();
            }
            else
                m_input_handler.handle_key(key);

            context().hooks().run_hook("RawKey", key_to_str(key), context());
        }
        catch (Kakoune::runtime_error& error)
        {
            context().print_status({ error.what().str(), get_face("Error") });
            context().hooks().run_hook("RuntimeError", error.what(), context());
        }
    }
    return not keys.empty();
}

void Client::print_status(DisplayLine status_line, bool immediate)
{
    m_status_line = std::move(status_line);
    if (immediate)
    {
        m_ui->draw_status(m_status_line, m_mode_line, get_face("StatusLine"));
        m_ui->refresh(true);
    }
    else
    {
        m_ui_pending |= StatusLine;
    }
}


DisplayCoord Client::dimensions() const
{
    return m_ui->dimensions();
}

String generate_context_info(const Context& context)
{
    String s = "";
    if (context.buffer().is_modified())
        s += "[+]";
    if (context.client().input_handler().is_recording())
        s += format("[recording ({})]", context.client().input_handler().recording_reg());
    if (context.buffer().flags() & Buffer::Flags::New)
        s += "[new file]";
    if (context.hooks_disabled())
        s += "[no-hooks]";
    if (context.buffer().flags() & Buffer::Flags::Fifo)
        s += "[fifo]";
    return s;
}

DisplayLine Client::generate_mode_line() const
{
    DisplayLine modeline;
    try
    {
        const String& modelinefmt = context().options()["modelinefmt"].get<String>();
        HashMap<String, DisplayLine> atoms{{ "mode_info", context().client().input_handler().mode_line() },
                                           { "context_info", {generate_context_info(context()), get_face("Information")}}};
        auto expanded = expand(modelinefmt, context(), ShellContext{},
                               [](String s) { return escape(s, '{', '\\'); });
        modeline = parse_display_line(expanded, atoms);
    }
    catch (runtime_error& err)
    {
        write_to_debug_buffer(format("Error while parsing modelinefmt: {}", err.what()));
        modeline.push_back({ "modelinefmt error, see *debug* buffer", get_face("Error") });
    }

    return modeline;
}

void Client::change_buffer(Buffer& buffer)
{
    if (m_buffer_reload_dialog_opened)
        close_buffer_reload_dialog();

    auto* current = &m_window->buffer();
    m_last_buffer = contains(BufferManager::instance(), current) ? current : nullptr;

    auto& client_manager = ClientManager::instance();
    m_window->options().unregister_watcher(*this);
    m_window->set_client(nullptr);
    client_manager.add_free_window(std::move(m_window),
                                   std::move(context().selections()));
    WindowAndSelections ws = client_manager.get_free_window(buffer);

    m_window = std::move(ws.window);
    m_window->set_client(this);
    m_window->options().register_watcher(*this);
    m_ui->set_ui_options(m_window->options()["ui_options"].get<UserInterface::Options>());

    context().selections_write_only() = std::move(ws.selections);
    context().set_window(*m_window);
    m_window->set_dimensions(m_ui->dimensions());

    m_window->hooks().run_hook("WinDisplay", buffer.name(), context());
    force_redraw();
}

static bool is_inline(InfoStyle style)
{
    return style == InfoStyle::Inline or
           style == InfoStyle::InlineAbove or
           style == InfoStyle::InlineBelow;
}

void Client::redraw_ifn()
{
    Window& window = context().window();
    if (window.needs_redraw(context()))
        m_ui_pending |= Draw;

    DisplayLine mode_line = generate_mode_line();
    if (mode_line.atoms() != m_mode_line.atoms())
    {
        m_ui_pending |= StatusLine;
        m_mode_line = std::move(mode_line);
    }

    if (m_ui_pending == 0)
        return;

    if (m_ui_pending & Draw)
    {
        m_ui->draw(window.update_display_buffer(context()),
                   get_face("Default"), get_face("BufferPadding"));

        if (not m_menu.items.empty() and m_menu.style == MenuStyle::Inline and
            m_menu.ui_anchor != window.display_position(m_menu.anchor))
            m_ui_pending |= (MenuShow | MenuSelect);
        if (not m_info.content.empty() and is_inline(m_info.style) and
            m_info.ui_anchor != window.display_position(m_info.anchor))
            m_ui_pending |= InfoShow;
    }

    if (m_ui_pending & MenuShow)
    {
        m_menu.ui_anchor = m_menu.style == MenuStyle::Inline ?
            window.display_position(m_menu.anchor) : DisplayCoord{};
        m_ui->menu_show(m_menu.items, m_menu.ui_anchor,
                        get_face("MenuForeground"), get_face("MenuBackground"),
                        m_menu.style);
    }
    if (m_ui_pending & MenuSelect)
        m_ui->menu_select(m_menu.selected);
    if (m_ui_pending & MenuHide)
        m_ui->menu_hide();

    if (m_ui_pending & InfoShow)
    {
        m_info.ui_anchor = is_inline(m_info.style) ?
            window.display_position(m_info.anchor) : DisplayCoord{};
        m_ui->info_show(m_info.title, m_info.content, m_info.ui_anchor,
                        get_face("Information"), m_info.style);
    }
    if (m_ui_pending & InfoHide)
        m_ui->info_hide();

    if (m_ui_pending & StatusLine)
        m_ui->draw_status(m_status_line, m_mode_line, get_face("StatusLine"));

    auto cursor = m_input_handler.get_cursor_info();
    m_ui->set_cursor(cursor.first, cursor.second);

    m_ui->refresh(m_ui_pending | Refresh);
    m_ui_pending = 0;
}

void Client::force_redraw()
{
    m_ui_pending |= Refresh | Draw | StatusLine |
        (m_menu.items.empty() ? MenuHide : MenuShow | MenuSelect) |
        (m_info.content.empty() ? InfoHide : InfoShow);
}

void Client::reload_buffer()
{
    Buffer& buffer = context().buffer();
    reload_file_buffer(buffer);
    context().print_status({ format("'{}' reloaded", buffer.display_name()),
                             get_face("Information") });
}

void Client::on_buffer_reload_key(Key key)
{
    auto& buffer = context().buffer();

    if (key == 'y' or key == Key::Return)
        reload_buffer();
    else if (key == 'n' or key == Key::Escape)
    {
        // reread timestamp in case the file was modified again
        buffer.set_fs_timestamp(get_fs_timestamp(buffer.name()));
        print_status({ format("'{}' kept", buffer.display_name()),
                       get_face("Information") });
    }
    else
    {
        print_status({ format("'{}' is not a valid choice", key_to_str(key)),
                       get_face("Error") });
        m_input_handler.on_next_key(KeymapMode::None, [this](Key key, Context&){ on_buffer_reload_key(key); });
        return;
    }

    for (auto& client : ClientManager::instance())
    {
        if (&client->context().buffer() == &buffer and
            client->m_buffer_reload_dialog_opened)
            client->close_buffer_reload_dialog();
    }
}

void Client::close_buffer_reload_dialog()
{
    kak_assert(m_buffer_reload_dialog_opened);
    m_buffer_reload_dialog_opened = false;
    info_hide(true);
    m_input_handler.reset_normal_mode();
}

void Client::check_if_buffer_needs_reloading()
{
    if (m_buffer_reload_dialog_opened)
        return;

    Buffer& buffer = context().buffer();
    auto reload = context().options()["autoreload"].get<Autoreload>();
    if (not (buffer.flags() & Buffer::Flags::File) or reload == Autoreload::No)
        return;

    const String& filename = buffer.name();
    timespec ts = get_fs_timestamp(filename);
    if (ts == InvalidTime or ts == buffer.fs_timestamp())
        return;
    if (reload == Autoreload::Ask)
    {
        StringView bufname = buffer.display_name();
        info_show(format("reload '{}' ?", bufname),
                  format("'{}' was modified externally\n"
                         "press <ret> or y to reload, <esc> or n to keep",
                         bufname), {}, InfoStyle::Modal);

        m_buffer_reload_dialog_opened = true;
        m_input_handler.on_next_key(KeymapMode::None, [this](Key key, Context&){ on_buffer_reload_key(key); });
    }
    else
        reload_buffer();
}

StringView Client::get_env_var(StringView name) const
{
    auto it = m_env_vars.find(name);
    if (it == m_env_vars.end())
        return {};
    return it->value;
}

void Client::on_option_changed(const Option& option)
{
    if (option.name() == "ui_options")
    {
        m_ui->set_ui_options(option.get<UserInterface::Options>());
        m_ui_pending |= Draw;
    }
}

void Client::menu_show(Vector<DisplayLine> choices, BufferCoord anchor, MenuStyle style)
{
    m_menu = Menu{ std::move(choices), anchor, {}, style, -1 };
    m_ui_pending |= MenuShow;
    m_ui_pending &= ~MenuHide;
}

void Client::menu_select(int selected)
{
    m_menu.selected = selected;
    m_ui_pending |= MenuSelect;
    m_ui_pending &= ~MenuHide;
}

void Client::menu_hide()
{
    m_menu = Menu{};
    m_ui_pending |= MenuHide;
    m_ui_pending &= ~(MenuShow | MenuSelect);
}

void Client::info_show(String title, String content, BufferCoord anchor, InfoStyle style)
{
    if (m_info.style == InfoStyle::Modal) // We already have a modal info opened, do not touch it.
        return;

    m_info = Info{ std::move(title), std::move(content), anchor, {}, style };
    m_ui_pending |= InfoShow;
    m_ui_pending &= ~InfoHide;
}

void Client::info_hide(bool even_modal)
{
    if (not even_modal and m_info.style == InfoStyle::Modal)
        return;

    m_info = Info{};
    m_ui_pending |= InfoHide;
    m_ui_pending &= ~InfoShow;
}

}