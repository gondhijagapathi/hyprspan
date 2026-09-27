// hyprspan: honour _NET_WM_FULLSCREEN_MONITORS for XWayland windows.
//
// Citrix Workspace (also VMware, xfreerdp /multimon, remote-viewer) spans a fullscreen window over several
// monitors by sending the EWMH _NET_WM_FULLSCREEN_MONITORS client message with the Xinerama indices of the
// top, bottom, left and right edge monitors. Hyprland neither advertises nor handles that message, so these
// clients stay on one monitor. This plugin:
//   - advertises the atom in _NET_SUPPORTED and records each window's requested monitors,
//   - sizes the window to the union of those monitors while it is fullscreen,
//   - makes every other covered monitor treat the window as the fullscreen window of its active workspace, so
//     bars and windows there fade out, input goes to the spanning window, and the renderer draws it there.

#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/animation/WorkspaceAnimationController.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>
#include <hyprland/src/xwayland/XWM.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>

#include <xcb/xinerama.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <unordered_map>
#include <vector>

using namespace Fullscreen;

namespace {
    HANDLE g_handle = nullptr;

    // Explicit template instantiation is exempt from access checks, which gives a standard-conforming way to
    // reach CXWM's private XCB connection without redefining `private`.
    template <typename Tag, typename Tag::type Member>
    struct SExposePrivate {
        friend typename Tag::type privateMember(Tag) {
            return Member;
        }
    };

    struct SXWMConnection {
        using type = UP<CXCBConnection> CXWM::*;
        friend type privateMember(SXWMConnection);
    };
    template struct SExposePrivate<SXWMConnection, &CXWM::m_connection>;

    xcb_connection_t* xwmConnection(CXWM* wm) {
        if (!wm)
            return nullptr;
        const auto& conn = wm->*privateMember(SXWMConnection{});
        return conn ? static_cast<xcb_connection_t*>(*conn) : nullptr;
    }

    xcb_connection_t* xwmConnection() {
        return g_pXWayland ? xwmConnection(g_pXWayland->m_wm.get()) : nullptr;
    }

    constexpr const char* FULLSCREEN_MONITORS = "_NET_WM_FULLSCREEN_MONITORS";

    xcb_atom_t fullscreenMonitorsAtom() {
        const auto it = HYPRATOMS.find(FULLSCREEN_MONITORS);
        return it == HYPRATOMS.end() ? xcb_atom_t{XCB_ATOM_NONE} : it->second;
    }

    struct SSpanRequest {
        std::array<uint32_t, 4>    edges = {}; // Xinerama indices of the top, bottom, left and right monitors
        std::vector<PHLMONITORREF> monitors;   // every monitor the span overlaps; empty while it can't be shown
        CBox                       box;        // the span, in layout coordinates
        PHLWINDOWREF               window;     // resolved lazily; the X window may not be mapped yet
        bool                       warned = false;
    };

    std::unordered_map<xcb_window_t, SSpanRequest> g_requests;
    std::vector<PHLWORKSPACEREF>                   g_covered; // workspaces currently hidden under another monitor's span
    std::vector<CHyprSignalListener>               g_listeners;
    wl_event_source*                               g_layoutTimer = nullptr;

    PHLWINDOW windowForXID(xcb_window_t xid) {
        for (auto const& w : Desktop::windowState()->windows()) {
            if (w->m_isX11 && w->m_xwaylandSurface && w->m_xwaylandSurface->m_xID == xid)
                return w;
        }
        return nullptr;
    }

    enum eResolveResult : uint8_t {
        RESOLVE_OK = 0,
        RESOLVE_BAD_INDEX,       // an edge index names no Xinerama screen
        RESOLVE_LAYOUT_MISMATCH, // the covered monitors are arranged differently for X11 than on screen
        RESOLVE_NO_XINERAMA,
    };

    // Turns the request's Xinerama edge indices into the monitors it covers and its box in layout coordinates.
    //
    // Hyprland gives X11 clients their own monitor layout: one top-aligned row in monitor order, at
    // m_xwaylandPosition, whatever the real arrangement is. The client draws for that layout, so a single window
    // shows the right part on every monitor only when the covered monitors' X11 layout is a translation of their
    // real one, at X11 scale 1. Anything else is refused rather than shown with the monitors' contents swapped.
    eResolveResult resolve(xcb_connection_t* conn, SSpanRequest& req) {
        req.monitors.clear();
        req.box = {};

        auto* reply = xcb_xinerama_query_screens_reply(conn, xcb_xinerama_query_screens(conn), nullptr);
        if (!reply)
            return RESOLVE_NO_XINERAMA;

        const uint32_t COUNT   = xcb_xinerama_query_screens_screen_info_length(reply);
        const auto*    screens = xcb_xinerama_query_screens_screen_info(reply);
        if (std::ranges::any_of(req.edges, [&](uint32_t i) { return i >= COUNT; })) {
            free(reply);
            return RESOLVE_BAD_INDEX;
        }

        const auto& TOP    = screens[req.edges[0]];
        const auto& BOTTOM = screens[req.edges[1]];
        const auto& LEFT   = screens[req.edges[2]];
        const auto& RIGHT  = screens[req.edges[3]];
        const CBox  XBOX{sc<double>(LEFT.x_org), sc<double>(TOP.y_org), sc<double>(RIGHT.x_org + RIGHT.width - LEFT.x_org),
                        sc<double>(BOTTOM.y_org + BOTTOM.height - TOP.y_org)};
        free(reply);

        std::optional<Vector2D> offset;
        for (auto const& m : State::monitorState()->monitors()) {
            if (!m->m_enabled || CBox{m->m_xwaylandPosition, m->m_size}.intersection(XBOX).empty())
                continue;

            const Vector2D OFFSET = m->m_position - m->m_xwaylandPosition;
            if (m->m_xwaylandScale != 1.F || (offset && *offset != OFFSET)) {
                req.monitors.clear();
                return RESOLVE_LAYOUT_MISMATCH;
            }

            offset = OFFSET;
            req.monitors.emplace_back(m);
        }

        if (offset)
            req.box = CBox{XBOX}.translate(*offset);

        return RESOLVE_OK;
    }

    void warnLayoutMismatch(SSpanRequest& req) {
        if (req.warned)
            return;
        req.warned = true;
        HyprlandAPI::addNotification(g_handle,
                                     "[hyprspan] Can't span this window: X11 apps see your monitors in one left-to-right row in monitor order, top-aligned. "
                                     "Arrange the monitors that way to span.",
                                     CHyprColor{1.0, 0.7, 0.2, 1.0}, 10000);
    }

    // The span for a window, if it is fullscreen and its request covers more than one monitor.
    const SSpanRequest* activeSpan(const PHLWINDOW& w) {
        if (!w || !w->m_isMapped || !w->m_isX11 || !w->m_xwaylandSurface)
            return nullptr;

        const auto it = g_requests.find(w->m_xwaylandSurface->m_xID);
        if (it == g_requests.end() || it->second.monitors.size() < 2)
            return nullptr;

        if (controller()->getFullscreenModes(w).internal != FSMODE_FULLSCREEN)
            return nullptr;

        return &it->second;
    }

    // The spanning window that covers `ws`, which must be the active workspace of some other monitor the
    // span overlaps. The window's own workspace is handled by Hyprland's normal fullscreen logic.
    PHLWINDOW spanOwnerFor(const PHLWORKSPACE& ws) {
        if (g_requests.empty() || !ws || ws->m_isSpecialWorkspace)
            return nullptr;

        const auto MONITOR = ws->m_monitor.lock();
        if (!MONITOR || MONITOR->m_activeWorkspace != ws)
            return nullptr;

        for (auto& [xid, req] : g_requests) {
            auto w = req.window.lock();
            if (!w) {
                w          = windowForXID(xid);
                req.window = w;
            }

            if (!w || w->m_workspace == ws || !w->m_workspace || !w->m_workspace->isVisible() || w->m_workspace->m_isSpecialWorkspace)
                continue;

            if (!activeSpan(w))
                continue;

            if (std::ranges::any_of(req.monitors, [&](const PHLMONITORREF& m) { return m.lock() == MONITOR; }))
                return w;
        }

        return nullptr;
    }

    // Same bookkeeping IFullscreenHandler::setNoMembersAboveFullscreen does for a fullscreen window's own
    // monitor, applied to a workspace that another monitor's span covers or has just stopped covering.
    void applyCoverage(const PHLWORKSPACE& ws) {
        if (!ws)
            return;

        const bool COVERED = controller()->hasFullscreen(ws);

        for (auto const& w : Desktop::windowState()->windows()) {
            if (w->m_workspace != ws || w->m_pinned || controller()->isFullscreen(w))
                continue;
            w->m_allowedOverFullscreen = !COVERED;
            w->updateFullscreenInputState();
        }

        const auto MONITOR = ws->m_monitor.lock();
        for (auto const& ls : Desktop::layerState()->layers()) {
            if (ls->m_monitor == MONITOR)
                ls->m_aboveFullscreen = !COVERED;
        }

        Animation::Workspace::setFullscreenFadeAnimation(ws, COVERED ? Animation::Workspace::ANIMATION_TYPE_IN : Animation::Workspace::ANIMATION_TYPE_OUT);
    }

    void refreshCoverage() {
        std::vector<PHLWORKSPACEREF> now;
        for (auto const& m : State::monitorState()->monitors()) {
            if (m->m_activeWorkspace && spanOwnerFor(m->m_activeWorkspace))
                now.emplace_back(m->m_activeWorkspace);
        }

        auto contains = [](const std::vector<PHLWORKSPACEREF>& list, const PHLWORKSPACEREF& ws) {
            return std::ranges::any_of(list, [&](const PHLWORKSPACEREF& other) { return other.lock() == ws.lock(); });
        };

        for (auto const& ws : g_covered) {
            if (!contains(now, ws))
                applyCoverage(ws.lock());
        }
        for (auto const& ws : now) {
            if (!contains(g_covered, ws))
                applyCoverage(ws.lock());
        }

        g_covered = std::move(now);
    }

    // A window that lands on a workspace after it was covered (opened or moved there without focus) starts out
    // visible and allowed over fullscreen; give it the same treatment as the rest of the workspace.
    void reapplyIfCovered(const PHLWORKSPACE& ws) {
        if (ws && std::ranges::any_of(g_covered, [&](const PHLWORKSPACEREF& c) { return c.lock() == ws; }))
            applyCoverage(ws);
    }

    // Moves a fullscreen window to its span, or back to its own monitor once the span is gone.
    void reapplyGeometry(const PHLWINDOW& w) {
        if (!w || controller()->getFullscreenModes(w).internal != FSMODE_FULLSCREEN)
            return;

        const auto TARGET  = w->layoutTarget();
        const auto MONITOR = w->m_workspace ? w->m_workspace->m_monitor.lock() : nullptr;
        if (!TARGET || !MONITOR)
            return;

        const auto SPAN                        = activeSpan(w);
        controller()->m_windowPosSettingQueued = true;
        TARGET->setPositionGlobal(SPAN ? SPAN->box : MONITOR->logicalBox());
    }

    void advertise(xcb_connection_t* conn) {
        const xcb_atom_t   ATOM = fullscreenMonitorsAtom();
        const xcb_window_t ROOT = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
        if (ATOM == XCB_ATOM_NONE)
            return;
        xcb_change_property(conn, XCB_PROP_MODE_APPEND, ROOT, HYPRATOMS["_NET_SUPPORTED"], XCB_ATOM_ATOM, 32, 1, &ATOM);
        xcb_flush(conn);
    }

    void onFullscreenMonitorsMessage(xcb_client_message_event_t* e) {
        const auto CONN = xwmConnection();
        if (!CONN)
            return;

        const xcb_window_t XID = e->window;
        SSpanRequest       req;
        for (size_t i = 0; i < req.edges.size(); ++i) {
            req.edges[i] = e->data.data32[i];
        }
        if (const auto OLD = g_requests.find(XID); OLD != g_requests.end())
            req.warned = OLD->second.warned;

        const auto RESULT = resolve(CONN, req);
        // GTK asks to return to a single monitor by sending indices past the last monitor.
        if (RESULT == RESOLVE_BAD_INDEX || RESULT == RESOLVE_NO_XINERAMA) {
            g_requests.erase(XID);
            xcb_delete_property(CONN, XID, fullscreenMonitorsAtom());
        } else {
            xcb_change_property(CONN, XCB_PROP_MODE_REPLACE, XID, fullscreenMonitorsAtom(), XCB_ATOM_CARDINAL, 32, req.edges.size(), req.edges.data());
            if (RESULT == RESOLVE_LAYOUT_MISMATCH)
                warnLayoutMismatch(req);
            req.window      = windowForXID(XID);
            g_requests[XID] = std::move(req);
        }
        xcb_flush(CONN);

        reapplyGeometry(windowForXID(XID));
        refreshCoverage();
    }

    // Monitors were added, removed or moved. Requests are kept even when they can't be shown right now (a
    // monitor is unplugged, or the layout no longer lines up), so they resume once the monitors are back.
    void reresolveAll() {
        const auto CONN = xwmConnection();
        if (!CONN)
            return;

        for (auto& [xid, req] : g_requests) {
            if (resolve(CONN, req) == RESOLVE_LAYOUT_MISMATCH)
                warnLayoutMismatch(req);
            reapplyGeometry(req.window.lock());
        }
        refreshCoverage();
    }

    // Xwayland learns about monitor changes asynchronously from the compositor, so its Xinerama screens lag
    // behind layoutChanged. Re-resolve once it has caught up.
    int onLayoutSettled(void*) {
        reresolveAll();
        return 0;
    }

    void scheduleReresolve() {
        if (!g_layoutTimer)
            g_layoutTimer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onLayoutSettled, nullptr);
        if (g_layoutTimer)
            wl_event_source_timer_update(g_layoutTimer, 250);
    }

    std::string describeState(eHyprCtlOutputFormat, std::string) {
        std::string out = std::format("atom {} = {}\n", FULLSCREEN_MONITORS, fullscreenMonitorsAtom());
        for (auto const& [xid, req] : g_requests) {
            const auto W = req.window.lock();
            out += std::format("window 0x{:x} ({}) edges t{} b{} l{} r{} -> {} monitor(s) box {:.0f},{:.0f} {:.0f}x{:.0f} {}\n", xid, W ? W->m_title : "unmapped",
                               req.edges[0], req.edges[1], req.edges[2], req.edges[3], req.monitors.size(), req.box.x, req.box.y, req.box.w, req.box.h,
                               activeSpan(W) ? "active" : "inactive");
        }
        for (auto const& ws : g_covered) {
            if (const auto WS = ws.lock())
                out += std::format("covering workspace {} on {}\n", WS->m_name, WS->m_monitor ? WS->m_monitor->m_name : "?");
        }
        return out;
    }

    SP<SHyprCtlCommand> g_hyprctlCommand;

    // ---- hooks ----

    CFunctionHook* g_hkHandleClientMessage  = nullptr;
    CFunctionHook* g_hkCreateWMWindow       = nullptr;
    CFunctionHook* g_hkHasFullscreen        = nullptr;
    CFunctionHook* g_hkGetFullscreenWindow  = nullptr;
    CFunctionHook* g_hkGetFullscreenModes   = nullptr;
    CFunctionHook* g_hkSetTargetSizeAndPos  = nullptr;
    CFunctionHook* g_hkSyncTargetSizeAndPos = nullptr;
    CFunctionHook* g_hkRenderFullscreen     = nullptr;

    using PRENDERWINDOW = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
    PRENDERWINDOW g_renderWindow = nullptr;

    void hkHandleClientMessage(CXWM* thisptr, xcb_client_message_event_t* e) {
        if (e->format == 32 && e->type != XCB_ATOM_NONE && e->type == fullscreenMonitorsAtom()) {
            onFullscreenMonitorsMessage(e);
            return;
        }
        ((decltype(&hkHandleClientMessage))g_hkHandleClientMessage->m_original)(thisptr, e);
    }

    // Runs at the end of the CXWM constructor, after _NET_SUPPORTED is written and HYPRATOMS (ours included)
    // are interned. g_pXWayland->m_wm is not assigned yet, so use thisptr. Requests from a previous Xwayland
    // instance refer to windows that no longer exist.
    void hkCreateWMWindow(CXWM* thisptr) {
        ((decltype(&hkCreateWMWindow))g_hkCreateWMWindow->m_original)(thisptr);
        g_requests.clear();
        if (const auto CONN = xwmConnection(thisptr))
            advertise(CONN);
    }

    bool hkHasFullscreen(CFullscreenController* thisptr, PHLWORKSPACE ws, std::optional<bool> covering) {
        if (spanOwnerFor(ws))
            return true;
        return ((decltype(&hkHasFullscreen))g_hkHasFullscreen->m_original)(thisptr, ws, covering);
    }

    PHLWINDOW hkGetFullscreenWindow(CFullscreenController* thisptr, PHLWORKSPACE ws, std::optional<bool> covering) {
        if (auto w = spanOwnerFor(ws))
            return w;
        return ((decltype(&hkGetFullscreenWindow))g_hkGetFullscreenWindow->m_original)(thisptr, ws, covering);
    }

    SFullscreenMode hkGetFullscreenModes(CFullscreenController* thisptr, PHLWORKSPACE ws, std::optional<bool> covering) {
        if (spanOwnerFor(ws))
            return {.internal = FSMODE_FULLSCREEN, .client = FSMODE_FULLSCREEN};
        return ((decltype(&hkGetFullscreenModes))g_hkGetFullscreenModes->m_original)(thisptr, ws, covering);
    }

    void hkSetTargetSizeAndPosition(IFullscreenHandler* thisptr, SP<Layout::ITarget> target) {
        const auto W = target ? target->window() : nullptr;
        if (const auto SPAN = activeSpan(W)) {
            controller()->m_windowPosSettingQueued = true;
            W->layoutTarget()->setPositionGlobal(SPAN->box);
            return;
        }
        ((decltype(&hkSetTargetSizeAndPosition))g_hkSetTargetSizeAndPos->m_original)(thisptr, target);
    }

    void hkSyncTargetSizeAndPosition(IFullscreenHandler* thisptr) {
        const auto TARGET = thisptr->getFullscreen(true);
        const auto W      = TARGET ? TARGET->window() : nullptr;
        if (const auto SPAN = activeSpan(W)) {
            const auto EXPECTED = CBox{SPAN->box}.round();
            if (W.get()->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) != EXPECTED.pos() ||
                W.get()->size(Desktop::View::IGeometric::GEOMETRIC_GOAL) != EXPECTED.size()) {
                controller()->m_windowPosSettingQueued = true;
                W->layoutTarget()->setPositionGlobal(SPAN->box);
            }
            return;
        }
        ((decltype(&hkSyncTargetSizeAndPosition))g_hkSyncTargetSizeAndPos->m_original)(thisptr);
    }

    // A covered workspace reports the spanning window as its fullscreen window, which sends rendering here,
    // but this pass only draws the workspace's own windows. Draw the span on top of them.
    void hkRenderWorkspaceWindowsFullscreen(Render::IHyprRenderer* thisptr, PHLMONITOR monitor, PHLWORKSPACE ws, const Time::steady_tp& time) {
        ((decltype(&hkRenderWorkspaceWindowsFullscreen))g_hkRenderFullscreen->m_original)(thisptr, monitor, ws, time);

        if (const auto OWNER = spanOwnerFor(ws))
            g_renderWindow(thisptr, OWNER, monitor, time, false, Render::RENDER_PASS_ALL, false, false);
    }

    // Finds exactly one function whose demangled name contains every entry of `mustContain`.
    void* findFunction(const std::string& name, const std::vector<std::string>& mustContain) {
        auto matches = HyprlandAPI::findFunctionsByName(g_handle, name);
        std::erase_if(matches, [&](const SFunctionMatch& m) { return !std::ranges::all_of(mustContain, [&](const auto& s) { return m.demangled.contains(s); }); });

        if (matches.size() != 1) {
            std::string found;
            for (auto const& m : HyprlandAPI::findFunctionsByName(g_handle, name)) {
                found += "\n  " + m.demangled;
            }
            throw std::runtime_error(std::format("[hyprspan] expected one match for {}, got {}. Candidates:{}", name, matches.size(), found));
        }

        return matches[0].address;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(handle, "[hyprspan] Built for a different Hyprland version, rebuild it", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hyprspan] version mismatch");
    }

    // Resolve everything before hooking anything, so a failed lookup leaves Hyprland untouched.
    void* const HANDLE_CLIENT_MESSAGE = findFunction("handleClientMessage", {"CXWM::handleClientMessage("});
    void* const CREATE_WM_WINDOW      = findFunction("createWMWindow", {"CXWM::createWMWindow("});
    void* const HAS_FULLSCREEN        = findFunction("hasFullscreen", {"CFullscreenController::hasFullscreen(", "CWorkspace"});
    void* const GET_FULLSCREEN_WINDOW = findFunction("getFullscreenWindow", {"CFullscreenController::getFullscreenWindow(", "CWorkspace"});
    void* const GET_FULLSCREEN_MODES  = findFunction("getFullscreenModes", {"CFullscreenController::getFullscreenModes(", "CWorkspace"});
    void* const SET_SIZE_AND_POS      = findFunction("setTargetSizeAndPosition", {"IFullscreenHandler::setTargetSizeAndPosition("});
    void* const SYNC_SIZE_AND_POS     = findFunction("syncTargetSizeAndPosition", {"IFullscreenHandler::syncTargetSizeAndPosition("});
    void* const RENDER_FULLSCREEN     = findFunction("renderWorkspaceWindowsFullscreen", {"IHyprRenderer::renderWorkspaceWindowsFullscreen("});
    g_renderWindow                    = rc<PRENDERWINDOW>(findFunction("renderWindow", {"IHyprRenderer::renderWindow("}));

    auto hook = [&](void* source, void* destination) {
        auto* h = HyprlandAPI::createFunctionHook(handle, source, destination);
        if (!h || !h->hook())
            throw std::runtime_error("[hyprspan] failed to install a hook");
        return h;
    };

    g_hkHandleClientMessage  = hook(HANDLE_CLIENT_MESSAGE, rc<void*>(&hkHandleClientMessage));
    g_hkCreateWMWindow       = hook(CREATE_WM_WINDOW, rc<void*>(&hkCreateWMWindow));
    g_hkHasFullscreen        = hook(HAS_FULLSCREEN, rc<void*>(&hkHasFullscreen));
    g_hkGetFullscreenWindow  = hook(GET_FULLSCREEN_WINDOW, rc<void*>(&hkGetFullscreenWindow));
    g_hkGetFullscreenModes   = hook(GET_FULLSCREEN_MODES, rc<void*>(&hkGetFullscreenModes));
    g_hkSetTargetSizeAndPos  = hook(SET_SIZE_AND_POS, rc<void*>(&hkSetTargetSizeAndPosition));
    g_hkSyncTargetSizeAndPos = hook(SYNC_SIZE_AND_POS, rc<void*>(&hkSyncTargetSizeAndPosition));
    g_hkRenderFullscreen     = hook(RENDER_FULLSCREEN, rc<void*>(&hkRenderWorkspaceWindowsFullscreen));

    g_hyprctlCommand = HyprlandAPI::registerHyprCtlCommand(handle, SHyprCtlCommand{.name = "hyprspan", .exact = true, .fn = describeState});

    auto& events = Event::bus()->m_events;
    g_listeners.emplace_back(events.window.fullscreen.listen([](PHLWINDOW) { refreshCoverage(); }));
    g_listeners.emplace_back(events.workspace.active.listen([](PHLWORKSPACE) { refreshCoverage(); }));
    g_listeners.emplace_back(events.window.openLate.listen([](PHLWINDOW w) { reapplyIfCovered(w ? w->m_workspace : nullptr); }));
    g_listeners.emplace_back(events.window.moveToWorkspace.listen([](PHLWINDOW, PHLWORKSPACE ws) { reapplyIfCovered(ws); }));
    g_listeners.emplace_back(events.window.close.listen([](PHLWINDOW w) {
        if (w && w->m_isX11 && w->m_xwaylandSurface && g_requests.erase(w->m_xwaylandSurface->m_xID))
            refreshCoverage();
    }));
    g_listeners.emplace_back(events.monitor.layoutChanged.listen([] { scheduleReresolve(); }));

    // Xwayland may already be running, in which case the constructor hook above has missed it.
    HYPRATOMS.try_emplace(FULLSCREEN_MONITORS, XCB_ATOM_NONE);
    if (const auto CONN = xwmConnection()) {
        auto* reply = xcb_intern_atom_reply(CONN, xcb_intern_atom(CONN, 0, std::strlen(FULLSCREEN_MONITORS), FULLSCREEN_MONITORS), nullptr);
        if (reply) {
            HYPRATOMS[FULLSCREEN_MONITORS] = reply->atom;
            free(reply);
        }
        advertise(CONN);
    }

    return {"hyprspan", "Spans fullscreen XWayland windows across monitors via _NET_WM_FULLSCREEN_MONITORS", "Jagapathi", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();
    HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_hyprctlCommand);
    if (g_layoutTimer)
        wl_event_source_remove(g_layoutTimer);

    // Put spanning windows and covered workspaces back before the hooks go away.
    std::vector<PHLWINDOWREF> spanning;
    for (auto const& [xid, req] : g_requests) {
        spanning.emplace_back(req.window);
    }
    g_requests.clear();
    for (auto const& w : spanning) {
        reapplyGeometry(w.lock());
    }
    refreshCoverage();

    // _NET_SUPPORTED keeps the atom until Xwayland restarts; with no requests recorded, the message is a no-op.
}
